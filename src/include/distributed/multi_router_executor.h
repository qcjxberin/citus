/*
 * multi_router_executor.h
 *
 * Function declarations used in executing distributed execution
 * plan.
 *
 */

#ifndef MULTI_ROUTER_EXECUTOR_H_
#define MULTI_ROUTER_EXECUTOR_H_

#include "c.h"

#include "access/sdir.h"
#include "distributed/multi_physical_planner.h"
#include "executor/execdesc.h"
#include "nodes/pg_list.h"


/*
 * XactShardConnSet keeps track of the mapping from shard to the set of nodes
 * involved in multi-statement transaction-wrapped modifications of that shard.
 * This information is used to mark placements inactive at transaction close.
 */
typedef struct XactShardConnSet
{
	uint64 shardId;            /* identifier of the shard that was modified */
	List *connectionEntryList; /* NodeConnectionEntry pointers to participating nodes */
} XactShardConnSet;


/* Config variables managed via guc.c */
extern bool AllModificationsCommutative;


extern void RouterExecutorStart(QueryDesc *queryDesc, int eflags, List *taskList);
extern void RouterExecutorRun(QueryDesc *queryDesc, ScanDirection direction, long count);
extern void RouterExecutorFinish(QueryDesc *queryDesc);
extern void RouterExecutorEnd(QueryDesc *queryDesc);

extern int64 ExecuteModifyTasksWithoutResults(List *taskList);

#endif /* MULTI_ROUTER_EXECUTOR_H_ */
