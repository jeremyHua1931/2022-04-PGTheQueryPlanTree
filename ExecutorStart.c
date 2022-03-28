/* ----------------------------------------------------------------
 *      ExecutorStart
 *
 *      This routine must be called at the beginning of any execution of any
 *      query plan
 *      ExecutorStart必须在执行开始前调用.
 *
 * Takes a QueryDesc previously created by CreateQueryDesc (which is separate
 * only because some places use QueryDescs for utility commands).  The tupDesc
 * field of the QueryDesc is filled in to describe the tuples that will be
 * returned, and the internal fields (estate and planstate) are set up.
 * 获取先前由CreateQueryDesc创建的QueryDesc(该数据结构是独立的，只是因为有些地方使用QueryDesc来执行实用命令)。
 * 填充QueryDesc的tupDesc字段，以描述将要返回的元组，并设置内部字段(estate和planstate)。
 * 
 * eflags contains flag bits as described in executor.h.
 * eflags存储标志位(在executor.h中有说明)
 * 
 * NB: the CurrentMemoryContext when this is called will become the parent
 * of the per-query context used for this Executor invocation.
 * 注意:CurrentMemoryContext会成为每个执行查询的上下文的parent
 *
 * We provide a function hook variable that lets loadable plugins
 * get control when ExecutorStart is called.  Such a plugin would
 * normally call standard_ExecutorStart().
 * 我们提供了一个函数钩子变量，可以让可加载插件在调用ExecutorStart时获得控制权。
 * 这样的插件通常会调用standard_ExecutorStart()函数。
 *
 * ----------------------------------------------------------------
 */
void
ExecutorStart(QueryDesc *queryDesc, int eflags)
{
    if (ExecutorStart_hook)//存在钩子函数
        (*ExecutorStart_hook) (queryDesc, eflags);
    else
        standard_ExecutorStart(queryDesc, eflags);
}

void
standard_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
    EState     *estate;
    MemoryContext oldcontext;

    /* sanity checks: queryDesc must not be started already */
    Assert(queryDesc != NULL);
    Assert(queryDesc->estate == NULL);

    /*
     * If the transaction is read-only, we need to check if any writes are
     * planned to non-temporary tables.  EXPLAIN is considered read-only.
     * 如果事务是只读的，需要检查是否计划对非临时表进行写操作。
     * EXPLAIN命令被认为是只读的。
     * 
     * Don't allow writes in parallel mode.  Supporting UPDATE and DELETE
     * would require (a) storing the combocid hash in shared memory, rather
     * than synchronizing it just once at the start of parallelism, and (b) an
     * alternative to heap_update()'s reliance on xmax for mutual exclusion.
     * INSERT may have no such troubles, but we forbid it to simplify the
     * checks.
     * 不要在并行模式下写。
     * 支持更新和删除需要:
     *   (a)在共享内存中存储combocid散列，而不是在并行性开始时只同步一次;
     *   (b) heap_update()依赖xmax实现互斥的替代方法。
     * INSERT可能没有这样的麻烦，但我们禁止它简化检查。
     * 
     * We have lower-level defenses in CommandCounterIncrement and elsewhere
     * against performing unsafe operations in parallel mode, but this gives a
     * more user-friendly error message.
     * 在CommandCounterIncrement和其他地方，对于在并行模式下执行不安全的操作，
     * PG有较低级别的防御，这里提供了更用户友好的错误消息。
     */
    if ((XactReadOnly || IsInParallelMode()) &&
        !(eflags & EXEC_FLAG_EXPLAIN_ONLY))
        ExecCheckXactReadOnly(queryDesc->plannedstmt);

    /*
     * Build EState, switch into per-query memory context for startup.
     * 构建EState,切换至每个查询的上下文中,准备开启执行
     */
    estate = CreateExecutorState();
    queryDesc->estate = estate;

    oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

    /*
     * Fill in external parameters, if any, from queryDesc; and allocate
     * workspace for internal parameters
     * 填充queryDesc的外部参数(如有);并为内部参数分配工作区
     */
    estate->es_param_list_info = queryDesc->params;

    if (queryDesc->plannedstmt->paramExecTypes != NIL)
    {
        int         nParamExec;

        nParamExec = list_length(queryDesc->plannedstmt->paramExecTypes);
        estate->es_param_exec_vals = (ParamExecData *)
            palloc0(nParamExec * sizeof(ParamExecData));
    }

    estate->es_sourceText = queryDesc->sourceText;

    /*
     * Fill in the query environment, if any, from queryDesc.
     * 填充查询执行环境,从queryDesc中获得
     */
    estate->es_queryEnv = queryDesc->queryEnv;

    /*
     * If non-read-only query, set the command ID to mark output tuples with
     * 非只读查询,设置命令ID
     */
    switch (queryDesc->operation)
    {
        case CMD_SELECT:

            /*
             * SELECT FOR [KEY] UPDATE/SHARE and modifying CTEs need to mark
             * tuples
             * SELECT FOR [KEY] UPDATE/SHARE和正在更新的CTEs需要标记元组
             */
            if (queryDesc->plannedstmt->rowMarks != NIL ||
                queryDesc->plannedstmt->hasModifyingCTE)
                estate->es_output_cid = GetCurrentCommandId(true);

            /*
             * A SELECT without modifying CTEs can't possibly queue triggers,
             * so force skip-triggers mode. This is just a marginal efficiency
             * hack, since AfterTriggerBeginQuery/AfterTriggerEndQuery aren't
             * all that expensive, but we might as well do it.
             * 不带更新CTEs的SELECT不可能执行触发器,因此强制为EXEC_FLAG_SKIP_TRIGGERS标记.
             * 这只是一个边际效益问题，因为AfterTriggerBeginQuery/AfterTriggerEndQuery成本并不高，但不妨这样做。
             */
            if (!queryDesc->plannedstmt->hasModifyingCTE)
                eflags |= EXEC_FLAG_SKIP_TRIGGERS;
            break;

        case CMD_INSERT:
        case CMD_DELETE:
        case CMD_UPDATE:
            estate->es_output_cid = GetCurrentCommandId(true);
            break;

        default:
            elog(ERROR, "unrecognized operation code: %d",
                 (int) queryDesc->operation);
            break;
    }

    /*
     * Copy other important information into the EState
     * 拷贝其他重要的信息到EState数据结构中
     */
    estate->es_snapshot = RegisterSnapshot(queryDesc->snapshot);
    estate->es_crosscheck_snapshot = RegisterSnapshot(queryDesc->crosscheck_snapshot);
    estate->es_top_eflags = eflags;
    estate->es_instrument = queryDesc->instrument_options;
    estate->es_jit_flags = queryDesc->plannedstmt->jitFlags;

    /*
     * Set up an AFTER-trigger statement context, unless told not to, or
     * unless it's EXPLAIN-only mode (when ExecutorFinish won't be called).
     * 设置AFTER-trigger语句上下文,除非明确不需要执行此操作或者是EXPLAIN-only模式
     */
    if (!(eflags & (EXEC_FLAG_SKIP_TRIGGERS | EXEC_FLAG_EXPLAIN_ONLY)))
        AfterTriggerBeginQuery();

    /*
     * Initialize the plan state tree
     * 初始化计划状态树
     */
    InitPlan(queryDesc, eflags);

    MemoryContextSwitchTo(oldcontext);
}