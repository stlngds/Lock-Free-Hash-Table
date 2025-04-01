// Contributors: Grace Biggs
// Adapted from Michael's "High performance dynamic lock-free hash tables and list-based sets": https://doi.org/10.1145/564870.564881 / https://citeseerx.ist.psu.edu/document?repid=rep1&type=pdf&doi=fb47306b9007a2a901f7214e2a9b2005f20ed4b7
#pragma once
#include <atomic>
#include <vector>
#include <cstdint>
#include <functional>
#include <algorithm>
#include <iostream>

// Combined MarkedPtr (64-bit for atomic operations)
struct MarkedPtr {
    uint64_t data;

    // Bitshift masks to get the following layout for our contiguous data: [marked (1)][tag (15)][ptr (48)]
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

// Node structure
template <typename K, typename V>
struct Node {
    K key;
    V value;
    std::atomic<MarkedPtr> next;

    Node(K k, V v) : key(k), value(v), next(MarkedPtr(nullptr, false, 0)) {}
};

// Bucket array wrapper
template <typename K, typename V>
struct BucketArray {
    std::vector<std::atomic<MarkedPtr>> buckets;
    const size_t size;

    BucketArray(size_t s) : size(s), buckets(s) {
        for (auto& head : buckets) {
            head.store(MarkedPtr(nullptr, false, 0));
        }
    }
};

// Main hash table class
template <typename K, typename V>
class LockFreeHashTable {
private:
    std::atomic<BucketArray<K, V>*> current_array;
    std::atomic<size_t> count;
    std::atomic<bool> resizing{ false };
    static constexpr size_t MIN_BUCKETS = 64;
    static constexpr double UPPER_LOAD_FACTOR = 2.0;
    static constexpr double LOWER_LOAD_FACTOR = 0.25;

public:
    LockFreeHashTable() : current_array(new BucketArray<K, V>(MIN_BUCKETS)), count(0) {}

    ~LockFreeHashTable() {
        delete current_array.load();
    }

    bool insert(K key, V value) {
        while (true) {
            BucketArray<K, V>* array = current_array.load();
            size_t idx = hash(key, array->size);
            Node<K, V>* new_node = new Node<K, V>(key, value);

            auto [prev_ptr, curr] = find_bucket(array, idx, key);

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
                size_t c = count.fetch_add(1, std::memory_order_relaxed) + 1;
                if (static_cast<double>(c) / array->size > UPPER_LOAD_FACTOR) {
                    try_resize(array, array->size * 2);
                }
                return true;
            }
        }
    }

    bool remove(K key) {
        while (true) {
            BucketArray<K, V>* array = current_array.load();
            size_t idx = hash(key, array->size);

            auto [prev_ptr, curr] = find_bucket(array, idx, key);
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
                // TODO: Safe memory reclamation (i.e., MemoryBank, Freelist)
                delete curr;
                size_t c = count.fetch_sub(1, std::memory_order_relaxed) - 1;
                if (static_cast<double>(c) / array->size < LOWER_LOAD_FACTOR) {
                    try_resize(array, std::max(MIN_BUCKETS, array->size / 2));
                }
                return true;
            }
        }
    }

    // contains() is more-or-less a public wrapper for find_bucket() as in the paper.
    bool contains(K key) {
        BucketArray<K, V>* array = current_array.load();
        size_t idx = hash(key, array->size);
        auto [prev_ptr, curr] = find_bucket(array, idx, key);
        return (curr && curr->key == key);
    }

private:
    size_t hash(K key, size_t size) const {
        return std::hash<K>{}(key) % size;
    }

    Node<K, V>* get_node(MarkedPtr mp) const {
        return static_cast<Node<K, V>*>(mp.ptr());
    }

    std::pair<std::atomic<MarkedPtr>*, Node<K, V>*>
        find_bucket(BucketArray<K, V>* array, size_t idx, K key) {
        std::atomic<MarkedPtr>* prev_ptr = &array->buckets[idx];
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
                    prev_ptr = &array->buckets[idx];
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

    void try_resize(BucketArray<K, V>* old_array, size_t new_size) {
        if (new_size == old_array->size) return;
        if (!resizing.exchange(true)) {
            BucketArray<K, V>* new_array = new BucketArray<K, V>(new_size);

            for (size_t i = 0; i < old_array->size; ++i) {
                rehash_bucket(old_array, new_array, i);
            }

            if (current_array.compare_exchange_strong(old_array, new_array)) {
                delete old_array; // In production: use hazard pointers
            }
            else {
                delete new_array;
            }
            resizing.store(false);
        }
    }

    void rehash_bucket(BucketArray<K, V>* old_array,
        BucketArray<K, V>* new_array,
        size_t old_idx) {
        auto& old_head = old_array->buckets[old_idx];
        Node<K, V>* curr = get_node(old_head.load());

        while (curr) {
            size_t new_idx = hash(curr->key, new_array->size);
            auto& new_head = new_array->buckets[new_idx];

            Node<K, V>* new_node = new Node<K, V>(curr->key, curr->value);
            MarkedPtr expected = new_head.load();
            new_node->next.store(expected);
            MarkedPtr desired = MarkedPtr(new_node, false, expected.tag() + 1);

            while (!new_head.compare_exchange_weak(expected, desired)) {
                new_node->next.store(expected);
            }

            Node<K, V>* next = get_node(curr->next.load());  
            curr = next;
        }
    }
};
