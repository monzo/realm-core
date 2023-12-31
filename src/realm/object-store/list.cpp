////////////////////////////////////////////////////////////////////////////
//
// Copyright 2015 Realm Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
////////////////////////////////////////////////////////////////////////////

#include <realm/object-store/list.hpp>

#include <realm/object-store/object_schema.hpp>
#include <realm/object-store/object_store.hpp>
#include <realm/object-store/results.hpp>
#include <realm/object-store/schema.hpp>
#include <realm/object-store/shared_realm.hpp>
#include <realm/exceptions.hpp>

namespace {
using namespace realm;

template <typename T>
struct ListType {
    using type = Lst<T>;
};

template <>
struct ListType<Obj> {
    using type = LnkLst;
};

} // namespace

namespace realm {
using namespace _impl;

List::List(const List&) = default;
List& List::operator=(const List&) = default;
List::List(List&&) = default;
List& List::operator=(List&&) = default;

Query List::get_query() const
{
    return get_table()->where(as<Obj>());
}

ConstTableRef List::get_table() const
{
    verify_attached();
    if (m_type == PropertyType::Object)
        return list_base().get_target_table();
    throw NotImplemented();
}

template <typename T>
T List::get(size_t row_ndx) const
{
    verify_attached();
    return as<T>().get(row_ndx);
}

template <>
Mixed List::get(size_t row_ndx) const
{
    verify_attached();
    auto value = as<Mixed>().get(row_ndx);
    record_audit_read(value);
    return value;
}

template <>
Obj List::get(size_t row_ndx) const
{
    verify_attached();
    auto& list = as<Obj>();
    auto obj = list.get_target_table()->get_object(list.get(row_ndx));
    record_audit_read(obj);
    return obj;
}

template <typename T>
size_t List::find(T const& value) const
{
    verify_attached();
    return as<T>().find_first(value);
}

template <>
size_t List::find(Obj const& o) const
{
    verify_attached();
    if (!o.is_valid())
        return not_found;
    validate(o);

    return as<Obj>().find_first(o.get_key());
}

size_t List::find(Query&& q) const
{
    verify_attached();
    if (m_type == PropertyType::Object) {
        ObjKey key = get_query().and_query(std::move(q)).find();
        return key ? as<Obj>().find_first(key) : not_found;
    }
    throw NotImplemented();
}

template <typename T>
void List::add(T value)
{
    verify_in_transaction();
    as<T>().add(value);
}

template <>
void List::add(Obj o)
{
    verify_in_transaction();
    validate(o);
    as<Obj>().add(o.get_key());
}

template <typename T>
void List::insert(size_t row_ndx, T value)
{
    verify_in_transaction();
    as<T>().insert(row_ndx, value);
}

template <>
void List::insert(size_t row_ndx, Obj o)
{
    verify_in_transaction();
    validate(o);
    as<Obj>().insert(row_ndx, o.get_key());
}

void List::move(size_t source_ndx, size_t dest_ndx)
{
    verify_in_transaction();
    list_base().move(source_ndx, dest_ndx);
}

void List::remove(size_t row_ndx)
{
    verify_in_transaction();
    list_base().remove(row_ndx, row_ndx + 1);
}

void List::remove_all()
{
    verify_in_transaction();
    list_base().clear();
}

template <typename T>
void List::set(size_t row_ndx, T value)
{
    verify_in_transaction();
    //    validate(row);
    as<T>().set(row_ndx, value);
}

void List::insert_any(size_t row_ndx, Mixed value)
{
    verify_in_transaction();
    list_base().insert_any(row_ndx, value);
}

void List::set_any(size_t row_ndx, Mixed value)
{
    verify_in_transaction();
    list_base().set_any(row_ndx, value);
}

Mixed List::get_any(size_t row_ndx) const
{
    verify_attached();
    auto value = list_base().get_any(row_ndx);
    record_audit_read(value);
    return value;
}

size_t List::find_any(Mixed value) const
{
    verify_attached();
    return list_base().find_any(value);
}

template <>
void List::set(size_t row_ndx, Obj o)
{
    verify_in_transaction();
    validate(o);
    as<Obj>().set(row_ndx, o.get_key());
}

Obj List::add_embedded()
{
    verify_in_transaction();

    if (!m_is_embedded)
        throw IllegalOperation("Not a list of embedded objects");

    return as<Obj>().create_and_insert_linked_object(size());
}

Obj List::set_embedded(size_t list_ndx)
{
    verify_in_transaction();

    if (!m_is_embedded)
        throw IllegalOperation("Not a list of embedded objects");

    return as<Obj>().create_and_set_linked_object(list_ndx);
}

Obj List::insert_embedded(size_t list_ndx)
{
    verify_in_transaction();

    if (!m_is_embedded)
        throw IllegalOperation("Not a list of embedded objects");

    return as<Obj>().create_and_insert_linked_object(list_ndx);
}

Obj List::get_object(size_t list_ndx)
{
    verify_attached();
    if (m_type == PropertyType::Object) {
        return as<Obj>().get_object(list_ndx);
    }
    return {};
}

void List::swap(size_t ndx1, size_t ndx2)
{
    verify_in_transaction();
    list_base().swap(ndx1, ndx2);
}

void List::delete_at(size_t row_ndx)
{
    verify_in_transaction();
    if (m_type == PropertyType::Object)
        as<Obj>().remove_target_row(row_ndx);
    else
        list_base().remove(row_ndx, row_ndx + 1);
}

void List::delete_all()
{
    verify_in_transaction();
    if (m_type == PropertyType::Object)
        as<Obj>().remove_all_target_rows();
    else
        list_base().clear();
}

Results List::filter(Query q) const
{
    verify_attached();
    return Results(m_realm, std::dynamic_pointer_cast<LnkLst>(m_coll_base), get_query().and_query(std::move(q)));
}

bool List::operator==(List const& rgt) const noexcept
{
    return list_base().get_table() == rgt.list_base().get_table() &&
           list_base().get_owner_key() == rgt.list_base().get_owner_key() &&
           list_base().get_col_key() == rgt.list_base().get_col_key();
}

List List::freeze(std::shared_ptr<Realm> const& frozen_realm) const
{
    auto frozen_list(frozen_realm->import_copy_of(*m_coll_base));
    if (frozen_list) {
        return List(frozen_realm, std::move(frozen_list));
    }
    else {
        return List{};
    }
}

#define REALM_PRIMITIVE_LIST_TYPE(T)                                                                                 \
    template T List::get<T>(size_t) const;                                                                           \
    template size_t List::find<T>(T const&) const;                                                                   \
    template void List::add<T>(T);                                                                                   \
    template void List::insert<T>(size_t, T);                                                                        \
    template void List::set<T>(size_t, T);

REALM_PRIMITIVE_LIST_TYPE(bool)
REALM_PRIMITIVE_LIST_TYPE(int64_t)
REALM_PRIMITIVE_LIST_TYPE(float)
REALM_PRIMITIVE_LIST_TYPE(double)
REALM_PRIMITIVE_LIST_TYPE(StringData)
REALM_PRIMITIVE_LIST_TYPE(BinaryData)
REALM_PRIMITIVE_LIST_TYPE(Timestamp)
REALM_PRIMITIVE_LIST_TYPE(ObjKey)
REALM_PRIMITIVE_LIST_TYPE(ObjectId)
REALM_PRIMITIVE_LIST_TYPE(Decimal)
REALM_PRIMITIVE_LIST_TYPE(UUID)
REALM_PRIMITIVE_LIST_TYPE(util::Optional<bool>)
REALM_PRIMITIVE_LIST_TYPE(util::Optional<int64_t>)
REALM_PRIMITIVE_LIST_TYPE(util::Optional<float>)
REALM_PRIMITIVE_LIST_TYPE(util::Optional<double>)
REALM_PRIMITIVE_LIST_TYPE(util::Optional<ObjectId>)
REALM_PRIMITIVE_LIST_TYPE(util::Optional<UUID>)

template size_t List::find<Mixed>(Mixed const&) const;
template void List::add<Mixed>(Mixed);
template void List::insert<Mixed>(size_t, Mixed);
template void List::set<Mixed>(size_t, Mixed);

#undef REALM_PRIMITIVE_LIST_TYPE
} // namespace realm

namespace std {
size_t hash<List>::operator()(List const& list) const
{
    return list.hash();
}
} // namespace std
