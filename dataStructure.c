/* ----------------
 *      query descriptor:
 *
 *  a QueryDesc encapsulates everything that the executor
 *  needs to execute the query.
 *  QueryDesc封装了执行器执行查询所需的所有内容。
 *
 *  For the convenience of SQL-language functions, we also support QueryDescs
 *  containing utility statements; these must not be passed to the executor
 *  however.
 *  为了使用SQL函数，还需要支持包含实用语句的QueryDescs;
 *  但是，这些内容不能传递给执行程序。
 * ---------------------
 */
typedef struct QueryDesc
{
    /* These fields are provided by CreateQueryDesc */
    //以下变量由CreateQueryDesc函数设置
    CmdType operation;            /* 操作类型,如CMD_SELECT等;CMD_SELECT, CMD_UPDATE, etc. */
    PlannedStmt *plannedstmt;     /* 已规划的语句,规划器的输出;planner's output (could be utility, too) */
    const char *sourceText;       /* 源SQL文本;source text of the query */
    Snapshot snapshot;            /* 查询使用的快照;snapshot to use for query */
    Snapshot crosscheck_snapshot; /* RI 更新/删除交叉检查快照;crosscheck for RI update/delete */
    DestReceiver *dest;           /* 元组输出的接收器;the destination for tuple output */
    ParamListInfo params;         /* 需传入的参数值;param values being passed in */
    QueryEnvironment *queryEnv;   /* 查询环境变量;query environment passed in */
    int instrument_options;       /* InstrumentOption选项;OR of InstrumentOption flags */

    /* These fields are set by ExecutorStart */
    //以下变量由ExecutorStart函数设置
    TupleDesc tupDesc;    /* 结果元组tuples描述;descriptor for result tuples */
    EState *estate;       /* 执行器状态;executor's query-wide state */
    PlanState *planstate; /* per-plan-node状态树;tree of per-plan-node state */

    /* This field is set by ExecutorRun */
    //以下变量由ExecutorRun设置
    bool already_executed; /* 先前已执行,则为T;true if previously executed */

    /* This is always set NULL by the core system, but plugins can change it */
    //内核设置为NULL,可由插件修改
    struct Instrumentation *totaltime; /* ExecutorRun函数所花费的时间;total time spent in ExecutorRun */
} QueryDesc;

/* ----------------
 *    EState information
 *    EState信息
 * Master working state for an Executor invocation
 * 执行器在调用时的主要工作状态
 * ----------------
 */
typedef struct EState
{
    NodeTag type; //标记

    /* Basic state for all query types: */
    //所有查询类型的基础状态
    ScanDirection es_direction;                  /* 扫描方向,如向前/后等;current scan direction */
    Snapshot es_snapshot;                        /* 时间条件(通过快照实现);time qual to use */
    Snapshot es_crosscheck_snapshot;             /* RI的交叉检查时间条件;crosscheck time qual for RI */
    List *es_range_table;                        /* RTE链表;List of RangeTblEntry */
    struct RangeTblEntry **es_range_table_array; /* 与链表等价的数组;equivalent array */
    Index es_range_table_size;                   /* 数组大小;size of the range table arrays */
    Relation *es_relations;                      /* RTE中的Relation指针,如未Open则为NULL;
                                                  * Array of per-range-table-entry Relation
                                                  * pointers, or NULL if not yet opened */
    struct ExecRowMark **es_rowmarks;            /* ExecRowMarks指针数组;
                                                  * Array of per-range-table-entry
                                                  * ExecRowMarks, or NULL if none */
    PlannedStmt *es_plannedstmt;                 /* 计划树的最顶层PlannedStmt;link to top of plan tree */
    const char *es_sourceText;                   /* QueryDesc中的源文本;Source text from QueryDesc */

    JunkFilter *es_junkFilter; /* 最顶层的JunkFilter;top-level junk filter, if any */

    /* If query can insert/delete tuples, the command ID to mark them with */
    //如查询可以插入/删除元组,这里记录了命令ID
    CommandId es_output_cid;

    /* Info about target table(s) for insert/update/delete queries: */
    // insert/update/delete 目标表信息
    ResultRelInfo *es_result_relations;     /* ResultRelInfos数组;array of ResultRelInfos */
    int es_num_result_relations;            /* 数组大小;length of array */
    ResultRelInfo *es_result_relation_info; /* 当前活动的数组;currently active array elt */

    /*
     * Info about the partition root table(s) for insert/update/delete queries
     * targeting partitioned tables.  Only leaf partitions are mentioned in
     * es_result_relations, but we need access to the roots for firing
     * triggers and for runtime tuple routing.
     * 关于针对分区表的插入/更新/删除查询的分区根表的信息。
     * 在es_result_relations中只提到了叶子分区，但是需要访问根表来触发触发器和运行时元组路由。
     */
    ResultRelInfo *es_root_result_relations; /* ResultRelInfos数组;array of ResultRelInfos */
    int es_num_root_result_relations;        /* 数组大小;length of the array */

    /*
     * The following list contains ResultRelInfos created by the tuple routing
     * code for partitions that don't already have one.
     * 下面的链表包含元组路由代码为没有分区的分区创建的ResultRelInfos。
     */
    List *es_tuple_routing_result_relations;

    /* Stuff used for firing triggers: */
    //用于触发触发器的信息
    List *es_trig_target_relations;      /* 与触发器相关的ResultRelInfos数组;trigger-only ResultRelInfos */
    TupleTableSlot *es_trig_tuple_slot;  /* 用于触发器输出的元组; for trigger output tuples */
    TupleTableSlot *es_trig_oldtup_slot; /* 用于TriggerEnabled;for TriggerEnabled */
    TupleTableSlot *es_trig_newtup_slot; /* 用于TriggerEnabled;for TriggerEnabled */

    /* Parameter info: */
    //参数信息
    ParamListInfo es_param_list_info;  /* 外部参数值; values of external params */
    ParamExecData *es_param_exec_vals; /* 内部参数值; values of internal params */

    QueryEnvironment *es_queryEnv; /* 查询环境; query environment */

    /* Other working state: */
    //其他工作状态
    MemoryContext es_query_cxt; /* EState所在的内存上下文;per-query context in which EState lives */

    List *es_tupleTable; /* TupleTableSlots链表;List of TupleTableSlots */

    uint64 es_processed; /* 处理的元组数;# of tuples processed */
    Oid es_lastoid;      /* 最后处理的oid(用于INSERT命令);last oid processed (by INSERT) */

    int es_top_eflags; /* 传递给ExecutorStart函数的eflags参数;eflags passed to ExecutorStart */
    int es_instrument; /* InstrumentOption标记;OR of InstrumentOption flags */
    bool es_finished;  /* ExecutorFinish函数已完成则为T;true when ExecutorFinish is done */

    List *es_exprcontexts; /* EState中的ExprContexts链表;List of ExprContexts within EState */

    List *es_subplanstates; /* SubPlans的PlanState链表;List of PlanState for SubPlans */

    List *es_auxmodifytables; /* 第二个ModifyTableStates链表;List of secondary ModifyTableStates */

    /*
     * this ExprContext is for per-output-tuple operations, such as constraint
     * checks and index-value computations.  It will be reset for each output
     * tuple.  Note that it will be created only if needed.
     * 这个ExprContext用于元组输出操作，例如约束检查和索引值计算。它在每个输出元组时重置。
     * 注意，只有在需要时才会创建它。
     */
    ExprContext *es_per_tuple_exprcontext;

    /*
     * These fields are for re-evaluating plan quals when an updated tuple is
     * substituted in READ COMMITTED mode.  es_epqTuple[] contains tuples that
     * scan plan nodes should return instead of whatever they'd normally
     * return, or NULL if nothing to return; es_epqTupleSet[] is true if a
     * particular array entry is valid; and es_epqScanDone[] is state to
     * remember if the tuple has been returned already.  Arrays are of size
     * es_range_table_size and are indexed by scan node scanrelid - 1.
     * 这些字段用于在读取提交模式中替换更新后的元组时重新评估计划的条件quals。
     * es_epqTuple[]数组包含扫描计划节点应该返回的元组，而不是它们通常返回的任何值，如果没有返回值，则为NULL;
     * 如果特定数组条目有效，则es_epqTupleSet[]为真;es_epqScanDone[]是状态，用来记住元组是否已经返回。
     * 数组大小为es_range_table_size，通过扫描节点scanrelid - 1进行索引。
     */
    HeapTuple *es_epqTuple; /* EPQ替换元组的数组;array of EPQ substitute tuples */
    bool *es_epqTupleSet;   /* 如EPQ元组已提供,则为T;true if EPQ tuple is provided */
    bool *es_epqScanDone;   /* 如EPQ元组已获取,则为T;true if EPQ tuple has been fetched */

    bool es_use_parallel_mode; /* 能否使用并行worker?can we use parallel workers? */

    /* The per-query shared memory area to use for parallel execution. */
    //用于并行执行的每个查询共享内存区域。
    struct dsa_area *es_query_dsa;

    /*
     * JIT information. es_jit_flags indicates whether JIT should be performed
     * and with which options.  es_jit is created on-demand when JITing is
     * performed.
     * JIT的信息。es_jit_flags指示是否应该执行JIT以及使用哪些选项。在执行JITing时，按需创建es_jit。
     *
     * es_jit_combined_instr is the the combined, on demand allocated,
     * instrumentation from all workers. The leader's instrumentation is kept
     * separate, and is combined on demand by ExplainPrintJITSummary().
     * es_jit_combined_instr是所有workers根据需要分配的组合工具。
     * leader的插装是独立的，根据需要由ExplainPrintJITSummary()组合。
     */
    int es_jit_flags;
    struct JitContext *es_jit;
    struct JitInstrumentation *es_jit_worker_instr;
} EState;
