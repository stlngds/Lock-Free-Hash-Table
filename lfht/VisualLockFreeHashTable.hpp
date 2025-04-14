#pragma once
#include <mutex>
#include <thread>
#include <atomic>
#include <vector>

#include "LockFreeHashTable.hpp"

template<typename K, typename V>
class VisualLockFreeHashTable 
{
public:    
    VisualLockFreeHashTable(size_t initBuckets = 16) :
        m_numBuckets(initBuckets)
    {
        m_shadowBuckets.resize(m_numBuckets);
        m_totalRemoved.store(0);
    }

    // @brief Insert operation.
	// @param key The key to insert.
	// @param value The value to insert.
	// @return True if the insert was successful, false if the key already exists.
    bool Insert(const K& key, const V& value)
    {
        bool result = m_table.insert(key, value);
        if (result) {
            size_t bucketIndex = key % m_numBuckets;
            std::lock_guard<std::mutex> lock(m_shadowMutex);
            m_shadowBuckets[bucketIndex].push_back(std::make_tuple(key, value, false));
        }
        return result;
    }

    // @brief Remove operation.
	// @param key The key to remove.
	// @return True if the remove was successful, false if the key was not found.
    bool Remove(const K& key)
    {
        bool result = m_table.remove(key);
        if (result) {
            size_t bucketIndex = key % m_numBuckets;
            std::lock_guard<std::mutex> lock(m_shadowMutex);
            // Mark the first matching active node as removed.
            for (auto& node : m_shadowBuckets[bucketIndex]) {
                if (std::get<0>(node) == key && !std::get<2>(node)) {
                    std::get<2>(node) = true;
                    break;
                }
            }
        }
        return result;
    }

	// @brief Collect removed nodes.
	// @return The number of nodes collected.
    int CollectRemovedNodes()
    {
        int collected = 0;
        std::lock_guard<std::mutex> lock(m_shadowMutex);
        for (auto& bucket : m_shadowBuckets) {
            for (auto it = bucket.begin(); it != bucket.end(); ) {
                if (std::get<2>(*it)) { // If marked as removed
                    ++collected;
                    it = bucket.erase(it);
                }
                else {
                    ++it;
                }
            }
        }
        m_totalRemoved.fetch_add(collected);
        return collected;
    }

	// @brief Adjust the number of buckets.
	// @param newCount The new number of buckets.
    void AdjustBucketCount(size_t newCount)
    {
        std::lock_guard<std::mutex> lock(m_shadowMutex);
        std::vector<std::vector<std::tuple<K, V, bool>>> newShadow(newCount);
        for (const auto& bucket : m_shadowBuckets) {
            for (const auto& node : bucket) {
                int key = std::get<0>(node);
                size_t newBucketIndex = key % newCount;
                newShadow[newBucketIndex].push_back(node);
            }
        }
        m_shadowBuckets = std::move(newShadow);
        m_numBuckets = newCount;
    }

	// @brief Get a snapshot of the current state of the shadow buckets.
	// @return A vector of vectors, where each inner vector contains tuples of (key, value, marked).
    std::vector<std::vector<std::tuple<K, V, bool>>> GetSnapshot()
    {
        std::lock_guard<std::mutex> lock(m_shadowMutex);
        return m_shadowBuckets;
    }

	// @brief Clear the shadow copy.
	// This function clears the shadow copy of the hash table.
    void ClearShadow()
    {
        std::lock_guard<std::mutex> lock(m_shadowMutex);
        for (auto& bucket : m_shadowBuckets) {
            bucket.clear();
        }
        m_totalRemoved.store(0);
    }

	// @brief Compute the load factor of active nodes in the hash table.
	// @return The load factor as a float.
    float ComputeLoadFactor()
    {
        std::lock_guard<std::mutex> lock(m_shadowMutex);
        int activeNodes = 0;
        for (const auto& bucket : m_shadowBuckets) {
            for (const auto& node : bucket) {
                if (!std::get<2>(node))
                    activeNodes++;
            }
        }
        return (m_numBuckets > 0) ? static_cast<float>(activeNodes) / m_numBuckets : 0.0f;
    }

	// @brief Get the number buckets.
	// @return The number of buckets.
	size_t GetBucketCount() const
	{
		return m_numBuckets;
	}
private:
    LockFreeHashTable<K, V> m_table;
    std::vector<std::vector<std::tuple<K, V, bool>>> m_shadowBuckets;
    std::mutex m_shadowMutex;
    std::atomic<int> m_totalRemoved;
    size_t m_numBuckets;
};

