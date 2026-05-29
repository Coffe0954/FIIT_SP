#include <not_implemented.h>
#include "../include/allocator_boundary_tags.h"
#include <new>
#include <algorithm>
#include <iostream>

inline std::pmr::memory_resource** get_parent_allocator_ptr(void* trusted) {
    return reinterpret_cast<std::pmr::memory_resource**>(trusted);
}
inline allocator_with_fit_mode::fit_mode* get_fit_mode_ptr(void* trusted) {
    return reinterpret_cast<allocator_with_fit_mode::fit_mode*>(reinterpret_cast<char*>(trusted) + 8);
}
inline size_t* get_total_size_ptr(void* trusted) {
    return reinterpret_cast<size_t*>(reinterpret_cast<char*>(trusted) + 12);
}
inline std::mutex* get_mutex(void* trusted) {
    return reinterpret_cast<std::mutex*>(reinterpret_cast<char*>(trusted) + 20);
}
inline void** get_first_block_ptr(void* trusted) {
    return reinterpret_cast<void**>(reinterpret_cast<char*>(trusted) + 60); //по другому сделать метадата не ддолжна быть в свободных блоках 
}

struct block_metadata {
    size_t size;
    block_metadata* prev;
    block_metadata* next;
    void* allocator_ptr;
};

static constexpr size_t METADATA_SIZE = 32;

inline bool is_occupied(const block_metadata* meta) {
    return (meta->size & 1) != 0;
}
inline size_t get_size(const block_metadata* meta) {
    return meta->size & ~static_cast<size_t>(1);
}
inline void set_occupied(block_metadata* meta, bool occupied) {
    if (occupied) meta->size |= 1;
    else meta->size &= ~static_cast<size_t>(1);
}

allocator_boundary_tags::allocator_boundary_tags(
    size_t space_size,
    std::pmr::memory_resource *parent_allocator,
    allocator_with_fit_mode::fit_mode allocate_fit_mode)
{
    size_t total_needed = 68 + space_size;
    if (parent_allocator == nullptr) {
        _trusted_memory = ::operator new(total_needed);
    } else {
        _trusted_memory = parent_allocator->allocate(total_needed);
    }

    *get_parent_allocator_ptr(_trusted_memory) = parent_allocator;
    *get_fit_mode_ptr(_trusted_memory) = allocate_fit_mode;
    *get_total_size_ptr(_trusted_memory) = space_size;
    new (get_mutex(_trusted_memory)) std::mutex(); //инициализация мьютекса в выделенной памяти

    void** first_block_ptr = get_first_block_ptr(_trusted_memory);
    if (space_size >= METADATA_SIZE) {
        block_metadata* first = reinterpret_cast<block_metadata*>(reinterpret_cast<char*>(_trusted_memory) + 68);
        first->size = space_size;
        first->prev = nullptr;
        first->next = nullptr;
        first->allocator_ptr = this;
        *first_block_ptr = first;
    } else {
        *first_block_ptr = nullptr;
    }
}

allocator_boundary_tags::~allocator_boundary_tags()
{
    if (_trusted_memory == nullptr) return;
    auto parent = *get_parent_allocator_ptr(_trusted_memory);
    size_t total_size = *get_total_size_ptr(_trusted_memory) + 68;
    get_mutex(_trusted_memory)->~mutex();
    if (parent == nullptr) ::operator delete(_trusted_memory);
    else parent->deallocate(_trusted_memory, total_size);
}

allocator_boundary_tags::allocator_boundary_tags(const allocator_boundary_tags &other) {
    throw std::runtime_error("Copying not supported");
}
allocator_boundary_tags &allocator_boundary_tags::operator=(const allocator_boundary_tags &other) {
    throw std::runtime_error("Copying not supported");
}
allocator_boundary_tags::allocator_boundary_tags(allocator_boundary_tags &&other) noexcept
    : _trusted_memory(other._trusted_memory) {
    other._trusted_memory = nullptr;
}
allocator_boundary_tags &allocator_boundary_tags::operator=(allocator_boundary_tags &&other) noexcept {
    if (this != &other) {
        this->~allocator_boundary_tags();
        _trusted_memory = other._trusted_memory;
        other._trusted_memory = nullptr;
    }
    return *this;
}

[[nodiscard]] void *allocator_boundary_tags::do_allocate_sm(size_t bytes)
{
    std::lock_guard<std::mutex> lock(*get_mutex(_trusted_memory));
    size_t needed = bytes + METADATA_SIZE;
    block_metadata* curr = reinterpret_cast<block_metadata*>(*get_first_block_ptr(_trusted_memory));
    block_metadata* best = nullptr;
    auto mode = *get_fit_mode_ptr(_trusted_memory);

    while (curr != nullptr) {
        if (!is_occupied(curr) && get_size(curr) >= needed) {
            if (mode == fit_mode::first_fit) {
                best = curr;
                break;
            } else if (mode == fit_mode::the_best_fit) {
                if (best == nullptr || get_size(curr) < get_size(best)) best = curr;
            } else if (mode == fit_mode::the_worst_fit) {
                if (best == nullptr || get_size(curr) > get_size(best)) best = curr;
            }
        }
        curr = curr->next;
    }

    if (best == nullptr) throw std::bad_alloc();

    if (get_size(best) >= needed + METADATA_SIZE) {
        // Split
        size_t old_size = get_size(best);
        block_metadata* next_block = reinterpret_cast<block_metadata*>(reinterpret_cast<char*>(best) + needed);
        next_block->size = old_size - needed;
        next_block->prev = best;
        next_block->next = best->next;
        next_block->allocator_ptr = this;
        if (best->next) best->next->prev = next_block;
        best->next = next_block;
        best->size = needed;
    }
    set_occupied(best, true);
    return reinterpret_cast<char*>(best) + METADATA_SIZE;
}

void allocator_boundary_tags::do_deallocate_sm(void *at)
{
    if (at == nullptr) return;
    std::lock_guard<std::mutex> lock(*get_mutex(_trusted_memory));
    block_metadata* curr = reinterpret_cast<block_metadata*>(reinterpret_cast<char*>(at) - METADATA_SIZE);
    
    if (curr->allocator_ptr != this) throw std::runtime_error("Block does not belong to this allocator");
    set_occupied(curr, false);

    // Merge next
    if (curr->next && !is_occupied(curr->next)) {
        block_metadata* next = curr->next;
        curr->size = get_size(curr) + get_size(next);
        curr->next = next->next;
        if (next->next) next->next->prev = curr;
    }
    // Merge prev
    if (curr->prev && !is_occupied(curr->prev)) {
        block_metadata* prev = curr->prev;
        prev->size = get_size(prev) + get_size(curr);
        prev->next = curr->next;
        if (curr->next) curr->next->prev = prev;
    }
}

bool allocator_boundary_tags::do_is_equal(const std::pmr::memory_resource &other) const noexcept {
    auto p = dynamic_cast<const allocator_boundary_tags*>(&other);
    return p != nullptr && p->_trusted_memory == _trusted_memory;
}

inline void allocator_boundary_tags::set_fit_mode(allocator_with_fit_mode::fit_mode mode) {
    std::lock_guard<std::mutex> lock(*get_mutex(_trusted_memory));
    *get_fit_mode_ptr(_trusted_memory) = mode;
}

std::vector<allocator_test_utils::block_info> allocator_boundary_tags::get_blocks_info() const {
    std::lock_guard<std::mutex> lock(*get_mutex(_trusted_memory));
    return get_blocks_info_inner();
}

std::vector<allocator_test_utils::block_info> allocator_boundary_tags::get_blocks_info_inner() const {
    std::vector<allocator_test_utils::block_info> res;
    for (auto it = begin(); it != end(); ++it) {
        res.push_back({it.size(), it.occupied()});
    }
    return res;
}


allocator_boundary_tags::boundary_iterator::boundary_iterator() : _occupied_ptr(nullptr), _occupied(false), _trusted_memory(nullptr) {}
allocator_boundary_tags::boundary_iterator::boundary_iterator(void* trusted) : _trusted_memory(trusted) {
    _occupied_ptr = *get_first_block_ptr(_trusted_memory);
    if (_occupied_ptr) _occupied = is_occupied(reinterpret_cast<block_metadata*>(_occupied_ptr));
}
bool allocator_boundary_tags::boundary_iterator::operator==(const boundary_iterator& other) const noexcept {
    return _occupied_ptr == other._occupied_ptr;
}
bool allocator_boundary_tags::boundary_iterator::operator!=(const boundary_iterator& other) const noexcept {
    return !(*this == other);
}
allocator_boundary_tags::boundary_iterator& allocator_boundary_tags::boundary_iterator::operator++() & noexcept {
    if (_occupied_ptr) {
        block_metadata* meta = reinterpret_cast<block_metadata*>(_occupied_ptr);
        _occupied_ptr = meta->next;
        if (_occupied_ptr) _occupied = is_occupied(reinterpret_cast<block_metadata*>(_occupied_ptr));
    }
    return *this;
}
allocator_boundary_tags::boundary_iterator& allocator_boundary_tags::boundary_iterator::operator--() & noexcept {
    if (_occupied_ptr) {
        block_metadata* meta = reinterpret_cast<block_metadata*>(_occupied_ptr);
        _occupied_ptr = meta->prev;
        if (_occupied_ptr) _occupied = is_occupied(reinterpret_cast<block_metadata*>(_occupied_ptr));
    }
    return *this;
}
allocator_boundary_tags::boundary_iterator allocator_boundary_tags::boundary_iterator::operator++(int) {
    boundary_iterator tmp = *this; ++(*this); return tmp;
}
allocator_boundary_tags::boundary_iterator allocator_boundary_tags::boundary_iterator::operator--(int) {
    boundary_iterator tmp = *this; --(*this); return tmp;
}
size_t allocator_boundary_tags::boundary_iterator::size() const noexcept {
    return _occupied_ptr ? get_size(reinterpret_cast<block_metadata*>(_occupied_ptr)) : 0;
}
bool allocator_boundary_tags::boundary_iterator::occupied() const noexcept {
    return _occupied_ptr ? is_occupied(reinterpret_cast<block_metadata*>(_occupied_ptr)) : false;
}
void* allocator_boundary_tags::boundary_iterator::operator*() const noexcept {
    return _occupied_ptr ? reinterpret_cast<char*>(_occupied_ptr) + METADATA_SIZE : nullptr;
}
void* allocator_boundary_tags::boundary_iterator::get_ptr() const noexcept {
    return _occupied_ptr;
}
allocator_boundary_tags::boundary_iterator allocator_boundary_tags::begin() const noexcept {
    return boundary_iterator(_trusted_memory);
}
allocator_boundary_tags::boundary_iterator allocator_boundary_tags::end() const noexcept {
    return boundary_iterator();
}
