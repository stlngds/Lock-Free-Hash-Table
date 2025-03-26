// Contributors: Grace Biggs
// Adapted from Michael's "High performance dynamic lock-free hash tables and list-based sets": https://doi.org/10.1145/564870.564881 / https://citeseerx.ist.psu.edu/document?repid=rep1&type=pdf&doi=fb47306b9007a2a901f7214e2a9b2005f20ed4b7
#pragma once
#include <cinttypes>
#include <atomic>
#include <vector>
#include <cstdint>

// Per the paper, <Mark, Next, Tag> must occupy a contiguous aligned memory block that can be manipulated atomically.
struct MarkedPtr {
	std::uintptr_t ptr : 48;
	bool marked : 1;
	std::uint16_t tag : 16;

	// Helper function to cast pointer to uintptr_t
	static MarkedPtr encode(void* p, bool mark, uint16_t t) {
		return { reinterpret_cast<uintptr_t>(p), mark, t };
	}
};

// Node
template <typename K, typename V>
struct Node {
	K key;
	V value;
	std::atomic<MarkedPtr> next;

	Node(K k, V v) : key(k), value(v), next(MarkedPtr::encode(nullptr, false, 0)) {}
};

//
template <typename K, typename V>
class LockFreeHashTable
{
private:
	std::vector<std::atomic<MarkedPtr>> buckets;
	size_t num_buckets;

	size_t hash(K key) const {
		return std::hash<K>{}(key) % num_buckets;
	}

public:
	LockFreeHashTable(size_t num_buckets = 64) : num_buckets(num_buckets) {
		buckets.resize(num_buckets);
		for (auto& head : buckets) {
			head.store(MarkedPtr::encode(nullptr, false, 0));
		}
	}

	bool insert(K key, V value) { // TODO: Use MemoryPool or something to reuse new_node.
		size_t idx = hash(key);
		Node<K, V>* new_node = new Node<K, V>(key, value);

		while (true) {
			auto [prev, curr] = search(idx, key);

			if (curr != nullptr && curr->key == key) {
				delete new_node;
				return false;
			}

			new_node->next.store(MarkedPtr::encode(curr, false, 0));

			MarkedPtr expected = MarkedPtr::encode(curr, false, prev->next.load().tag);
			MarkedPtr desired = MarkedPtr::encode(new_node, false, expected.tag + 1);

			if (prev->next.compare_exchange_strong(expected, desired)) {
				return true;
			}
		}
	}

	bool remove(K key) {
		size_t idx = hash(key);

		while (true) {
			auto [prev, curr] = search(idx, key);

			if (curr == nullptr || curr->key != key) {
				return false; // No node found
			}

			MarkedPtr curr_next = curr->next.load();
			MarkedPtr desired_marked = MarkedPtr::encode(curr_next.ptr, true, curr_next.tag + 1);

			if (!curr->next.compare_exchange_strong(curr_next, desired_marked)) {
				continue;
			}
			
			MarkedPtr prev_expected = MarkedPtr::encode(curr, false, prev->next.load().tag);
			MarkedPtr prev_desired = MarkedPtr::encode(curr_next.ptr, false, prev_expected.tag + 1);

			if (prev->next.compare_exchange_strong(prev_expected, prev_desired)) {
				delete curr;
			}
			else {
				find_bucket(idx, key);
			}
			return true;
		}
	}

	// As in the paper, Find() is basically a wrapper for Search()...
	bool find(K key) {
		size_t idx = hash(key);
		auto [prev, curr] = find_bucket(idx, key);
		return (curr != nullptr && curr->key == key);
	}

private:
	std::pair<Node<K, V>*, Node<K, V>*> search(size_t idx, K key) {
		while (true) {
			Node<K, V>* prev = &buckets[idx];
			Node<K, V>* curr = get_node(buckets[idx].load());

			while (curr != nullptr) {
				MarkedPtr curr_next = curr->next.load();

				if (curr_next.marked) {
					MarkedPtr expected = MarkedPtr::encode(curr, false, prev->next.load().tag);
					MarkedPtr desired = MarkedPtr::encode(curr_next.ptr, false, expected.tag + 1);

					if (prev->next.compare_exchange_strong(expected, desired)) {
						curr = get_node(curr_next.ptr);
					}
					else {
						break;
					}
				}

				if (curr->key >= key) {
					return { prev, curr };
				}

				prev = curr;
				curr = get_node(curr_next.ptr);
			}

			return { prev, nullptr };
		}
	}

	Node<K, V>* get_node(MarkedPtr mp) { // Extract pointer
		return reinterpret_cast<Node<K, V>*>(mp.ptr);
	}
};