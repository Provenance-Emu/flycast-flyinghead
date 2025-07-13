#pragma once
#include "types.h"
#include <array>
#include <atomic>

namespace cache_opt {

/// Cache-line sized and aligned structure to reduce false sharing
#define CACHE_ALIGNED alignas(64)

/// Optimized memory pool allocator for frequent allocations
template<typename T, size_t PoolSize = 1024>
class CacheOptimizedPool {
    static constexpr size_t CHUNK_SIZE = sizeof(T);
    static constexpr size_t ALIGN_SIZE = std::max(sizeof(T), size_t(64));

    CACHE_ALIGNED char pool[PoolSize * ALIGN_SIZE];
    CACHE_ALIGNED std::atomic<size_t> nextIndex{0};
    CACHE_ALIGNED std::array<std::atomic<bool>, PoolSize> inUse{};

public:
    T* allocate() {
        for (size_t attempts = 0; attempts < PoolSize; attempts++) {
            size_t index = nextIndex.fetch_add(1) % PoolSize;
            bool expected = false;
            if (inUse[index].compare_exchange_weak(expected, true)) {
                return reinterpret_cast<T*>(&pool[index * ALIGN_SIZE]);
            }
        }
        return nullptr; // Pool exhausted
    }

    void deallocate(T* ptr) {
        if (!ptr) return;
        size_t offset = reinterpret_cast<char*>(ptr) - pool;
        size_t index = offset / ALIGN_SIZE;
        if (index < PoolSize) {
            inUse[index].store(false);
        }
    }

    bool owns(T* ptr) const {
        return ptr >= reinterpret_cast<const T*>(pool) &&
               ptr < reinterpret_cast<const T*>(pool + sizeof(pool));
    }
};

/// Structure-of-Arrays layout for better cache performance
template<size_t N>
struct SOA_TLBEntry {
    // Separate arrays for better cache locality during searches
    CACHE_ALIGNED std::array<u32, N> vpn;           // Virtual page numbers
    CACHE_ALIGNED std::array<u32, N> ppn;           // Physical page numbers
    CACHE_ALIGNED std::array<u8, N> asid;           // Address space IDs
    CACHE_ALIGNED std::array<u8, N> valid;          // Valid flags
    CACHE_ALIGNED std::array<u8, N> size;           // Page sizes
    CACHE_ALIGNED std::array<u8, N> protection;     // Protection bits

    // Optimized lookup with early exit
    int findEntry(u32 virtual_addr, u8 address_space_id) const {
        const u32 page = virtual_addr >> 12;

        // Vectorized search when possible
        for (size_t i = 0; i < N; i += 4) {
            // Check 4 entries at once for better cache utilization
            if (valid[i] && vpn[i] == page && asid[i] == address_space_id) return i;
            if (i + 1 < N && valid[i + 1] && vpn[i + 1] == page && asid[i + 1] == address_space_id) return i + 1;
            if (i + 2 < N && valid[i + 2] && vpn[i + 2] == page && asid[i + 2] == address_space_id) return i + 2;
            if (i + 3 < N && valid[i + 3] && vpn[i + 3] == page && asid[i + 3] == address_space_id) return i + 3;
        }
        return -1; // Not found
    }
};

/// Lock-free ring buffer for high-performance event logging
template<typename T, size_t Size>
class LockFreeRingBuffer {
    static_assert((Size & (Size - 1)) == 0, "Size must be power of 2");

    CACHE_ALIGNED std::array<T, Size> buffer;
    CACHE_ALIGNED std::atomic<size_t> writeIndex{0};
    CACHE_ALIGNED std::atomic<size_t> readIndex{0};

public:
    bool push(const T& item) {
        const size_t currentWrite = writeIndex.load(std::memory_order_relaxed);
        const size_t nextWrite = (currentWrite + 1) & (Size - 1);

        if (nextWrite == readIndex.load(std::memory_order_acquire)) {
            return false; // Buffer full
        }

        buffer[currentWrite] = item;
        writeIndex.store(nextWrite, std::memory_order_release);
        return true;
    }

    bool pop(T& item) {
        const size_t currentRead = readIndex.load(std::memory_order_relaxed);

        if (currentRead == writeIndex.load(std::memory_order_acquire)) {
            return false; // Buffer empty
        }

        item = buffer[currentRead];
        readIndex.store((currentRead + 1) & (Size - 1), std::memory_order_release);
        return true;
    }

    bool empty() const {
        return readIndex.load() == writeIndex.load();
    }

    size_t size() const {
        const size_t w = writeIndex.load();
        const size_t r = readIndex.load();
        return (w - r) & (Size - 1);
    }
};

/// Optimized hash table with linear probing and cache-friendly layout
template<typename Key, typename Value, size_t Capacity>
class CacheOptimizedHashMap {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");

    struct Entry {
        Key key;
        Value value;
        std::atomic<bool> occupied{false};

        Entry() = default;
        Entry(const Key& k, const Value& v) : key(k), value(v), occupied(true) {}
    };

    CACHE_ALIGNED std::array<Entry, Capacity> table;
    CACHE_ALIGNED std::atomic<size_t> count{0};

    size_t hash(const Key& key) const {
        // Simple hash for u32 keys, can be specialized
        return static_cast<size_t>(key) & (Capacity - 1);
    }

public:
    bool insert(const Key& key, const Value& value) {
        if (count.load() >= Capacity * 3 / 4) {
            return false; // Load factor too high
        }

        size_t index = hash(key);

        for (size_t i = 0; i < Capacity; i++) {
            Entry& entry = table[index];
            bool expected = false;

            if (entry.occupied.compare_exchange_weak(expected, true)) {
                entry.key = key;
                entry.value = value;
                count.fetch_add(1);
                return true;
            } else if (entry.key == key) {
                entry.value = value; // Update existing
                return true;
            }

            index = (index + 1) & (Capacity - 1);
        }

        return false; // Table full
    }

    bool find(const Key& key, Value& value) const {
        size_t index = hash(key);

        for (size_t i = 0; i < Capacity; i++) {
            const Entry& entry = table[index];

            if (!entry.occupied.load()) {
                return false; // Empty slot, key not found
            }

            if (entry.key == key) {
                value = entry.value;
                return true;
            }

            index = (index + 1) & (Capacity - 1);
        }

        return false; // Not found
    }

    void clear() {
        for (auto& entry : table) {
            entry.occupied.store(false);
        }
        count.store(0);
    }
};

/// Cache-optimized memory allocator using segregated free lists
class SegregatedAllocator {
    static constexpr size_t NUM_SIZE_CLASSES = 16;
    static constexpr size_t MIN_BLOCK_SIZE = 16;
    static constexpr size_t MAX_BLOCK_SIZE = MIN_BLOCK_SIZE << (NUM_SIZE_CLASSES - 1);

    struct FreeBlock {
        FreeBlock* next;
    };

    CACHE_ALIGNED std::array<FreeBlock*, NUM_SIZE_CLASSES> freeLists{};
    CACHE_ALIGNED std::atomic<size_t> totalAllocated{0};

    size_t getSizeClass(size_t size) const {
        if (size <= MIN_BLOCK_SIZE) return 0;
        return std::min(NUM_SIZE_CLASSES - 1,
                       static_cast<size_t>(32 - __builtin_clz(size - 1)) - 4);
    }

    size_t getBlockSize(size_t sizeClass) const {
        return MIN_BLOCK_SIZE << sizeClass;
    }

public:
    void* allocate(size_t size) {
        size_t sizeClass = getSizeClass(size);
        FreeBlock*& freeList = freeLists[sizeClass];

        if (freeList) {
            void* result = freeList;
            freeList = freeList->next;
            totalAllocated.fetch_add(getBlockSize(sizeClass));
            return result;
        }

        // Allocate new block
        size_t blockSize = getBlockSize(sizeClass);
        void* block = std::aligned_alloc(64, blockSize);

        if (block) {
            totalAllocated.fetch_add(blockSize);
        }

        return block;
    }

    void deallocate(void* ptr, size_t size) {
        if (!ptr) return;

        size_t sizeClass = getSizeClass(size);
        FreeBlock* block = static_cast<FreeBlock*>(ptr);

        block->next = freeLists[sizeClass];
        freeLists[sizeClass] = block;

        totalAllocated.fetch_sub(getBlockSize(sizeClass));
    }

    size_t getTotalAllocated() const {
        return totalAllocated.load();
    }
};

} // namespace cache_opt
