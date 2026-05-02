#include <not_implemented.h>
#include "../include/allocator_buddies_system.h"
#include <new>
#include <algorithm>
#include <iostream>

// Allocator metadata layout in trusted memory (using 256 bytes for enough free list heads)
static constexpr size_t ALLOC_META_SIZE = 256;

inline std::pmr::memory_resource** get_parent_ptr(void* trusted) { return reinterpret_cast<std::pmr::memory_resource**>(trusted); }
inline allocator_with_fit_mode::fit_mode* get_fit_mode_ptr(void* trusted) { return reinterpret_cast<allocator_with_fit_mode::fit_mode*>(reinterpret_cast<char*>(trusted) + 8); }
inline unsigned char* get_max_k_ptr(void* trusted) { return reinterpret_cast<unsigned char*>(reinterpret_cast<char*>(trusted) + 12); }
inline std::mutex* get_mutex(void* trusted) { return reinterpret_cast<std::mutex*>(reinterpret_cast<char*>(trusted) + 20); }
inline void** get_free_lists(void* trusted) { return reinterpret_cast<void**>(reinterpret_cast<char*>(trusted) + 64); }

struct block_meta {
    unsigned char occupied : 1;
    unsigned char k : 7;
};

struct free_block_node {
    block_meta meta;
    void* next;
};

struct occupied_block_node {
    block_meta meta;
    void* allocator_ptr;
};

// Metadata size is 1 (meta) + 8 (ptr) = 9.
// Smallest power of 2 for this is 2^4 = 16.

allocator_buddies_system::allocator_buddies_system(
    size_t space_size,
    std::pmr::memory_resource *parent_allocator,
    allocator_with_fit_mode::fit_mode allocate_fit_mode)
{
    if (space_size == 0) throw std::logic_error("Size cannot be 0");

    unsigned char k = 0;
    size_t size = 1;
    while (size < space_size) {
        if (size > (std::numeric_limits<size_t>::max() >> 1)) break;
        size <<= 1;
        k++;
    }
    if (size < space_size) throw std::bad_alloc();

    if (k < min_k) throw std::logic_error("Space size too small");

    size_t total_needed = ALLOC_META_SIZE + size;
    if (parent_allocator == nullptr) _trusted_memory = ::operator new(total_needed);
    else _trusted_memory = parent_allocator->allocate(total_needed);

    *get_parent_ptr(_trusted_memory) = parent_allocator;
    *get_fit_mode_ptr(_trusted_memory) = allocate_fit_mode;
    *get_max_k_ptr(_trusted_memory) = k;
    new (get_mutex(_trusted_memory)) std::mutex();

    void** lists = get_free_lists(_trusted_memory);
    for (int i = 0; i < 24; ++i) lists[i] = nullptr;

    void* managed_start = reinterpret_cast<char*>(_trusted_memory) + ALLOC_META_SIZE;
    free_block_node* first = reinterpret_cast<free_block_node*>(managed_start);
    first->meta.occupied = 0;
    first->meta.k = k;
    first->next = nullptr;
    lists[k] = first;
}

allocator_buddies_system::~allocator_buddies_system()
{
    if (_trusted_memory == nullptr) return;
    auto parent = *get_parent_ptr(_trusted_memory);
    size_t total_size = ALLOC_META_SIZE + (static_cast<size_t>(1) << *get_max_k_ptr(_trusted_memory));
    get_mutex(_trusted_memory)->~mutex();
    if (parent == nullptr) ::operator delete(_trusted_memory);
    else parent->deallocate(_trusted_memory, total_size);
}

allocator_buddies_system::allocator_buddies_system(const allocator_buddies_system &other) { throw std::runtime_error("Copying not supported"); }
allocator_buddies_system &allocator_buddies_system::operator=(const allocator_buddies_system &other) { throw std::runtime_error("Copying not supported"); }
allocator_buddies_system::allocator_buddies_system(allocator_buddies_system &&other) noexcept : _trusted_memory(other._trusted_memory) { other._trusted_memory = nullptr; }
allocator_buddies_system &allocator_buddies_system::operator=(allocator_buddies_system &&other) noexcept {
    if (this != &other) { this->~allocator_buddies_system(); _trusted_memory = other._trusted_memory; other._trusted_memory = nullptr; }
    return *this;
}

[[nodiscard]] void *allocator_buddies_system::do_allocate_sm(size_t size)
{
    std::lock_guard<std::mutex> lock(*get_mutex(_trusted_memory));

    unsigned char target_k = 0;
    size_t needed = size + 9;
    size_t block_size = 1;
    while (block_size < needed) {
        block_size <<= 1;
        target_k++;
    }
    if (target_k < min_k) target_k = static_cast<unsigned char>(min_k);

    void** lists = get_free_lists(_trusted_memory);
    int chosen_k = -1;
    auto mode = *get_fit_mode_ptr(_trusted_memory);

    if (mode == fit_mode::the_worst_fit) {
        for (int j = *get_max_k_ptr(_trusted_memory); j >= target_k; --j) {
            if (lists[j]) { chosen_k = j; break; }
        }
    } else {
        for (int j = target_k; j <= *get_max_k_ptr(_trusted_memory); ++j) {
            if (lists[j]) { chosen_k = j; break; }
        }
    }

    if (chosen_k == -1) throw std::bad_alloc();

    free_block_node* block = reinterpret_cast<free_block_node*>(lists[chosen_k]);
    lists[chosen_k] = block->next;

    while (chosen_k > target_k) {
        chosen_k--;
        size_t split_size = static_cast<size_t>(1) << chosen_k;
        free_block_node* buddy = reinterpret_cast<free_block_node*>(reinterpret_cast<char*>(block) + split_size);
        buddy->meta.occupied = 0;
        buddy->meta.k = static_cast<unsigned char>(chosen_k);
        buddy->next = lists[chosen_k];
        lists[chosen_k] = buddy;
        block->meta.k = static_cast<unsigned char>(chosen_k);
    }

    block->meta.occupied = 1;
    reinterpret_cast<occupied_block_node*>(block)->allocator_ptr = this;
    return reinterpret_cast<char*>(block) + 9;
}

void allocator_buddies_system::do_deallocate_sm(void *at)
{
    if (at == nullptr) return;
    std::lock_guard<std::mutex> lock(*get_mutex(_trusted_memory));
    occupied_block_node* block = reinterpret_cast<occupied_block_node*>(reinterpret_cast<char*>(at) - 9);
    if (block->allocator_ptr != this) throw std::runtime_error("Block does not belong to this allocator");

    unsigned char k = block->meta.k;
    block->meta.occupied = 0;

    void* curr_ptr = block;
    char* managed_start = reinterpret_cast<char*>(_trusted_memory) + ALLOC_META_SIZE;
    unsigned char max_k = *get_max_k_ptr(_trusted_memory);

    while (k < max_k) {
        size_t b_size = static_cast<size_t>(1) << k;
        size_t rel = reinterpret_cast<char*>(curr_ptr) - managed_start;
        size_t buddy_rel = rel ^ b_size;
        void* buddy_ptr = managed_start + buddy_rel;

        block_meta* b_meta = reinterpret_cast<block_meta*>(buddy_ptr);
        if (b_meta->occupied || b_meta->k != k) break;

        // Merge buddy: find and remove from free list
        void** lists = get_free_lists(_trusted_memory);
        free_block_node** p_next = reinterpret_cast<free_block_node**>(&lists[k]);
        free_block_node* it = reinterpret_cast<free_block_node*>(lists[k]);
        while (it && it != buddy_ptr) {
            p_next = reinterpret_cast<free_block_node**>(&it->next);
            it = reinterpret_cast<free_block_node*>(it->next);
        }
        if (it) {
            *p_next = reinterpret_cast<free_block_node*>(it->next);
            if (buddy_ptr < curr_ptr) curr_ptr = buddy_ptr;
            k++;
            reinterpret_cast<block_meta*>(curr_ptr)->k = k;
        } else break;
    }

    void** lists = get_free_lists(_trusted_memory);
    reinterpret_cast<free_block_node*>(curr_ptr)->next = lists[k];
    lists[k] = curr_ptr;
}

bool allocator_buddies_system::do_is_equal(const std::pmr::memory_resource& other) const noexcept {
    auto p = dynamic_cast<const allocator_buddies_system*>(&other);
    return p != nullptr && p->_trusted_memory == _trusted_memory;
}

inline void allocator_buddies_system::set_fit_mode(fit_mode mode) {
    std::lock_guard<std::mutex> lock(*get_mutex(_trusted_memory));
    *get_fit_mode_ptr(_trusted_memory) = mode;
}

std::vector<allocator_test_utils::block_info> allocator_buddies_system::get_blocks_info() const noexcept {
    std::lock_guard<std::mutex> lock(*get_mutex(_trusted_memory));
    return get_blocks_info_inner();
}

std::vector<allocator_test_utils::block_info> allocator_buddies_system::get_blocks_info_inner() const {
    std::vector<allocator_test_utils::block_info> res;
    char* curr = reinterpret_cast<char*>(_trusted_memory) + ALLOC_META_SIZE;
    char* end = curr + (static_cast<size_t>(1) << *get_max_k_ptr(_trusted_memory));
    while (curr < end) {
        block_meta* m = reinterpret_cast<block_meta*>(curr);
        res.push_back({static_cast<size_t>(1) << m->k, static_cast<bool>(m->occupied)});
        curr += (static_cast<size_t>(1) << m->k);
    }
    return res;
}

allocator_buddies_system::buddy_iterator::buddy_iterator() : _block(nullptr) {}
allocator_buddies_system::buddy_iterator::buddy_iterator(void* start) : _block(start) {}
bool allocator_buddies_system::buddy_iterator::operator==(const buddy_iterator& other) const noexcept { return _block == other._block; }
bool allocator_buddies_system::buddy_iterator::operator!=(const buddy_iterator& other) const noexcept { return !(*this == other); }
allocator_buddies_system::buddy_iterator& allocator_buddies_system::buddy_iterator::operator++() & noexcept {
    if (_block) _block = reinterpret_cast<char*>(_block) + (static_cast<size_t>(1) << reinterpret_cast<block_meta*>(_block)->k);
    return *this;
}
allocator_buddies_system::buddy_iterator allocator_buddies_system::buddy_iterator::operator++(int) { buddy_iterator t = *this; ++(*this); return t; }
size_t allocator_buddies_system::buddy_iterator::size() const noexcept { return _block ? (static_cast<size_t>(1) << reinterpret_cast<block_meta*>(_block)->k) : 0; }
bool allocator_buddies_system::buddy_iterator::occupied() const noexcept { return _block ? static_cast<bool>(reinterpret_cast<block_meta*>(_block)->occupied) : false; }
void* allocator_buddies_system::buddy_iterator::operator*() const noexcept { return _block ? reinterpret_cast<char*>(_block) + 9 : nullptr; }
allocator_buddies_system::buddy_iterator allocator_buddies_system::begin() const noexcept { return buddy_iterator(reinterpret_cast<char*>(_trusted_memory) + ALLOC_META_SIZE); }
allocator_buddies_system::buddy_iterator allocator_buddies_system::end() const noexcept { return buddy_iterator(reinterpret_cast<char*>(_trusted_memory) + ALLOC_META_SIZE + (static_cast<size_t>(1) << *get_max_k_ptr(_trusted_memory))); }
