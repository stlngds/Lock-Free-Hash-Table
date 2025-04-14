#pragma once
#include "VisualLockFreeHashTable.hpp"
#include <tuple>
#include <cstdio>
#include <string>
#include <sstream>
#include <random>
#include <algorithm>

const int MAX_HISTORY_SIZE = 200;

struct BucketInfo {
    int nodeCount;
};

class TestSettings
{
public:
	TestSettings(int maxThreads): m_maxThreads(maxThreads), m_threadOpCounts(maxThreads)
	{
		m_pVisualTable = new VisualLockFreeHashTable<int, std::string>();
		m_lastThreadCounts.resize(m_maxThreads, 0);
		m_threadOpsPerSec.resize(m_maxThreads, 0);
		m_runWorkers.store(false);

		m_opInsertCount.store(0);
		m_opRemoveCount.store(0);
		for (auto& counter : m_threadOpCounts)
		{
			counter.store(0);
		}
		m_lastOpsUpdateTime = std::chrono::steady_clock::now();
	}

	~TestSettings()
	{
		delete m_pVisualTable;
	}

	// @brief Start worker threads for random insert/remove operations.
	// @param threadID The ID of the thread to start.
	void WorkerFunction(int threadID)
	{
		std::mt19937 rng(threadID + std::random_device{}());
		std::uniform_int_distribution<int> distKey(0, 100);
		std::uniform_int_distribution<int> distOp(0, 1);
		while (m_runWorkers.load())
		{
			int key = distKey(rng);
			if (distOp(rng) == 0)
			{
				if (m_pVisualTable->Insert(key, "val"))
					m_opInsertCount.fetch_add(1);
			}
			else
			{
				if (m_pVisualTable->Remove(key))
					m_opRemoveCount.fetch_add(1);
			}
			m_threadOpCounts[threadID].fetch_add(1);
			// This prevents the worker threads from running too fast and overloading the CPU.
			if (m_limitOps.load())
			{
				std::this_thread::sleep_for(std::chrono::microseconds(5));
			}
			else
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
			}
		}
	}

	// @brief Update the load factor history for visualization.
	void UpdateLoadFactorHistory()
	{
		float loadFactor = m_pVisualTable->ComputeLoadFactor();
		m_loadFactorHistory.push_back(loadFactor);
		if (m_loadFactorHistory.size() > MAX_HISTORY_SIZE)
		{
			m_loadFactorHistory.erase(m_loadFactorHistory.begin());
		}
	}

	// @brief Get a snapshot of the bucket information.
	// @return A vector of BucketInfo structures.
	std::vector<BucketInfo> GetBucketInfoSnapshot()
	{
		auto snapshot = m_pVisualTable->GetSnapshot();
		std::vector<BucketInfo> buckets;
		for (const auto& bucket : snapshot)
		{
			BucketInfo info;
			int active = 0;
			for (const auto& node : bucket)
			{
				if (!std::get<2>(node))
					active++;
			}
			info.nodeCount = active;
			buckets.push_back(info);
		}
		return buckets;
	}

	// @brief Get visual table instance.
	// @return A pointer to the VisualLockFreeHashTable instance.
	inline VisualLockFreeHashTable<int, std::string>* GetVisualTable()
	{
		return m_pVisualTable;
	}

	// @brief Get maximum number of threads.
	// @return The maximum number of threads.
	inline int GetMaxThreads() const
	{
		return m_maxThreads;
	}

	// @brief Get current worker threads.
	// @return A vector of thread objects.
	inline std::vector<std::thread>& GetWorkers()
	{
		return m_workers;
	}

	// @brief Get the load factor history.
	// @return A vector of load factor values.
	inline const std::vector<float>& GetLoadFactorHistory() const
	{
		return m_loadFactorHistory;
	}

	// @brief Get the last thread counts.
	// @param threadID The index of the thread.
	// @return A vector of last thread counts.
	inline const int GetLastThreadCounts(int threadID) const
	{
		if (threadID < 0 || threadID >= m_maxThreads)
			throw std::out_of_range("Thread ID out of range");
		return m_lastThreadCounts[threadID];
	}

	// @brief Get the thread operations per second.
	// @param threadID The index of the thread.
	// @return A vector of thread operations per second.
	inline const int GetThreadOpsPerSec(int threadID) const
	{
		if (threadID < 0 || threadID >= m_maxThreads)
			throw std::out_of_range("Thread ID out of range");
		return m_threadOpsPerSec[threadID];
	}

	// @brief Get the operation insert count.
	// @return The operation insert count.
	inline int GetOpInsertCount() const
	{
		return m_opInsertCount.load();
	}

	// @brief Get the operation remove count.
	// @return The operation remove count.
	inline int GetOpRemoveCount() const
	{
		return m_opRemoveCount.load();
	}

	// @brief Get the operation counts for each thread.
	// @return A vector of operation counts for each thread.
	inline const std::vector<std::atomic<int>>& GetThreadOpCounts() const
	{
		return m_threadOpCounts;
	}

	// @brief Set the run workers flag.
	// @param run Whether to run the workers or not.
	inline void SetRunWorkers(bool run)
	{
		m_runWorkers.store(run);
	}

	// @brief Set the limit operations flag.
	// @param limit Whether to limit operations or not.
	inline void SetLimitOps(bool limit)
	{
		m_limitOps.store(limit);
	}

	// @brief Set thread ops per second.
	// @param threadID The index of the thread.
	// @param ops The operations per second to set.
	inline void SetThreadOpsPerSec(int threadID, int ops)
	{
		if (threadID < 0 || threadID >= m_maxThreads)
			throw std::out_of_range("Thread ID out of range");
		m_threadOpsPerSec[threadID] = ops;
	}

	// @brief Set last thread counts.
	// @param threadID The index of the thread.
	// @param count The last count to set.
	inline void SetLastThreadCounts(int threadID, int count)
	{
		if (threadID < 0 || threadID >= m_maxThreads)
			throw std::out_of_range("Thread ID out of range");
		m_lastThreadCounts[threadID] = count;
	}

	// @brief Add insert operation count.
	// @param count The count to add.
	inline void AddInsertOpCount(int count)
	{
		m_opInsertCount.fetch_add(count);
	}

	// @brief Add remove operation count.
	// @param count The count to add.
	inline void AddRemoveOpCount(int count)
	{
		m_opRemoveCount.fetch_add(count);
	}

	// @brief Reset operation counts.
	inline void Reset()
	{
		m_runWorkers.store(false);
		for (auto& t : m_workers)
		{
			if (t.joinable())
				t.join();
		}
		m_workers.clear();

		m_opInsertCount.store(0);
		m_opRemoveCount.store(0);
		for (auto& counter : m_threadOpCounts)
		{
			counter.store(0);
		}
		std::fill(m_lastThreadCounts.begin(), m_lastThreadCounts.end(), 0);
		std::fill(m_threadOpsPerSec.begin(), m_threadOpsPerSec.end(), 0);
		m_loadFactorHistory.clear();
		m_pVisualTable->ClearShadow();
	}

	// @brief Get the last update time for operations.
	// @return The last update time.
	inline std::chrono::steady_clock::time_point GetLastOpsUpdateTime() const
	{
		return m_lastOpsUpdateTime;
	}

	// @brief Set the last update time for operations.
	// @param time The time to set.
	inline void SetLastOpsUpdateTime(std::chrono::steady_clock::time_point time)
	{
		m_lastOpsUpdateTime = time;
	}

private:
	VisualLockFreeHashTable<int, std::string>* m_pVisualTable;
	std::vector<std::atomic<int>> m_threadOpCounts;
	std::vector<std::thread> m_workers;
	std::vector<float> m_loadFactorHistory;
	std::vector<int> m_lastThreadCounts;
	std::vector<int> m_threadOpsPerSec;
	std::atomic<int> m_opInsertCount;
	std::atomic<int> m_opRemoveCount;
	std::atomic<bool> m_runWorkers;
	std::atomic<bool> m_limitOps;
	std::chrono::steady_clock::time_point m_lastOpsUpdateTime; 
	int m_maxThreads;
};

