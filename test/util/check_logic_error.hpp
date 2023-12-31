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

#ifndef REALM_TEST_UTIL_CHECK_LOGIC_ERROR_HPP
#define REALM_TEST_UTIL_CHECK_LOGIC_ERROR_HPP

#include <realm/exceptions.hpp>

#include "unit_test.hpp"

#define CHECK_LOGIC_ERROR(expr, error_kind) CHECK_THROW_EX(expr, realm::LogicError, e.code() == error_kind)

#define CHECK_RUNTIME_ERROR(expr, error_kind) CHECK_THROW_EX(expr, realm::RuntimeError, e.code() == error_kind)

#endif // REALM_TEST_UTIL_CHECK_LOGIC_ERROR_HPP
