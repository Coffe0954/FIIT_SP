#include <not_implemented.h>
#include "../include/allocator_red_black_tree.h"
#include <new>
#include <algorithm>
#include <iostream>

// Metadata Layout
static constexpr size_t ALLOC_META_SIZE = 68;
static constexpr size_t OCC_META_SIZE = 24;
static constexpr size_t FREE_META_SIZE = 48;

inline std::pmr::memory_resource** get_parent_ptr(void* trusted) { return reinterpret_cast<std::pmr::memory_resource**>(trusted); }
inline allocator_with_fit_mode::fit_mode* get_fit_mode_ptr(void* trusted) { return reinterpret_cast<allocator_with_fit_mode::fit_mode*>(reinterpret_cast<char*>(trusted) + 8); }
inline size_t* get_total_size_ptr(void* trusted) { return reinterpret_cast<size_t*>(reinterpret_cast<char*>(trusted) + 12); }
inline std::mutex* get_mutex(void* trusted) { return reinterpret_cast<std::mutex*>(reinterpret_cast<char*>(trusted) + 20); }
inline void** get_root_ptr(void* trusted) { return reinterpret_cast<void**>(reinterpret_cast<char*>(trusted) + 60); }

enum class rbt_color : unsigned char { RED, BLACK };
struct block_meta {
    size_t size_flags; // LSB: occupied, 2nd LSB: color (0=RED, 1=BLACK)
    block_meta *prev_phys, *next_phys;
    block_meta *left, *right, *parent;
};

inline bool is_occ(block_meta* m) { return (m->size_flags & 1) != 0; }
inline rbt_color get_color(block_meta* m) { return (m->size_flags & 2) ? rbt_color::BLACK : rbt_color::RED; }
inline size_t get_sz(block_meta* m) { return m->size_flags & ~static_cast<size_t>(3); }

inline void set_occ(block_meta* m, bool occ) { if (occ) m->size_flags |= 1; else m->size_flags &= ~static_cast<size_t>(1); }
inline void set_color(block_meta* m, rbt_color c) { if (c == rbt_color::BLACK) m->size_flags |= 2; else m->size_flags &= ~static_cast<size_t>(2); }
inline void set_sz(block_meta* m, size_t sz) { m->size_flags = (m->size_flags & 3) | (sz & ~static_cast<size_t>(3)); }

// RB Tree Helpers
void rotate_left(void* trusted, block_meta* x) {
    block_meta* y = x->right;
    x->right = y->left;
    if (y->left) y->left->parent = x;
    y->parent = x->parent;
    if (!x->parent) *get_root_ptr(trusted) = y;
    else if (x == x->parent->left) x->parent->left = y;
    else x->parent->right = y;
    y->left = x;
    x->parent = y;
}

void rotate_right(void* trusted, block_meta* y) {
    block_meta* x = y->left;
    y->left = x->right;
    if (x->right) x->right->parent = y;
    x->parent = y->parent;
    if (!y->parent) *get_root_ptr(trusted) = x;
    else if (y == y->parent->left) y->parent->left = x;
    else y->parent->right = x;
    x->right = y;
    y->parent = x;
}

void insert_fixup(void* trusted, block_meta* z) {
    while (z->parent && get_color(z->parent) == rbt_color::RED) {
        if (z->parent == z->parent->parent->left) {
            block_meta* y = z->parent->parent->right;
            if (y && get_color(y) == rbt_color::RED) {
                set_color(z->parent, rbt_color::BLACK);
                set_color(y, rbt_color::BLACK);
                set_color(z->parent->parent, rbt_color::RED);
                z = z->parent->parent;
            } else {
                if (z == z->parent->right) {
                    z = z->parent;
                    rotate_left(trusted, z);
                }
                set_color(z->parent, rbt_color::BLACK);
                set_color(z->parent->parent, rbt_color::RED);
                rotate_right(trusted, z->parent->parent);
            }
        } else {
            block_meta* y = z->parent->parent->left;
            if (y && get_color(y) == rbt_color::RED) {
                set_color(z->parent, rbt_color::BLACK);
                set_color(y, rbt_color::BLACK);
                set_color(z->parent->parent, rbt_color::RED);
                z = z->parent->parent;
            } else {
                if (z == z->parent->left) {
                    z = z->parent;
                    rotate_right(trusted, z);
                }
                set_color(z->parent, rbt_color::BLACK);
                set_color(z->parent->parent, rbt_color::RED);
                rotate_left(trusted, z->parent->parent);
            }
        }
    }
    block_meta* root = reinterpret_cast<block_meta*>(*get_root_ptr(trusted));
    if (root) set_color(root, rbt_color::BLACK);
}

void tree_insert(void* trusted, block_meta* z) {
    z->left = z->right = z->parent = nullptr;
    set_color(z, rbt_color::RED);
    block_meta* y = nullptr;
    block_meta* x = reinterpret_cast<block_meta*>(*get_root_ptr(trusted));
    while (x) {
        y = x;
        if (get_sz(z) < get_sz(x)) x = x->left;
        else x = x->right;
    }
    z->parent = y;
    if (!y) *get_root_ptr(trusted) = z;
    else if (get_sz(z) < get_sz(y)) y->left = z;
    else y->right = z;
    insert_fixup(trusted, z);
}

void transplant(void* trusted, block_meta* u, block_meta* v) {
    if (!u->parent) *get_root_ptr(trusted) = v;
    else if (u == u->parent->left) u->parent->left = v;
    else u->parent->right = v;
    if (v) v->parent = u->parent;
}

block_meta* tree_min(block_meta* x) {
    while (x->left) x = x->left;
    return x;
}

void delete_fixup(void* trusted, block_meta* x, block_meta* x_parent) {
    while (x != *get_root_ptr(trusted) && (!x || get_color(x) == rbt_color::BLACK)) {
        if (x == x_parent->left) {
            block_meta* w = x_parent->right;
            if (get_color(w) == rbt_color::RED) {
                set_color(w, rbt_color::BLACK);
                set_color(x_parent, rbt_color::RED);
                rotate_left(trusted, x_parent);
                w = x_parent->right;
            }
            if ((!w->left || get_color(w->left) == rbt_color::BLACK) && (!w->right || get_color(w->right) == rbt_color::BLACK)) {
                set_color(w, rbt_color::RED);
                x = x_parent;
                x_parent = x->parent;
            } else {
                if (!w->right || get_color(w->right) == rbt_color::BLACK) {
                    if (w->left) set_color(w->left, rbt_color::BLACK);
                    set_color(w, rbt_color::RED);
                    rotate_right(trusted, w);
                    w = x_parent->right;
                }
                set_color(w, get_color(x_parent));
                set_color(x_parent, rbt_color::BLACK);
                if (w->right) set_color(w->right, rbt_color::BLACK);
                rotate_left(trusted, x_parent);
                x = reinterpret_cast<block_meta*>(*get_root_ptr(trusted));
                x_parent = nullptr;
            }
        } else {
            block_meta* w = x_parent->left;
            if (get_color(w) == rbt_color::RED) {
                set_color(w, rbt_color::BLACK);
                set_color(x_parent, rbt_color::RED);
                rotate_right(trusted, x_parent);
                w = x_parent->left;
            }
            if ((!w->right || get_color(w->right) == rbt_color::BLACK) && (!w->left || get_color(w->left) == rbt_color::BLACK)) {
                set_color(w, rbt_color::RED);
                x = x_parent;
                x_parent = x->parent;
            } else {
                if (!w->left || get_color(w->left) == rbt_color::BLACK) {
                    if (w->right) set_color(w->right, rbt_color::BLACK);
                    set_color(w, rbt_color::RED);
                    rotate_left(trusted, w);
                    w = x_parent->left;
                }
                set_color(w, get_color(x_parent));
                set_color(x_parent, rbt_color::BLACK);
                if (w->left) set_color(w->left, rbt_color::BLACK);
                rotate_right(trusted, x_parent);
                x = reinterpret_cast<block_meta*>(*get_root_ptr(trusted));
                x_parent = nullptr;
            }
        }
    }
    if (x) set_color(x, rbt_color::BLACK);
}

void tree_delete(void* trusted, block_meta* z) {
    block_meta* y = z;
    rbt_color y_orig_color = get_color(y);
    block_meta* x;
    block_meta* x_parent;
    if (!z->left) {
        x = z->right;
        x_parent = z->parent;
        transplant(trusted, z, z->right);
    } else if (!z->right) {
        x = z->left;
        x_parent = z->parent;
        transplant(trusted, z, z->left);
    } else {
        y = tree_min(z->right);
        y_orig_color = get_color(y);
        x = y->right;
        if (y->parent == z) {
            x_parent = y;
        } else {
            x_parent = y->parent;
            transplant(trusted, y, y->right);
            y->right = z->right;
            y->right->parent = y;
        }
        transplant(trusted, z, y);
        y->left = z->left;
        y->left->parent = y;
        set_color(y, get_color(z));
    }
    if (y_orig_color == rbt_color::BLACK) delete_fixup(trusted, x, x_parent);
}

// Allocator impl
allocator_red_black_tree::allocator_red_black_tree(size_t space_size, std::pmr::memory_resource *parent_allocator, fit_mode allocate_fit_mode)
{
    size_t total = ALLOC_META_SIZE + space_size;
    _trusted_memory = parent_allocator ? parent_allocator->allocate(total) : ::operator new(total);
    *get_parent_ptr(_trusted_memory) = parent_allocator;
    *get_fit_mode_ptr(_trusted_memory) = allocate_fit_mode;
    *get_total_size_ptr(_trusted_memory) = space_size;
    new (get_mutex(_trusted_memory)) std::mutex();
    *get_root_ptr(_trusted_memory) = nullptr;

    if (space_size >= FREE_META_SIZE) {
        block_meta* first = reinterpret_cast<block_meta*>(reinterpret_cast<char*>(_trusted_memory) + ALLOC_META_SIZE);
        first->size_flags = 0;
        set_sz(first, space_size);
        first->prev_phys = first->next_phys = nullptr;
        tree_insert(_trusted_memory, first);
    }
}

allocator_red_black_tree::~allocator_red_black_tree() {
    if (!_trusted_memory) return;
    auto p = *get_parent_ptr(_trusted_memory);
    size_t sz = *get_total_size_ptr(_trusted_memory) + ALLOC_META_SIZE;
    get_mutex(_trusted_memory)->~mutex();
    if (p) p->deallocate(_trusted_memory, sz); else ::operator delete(_trusted_memory);
}

block_meta* find_block(void* trusted, size_t needed, allocator_with_fit_mode::fit_mode mode) {
    block_meta* root = reinterpret_cast<block_meta*>(*get_root_ptr(trusted));
    if (!root) return nullptr;
    block_meta* best = nullptr;
    if (mode == allocator_with_fit_mode::fit_mode::the_best_fit || mode == allocator_with_fit_mode::fit_mode::first_fit) {
        block_meta* curr = root;
        while (curr) {
            if (get_sz(curr) >= needed) { best = curr; curr = curr->left; }
            else curr = curr->right;
        }
    } else { // worst fit
        block_meta* curr = root;
        while (curr) {
            if (get_sz(curr) >= needed) {
                if (!best || get_sz(curr) > get_sz(best)) best = curr;
            }
            curr = curr->right;
        }
    }
    return best;
}

[[nodiscard]] void *allocator_red_black_tree::do_allocate_sm(size_t size) {
    std::lock_guard<std::mutex> lock(*get_mutex(_trusted_memory));
    size_t needed = size + OCC_META_SIZE;
    block_meta* best = find_block(_trusted_memory, needed, *get_fit_mode_ptr(_trusted_memory));
    if (!best) throw std::bad_alloc();
    tree_delete(_trusted_memory, best);
    if (get_sz(best) >= needed + FREE_META_SIZE) {
        size_t old_sz = get_sz(best);
        block_meta* next = reinterpret_cast<block_meta*>(reinterpret_cast<char*>(best) + needed);
        next->size_flags = 0;
        set_sz(next, old_sz - needed);
        next->prev_phys = best;
        next->next_phys = best->next_phys;
        if (best->next_phys) best->next_phys->prev_phys = next;
        best->next_phys = next;
        set_sz(best, needed);
        tree_insert(_trusted_memory, next);
    }
    set_occ(best, true);
    return reinterpret_cast<char*>(best) + OCC_META_SIZE;
}

void allocator_red_black_tree::do_deallocate_sm(void *at) {
    if (!at) return;
    std::lock_guard<std::mutex> lock(*get_mutex(_trusted_memory));
    
    char* managed_start = reinterpret_cast<char*>(_trusted_memory) + ALLOC_META_SIZE;
    char* managed_end = managed_start + *get_total_size_ptr(_trusted_memory);
    if (reinterpret_cast<char*>(at) < managed_start || reinterpret_cast<char*>(at) >= managed_end)
        throw std::runtime_error("Not my block");

    block_meta* curr = reinterpret_cast<block_meta*>(reinterpret_cast<char*>(at) - OCC_META_SIZE);
    set_occ(curr, false);
    if (curr->next_phys && !is_occ(curr->next_phys)) {
        block_meta* next = curr->next_phys;
        tree_delete(_trusted_memory, next);
        set_sz(curr, get_sz(curr) + get_sz(next));
        curr->next_phys = next->next_phys;
        if (next->next_phys) next->next_phys->prev_phys = curr;
    }
    if (curr->prev_phys && !is_occ(curr->prev_phys)) {
        block_meta* prev = curr->prev_phys;
        tree_delete(_trusted_memory, prev);
        set_sz(prev, get_sz(prev) + get_sz(curr));
        prev->next_phys = curr->next_phys;
        if (curr->next_phys) curr->next_phys->prev_phys = prev;
        curr = prev;
    }
    tree_insert(_trusted_memory, curr);
}

allocator_red_black_tree::allocator_red_black_tree(const allocator_red_black_tree &other) { throw std::runtime_error("No copy"); }
allocator_red_black_tree &allocator_red_black_tree::operator=(const allocator_red_black_tree &other) { throw std::runtime_error("No copy"); }
allocator_red_black_tree::allocator_red_black_tree(allocator_red_black_tree &&other) noexcept : _trusted_memory(other._trusted_memory) { other._trusted_memory = nullptr; }
allocator_red_black_tree &allocator_red_black_tree::operator=(allocator_red_black_tree &&other) noexcept { if (this != &other) { this->~allocator_red_black_tree(); _trusted_memory = other._trusted_memory; other._trusted_memory = nullptr; } return *this; }
bool allocator_red_black_tree::do_is_equal(const std::pmr::memory_resource& other) const noexcept { auto p = dynamic_cast<const allocator_red_black_tree*>(&other); return p && p->_trusted_memory == _trusted_memory; }
void allocator_red_black_tree::set_fit_mode(fit_mode mode) { std::lock_guard<std::mutex> lock(*get_mutex(_trusted_memory)); *get_fit_mode_ptr(_trusted_memory) = mode; }
std::vector<allocator_test_utils::block_info> allocator_red_black_tree::get_blocks_info() const { std::lock_guard<std::mutex> lock(*get_mutex(_trusted_memory)); return get_blocks_info_inner(); }
std::vector<allocator_test_utils::block_info> allocator_red_black_tree::get_blocks_info_inner() const {
    std::vector<allocator_test_utils::block_info> res;
    char* curr = reinterpret_cast<char*>(_trusted_memory) + ALLOC_META_SIZE;
    char* end = curr + *get_total_size_ptr(_trusted_memory);
    while (curr < end) {
        block_meta* m = reinterpret_cast<block_meta*>(curr);
        res.push_back({get_sz(m), is_occ(m)});
        curr += get_sz(m);
    }
    return res;
}
allocator_red_black_tree::rb_iterator::rb_iterator() : _block_ptr(nullptr), _trusted(nullptr) {}
allocator_red_black_tree::rb_iterator::rb_iterator(void* trusted) : _trusted(trusted) { if (_trusted) _block_ptr = reinterpret_cast<char*>(_trusted) + ALLOC_META_SIZE; }
bool allocator_red_black_tree::rb_iterator::operator==(const rb_iterator& other) const noexcept { return _block_ptr == other._block_ptr; }
bool allocator_red_black_tree::rb_iterator::operator!=(const rb_iterator& other) const noexcept { return !(*this == other); }
allocator_red_black_tree::rb_iterator& allocator_red_black_tree::rb_iterator::operator++() & noexcept {
    if (_block_ptr) {
        size_t sz = get_sz(reinterpret_cast<block_meta*>(_block_ptr));
        _block_ptr = reinterpret_cast<char*>(_block_ptr) + sz;
        char* end = reinterpret_cast<char*>(_trusted) + ALLOC_META_SIZE + *get_total_size_ptr(_trusted);
        if (_block_ptr >= end) _block_ptr = nullptr;
    }
    return *this;
}
allocator_red_black_tree::rb_iterator allocator_red_black_tree::rb_iterator::operator++(int) { rb_iterator t = *this; ++(*this); return t; }
size_t allocator_red_black_tree::rb_iterator::size() const noexcept { return _block_ptr ? get_sz(reinterpret_cast<block_meta*>(_block_ptr)) : 0; }
void* allocator_red_black_tree::rb_iterator::operator*() const noexcept { return _block_ptr ? reinterpret_cast<char*>(_block_ptr) + OCC_META_SIZE : nullptr; }
bool allocator_red_black_tree::rb_iterator::occupied() const noexcept { return _block_ptr ? is_occ(reinterpret_cast<block_meta*>(_block_ptr)) : false; }
allocator_red_black_tree::rb_iterator allocator_red_black_tree::begin() const noexcept { return rb_iterator(_trusted_memory); }
allocator_red_black_tree::rb_iterator allocator_red_black_tree::end() const noexcept { return rb_iterator(); }
