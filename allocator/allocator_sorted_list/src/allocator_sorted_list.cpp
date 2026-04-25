#include <not_implemented.h>
#include "../include/allocator_sorted_list.h"
#include <new>
#include <algorithm>
#include <iostream>

// Internal helpers to access metadata using the layout expected by allocator_metadata_size (68 bytes)
inline std::pmr::memory_resource** get_parent_allocator_ptr(void* trusted) {
    return reinterpret_cast<std::pmr::memory_resource**>(trusted);
}

inline allocator_with_fit_mode::fit_mode* get_fit_mode_ptr(void* trusted) {
    return reinterpret_cast<allocator_with_fit_mode::fit_mode*>(reinterpret_cast<char*>(trusted) + sizeof(std::pmr::memory_resource*));
}

inline size_t* get_total_size_ptr(void* trusted) {
    return reinterpret_cast<size_t*>(reinterpret_cast<char*>(trusted) + sizeof(std::pmr::memory_resource*) + sizeof(allocator_with_fit_mode::fit_mode));
}

inline std::mutex* get_mutex(void* trusted) {
    return reinterpret_cast<std::mutex*>(reinterpret_cast<char*>(trusted) + sizeof(std::pmr::memory_resource*) + sizeof(allocator_with_fit_mode::fit_mode) + sizeof(size_t));
}

inline void** get_first_free_ptr(void* trusted) {
    return reinterpret_cast<void**>(reinterpret_cast<char*>(trusted) + sizeof(std::pmr::memory_resource*) + sizeof(allocator_with_fit_mode::fit_mode) + sizeof(size_t) + sizeof(std::mutex));
}

inline char* get_managed_space_start(void* trusted) {
    // This should be exactly trusted + 68
    return reinterpret_cast<char*>(trusted) + (sizeof(std::pmr::memory_resource *) + sizeof(allocator_with_fit_mode::fit_mode) + sizeof(size_t) + sizeof(std::mutex) + sizeof(void*));
}

struct block_metadata {
    void *next_free;
    size_t size; // Size of the WHOLE block including this metadata
};

allocator_sorted_list::allocator_sorted_list(
        size_t space_size,
        std::pmr::memory_resource *parent_allocator,
        allocator_with_fit_mode::fit_mode allocate_fit_mode)
{
    // allocator_metadata_size is 68.
    size_t total_needed = 68 + space_size;
    if (parent_allocator == nullptr) {
        _trusted_memory = ::operator new(total_needed);
    } else {
        _trusted_memory = parent_allocator->allocate(total_needed);
    }

    *get_parent_allocator_ptr(_trusted_memory) = parent_allocator;
    *get_fit_mode_ptr(_trusted_memory) = allocate_fit_mode;
    *get_total_size_ptr(_trusted_memory) = space_size;

    new (get_mutex(_trusted_memory)) std::mutex();

    void** first_free = get_first_free_ptr(_trusted_memory);
    if (space_size >= sizeof(block_metadata)) {
        *first_free = get_managed_space_start(_trusted_memory);
        auto first_block = reinterpret_cast<block_metadata*>(*first_free);
        first_block->next_free = nullptr;
        first_block->size = space_size;
    } else {
        *first_free = nullptr;
    }
}

allocator_sorted_list::~allocator_sorted_list()
{
    if (_trusted_memory == nullptr) return;

    auto parent = *get_parent_allocator_ptr(_trusted_memory);
    size_t total_size = *get_total_size_ptr(_trusted_memory) + 68;

    get_mutex(_trusted_memory)->~mutex();

    if (parent == nullptr) {
        ::operator delete(_trusted_memory);
    } else {
        parent->deallocate(_trusted_memory, total_size);
    }
}

allocator_sorted_list::allocator_sorted_list(const allocator_sorted_list &other) {
    throw std::runtime_error("Copying not supported");
}

allocator_sorted_list &allocator_sorted_list::operator=(const allocator_sorted_list &other) {
    throw std::runtime_error("Copying not supported");
}

allocator_sorted_list::allocator_sorted_list(allocator_sorted_list &&other) noexcept
    : _trusted_memory(other._trusted_memory)
{
    other._trusted_memory = nullptr;
}

allocator_sorted_list &allocator_sorted_list::operator=(allocator_sorted_list &&other) noexcept
{
    if (this != &other) {
        this->~allocator_sorted_list();
        _trusted_memory = other._trusted_memory;
        other._trusted_memory = nullptr;
    }
    return *this;
}

[[nodiscard]] void *allocator_sorted_list::do_allocate_sm(size_t size)
{
    std::lock_guard<std::mutex> lock(*get_mutex(_trusted_memory));

    size_t needed = size + sizeof(block_metadata);
    void** first_free_ptr = get_first_free_ptr(_trusted_memory);

    void* best_curr = nullptr;
    void** best_prev_ptr = nullptr;
    size_t best_size = 0;

    auto mode = *get_fit_mode_ptr(_trusted_memory);

    void** curr_prev_ptr = first_free_ptr;
    void* curr = *curr_prev_ptr;

    while (curr != nullptr) {
        block_metadata* meta = reinterpret_cast<block_metadata*>(curr);
        if (meta->size >= needed) {
            if (mode == fit_mode::first_fit) {
                best_curr = curr;
                best_prev_ptr = curr_prev_ptr;
                break;
            } else if (mode == fit_mode::the_best_fit) {
                if (best_curr == nullptr || meta->size < best_size) {
                    best_curr = curr;
                    best_size = meta->size;
                    best_prev_ptr = curr_prev_ptr;
                }
            } else if (mode == fit_mode::the_worst_fit) {
                if (best_curr == nullptr || meta->size > best_size) {
                    best_curr = curr;
                    best_size = meta->size;
                    best_prev_ptr = curr_prev_ptr;
                }
            }
        }
        curr_prev_ptr = reinterpret_cast<void**>(curr); // next_free is at offset 0
        curr = meta->next_free;
    }

    if (best_curr == nullptr) {
        throw std::bad_alloc();
    }

    block_metadata* target = reinterpret_cast<block_metadata*>(best_curr);
    if (target->size >= needed + sizeof(block_metadata)) {
        size_t remaining_size = target->size - needed;
        void* next_free_node = reinterpret_cast<char*>(best_curr) + needed;
        block_metadata* next_meta = reinterpret_cast<block_metadata*>(next_free_node);
        next_meta->size = remaining_size;
        next_meta->next_free = target->next_free;

        *best_prev_ptr = next_free_node;
        target->size = needed;
    } else {
        *best_prev_ptr = target->next_free;
    }

    return reinterpret_cast<char*>(best_curr) + sizeof(block_metadata);
}

void allocator_sorted_list::do_deallocate_sm(void *at)
{
    if (at == nullptr) return;
    std::lock_guard<std::mutex> lock(*get_mutex(_trusted_memory));

    void* block_start = reinterpret_cast<char*>(at) - sizeof(block_metadata);
    block_metadata* to_free = reinterpret_cast<block_metadata*>(block_start);

    // Validation
    char* managed_start = get_managed_space_start(_trusted_memory);
    char* managed_end = managed_start + *get_total_size_ptr(_trusted_memory);
    if (reinterpret_cast<char*>(block_start) < managed_start || reinterpret_cast<char*>(block_start) >= managed_end) {
        throw std::runtime_error("Block does not belong to this allocator");
    }

    void** prev_ptr = get_first_free_ptr(_trusted_memory);
    void* curr = *prev_ptr;

    while (curr != nullptr && curr < block_start) {
        prev_ptr = reinterpret_cast<void**>(curr);
        curr = *prev_ptr;
    }

    to_free->next_free = curr;
    *prev_ptr = block_start;

    // Merge with next
    if (curr != nullptr && reinterpret_cast<char*>(block_start) + to_free->size == reinterpret_cast<char*>(curr)) {
        to_free->size += reinterpret_cast<block_metadata*>(curr)->size;
        to_free->next_free = reinterpret_cast<block_metadata*>(curr)->next_free;
    }

    // Merge with previous
    void** first_free_ptr = get_first_free_ptr(_trusted_memory);
    if (prev_ptr != first_free_ptr) {
        block_metadata* prev_block = reinterpret_cast<block_metadata*>(prev_ptr);
        if (reinterpret_cast<char*>(prev_block) + prev_block->size == reinterpret_cast<char*>(block_start)) {
            prev_block->size += to_free->size;
            prev_block->next_free = to_free->next_free;
        }
    }
}

bool allocator_sorted_list::do_is_equal(const std::pmr::memory_resource &other) const noexcept
{
    auto p = dynamic_cast<const allocator_sorted_list *>(&other);
    return p != nullptr && p->_trusted_memory == _trusted_memory;
}

inline void allocator_sorted_list::set_fit_mode(allocator_with_fit_mode::fit_mode mode)
{
    std::lock_guard<std::mutex> lock(*get_mutex(_trusted_memory));
    *get_fit_mode_ptr(_trusted_memory) = mode;
}

std::vector<allocator_test_utils::block_info> allocator_sorted_list::get_blocks_info() const noexcept
{
    std::lock_guard<std::mutex> lock(*get_mutex(_trusted_memory));
    return get_blocks_info_inner();
}

std::vector<allocator_test_utils::block_info> allocator_sorted_list::get_blocks_info_inner() const
{
    std::vector<allocator_test_utils::block_info> result;
    char* managed_start = get_managed_space_start(_trusted_memory);
    char* managed_end = managed_start + *get_total_size_ptr(_trusted_memory);
    char* curr = managed_start;
    void* free_ptr = *get_first_free_ptr(_trusted_memory);

    while (curr < managed_end) {
        block_metadata* meta = reinterpret_cast<block_metadata*>(curr);
        result.push_back({meta->size, (void*)curr != free_ptr});
        if ((void*)curr == free_ptr) {
            free_ptr = meta->next_free;
        }
        curr += meta->size;
    }
    return result;
}

allocator_sorted_list::sorted_free_iterator::sorted_free_iterator() : _free_ptr(nullptr) {}
allocator_sorted_list::sorted_free_iterator::sorted_free_iterator(void *ptr) : _free_ptr(ptr) {}

bool allocator_sorted_list::sorted_free_iterator::operator==(const sorted_free_iterator &other) const noexcept {
    return _free_ptr == other._free_ptr;
}
bool allocator_sorted_list::sorted_free_iterator::operator!=(const sorted_free_iterator &other) const noexcept {
    return !(*this == other);
}
allocator_sorted_list::sorted_free_iterator &allocator_sorted_list::sorted_free_iterator::operator++() & noexcept {
    if (_free_ptr) _free_ptr = reinterpret_cast<block_metadata*>(_free_ptr)->next_free;
    return *this;
}
allocator_sorted_list::sorted_free_iterator allocator_sorted_list::sorted_free_iterator::operator++(int) {
    sorted_free_iterator tmp = *this; ++(*this); return tmp;
}
size_t allocator_sorted_list::sorted_free_iterator::size() const noexcept {
    return _free_ptr ? reinterpret_cast<block_metadata*>(_free_ptr)->size : 0;
}
void *allocator_sorted_list::sorted_free_iterator::operator*() const noexcept {
    return _free_ptr ? reinterpret_cast<char*>(_free_ptr) + sizeof(block_metadata) : nullptr;
}

allocator_sorted_list::sorted_iterator::sorted_iterator() : _free_ptr(nullptr), _current_ptr(nullptr), _trusted_memory(nullptr) {}
allocator_sorted_list::sorted_iterator::sorted_iterator(void *trusted) : _trusted_memory(trusted) {
    if (_trusted_memory) {
        _current_ptr = get_managed_space_start(_trusted_memory);
        _free_ptr = *get_first_free_ptr(_trusted_memory);
    }
}
bool allocator_sorted_list::sorted_iterator::operator==(const sorted_iterator &other) const noexcept {
    return _current_ptr == other._current_ptr;
}
bool allocator_sorted_list::sorted_iterator::operator!=(const sorted_iterator &other) const noexcept {
    return !(*this == other);
}
allocator_sorted_list::sorted_iterator &allocator_sorted_list::sorted_iterator::operator++() & noexcept {
    if (_current_ptr) {
        size_t sz = reinterpret_cast<block_metadata*>(_current_ptr)->size;
        if (_current_ptr == _free_ptr) _free_ptr = reinterpret_cast<block_metadata*>(_free_ptr)->next_free;
        _current_ptr = reinterpret_cast<char*>(_current_ptr) + sz;
        char* managed_end = get_managed_space_start(_trusted_memory) + *get_total_size_ptr(_trusted_memory);
        if (_current_ptr >= managed_end) _current_ptr = nullptr;
    }
    return *this;
}
allocator_sorted_list::sorted_iterator allocator_sorted_list::sorted_iterator::operator++(int) {
    sorted_iterator tmp = *this; ++(*this); return tmp;
}
size_t allocator_sorted_list::sorted_iterator::size() const noexcept {
    return _current_ptr ? reinterpret_cast<block_metadata*>(_current_ptr)->size : 0;
}
void *allocator_sorted_list::sorted_iterator::operator*() const noexcept {
    return _current_ptr ? reinterpret_cast<char*>(_current_ptr) + sizeof(block_metadata) : nullptr;
}
bool allocator_sorted_list::sorted_iterator::occupied() const noexcept {
    return _current_ptr != _free_ptr;
}

allocator_sorted_list::sorted_free_iterator allocator_sorted_list::free_begin() const noexcept {
    return sorted_free_iterator(*get_first_free_ptr(_trusted_memory));
}
allocator_sorted_list::sorted_free_iterator allocator_sorted_list::free_end() const noexcept {
    return sorted_free_iterator(nullptr);
}
allocator_sorted_list::sorted_iterator allocator_sorted_list::begin() const noexcept {
    return sorted_iterator(_trusted_memory);
}
allocator_sorted_list::sorted_iterator allocator_sorted_list::end() const noexcept {
    return sorted_iterator();
}
