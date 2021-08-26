//===- PersistentPointsToCache.h -- Persistent points-to sets ----------------//

/*
 * PersistentPointsToCache.h
 *
 *  Persistent, hash-consed points-to sets
 *
 *  Created on: Sep 28, 2020
 *      Author: Mohamad Barbar
 */

#ifndef PERSISTENT_POINTS_TO_H_
#define PERSISTENT_POINTS_TO_H_

#include <iomanip>
#include <iostream>
#include <vector>

#include "Util/SVFBasicTypes.h"

namespace SVF
{

template <typename Data> class PersistentPointsToCache;

/// ID which maps to a uniquely stored points-to set.
/// Performs reference counting.
template <typename Data>
class PointsToID final
{
public:
    typedef PersistentPointsToCache<Data> Cache;

    PointsToID(void) : index(0), cache(nullptr) { }

    PointsToID(const typename Cache::PointsToIndex index, Cache *cache) : index(index), cache(cache)
    {
        assert(cache != nullptr && index < cache->pointsToSets.size());

        if (index == 0) return;

        typename Cache::References &refs = cache->pointsToSets[index].references;
        ++refs;
    }

    /// Copy constructor. Delegation will handle reference counting.
    PointsToID(const PointsToID &ptId) : PointsToID(ptId.index, ptId.cache) { }

    /// Move constructor.
    /// Reference count will go up (delegation), then when moved object is destoyed,
    /// it will go down again.
    PointsToID(PointsToID &&ptId) : PointsToID(ptId.index, ptId.cache) { }

    PointsToID &operator=(const PointsToID &rhs)
    {
        if (index != Cache::EmptyPointsToIndex)
        {
            typename Cache::References &oldRefs = cache->pointsToSets[index].references;
            --oldRefs;
            if (oldRefs == 0) releaseIndex(index);
        }

        index = rhs.index;
        cache = rhs.cache;

        if (index != Cache::EmptyPointsToIndex) ++(cache->pointsToSets[index].references);

        return *this;
    }

    PointsToID &operator=(PointsToID &&rhs)
    {
        if (index != Cache::EmptyPointsToIndex)
        {
            typename Cache::References &oldRefs = cache->pointsToSets[index].references;
            --oldRefs;
            if (oldRefs == 0) releaseIndex(index);
        }

        index = rhs.index;
        cache = rhs.cache;

        if (index != Cache::EmptyPointsToIndex) ++(cache->pointsToSets[index].references);

        return *this;
    }

    ~PointsToID(void)
    {
        if (index == Cache::EmptyPointsToIndex) return;

        typename Cache::References &refs = cache->pointsToSets[index].references;
        assert(refs != 0);
        --refs;

        if (refs == 0) releaseIndex(index);
    }

    typename Cache::PointsToIndex getIndex(void) const { return index; }

private:
    void releaseIndex(const typename Cache::PointsToIndex index)
    {
        // TODO: this lookup can be made cheaper.
        cache->pointsToSetToIndex.erase(cache->pointsToSets[index].data);

        cache->pointsToSets[index].data.clear();
        ++(cache->pointsToSets[index].generation);

        // Let someone else take this spot.
        if (index < cache->firstFreeIndex) cache->firstFreeIndex = index;
    }

private:
    /// Cache this ID belongs to.
    Cache *cache;
    /// Index into pointsToSets.
    typename Cache::PointsToIndex index;
};

template <typename Data>
class PersistentPointsToCache
{
    friend PointsToID<Data>;

public:
    /// Index into pointsToSets.
    typedef uint32_t PointsToIndex;

private:
    typedef PointsToID<Data> PtID;

    /// Current generation a points-to set is.
    typedef uint32_t Generation;
    /// Number of references to a points-to set.
    typedef uint32_t References;

    /// Unique points-to set with some auxiliary information.
    /// Index in pointsToSets is their ID number (no need to store).
    struct SharedData
    {
        /// Concrete points-to set.
        Data data;
        /// Number of "pointers" to this points-to set.
        /// Having 0 references means this points-to set is dead/free.
        References references;
        /// Each time data is replaced, the generation is incremented.
        Generation generation;
    };

    /// Result of a cached operation.
    struct OpResult
    {
        /// Result points-to set.
        PointsToIndex result;
        /// Generation of id.
        /// pointsToSets[id].generation != generation -> entry in an OpCache is invalid.
        Generation resultGeneration;
        /// Like resultGeneration but for the LHS operand.
        Generation lhsGeneration;
        /// Like resultGeneration but for the RHS operand.
        Generation rhsGeneration;
    };

    /// Cache of binary points-to set operations.
    typedef Map<std::pair<PointsToIndex, PointsToIndex>, OpResult> OpCache;
    /// Binary operation on actual points-to sets.
    typedef std::function<Data(const Data &, const Data &)> DataOp;
    /// Operands of a binary operation as IDs.
    typedef std::pair<PointsToIndex, PointsToIndex> OperandPair;
    typedef Map<Data, PointsToIndex> PointsToSetToIndex;

public:
    PersistentPointsToCache(const Data &emptyData) : firstFreeIndex(1)
    {
        pointsToSets.push_back({ emptyData, 0, 0 });
        pointsToSetToIndex[emptyData] = 0;
        initStats();
    }

    /// Resets the cache removing everything except the emptyData it was initialised with.
    void reset(void)
    {
        // Grab the empty Data.
        const Data emptyData = pointsToSets[0].data;
        pointsToSets.clear();
        pointsToSetToIndex.clear();

        // Put the empty Data back in.
        pointsToSets.push_back({ emptyData, 0, 0 });
        pointsToSetToIndex[emptyData] = 0;

        unionCache.clear();
        complementCache.clear();
        intersectionCache.clear();

        firstFreeIndex = 1;
        // Cache is empty, clear stats.
        initStats();
    }

    /// If pts is not in the PersistentPointsToCache, inserts it, assigns an ID, and returns
    /// that ID. If it is, then the ID is returned.
    PtID emplacePts(const Data &pts)
    {
        // Is it already in the cache?
        typename PointsToSetToIndex::const_iterator foundId = pointsToSetToIndex.find(pts);
        if (foundId != pointsToSetToIndex.end()) return PtID(foundId->second, this);

        // Otherwise, insert it.
        PointsToIndex index = consumeNextFreeIndex();;

        // New data, refs must be 0 otherwise this index isn't free.
        pointsToSets[index].data = pts;
        assert(pointsToSets[index].references == 0);

        pointsToSetToIndex[pts] = index;

        return PtID(index, this);
    }

    /// Returns the points-to set which id represents. id must be stored in the cache.
    const Data &getActualPts(const PointsToIndex index) const
    {
        // Check if the points-to set for ID has already been stored.
        assert(pointsToSets.size() > index && "PPTC::getActualPts: too large an ID!");
        return pointsToSets[index].data;
    }

    /// Unions lhs and rhs and returns their union's ID.
    PtID unionPts(const PointsToIndex lhs, const PointsToIndex rhs)
    {
        static const DataOp unionOp = [](const Data &lhs, const Data &rhs) { return lhs | rhs; };

        ++totalUnions;

        // Order operands so we don't perform x U y and y U x separately.
        OperandPair operands = std::minmax(lhs, rhs);

        // Property cases.
        // EMPTY_SET U x
        if (operands.first == 0)
        {
            ++propertyUnions;
            return PtID(operands.second, this);
        }

        // x U x
        if (operands.first == operands.second)
        {
            ++propertyUnions;
            return PtID(operands.first, this);
        }

        bool opPerformed = false;
        PointsToIndex result = opPts(lhs, rhs, unionOp, unionCache, opPerformed);

        if (opPerformed)
        {
            ++uniqueUnions;

            // We can use lhs/rhs here rather than our ordered operands,
            // because the operation is commutative.

            // if x U y = z, then x U z = z,
            if (lhs != result)
            {
                unionCache[std::minmax(lhs, result)] =
                    { .result = result, .resultGeneration = pointsToSets[result].generation,
                      .lhsGeneration = pointsToSets[std::min(lhs, result)].generation,
                      .rhsGeneration = pointsToSets[std::max(lhs, result)].generation };
                ++propertyUnions;
                ++totalUnions;
            }

            // and y U z = z.
            if (rhs != result)
            {
                unionCache[std::minmax(rhs, result)] =
                    { .result = result, .resultGeneration = pointsToSets[result].generation,
                      .lhsGeneration = pointsToSets[std::min(rhs, result)].generation,
                      .rhsGeneration = pointsToSets[std::max(rhs, result)].generation };;
                ++propertyUnions;
                ++totalUnions;
            }
        } else ++lookupUnions;

        return PtID(result, this);
    }

    /// Intersects lhs and rhs (lhs AND rhs) and returns the intersection's ID.
    PtID intersectPts(const PointsToIndex lhs, const PointsToIndex rhs)
    {
        static const DataOp intersectionOp = [](const Data &lhs, const Data &rhs) { return lhs & rhs; };

        ++totalIntersections;
        // Order operands so we don't perform x & y and y & x separately.
        std::pair<PointsToIndex, PointsToIndex> operands = std::minmax(lhs, rhs);

        // Property cases.
        // EMPTY_SET & x
        if (operands.first == EmptyPointsToIndex)
        {
            ++propertyIntersections;
            return PtID(EmptyPointsToIndex, this);
        }

        // x & x
        if (operands.first == operands.second)
        {
            ++propertyIntersections;
            return PtID(operands.first, this);
        }

        bool opPerformed = false;
        const PointsToIndex result = opPts(lhs, rhs, intersectionOp, intersectionCache, opPerformed);
        if (opPerformed)
        {
            ++uniqueIntersections;

            // When the result is empty, we won't be adding anything of substance.
            if (result != EmptyPointsToIndex)
            {
                // We performed lhs AND rhs = result, so...
                // result AND rhs = result,
                if (result != rhs)
                {
                    intersectionCache[std::minmax(rhs, result)] =
                        { .result = result, .resultGeneration = pointsToSets[result].generation,
                          .lhsGeneration = pointsToSets[std::min(rhs, result)].generation,
                          .rhsGeneration = pointsToSets[std::max(rhs, result)].generation };
                    ++propertyIntersections;
                    ++totalIntersections;
                }

                // and result AND lhs = result,
                if (result != lhs)
                {
                    intersectionCache[std::minmax(lhs, result)] =
                        { .result = result, .resultGeneration = pointsToSets[result].generation,
                          .lhsGeneration = pointsToSets[std::min(lhs, result)].generation,
                          .rhsGeneration = pointsToSets[std::max(lhs, result)].generation };
                    ++propertyIntersections;
                    ++totalIntersections;
                }

                // Also (thanks reviewer #2)
                // result U lhs = result,
                if (result != EmptyPointsToIndex && result != lhs)
                {
                    unionCache[std::minmax(lhs, result)] =
                        { .result = lhs, .resultGeneration = pointsToSets[lhs].generation,
                          .lhsGeneration = pointsToSets[std::min(lhs, result)].generation,
                          .rhsGeneration = pointsToSets[std::max(lhs, result)].generation };;
                    ++propertyUnions;
                    ++totalUnions;
                }

                // And result U rhs = rhs.
                if (result != EmptyPointsToIndex && result != rhs)
                {
                    unionCache[std::minmax(rhs, result)] =
                        { .result = rhs, .resultGeneration = pointsToSets[rhs].generation,
                          .lhsGeneration = pointsToSets[std::min(rhs, result)].generation,
                          .rhsGeneration = pointsToSets[std::max(rhs, result)].generation };;
                    ++propertyUnions;
                    ++totalUnions;
                }
            }
        } else ++lookupIntersections;

        return PtID(result, this);
    }

    /// Relatively complements lhs and rhs (lhs \ rhs) and returns it's ID.
    PtID complementPts(const PointsToIndex lhs, const PointsToIndex rhs)
    {
        static const DataOp complementOp = [](const Data &lhs, const Data &rhs) { return lhs - rhs; };

        ++totalComplements;

        // Property cases.
        // x - x
        if (lhs == rhs)
        {
            ++propertyComplements;
            return PtID(EmptyPointsToIndex, this);
        }

        // x - EMPTY_SET = x
        if (rhs == EmptyPointsToIndex)
        {
            ++propertyComplements;
            return PtID(lhs, this);
        }

        // EMPTY_SET - x = EMPTY_SET
        if (lhs == EmptyPointsToIndex)
        {
            ++propertyComplements;
            return PtID(EmptyPointsToIndex, this);
        }

        bool opPerformed = false;
        const PointsToIndex result = opPts(lhs, rhs, complementOp, complementCache, opPerformed);

        if (opPerformed)
        {
            ++uniqueComplements;

            // We performed lhs - rhs = result, so...
            if (result != EmptyPointsToIndex)
            {
                // result AND rhs = EMPTY_SET,
                intersectionCache[std::minmax(result, rhs)] =
                    { .result = EmptyPointsToIndex, .resultGeneration = 0,
                      .lhsGeneration = pointsToSets[std::min(result, rhs)].generation,
                      .rhsGeneration = pointsToSets[std::max(result, rhs)].generation };
                ++propertyIntersections;
                ++totalIntersections;

                // and result AND lhs = result,
                intersectionCache[std::minmax(result, lhs)] =
                    { .result = lhs, .resultGeneration = pointsToSets[lhs].generation,
                      .lhsGeneration = pointsToSets[std::min(result, lhs)].generation,
                      .rhsGeneration = pointsToSets[std::max(result, lhs)].generation };
                ++propertyIntersections;
                ++totalIntersections;

                // and result - rhs = result.
                complementCache[std::make_pair(result, rhs)] =
                    { .result = result, .resultGeneration = pointsToSets[result].generation,
                      .lhsGeneration = pointsToSets[result].generation,
                      .rhsGeneration = pointsToSets[rhs].generation };
                ++propertyComplements;
                ++totalComplements;
            }
        } else ++lookupComplements;

        return PtID(result, this);
    }

    /// Print statistics on operations and points-to set numbers.
    void printStats(const std::string subtitle) const
    {
        static const unsigned fieldWidth = 25;
        std::cout.flags(std::ios::left);

        std::cout << "****Persistent Points-To Cache Statistics: " << subtitle << "****\n";

        size_t uniquePointsToSets = 0;
        for (const SharedData &sd : pointsToSets) if (sd.references != 0) ++uniquePointsToSets;

        std::cout << std::setw(fieldWidth) << "UniquePointsToSets"    << uniquePointsToSets    << "\n";
        std::cout << std::setw(fieldWidth) << "PointsToSetsArraySize" << pointsToSets.size()   << "\n";

        std::cout << std::setw(fieldWidth) << "TotalUnions"           << totalUnions           << "\n";
        std::cout << std::setw(fieldWidth) << "PropertyUnions"        << propertyUnions        << "\n";
        std::cout << std::setw(fieldWidth) << "UniqueUnions"          << uniqueUnions          << "\n";
        std::cout << std::setw(fieldWidth) << "LookupUnions"          << lookupUnions          << "\n";

        std::cout << std::setw(fieldWidth) << "TotalComplements"      << totalComplements      << "\n";
        std::cout << std::setw(fieldWidth) << "PropertyComplements"   << propertyComplements   << "\n";
        std::cout << std::setw(fieldWidth) << "UniqueComplements"     << uniqueComplements     << "\n";
        std::cout << std::setw(fieldWidth) << "LookupComplements"     << lookupComplements     << "\n";

        std::cout << std::setw(fieldWidth) << "TotalIntersections"    << totalIntersections    << "\n";
        std::cout << std::setw(fieldWidth) << "PropertyIntersections" << propertyIntersections << "\n";
        std::cout << std::setw(fieldWidth) << "UniqueIntersections"   << uniqueIntersections   << "\n";
        std::cout << std::setw(fieldWidth) << "LookupIntersections"   << lookupIntersections   << "\n";

        std::cout.flush();
    }

private:
    /// Performs dataOp on lhs and rhs, checking the opCache first and updating it afterwards.
    /// opPerformed is set to true if the operation was *not* cached and thus performed, false otherwise.
    /// Callers responsibility to order operands if the operation is commutative.
    inline PointsToIndex opPts(PointsToIndex lhs, PointsToIndex rhs, const DataOp &dataOp,
                               OpCache &opCache, bool &opPerformed)
    {
        OperandPair operands = std::make_pair(lhs, rhs);

        // Check if we have performed this operation
        typename OpCache::const_iterator foundResult = opCache.find(operands);
        if (foundResult != opCache.end())
        {
            // Check: is it valid?
            if (pointsToSets[lhs].generation == foundResult->second.lhsGeneration
                && pointsToSets[rhs].generation == foundResult->second.rhsGeneration
                && pointsToSets[foundResult->second.result].generation == foundResult->second.resultGeneration)
            {
                return foundResult->second.result;
            }
            // Otherwise, we'll do the operation and overwrite this entry.
        }

        // Operation not cached; do it.
        opPerformed = true;

        const Data &lhsPts = getActualPts(lhs);
        const Data &rhsPts = getActualPts(rhs);

        const Data result = dataOp(lhsPts, rhsPts);

        typename PointsToSetToIndex::const_iterator foundIndexIt = pointsToSetToIndex.find(result);
        // Intern points-to set if result doesn't already exists.
        PointsToIndex resultIndex;
        if (foundIndexIt != pointsToSetToIndex.end()) resultIndex = foundIndexIt->second;
        else
        {
            resultIndex = consumeNextFreeIndex();
            pointsToSets[resultIndex].data = result;
            assert(pointsToSets[resultIndex].references == 0);

            pointsToSetToIndex[result] = resultIndex;
        }

        // Cache the result.
        opCache[operands] = { resultIndex, pointsToSets[resultIndex].generation,
                              pointsToSets[lhs].generation, pointsToSets[rhs].generation };
        return resultIndex;
    }

    inline PointsToIndex consumeNextFreeIndex(void)
    {
        // We'll return this and update firstFreeIndex.
        const PointsToIndex currentlyFreeIndex = firstFreeIndex;

        // If pointsToSets is full, make sure the cell exists.
        if (pointsToSets.size() <= currentlyFreeIndex)
        {
            assert(pointsToSets.size() == currentlyFreeIndex);
            pointsToSets.resize(currentlyFreeIndex + 1);
        }

        // Either break early for a free cell, or we'll reach the end and firstFreeIndex will
        // be one past the end. Then, we'll reserve the next time this is called for that value.
        for (++firstFreeIndex; firstFreeIndex < pointsToSets.size(); ++firstFreeIndex)
        {
            if (pointsToSets[firstFreeIndex].references == 0) break;
        }

        return currentlyFreeIndex;
    }

    /// Initialises statistics variables to 0.
    inline void initStats(void)
    {

        totalUnions           = 0;
        uniqueUnions          = 0;
        propertyUnions        = 0;
        lookupUnions          = 0;
        totalComplements      = 0;
        uniqueComplements     = 0;
        propertyComplements   = 0;
        lookupComplements     = 0;
        totalIntersections    = 0;
        uniqueIntersections   = 0;
        propertyIntersections = 0;
        lookupIntersections   = 0;
    }

public:
    /// Represents the empty points-to set.
    static const PointsToIndex EmptyPointsToIndex;

private:
    /// Where unique points-to sets are stored. Indexed by an (unsigned) integer.
    /// Index is the ID number of what is stored at that index.
    std::vector<SharedData> pointsToSets;

    /// For interning -- an index into pointsToSets.
    PointsToSetToIndex pointsToSetToIndex;

    /// First free ID (index) in pointsToSets.
    PointsToIndex firstFreeIndex;

    /// Union operations. key.first is always less than key.second.
    OpCache unionCache;
    /// Intersection operations. key.first is always less than key.second.
    OpCache intersectionCache;
    /// (Relative) complement operations.
    OpCache complementCache;

    // Statistics:
    u64_t totalUnions;
    u64_t uniqueUnions;
    u64_t propertyUnions;
    u64_t lookupUnions;
    u64_t totalComplements;
    u64_t uniqueComplements;
    u64_t propertyComplements;
    u64_t lookupComplements;
    u64_t totalIntersections;
    u64_t uniqueIntersections;
    u64_t propertyIntersections;
    u64_t lookupIntersections;
};

template <typename Data>
const typename PersistentPointsToCache<Data>::PointsToIndex PersistentPointsToCache<Data>::EmptyPointsToIndex = 0;

} // End namespace SVF

#endif /* PERSISTENT_POINTS_TO_H_ */
