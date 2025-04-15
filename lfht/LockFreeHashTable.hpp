// Contributors: Grace Biggs
// Adapted from Michael's "High performance dynamic lock-free hash tables and list-based sets": https://doi.org/10.1145/564870.564881 / https://citeseerx.ist.psu.edu/document?repid=rep1&type=pdf&doi=fb47306b9007a2a901f7214e2a9b2005f20ed4b7
#pragma once
#include <atomic>
#include <vector>
#include <cstdint>
#include <functional>
#include <algorithm>
#include <iostream>
#include <unordered_set>
#include <memory>
#include <thread>

// Combined MarkedPtr (64-bit for atomic operations)
// Continguous Data layout: [marked (1)][tag (15)][ptr (48)]
// marked = marked for deletion
// tag = versioning for CAS (CAS succeeds only if the tag has not changed since the thread last read the location)
// ptr = pointer to the next node in the linked list
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

    bool operator!=(const MarkedPtr& other) const {
        return data != other.data;
    }
};

static_assert(sizeof(MarkedPtr) == sizeof(uint64_t), "MarkedPtr must be 64 bits");
static_assert(std::atomic<MarkedPtr>::is_always_lock_free, "Atomic MarkedPtr not lock-free");

// Node structure
// Each node contains a key, value, and a MarkedPtr to the next node
// Key is the key for the hash table
// Value is the value associated with that key
// Next is a pointer to the next node in the linked list
template <typename K, typename V>
struct Node {
    K key;
    V value;
    std::atomic<MarkedPtr> next;

    Node(K k, V v) : key(k), value(v), next(MarkedPtr(nullptr, false, 0)) {}
};

// Bucket array wrapper
// Each bucket array contains a vector of atomic MarkedPtr, which points to the head of the linked list
// Any hashkey that is not unique will be stored in the same bucket
// The size of the bucket array is determined by the number of buckets
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
// The hash table contains a pointer to the current bucket array
// Count is the total number of elements in the hash table
// Resizing is a flag to indicate if the hash table is currently resizing
// MIN_BUCKETS is the minimum number of buckets in the hash table to avoid excessive resizing
// load factor is the ratio of the number of elements to the number of buckets

template <typename K, typename V>
class LockFreeHashTable {
private:
    std::atomic<BucketArray<K, V>*> current_array;
    std::atomic<size_t> count;
    std::atomic<bool> resizing{ false };
    static constexpr size_t MIN_BUCKETS = 64;
    static constexpr double UPPER_LOAD_FACTOR = 2.0;
    static constexpr double LOWER_LOAD_FACTOR = 0.25;
    
    #pragma region "SMR"

        // SMR Types
        struct HazardRecord {
            std::atomic<Node<K, V>*> hazard_pointer{ nullptr };
            std::atomic<HazardRecord*> next_pointer{ nullptr };
        };

        static constexpr int HP_COUNT_PER_THREAD = 3;
        static std::atomic<HazardRecord*> hp_head;

        // One set of hazard pointers per thread
        inline static thread_local HazardRecord* hp_records{ nullptr };
        inline static thread_local int hazard_pointer_index = -1;

        // List of retired nodes for safe memory reclamation
        static std::atomic<Node<K, V>*> retired_list;
        static std::atomic<size_t> retired_count;

        // SMR management functions
        void init_thread_hp();
        void retire_node(Node<K, V>* node);
        void scan_retired_nodes();
        void free_retired_node(Node<K, V>* node);  // Optionally implement this

    #pragma endregion

public:
    LockFreeHashTable() : current_array(new BucketArray<K, V>(MIN_BUCKETS)), count(0) {}

    ~LockFreeHashTable() {
        scan_retired_nodes(); // Clean up retired nodes
        // Clean up the current array
        delete current_array.load();
    }

    //@brief Inserts a key-value pair into the hash table.
    //@param key The key to insert.
    //@param value The value to insert.
    //@return true if the insertion was successful, false if the key already exists.
    //@note This function may trigger a resize if the load factor exceeds the upper limit.
    bool insert(K key, V value) {
        while (true) {
            BucketArray<K, V>* array = current_array.load();
            size_t idx = hash(key, array->size);
            Node<K, V>* new_node = new Node<K, V>(key, value);

            // auto = std::pair<std::atomic<MarkedPtr>*, Node<K, V>*>
            auto [prev_nextPtr, curr] = find_bucket(array, idx, key);

            // Key exists, do "nothing" (deallocate new_node first). TODO: MemoryPool or Freelist this instead. (Lane: Look into SMR)
            if (curr && curr->key == key) {
                delete new_node;
                return false;
            }

            // Link new node
            new_node->next.store(MarkedPtr(curr, false, 0));

            // Prepare CAS
            // Get next pointer of prev_ptr
            MarkedPtr expected = prev_nextPtr->load();
            // if prev_ptr is marked for deletion
            // or if the next pointer of prev_ptr is not curr
            if (expected.marked() || get_node(expected) != curr) continue; 

            MarkedPtr desired = MarkedPtr(new_node, false, expected.tag() + 1);
            if (prev_nextPtr->compare_exchange_strong(expected, desired)) {
                size_t c = count.fetch_add(1, std::memory_order_relaxed) + 1;
                // if current load factor is above the upper limit
                // try to resize the array to double its size
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
            // node does not exist or have been changed
            if (!curr || curr->key != key) return false;

            // Try to mark node
            MarkedPtr curr_next = curr->next.load();
            if (curr_next.marked()) continue;

            // Prepare CAS
            // Mark the current node as deleted
            // and increment the tag
            MarkedPtr desired_marked = MarkedPtr(curr_next.ptr(), true, curr_next.tag() + 1);
            if (!curr->next.compare_exchange_strong(curr_next, desired_marked)) continue;

            // Check if the previous node is marked for deletion
            // and if the next pointer of the previous node is not curr
            MarkedPtr prev_expected = prev_ptr->load();
            if (prev_expected.marked() || get_node(prev_expected) != curr) continue;

            // Physically remove
            MarkedPtr prev_desired = MarkedPtr(curr_next.ptr(), false, prev_expected.tag() + 1);
            if (prev_ptr->compare_exchange_strong(prev_expected, prev_desired)) {
                retire_node(curr); // SMR
                // Decrement the count
                size_t c = count.fetch_sub(1, std::memory_order_relaxed) - 1;
                if (static_cast<double>(c) / array->size < LOWER_LOAD_FACTOR) {
                    try_resize(array, std::max(MIN_BUCKETS, array->size / 2));
                }
                return true;
            }
        }
    }

    //@brief Checks if the hash table contains a key.
    //@param key The key to check.
    //@return true if the key exists, false otherwise.
    bool contains(K key) {
        BucketArray<K, V>* array = current_array.load();
        size_t idx = hash(key, array->size);
        auto [prev_ptr, curr] = find_bucket(array, idx, key);
        return (curr && curr->key == key);
    }

private:
    //@brief Hash function to map a key to an index in the bucket array.
    //@param key The key to hash.
    //@param size The size of the bucket array.
    //@return The index in the bucket array.
    size_t hash(K key, size_t size) const {
        return std::hash<K>{}(key) % size;
    }

    //@brief Get the node from a MarkedPtr.
    //@param mp The MarkedPtr to get the node from.
    //@return The node pointed to by the MarkedPtr.
    Node<K, V>* get_node(MarkedPtr mp) const {
        return static_cast<Node<K, V>*>(mp.ptr());
    }

    //@brief Find the bucket for a given key in the bucket array.
    //@param array The bucket array to search in.
    //@param idx The index of the bucket to search in.
    //@param key The key to search for.
    //@return A pair containing the pointer to the previous node and the current node.
    std::pair<std::atomic<MarkedPtr>*, Node<K, V>*>
        find_bucket(BucketArray<K, V>* array, size_t idx, K key)
    {


        std::atomic<MarkedPtr>* prev_nextPtr = &array->buckets[idx];
        Node<K, V>* curr = get_node(prev_nextPtr->load());
        Node<K, V>* next_node = nullptr;

        // Initialize hazard pointer index
        init_thread_hp();
        hp_records[0].hazard_pointer.store(next_node, std::memory_order_release);    // hp0
        hp_records[1].hazard_pointer.store(curr, std::memory_order_release);         // hp1
        hp_records[2].hazard_pointer.store(get_node(prev_nextPtr->load()), std::memory_order_release); // hp2

        while (true) {
            if (!curr) return { prev_nextPtr, nullptr };

            MarkedPtr curr_nextPtr = curr->next.load();
            next_node = get_node(curr_nextPtr);

            // SMR
            // Protect next_node first (hp0)
            hp_records[0].hazard_pointer.store(next_node, std::memory_order_release);
            // Verify nothing changed
            if (curr->next.load() != curr_nextPtr) {
                curr = get_node(prev_nextPtr->load());
                continue;
            }

            // Remove pointers to current marked node
            // Will be freed in next Remove call.
            if (curr_nextPtr.marked()) {
                MarkedPtr expected_curr = prev_nextPtr->load();
                if (expected_curr.marked() || get_node(expected_curr) != curr) {
                    prev_nextPtr = &array->buckets[idx];
                    curr = get_node(prev_nextPtr->load());
                    continue;
                }

                MarkedPtr desired = MarkedPtr(next_node, false, expected_curr.tag() + 1);
                prev_nextPtr->compare_exchange_strong(expected_curr, desired);
                retire_node(curr); // SMR
                curr = next_node;
                continue;
            }

            if (curr->key >= key) return { prev_nextPtr, curr };

            hp_records[2].hazard_pointer.store(curr, std::memory_order_release);
            prev_nextPtr = &curr->next;
            hp_records[1].hazard_pointer.store(next_node, std::memory_order_release);
            curr = next_node;
        }
    }

    //@brief Resize the hash table to a new size
    //@param old_array The old bucket array to resize
    //@param new_size The new size for the bucket array
    //@note Only fails if resizing is happening or has happened
    void try_resize(BucketArray<K, V>* old_array, size_t new_size) {
        // if already resized, return
        if (new_size == old_array->size) return;
        // Check if resizing is already in progress
        if (!resizing.exchange(true)) {
            BucketArray<K, V>* new_array = new BucketArray<K, V>(new_size);

            for (size_t i = 0; i < old_array->size; ++i) {
                rehash_bucket(old_array, new_array, i);
            }

            if (current_array.compare_exchange_strong(old_array, new_array)) {
                delete old_array; // TODO: Hazard pointer implementation with this? (Lane: Look into SMR)
            }
            else {
                delete new_array;
            }
            resizing.store(false);
        }
    }

    //@brief Rehash the bucket at the given index from the old array to the new array
    //@param old_array The old bucket array to rehash from
    //@param new_array The new bucket array to rehash to
    void rehash_bucket(BucketArray<K, V>* old_array,
        BucketArray<K, V>* new_array,
        size_t old_idx)
    {
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

// Define static members (template headers needed)
template <typename K, typename V>  
std::atomic<typename LockFreeHashTable<K, V>::HazardRecord*> LockFreeHashTable<K, V>::hp_head{ nullptr };
template <typename K, typename V>
std::atomic<Node<K, V>*> LockFreeHashTable<K, V>::retired_list{ nullptr };

template <typename K, typename V>
std::atomic<size_t> LockFreeHashTable<K, V>::retired_count{ 0 };

template <typename K, typename V>
void LockFreeHashTable<K, V>::init_thread_hp() {
    if (!hp_records) {
        hp_records = new HazardRecord[HP_COUNT_PER_THREAD];
        for (int i = 0; i < HP_COUNT_PER_THREAD; ++i) {
            hp_records[i].hazard_pointer.store(nullptr, std::memory_order_relaxed);
        }

        HazardRecord* old_head = hp_head.load(std::memory_order_relaxed);
        do {
            hp_records[HP_COUNT_PER_THREAD - 1].next_pointer.store(old_head, std::memory_order_relaxed);
        } while (!hp_head.compare_exchange_weak(
            old_head,
            hp_records,
            std::memory_order_release,
            std::memory_order_relaxed));
    }
}

template <typename K, typename V>
void LockFreeHashTable<K, V>::retire_node(Node<K, V>* node) {
    Node<K, V>* old_head = retired_list.load(std::memory_order_relaxed);
    do {
        node->next.store(MarkedPtr(old_head, false, 0), std::memory_order_relaxed);
    } while (!retired_list.compare_exchange_weak(
        old_head,
        node,
        std::memory_order_release,
        std::memory_order_relaxed));

    if (retired_count.fetch_add(1, std::memory_order_relaxed) + 1 >=
        2 * std::thread::hardware_concurrency() * HP_COUNT_PER_THREAD) {
        scan_retired_nodes();
    }
}

template <typename K, typename V>
void LockFreeHashTable<K, V>::scan_retired_nodes() {
    std::unordered_set<Node<K, V>*> protected_ptrs;

    HazardRecord* current = hp_head.load(std::memory_order_acquire);
    while (current) {
        for (int i = 0; i < HP_COUNT_PER_THREAD; ++i) {
            Node<K, V>* ptr = current[i].hazard_pointer.load(std::memory_order_acquire);
            if (ptr) protected_ptrs.insert(ptr);
        }
        current = current[HP_COUNT_PER_THREAD - 1].next_pointer.load(std::memory_order_acquire);
    }

    Node<K, V>* old_head = retired_list.exchange(nullptr, std::memory_order_acquire);
    size_t new_count = 0;

    while (old_head) {
        Node<K, V>* next = static_cast<Node<K, V>*>(old_head->next.load().ptr());
        if (protected_ptrs.count(old_head)) {
            Node<K, V>* temp_head;
            do {
                temp_head = retired_list.load(std::memory_order_relaxed);
                old_head->next.store(MarkedPtr(temp_head, false, 0), std::memory_order_relaxed);
            } while (!retired_list.compare_exchange_weak(
                temp_head,
                old_head,
                std::memory_order_release,
                std::memory_order_relaxed));
            new_count++;
        }
        else {
            free_retired_node(old_head);
        }
        old_head = next;
    }

    retired_count.store(new_count, std::memory_order_release);
}

template <typename K, typename V>
void LockFreeHashTable<K, V>::free_retired_node(Node<K, V>* node) {
    delete node;
}