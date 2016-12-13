/*-------------------------------------------------------------------------
 *
 * shardinterval_utils.c
 *
 * This file contains functions to perform useful operations on shard intervals.
 *
 * Copyright (c) 2014-2016, Citus Data, Inc.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/nbtree.h"
#include "catalog/pg_am.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_type.h"
#include "distributed/metadata_cache.h"
#include "distributed/shardinterval_utils.h"
#include "distributed/pg_dist_partition.h"
#include "distributed/worker_protocol.h"
#include "utils/catcache.h"
#include "utils/memutils.h"


static ShardInterval * SearchCachedShardInterval(Datum partitionColumnValue,
												 ShardInterval **shardIntervalCache,
												 int shardCount,
												 FmgrInfo *compareFunction);


/*
 * LowestShardIntervalById returns the shard interval with the lowest shard
 * ID from a list of shard intervals.
 */
ShardInterval *
LowestShardIntervalById(List *shardIntervalList)
{
	ShardInterval *lowestShardInterval = NULL;
	ListCell *shardIntervalCell = NULL;

	foreach(shardIntervalCell, shardIntervalList)
	{
		ShardInterval *shardInterval = (ShardInterval *) lfirst(shardIntervalCell);

		if (lowestShardInterval == NULL ||
			lowestShardInterval->shardId > shardInterval->shardId)
		{
			lowestShardInterval = shardInterval;
		}
	}

	return lowestShardInterval;
}


/*
 * CompareShardIntervals acts as a helper function to compare two shard intervals
 * by their minimum values, using the value's type comparison function.
 *
 * If a shard interval does not have min/max value, it's treated as being greater
 * than the other.
 */
int
CompareShardIntervals(const void *leftElement, const void *rightElement,
					  FmgrInfo *typeCompareFunction)
{
	ShardInterval *leftShardInterval = *((ShardInterval **) leftElement);
	ShardInterval *rightShardInterval = *((ShardInterval **) rightElement);
	Datum leftDatum = 0;
	Datum rightDatum = 0;
	Datum comparisonDatum = 0;
	int comparisonResult = 0;

	Assert(typeCompareFunction != NULL);

	/*
	 * Left element should be treated as the greater element in case it doesn't
	 * have min or max values.
	 */
	if (!leftShardInterval->minValueExists || !leftShardInterval->maxValueExists)
	{
		comparisonResult = 1;
		return comparisonResult;
	}

	/*
	 * Right element should be treated as the greater element in case it doesn't
	 * have min or max values.
	 */
	if (!rightShardInterval->minValueExists || !rightShardInterval->maxValueExists)
	{
		comparisonResult = -1;
		return comparisonResult;
	}

	/* if both shard interval have min/max values, calculate the comparison result */
	leftDatum = leftShardInterval->minValue;
	rightDatum = rightShardInterval->minValue;

	comparisonDatum = CompareCall2(typeCompareFunction, leftDatum, rightDatum);
	comparisonResult = DatumGetInt32(comparisonDatum);

	return comparisonResult;
}


/*
 * CompareShardIntervalsById is a comparison function for sort shard
 * intervals by their shard ID.
 */
int
CompareShardIntervalsById(const void *leftElement, const void *rightElement)
{
	ShardInterval *leftInterval = *((ShardInterval **) leftElement);
	ShardInterval *rightInterval = *((ShardInterval **) rightElement);
	int64 leftShardId = leftInterval->shardId;
	int64 rightShardId = rightInterval->shardId;

	/* we compare 64-bit integers, instead of casting their difference to int */
	if (leftShardId > rightShardId)
	{
		return 1;
	}
	else if (leftShardId < rightShardId)
	{
		return -1;
	}
	else
	{
		return 0;
	}
}


/*
 * FindShardIntervalIndex finds index of given shard in sorted shard interval array.
 *
 * For hash partitioned tables, it calculates hash value of a number in its
 * range (e.g. min value) and finds which shard should contain the hashed
 * value. For reference tables (i.e., all distributed), it simply returns 0.
 * For distribution methods, the function errors out.
 */
int
FindShardIntervalIndex(ShardInterval *shardInterval)
{
	Oid distributedTableId = shardInterval->relationId;
	DistTableCacheEntry *cacheEntry = DistributedTableCacheEntry(distributedTableId);
	char partitionMethod = cacheEntry->partitionMethod;
	int shardCount = 0;
	int32 shardMinValue = 0;
	uint64 hashTokenIncrement = 0;
	int shardIndex = -1;

	/* short-circuit for reference tables */
	if (partitionMethod == DISTRIBUTE_BY_ALL)
	{
		/* reference tables has only a single shard, so the index is fixed to 0 */
		Assert(cacheEntry->shardIntervalArrayLength == 1);
		shardIndex = 0;

		return shardIndex;
	}

	/*
	 * We can support it for other types of partitioned tables with simple binary scan
	 * but it is not necessary at the moment. If we need that simply check algorithm in
	 * FindShardInterval and SearchCachedShardInterval.
	 */
	if (partitionMethod != DISTRIBUTE_BY_HASH)
	{
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("finding index of given shard is not supported for "
							   "non-hash partitioned tables")));
	}

	shardCount = cacheEntry->shardIntervalArrayLength;
	shardMinValue = DatumGetInt32(shardInterval->minValue);
	hashTokenIncrement = HASH_TOKEN_COUNT / shardCount;
	shardIndex = (uint32) (shardMinValue - INT32_MIN) / hashTokenIncrement;

	Assert(shardIndex <= shardCount);

	/*
	 * If the shard count is not power of 2, the range of the last
	 * shard becomes larger than others. For that extra piece of range,
	 * we still need to use the last shard.
	 */
	if (shardIndex == shardCount)
	{
		shardIndex = shardCount - 1;
	}

	return shardIndex;
}


/*
 * FindShardInterval finds a single shard interval in the cache for the
 * given partition column value.
 */
ShardInterval *
FindShardInterval(Datum partitionColumnValue, ShardInterval **shardIntervalCache,
				  int shardCount, char partitionMethod, FmgrInfo *compareFunction,
				  FmgrInfo *hashFunction, bool useBinarySearch)
{
	ShardInterval *shardInterval = NULL;

	if (partitionMethod == DISTRIBUTE_BY_HASH)
	{
		int hashedValue = DatumGetInt32(FunctionCall1(hashFunction,
													  partitionColumnValue));
		if (useBinarySearch)
		{
			Assert(compareFunction != NULL);

			shardInterval = SearchCachedShardInterval(Int32GetDatum(hashedValue),
													  shardIntervalCache, shardCount,
													  compareFunction);
		}
		else
		{
			uint64 hashTokenIncrement = HASH_TOKEN_COUNT / shardCount;
			int shardIndex = (uint32) (hashedValue - INT32_MIN) / hashTokenIncrement;

			Assert(shardIndex <= shardCount);

			/*
			 * If the shard count is not power of 2, the range of the last
			 * shard becomes larger than others. For that extra piece of range,
			 * we still need to use the last shard.
			 */
			if (shardIndex == shardCount)
			{
				shardIndex = shardCount - 1;
			}

			shardInterval = shardIntervalCache[shardIndex];
		}
	}
	else if (partitionMethod == DISTRIBUTE_BY_ALL)
	{
		int shardIndex = 0;

		/* reference tables has a single shard, all values mapped to that shard */
		Assert(shardCount == 1);
		shardInterval = shardIntervalCache[shardIndex];
	}
	else
	{
		Assert(compareFunction != NULL);

		shardInterval = SearchCachedShardInterval(partitionColumnValue,
												  shardIntervalCache, shardCount,
												  compareFunction);
	}

	return shardInterval;
}


/*
 * SearchCachedShardInterval performs a binary search for a shard interval matching a
 * given partition column value and returns it.
 */
static ShardInterval *
SearchCachedShardInterval(Datum partitionColumnValue, ShardInterval **shardIntervalCache,
						  int shardCount, FmgrInfo *compareFunction)
{
	int lowerBoundIndex = 0;
	int upperBoundIndex = shardCount;

	while (lowerBoundIndex < upperBoundIndex)
	{
		int middleIndex = (lowerBoundIndex + upperBoundIndex) / 2;
		int maxValueComparison = 0;
		int minValueComparison = 0;

		minValueComparison = FunctionCall2Coll(compareFunction,
											   DEFAULT_COLLATION_OID,
											   partitionColumnValue,
											   shardIntervalCache[middleIndex]->minValue);

		if (DatumGetInt32(minValueComparison) < 0)
		{
			upperBoundIndex = middleIndex;
			continue;
		}

		maxValueComparison = FunctionCall2Coll(compareFunction,
											   DEFAULT_COLLATION_OID,
											   partitionColumnValue,
											   shardIntervalCache[middleIndex]->maxValue);

		if (DatumGetInt32(maxValueComparison) <= 0)
		{
			return shardIntervalCache[middleIndex];
		}

		lowerBoundIndex = middleIndex + 1;
	}

	return NULL;
}


/*
 * SingleReplicatedTable checks whether all shards of a distributed table, do not have
 * more than one replica. If even one shard has more than one replica, this function
 * returns false, otherwise it returns true.
 */
bool
SingleReplicatedTable(Oid relationId)
{
	List *shardIntervalList = LoadShardList(relationId);
	ListCell *shardIntervalCell = NULL;

	foreach(shardIntervalCell, shardIntervalList)
	{
		uint64 *shardIdPointer = (uint64 *) lfirst(shardIntervalCell);
		uint64 shardId = (*shardIdPointer);
		List *shardPlacementList = ShardPlacementList(shardId);

		if (shardPlacementList->length > 1)
		{
			return false;
		}
	}

	return true;
}
