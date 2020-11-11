/*************************************************************************
 *
 * Copyright 2016 Realm Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 **************************************************************************/

#ifndef REALM_LIST_HPP
#define REALM_LIST_HPP

#include <realm/collection.hpp>

#include <realm/obj.hpp>
#include <realm/bplustree.hpp>
#include <realm/obj_list.hpp>
#include <realm/array_basic.hpp>
#include <realm/array_integer.hpp>
#include <realm/array_key.hpp>
#include <realm/array_bool.hpp>
#include <realm/array_string.hpp>
#include <realm/array_binary.hpp>
#include <realm/array_timestamp.hpp>
#include <realm/array_ref.hpp>
#include <realm/array_fixed_bytes.hpp>
#include <realm/array_decimal128.hpp>
#include <realm/array_mixed.hpp>
#include <realm/array_typed_link.hpp>
#include <realm/replication.hpp>

#ifdef _MSC_VER
#pragma warning(disable : 4250) // Suppress 'inherits ... via dominance' on MSVC
#endif

namespace realm {

class TableView;
class SortDescriptor;
class Group;
class LstBase;
template <class>
class Lst;

template <class T>
using LstIterator = CollectionIterator<Lst<T>>;

/*
 * This class defines a virtual interface to a writable list
 */
class LstBase : public CollectionBase {
public:
    using CollectionBase::CollectionBase;

    virtual ~LstBase() {}
    virtual LstBasePtr clone() const = 0;
    virtual void set_null(size_t ndx) = 0;
    virtual void set_any(size_t ndx, Mixed val) = 0;
    virtual void insert_null(size_t ndx) = 0;
    virtual void insert_any(size_t ndx, Mixed val) = 0;
    virtual void resize(size_t new_size) = 0;
    virtual void remove(size_t from, size_t to) = 0;
    virtual void move(size_t from, size_t to) = 0;
    virtual void swap(size_t ndx1, size_t ndx2) = 0;

protected:
    void swap_repl(Replication* repl, size_t ndx1, size_t ndx2) const;
};

template <class T>
class Lst final : public CollectionBaseImpl<LstBase> {
public:
    using Base = CollectionBaseImpl<LstBase>;
    using iterator = LstIterator<T>;
    using value_type = T;

    Lst() = default;
    Lst(const Obj& owner, ColKey col_key);
    Lst(const Lst& other);
    Lst& operator=(const Lst& other);
    Lst& operator=(const BPlusTree<T>& other);

    void create();

    iterator begin() const noexcept
    {
        return iterator{this, 0};
    }

    iterator end() const noexcept
    {
        return iterator{this, size()};
    }

    T get(size_t ndx) const;
    size_t find_first(const T& value) const;
    T set(size_t ndx, T value);
    void insert(size_t ndx, T value);
    T remove(size_t ndx);

    // Overriding members of CollectionBase:
    using Base::get_col_key;
    using Base::get_obj;
    using Base::get_table;
    using Base::get_target_table;
    using Base::has_changed;
    using Base::is_attached;
    size_t size() const final;
    void clear() final;
    Mixed get_any(size_t ndx) const final;
    bool is_null(size_t ndx) const final;
    CollectionBasePtr clone_collection() const final;
    Mixed min(size_t* return_ndx = nullptr) const final;
    Mixed max(size_t* return_ndx = nullptr) const final;
    Mixed sum(size_t* return_cnt = nullptr) const final;
    Mixed avg(size_t* return_cnt = nullptr) const final;
    void sort(std::vector<size_t>& indices, bool ascending = true) const final;
    void distinct(std::vector<size_t>& indices, util::Optional<bool> sort_order = util::none) const final;

    // Overriding members of LstBase:
    LstBasePtr clone() const final;
    void set_null(size_t ndx) final;
    void set_any(size_t ndx, Mixed val) final;
    void insert_null(size_t ndx) final;
    void insert_any(size_t ndx, Mixed val) final;
    void resize(size_t new_size) final;
    void remove(size_t from, size_t to) final;
    void move(size_t from, size_t to) final;
    void swap(size_t ndx1, size_t ndx2) final;

    // Lst<T> interface:
    T remove(const iterator& it);

    void add(T value)
    {
        insert(size(), std::move(value));
    }

    T operator[](size_t ndx) const
    {
        return this->get(ndx);
    }

    template <typename Func>
    void find_all(value_type value, Func&& func) const
    {
        if (m_valid && init_from_parent())
            m_tree->find_all(value, std::forward<Func>(func));
    }

    inline const BPlusTree<T>& get_tree() const
    {
        return *m_tree;
    }

protected:
    void ensure_created();
    void do_set(size_t ndx, T value);
    void do_insert(size_t ndx, T value);
    void do_remove(size_t ndx);

    friend class LnkLst;

    mutable std::unique_ptr<BPlusTree<T>> m_tree;
    using Base::m_col_key;
    using Base::m_nullable;
    using Base::m_obj;
    using Base::m_valid;
    using Base::update_if_needed;

    bool init_from_parent() const final
    {
        m_valid = m_tree->init_from_parent();
        update_content_version();
        return m_valid;
    }
};

// Specialization of Lst<ObjKey>:
template <>
void Lst<ObjKey>::clear();
template <>
void Lst<ObjKey>::do_set(size_t, ObjKey);
template <>
void Lst<ObjKey>::do_insert(size_t, ObjKey);
template <>
void Lst<ObjKey>::do_remove(size_t);
extern template class Lst<ObjKey>;

// Specialization of Lst<Mixed>:
template <>
void Lst<Mixed>::do_set(size_t, Mixed);
template <>
void Lst<Mixed>::do_insert(size_t, Mixed);
template <>
void Lst<Mixed>::do_remove(size_t);
extern template class Lst<Mixed>;

// Specialization of Lst<ObjLink>:
template <>
void Lst<ObjLink>::do_set(size_t, ObjLink);
template <>
void Lst<ObjLink>::do_insert(size_t, ObjLink);
template <>
void Lst<ObjLink>::do_remove(size_t);
extern template class Lst<ObjLink>;

class LnkLst final : public ObjCollectionBase<LstBase> {
public:
    using Base = ObjCollectionBase<LstBase>;
    using value_type = ObjKey;
    using iterator = CollectionIterator<LnkLst>;

    LnkLst() = default;

    LnkLst(const Obj& owner, ColKey col_key)
        : m_keys(owner, col_key)
    {
        update_unresolved(*m_keys.m_tree);
    }

    LnkLst(const LnkLst& other) = default;
    LnkLst& operator=(const LnkLst& other) = default;
    bool operator==(const LnkLst& other) const;
    bool operator!=(const LnkLst& other) const;

    Obj operator[](size_t ndx) const
    {
        return get_object(ndx);
    }

    ObjKey get(size_t ndx) const;
    size_t find_first(const ObjKey&) const;
    void insert(size_t ndx, ObjKey value);
    ObjKey set(size_t ndx, ObjKey value);
    ObjKey remove(size_t ndx);

    void add(ObjKey value)
    {
        // FIXME: Should this add to the end of the unresolved list?
        insert(size(), value);
    }

    // Overriding members of CollectionBase:
    size_t size() const final;
    bool is_null(size_t ndx) const final;
    Mixed get_any(size_t ndx) const final;
    void clear() final;
    Mixed min(size_t* return_ndx = nullptr) const final;
    Mixed max(size_t* return_ndx = nullptr) const final;
    Mixed sum(size_t* return_cnt = nullptr) const final;
    Mixed avg(size_t* return_cnt = nullptr) const final;
    std::unique_ptr<CollectionBase> clone_collection() const final;
    TableRef get_target_table() const final;
    void sort(std::vector<size_t>& indices, bool ascending = true) const final;
    void distinct(std::vector<size_t>& indices, util::Optional<bool> sort_order = util::none) const final;
    const Obj& get_obj() const noexcept final;
    ObjKey get_key() const final;
    bool is_attached() const final;
    bool has_changed() const final;
    ConstTableRef get_table() const noexcept final;
    ColKey get_col_key() const noexcept final;

    // Overriding members of LstBase:
    std::unique_ptr<LstBase> clone() const
    {
        if (get_obj().is_valid()) {
            return std::make_unique<LnkLst>(get_obj(), get_col_key());
        }
        else {
            return std::make_unique<LnkLst>();
        }
    }
    void set_null(size_t ndx) final;
    void set_any(size_t ndx, Mixed val) final;
    void insert_null(size_t ndx) final;
    void insert_any(size_t ndx, Mixed val) final;
    void resize(size_t new_size) final;
    void remove(size_t from, size_t to) final;
    void move(size_t from, size_t to) final;
    void swap(size_t ndx1, size_t ndx2) final;

    // Overriding members of ObjList:
    bool is_obj_valid(size_t) const noexcept final
    {
        // A link list cannot contain null values
        return true;
    }
    Obj get_object(size_t ndx) const final
    {
        ObjKey key = this->get(ndx);
        return get_target_table()->get_object(key);
    }

    ObjKey get_key(size_t ndx) const final
    {
        return get(ndx);
    }

    // LnkLst interface:

    std::unique_ptr<LnkLst> clone_linklist() const
    {
        return std::make_unique<LnkLst>(*this);
    }

    template <class Func>
    void find_all(ObjKey value, Func&& func) const
    {
        if (value.is_unresolved())
            return;

        m_keys.find_all(value, [&](size_t ndx) {
            func(real2virtual(ndx));
        });
    }

    // Create a new object in insert a link to it
    Obj create_and_insert_linked_object(size_t ndx);

    // Create a new object and link it. If an embedded object
    // is already set, it will be removed. TBD: If a non-embedded
    // object is already set, we throw LogicError (to prevent
    // dangling objects, since they do not delete automatically
    // if they are not embedded...)
    Obj create_and_set_linked_object(size_t ndx);

    // to be implemented:
    Obj clear_linked_object(size_t ndx);

    TableView get_sorted_view(SortDescriptor order) const;
    TableView get_sorted_view(ColKey column_key, bool ascending = true) const;
    void remove_target_row(size_t link_ndx);
    void remove_all_target_rows();

    iterator begin() const noexcept
    {
        return iterator{this, 0};
    }
    iterator end() const noexcept
    {
        return iterator{this, size()};
    }

    const BPlusTree<ObjKey>& get_tree() const
    {
        return m_keys.get_tree();
    }

protected:
    bool update_if_needed() const final
    {
        if (m_keys.update_if_needed()) {
            update_unresolved(*m_keys.m_tree);
            return true;
        }
        return false;
    }

private:
    friend class ConstTableView;
    friend class Query;

    Lst<ObjKey> m_keys;

    bool init_from_parent() const final;
};


// Implementation:

inline void LstBase::swap_repl(Replication* repl, size_t ndx1, size_t ndx2) const
{
    if (ndx2 < ndx1)
        std::swap(ndx1, ndx2);
    repl->list_move(*this, ndx2, ndx1);
    if (ndx1 + 1 != ndx2)
        repl->list_move(*this, ndx1 + 1, ndx2);
}

template <class T>
inline Lst<T>::Lst(const Obj& obj, ColKey col_key)
    : Base(obj, col_key)
    , m_tree(new BPlusTree<T>(obj.get_alloc()))
{
    if (!col_key.is_list()) {
        throw LogicError(LogicError::collection_type_mismatch);
    }

    check_column_type<T>(m_col_key);

    m_tree->set_parent(this, 0); // ndx not used, implicit in m_owner
    if (m_obj) {
        // Fine because init_from_parent() is final.
        init_from_parent();
    }
}

template <class T>
inline Lst<T>::Lst(const Lst& other)
    : Base(static_cast<const Base&>(other))
{
    if (other.m_tree) {
        Allocator& alloc = other.m_tree->get_alloc();
        m_tree = std::make_unique<BPlusTree<T>>(alloc);
        m_tree->set_parent(this, 0);
        if (m_valid)
            m_tree->init_from_ref(other.m_tree->get_ref());
    }
}

template <class T>
inline void Lst<T>::create()
{
    m_tree->create();
    m_valid = true;
}

template <class T>
Lst<T>& Lst<T>::operator=(const Lst& other)
{
    Base::operator=(static_cast<const Base&>(other));

    if (this != &other) {
        m_tree.reset();
        if (other.m_tree) {
            Allocator& alloc = other.m_tree->get_alloc();
            m_tree = std::make_unique<BPlusTree<T>>(alloc);
            m_tree->set_parent(this, 0);
            if (m_valid) {
                m_tree->init_from_ref(other.m_tree->get_ref());
            }
        }
    }

    return *this;
}

template <class T>
Lst<T>& Lst<T>::operator=(const BPlusTree<T>& other)
{
    *m_tree = other;
    return *this;
}

template <class T>
T Lst<T>::remove(const iterator& it)
{
    return remove(it.index());
}

template <class T>
inline size_t Lst<T>::size() const
{
    if (!is_attached())
        return 0;
    update_if_needed();
    if (!m_valid) {
        return 0;
    }
    return m_tree->size();
}

template <class T>
inline bool Lst<T>::is_null(size_t ndx) const
{
    return m_nullable && value_is_null(get(ndx));
}

template <class T>
inline Mixed Lst<T>::get_any(size_t ndx) const
{
    return get(ndx);
}

template <class T>
inline void Lst<T>::ensure_created()
{
    if (!m_valid && m_obj.is_valid()) {
        create();
    }
}

template <class T>
inline void Lst<T>::do_set(size_t ndx, T value)
{
    m_tree->set(ndx, value);
}

template <class T>
inline void Lst<T>::do_insert(size_t ndx, T value)
{
    m_tree->insert(ndx, value);
}

template <class T>
inline void Lst<T>::do_remove(size_t ndx)
{
    m_tree->erase(ndx);
}


template <typename U>
Lst<U> Obj::get_list(ColKey col_key) const
{
    return Lst<U>(*this, col_key);
}

template <typename U>
LstPtr<U> Obj::get_list_ptr(ColKey col_key) const
{
    return std::make_unique<Lst<U>>(*this, col_key);
}

inline LnkLst Obj::get_linklist(ColKey col_key) const
{
    return LnkLst(*this, col_key);
}

inline LnkLstPtr Obj::get_linklist_ptr(ColKey col_key) const
{
    return std::make_unique<LnkLst>(*this, col_key);
}

inline LnkLst Obj::get_linklist(StringData col_name) const
{
    return get_linklist(get_column_key(col_name));
}

template <class T>
void Lst<T>::clear()
{
    static_assert(!std::is_same_v<T, ObjKey>);
    ensure_created();
    update_if_needed();
    this->ensure_writeable();
    if (size() > 0) {
        if (Replication* repl = this->m_obj.get_replication()) {
            repl->list_clear(*this);
        }
        m_tree->clear();
        bump_content_version();
    }
}

template <class T>
CollectionBasePtr Lst<T>::clone_collection() const
{
    return std::make_unique<Lst<T>>(m_obj, m_col_key);
}

template <class T>
inline T Lst<T>::get(size_t ndx) const
{
    const auto current_size = size();
    if (ndx >= current_size) {
        throw std::out_of_range("Index out of range");
    }
    return m_tree->get(ndx);
}

template <class T>
inline size_t Lst<T>::find_first(const T& value) const
{
    if (!m_valid && !init_from_parent())
        return not_found;
    update_if_needed();
    return m_tree->find_first(value);
}

template <class T>
inline Mixed Lst<T>::min(size_t* return_ndx) const
{
    return MinHelper<T>::eval(*m_tree, return_ndx);
}

template <class T>
inline Mixed Lst<T>::max(size_t* return_ndx) const
{
    return MaxHelper<T>::eval(*m_tree, return_ndx);
}

template <class T>
inline Mixed Lst<T>::sum(size_t* return_cnt) const
{
    return SumHelper<T>::eval(*m_tree, return_cnt);
}

template <class T>
inline Mixed Lst<T>::avg(size_t* return_cnt) const
{
    return AverageHelper<T>::eval(*m_tree, return_cnt);
}

template <class T>
LstBasePtr Lst<T>::clone() const
{
    return std::make_unique<Lst<T>>(m_obj, m_col_key);
}

template <class T>
void Lst<T>::set_null(size_t ndx)
{
    set(ndx, BPlusTree<T>::default_value(m_nullable));
}

template <class T>
void Lst<T>::set_any(size_t ndx, Mixed val)
{
    if constexpr (std::is_same_v<T, Mixed>) {
        set(ndx, val);
    }
    else {
        if (val.is_null()) {
            set_null(ndx);
        }
        else {
            set(ndx, val.get<typename util::RemoveOptional<T>::type>());
        }
    }
}

template <class T>
void Lst<T>::insert_null(size_t ndx)
{
    insert(ndx, BPlusTree<T>::default_value(m_nullable));
}

template <class T>
void Lst<T>::insert_any(size_t ndx, Mixed val)
{
    if constexpr (std::is_same_v<T, Mixed>) {
        insert(ndx, val);
    }
    else {
        if (val.is_null()) {
            insert_null(ndx);
        }
        else {
            insert(ndx, val.get<typename util::RemoveOptional<T>::type>());
        }
    }
}

template <class T>
void Lst<T>::resize(size_t new_size)
{
    update_if_needed();
    size_t current_size = m_tree->size();
    while (new_size > current_size) {
        insert_null(current_size++);
    }
    remove(new_size, current_size);
    m_obj.bump_both_versions();
}

template <class T>
void Lst<T>::remove(size_t from, size_t to)
{
    while (from < to) {
        remove(--to);
    }
}

template <class T>
void Lst<T>::move(size_t from, size_t to)
{
    update_if_needed();
    if (from != to) {
        this->ensure_writeable();
        if (Replication* repl = this->m_obj.get_replication()) {
            repl->list_move(*this, from, to);
        }
        if (to > from) {
            to++;
        }
        else {
            from++;
        }
        // We use swap here as it handles the special case for StringData where
        // 'to' and 'from' points into the same array. In this case you cannot
        // set an entry with the result of a get from another entry in the same
        // leaf.
        m_tree->insert(to, BPlusTree<T>::default_value(m_nullable));
        m_tree->swap(from, to);
        m_tree->erase(from);

        bump_content_version();
    }
}

template <class T>
void Lst<T>::swap(size_t ndx1, size_t ndx2)
{
    update_if_needed();
    if (ndx1 != ndx2) {
        if (Replication* repl = this->m_obj.get_replication()) {
            LstBase::swap_repl(repl, ndx1, ndx2);
        }
        m_tree->swap(ndx1, ndx2);
        bump_content_version();
    }
}

template <class T>
T Lst<T>::set(size_t ndx, T value)
{
    update_if_needed();

    if (value_is_null(value) && !m_nullable)
        throw LogicError(LogicError::column_not_nullable);

    // get will check for ndx out of bounds
    T old = get(ndx);
    if (old != value) {
        this->ensure_writeable();
        do_set(ndx, value);
        bump_content_version();
    }
    if (Replication* repl = this->m_obj.get_replication()) {
        repl->list_set(*this, ndx, value);
    }
    return old;
}

template <class T>
void Lst<T>::insert(size_t ndx, T value)
{
    update_if_needed();

    if (value_is_null(value) && !m_nullable)
        throw LogicError(LogicError::column_not_nullable);

    ensure_created();
    if (ndx > m_tree->size()) {
        throw std::out_of_range("Index out of range");
    }
    this->ensure_writeable();
    if (Replication* repl = this->m_obj.get_replication()) {
        repl->list_insert(*this, ndx, value);
    }
    do_insert(ndx, value);
    bump_content_version();
}

template <class T>
T Lst<T>::remove(size_t ndx)
{
    update_if_needed();

    this->ensure_writeable();

    // get will check for ndx out of bounds
    T old = get(ndx);
    if (Replication* repl = this->m_obj.get_replication()) {
        repl->list_erase(*this, ndx);
    }

    do_remove(ndx);
    bump_content_version();
    return old;
}

inline bool LnkLst::operator==(const LnkLst& other) const
{
    return m_keys == other.m_keys;
}
inline bool LnkLst::operator!=(const LnkLst& other) const
{
    return m_keys != other.m_keys;
}

inline size_t LnkLst::size() const
{
    update_if_needed();
    return m_keys.size() - num_unresolved();
}

inline bool LnkLst::is_null(size_t ndx) const
{
    update_if_needed();
    return m_keys.is_null(virtual2real(ndx));
}

inline Mixed LnkLst::get_any(size_t ndx) const
{
    update_if_needed();
    return m_keys.get_any(virtual2real(ndx));
}

inline void LnkLst::clear()
{
    m_keys.clear();
    clear_unresolved();
}

inline Mixed LnkLst::min(size_t* return_ndx) const
{
    static_cast<void>(return_ndx);
    REALM_TERMINATE("Not implemented yet");
}
inline Mixed LnkLst::max(size_t* return_ndx) const
{
    static_cast<void>(return_ndx);
    REALM_TERMINATE("Not implemented yet");
}
inline Mixed LnkLst::sum(size_t* return_cnt) const
{
    static_cast<void>(return_cnt);
    REALM_TERMINATE("Not implemented yet");
}
inline Mixed LnkLst::avg(size_t* return_cnt) const
{
    static_cast<void>(return_cnt);
    REALM_TERMINATE("Not implemented yet");
}

inline std::unique_ptr<CollectionBase> LnkLst::clone_collection() const
{
    return get_obj().get_linklist_ptr(get_col_key());
}

inline TableRef LnkLst::get_target_table() const
{
    return m_keys.get_target_table();
}

inline void LnkLst::sort(std::vector<size_t>& indices, bool ascending) const
{
    static_cast<void>(indices);
    static_cast<void>(ascending);
    REALM_TERMINATE("Not implemented yet");
}

inline void LnkLst::distinct(std::vector<size_t>& indices, util::Optional<bool> sort_order) const
{
    static_cast<void>(indices);
    static_cast<void>(sort_order);
    REALM_TERMINATE("Not implemented yet");
}

inline const Obj& LnkLst::get_obj() const noexcept
{
    return m_keys.get_obj();
}

inline ObjKey LnkLst::get_key() const
{
    return m_keys.get_key();
}

inline bool LnkLst::is_attached() const
{
    return m_keys.is_attached();
}

inline bool LnkLst::has_changed() const
{
    return m_keys.has_changed();
}

inline ConstTableRef LnkLst::get_table() const noexcept
{
    return m_keys.get_table();
}

inline ColKey LnkLst::get_col_key() const noexcept
{
    return m_keys.get_col_key();
}

inline void LnkLst::set_null(size_t ndx)
{
    update_if_needed();
    m_keys.set_null(virtual2real(ndx));
}

inline void LnkLst::set_any(size_t ndx, Mixed val)
{
    update_if_needed();
    m_keys.set_any(virtual2real(ndx), val);
}

inline void LnkLst::insert_null(size_t ndx)
{
    update_if_needed();
    m_keys.insert_null(virtual2real(ndx));
}

inline void LnkLst::insert_any(size_t ndx, Mixed val)
{
    update_if_needed();
    m_keys.insert_any(virtual2real(ndx), val);
}

inline void LnkLst::resize(size_t new_size)
{
    update_if_needed();
    m_keys.resize(new_size + num_unresolved());
}

inline void LnkLst::remove(size_t from, size_t to)
{
    update_if_needed();
    m_keys.remove(virtual2real(from), virtual2real(to));
}

inline void LnkLst::move(size_t from, size_t to)
{
    update_if_needed();
    m_keys.move(virtual2real(from), virtual2real(to));
}

inline void LnkLst::swap(size_t ndx1, size_t ndx2)
{
    update_if_needed();
    m_keys.swap(virtual2real(ndx1), virtual2real(ndx2));
}

inline ObjKey LnkLst::get(size_t ndx) const
{
    update_if_needed();
    return m_keys.get(virtual2real(ndx));
}

inline size_t LnkLst::find_first(const ObjKey& key) const
{
    if (key.is_unresolved())
        return not_found;

    update_if_needed();
    size_t found = m_keys.find_first(key);
    if (found == not_found)
        return not_found;
    return real2virtual(found);
}

inline void LnkLst::insert(size_t ndx, ObjKey value)
{
    REALM_ASSERT(!value.is_unresolved());
    if (get_target_table()->is_embedded() && value != ObjKey())
        throw LogicError(LogicError::wrong_kind_of_table);
    update_if_needed();
    m_keys.insert(virtual2real(ndx), value);
}

inline ObjKey LnkLst::set(size_t ndx, ObjKey value)
{
    REALM_ASSERT(!value.is_unresolved());
    if (get_target_table()->is_embedded() && value != ObjKey())
        throw LogicError(LogicError::wrong_kind_of_table);
    update_if_needed();
    ObjKey old = m_keys.set(virtual2real(ndx), value);
    REALM_ASSERT(!old.is_unresolved());
    return old;
}

inline ObjKey LnkLst::remove(size_t ndx)
{
    update_if_needed();
    ObjKey old = m_keys.remove(virtual2real(ndx));
    REALM_ASSERT(!old.is_unresolved());
    return old;
}

} // namespace realm

#endif /* REALM_LIST_HPP */
