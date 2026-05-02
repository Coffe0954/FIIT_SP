#ifndef SYS_PROG_B_TREE_H
#define SYS_PROG_B_TREE_H

#include <iterator>
#include <utility>
#include <boost/container/static_vector.hpp>
#include <stack>
#include <vector>
#include <pp_allocator.h>
#include <associative_container.h>
#include <not_implemented.h>
#include <initializer_list>
#include <stdexcept>
#include <algorithm>

template <typename tkey, typename tvalue, comparator<tkey> compare = std::less<tkey>, std::size_t t = 5>
class B_tree final : private compare
{
public:
    using tree_data_type = std::pair<tkey, tvalue>;
    using tree_data_type_const = std::pair<const tkey, tvalue>;
    using value_type = tree_data_type_const;

private:
    struct btree_node {
        boost::container::static_vector<tree_data_type, 2 * t> _keys;
        boost::container::static_vector<btree_node*, 2 * t + 1> _pointers;
        bool _leaf;
        btree_node(bool leaf) noexcept : _leaf(leaf) {}
    };

    pp_allocator<value_type> _allocator;
    btree_node* _root;
    size_t _size;

    inline bool compare_keys(const tkey& lhs, const tkey& rhs) const { return compare::operator()(lhs, rhs); }

    void destroy_node(btree_node* node) {
        if (!node) return;
        if (!node->_leaf) for (auto* ptr : node->_pointers) destroy_node(ptr);
        pp_allocator<btree_node> node_alloc(_allocator.resource());
        node_alloc.delete_object(node);
    }

    btree_node* copy_node(btree_node* node) {
        if (!node) return nullptr;
        pp_allocator<btree_node> node_alloc(_allocator.resource());
        btree_node* res = node_alloc.template new_object<btree_node>(node->_leaf);
        res->_keys = node->_keys;
        if (!node->_leaf) for (auto* ptr : node->_pointers) res->_pointers.push_back(copy_node(ptr));
        return res;
    }

    void split(btree_node* parent, size_t child_idx) {
        btree_node* y = parent->_pointers[child_idx];
        pp_allocator<btree_node> node_alloc(_allocator.resource());
        btree_node* z = node_alloc.template new_object<btree_node>(y->_leaf);
        size_t mid_idx = t;
        tree_data_type mid_key = std::move(y->_keys[mid_idx]);
        for (size_t j = mid_idx + 1; j < y->_keys.size(); ++j) z->_keys.push_back(std::move(y->_keys[j]));
        if (!y->_leaf) {
            for (size_t j = mid_idx + 1; j < y->_pointers.size(); ++j) z->_pointers.push_back(y->_pointers[j]);
            y->_pointers.erase(y->_pointers.begin() + (mid_idx + 1), y->_pointers.end());
        }
        y->_keys.erase(y->_keys.begin() + mid_idx, y->_keys.end());
        parent->_keys.insert(parent->_keys.begin() + child_idx, std::move(mid_key));
        parent->_pointers.insert(parent->_pointers.begin() + child_idx + 1, z);
    }

    void insert_recursive(btree_node* node, tree_data_type&& data) {
        size_t i = 0;
        while (i < node->_keys.size() && compare_keys(node->_keys[i].first, data.first)) i++;
        if (i < node->_keys.size() && !compare_keys(data.first, node->_keys[i].first)) {
            node->_keys[i].second = std::move(data.second);
            return;
        }
        if (node->_leaf) {
            node->_keys.insert(node->_keys.begin() + i, std::move(data));
            _size++;
        } else {
            insert_recursive(node->_pointers[i], std::move(data));
            if (node->_pointers[i]->_keys.size() >= 2 * t) split(node, i);
        }
    }

    void remove_recursive(btree_node* node, const tkey& key) {
        size_t i = 0;
        while (i < node->_keys.size() && compare_keys(node->_keys[i].first, key)) i++;
        if (i < node->_keys.size() && !compare_keys(key, node->_keys[i].first)) {
            if (node->_leaf) { node->_keys.erase(node->_keys.begin() + i); _size--; }
            else {
                btree_node* pred = node->_pointers[i];
                while (!pred->_leaf) pred = pred->_pointers.back();
                node->_keys[i] = pred->_keys.back();
                remove_recursive(node->_pointers[i], node->_keys[i].first);
            }
        } else if (!node->_leaf) {
            remove_recursive(node->_pointers[i], key);
        }
    }

public:
    explicit B_tree(const compare& cmp = compare(), pp_allocator<value_type> alloc = pp_allocator<value_type>()) : compare(cmp), _allocator(alloc), _root(nullptr), _size(0) {}
    explicit B_tree(pp_allocator<value_type> alloc, const compare& comp = compare()) : compare(comp), _allocator(alloc), _root(nullptr), _size(0) {}
    B_tree(const B_tree& other) : compare(other), _allocator(other._allocator), _root(copy_node(other._root)), _size(other._size) {}
    ~B_tree() noexcept { clear(); }
    B_tree(B_tree&& other) noexcept : compare(std::move(other)), _allocator(std::move(other._allocator)), _root(other._root), _size(other._size) { other._root = nullptr; other._size = 0; }
    B_tree& operator=(const B_tree& other) { if (this != &other) { clear(); compare::operator=(other); _allocator = other._allocator; _root = copy_node(other._root); _size = other._size; } return *this; }
    B_tree& operator=(B_tree&& other) noexcept { if (this != &other) { clear(); compare::operator=(std::move(other)); _allocator = std::move(other._allocator); _root = other._root; _size = other._size; other._root = nullptr; other._size = 0; } return *this; }

    struct btree_iterator {
        using iterator_category = std::bidirectional_iterator_tag;
        using value_type = tree_data_type_const;
        using reference = value_type&;
        using pointer = value_type*;
        using difference_type = ptrdiff_t;
        std::vector<std::pair<btree_node*, size_t>> _path;
        btree_iterator() = default;
        btree_iterator(std::vector<std::pair<btree_node*, size_t>> p) : _path(std::move(p)) {}
        reference operator*() const noexcept { return reinterpret_cast<reference>(_path.back().first->_keys[_path.back().second]); }
        pointer operator->() const noexcept { return &(operator*()); }
        bool operator==(const btree_iterator& other) const noexcept { return _path == other._path; }
        bool operator!=(const btree_iterator& other) const noexcept { return !(*this == other); }
        btree_iterator& operator++() {
            auto& top = _path.back();
            if (!top.first->_leaf) {
                top.second++;
                btree_node* curr = top.first->_pointers[top.second];
                while (true) { _path.push_back({curr, 0}); if (curr->_leaf) break; curr = curr->_pointers[0]; }
            } else {
                top.second++;
                while (!_path.empty() && _path.back().second >= _path.back().first->_keys.size()) {
                    _path.pop_back();
                    if (!_path.empty() && _path.back().second < _path.back().first->_keys.size()) break;
                }
            }
            return *this;
        }
        size_t depth() const noexcept { return _path.empty() ? 0 : _path.size() - 1; }
        size_t index() const noexcept { return _path.empty() ? 0 : _path.back().second; }
    };

    struct btree_const_iterator {
        using iterator_category = std::bidirectional_iterator_tag;
        using value_type = tree_data_type_const;
        using reference = const value_type&;
        using pointer = const value_type*;
        using difference_type = ptrdiff_t;
        std::vector<std::pair<const btree_node*, size_t>> _path;
        btree_const_iterator() = default;
        btree_const_iterator(std::vector<std::pair<const btree_node*, size_t>> p) : _path(std::move(p)) {}
        btree_const_iterator(const btree_iterator& it) { for (auto& p : it._path) _path.push_back({p.first, p.second}); }
        reference operator*() const noexcept { return reinterpret_cast<reference>(const_cast<tree_data_type&>(_path.back().first->_keys[_path.back().second])); }
        pointer operator->() const noexcept { return &(operator*()); }
        bool operator==(const btree_const_iterator& other) const noexcept { return _path == other._path; }
        bool operator!=(const btree_const_iterator& other) const noexcept { return !(*this == other); }
        bool operator==(const btree_iterator& other) const noexcept {
            if (_path.size() != other._path.size()) return false;
            for (size_t i = 0; i < _path.size(); ++i) if (_path[i].first != other._path[i].first || _path[i].second != other._path[i].second) return false;
            return true;
        }
        bool operator!=(const btree_iterator& other) const noexcept { return !(*this == other); }
        btree_const_iterator& operator++() {
            auto& top = _path.back();
            if (!top.first->_leaf) {
                top.second++;
                const btree_node* curr = top.first->_pointers[top.second];
                while (true) { _path.push_back({curr, 0}); if (curr->_leaf) break; curr = curr->_pointers[0]; }
            } else {
                top.second++;
                while (!_path.empty() && _path.back().second >= _path.back().first->_keys.size()) {
                    _path.pop_back();
                    if (!_path.empty() && _path.back().second < _path.back().first->_keys.size()) break;
                }
            }
            return *this;
        }
        size_t depth() const noexcept { return _path.empty() ? 0 : _path.size() - 1; }
        size_t index() const noexcept { return _path.empty() ? 0 : _path.back().second; }
    };

    btree_iterator begin() {
        if (!_root || _root->_keys.empty()) return end();
        std::vector<std::pair<btree_node*, size_t>> p; btree_node* curr = _root;
        while (true) { p.push_back({curr, 0}); if (curr->_leaf) break; curr = curr->_pointers[0]; }
        return btree_iterator(std::move(p));
    }
    btree_iterator end() { return btree_iterator(); }
    btree_const_iterator cbegin() const {
        if (!_root || _root->_keys.empty()) return cend();
        std::vector<std::pair<const btree_node*, size_t>> p; const btree_node* curr = _root;
        while (true) { p.push_back({curr, 0}); if (curr->_leaf) break; curr = curr->_pointers[0]; }
        return btree_const_iterator(std::move(p));
    }
    btree_const_iterator cend() const { return btree_const_iterator(); }
    btree_const_iterator begin() const { return cbegin(); }
    btree_const_iterator end() const { return cend(); }

    void clear() noexcept { destroy_node(_root); _root = nullptr; _size = 0; }
    size_t size() const noexcept { return _size; }
    bool empty() const noexcept { return _size == 0; }

    std::pair<btree_iterator, bool> insert(const tree_data_type& data) {
        if (!_root) { pp_allocator<btree_node> node_alloc(_allocator.resource()); _root = node_alloc.template new_object<btree_node>(true); }
        size_t old_sz = _size; insert_recursive(_root, tree_data_type(data));
        if (_root->_keys.size() >= 2 * t) {
            pp_allocator<btree_node> node_alloc(_allocator.resource());
            btree_node* new_root = node_alloc.template new_object<btree_node>(false);
            new_root->_pointers.push_back(_root); split(new_root, 0); _root = new_root;
        }
        return {find(data.first), _size > old_sz};
    }
    template<typename... Args> std::pair<btree_iterator, bool> emplace(Args&&... args) { return insert(tree_data_type(std::forward<Args>(args)...)); }

    btree_iterator find(const tkey& key) {
        btree_node* curr = _root; std::vector<std::pair<btree_node*, size_t>> p;
        while (curr) {
            size_t i = 0; while (i < curr->_keys.size() && compare_keys(curr->_keys[i].first, key)) i++;
            if (i < curr->_keys.size() && !compare_keys(key, curr->_keys[i].first)) { p.push_back({curr, i}); return btree_iterator(std::move(p)); }
            if (curr->_leaf) break; p.push_back({curr, i}); curr = curr->_pointers[i];
        }
        return end();
    }
    btree_const_iterator find(const tkey& key) const {
        const btree_node* curr = _root; std::vector<std::pair<const btree_node*, size_t>> p;
        while (curr) {
            size_t i = 0; while (i < curr->_keys.size() && compare_keys(curr->_keys[i].first, key)) i++;
            if (i < curr->_keys.size() && !compare_keys(key, curr->_keys[i].first)) { p.push_back({curr, i}); return btree_const_iterator(std::move(p)); }
            if (curr->_leaf) break; p.push_back({curr, i}); curr = curr->_pointers[i];
        }
        return cend();
    }
    bool contains(const tkey& key) const { return find(key) != cend(); }
    tvalue& at(const tkey& key) { auto it = find(key); if (it == end()) throw std::out_of_range("Not found"); return const_cast<tvalue&>(it->second); }
    const tvalue& at(const tkey& key) const { auto it = find(key); if (it == cend()) throw std::out_of_range("Not found"); return it->second; }
    btree_iterator erase(const tkey& key) { if (contains(key)) remove_recursive(_root, key); return end(); }

    btree_iterator lower_bound(const tkey& key) {
        btree_node* curr = _root; std::vector<std::pair<btree_node*, size_t>> p, best;
        while (curr) {
            size_t i = 0; while (i < curr->_keys.size() && compare_keys(curr->_keys[i].first, key)) i++;
            if (i < curr->_keys.size()) { best = p; best.push_back({curr, i}); if (!compare_keys(key, curr->_keys[i].first)) break; }
            if (curr->_leaf) break; p.push_back({curr, i}); curr = curr->_pointers[i];
        }
        return btree_iterator(std::move(best));
    }
    btree_const_iterator lower_bound(const tkey& key) const {
        const btree_node* curr = _root; std::vector<std::pair<const btree_node*, size_t>> p, best;
        while (curr) {
            size_t i = 0; while (i < curr->_keys.size() && compare_keys(curr->_keys[i].first, key)) i++;
            if (i < curr->_keys.size()) { best = p; best.push_back({curr, i}); if (!compare_keys(key, curr->_keys[i].first)) break; }
            if (curr->_leaf) break; p.push_back({curr, i}); curr = curr->_pointers[i];
        }
        return btree_const_iterator(std::move(best));
    }
    btree_iterator upper_bound(const tkey& key) { return lower_bound(key); }
    btree_const_iterator upper_bound(const tkey& key) const { return lower_bound(key); }
};

#endif
