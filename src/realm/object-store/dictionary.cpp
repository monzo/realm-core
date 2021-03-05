/////////////////////////////////////////////////////////////////////////////
//
// Copyright 2020 Realm Inc.
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

#include <realm/object-store/dictionary.hpp>
#include <realm/object-store/results.hpp>
#include <realm/table.hpp>

namespace realm {
namespace {
class DictionaryKeyAdapter : public CollectionBase {
public:
    DictionaryKeyAdapter(std::shared_ptr<Dictionary> dictionary)
        : m_dictionary(std::move(dictionary))
    {
    }

    // -------------------------------------------------------------------------
    // Things which this adapter does something different from Dictionary for

    Mixed get_any(size_t ndx) const override
    {
        return m_dictionary->get_key(ndx);
    }

    size_t find_any(Mixed value) const override
    {
        return m_dictionary->find_any_key(value);
    }

    ColKey get_col_key() const noexcept override
    {
        auto col_key = m_dictionary->get_col_key();
        auto type = ColumnType(m_dictionary->get_key_data_type());
        return ColKey(col_key.get_index(), type, col_key.get_attrs(), col_key.get_tag());
    }

    std::unique_ptr<CollectionBase> clone_collection() const override
    {
        return std::make_unique<DictionaryKeyAdapter>(*this);
    }

    // -------------------------------------------------------------------------
    // Things which this just forwards on to Dictionary

    size_t size() const override
    {
        return m_dictionary->size();
    }
    bool is_null(size_t ndx) const override
    {
        return m_dictionary->is_null(ndx);
    }
    void clear() override
    {
        m_dictionary->clear();
    }
    void sort(std::vector<size_t>& indices, bool ascending = true) const override
    {
        m_dictionary->sort(indices, ascending);
    }
    void distinct(std::vector<size_t>& indices, util::Optional<bool> sort_order = util::none) const override
    {
        m_dictionary->distinct(indices, sort_order);
    }
    const Obj& get_obj() const noexcept override
    {
        return m_dictionary->get_obj();
    }
    bool has_changed() const override
    {
        return m_dictionary->has_changed();
    }

    // -------------------------------------------------------------------------
    // Things not applicable to the adapter

    bool init_from_parent() const override
    {
        REALM_TERMINATE("not implemented");
    }
    bool update_if_needed() const override
    {
        REALM_TERMINATE("not implemented");
    }

    // We currently only support string keys which means these aren't reachable
    // as Results will handle the type-checks
    util::Optional<Mixed> min(size_t* = nullptr) const override
    {
        REALM_TERMINATE("not implemented");
    }
    util::Optional<Mixed> max(size_t* = nullptr) const override
    {
        REALM_TERMINATE("not implemented");
    }
    util::Optional<Mixed> sum(size_t* = nullptr) const override
    {
        REALM_TERMINATE("not implemented");
    }
    util::Optional<Mixed> avg(size_t* = nullptr) const override
    {
        REALM_TERMINATE("not implemented");
    }

private:
    std::shared_ptr<Dictionary> m_dictionary;
};
} // anonymous namespace

namespace object_store {

bool Dictionary::operator==(const Dictionary& rgt) const noexcept
{
    return dict() == rgt.dict();
}

Obj Dictionary::insert_embedded(StringData key)
{
    return dict().create_and_insert_linked_object(key);
}

void Dictionary::erase(StringData key)
{
    verify_in_transaction();
    dict().erase(key);
}

void Dictionary::remove_all()
{
    verify_in_transaction();
    dict().clear();
}

Obj Dictionary::get_object(StringData key)
{
    return dict().get_object(key);
}

Mixed Dictionary::get_any(StringData key)
{
    return dict().get(key);
}

Mixed Dictionary::get_any(size_t ndx) const
{
    verify_valid_row(ndx);
    return dict().get_any(ndx);
}

util::Optional<Mixed> Dictionary::try_get_any(StringData key) const
{
    return dict().try_get(key);
}

std::pair<StringData, Mixed> Dictionary::get_pair(size_t ndx) const
{
    verify_valid_row(ndx);
    auto pair = dict().get_pair(ndx);
    return {pair.first.get_string(), pair.second};
}

size_t Dictionary::find_any(Mixed value) const
{
    return dict().find_any(value);
}

bool Dictionary::contains(StringData key)
{
    return dict().contains(key);
}

Obj Dictionary::get_object(StringData key) const
{
    auto k = dict().get(key).get<ObjKey>();
    return dict().get_target_table()->get_object(k);
}

Results Dictionary::snapshot() const
{
    return as_results().snapshot();
}

Results Dictionary::get_keys() const
{
    verify_attached();
    return Results(m_realm,
                   std::make_shared<DictionaryKeyAdapter>(std::dynamic_pointer_cast<realm::Dictionary>(m_coll_base)));
}

Results Dictionary::get_values() const
{
    return as_results();
}

Dictionary::Iterator Dictionary::begin() const
{
    return dict().begin();
}

Dictionary::Iterator Dictionary::end() const
{
    return dict().end();
}

class NotificationHandler {
public:
    NotificationHandler(realm::Dictionary& dict, Dictionary::CBFunc cb)
        : m_dict(dict)
        , m_cb(cb)
    {
    }
    void before(CollectionChangeSet const&) {}
    void after(CollectionChangeSet const& c)
    {
        DictionaryChangeSet changes;
        for (auto ndx : c.deletions.as_indexes()) {
            changes.deletions.push_back(ndx);
        }
        for (auto ndx : c.insertions.as_indexes()) {
            changes.insertions.push_back(m_dict.get_key(ndx));
        }
        for (auto ndx : c.modifications_new.as_indexes()) {
            changes.modifications.push_back(m_dict.get_key(ndx));
        }
        m_cb(changes, {});
    }
    void error(std::exception_ptr ptr)
    {
        m_cb({}, ptr);
    }

private:
    realm::Dictionary& m_dict;
    Dictionary::CBFunc m_cb;
};

NotificationToken Dictionary::add_key_based_notification_callback(CBFunc cb) &
{
    return add_notification_callback(NotificationHandler(dict(), cb));
}

Dictionary Dictionary::freeze(const std::shared_ptr<Realm>& frozen_realm) const
{
    return Dictionary(frozen_realm, frozen_realm->import_copy_of(*m_coll_base));
}

} // namespace object_store
} // namespace realm
