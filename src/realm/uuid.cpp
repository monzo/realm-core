/*************************************************************************
 *
 * Copyright 2020 Realm Inc.
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

#include <realm/uuid.hpp>
#include <realm/string_data.hpp>
#include <realm/util/assert.hpp>
#include <atomic>
#include <cctype>

namespace realm {

constexpr char hex_digits[] = "0123456789abcdef";
constexpr size_t size_of_uuid_string = 36;
constexpr UUID::UUIDBytes null_uuid = {};
constexpr char null_uuid_string[] = "00000000-0000-0000-0000-000000000000";
static_assert(sizeof(UUID) == 16, "A change to the size of UUID is a file format breaking change");
static_assert(sizeof(null_uuid_string) - 1 == (sizeof(UUID) * 2) + 4,
              "size mismatch on uuid content and it's string representation");
static_assert(sizeof(null_uuid_string) - 1 == size_of_uuid_string,
              "size mismatch on uuid content and it's string representation");
constexpr size_t hyphen_pos_0 = 8;
constexpr size_t hyphen_pos_1 = 13;
constexpr size_t hyphen_pos_2 = 18;
constexpr size_t hyphen_pos_3 = 23;
constexpr char hyphen = '-';


bool UUID::is_valid_string(StringData str) noexcept
{
    if (str.size() != size_of_uuid_string) {
        return false;
    }
    if (str[hyphen_pos_0] != hyphen || str[hyphen_pos_1] != hyphen || str[hyphen_pos_2] != hyphen ||
        str[hyphen_pos_3] != hyphen) {
        return false;
    }
    for (size_t i = 0; i < size_of_uuid_string; ++i) {
        if (i == hyphen_pos_0 || i == hyphen_pos_1 || i == hyphen_pos_2 || i == hyphen_pos_3) {
            ++i;
        }
        if (!std::isxdigit(str[i])) {
            return false;
        }
    }
    return true;
}

UUID::UUID(const char* init)
{
    char buf[3] = {};
    if (!is_valid_string(init)) {
        throw InvalidUUIDString{
            util::format("Invalid string format encountered when constructing a UUID: '%1'.", init)};
    }

    size_t j = 0;
    for (size_t i = 0; i < sizeof(m_bytes); i++) {
        buf[0] = init[j++];
        buf[1] = init[j++];
        if (j == hyphen_pos_0 || j == hyphen_pos_1 || j == hyphen_pos_2 || j == hyphen_pos_3) {
            j++;
        }
        m_bytes[i] = char(strtol(buf, nullptr, 16));
    }
}

UUID::UUID(const StringData& init)
    : UUID(init.data())
{
}

UUID::UUID() noexcept
    : m_bytes{}
{
}

bool UUID::is_null() const
{
    return m_bytes == null_uuid;
}

std::string UUID::to_string() const
{
    std::string ret(null_uuid_string);
    REALM_ASSERT_DEBUG(ret.size() == (sizeof(m_bytes) * 2) + 4);
    size_t mod_ndx = 0;
    for (size_t i = 0; i < sizeof(m_bytes); i++) {
        ret[mod_ndx++] = hex_digits[m_bytes[i] >> 4];
        ret[mod_ndx++] = hex_digits[m_bytes[i] & 0xf];
        if (mod_ndx == hyphen_pos_0 || mod_ndx == hyphen_pos_1 || mod_ndx == hyphen_pos_2 ||
            mod_ndx == hyphen_pos_3) {
            ++mod_ndx;
        }
    }
    return ret;
}

size_t UUID::hash() const noexcept
{
    return murmur2_or_cityhash(m_bytes.data(), sizeof(m_bytes));
}

} // namespace realm
