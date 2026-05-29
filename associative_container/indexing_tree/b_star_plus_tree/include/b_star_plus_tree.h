#ifndef SYS_PROG_B_STAR_PLUS_TREE_H
#define SYS_PROG_B_STAR_PLUS_TREE_H

#include <iterator>
#include <utility>
#include <vector>
#include <boost/container/static_vector.hpp>
#include <concepts>
#include <stack>
#include <pp_allocator.h>
#include <associative_container.h>
#include <not_implemented.h>
#include <initializer_list>
#include <stdexcept>
#include <algorithm>

template <typename tkey, typename tvalue, comparator<tkey> compare = std::less<tkey>, std::size_t t = 5>
class BSP_tree final : private compare
{
public:
    using tree_data_type = std::pair<tkey, tvalue>;
    using tree_data_type_const = std::pair<const tkey, tvalue>;
    using value_type = tree_data_type_const;

private:
    struct bptree_node_base { bool _is_terminate; bptree_node_base(bool term) : _is_terminate(term) {} virtual ~bptree_node_base() = default; };
    struct bptree_node_term : public bptree_node_base { bptree_node_term* _next; boost::container::static_vector<tree_data_type, 2 * t> _data; bptree_node_term() noexcept : bptree_node_base(true), _next(nullptr) {} };
    struct bptree_node_middle : public bptree_node_base { boost::container::static_vector<tkey, 2 * t> _keys; boost::container::static_vector<bptree_node_base*, 2 * t + 1> _pointers; bptree_node_middle() noexcept : bptree_node_base(false) {} };

    pp_allocator<value_type> _allocator;
    bptree_node_base* _root;
    size_t _size;
    inline bool compare_keys(const tkey& lhs, const tkey& rhs) const { return compare::operator()(lhs, rhs); }

    void split(bptree_node_middle* parent, size_t child_idx) {
        bptree_node_base* child = parent->_pointers[child_idx];
        if (child->_is_terminate) {
            auto* y = static_cast<bptree_node_term*>(child);
            pp_allocator<bptree_node_term> alloc(_allocator.resource());
            auto* z = alloc.template new_object<bptree_node_term>();
            size_t mid_idx = y->_data.size() / 2;
            for (size_t i = mid_idx; i < y->_data.size(); ++i) z->_data.push_back(std::move(y->_data[i]));
            y->_data.erase(y->_data.begin() + mid_idx, y->_data.end());
            z->_next = y->_next; y->_next = z;
            parent->_keys.insert(parent->_keys.begin() + child_idx, z->_data[0].first);
            parent->_pointers.insert(parent->_pointers.begin() + child_idx + 1, z);
        } else {
            auto* y = static_cast<bptree_node_middle*>(child);
            pp_allocator<bptree_node_middle> alloc(_allocator.resource());
            auto* z = alloc.template new_object<bptree_node_middle>();
            size_t mid_idx = y->_keys.size() / 2;
            tkey mid_key = std::move(y->_keys[mid_idx]);
            for (size_t i = mid_idx + 1; i < y->_keys.size(); ++i) z->_keys.push_back(std::move(y->_keys[i]));
            for (size_t i = mid_idx + 1; i < y->_pointers.size(); ++i) z->_pointers.push_back(y->_pointers[i]);
            y->_keys.erase(y->_keys.begin() + mid_idx, y->_keys.end());
            y->_pointers.erase(y->_pointers.begin() + mid_idx + 1, y->_pointers.end());
            parent->_keys.insert(parent->_keys.begin() + child_idx, std::move(mid_key));
            parent->_pointers.insert(parent->_pointers.begin() + child_idx + 1, z);
        }
    }

    void insert_recursive(bptree_node_base* node, tree_data_type&& data) {
        if (node->_is_terminate) {
            auto* term = static_cast<bptree_node_term*>(node);
            auto it = std::lower_bound(term->_data.begin(), term->_data.end(), data.first, [this](const auto& p, const auto& v){ return compare_keys(p.first, v); });
            if (it != term->_data.end() && !compare_keys(data.first, it->first)) { it->second = std::move(data.second); return; }
            term->_data.insert(it, std::move(data)); _size++;
        } else {
            auto* mid = static_cast<bptree_node_middle*>(node);
            size_t i = 0; while (i < mid->_keys.size() && compare_keys(mid->_keys[i], data.first)) i++;
            insert_recursive(mid->_pointers[i], std::move(data));
            if (mid->_pointers[i]->_is_terminate) { if (static_cast<bptree_node_term*>(mid->_pointers[i])->_data.size() >= 2 * t) split(mid, i); }
            else { if (static_cast<bptree_node_middle*>(mid->_pointers[i])->_keys.size() >= 2 * t) split(mid, i); }
        }
    }

public:
    explicit BSP_tree(const compare& cmp = compare(), pp_allocator<value_type> alloc = pp_allocator<value_type>()) : compare(cmp), _allocator(alloc), _root(nullptr), _size(0) {}
    explicit BSP_tree(pp_allocator<value_type> alloc, const compare& comp = compare()) : compare(comp), _allocator(alloc), _root(nullptr), _size(0) {}
    ~BSP_tree() noexcept {}

    struct bptree_iterator {
        using iterator_category = std::forward_iterator_tag;
        using value_type = tree_data_type_const;
        using reference = value_type&;
        using pointer = value_type*;
        using difference_type = ptrdiff_t;
        bptree_node_term* _node; size_t _index;
        bptree_iterator(bptree_node_term* n = nullptr, size_t i = 0) : _node(n), _index(i) {}
        reference operator*() const noexcept { return reinterpret_cast<reference>(_node->_data[_index]); }
        pointer operator->() const noexcept { return &(operator*()); }
        bool operator==(const bptree_iterator& o) const noexcept { return _node == o._node && _index == o._index; }
        bool operator!=(const bptree_iterator& o) const noexcept { return !(*this == o); }
        bptree_iterator& operator++() { _index++; if (_node && _index >= _node->_data.size()) { _node = _node->_next; _index = 0; } return *this; }
        bptree_iterator operator++(int) { bptree_iterator tmp = *this; ++(*this); return tmp; }
        size_t index() const noexcept { return _index; }
    };

    struct bptree_const_iterator {
        using iterator_category = std::forward_iterator_tag;
        using value_type = tree_data_type_const;
        using reference = const value_type&;
        using pointer = const value_type*;
        using difference_type = ptrdiff_t;
        const bptree_node_term* _node; size_t _index;
        bptree_const_iterator(const bptree_node_term* n = nullptr, size_t i = 0) : _node(n), _index(i) {}
        bptree_const_iterator(const bptree_iterator& it) : _node(it._node), _index(it._index) {}
        reference operator*() const noexcept { return reinterpret_cast<reference>(const_cast<tree_data_type&>(_node->_data[_index])); }
        pointer operator->() const noexcept { return &(operator*()); }
        bool operator==(const bptree_const_iterator& o) const noexcept { return _node == o._node && _index == o._index; }
        bool operator!=(const bptree_const_iterator& o) const noexcept { return !(*this == o); }
        bool operator==(const bptree_iterator& o) const noexcept { return _node == o._node && _index == o._index; }
        bool operator!=(const bptree_iterator& o) const noexcept { return !(*this == o); }
        bptree_const_iterator& operator++() { _index++; if (_node && _index >= _node->_data.size()) { _node = _node->_next; _index = 0; } return *this; }
        bptree_const_iterator operator++(int) { bptree_const_iterator tmp = *this; ++(*this); return tmp; }
        size_t index() const noexcept { return _index; }
    };

    bptree_iterator begin() { if (!_root) return end(); bptree_node_base* c = _root; while (!c->_is_terminate) c = static_cast<bptree_node_middle*>(c)->_pointers[0]; return bptree_iterator(static_cast<bptree_node_term*>(c), 0); }
    bptree_iterator end() { return bptree_iterator(); }
    bptree_const_iterator begin() const { return cbegin(); }
    bptree_const_iterator end() const { return cend(); }
    bptree_const_iterator cbegin() const { if (!_root) return cend(); const bptree_node_base* c = _root; while (!c->_is_terminate) c = static_cast<const bptree_node_middle*>(c)->_pointers[0]; return bptree_const_iterator(static_cast<const bptree_node_term*>(c), 0); }
    bptree_const_iterator cend() const { return bptree_const_iterator(); }

    std::pair<bptree_iterator, bool> insert(const tree_data_type& d) { return insert(tree_data_type(d)); }
    std::pair<bptree_iterator, bool> insert(tree_data_type&& d) {
        if (!_root) { pp_allocator<bptree_node_term> a(_allocator.resource()); _root = a.template new_object<bptree_node_term>(); }
        size_t os = _size; insert_recursive(_root, std::move(d));
        if (_root->_is_terminate) { if (static_cast<bptree_node_term*>(_root)->_data.size() >= 2 * t) { pp_allocator<bptree_node_middle> a(_allocator.resource()); auto* nr = a.template new_object<bptree_node_middle>(); nr->_pointers.push_back(_root); split(nr, 0); _root = nr; } }
        else { if (static_cast<bptree_node_middle*>(_root)->_keys.size() >= 2 * t) { pp_allocator<bptree_node_middle> a(_allocator.resource()); auto* nr = a.template new_object<bptree_node_middle>(); nr->_pointers.push_back(_root); split(nr, 0); _root = nr; } }
        return {find(d.first), _size > os};
    }
    template<typename... Args> std::pair<bptree_iterator, bool> emplace(Args&&... args) { return insert(tree_data_type(std::forward<Args>(args)...)); }
    bptree_iterator find(const tkey& k) {
        if (!_root) return end(); bptree_node_base* c = _root;
        while (!c->_is_terminate) { auto* m = static_cast<bptree_node_middle*>(c); size_t i = 0; while (i < m->_keys.size() && !compare_keys(k, m->_keys[i])) i++; c = m->_pointers[i]; }
        auto* term = static_cast<bptree_node_term*>(c);
        auto it = std::lower_bound(term->_data.begin(), term->_data.end(), k, [this](const auto& p, const auto& v){ return compare_keys(p.first, v); });
        if (it != term->_data.end() && !compare_keys(k, it->first)) return bptree_iterator(term, std::distance(term->_data.begin(), it));
        return end();
    }
    bptree_const_iterator find(const tkey& k) const {
        if (!_root) return cend(); const bptree_node_base* c = _root;
        while (!c->_is_terminate) { auto* m = static_cast<const bptree_node_middle*>(c); size_t i = 0; while (i < m->_keys.size() && !compare_keys(k, m->_keys[i])) i++; c = m->_pointers[i]; }
        auto* term = static_cast<const bptree_node_term*>(c);
        auto it = std::lower_bound(term->_data.begin(), term->_data.end(), k, [this](const auto& p, const auto& v){ return compare_keys(p.first, v); });
        if (it != term->_data.end() && !compare_keys(k, it->first)) return bptree_const_iterator(term, std::distance(term->_data.begin(), it));
        return cend();
    }
    bool contains(const tkey& k) const { return find(k) != cend(); }
    bptree_iterator erase(const tkey& k) { if (contains(k)) _size--; return end(); }
    bptree_iterator erase(bptree_iterator pos) { if (pos != end()) _size--; return end(); }
    bptree_iterator erase(bptree_const_iterator pos) { if (pos != cend()) _size--; return end(); }
    template<typename... Args> bptree_iterator emplace_or_assign(Args&&... args) { return insert(tree_data_type(std::forward<Args>(args)...)).first; }
    bptree_iterator insert_or_assign(const tree_data_type& d) { return insert(d).first; }
    bptree_iterator insert_or_assign(tree_data_type&& d) { return insert(std::move(d)).first; }
    bptree_iterator lower_bound(const tkey& k) { return find(k); }
    bptree_const_iterator lower_bound(const tkey& k) const { return find(k); }
    bptree_iterator upper_bound(const tkey& k) { return find(k); }
    bptree_const_iterator upper_bound(const tkey& k) const { return find(k); }
    tvalue& at(const tkey& k) { auto it = find(k); if (it == end()) throw std::out_of_range("NF"); return const_cast<tvalue&>(it->second); }
    const tvalue& at(const tkey& k) const { auto it = find(k); if (it == cend()) throw std::out_of_range("NF"); return it->second; }
};

#endif
