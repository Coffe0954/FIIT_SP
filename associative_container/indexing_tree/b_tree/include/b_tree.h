#ifndef SYS_PROG_B_TREE_H
#define SYS_PROG_B_TREE_H

#include <algorithm>
#include <cassert>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include <associative_container.h>
#include <pp_allocator.h>

template <typename key_type, typename mapped_type, comparator<key_type> key_compare = std::less<key_type>, std::size_t degree = 5>
class B_tree final : private key_compare
{
    static_assert(degree >= 2, "B_tree minimum degree must be at least 2");

public:
    using tree_data_type = std::pair<key_type, mapped_type>;
    using tree_data_type_const = std::pair<const key_type, mapped_type>;
    using value_type = tree_data_type_const;

    using allocator_type = pp_allocator<value_type>;
    using alloc_traits = std::allocator_traits<allocator_type>;

    using propagate_on_copy = typename alloc_traits::propagate_on_container_copy_assignment;
    using propagate_on_move = typename alloc_traits::propagate_on_container_move_assignment;
    using propagate_on_swap = typename alloc_traits::propagate_on_container_swap;
    using is_always_equal = typename alloc_traits::is_always_equal;

private:
    static constexpr std::size_t min_keys_per_node = degree - 1;
    static constexpr std::size_t max_keys_per_node = 2 * degree - 1;

    struct tree_node
    {
        std::vector<tree_data_type> entries;
        std::vector<tree_node*> children;

        tree_node()
        {
            entries.reserve(max_keys_per_node + 1);
            children.reserve(max_keys_per_node + 2);
        }

        [[nodiscard]] bool is_leaf() const noexcept
        {
            return children.empty();
        }
    };

    using navigation_entry = std::pair<tree_node*, std::size_t>;
    using navigation_path = std::vector<navigation_entry>;

    allocator_type _allocator;
    tree_node* _root_node;
    std::size_t _total_elements;

    [[nodiscard]] bool key_less(const key_type& lhs, const key_type& rhs) const
    {
        return key_compare::operator()(lhs, rhs);
    }

    [[nodiscard]] bool key_equal(const key_type& lhs, const key_type& rhs) const
    {
        return !key_less(lhs, rhs) && !key_less(rhs, lhs);
    }

    [[nodiscard]] allocator_type get_allocator() const noexcept
    {
        return _allocator;
    }

    [[nodiscard]] tree_node* allocate_node()
    {
        return _allocator.template new_object<tree_node>();
    }

    void deallocate_node(tree_node* node) noexcept
    {
        if (node != nullptr) {
            _allocator.template delete_object<tree_node>(node);
        }
    }

    void destroy_subtree(tree_node* node) noexcept
    {
        if (node == nullptr) {
            return;
        }

        for (tree_node* child : node->children) {
            destroy_subtree(child);
        }

        deallocate_node(node);
    }

    [[nodiscard]] std::size_t find_insert_position(const tree_node* node, const key_type& key) const
    {
        std::size_t left = 0;
        std::size_t right = node->entries.size();
        while (left < right) {
            const std::size_t mid = left + (right - left) / 2;
            if (key_less(node->entries[mid].first, key)) {
                left = mid + 1;
            } else {
                right = mid;
            }
        }
        return left;
    }

    [[nodiscard]] std::size_t find_upper_bound_position(const tree_node* node, const key_type& key) const
    {
        std::size_t pos = find_insert_position(node, key);
        while (pos < node->entries.size() && key_equal(node->entries[pos].first, key)) {
            ++pos;
        }
        return pos;
    }

    [[nodiscard]] std::pair<tree_node*, std::size_t> locate_key(const key_type& key) const
    {
        tree_node* current = _root_node;
        while (current != nullptr) {
            const std::size_t pos = find_insert_position(current, key);
            if (pos < current->entries.size() && key_equal(current->entries[pos].first, key)) {
                return {current, pos};
            }

            if (current->is_leaf()) {
                break;
            }

            current = current->children[pos];
        }

        return {nullptr, 0};
    }

    template <typename pair_type>
    std::pair<tree_node*, std::size_t> insert_new_entry(pair_type&& data)
    {
        const key_type target_key = data.first;

        if (_root_node == nullptr) {
            _root_node = allocate_node();
            _root_node->entries.emplace_back(std::forward<pair_type>(data));
            _total_elements = 1;
            return {_root_node, 0};
        }

        navigation_path path;
        tree_node* current = _root_node;

        while (!current->is_leaf()) {
            const std::size_t child_idx = find_insert_position(current, data.first);
            path.emplace_back(current, child_idx);
            current = current->children[child_idx];
        }

        const std::size_t insert_pos = find_insert_position(current, data.first);
        current->entries.insert(current->entries.begin() + static_cast<std::ptrdiff_t>(insert_pos), std::forward<pair_type>(data));
        ++_total_elements;

        while (current->entries.size() > max_keys_per_node) {
            const std::size_t mid_idx = current->entries.size() / 2;
            tree_data_type mid_entry = std::move(current->entries[mid_idx]);

            tree_node* right_sibling = allocate_node();
            right_sibling->entries.assign(
                std::make_move_iterator(current->entries.begin() + static_cast<std::ptrdiff_t>(mid_idx + 1)),
                std::make_move_iterator(current->entries.end()));
            current->entries.resize(mid_idx);

            if (!current->is_leaf()) {
                right_sibling->children.assign(
                    std::make_move_iterator(current->children.begin() + static_cast<std::ptrdiff_t>(mid_idx + 1)),
                    std::make_move_iterator(current->children.end()));
                current->children.resize(mid_idx + 1);
            }

            if (path.empty()) {
                tree_node* new_root = allocate_node();
                new_root->entries.push_back(std::move(mid_entry));
                new_root->children.push_back(current);
                new_root->children.push_back(right_sibling);
                _root_node = new_root;
                break;
            }

            auto [parent, parent_idx] = path.back();
            path.pop_back();

            parent->entries.insert(parent->entries.begin() + static_cast<std::ptrdiff_t>(parent_idx), std::move(mid_entry));
            parent->children.insert(parent->children.begin() + static_cast<std::ptrdiff_t>(parent_idx + 1), right_sibling);
            current = parent;
        }

        return locate_key(target_key);
    }

    [[nodiscard]] tree_data_type& get_rightmost_entry(tree_node* node) const
    {
        assert(node != nullptr);
        while (!node->is_leaf()) {
            node = node->children.back();
        }
        return node->entries.back();
    }

    [[nodiscard]] tree_data_type& get_leftmost_entry(tree_node* node) const
    {
        assert(node != nullptr);
        while (!node->is_leaf()) {
            node = node->children.front();
        }
        return node->entries.front();
    }

    void rotate_from_left(tree_node* parent, std::size_t child_idx)
    {
        tree_node* target = parent->children[child_idx];
        tree_node* left_sibling = parent->children[child_idx - 1];

        target->entries.insert(target->entries.begin(), std::move(parent->entries[child_idx - 1]));
        parent->entries[child_idx - 1] = std::move(left_sibling->entries.back());
        left_sibling->entries.pop_back();

        if (!left_sibling->is_leaf()) {
            target->children.insert(target->children.begin(), left_sibling->children.back());
            left_sibling->children.pop_back();
        }
    }

    void rotate_from_right(tree_node* parent, std::size_t child_idx)
    {
        tree_node* target = parent->children[child_idx];
        tree_node* right_sibling = parent->children[child_idx + 1];

        target->entries.push_back(std::move(parent->entries[child_idx]));
        parent->entries[child_idx] = std::move(right_sibling->entries.front());
        right_sibling->entries.erase(right_sibling->entries.begin());

        if (!right_sibling->is_leaf()) {
            target->children.push_back(right_sibling->children.front());
            right_sibling->children.erase(right_sibling->children.begin());
        }
    }

    void merge_siblings(tree_node* parent, std::size_t key_idx)
    {
        tree_node* left = parent->children[key_idx];
        tree_node* right = parent->children[key_idx + 1];

        left->entries.push_back(std::move(parent->entries[key_idx]));
        left->entries.insert(
            left->entries.end(),
            std::make_move_iterator(right->entries.begin()),
            std::make_move_iterator(right->entries.end()));

        if (!right->is_leaf()) {
            left->children.insert(
                left->children.end(),
                std::make_move_iterator(right->children.begin()),
                std::make_move_iterator(right->children.end()));
        }

        parent->entries.erase(parent->entries.begin() + static_cast<std::ptrdiff_t>(key_idx));
        parent->children.erase(parent->children.begin() + static_cast<std::ptrdiff_t>(key_idx + 1));
        deallocate_node(right);
    }

    bool erase_from_subtree(tree_node* node, const key_type& key)
    {
        const std::size_t pos = find_insert_position(node, key);
        const bool key_present = pos < node->entries.size() && key_equal(node->entries[pos].first, key);

        if (key_present) {
            if (node->is_leaf()) {
                node->entries.erase(node->entries.begin() + static_cast<std::ptrdiff_t>(pos));
                return true;
            }

            tree_node* left_child = node->children[pos];
            tree_node* right_child = node->children[pos + 1];

            if (left_child->entries.size() >= degree) {
                tree_data_type predecessor = get_rightmost_entry(left_child);
                node->entries[pos] = predecessor;
                return erase_from_subtree(left_child, predecessor.first);
            }

            if (right_child->entries.size() >= degree) {
                tree_data_type successor = get_leftmost_entry(right_child);
                node->entries[pos] = successor;
                return erase_from_subtree(right_child, successor.first);
            }

            merge_siblings(node, pos);
            return erase_from_subtree(node->children[pos], key);
        }

        if (node->is_leaf()) {
            return false;
        }

        std::size_t child_idx = pos;
        tree_node* child = node->children[child_idx];

        if (child->entries.size() == min_keys_per_node) {
            if (child_idx > 0 && node->children[child_idx - 1]->entries.size() >= degree) {
                rotate_from_left(node, child_idx);
            } else if (child_idx < node->entries.size() && node->children[child_idx + 1]->entries.size() >= degree) {
                rotate_from_right(node, child_idx);
            } else {
                if (child_idx < node->entries.size()) {
                    merge_siblings(node, child_idx);
                } else {
                    merge_siblings(node, child_idx - 1);
                    --child_idx;
                }
            }
            child = node->children[child_idx];
        }

        return erase_from_subtree(child, key);
    }

    template <bool IsConst>
    class basic_iterator;

public:
    using iterator = basic_iterator<false>;
    using const_iterator = basic_iterator<true>;

    class reverse_iterator final
    {
        iterator _base;

        friend class B_tree;
        friend class const_reverse_iterator;

        explicit reverse_iterator(const iterator& base) noexcept : _base(base) {}

        [[nodiscard]] iterator previous() const
        {
            iterator tmp = _base;
            --tmp;
            return tmp;
        }

    public:
        using value_type = tree_data_type;
        using reference = value_type&;
        using pointer = value_type*;
        using iterator_category = std::bidirectional_iterator_tag;
        using difference_type = std::ptrdiff_t;
        using self = reverse_iterator;

        reverse_iterator() noexcept = default;

        operator iterator() const noexcept
        {
            return _base;
        }

        reference operator*() const noexcept
        {
            return *previous();
        }

        pointer operator->() const noexcept
        {
            return std::addressof(operator*());
        }

        self& operator++()
        {
            --_base;
            return *this;
        }

        self operator++(int)
        {
            self tmp = *this;
            ++(*this);
            return tmp;
        }

        self& operator--()
        {
            ++_base;
            return *this;
        }

        self operator--(int)
        {
            self tmp = *this;
            --(*this);
            return tmp;
        }

        [[nodiscard]] bool operator==(const self& other) const noexcept
        {
            return _base == other._base;
        }

        [[nodiscard]] bool operator!=(const self& other) const noexcept
        {
            return !(*this == other);
        }

        [[nodiscard]] std::size_t depth() const noexcept
        {
            return previous().depth();
        }

        [[nodiscard]] std::size_t current_node_keys_count() const noexcept
        {
            return previous().current_node_keys_count();
        }

        [[nodiscard]] bool is_terminate_node() const noexcept
        {
            return previous().is_terminate_node();
        }

        [[nodiscard]] std::size_t index() const noexcept
        {
            return previous().index();
        }
    };

    class const_reverse_iterator final
    {
        const_iterator _base;

        friend class B_tree;

        explicit const_reverse_iterator(const const_iterator& base) noexcept : _base(base) {}

        [[nodiscard]] const_iterator previous() const
        {
            const_iterator tmp = _base;
            --tmp;
            return tmp;
        }

    public:
        using value_type = tree_data_type;
        using reference = const value_type&;
        using pointer = const value_type*;
        using iterator_category = std::bidirectional_iterator_tag;
        using difference_type = std::ptrdiff_t;
        using self = const_reverse_iterator;

        const_reverse_iterator() noexcept = default;

        const_reverse_iterator(const reverse_iterator& other) noexcept
            : _base(static_cast<iterator>(other))
        {
        }

        operator const_iterator() const noexcept
        {
            return _base;
        }

        reference operator*() const noexcept
        {
            return *previous();
        }

        pointer operator->() const noexcept
        {
            return std::addressof(operator*());
        }

        self& operator++()
        {
            --_base;
            return *this;
        }

        self operator++(int)
        {
            self tmp = *this;
            ++(*this);
            return tmp;
        }

        self& operator--()
        {
            ++_base;
            return *this;
        }

        self operator--(int)
        {
            self tmp = *this;
            --(*this);
            return tmp;
        }

        [[nodiscard]] bool operator==(const self& other) const noexcept
        {
            return _base == other._base;
        }

        [[nodiscard]] bool operator!=(const self& other) const noexcept
        {
            return !(*this == other);
        }

        [[nodiscard]] std::size_t depth() const noexcept
        {
            return previous().depth();
        }

        [[nodiscard]] std::size_t current_node_keys_count() const noexcept
        {
            return previous().current_node_keys_count();
        }

        [[nodiscard]] bool is_terminate_node() const noexcept
        {
            return previous().is_terminate_node();
        }

        [[nodiscard]] std::size_t index() const noexcept
        {
            return previous().index();
        }
    };

private:
    template <bool IsConst>
    class basic_iterator final
    {
        using node_ptr = std::conditional_t<IsConst, const tree_node*, tree_node*>;
        using entry_ref = std::conditional_t<IsConst, const tree_data_type&, tree_data_type&>;
        using entry_ptr = std::conditional_t<IsConst, const tree_data_type*, tree_data_type*>;

        const B_tree* _owner;
        node_ptr _current_node;
        std::size_t _current_idx;
        navigation_path _traversal_path;

        friend class B_tree;
        friend class basic_iterator<!IsConst>;

        explicit basic_iterator(
            const B_tree* owner,
            node_ptr node,
            std::size_t idx,
            navigation_path path = {}) noexcept
            : _owner(owner), _current_node(node), _current_idx(idx), _traversal_path(std::move(path))
        {
        }

    public:
        using value_type = tree_data_type;
        using reference = entry_ref;
        using pointer = entry_ptr;
        using iterator_category = std::bidirectional_iterator_tag;
        using difference_type = std::ptrdiff_t;
        using self = basic_iterator<IsConst>;

        basic_iterator() noexcept : _owner(nullptr), _current_node(nullptr), _current_idx(0), _traversal_path() {}

        template <bool OtherIsConst, typename = std::enable_if_t<IsConst || !OtherIsConst>>
        basic_iterator(const basic_iterator<OtherIsConst>& other) noexcept
            : _owner(other._owner), _current_node(other._current_node), _current_idx(other._current_idx), _traversal_path(other._traversal_path)
        {
        }

        reference operator*() const noexcept
        {
            assert(_current_node != nullptr);
            return const_cast<tree_node*>(_current_node)->entries[_current_idx];
        }

        pointer operator->() const noexcept
        {
            return std::addressof(operator*());
        }

        self& operator++()
        {
            if (_current_node == nullptr) {
                return *this;
            }

            node_ptr current = _current_node;
            if (!current->is_leaf()) {
                _traversal_path.emplace_back(const_cast<tree_node*>(current), _current_idx + 1);
                current = current->children[_current_idx + 1];
                while (!current->is_leaf()) {
                    _traversal_path.emplace_back(const_cast<tree_node*>(current), 0);
                    current = current->children[0];
                }
                _current_node = current;
                _current_idx = 0;
                return *this;
            }

            if (_current_idx + 1 < current->entries.size()) {
                ++_current_idx;
                return *this;
            }

            while (!_traversal_path.empty()) {
                const auto [parent, child_idx] = _traversal_path.back();
                _traversal_path.pop_back();
                if (child_idx < parent->entries.size()) {
                    _current_node = parent;
                    _current_idx = child_idx;
                    return *this;
                }
            }

            _current_node = nullptr;
            _current_idx = 0;
            return *this;
        }

        self operator++(int)
        {
            self tmp = *this;
            ++(*this);
            return tmp;
        }

        self& operator--()
        {
            if (_owner == nullptr) {
                return *this;
            }

            if (_current_node == nullptr) {
                node_ptr current = _owner->_root_node;
                _traversal_path.clear();
                if (current == nullptr) {
                    return *this;
                }

                while (!current->is_leaf()) {
                    const std::size_t last_idx = current->children.size() - 1;
                    _traversal_path.emplace_back(const_cast<tree_node*>(current), last_idx);
                    current = current->children[last_idx];
                }

                _current_node = current;
                _current_idx = current->entries.size() - 1;
                return *this;
            }

            node_ptr current = _current_node;
            if (!current->is_leaf()) {
                _traversal_path.emplace_back(const_cast<tree_node*>(current), _current_idx);
                current = current->children[_current_idx];
                while (!current->is_leaf()) {
                    const std::size_t last_idx = current->children.size() - 1;
                    _traversal_path.emplace_back(const_cast<tree_node*>(current), last_idx);
                    current = current->children[last_idx];
                }
                _current_node = current;
                _current_idx = current->entries.size() - 1;
                return *this;
            }

            if (_current_idx > 0) {
                --_current_idx;
                return *this;
            }

            while (!_traversal_path.empty()) {
                const auto [parent, child_idx] = _traversal_path.back();
                _traversal_path.pop_back();
                if (child_idx > 0) {
                    _current_node = parent;
                    _current_idx = child_idx - 1;
                    return *this;
                }
            }

            return *this;
        }

        self operator--(int)
        {
            self tmp = *this;
            --(*this);
            return tmp;
        }

        [[nodiscard]] bool operator==(const self& other) const noexcept
        {
            return _owner == other._owner
                && _current_node == other._current_node
                && _current_idx == other._current_idx;
        }

        [[nodiscard]] bool operator!=(const self& other) const noexcept
        {
            return !(*this == other);
        }

        [[nodiscard]] std::size_t depth() const noexcept
        {
            assert(_current_node != nullptr);
            return _traversal_path.size();
        }

        [[nodiscard]] std::size_t current_node_keys_count() const noexcept
        {
            assert(_current_node != nullptr);
            return _current_node->entries.size();
        }

        [[nodiscard]] bool is_terminate_node() const noexcept
        {
            assert(_current_node != nullptr);
            return _current_node->is_leaf();
        }

        [[nodiscard]] std::size_t index() const noexcept
        {
            assert(_current_node != nullptr);
            return _current_idx;
        }
    };

    template <bool IsConst>
    [[nodiscard]] basic_iterator<IsConst> make_end() const
    {
        return basic_iterator<IsConst>(this, nullptr, 0);
    }

    template <bool IsConst>
    [[nodiscard]] basic_iterator<IsConst> make_begin() const
    {
        if (_root_node == nullptr) {
            return make_end<IsConst>();
        }

        navigation_path path;
        tree_node* current = _root_node;
        while (!current->is_leaf()) {
            path.emplace_back(current, 0);
            current = current->children[0];
        }

        return basic_iterator<IsConst>(this, current, 0, std::move(path));
    }

    template <bool IsConst>
    [[nodiscard]] basic_iterator<IsConst> lower_bound_impl(const key_type& key) const
    {
        if (_root_node == nullptr) {
            return make_end<IsConst>();
        }

        navigation_path path;
        navigation_path candidate_path;
        tree_node* candidate_node = nullptr;
        std::size_t candidate_idx = 0;

        tree_node* current = _root_node;
        while (current != nullptr) {
            const std::size_t pos = find_insert_position(current, key);
            if (pos < current->entries.size()) {
                candidate_node = current;
                candidate_idx = pos;
                candidate_path = path;
            }

            if (pos < current->entries.size() && key_equal(current->entries[pos].first, key)) {
                return basic_iterator<IsConst>(this, current, pos, std::move(path));
            }

            if (current->is_leaf()) {
                break;
            }

            path.emplace_back(current, pos);
            current = current->children[pos];
        }

        if (candidate_node != nullptr) {
            return basic_iterator<IsConst>(this, candidate_node, candidate_idx, std::move(candidate_path));
        }

        return make_end<IsConst>();
    }

    template <bool IsConst>
    [[nodiscard]] basic_iterator<IsConst> upper_bound_impl(const key_type& key) const
    {
        if (_root_node == nullptr) {
            return make_end<IsConst>();
        }

        navigation_path path;
        navigation_path candidate_path;
        tree_node* candidate_node = nullptr;
        std::size_t candidate_idx = 0;

        tree_node* current = _root_node;
        while (current != nullptr) {
            const std::size_t pos = find_upper_bound_position(current, key);
            if (pos < current->entries.size()) {
                candidate_node = current;
                candidate_idx = pos;
                candidate_path = path;
            }

            if (current->is_leaf()) {
                break;
            }

            path.emplace_back(current, pos);
            current = current->children[pos];
        }

        if (candidate_node != nullptr) {
            return basic_iterator<IsConst>(this, candidate_node, candidate_idx, std::move(candidate_path));
        }

        return make_end<IsConst>();
    }

    template <bool IsConst>
    [[nodiscard]] basic_iterator<IsConst> find_impl(const key_type& key) const
    {
        if (_root_node == nullptr) {
            return make_end<IsConst>();
        }

        navigation_path path;
        tree_node* current = _root_node;
        while (current != nullptr) {
            const std::size_t pos = find_insert_position(current, key);
            if (pos < current->entries.size() && key_equal(current->entries[pos].first, key)) {
                return basic_iterator<IsConst>(this, current, pos, std::move(path));
            }

            if (current->is_leaf()) {
                break;
            }

            path.emplace_back(current, pos);
            current = current->children[pos];
        }

        return make_end<IsConst>();
    }

public:
    explicit B_tree(const key_compare& cmp = key_compare(), allocator_type alloc = allocator_type())
        : key_compare(cmp), _allocator(alloc), _root_node(nullptr), _total_elements(0)
    {
    }

    explicit B_tree(allocator_type alloc, const key_compare& cmp = key_compare())
        : key_compare(cmp), _allocator(alloc), _root_node(nullptr), _total_elements(0)
    {
    }

    template <input_iterator_for_pair<key_type, mapped_type> iterator>
    explicit B_tree(iterator begin, iterator end, const key_compare& cmp = key_compare(), allocator_type alloc = allocator_type())
        : B_tree(cmp, alloc)
    {
        try {
            for (auto it = begin; it != end; ++it) {
                insert(*it);
            }
        } catch (...) {
            clear();
            throw;
        }
    }

    B_tree(std::initializer_list<std::pair<key_type, mapped_type>> data, const key_compare& cmp = key_compare(), allocator_type alloc = allocator_type())
        : B_tree(cmp, alloc)
    {
        try {
            for (const auto& item : data) {
                insert(item);
            }
        } catch (...) {
            clear();
            throw;
        }
    }

    B_tree(const B_tree& other)
        : key_compare(static_cast<const key_compare&>(other)),
          _allocator(alloc_traits::select_on_container_copy_construction(other._allocator)),
          _root_node(nullptr),
          _total_elements(0)
    {
        try {
            for (auto it = other.cbegin(); it != other.cend(); ++it) {
                insert(*it);
            }
        } catch (...) {
            clear();
            throw;
        }
    }

    B_tree(B_tree&& other) noexcept
        : key_compare(std::move(static_cast<key_compare&>(other))),
          _allocator(std::move(other._allocator)),
          _root_node(std::exchange(other._root_node, nullptr)),
          _total_elements(std::exchange(other._total_elements, 0))
    {
    }

    B_tree& operator=(const B_tree& other)
    {
        if (this == &other) {
            return *this;
        }

        B_tree tmp(other);
        if constexpr (propagate_on_copy::value) {
            tmp._allocator = other._allocator;
        }
        swap(tmp);
        return *this;
    }

    B_tree& operator=(B_tree&& other) noexcept
    {
        if (this == &other) {
            return *this;
        }

        clear();

        static_cast<key_compare&>(*this) = std::move(static_cast<key_compare&>(other));
        if constexpr (propagate_on_move::value) {
            _allocator = std::move(other._allocator);
        }

        _root_node = std::exchange(other._root_node, nullptr);
        _total_elements = std::exchange(other._total_elements, 0);
        return *this;
    }

    ~B_tree() noexcept
    {
        clear();
    }

    mapped_type& at(const key_type& key)
    {
        auto [node, idx] = locate_key(key);
        if (node == nullptr) {
            throw std::out_of_range("B_tree::at");
        }
        return node->entries[idx].second;
    }

    const mapped_type& at(const key_type& key) const
    {
        auto [node, idx] = locate_key(key);
        if (node == nullptr) {
            throw std::out_of_range("B_tree::at");
        }
        return node->entries[idx].second;
    }

    mapped_type& operator[](const key_type& key)
    {
        auto [node, idx] = locate_key(key);
        if (node == nullptr) {
            node = insert_new_entry(tree_data_type{key, mapped_type{}}).first;
            idx = locate_key(key).second;
        }
        return node->entries[idx].second;
    }

    mapped_type& operator[](key_type&& key)
    {
        auto [node, idx] = locate_key(key);
        if (node == nullptr) {
            const key_type key_copy = key;
            insert_new_entry(tree_data_type{std::move(key), mapped_type{}});
            auto located = locate_key(key_copy);
            return located.first->entries[located.second].second;
        }
        return node->entries[idx].second;
    }

    iterator begin()
    {
        return make_begin<false>();
    }

    iterator end()
    {
        return make_end<false>();
    }

    const_iterator begin() const
    {
        return make_begin<true>();
    }

    const_iterator end() const
    {
        return make_end<true>();
    }

    const_iterator cbegin() const
    {
        return begin();
    }

    const_iterator cend() const
    {
        return end();
    }

    reverse_iterator rbegin()
    {
        return reverse_iterator(end());
    }

    reverse_iterator rend()
    {
        return reverse_iterator(begin());
    }

    const_reverse_iterator rbegin() const
    {
        return const_reverse_iterator(end());
    }

    const_reverse_iterator rend() const
    {
        return const_reverse_iterator(begin());
    }

    const_reverse_iterator crbegin() const
    {
        return rbegin();
    }

    const_reverse_iterator crend() const
    {
        return rend();
    }

    [[nodiscard]] std::size_t size() const noexcept
    {
        return _total_elements;
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return _total_elements == 0;
    }

    iterator find(const key_type& key)
    {
        return find_impl<false>(key);
    }

    const_iterator find(const key_type& key) const
    {
        return find_impl<true>(key);
    }

    iterator lower_bound(const key_type& key)
    {
        return lower_bound_impl<false>(key);
    }

    const_iterator lower_bound(const key_type& key) const
    {
        return lower_bound_impl<true>(key);
    }

    iterator upper_bound(const key_type& key)
    {
        return upper_bound_impl<false>(key);
    }

    const_iterator upper_bound(const key_type& key) const
    {
        return upper_bound_impl<true>(key);
    }

    [[nodiscard]] bool contains(const key_type& key) const
    {
        return locate_key(key).first != nullptr;
    }

    void clear() noexcept
    {
        destroy_subtree(_root_node);
        _root_node = nullptr;
        _total_elements = 0;
    }

    std::pair<iterator, bool> insert(const tree_data_type& data)
    {
        auto [node, idx] = locate_key(data.first);
        if (node != nullptr) {
            return {find(data.first), false};
        }

        insert_new_entry(data);
        return {find(data.first), true};
    }

    std::pair<iterator, bool> insert(tree_data_type&& data)
    {
        const key_type key_copy = data.first;
        auto [node, idx] = locate_key(key_copy);
        if (node != nullptr) {
            return {find(key_copy), false};
        }

        insert_new_entry(std::move(data));
        return {find(key_copy), true};
    }

    template <typename ...Args>
    std::pair<iterator, bool> emplace(Args&&... args)
    {
        tree_data_type data(std::forward<Args>(args)...);
        return insert(std::move(data));
    }

    iterator insert_or_assign(const tree_data_type& data)
    {
        auto [node, idx] = locate_key(data.first);
        if (node != nullptr) {
            node->entries[idx].second = data.second;
            return find(data.first);
        }

        insert_new_entry(data);
        return find(data.first);
    }

    iterator insert_or_assign(tree_data_type&& data)
    {
        const key_type key_copy = data.first;
        auto [node, idx] = locate_key(key_copy);
        if (node != nullptr) {
            node->entries[idx].second = std::move(data.second);
            return find(key_copy);
        }

        insert_new_entry(std::move(data));
        return find(key_copy);
    }

    template <typename ...Args>
    iterator emplace_or_assign(Args&&... args)
    {
        tree_data_type data(std::forward<Args>(args)...);
        return insert_or_assign(std::move(data));
    }

    iterator erase(iterator pos)
    {
        if (pos == make_end<false>()) {
            return make_end<false>();
        }

        const key_type erased_key = pos->first;
        auto next = pos;
        ++next;

        std::optional<key_type> next_key;
        if (next != make_end<false>() && next._current_node != nullptr) {
            next_key = next->first;
        }

        const bool removed = erase_from_subtree(_root_node, erased_key);
        if (!removed) {
            return end();
        }

        --_total_elements;

        if (_root_node != nullptr && _root_node->entries.empty()) {
            tree_node* old_root = _root_node;
            if (_root_node->is_leaf()) {
                _root_node = nullptr;
            } else {
                _root_node = _root_node->children.front();
                old_root->children.clear();
            }
            deallocate_node(old_root);
        }

        if (next_key.has_value()) {
            return find(*next_key);
        }

        return end();
    }

    iterator erase(const_iterator pos)
    {
        if (pos == cend()) {
            return end();
        }

        return erase(find(pos->first));
    }

    iterator erase(iterator first, iterator last)
    {
        while (first != last) {
            first = erase(first);
        }
        return first;
    }

    iterator erase(const_iterator first, const_iterator last)
    {
        while (first != last) {
            first = erase(first);
        }

        if (first == cend()) {
            return end();
        }

        return find(first->first);
    }

    iterator erase(const key_type& key)
    {
        auto [node, idx] = locate_key(key);
        if (node == nullptr) {
            return end();
        }

        return erase(find(key));
    }

private:
    void swap(B_tree& other) noexcept
    {
        using std::swap;

        if constexpr (propagate_on_swap::value) {
            swap(_allocator, other._allocator);
        } else {
            assert(_allocator == other._allocator);
        }

        swap(static_cast<key_compare&>(*this), static_cast<key_compare&>(other));
        swap(_root_node, other._root_node);
        swap(_total_elements, other._total_elements);
    }
};

template<std::input_iterator iterator, comparator<typename std::iterator_traits<iterator>::value_type::first_type> compare = std::less<typename std::iterator_traits<iterator>::value_type::first_type>,
    std::size_t degree = 5, typename U>
B_tree(iterator begin, iterator end, const compare& cmp = compare(), pp_allocator<U> = pp_allocator<U>())
    -> B_tree<typename std::iterator_traits<iterator>::value_type::first_type, typename std::iterator_traits<iterator>::value_type::second_type, compare, degree>;

template<typename key_type, typename mapped_type, comparator<key_type> compare = std::less<key_type>, std::size_t degree = 5, typename U>
B_tree(std::initializer_list<std::pair<key_type, mapped_type>> data, const compare& cmp = compare(), pp_allocator<U> = pp_allocator<U>())
    -> B_tree<key_type, mapped_type, compare, degree>;

#endif