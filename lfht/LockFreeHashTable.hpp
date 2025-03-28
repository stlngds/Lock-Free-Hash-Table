// Contributors: Grace Biggs
// Adapted from Michael's "High performance dynamic lock-free hash tables and list-based sets": https://doi.org/10.1145/564870.564881 / https://citeseerx.ist.psu.edu/document?repid=rep1&type=pdf&doi=fb47306b9007a2a901f7214e2a9b2005f20ed4b7
#pragma once
#include <atomic>
#include <vector>
#include <cstdint>
#include <functional>
#include <iostream>


// Combined MarkedPtr (64-bit and lock-free for atomic ops)
struct MarkedPtr {
    uint64_t data;

    // Bitshift to get the following layout for our contiguous data: [marked (1)][tag (15)][ptr (48)]
    static constexpr uint64_t kPtrMask = (1ULL << 48) - 1;
    static constexpr uint64_t kTagMask = (1ULL << 15) - 1;
    static constexpr int kTagShift = 48;
    static constexpr int kMarkShift = 63;

    MarkedPtr() : data(0) {}
    MarkedPtr(void* ptr, bool marked, uint16_t tag) {
        uint64_t p = reinterpret_cast<uint64_t>(ptr) & kPtrMask;
        uint64_t t = (static_cast<uint64_t>(tag) & kTagMask) << kTagShift;
        uint64_t m = static_cast<uint64_t>(marked) << kMarkShift;
        data = p | t | m;
    }

    void* ptr() const { return reinterpret_cast<void*>(data & kPtrMask); }
    uint16_t tag() const { return (data >> kTagShift) & kTagMask; }
    bool marked() const { return (data >> kMarkShift) & 0x1; }
};

static_assert(sizeof(MarkedPtr) == sizeof(uint64_t), "MarkedPtr must be 64 bits");
static_assert(std::atomic<MarkedPtr>::is_always_lock_free, "Atomic MarkedPtr not lock-free");


template <typename K, typename V>
struct Node {
    K key;
    V value;
    std::atomic<MarkedPtr> next;

    Node(K k, V v) : key(k), value(v), next(MarkedPtr(nullptr, false, 0)) {}
};


template <typename K, typename V>
class LockFreeHashTable {
private:
    std::vector<std::atomic<MarkedPtr>> buckets;
    size_t num_buckets;

    size_t hash(K key) const {
        return std::hash<K>{}(key) % num_buckets;
    }

    Node<K, V>* get_node(MarkedPtr mp) const {
        return static_cast<Node<K, V>*>(mp.ptr());
    }

public:
    LockFreeHashTable(size_t num_buckets = 64) : num_buckets(num_buckets), buckets(num_buckets) {
        for (auto& head : buckets) {
            head.store(MarkedPtr(nullptr, false, 0));
        }
    }

    bool insert(K key, V value) {
        size_t idx = hash(key);
        Node<K, V>* new_node = new Node<K, V>(key, value);

        while (true) {
            auto [prev_ptr, curr] = find_bucket(idx, key);

            // Key exists, do "nothing" (deallocate new_node first)
            if (curr && curr->key == key) {
                delete new_node;
                return false;
            }

            // Link new node
            new_node->next.store(MarkedPtr(curr, false, 0));

            // Prepare CAS
            MarkedPtr expected = prev_ptr->load();
            if (expected.marked() || get_node(expected) != curr) continue;

            MarkedPtr desired = MarkedPtr(new_node, false, expected.tag() + 1);

            if (prev_ptr->compare_exchange_strong(expected, desired)) {
                return true;
            }
        }
    }

    bool remove(K key) {
        size_t idx = hash(key);
        while (true) {
            auto [prev_ptr, curr] = find_bucket(idx, key);
            if (!curr || curr->key != key) return false;

            // Try to mark node
            MarkedPtr curr_next = curr->next.load();
            if (curr_next.marked()) continue;

            MarkedPtr desired_marked = MarkedPtr(curr_next.ptr(), true, curr_next.tag() + 1);
            if (!curr->next.compare_exchange_strong(curr_next, desired_marked)) continue;

            // Physically remove
            MarkedPtr prev_expected = prev_ptr->load();
            if (prev_expected.marked() || get_node(prev_expected) != curr) continue;

            MarkedPtr prev_desired = MarkedPtr(curr_next.ptr(), false, prev_expected.tag() + 1);
            if (prev_ptr->compare_exchange_strong(prev_expected, prev_desired)) {
                // TODO: Safe memory reclamation (i.e., MemoryBank)
                delete curr;
            }
            else {
                find_bucket(idx, key); // Help other threads
            }
            return true;
        }
    }

    // contains() is more-or-less a public wrapper for find_bucket() as in the paper.
    bool contains(K key) {
        size_t idx = hash(key);
        auto [prev_ptr, curr] = find_bucket(idx, key);
        return (curr && curr->key == key);
    }

private:
    std::pair<std::atomic<MarkedPtr>*, Node<K, V>*> find_bucket(size_t idx, K key) {
        std::atomic<MarkedPtr>* prev_ptr = &buckets[idx];
        Node<K, V>* curr = get_node(prev_ptr->load());
        Node<K, V>* next_node = nullptr;

        while (true) {
            if (!curr) return { prev_ptr, nullptr };

            MarkedPtr curr_next = curr->next.load();
            next_node = get_node(curr_next);

            // Help remove marked nodes
            if (curr_next.marked()) {
                MarkedPtr expected = prev_ptr->load();
                if (expected.marked() || get_node(expected) != curr) {
                    prev_ptr = &buckets[idx]; // Restart
                    curr = get_node(prev_ptr->load());
                    continue;
                }

                MarkedPtr desired = MarkedPtr(next_node, false, expected.tag() + 1);
                prev_ptr->compare_exchange_strong(expected, desired);
                curr = next_node;
                continue;
            }

            if (curr->key >= key) return { prev_ptr, curr };
            prev_ptr = &curr->next;
            curr = next_node;
        }
    }
};