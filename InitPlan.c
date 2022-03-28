/* ----------------------------------------------------------------
 *      InitPlan
 *
 *      Initializes the query plan: open files, allocate storage
 *      and start up the rule manager
 *      初始化查询执行计划:打开文件/分配存储空间并启动规则管理器
 * ----------------------------------------------------------------
 */
static void
InitPlan(QueryDesc *queryDesc, int eflags)
{
    CmdType     operation = queryDesc->operation;//命令类型
    PlannedStmt *plannedstmt = queryDesc->plannedstmt;//已规划的语句指针
    Plan       *plan = plannedstmt->planTree;//计划树
    List       *rangeTable = plannedstmt->rtable;//RTE链表
    EState     *estate = queryDesc->estate;//参见数据结构
    PlanState  *planstate;//参见数据结构
    TupleDesc   tupType;//参见数据结构
    ListCell   *l;
    int         i;

    /*
     * Do permissions checks
     * 权限检查
     */
    ExecCheckRTPerms(rangeTable, true);

    /*
     * initialize the node's execution state
     * 初始化节点执行状态
     */
    ExecInitRangeTable(estate, rangeTable);

    estate->es_plannedstmt = plannedstmt;

    /*
     * Initialize ResultRelInfo data structures, and open the result rels.
     * 初始化ResultRelInfo数据结构,打开结果rels
     */
    if (plannedstmt->resultRelations)//存在resultRelations
    {
        List       *resultRelations = plannedstmt->resultRelations;//结果Relation链表
        int         numResultRelations = list_length(resultRelations);//链表大小
        ResultRelInfo *resultRelInfos;//ResultRelInfo数组
        ResultRelInfo *resultRelInfo;//ResultRelInfo指针

        resultRelInfos = (ResultRelInfo *)
            palloc(numResultRelations * sizeof(ResultRelInfo));//分配空间
        resultRelInfo = resultRelInfos;//指针赋值
        foreach(l, resultRelations)//遍历链表
        {
            Index       resultRelationIndex = lfirst_int(l);
            Relation    resultRelation;

            resultRelation = ExecGetRangeTableRelation(estate,
                                                       resultRelationIndex);//获取结果Relation
            InitResultRelInfo(resultRelInfo,
                              resultRelation,
                              resultRelationIndex,
                              NULL,
                              estate->es_instrument);//初始化ResultRelInfo
            resultRelInfo++;//处理下一个ResultRelInfo
        }
        estate->es_result_relations = resultRelInfos;//赋值
        estate->es_num_result_relations = numResultRelations;

        /* es_result_relation_info is NULL except when within ModifyTable */
        //设置es_result_relation_info为NULL,除了ModifyTable
        estate->es_result_relation_info = NULL;

        /*
         * In the partitioned result relation case, also build ResultRelInfos
         * for all the partitioned table roots, because we will need them to
         * fire statement-level triggers, if any.
         * 在分区结果关系这种情况中，还为所有分区表根构建ResultRelInfos，
         * 因为我们需要它们触发语句级别的触发器(如果有的话)。
         */
        if (plannedstmt->rootResultRelations)//存在rootResultRelations(并行处理)
        {
            int         num_roots = list_length(plannedstmt->rootResultRelations);

            resultRelInfos = (ResultRelInfo *)
                palloc(num_roots * sizeof(ResultRelInfo));
            resultRelInfo = resultRelInfos;
            foreach(l, plannedstmt->rootResultRelations)
            {
                Index       resultRelIndex = lfirst_int(l);
                Relation    resultRelDesc;

                resultRelDesc = ExecGetRangeTableRelation(estate,
                                                          resultRelIndex);
                InitResultRelInfo(resultRelInfo,
                                  resultRelDesc,
                                  resultRelIndex,
                                  NULL,
                                  estate->es_instrument);
                resultRelInfo++;
            }

            estate->es_root_result_relations = resultRelInfos;
            estate->es_num_root_result_relations = num_roots;
        }
        else
        {
            estate->es_root_result_relations = NULL;
            estate->es_num_root_result_relations = 0;
        }
    }
    else//不存在resultRelations
    {
        /*
         * if no result relation, then set state appropriately
         * 无resultRelations,设置相应的信息为NULL或为0
         */
        estate->es_result_relations = NULL;
        estate->es_num_result_relations = 0;
        estate->es_result_relation_info = NULL;
        estate->es_root_result_relations = NULL;
        estate->es_num_root_result_relations = 0;
    }

    /*
     * Next, build the ExecRowMark array from the PlanRowMark(s), if any.
     * 下一步,利用PlanRowMark(s)创建ExecRowMark数组
     */
    if (plannedstmt->rowMarks)//如存在rowMarks
    {
        estate->es_rowmarks = (ExecRowMark **)
            palloc0(estate->es_range_table_size * sizeof(ExecRowMark *));
        foreach(l, plannedstmt->rowMarks)
        {
            PlanRowMark *rc = (PlanRowMark *) lfirst(l);
            Oid         relid;
            Relation    relation;
            ExecRowMark *erm;

            /* ignore "parent" rowmarks; they are irrelevant at runtime */
            if (rc->isParent)
                continue;

            /* get relation's OID (will produce InvalidOid if subquery) */
            relid = exec_rt_fetch(rc->rti, estate)->relid;

            /* open relation, if we need to access it for this mark type */
            switch (rc->markType)
            {
                case ROW_MARK_EXCLUSIVE:
                case ROW_MARK_NOKEYEXCLUSIVE:
                case ROW_MARK_SHARE:
                case ROW_MARK_KEYSHARE:
                case ROW_MARK_REFERENCE:
                    relation = ExecGetRangeTableRelation(estate, rc->rti);
                    break;
                case ROW_MARK_COPY:
                    /* no physical table access is required */
                    relation = NULL;
                    break;
                default:
                    elog(ERROR, "unrecognized markType: %d", rc->markType);
                    relation = NULL;    /* keep compiler quiet */
                    break;
            }

            /* Check that relation is a legal target for marking */
            if (relation)
                CheckValidRowMarkRel(relation, rc->markType);

            erm = (ExecRowMark *) palloc(sizeof(ExecRowMark));
            erm->relation = relation;
            erm->relid = relid;
            erm->rti = rc->rti;
            erm->prti = rc->prti;
            erm->rowmarkId = rc->rowmarkId;
            erm->markType = rc->markType;
            erm->strength = rc->strength;
            erm->waitPolicy = rc->waitPolicy;
            erm->ermActive = false;
            ItemPointerSetInvalid(&(erm->curCtid));
            erm->ermExtra = NULL;

            Assert(erm->rti > 0 && erm->rti <= estate->es_range_table_size &&
                   estate->es_rowmarks[erm->rti - 1] == NULL);

            estate->es_rowmarks[erm->rti - 1] = erm;
        }
    }

    /*
     * Initialize the executor's tuple table to empty.
     * 初始化执行器的元组表为NULL
     */
    estate->es_tupleTable = NIL;
    estate->es_trig_tuple_slot = NULL;
    estate->es_trig_oldtup_slot = NULL;
    estate->es_trig_newtup_slot = NULL;

    /* mark EvalPlanQual not active */
    //标记EvalPlanQual为非活动模式
    estate->es_epqTuple = NULL;
    estate->es_epqTupleSet = NULL;
    estate->es_epqScanDone = NULL;

    /*
     * Initialize private state information for each SubPlan.  We must do this
     * before running ExecInitNode on the main query tree, since
     * ExecInitSubPlan expects to be able to find these entries.
     * 为每个子计划初始化私有状态信息。
     * 在主查询树上运行ExecInitNode之前，必须这样做，因为ExecInitSubPlan希望能够找到这些条目。
     */
    Assert(estate->es_subplanstates == NIL);
    i = 1;                      /* subplan索引计数从1开始;subplan indices count from 1 */
    foreach(l, plannedstmt->subplans)//遍历subplans
    {
        Plan       *subplan = (Plan *) lfirst(l);
        PlanState  *subplanstate;
        int         sp_eflags;

        /*
         * A subplan will never need to do BACKWARD scan nor MARK/RESTORE. If
         * it is a parameterless subplan (not initplan), we suggest that it be
         * prepared to handle REWIND efficiently; otherwise there is no need.
         * 子计划永远不需要执行向后扫描或标记/恢复。
         * 如果它是一个无参数的子计划(不是initplan)，建议它准备好有效地处理向后扫描;否则就没有必要了。
         */
        sp_eflags = eflags
            & (EXEC_FLAG_EXPLAIN_ONLY | EXEC_FLAG_WITH_NO_DATA);//设置sp_eflags
        if (bms_is_member(i, plannedstmt->rewindPlanIDs))
            sp_eflags |= EXEC_FLAG_REWIND;

        subplanstate = ExecInitNode(subplan, estate, sp_eflags);//执行Plan节点初始化过程

        estate->es_subplanstates = lappend(estate->es_subplanstates,
                                           subplanstate);

        i++;
    }

    /*
     * Initialize the private state information for all the nodes in the query
     * tree.  This opens files, allocates storage and leaves us ready to start
     * processing tuples.
     * 为查询树中的所有节点初始化私有状态信息。
     * 这将打开文件、分配存储并让我们准备好开始处理元组。
     */
    planstate = ExecInitNode(plan, estate, eflags);//执行Plan节点初始化过程

    /*
     * Get the tuple descriptor describing the type of tuples to return.
     * 获取元组描述(返回的元组类型等)
     */
    tupType = ExecGetResultType(planstate);

    /*
     * Initialize the junk filter if needed.  SELECT queries need a filter if
     * there are any junk attrs in the top-level tlist.
     * 如需要，初始化垃圾过滤器。如果顶层的tlist中有任何垃圾标识，SELECT查询需要一个过滤器。
     */
    if (operation == CMD_SELECT)//SELECT命令
    {
        bool        junk_filter_needed = false;
        ListCell   *tlist;

        foreach(tlist, plan->targetlist)//遍历tlist
        {
            TargetEntry *tle = (TargetEntry *) lfirst(tlist);

            if (tle->resjunk)//如需要垃圾过滤器
            {
                junk_filter_needed = true;//设置为T
                break;
            }
        }

        if (junk_filter_needed)
        {
            JunkFilter *j;

            j = ExecInitJunkFilter(planstate->plan->targetlist,
                                   tupType->tdhasoid,
                                   ExecInitExtraTupleSlot(estate, NULL));//初始化
            estate->es_junkFilter = j;

            /* Want to return the cleaned tuple type */
            //期望返回已清理的元组类型
            tupType = j->jf_cleanTupType;
        }
    }
    //赋值
    queryDesc->tupDesc = tupType;
    queryDesc->planstate = planstate;
}
