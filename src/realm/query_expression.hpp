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

/*
This file lets you write queries in C++ syntax like: Expression* e = (first + 1 / second >= third + 12.3);

Type conversion/promotion semantics is the same as in the C++ expressions, e.g float + int > double == float +
(float)int > double.


Grammar:
-----------------------------------------------------------------------------------------------------------------------
    Expression:         Subexpr2<T>  Compare<Cond, T>  Subexpr2<T>
                        operator! Expression

    Subexpr2<T>:        Value<T>
                        Columns<T>
                        Subexpr2<T>  Operator<Oper<T>  Subexpr2<T>
                        power(Subexpr2<T>) // power(x) = x * x, as example of unary operator

    Value<T>:           T

    Operator<Oper<T>>:  +, -, *, /

    Compare<Cond, T>:   ==, !=, >=, <=, >, <

    T:                  bool, int, int64_t, float, double, StringData


Class diagram
-----------------------------------------------------------------------------------------------------------------------
Subexpr2
    void evaluate(size_t i, ValueBase* destination)

Compare: public Subexpr2
    size_t find_first(size_t start, size_t end)     // main method that executes query

    unique_ptr<Subexpr2> m_left;                               // left expression subtree
    unique_ptr<Subexpr2> m_right;                              // right expression subtree

Operator: public Subexpr2
    void evaluate(size_t i, ValueBase* destination)
    unique_ptr<Subexpr2> m_left;                               // left expression subtree
    unique_ptr<Subexpr2> m_right;                              // right expression subtree

Value<T>: public Subexpr2
    void evaluate(size_t i, ValueBase* destination)
    T m_v[8];

Columns<T>: public Subexpr2
    void evaluate(size_t i, ValueBase* destination)
    SequentialGetter<T> sg;                         // class bound to a column, lets you read values in a fast way
    Table* m_table;

class ColumnAccessor<>: public Columns<double>


Call diagram:
-----------------------------------------------------------------------------------------------------------------------
Example of 'table.first > 34.6 + table.second':

size_t Compare<Greater>::find_first()-------------+
         |                                        |
         |                                        |
         |                                        |
         +--> Columns<float>::evaluate()          +--------> Operator<Plus>::evaluate()
                                                                |               |
                                               Value<float>::evaluate()    Columns<float>::evaluate()

Operator, Value and Columns have an evaluate(size_t i, ValueBase* destination) method which returns a Value<T>
containing 8 values representing table rows i...i + 7.

So Value<T> contains 8 concecutive values and all operations are based on these chunks. This is
to save overhead by virtual calls needed for evaluating a query that has been dynamically constructed at runtime.


Memory allocation:
-----------------------------------------------------------------------------------------------------------------------
Subexpressions created by the end-user are stack allocated. They are cloned to the heap when passed to UnaryOperator,
Operator, and Compare. Those types own the clones and deallocate them when destroyed.


Caveats, notes and todos
-----------------------------------------------------------------------------------------------------------------------
    * Perhaps disallow columns from two different tables in same expression
    * The name Columns (with s) an be confusing because we also have Column (without s)
    * We have Columns::m_table, Query::m_table and ColumnAccessorBase::m_table that point at the same thing, even with
      ColumnAccessor<> extending Columns. So m_table is redundant, but this is in order to keep class dependencies and
      entanglement low so that the design is flexible (if you perhaps later want a Columns class that is not dependent
      on ColumnAccessor)

Nulls
-----------------------------------------------------------------------------------------------------------------------
First note that at array level, nulls are distinguished between non-null in different ways:
String:
    m_data == 0 && m_size == 0

Integer, Bool stored in ArrayIntNull:
    value == get(0) (entry 0 determins a magic value that represents nulls)

Float/double:
    null::is_null(value) which tests if value bit-matches one specific bit pattern reserved for null

The Columns class encapsulates all this into a simple class that, for any type T has
    evaluate(size_t index) that reads values from a column, taking nulls in count
    get(index)
    set(index)
    is_null(index)
    set_null(index)
*/

#ifndef REALM_QUERY_EXPRESSION_HPP
#define REALM_QUERY_EXPRESSION_HPP

#include <realm/array_timestamp.hpp>
#include <realm/array_binary.hpp>
#include <realm/array_string.hpp>
#include <realm/array_backlink.hpp>
#include <realm/array_list.hpp>
#include <realm/array_key.hpp>
#include <realm/array_bool.hpp>
#include <realm/column_type_traits.hpp>
#include <realm/list.hpp>
#include <realm/link_view.hpp>
#include <realm/metrics/query_info.hpp>
#include <realm/util/optional.hpp>
#include <realm/util/serializer.hpp>

#include <numeric>

// Normally, if a next-generation-syntax condition is supported by the old query_engine.hpp, a query_engine node is
// created because it's faster (by a factor of 5 - 10). Because many of our existing next-generation-syntax unit
// unit tests are indeed simple enough to fallback to old query_engine, query_expression gets low test coverage. Undef
// flag to get higher query_expression test coverage. This is a good idea to try out each time you develop on/modify
// query_expression.

#define REALM_OLDQUERY_FALLBACK

namespace realm {

template <class T>
T minimum(T a, T b)
{
    return a < b ? a : b;
}

#ifdef REALM_OLDQUERY_FALLBACK
// Hack to avoid template instantiation errors. See create(). Todo, see if we can simplify only_numeric somehow
namespace _impl {

template <class T, class U>
inline T only_numeric(U in)
{
    return static_cast<T>(util::unwrap(in));
}

template <class T>
inline int only_numeric(const StringData&)
{
    REALM_ASSERT(false);
    return 0;
}

template <class T>
inline int only_numeric(const BinaryData&)
{
    REALM_ASSERT(false);
    return 0;
}

template <class T>
inline StringData only_string(T in)
{
    REALM_ASSERT(false);
    static_cast<void>(in);
    return StringData();
}

inline StringData only_string(StringData in)
{
    return in;
}

template <class T, class U>
inline T no_timestamp(U in)
{
    return static_cast<T>(util::unwrap(in));
}

template <class T>
inline int no_timestamp(const Timestamp&)
{
    REALM_ASSERT(false);
    return 0;
}

} // namespace _impl

#endif // REALM_OLDQUERY_FALLBACK

template <class T>
struct Plus {
    T operator()(T v1, T v2) const
    {
        return v1 + v2;
    }
    static std::string description()
    {
        return "plus";
    }
    typedef T type;
};

template <class T>
struct Minus {
    T operator()(T v1, T v2) const
    {
        return v1 - v2;
    }
    static std::string description()
    {
        return "minus";
    }
    typedef T type;
};

template <class T>
struct Div {
    T operator()(T v1, T v2) const
    {
        return v1 / v2;
    }
    static std::string description()
    {
        return "divided by";
    }
    typedef T type;
};

template <class T>
struct Mul {
    T operator()(T v1, T v2) const
    {
        return v1 * v2;
    }
    static std::string description()
    {
        return "multiplied by";
    }
    typedef T type;
};

// Unary operator
template <class T>
struct Pow {
    T operator()(T v) const
    {
        return v * v;
    }
    static std::string description()
    {
        return "to the power of";
    }
    typedef T type;
};

// Finds a common type for T1 and T2 according to C++ conversion/promotion in arithmetic (float + int => float, etc)
template <class T1, class T2, bool T1_is_int = std::numeric_limits<T1>::is_integer || std::is_same<T1, null>::value,
          bool T2_is_int = std::numeric_limits<T2>::is_integer || std::is_same<T2, null>::value,
          bool T1_is_widest = (sizeof(T1) > sizeof(T2) || std::is_same<T2, null>::value)>
struct Common;
template <class T1, class T2, bool b>
struct Common<T1, T2, b, b, true> {
    typedef T1 type;
};
template <class T1, class T2, bool b>
struct Common<T1, T2, b, b, false> {
    typedef T2 type;
};
template <class T1, class T2, bool b>
struct Common<T1, T2, false, true, b> {
    typedef T1 type;
};
template <class T1, class T2, bool b>
struct Common<T1, T2, true, false, b> {
    typedef T2 type;
};


struct ValueBase {
    static const size_t default_size = 8;
    virtual void export_bool(ValueBase& destination) const = 0;
    virtual void export_Timestamp(ValueBase& destination) const = 0;
    virtual void export_int(ValueBase& destination) const = 0;
    virtual void export_float(ValueBase& destination) const = 0;
    virtual void export_int64_t(ValueBase& destination) const = 0;
    virtual void export_double(ValueBase& destination) const = 0;
    virtual void export_StringData(ValueBase& destination) const = 0;
    virtual void export_BinaryData(ValueBase& destination) const = 0;
    virtual void export_null(ValueBase& destination) const = 0;
    virtual void import(const ValueBase& destination) = 0;

    // If true, all values in the class come from a link list of a single field in the parent table (m_table). If
    // false, then values come from successive rows of m_table (query operations are operated on in bulks for speed)
    bool m_from_link_list;

    // Number of values stored in the class.
    size_t m_values;
};

class Expression {
public:
    Expression()
    {
    }
    virtual ~Expression()
    {
    }

    virtual size_t find_first(size_t start, size_t end) const = 0;
    virtual void set_base_table(const Table* table) = 0;
    virtual void set_cluster(const Cluster*) = 0;
    virtual void update_column() const = 0;
    virtual const Table* get_base_table() const = 0;
    virtual std::string description() const = 0;

    virtual std::unique_ptr<Expression> clone(QueryNodeHandoverPatches*) const = 0;
    virtual void apply_handover_patch(QueryNodeHandoverPatches&, Group&)
    {
    }
};

template <typename T, typename... Args>
std::unique_ptr<Expression> make_expression(Args&&... args)
{
    return std::unique_ptr<Expression>(new T(std::forward<Args>(args)...));
}

class Subexpr {
public:
    virtual ~Subexpr()
    {
    }

    virtual std::unique_ptr<Subexpr> clone(QueryNodeHandoverPatches* = nullptr) const = 0;
    virtual void apply_handover_patch(QueryNodeHandoverPatches&, Group&)
    {
    }

    // When the user constructs a query, it always "belongs" to one single base/parent table (regardless of
    // any links or not and regardless of any queries assembled with || or &&). When you do a Query::find(),
    // then Query::m_table is set to this table, and set_base_table() is called on all Columns and LinkMaps in
    // the query expression tree so that they can set/update their internals as required.
    //
    // During thread-handover of a Query, set_base_table() is also called to make objects point at the new table
    // instead of the old one from the old thread.
    virtual void set_base_table(const Table*)
    {
    }

    virtual void update_column() const = 0;
    virtual std::string description() const = 0;

    virtual void set_cluster(const Cluster*)
    {
    }

    // Recursively fetch tables of columns in expression tree. Used when user first builds a stand-alone expression
    // and
    // binds it to a Query at a later time
    virtual const Table* get_base_table() const
    {
        return nullptr;
    }

    virtual void evaluate(size_t index, ValueBase& destination) = 0;
    // This function supports SubColumnAggregate
    virtual void evaluate(Key, ValueBase&)
    {
        REALM_ASSERT(false); // Unimplemented
    }
};

template <typename T, typename... Args>
std::unique_ptr<Subexpr> make_subexpr(Args&&... args)
{
    return std::unique_ptr<Subexpr>(new T(std::forward<Args>(args)...));
}

template <class T>
class Columns;
template <class T>
class Value;
class ConstantStringValue;
template <class T>
class Subexpr2;
template <class oper, class TLeft = Subexpr, class TRight = Subexpr>
class Operator;
template <class oper, class TLeft = Subexpr>
class UnaryOperator;
template <class oper, class TLeft = Subexpr>
class SizeOperator;
template <class TCond, class T, class TLeft = Subexpr, class TRight = Subexpr>
class Compare;
template <bool has_links>
class UnaryLinkCompare;
class ColumnAccessorBase;


// Handle cases where left side is a constant (int, float, int64_t, double, StringData)
template <class Cond, class L, class R>
Query create(L left, const Subexpr2<R>& right)
{
// Purpose of below code is to intercept the creation of a condition and test if it's supported by the old
// query_engine.hpp which is faster. If it's supported, create a query_engine.hpp node, otherwise create a
// query_expression.hpp node.
//
// This method intercepts only Value <cond> Subexpr2. Interception of Subexpr2 <cond> Subexpr is elsewhere.

#ifdef REALM_OLDQUERY_FALLBACK // if not defined, then never fallback to query_engine.hpp; always use query_expression
    const Columns<R>* column = dynamic_cast<const Columns<R>*>(&right);
    // TODO: recognize size operator expressions
    // auto size_operator = dynamic_cast<const SizeOperator<Size<StringData>, Subexpr>*>(&right);

    if (column && ((std::numeric_limits<L>::is_integer && std::numeric_limits<R>::is_integer) ||
                   (std::is_same<L, double>::value && std::is_same<R, double>::value) ||
                   (std::is_same<L, float>::value && std::is_same<R, float>::value) ||
                   (std::is_same<L, Timestamp>::value && std::is_same<R, Timestamp>::value) ||
                   (std::is_same<L, StringData>::value && std::is_same<R, StringData>::value) ||
                   (std::is_same<L, BinaryData>::value && std::is_same<R, BinaryData>::value)) &&
        !column->links_exist()) {
        const Table* t = column->get_base_table();
        Query q = Query(*t);

        if (std::is_same<Cond, Less>::value)
            q.greater(column->column_ndx(), _impl::only_numeric<R>(left));
        else if (std::is_same<Cond, Greater>::value)
            q.less(column->column_ndx(), _impl::only_numeric<R>(left));
        else if (std::is_same<Cond, Equal>::value)
            q.equal(column->column_ndx(), left);
        else if (std::is_same<Cond, NotEqual>::value)
            q.not_equal(column->column_ndx(), left);
        else if (std::is_same<Cond, LessEqual>::value)
            q.greater_equal(column->column_ndx(), _impl::only_numeric<R>(left));
        else if (std::is_same<Cond, GreaterEqual>::value)
            q.less_equal(column->column_ndx(), _impl::only_numeric<R>(left));
        else if (std::is_same<Cond, EqualIns>::value)
            q.equal(column->column_ndx(), _impl::only_string(left), false);
        else if (std::is_same<Cond, NotEqualIns>::value)
            q.not_equal(column->column_ndx(), _impl::only_string(left), false);
        else if (std::is_same<Cond, BeginsWith>::value)
            q.begins_with(column->column_ndx(), _impl::only_string(left));
        else if (std::is_same<Cond, BeginsWithIns>::value)
            q.begins_with(column->column_ndx(), _impl::only_string(left), false);
        else if (std::is_same<Cond, EndsWith>::value)
            q.ends_with(column->column_ndx(), _impl::only_string(left));
        else if (std::is_same<Cond, EndsWithIns>::value)
            q.ends_with(column->column_ndx(), _impl::only_string(left), false);
        else if (std::is_same<Cond, Contains>::value)
            q.contains(column->column_ndx(), _impl::only_string(left));
        else if (std::is_same<Cond, ContainsIns>::value)
            q.contains(column->column_ndx(), _impl::only_string(left), false);
        else if (std::is_same<Cond, Like>::value)
            q.like(column->column_ndx(), _impl::only_string(left));
        else if (std::is_same<Cond, LikeIns>::value)
            q.like(column->column_ndx(), _impl::only_string(left), false);
        else {
            // query_engine.hpp does not support this Cond. Please either add support for it in query_engine.hpp or
            // fallback to using use 'return new Compare<>' instead.
            REALM_ASSERT(false);
        }
        // Return query_engine.hpp node
        return q;
    }
    else
#endif
    {
        // Return query_expression.hpp node
        using CommonType = typename Common<L, R>::type;
        using ValueType =
            typename std::conditional<std::is_same<L, StringData>::value, ConstantStringValue, Value<L>>::type;
        return make_expression<Compare<Cond, CommonType>>(make_subexpr<ValueType>(left), right.clone());
    }
}


// All overloads where left-hand-side is Subexpr2<L>:
//
// left-hand-side       operator                              right-hand-side
// Subexpr2<L>          +, -, *, /, <, >, ==, !=, <=, >=      R, Subexpr2<R>
//
// For L = R = {int, int64_t, float, double, StringData, Timestamp}:
template <class L, class R>
class Overloads {
    typedef typename Common<L, R>::type CommonType;

    std::unique_ptr<Subexpr> clone_subexpr() const
    {
        return static_cast<const Subexpr2<L>&>(*this).clone();
    }

public:
    // Arithmetic, right side constant
    Operator<Plus<CommonType>> operator+(R right) const
    {
        return {clone_subexpr(), make_subexpr<Value<R>>(right)};
    }
    Operator<Minus<CommonType>> operator-(R right) const
    {
        return {clone_subexpr(), make_subexpr<Value<R>>(right)};
    }
    Operator<Mul<CommonType>> operator*(R right) const
    {
        return {clone_subexpr(), make_subexpr<Value<R>>(right)};
    }
    Operator<Div<CommonType>> operator/(R right) const
    {
        return {clone_subexpr(), make_subexpr<Value<R>>(right)};
    }

    // Arithmetic, right side subexpression
    Operator<Plus<CommonType>> operator+(const Subexpr2<R>& right) const
    {
        return {clone_subexpr(), right.clone()};
    }
    Operator<Minus<CommonType>> operator-(const Subexpr2<R>& right) const
    {
        return {clone_subexpr(), right.clone()};
    }
    Operator<Mul<CommonType>> operator*(const Subexpr2<R>& right) const
    {
        return {clone_subexpr(), right.clone()};
    }
    Operator<Div<CommonType>> operator/(const Subexpr2<R>& right) const
    {
        return {clone_subexpr(), right.clone()};
    }

    // Compare, right side constant
    Query operator>(R right)
    {
        return create<Less>(right, static_cast<Subexpr2<L>&>(*this));
    }
    Query operator<(R right)
    {
        return create<Greater>(right, static_cast<Subexpr2<L>&>(*this));
    }
    Query operator>=(R right)
    {
        return create<LessEqual>(right, static_cast<Subexpr2<L>&>(*this));
    }
    Query operator<=(R right)
    {
        return create<GreaterEqual>(right, static_cast<Subexpr2<L>&>(*this));
    }
    Query operator==(R right)
    {
        return create<Equal>(right, static_cast<Subexpr2<L>&>(*this));
    }
    Query operator!=(R right)
    {
        return create<NotEqual>(right, static_cast<Subexpr2<L>&>(*this));
    }

    // Purpose of this method is to intercept the creation of a condition and test if it's supported by the old
    // query_engine.hpp which is faster. If it's supported, create a query_engine.hpp node, otherwise create a
    // query_expression.hpp node.
    //
    // This method intercepts Subexpr2 <cond> Subexpr2 only. Value <cond> Subexpr2 is intercepted elsewhere.
    template <class Cond>
    Query create2(const Subexpr2<R>& right)
    {
#ifdef REALM_OLDQUERY_FALLBACK // if not defined, never fallback query_engine; always use query_expression
        // Test if expressions are of type Columns. Other possibilities are Value and Operator.
        const Columns<R>* left_col = dynamic_cast<const Columns<R>*>(static_cast<Subexpr2<L>*>(this));
        const Columns<R>* right_col = dynamic_cast<const Columns<R>*>(&right);

        // query_engine supports 'T-column <op> <T-column>' for T = {int64_t, float, double}, op = {<, >, ==, !=, <=,
        // >=},
        // but only if both columns are non-nullable, and aren't in linked tables.
        if (left_col && right_col && std::is_same<L, R>::value && !left_col->is_nullable() &&
            !right_col->is_nullable() && !left_col->links_exist() && !right_col->links_exist() &&
            !std::is_same<L, Timestamp>::value) {
            const Table* t = left_col->get_base_table();
            Query q = Query(*t);

            if (std::numeric_limits<L>::is_integer) {
                if (std::is_same<Cond, Less>::value)
                    q.less_int(left_col->column_ndx(), right_col->column_ndx());
                else if (std::is_same<Cond, Greater>::value)
                    q.greater_int(left_col->column_ndx(), right_col->column_ndx());
                else if (std::is_same<Cond, Equal>::value)
                    q.equal_int(left_col->column_ndx(), right_col->column_ndx());
                else if (std::is_same<Cond, NotEqual>::value)
                    q.not_equal_int(left_col->column_ndx(), right_col->column_ndx());
                else if (std::is_same<Cond, LessEqual>::value)
                    q.less_equal_int(left_col->column_ndx(), right_col->column_ndx());
                else if (std::is_same<Cond, GreaterEqual>::value)
                    q.greater_equal_int(left_col->column_ndx(), right_col->column_ndx());
                else {
                    REALM_ASSERT(false);
                }
            }
            else if (std::is_same<L, float>::value) {
                if (std::is_same<Cond, Less>::value)
                    q.less_float(left_col->column_ndx(), right_col->column_ndx());
                else if (std::is_same<Cond, Greater>::value)
                    q.greater_float(left_col->column_ndx(), right_col->column_ndx());
                else if (std::is_same<Cond, Equal>::value)
                    q.equal_float(left_col->column_ndx(), right_col->column_ndx());
                else if (std::is_same<Cond, NotEqual>::value)
                    q.not_equal_float(left_col->column_ndx(), right_col->column_ndx());
                else if (std::is_same<Cond, LessEqual>::value)
                    q.less_equal_float(left_col->column_ndx(), right_col->column_ndx());
                else if (std::is_same<Cond, GreaterEqual>::value)
                    q.greater_equal_float(left_col->column_ndx(), right_col->column_ndx());
                else {
                    REALM_ASSERT(false);
                }
            }
            else if (std::is_same<L, double>::value) {
                if (std::is_same<Cond, Less>::value)
                    q.less_double(left_col->column_ndx(), right_col->column_ndx());
                else if (std::is_same<Cond, Greater>::value)
                    q.greater_double(left_col->column_ndx(), right_col->column_ndx());
                else if (std::is_same<Cond, Equal>::value)
                    q.equal_double(left_col->column_ndx(), right_col->column_ndx());
                else if (std::is_same<Cond, NotEqual>::value)
                    q.not_equal_double(left_col->column_ndx(), right_col->column_ndx());
                else if (std::is_same<Cond, LessEqual>::value)
                    q.less_equal_double(left_col->column_ndx(), right_col->column_ndx());
                else if (std::is_same<Cond, GreaterEqual>::value)
                    q.greater_equal_double(left_col->column_ndx(), right_col->column_ndx());
                else {
                    REALM_ASSERT(false);
                }
            }
            else {
                REALM_ASSERT(false);
            }
            // Return query_engine.hpp node
            return q;
        }
        else
#endif
        {
            // Return query_expression.hpp node
            return make_expression<Compare<Cond, typename Common<L, R>::type>>(clone_subexpr(), right.clone());
        }
    }

    // Compare, right side subexpression
    Query operator==(const Subexpr2<R>& right)
    {
        return create2<Equal>(right);
    }
    Query operator!=(const Subexpr2<R>& right)
    {
        return create2<NotEqual>(right);
    }
    Query operator>(const Subexpr2<R>& right)
    {
        return create2<Greater>(right);
    }
    Query operator<(const Subexpr2<R>& right)
    {
        return create2<Less>(right);
    }
    Query operator>=(const Subexpr2<R>& right)
    {
        return create2<GreaterEqual>(right);
    }
    Query operator<=(const Subexpr2<R>& right)
    {
        return create2<LessEqual>(right);
    }
};

// With this wrapper class we can define just 20 overloads inside Overloads<L, R> instead of 5 * 20 = 100. Todo: We
// can
// consider if it's simpler/better to remove this class completely and just list all 100 overloads manually anyway.
template <class T>
class Subexpr2 : public Subexpr,
                 public Overloads<T, const char*>,
                 public Overloads<T, int>,
                 public Overloads<T, float>,
                 public Overloads<T, double>,
                 public Overloads<T, int64_t>,
                 public Overloads<T, StringData>,
                 public Overloads<T, bool>,
                 public Overloads<T, Timestamp>,
                 public Overloads<T, null> {
public:
    virtual ~Subexpr2()
    {
    }

#define RLM_U2(t, o) using Overloads<T, t>::operator o;
#define RLM_U(o)                                                                                                     \
    RLM_U2(int, o)                                                                                                   \
    RLM_U2(float, o)                                                                                                 \
    RLM_U2(double, o)                                                                                                \
    RLM_U2(int64_t, o)                                                                                               \
    RLM_U2(StringData, o) RLM_U2(bool, o) RLM_U2(Timestamp, o) RLM_U2(null, o)
    RLM_U(+) RLM_U(-) RLM_U(*) RLM_U(/) RLM_U(>) RLM_U(<) RLM_U(==) RLM_U(!=) RLM_U(>=) RLM_U(<=)
};

// Subexpr2<Link> only provides equality comparisons. Their implementations can be found later in this file.
template <>
class Subexpr2<Link> : public Subexpr {
};

template <>
class Subexpr2<StringData> : public Subexpr, public Overloads<StringData, StringData> {
public:
    Query equal(StringData sd, bool case_sensitive = true);
    Query equal(const Subexpr2<StringData>& col, bool case_sensitive = true);
    Query not_equal(StringData sd, bool case_sensitive = true);
    Query not_equal(const Subexpr2<StringData>& col, bool case_sensitive = true);
    Query begins_with(StringData sd, bool case_sensitive = true);
    Query begins_with(const Subexpr2<StringData>& col, bool case_sensitive = true);
    Query ends_with(StringData sd, bool case_sensitive = true);
    Query ends_with(const Subexpr2<StringData>& col, bool case_sensitive = true);
    Query contains(StringData sd, bool case_sensitive = true);
    Query contains(const Subexpr2<StringData>& col, bool case_sensitive = true);
    Query like(StringData sd, bool case_sensitive = true);
    Query like(const Subexpr2<StringData>& col, bool case_sensitive = true);
};

/*
This class is used to store N values of type T = {int64_t, bool or StringData}, and allows an entry
to be null too. It's used by the Value class for internal storage.

To indicate nulls, we could have chosen a separate bool vector or some other bitmask construction. But for
performance, we customize indication of nulls to match the same indication that is used in the persisted database
file

Queries in query_expression.hpp execute by processing chunks of 8 rows at a time. Assume you have a column:

    price (int) = {1, 2, 3, null, 1, 6, 6, 9, 5, 2, null}

And perform a query:

    Query q = (price + 2 == 5);

query_expression.hpp will then create a NullableVector<int> = {5, 5, 5, 5, 5, 5, 5, 5} and then read values
NullableVector<int> = {1, 2, 3, null, 1, 6, 6, 9} from the column, and then perform `+` and `==` on these chunks.

See the top of this file for more information on all this.

Assume the user specifies the null constant in a query:

Query q = (price == null)

The query system will then construct a NullableVector of type `null` (NullableVector<null>). This allows compile
time optimizations for these cases.
*/

template <class T, size_t prealloc = 8>
struct NullableVector {
    using Underlying = typename util::RemoveOptional<T>::type;
    using t_storage =
        typename std::conditional<std::is_same<Underlying, bool>::value || std::is_same<Underlying, int>::value,
                                  int64_t, Underlying>::type;

    NullableVector()
    {
    }

    NullableVector& operator=(const NullableVector& other)
    {
        if (this != &other) {
            init(other.m_size);
            realm::safe_copy_n(other.m_first, other.m_size, m_first);
            m_null = other.m_null;
        }
        return *this;
    }

    NullableVector(const NullableVector& other)
    {
        init(other.m_size);
        realm::safe_copy_n(other.m_first, other.m_size, m_first);
        m_null = other.m_null;
    }

    ~NullableVector()
    {
        dealloc();
    }

    T operator[](size_t index) const
    {
        REALM_ASSERT_3(index, <, m_size);
        return static_cast<T>(m_first[index]);
    }

    inline bool is_null(size_t index) const
    {
        REALM_ASSERT((std::is_same<t_storage, int64_t>::value));
        return m_first[index] == m_null;
    }

    inline void set_null(size_t index)
    {
        REALM_ASSERT((std::is_same<t_storage, int64_t>::value));
        m_first[index] = m_null;
    }

    template <typename Type = t_storage>
    typename std::enable_if<std::is_same<Type, int64_t>::value, void>::type set(size_t index, t_storage value)
    {
        REALM_ASSERT((std::is_same<t_storage, int64_t>::value));

        // If value collides with magic null value, then switch to a new unique representation for null
        if (REALM_UNLIKELY(value == m_null)) {
            // adding a prime will generate 2^64 unique values. Todo: Only works on 2's complement architecture
            uint64_t candidate = static_cast<uint64_t>(m_null) + 0xfffffffbULL;
            while (std::find(m_first, m_first + m_size, static_cast<int64_t>(candidate)) != m_first + m_size)
                candidate += 0xfffffffbULL;
            std::replace(m_first, m_first + m_size, m_null, static_cast<int64_t>(candidate));
        }
        m_first[index] = value;
    }

    template <typename Type = T>
    typename std::enable_if<
        realm::is_any<Type, float, double, BinaryData, StringData, Key, Timestamp, ref_type, SizeOfList, null>::value,
        void>::type
    set(size_t index, t_storage value)
    {
        m_first[index] = value;
    }

    inline util::Optional<T> get(size_t index) const
    {
        if (is_null(index))
            return util::none;

        return util::make_optional((*this)[index]);
    }

    inline void set(size_t index, util::Optional<Underlying> value)
    {
        if (value) {
            Underlying v = *value;
            set(index, v);
        }
        else {
            set_null(index);
        }
    }

    void fill(T value)
    {
        for (size_t t = 0; t < m_size; t++) {
            if (std::is_same<T, null>::value)
                set_null(t);
            else
                set(t, value);
        }
    }

    void init(size_t size)
    {
        if (size == m_size)
            return;

        dealloc();
        m_size = size;
        if (m_size > 0) {
            if (m_size > prealloc)
                m_first = reinterpret_cast<t_storage*>(new t_storage[m_size]);
            else
                m_first = m_cache;
        }
    }

    void init(size_t size, T values)
    {
        init(size);
        fill(values);
    }

    void init(const std::vector<T>& values)
    {
        size_t sz = values.size();
        init(sz);
        for (size_t t = 0; t < sz; t++) {
            set(t, values[t]);
        }
    }

    void dealloc()
    {
        if (m_first) {
            if (m_size > prealloc)
                delete[] m_first;
            m_first = nullptr;
        }
    }

    t_storage m_cache[prealloc];
    t_storage* m_first = &m_cache[0];
    size_t m_size = 0;

    int64_t m_null = reinterpret_cast<int64_t>(&m_null); // choose magic value to represent nulls
};

// Double
// NOTE: fails in gcc 4.8 without `inline`. Do not remove. Same applies for all methods below.
template <>
inline bool NullableVector<double>::is_null(size_t index) const
{
    return null::is_null_float(m_first[index]);
}

template <>
inline void NullableVector<double>::set_null(size_t index)
{
    m_first[index] = null::get_null_float<double>();
}

// Float
template <>
inline bool NullableVector<float>::is_null(size_t index) const
{
    return null::is_null_float(m_first[index]);
}

template <>
inline void NullableVector<float>::set_null(size_t index)
{
    m_first[index] = null::get_null_float<float>();
}


// Null
template <>
inline void NullableVector<null>::set_null(size_t)
{
    return;
}
template <>
inline bool NullableVector<null>::is_null(size_t) const
{
    return true;
}

// StringData

template <>
inline bool NullableVector<StringData>::is_null(size_t index) const
{
    return m_first[index].is_null();
}

template <>
inline void NullableVector<StringData>::set_null(size_t index)
{
    m_first[index] = StringData();
}

// BinaryData

template <>
inline bool NullableVector<BinaryData>::is_null(size_t index) const
{
    return m_first[index].is_null();
}

template <>
inline void NullableVector<BinaryData>::set_null(size_t index)
{
    m_first[index] = BinaryData();
}

// Timestamp

template <>
inline bool NullableVector<Timestamp>::is_null(size_t index) const
{
    return m_first[index].is_null();
}

template <>
inline void NullableVector<Timestamp>::set_null(size_t index)
{
    m_first[index] = Timestamp{};
}

// ref_type
template <>
inline bool NullableVector<ref_type>::is_null(size_t index) const
{
    return m_first[index] == 0;
}
template <>
inline void NullableVector<ref_type>::set_null(size_t index)
{
    m_first[index] = 0;
}

// SizeOfList
template <>
inline bool NullableVector<SizeOfList>::is_null(size_t index) const
{
    return m_first[index].is_null();
}
template <>
inline void NullableVector<SizeOfList>::set_null(size_t index)
{
    m_first[index].set_null();
}

// Key
template <>
inline bool NullableVector<Key>::is_null(size_t index) const
{
    return m_first[index] == null_key;
}
template <>
inline void NullableVector<Key>::set_null(size_t index)
{
    m_first[index] = Key{};
}

template <typename Operator>
struct OperatorOptionalAdapter {
    template <typename L, typename R>
    util::Optional<typename Operator::type> operator()(const util::Optional<L>& left, const util::Optional<R>& right)
    {
        if (!left || !right)
            return util::none;
        return Operator()(*left, *right);
    }

    template <typename T>
    util::Optional<typename Operator::type> operator()(const util::Optional<T>& arg)
    {
        if (!arg)
            return util::none;
        return Operator()(*arg);
    }
};


struct TrueExpression : Expression {
    size_t find_first(size_t start, size_t end) const override
    {
        REALM_ASSERT(start <= end);
        if (start != end)
            return start;

        return realm::not_found;
    }
    void set_base_table(const Table*) override
    {
    }
    void set_cluster(const Cluster*) override
    {
    }
    const Table* get_base_table() const override
    {
        return nullptr;
    }
    void update_column() const override
    {
    }
    std::string description() const override
    {
        return "TRUEPREDICATE";
    }
    std::unique_ptr<Expression> clone(QueryNodeHandoverPatches*) const override
    {
        return std::unique_ptr<Expression>(new TrueExpression(*this));
    }
};


struct FalseExpression : Expression {
    size_t find_first(size_t, size_t) const override
    {
        return realm::not_found;
    }
    void set_base_table(const Table*) override
    {
    }
    void set_cluster(const Cluster*) override
    {
    }
    void update_column() const override
    {
    }
    std::string description() const override
    {
        return "FALSEPREDICATE";
    }
    const Table* get_base_table() const override
    {
        return nullptr;
    }
    std::unique_ptr<Expression> clone(QueryNodeHandoverPatches*) const override
    {
        return std::unique_ptr<Expression>(new FalseExpression(*this));
    }
};


// Stores N values of type T. Can also exchange data with other ValueBase of different types
template <class T>
class Value : public ValueBase, public Subexpr2<T> {
public:
    Value()
    {
        init(false, ValueBase::default_size, T());
    }
    Value(T v)
    {
        init(false, ValueBase::default_size, v);
    }

    Value(bool from_link_list, size_t values)
    {
        init(from_link_list, values, T());
    }

    Value(bool from_link_list, size_t values, T v)
    {
        init(from_link_list, values, v);
    }

    Value(const Value&) = default;
    Value& operator=(const Value&) = default;

    void init(bool from_link_list, size_t values, T v)
    {
        m_storage.init(values, v);
        ValueBase::m_from_link_list = from_link_list;
        ValueBase::m_values = values;
    }

    void init(bool from_link_list, size_t values)
    {
        m_storage.init(values);
        ValueBase::m_from_link_list = from_link_list;
        ValueBase::m_values = values;
    }

    void init(bool from_link_list, const std::vector<T>& values)
    {
        m_storage.init(values);
        ValueBase::m_from_link_list = from_link_list;
        ValueBase::m_values = values.size();
    }

    void update_column() const override
    {
    }

    virtual std::string description() const override
    {
        if (ValueBase::m_from_link_list) {
            return util::serializer::print_value(util::to_string(ValueBase::m_values) +
                                                 (ValueBase::m_values == 1 ? " value" : " values"));
        }
        if (m_storage.m_size > 0) {
            return util::serializer::print_value(m_storage[0]);
        }
        return "";
    }

    void evaluate(size_t, ValueBase& destination) override
    {
        destination.import(*this);
    }


    template <class TOperator>
    REALM_FORCEINLINE void fun(const Value* left, const Value* right)
    {
        OperatorOptionalAdapter<TOperator> o;

        if (!left->m_from_link_list && !right->m_from_link_list) {
            // Operate on values one-by-one (one value is one row; no links)
            size_t min = std::min(left->m_values, right->m_values);
            init(false, min);

            for (size_t i = 0; i < min; i++) {
                m_storage.set(i, o(left->m_storage.get(i), right->m_storage.get(i)));
            }
        }
        else if (left->m_from_link_list && right->m_from_link_list) {
            // FIXME: Many-to-many links not supported yet. Need to specify behaviour
            REALM_ASSERT_DEBUG(false);
        }
        else if (!left->m_from_link_list && right->m_from_link_list) {
            // Right values come from link. Left must come from single row.
            REALM_ASSERT_DEBUG(left->m_values > 0);
            init(true, right->m_values);

            auto left_value = left->m_storage.get(0);
            for (size_t i = 0; i < right->m_values; i++) {
                m_storage.set(i, o(left_value, right->m_storage.get(i)));
            }
        }
        else if (left->m_from_link_list && !right->m_from_link_list) {
            // Same as above, but with left values coming from links
            REALM_ASSERT_DEBUG(right->m_values > 0);
            init(true, left->m_values);

            auto right_value = right->m_storage.get(0);
            for (size_t i = 0; i < left->m_values; i++) {
                m_storage.set(i, o(left->m_storage.get(i), right_value));
            }
        }
    }

    template <class TOperator>
    REALM_FORCEINLINE void fun(const Value* value)
    {
        init(value->m_from_link_list, value->m_values);

        OperatorOptionalAdapter<TOperator> o;
        for (size_t i = 0; i < value->m_values; i++) {
            m_storage.set(i, o(value->m_storage.get(i)));
        }
    }


    // Below import and export methods are for type conversion between float, double, int64_t, etc.
    template <class D>
    typename std::enable_if<std::is_convertible<T, D>::value>::type REALM_FORCEINLINE
    export2(ValueBase& destination) const
    {
        Value<D>& d = static_cast<Value<D>&>(destination);
        d.init(ValueBase::m_from_link_list, ValueBase::m_values, D());
        for (size_t t = 0; t < ValueBase::m_values; t++) {
            if (m_storage.is_null(t))
                d.m_storage.set_null(t);
            else {
                d.m_storage.set(t, static_cast<D>(m_storage[t]));
            }
        }
    }

    template <class D>
    typename std::enable_if<!std::is_convertible<T, D>::value>::type REALM_FORCEINLINE export2(ValueBase&) const
    {
        // export2 is instantiated for impossible conversions like T=StringData, D=int64_t. These are never
        // performed at runtime but would result in a compiler error if we did not provide this implementation.
        REALM_ASSERT_DEBUG(false);
    }

    REALM_FORCEINLINE void export_Timestamp(ValueBase& destination) const override
    {
        export2<Timestamp>(destination);
    }

    REALM_FORCEINLINE void export_bool(ValueBase& destination) const override
    {
        export2<bool>(destination);
    }

    REALM_FORCEINLINE void export_int64_t(ValueBase& destination) const override
    {
        export2<int64_t>(destination);
    }

    REALM_FORCEINLINE void export_float(ValueBase& destination) const override
    {
        export2<float>(destination);
    }

    REALM_FORCEINLINE void export_int(ValueBase& destination) const override
    {
        export2<int>(destination);
    }

    REALM_FORCEINLINE void export_double(ValueBase& destination) const override
    {
        export2<double>(destination);
    }
    REALM_FORCEINLINE void export_StringData(ValueBase& destination) const override
    {
        export2<StringData>(destination);
    }
    REALM_FORCEINLINE void export_BinaryData(ValueBase& destination) const override
    {
        export2<BinaryData>(destination);
    }
    REALM_FORCEINLINE void export_null(ValueBase& destination) const override
    {
        Value<null>& d = static_cast<Value<null>&>(destination);
        d.init(m_from_link_list, m_values);
    }

    REALM_FORCEINLINE void import(const ValueBase& source) override
    {
        if (std::is_same<T, int>::value)
            source.export_int(*this);
        else if (std::is_same<T, Timestamp>::value)
            source.export_Timestamp(*this);
        else if (std::is_same<T, bool>::value)
            source.export_bool(*this);
        else if (std::is_same<T, float>::value)
            source.export_float(*this);
        else if (std::is_same<T, double>::value)
            source.export_double(*this);
        else if (std::is_same<T, int64_t>::value || std::is_same<T, bool>::value)
            source.export_int64_t(*this);
        else if (std::is_same<T, StringData>::value)
            source.export_StringData(*this);
        else if (std::is_same<T, BinaryData>::value)
            source.export_BinaryData(*this);
        else if (std::is_same<T, null>::value)
            source.export_null(*this);
        else
            REALM_ASSERT_DEBUG(false);
    }

    // Given a TCond (==, !=, >, <, >=, <=) and two Value<T>, return index of first match
    template <class TCond>
    REALM_FORCEINLINE static size_t compare(Value<T>* left, Value<T>* right)
    {
        TCond c;

        if (!left->m_from_link_list && !right->m_from_link_list) {
            // Compare values one-by-one (one value is one row; no link lists)
            size_t min = minimum(left->ValueBase::m_values, right->ValueBase::m_values);
            for (size_t m = 0; m < min; m++) {

                if (c(left->m_storage[m], right->m_storage[m], left->m_storage.is_null(m),
                      right->m_storage.is_null(m)))
                    return m;
            }
        }
        else if (left->m_from_link_list && right->m_from_link_list) {
            // FIXME: Many-to-many links not supported yet. Need to specify behaviour
            REALM_ASSERT_DEBUG(false);
        }
        else if (!left->m_from_link_list && right->m_from_link_list) {
            // Right values come from link list. Left must come from single row. Semantics: Match if at least 1
            // linked-to-value fulfills the condition
            REALM_ASSERT_DEBUG(left->m_values > 0);
            for (size_t r = 0; r < right->m_values; r++) {
                if (c(left->m_storage[0], right->m_storage[r], left->m_storage.is_null(0),
                      right->m_storage.is_null(r)))
                    return 0;
            }
        }
        else if (left->m_from_link_list && !right->m_from_link_list) {
            // Same as above, but with left values coming from link list.
            REALM_ASSERT_DEBUG(right->m_values > 0);
            for (size_t l = 0; l < left->m_values; l++) {
                if (c(left->m_storage[l], right->m_storage[0], left->m_storage.is_null(l),
                      right->m_storage.is_null(0)))
                    return 0;
            }
        }

        return not_found; // no match
    }

    std::unique_ptr<Subexpr> clone(QueryNodeHandoverPatches*) const override
    {
        return make_subexpr<Value<T>>(*this);
    }

    NullableVector<T> m_storage;
};

class ConstantStringValue : public Value<StringData> {
public:
    ConstantStringValue(const StringData& string)
        : Value()
        , m_string(string.is_null() ? util::none : util::make_optional(std::string(string)))
    {
        init(false, ValueBase::default_size, m_string);
    }

    std::unique_ptr<Subexpr> clone(QueryNodeHandoverPatches*) const override
    {
        return std::unique_ptr<Subexpr>(new ConstantStringValue(*this));
    }

private:
    ConstantStringValue(const ConstantStringValue& other)
        : Value()
        , m_string(other.m_string)
    {
        init(other.m_from_link_list, other.m_values, m_string);
    }

    util::Optional<std::string> m_string;
};

// All overloads where left-hand-side is L:
//
// left-hand-side       operator                              right-hand-side
// L                    +, -, *, /, <, >, ==, !=, <=, >=      Subexpr2<R>
//
// For L = R = {int, int64_t, float, double, Timestamp}:
// Compare numeric values
template <class R>
Query operator>(double left, const Subexpr2<R>& right)
{
    return create<Greater>(left, right);
}
template <class R>
Query operator>(float left, const Subexpr2<R>& right)
{
    return create<Greater>(left, right);
}
template <class R>
Query operator>(int left, const Subexpr2<R>& right)
{
    return create<Greater>(left, right);
}
template <class R>
Query operator>(int64_t left, const Subexpr2<R>& right)
{
    return create<Greater>(left, right);
}
template <class R>
Query operator>(Timestamp left, const Subexpr2<R>& right)
{
    return create<Greater>(left, right);
}

template <class R>
Query operator<(double left, const Subexpr2<R>& right)
{
    return create<Less>(left, right);
}
template <class R>
Query operator<(float left, const Subexpr2<R>& right)
{
    return create<Less>(left, right);
}
template <class R>
Query operator<(int left, const Subexpr2<R>& right)
{
    return create<Less>(left, right);
}
template <class R>
Query operator<(int64_t left, const Subexpr2<R>& right)
{
    return create<Less>(left, right);
}
template <class R>
Query operator<(Timestamp left, const Subexpr2<R>& right)
{
    return create<Less>(left, right);
}
template <class R>
Query operator==(double left, const Subexpr2<R>& right)
{
    return create<Equal>(left, right);
}
template <class R>
Query operator==(float left, const Subexpr2<R>& right)
{
    return create<Equal>(left, right);
}
template <class R>
Query operator==(int left, const Subexpr2<R>& right)
{
    return create<Equal>(left, right);
}
template <class R>
Query operator==(int64_t left, const Subexpr2<R>& right)
{
    return create<Equal>(left, right);
}
template <class R>
Query operator==(Timestamp left, const Subexpr2<R>& right)
{
    return create<Equal>(left, right);
}
template <class R>
Query operator>=(double left, const Subexpr2<R>& right)
{
    return create<GreaterEqual>(left, right);
}
template <class R>
Query operator>=(float left, const Subexpr2<R>& right)
{
    return create<GreaterEqual>(left, right);
}
template <class R>
Query operator>=(int left, const Subexpr2<R>& right)
{
    return create<GreaterEqual>(left, right);
}
template <class R>
Query operator>=(int64_t left, const Subexpr2<R>& right)
{
    return create<GreaterEqual>(left, right);
}
template <class R>
Query operator>=(Timestamp left, const Subexpr2<R>& right)
{
    return create<GreaterEqual>(left, right);
}
template <class R>
Query operator<=(double left, const Subexpr2<R>& right)
{
    return create<LessEqual>(left, right);
}
template <class R>
Query operator<=(float left, const Subexpr2<R>& right)
{
    return create<LessEqual>(left, right);
}
template <class R>
Query operator<=(int left, const Subexpr2<R>& right)
{
    return create<LessEqual>(left, right);
}
template <class R>
Query operator<=(int64_t left, const Subexpr2<R>& right)
{
    return create<LessEqual>(left, right);
}
template <class R>
Query operator<=(Timestamp left, const Subexpr2<R>& right)
{
    return create<LessEqual>(left, right);
}
template <class R>
Query operator!=(double left, const Subexpr2<R>& right)
{
    return create<NotEqual>(left, right);
}
template <class R>
Query operator!=(float left, const Subexpr2<R>& right)
{
    return create<NotEqual>(left, right);
}
template <class R>
Query operator!=(int left, const Subexpr2<R>& right)
{
    return create<NotEqual>(left, right);
}
template <class R>
Query operator!=(int64_t left, const Subexpr2<R>& right)
{
    return create<NotEqual>(left, right);
}
template <class R>
Query operator!=(Timestamp left, const Subexpr2<R>& right)
{
    return create<NotEqual>(left, right);
}

// Arithmetic
template <class R>
Operator<Plus<typename Common<R, double>::type>> operator+(double left, const Subexpr2<R>& right)
{
    return {make_subexpr<Value<double>>(left), right.clone()};
}
template <class R>
Operator<Plus<typename Common<R, float>::type>> operator+(float left, const Subexpr2<R>& right)
{
    return {make_subexpr<Value<float>>(left), right.clone()};
}
template <class R>
Operator<Plus<typename Common<R, int>::type>> operator+(int left, const Subexpr2<R>& right)
{
    return {make_subexpr<Value<int>>(left), right.clone()};
}
template <class R>
Operator<Plus<typename Common<R, int64_t>::type>> operator+(int64_t left, const Subexpr2<R>& right)
{
    return {make_subexpr<Value<int64_t>>(left), right.clone()};
}
template <class R>
Operator<Minus<typename Common<R, double>::type>> operator-(double left, const Subexpr2<R>& right)
{
    return {make_subexpr<Value<double>>(left), right.clone()};
}
template <class R>
Operator<Minus<typename Common<R, float>::type>> operator-(float left, const Subexpr2<R>& right)
{
    return {make_subexpr<Value<float>>(left), right.clone()};
}
template <class R>
Operator<Minus<typename Common<R, int>::type>> operator-(int left, const Subexpr2<R>& right)
{
    return {make_subexpr<Value<int>>(left), right.clone()};
}
template <class R>
Operator<Minus<typename Common<R, int64_t>::type>> operator-(int64_t left, const Subexpr2<R>& right)
{
    return {make_subexpr<Value<int64_t>>(left), right.clone()};
}
template <class R>
Operator<Mul<typename Common<R, double>::type>> operator*(double left, const Subexpr2<R>& right)
{
    return {make_subexpr<Value<double>>(left), right.clone()};
}
template <class R>
Operator<Mul<typename Common<R, float>::type>> operator*(float left, const Subexpr2<R>& right)
{
    return {make_subexpr<Value<float>>(left), right.clone()};
}
template <class R>
Operator<Mul<typename Common<R, int>::type>> operator*(int left, const Subexpr2<R>& right)
{
    return {make_subexpr<Value<int>>(left), right.clone()};
}
template <class R>
Operator<Mul<typename Common<R, int64_t>::type>> operator*(int64_t left, const Subexpr2<R>& right)
{
    return {make_subexpr<Value<int64_t>>(left), right.clone()};
}
template <class R>
Operator<Div<typename Common<R, double>::type>> operator/(double left, const Subexpr2<R>& right)
{
    return {make_subexpr<Value<double>>(left), right.clone()};
}
template <class R>
Operator<Div<typename Common<R, float>::type>> operator/(float left, const Subexpr2<R>& right)
{
    return {make_subexpr<Value<float>>(left), right.clone()};
}
template <class R>
Operator<Div<typename Common<R, int>::type>> operator/(int left, const Subexpr2<R>& right)
{
    return {make_subexpr<Value<int>>(left), right.clone()};
}
template <class R>
Operator<Div<typename Common<R, int64_t>::type>> operator/(int64_t left, const Subexpr2<R>& right)
{
    return {make_subexpr<Value<int64_t>>(left), right.clone()};
}

// Unary operators
template <class T>
UnaryOperator<Pow<T>> power(const Subexpr2<T>& left)
{
    return {left.clone()};
}

// Classes used for LinkMap (see below).
struct LinkMapFunction {
    // Your consume() method is given key within the linked-to table as argument, and you must return whether or
    // not you want the LinkMapFunction to exit (return false) or continue (return true) harvesting the link tree
    // for the current main table object (it will be a link tree if you have multiple type_LinkList columns
    // in a link()->link() query.
    virtual bool consume(Key) = 0;
};

struct FindNullLinks : public LinkMapFunction {
    bool consume(Key) override
    {
        m_has_link = true;
        return false; // we've found a key, so this can't be a null-link, so exit link harvesting
    }

    bool m_has_link = false;
};

struct MakeLinkVector : public LinkMapFunction {
    MakeLinkVector(std::vector<Key>& result)
        : m_links(result)
    {
    }

    bool consume(Key key) override
    {
        m_links.push_back(key);
        return true; // continue evaluation
    }
    std::vector<Key>& m_links;
};

struct CountLinks : public LinkMapFunction {
    bool consume(Key) override
    {
        m_link_count++;
        return true;
    }

    size_t result() const
    {
        return m_link_count;
    }

    size_t m_link_count = 0;
};


/*
The LinkMap and LinkMapFunction classes are used for query conditions on links themselves (contrary to conditions on
the value payload they point at).

MapLink::map_links() takes a row index of the link array as argument and follows any link chain stated in the query
(through the link()->link() methods) until the final payload table is reached, and then applies LinkMapFunction on
the linked-to key(s).

If all link columns are type_Link, then LinkMapFunction is only invoked for a single key. If one or more
columns are type_LinkList, then it may result in multiple keys.

The reason we use this map pattern is that we can exit the link-tree-traversal as early as possible, e.g. when we've
found the first link that points to key '5'. Other solutions could be a std::vector<size_t> harvest_all_links(), or an
iterator pattern. First solution can't exit, second solution requires internal state.
*/
class LinkMap {
public:
    LinkMap() = default;
    LinkMap(const Table* table, std::vector<size_t> columns)
        : m_link_column_indexes(std::move(columns))
    {
        set_base_table(table);
    }

    LinkMap(LinkMap const& other)
    {
        m_link_column_names = other.m_link_column_names;
        m_link_column_indexes = other.m_link_column_indexes;
        m_tables = other.m_tables;
        m_link_types = other.m_link_types;
        m_only_unary_links = other.m_only_unary_links;
    }

    LinkMap(LinkMap const& other, QueryNodeHandoverPatches*)
        : LinkMap(other)
    {
    }

    size_t get_nb_hops() const
    {
        return m_link_column_indexes.size();
    }

    bool has_links() const
    {
        return m_link_column_indexes.size() > 0;
    }

    void set_base_table(const Table* table);

    void set_cluster(const Cluster* cluster)
    {
        Allocator& alloc = m_tables.back()->get_alloc();
        m_array_ptr = nullptr;
        switch (m_link_types[0]) {
            case col_type_Link:
                m_array_ptr = LeafPtr(new (&m_storage.m_list) ArrayKey(alloc));
                break;
            case col_type_LinkList:
                m_array_ptr = LeafPtr(new (&m_storage.m_linklist) ArrayList(alloc));
                break;
            case col_type_BackLink:
                m_array_ptr = LeafPtr(new (&m_storage.m_backlink) ArrayBacklink(alloc));
                break;
            default:
                break;
        }
        cluster->init_leaf(m_link_column_indexes[0], m_array_ptr.get());
        m_leaf_ptr = m_array_ptr.get();
    }

    void update_columns() const
    {
        for (size_t i = 0; i < m_link_column_indexes.size(); i++) {
            m_link_column_indexes[i] = m_tables[i]->get_column_index(m_link_column_names[i]);
            if (m_link_column_indexes[i] == npos) {
                throw LogicError(LogicError::column_does_not_exist);
            }
        }
    }

    std::string description() const;

    std::vector<Key> get_links(size_t index)
    {
        std::vector<Key> res;
        get_links(index, res);
        return res;
    }

    size_t count_links(size_t row)
    {
        CountLinks counter;
        map_links(row, counter);
        return counter.result();
    }

    void map_links(size_t row, LinkMapFunction& lm)
    {
        map_links(0, row, lm);
    }

    bool only_unary_links() const
    {
        return m_only_unary_links;
    }

    ConstTableRef base_table() const
    {
        return m_tables.empty() ? ConstTableRef() : m_tables[0];
    }

    ConstTableRef target_table() const
    {
        REALM_ASSERT(!m_tables.empty());
        return m_tables.back();
    }

private:
    void map_links(size_t column, Key key, LinkMapFunction& lm);
    void map_links(size_t column, size_t row, LinkMapFunction& lm);

    void get_links(size_t row, std::vector<Key>& result)
    {
        MakeLinkVector mlv = MakeLinkVector(result);
        map_links(row, mlv);
    }

    mutable std::vector<size_t> m_link_column_indexes;
    std::vector<std::string> m_link_column_names;
    std::vector<ColumnType> m_link_types;
    std::vector<ConstTableRef> m_tables;
    bool m_only_unary_links = true;
    // Leaf cache
    using LeafPtr = std::unique_ptr<ArrayPayload, PlacementDelete>;
    union Storage {
        typename std::aligned_storage<sizeof(ArrayKey), alignof(ArrayKey)>::type m_list;
        typename std::aligned_storage<sizeof(ArrayList), alignof(ArrayList)>::type m_linklist;
        typename std::aligned_storage<sizeof(ArrayList), alignof(ArrayList)>::type m_backlink;
    };
    Storage m_storage;
    LeafPtr m_array_ptr;
    const ArrayPayload* m_leaf_ptr = nullptr;

    template <class>
    friend Query compare(const Subexpr2<Link>&, const ConstObj&);
};

template <class T, class S, class I>
Query string_compare(const Subexpr2<StringData>& left, T right, bool case_insensitive);
template <class S, class I>
Query string_compare(const Subexpr2<StringData>& left, const Subexpr2<StringData>& right, bool case_insensitive);

template <class T>
Value<T> make_value_for_link(bool only_unary_links, size_t size)
{
    Value<T> value;
    if (only_unary_links) {
        REALM_ASSERT(size <= 1);
        value.init(false, 1);
        value.m_storage.set_null(0);
    }
    else {
        value.init(true, size);
    }
    return value;
}


// If we add a new Realm type T and quickly want Query support for it, then simply inherit from it like
// `template <> class Columns<T> : public SimpleQuerySupport<T>` and you're done. Any operators of the set
// { ==, >=, <=, !=, >, < } that are supported by T will be supported by the "query expression syntax"
// automatically. NOTE: This method of Query support will be slow because it goes through Table::get<T>.
// To get faster Query support, either add SequentialGetter support (faster) or create a query_engine.hpp
// node for it (super fast).

template <class T>
class SimpleQuerySupport : public Subexpr2<T> {
public:
    SimpleQuerySupport(size_t column, const Table* table, std::vector<size_t> links = {})
        : m_link_map(table, std::move(links))
        , m_column_name(m_link_map.target_table()->get_column_name(column))
        , m_column_ndx(column)
    {
    }

    bool is_nullable() const noexcept
    {
        return m_link_map.base_table()->is_nullable(m_column_ndx);
    }

    const Table* get_base_table() const override
    {
        return m_link_map.base_table();
    }

    void set_base_table(const Table* table) override
    {
        if (table != get_base_table()) {
            m_link_map.set_base_table(table);
            m_column_name = m_link_map.target_table()->get_column_name(m_column_ndx);
        }
    }

    void set_cluster(const Cluster* cluster) override
    {
        m_array_ptr = nullptr;
        m_leaf_ptr = nullptr;
        if (links_exist()) {
            m_link_map.set_cluster(cluster);
        }
        else {
            // Create new Leaf
            m_array_ptr = LeafPtr(new (&m_leaf_cache_storage) LeafType(m_link_map.base_table()->get_alloc()));
            cluster->init_leaf(this->m_column_ndx, m_array_ptr.get());
            m_leaf_ptr = m_array_ptr.get();
        }
    }

    void update_column() const override
    {
        // update links
        m_link_map.update_columns();
        // update target column
        m_column_ndx = m_link_map.target_table()->get_column_index(m_column_name);
        if (m_column_ndx == realm::npos) {
            throw LogicError(LogicError::column_does_not_exist);
        }
    }

    void evaluate(size_t index, ValueBase& destination) override
    {
        Value<T>& d = static_cast<Value<T>&>(destination);

        if (links_exist()) {
            REALM_ASSERT(m_leaf_ptr == nullptr);
            std::vector<Key> links = m_link_map.get_links(index);
            Value<T> v = make_value_for_link<T>(m_link_map.only_unary_links(), links.size());

            for (size_t t = 0; t < links.size(); t++) {
                ConstObj obj = m_link_map.target_table()->get_object(links[t]);
                v.m_storage.set(t, obj.get<T>(m_column_ndx));
            }
            destination.import(v);
        }
        else {
            REALM_ASSERT(m_leaf_ptr != nullptr);
            // Not a link column
            for (size_t t = 0; t < destination.m_values && index + t < m_leaf_ptr->size(); t++) {
                d.m_storage.set(t, m_leaf_ptr->get(index + t));
            }
        }
    }

    void evaluate(Key key, ValueBase& destination) override
    {
        Value<T>& d = static_cast<Value<T>&>(destination);
        d.m_storage.set(0, m_link_map.target_table()->get_object(key).template get<T>(m_column_ndx));
    }

    bool links_exist() const
    {
        return m_link_map.has_links();
    }

    virtual std::string description() const override
    {
        std::string desc;
        if (links_exist()) {
            desc = m_link_map.description() + util::serializer::value_separator;
        }
        auto target_table = m_link_map.target_table();
        if (target_table) {
            desc += std::string(target_table->get_column_name(m_column_ndx));
        }
        return desc;
    }

    std::unique_ptr<Subexpr> clone(QueryNodeHandoverPatches* patches = nullptr) const override
    {
        return make_subexpr<Columns<T>>(static_cast<const Columns<T>&>(*this), patches);
    }

    SimpleQuerySupport(SimpleQuerySupport const& other, QueryNodeHandoverPatches* patches)
        : Subexpr2<T>(other)
        , m_link_map(other.m_link_map, patches)
        , m_column_name(other.m_column_name)
        , m_column_ndx(other.m_column_ndx)
    {
    }

    SimpleQuerySupport(SimpleQuerySupport const& other)
        : SimpleQuerySupport(other, nullptr)
    {
    }

    size_t column_ndx() const
    {
        return m_column_ndx;
    }

    SizeOperator<T> size()
    {
        return SizeOperator<T>(this->clone(nullptr));
    }

private:
    LinkMap m_link_map;

    // Column index of payload column of m_table
    std::string m_column_name;
    mutable size_t m_column_ndx;

    // Leaf cache
    using LeafType = typename ColumnTypeTraits<T>::cluster_leaf_type;
    using LeafCacheStorage = typename std::aligned_storage<sizeof(LeafType), alignof(LeafType)>::type;
    using LeafPtr = std::unique_ptr<LeafType, PlacementDelete>;
    LeafCacheStorage m_leaf_cache_storage;
    LeafPtr m_array_ptr;
    LeafType* m_leaf_ptr = nullptr;
};


template <>
class Columns<Timestamp> : public SimpleQuerySupport<Timestamp> {
    using SimpleQuerySupport::SimpleQuerySupport;
};

template <>
class Columns<BinaryData> : public SimpleQuerySupport<BinaryData> {
    using SimpleQuerySupport::SimpleQuerySupport;
};

template <>
class Columns<StringData> : public SimpleQuerySupport<StringData> {
public:
    Columns(size_t column, const Table* table, std::vector<size_t> links = {})
        : SimpleQuerySupport(column, table, links)
    {
    }

    Columns(Columns const& other, QueryNodeHandoverPatches* patches = nullptr)
        : SimpleQuerySupport(other, patches)
    {
    }

    Columns(Columns&& other)
        : SimpleQuerySupport(other)
    {
    }
};

template <class T, class S, class I>
Query string_compare(const Subexpr2<StringData>& left, T right, bool case_sensitive)
{
    StringData sd(right);
    if (case_sensitive)
        return create<S>(sd, left);
    else
        return create<I>(sd, left);
}

template <class S, class I>
Query string_compare(const Subexpr2<StringData>& left, const Subexpr2<StringData>& right, bool case_sensitive)
{
    if (case_sensitive)
        return make_expression<Compare<S, StringData>>(right.clone(), left.clone());
    else
        return make_expression<Compare<I, StringData>>(right.clone(), left.clone());
}

// Columns<String> == Columns<String>
inline Query operator==(const Columns<StringData>& left, const Columns<StringData>& right)
{
    return string_compare<Equal, EqualIns>(left, right, true);
}

// Columns<String> != Columns<String>
inline Query operator!=(const Columns<StringData>& left, const Columns<StringData>& right)
{
    return string_compare<NotEqual, NotEqualIns>(left, right, true);
}

// String == Columns<String>
template <class T>
Query operator==(T left, const Columns<StringData>& right)
{
    return operator==(right, left);
}

// String != Columns<String>
template <class T>
Query operator!=(T left, const Columns<StringData>& right)
{
    return operator!=(right, left);
}

// Columns<String> == String
template <class T>
Query operator==(const Columns<StringData>& left, T right)
{
    return string_compare<T, Equal, EqualIns>(left, right, true);
}

// Columns<String> != String
template <class T>
Query operator!=(const Columns<StringData>& left, T right)
{
    return string_compare<T, NotEqual, NotEqualIns>(left, right, true);
}


inline Query operator==(const Columns<BinaryData>& left, BinaryData right)
{
    return create<Equal>(right, left);
}

inline Query operator==(BinaryData left, const Columns<BinaryData>& right)
{
    return create<Equal>(left, right);
}

inline Query operator!=(const Columns<BinaryData>& left, BinaryData right)
{
    return create<NotEqual>(right, left);
}

inline Query operator!=(BinaryData left, const Columns<BinaryData>& right)
{
    return create<NotEqual>(left, right);
}


// This class is intended to perform queries on the *pointers* of links, contrary to performing queries on *payload*
// in linked-to tables. Queries can be "find first link that points at row X" or "find first null-link". Currently
// only "find first null link" and "find first non-null link" is supported. More will be added later. When we add
// more, I propose to remove the <bool has_links> template argument from this class and instead template it by
// a criteria-class (like the FindNullLinks class below in find_first()) in some generalized fashion.
template <bool has_links>
class UnaryLinkCompare : public Expression {
public:
    UnaryLinkCompare(LinkMap lm)
        : m_link_map(std::move(lm))
    {
    }

    void set_base_table(const Table* table) override
    {
        m_link_map.set_base_table(table);
    }

    void set_cluster(const Cluster* cluster) override
    {
        m_link_map.set_cluster(cluster);
    }

    void update_column() const override
    {
        m_link_map.update_columns();
    }

    // Return main table of query (table on which table->where()... is invoked). Note that this is not the same as
    // any linked-to payload tables
    const Table* get_base_table() const override
    {
        return m_link_map.base_table();
    }

    size_t find_first(size_t start, size_t end) const override
    {
        for (; start < end;) {
            FindNullLinks fnl;
            m_link_map.map_links(start, fnl);
            if (fnl.m_has_link == has_links)
                return start;

            start++;
        }

        return not_found;
    }

    virtual std::string description() const override
    {
        return m_link_map.description() + (has_links ? " != NULL" : " == NULL");
    }

    std::unique_ptr<Expression> clone(QueryNodeHandoverPatches* patches) const override
    {
        return std::unique_ptr<Expression>(new UnaryLinkCompare(*this, patches));
    }

private:
    UnaryLinkCompare(const UnaryLinkCompare& other, QueryNodeHandoverPatches* patches = nullptr)
        : Expression(other)
        , m_link_map(other.m_link_map, patches)
    {
    }

    mutable LinkMap m_link_map;
};

class LinkCount : public Subexpr2<Int> {
public:
    LinkCount(LinkMap link_map)
        : m_link_map(std::move(link_map))
    {
    }
    LinkCount(LinkCount const& other, QueryNodeHandoverPatches* patches)
        : Subexpr2<Int>(other)
        , m_link_map(other.m_link_map, patches)
    {
    }

    std::unique_ptr<Subexpr> clone(QueryNodeHandoverPatches* patches) const override
    {
        return make_subexpr<LinkCount>(*this, patches);
    }

    const Table* get_base_table() const override
    {
        return m_link_map.base_table();
    }

    void set_base_table(const Table* table) override
    {
        m_link_map.set_base_table(table);
    }

    void set_cluster(const Cluster* cluster) override
    {
        m_link_map.set_cluster(cluster);
    }

    void update_column() const override
    {
        m_link_map.update_columns();
    }

    void evaluate(size_t index, ValueBase& destination) override
    {
        size_t count = m_link_map.count_links(index);
        destination.import(Value<Int>(false, 1, count));
    }

    virtual std::string description() const override
    {
        return m_link_map.description() + util::serializer::value_separator + "@count";
    }

private:
    LinkMap m_link_map;
};

template <class T, class TExpr>
class SizeOperator : public Subexpr2<Int> {
public:
    SizeOperator(std::unique_ptr<TExpr> left)
        : m_expr(std::move(left))
    {
    }

    // See comment in base class
    void set_base_table(const Table* table) override
    {
        m_expr->set_base_table(table);
    }

    void set_cluster(const Cluster* cluster) override
    {
        m_expr->set_cluster(cluster);
    }

    void update_column() const override
    {
        m_expr->update_column();
    }

    // Recursively fetch tables of columns in expression tree. Used when user first builds a stand-alone expression
    // and binds it to a Query at a later time
    const Table* get_base_table() const override
    {
        return m_expr->get_base_table();
    }

    // destination = operator(left)
    void evaluate(size_t index, ValueBase& destination) override
    {
        REALM_ASSERT_DEBUG(dynamic_cast<Value<Int>*>(&destination) != nullptr);
        Value<Int>* d = static_cast<Value<Int>*>(&destination);
        REALM_ASSERT(d);

        Value<T> v;
        m_expr->evaluate(index, v);

        size_t sz = v.m_values;
        d->init(v.m_from_link_list, sz);

        for (size_t i = 0; i < sz; i++) {
            auto elem = v.m_storage.get(i);
            if (!elem) {
                d->m_storage.set_null(i);
            }
            else {
                d->m_storage.set(i, elem->size());
            }
        }
    }

    std::string description() const override
    {
        if (m_expr) {
            return m_expr->description() + util::serializer::value_separator + "@size";
        }
        return "@size";
    }

    std::unique_ptr<Subexpr> clone(QueryNodeHandoverPatches* patches) const override
    {
        return std::unique_ptr<Subexpr>(new SizeOperator(*this, patches));
    }

    void apply_handover_patch(QueryNodeHandoverPatches& patches, Group& group) override
    {
        m_expr->apply_handover_patch(patches, group);
    }

private:
    SizeOperator(const SizeOperator& other, QueryNodeHandoverPatches* patches)
        : m_expr(other.m_expr->clone(patches))
    {
    }

    std::unique_ptr<TExpr> m_expr;
};

class KeyValue : public Subexpr2<Link> {
public:
    KeyValue(Key key)
        : m_key(key)
    {
    }

    void set_base_table(const Table*) override
    {
    }

    void update_column() const override
    {
    }

    const Table* get_base_table() const override
    {
        return nullptr;
    }

    void evaluate(size_t, ValueBase& destination) override
    {
        // Destination must be of Key type. It only makes sense to
        // compare keys with keys
        auto d = dynamic_cast<Value<Key>*>(&destination);
        REALM_ASSERT(d != nullptr);
        d->init(false, 1, m_key);
    }

    virtual std::string description() const override
    {
        return util::serializer::print_value(m_key);
    }

    std::unique_ptr<Subexpr> clone(QueryNodeHandoverPatches*) const override
    {
        return std::unique_ptr<Subexpr>(new KeyValue(*this));
    }

private:
    KeyValue(const KeyValue& source)
        : m_key(source.m_key)
    {
    }

    Key m_key;
};

template <typename T>
class SubColumns;

// This is for LinkList and BackLink too since they're declared as typedefs of Link.
template <>
class Columns<Link> : public Subexpr2<Link> {
public:
    Query is_null()
    {
        if (m_link_map.get_nb_hops() > 1)
            throw std::runtime_error("Combining link() and is_null() is currently not supported");
        // Todo, it may be useful to support the above, but we would need to figure out an intuitive behaviour
        return make_expression<UnaryLinkCompare<false>>(m_link_map);
    }

    Query is_not_null()
    {
        if (m_link_map.get_nb_hops() > 1)
            throw std::runtime_error("Combining link() and is_not_null() is currently not supported");
        // Todo, it may be useful to support the above, but we would need to figure out an intuitive behaviour
        return make_expression<UnaryLinkCompare<true>>(m_link_map);
    }

    LinkCount count() const
    {
        return LinkCount(m_link_map);
    }

    template <typename C>
    SubColumns<C> column(size_t column_ndx) const
    {
        return SubColumns<C>(Columns<C>(column_ndx, m_link_map.target_table()), m_link_map);
    }

    const LinkMap& link_map() const
    {
        return m_link_map;
    }

    const Table* get_base_table() const override
    {
        return m_link_map.base_table();
    }

    void set_base_table(const Table* table) override
    {
        m_link_map.set_base_table(table);
    }

    void set_cluster(const Cluster* cluster) override
    {
        REALM_ASSERT(m_link_map.has_links());
        m_link_map.set_cluster(cluster);
    }

    void update_column() const override
    {
        m_link_map.update_columns();
    }

    std::string description() const override
    {
        return m_link_map.description();
    }

    std::unique_ptr<Subexpr> clone(QueryNodeHandoverPatches* patches) const override
    {
        return std::unique_ptr<Subexpr>(new Columns<Link>(*this, patches));
    }

    void evaluate(size_t index, ValueBase& destination) override;


private:
    LinkMap m_link_map;
    friend class Table;

    Columns(size_t column_ndx, const Table* table, const std::vector<size_t>& links = {})
        : m_link_map(table, links)
    {
        static_cast<void>(column_ndx);
    }
    Columns(const Columns& other, QueryNodeHandoverPatches* patches)
        : Subexpr2<Link>(other)
        , m_link_map(other.m_link_map, patches)
    {
    }
};

template <typename T>
class ListColumns;
template <typename T, typename Operation>
class ListColumnAggregate;
namespace aggregate_operations {
template <typename T>
class Minimum;
template <typename T>
class Maximum;
template <typename T>
class Sum;
template <typename T>
class Average;
}

class ColumnListBase {
public:
    ColumnListBase(size_t column_ndx, const Table* table, const std::vector<size_t>& links)
        : m_column_ndx(column_ndx)
        , m_link_map(table, links)
    {
    }

    ColumnListBase(const ColumnListBase& other, QueryNodeHandoverPatches* patches)
        : m_column_name(other.m_column_name)
        , m_column_ndx(other.m_column_ndx)
        , m_link_map(other.m_link_map, patches)
    {
    }

    ColumnListBase(const ColumnListBase& other)
        : ColumnListBase(other, nullptr)
    {
    }

    void set_cluster(const Cluster* cluster);

    void get_lists(size_t index, Value<ref_type>& destination, size_t nb_elements);

    bool links_exist() const
    {
        return m_link_map.has_links();
    }

    std::string m_column_name;
    mutable size_t m_column_ndx;
    LinkMap m_link_map;
    // Leaf cache
    using LeafCacheStorage = typename std::aligned_storage<sizeof(ArrayList), alignof(Array)>::type;
    using LeafPtr = std::unique_ptr<ArrayList, PlacementDelete>;
    LeafCacheStorage m_leaf_cache_storage;
    LeafPtr m_array_ptr;
    ArrayList* m_leaf_ptr = nullptr;
};

template <typename>
class ColumnListSize;

template <typename T>
class Columns<List<T>> : public Subexpr2<T>, public ColumnListBase {
public:
    Columns(const Columns<List<T>>& other, QueryNodeHandoverPatches* patches)
        : Subexpr2<T>(other)
        , ColumnListBase(other, patches)
    {
    }

    Columns(const Columns<List<T>>& other)
        : Subexpr2<T>(other)
        , ColumnListBase(other)
    {
    }

    std::unique_ptr<Subexpr> clone(QueryNodeHandoverPatches* patches) const override
    {
        return make_subexpr<Columns<List<T>>>(*this, patches);
    }

    const Table* get_base_table() const override
    {
        return m_link_map.base_table();
    }

    void set_base_table(const Table* table) override
    {
        m_link_map.set_base_table(table);
        m_column_name = m_link_map.target_table()->get_column_name(m_column_ndx);
    }

    void set_cluster(const Cluster* cluster) override
    {
        ColumnListBase::set_cluster(cluster);
    }

    void update_column() const override
    {
        m_link_map.update_columns();
        m_column_ndx = m_link_map.target_table()->get_column_index(m_column_name);
        if (m_column_ndx == realm::npos) {
            throw LogicError(LogicError::column_does_not_exist);
        }
    }

    void evaluate(size_t index, ValueBase& destination) override
    {
        Allocator& alloc = get_base_table()->get_alloc();
        Value<ref_type> list_refs;
        get_lists(index, list_refs, 1);
        size_t sz = 0;
        for (size_t i = 0; i < list_refs.m_values; i++) {
            ref_type val = list_refs.m_storage[i];
            if (val) {
                char* header = alloc.translate(val);
                sz += Array::get_size_from_header(header);
            }
        }
        auto v = make_value_for_link<typename util::RemoveOptional<T>::type>(false, sz);
        size_t k = 0;
        for (size_t i = 0; i < list_refs.m_values; i++) {
            ref_type list_ref = list_refs.m_storage[i];
            if (list_ref) {
                typename ColumnTypeTraits<T>::cluster_leaf_type leaf(alloc);
                leaf.init_from_ref(list_ref);
                size_t s = leaf.size();
                for (size_t j = 0; j < s; j++) {
                    if (!leaf.is_null(j)) {
                        v.m_storage.set(k++, leaf.get(j));
                    }
                }
            }
        }
        destination.import(v);
    }

    virtual std::string description() const override
    {
        if (links_exist()) {
            return m_link_map.description();
        }
        auto target_table = m_link_map.target_table();
        if (target_table) {
            return std::string(target_table->get_column_name(m_column_ndx));
        }
        return "";
    }

    SizeOperator<SizeOfList> size();

    ListColumnAggregate<T, aggregate_operations::Minimum<T>> min() const
    {
        return {m_column_ndx, *this};
    }

    ListColumnAggregate<T, aggregate_operations::Maximum<T>> max() const
    {
        return {m_column_ndx, *this};
    }

    ListColumnAggregate<T, aggregate_operations::Sum<T>> sum() const
    {
        return {m_column_ndx, *this};
    }

    ListColumnAggregate<T, aggregate_operations::Average<T>> average() const
    {
        return {m_column_ndx, *this};
    }


private:
    friend class Table;

    Columns(size_t column_ndx, const Table* table, const std::vector<size_t>& links = {})
        : ColumnListBase(column_ndx, table, links)
    {
    }
};

template <typename T>
class ColumnListSize : public Columns<List<T>> {
public:
    ColumnListSize(const Columns<List<T>>& other)
        : Columns<List<T>>(other)
    {
    }
    void evaluate(size_t index, ValueBase& destination) override
    {
        REALM_ASSERT_DEBUG(dynamic_cast<Value<SizeOfList>*>(&destination) != nullptr);
        Value<SizeOfList>* d = static_cast<Value<SizeOfList>*>(&destination);

        Allocator& alloc = this->get_base_table()->get_alloc();
        Value<ref_type> list_refs;
        this->get_lists(index, list_refs, 1);
        d->init(list_refs.m_from_link_list, list_refs.m_values);

        for (size_t i = 0; i < list_refs.m_values; i++) {
            ref_type list_ref = list_refs.m_storage[i];
            if (list_ref) {
                typename ColumnTypeTraits<T>::cluster_leaf_type leaf(alloc);
                leaf.init_from_ref(list_ref);
                size_t s = leaf.size();
                d->m_storage.set(i, SizeOfList(s));
            }
            else {
                d->m_storage.set_null(i);
            }
        }
    }
    std::unique_ptr<Subexpr> clone(QueryNodeHandoverPatches*) const override
    {
        return std::unique_ptr<Subexpr>(new ColumnListSize<T>(*this));
    }
};

template <typename T>
SizeOperator<SizeOfList> Columns<List<T>>::size()
{
    std::unique_ptr<Subexpr> ptr(new ColumnListSize<T>(*this));
    return SizeOperator<SizeOfList>(std::move(ptr));
}

template <typename T, typename Operation>
class ListColumnAggregate : public Subexpr2<typename Operation::ResultType> {
public:
    using R = typename Operation::ResultType;

    ListColumnAggregate(size_t column_ndx, Columns<List<T>> column)
        : m_column_ndx(column_ndx)
        , m_list(std::move(column))
    {
    }

    ListColumnAggregate(const ListColumnAggregate& other, QueryNodeHandoverPatches* patches)
        : m_column_ndx(other.m_column_ndx)
        , m_list(other.m_list, patches)
    {
    }

    std::unique_ptr<Subexpr> clone(QueryNodeHandoverPatches* patches) const override
    {
        return make_subexpr<ListColumnAggregate>(*this, patches);
    }

    const Table* get_base_table() const override
    {
        return m_list.get_base_table();
    }

    void set_base_table(const Table* table) override
    {
        m_list.set_base_table(table);
    }

    void set_cluster(const Cluster* cluster) override
    {
        m_list.set_cluster(cluster);
    }

    void update_column() const override
    {
        m_list.update_column();
    }

    void evaluate(size_t index, ValueBase& destination) override
    {
        Allocator& alloc = get_base_table()->get_alloc();
        Value<ref_type> list_refs;
        m_list.get_lists(index, list_refs, 1);
        REALM_ASSERT_DEBUG(list_refs.m_values > 0 || list_refs.m_from_link_list);
        size_t sz = list_refs.m_values;
        // The result is an aggregate value for each table
        auto v = make_value_for_link<R>(!list_refs.m_from_link_list, sz);
        for (unsigned i = 0; i < sz; i++) {
            auto list_ref = list_refs.m_storage[i];
            Operation op;
            if (list_ref) {
                typename ColumnTypeTraits<T>::cluster_leaf_type leaf(alloc);
                leaf.init_from_ref(list_ref);
                size_t s = leaf.size();
                for (unsigned j = 0; j < s; j++) {
                    op.accumulate(leaf.get(j));
                }
            }
            if (op.is_null()) {
                v.m_storage.set_null(i);
            }
            else {
                v.m_storage.set(i, op.result());
            }
        }
        destination.import(v);
    }

    virtual std::string description() const override
    {
        auto table = get_base_table();
        if (table) {
            return std::string(table->get_column_name(m_column_ndx)) + util::serializer::value_separator +
                   Operation::description() + "()";
        }
        return "";
    }

private:
    size_t m_column_ndx;
    Columns<List<T>> m_list;
};

template <class Operator>
Query compare(const Subexpr2<Link>& left, const ConstObj& obj)
{
    static_assert(std::is_same<Operator, Equal>::value || std::is_same<Operator, NotEqual>::value,
                  "Links can only be compared for equality.");
    const Columns<Link>* column = dynamic_cast<const Columns<Link>*>(&left);
    if (column) {
        const LinkMap& link_map = column->link_map();
        REALM_ASSERT(link_map.target_table() == ConstTableRef(obj.get_table()));
#ifdef REALM_OLDQUERY_FALLBACK
        if (link_map.get_nb_hops() == 1) {
            // We can fall back to Query::links_to for != and == operations on links, but only
            // for == on link lists. This is because negating query.links_to() is equivalent to
            // to "ALL linklist != row" rather than the "ANY linklist != row" semantics we're after.
            if (link_map.m_link_types[0] == col_type_Link ||
                (link_map.m_link_types[0] == col_type_LinkList && std::is_same<Operator, Equal>::value)) {
                const Table* t = column->get_base_table();
                Query query(*t);

                if (std::is_same<Operator, NotEqual>::value) {
                    // Negate the following `links_to`.
                    query.Not();
                }
                query.links_to(link_map.m_link_column_indexes[0], obj.get_key());
                return query;
            }
        }
#endif
    }
    return make_expression<Compare<Operator, Key>>(left.clone(), make_subexpr<KeyValue>(obj.get_key()));
}

inline Query operator==(const Subexpr2<Link>& left, const ConstObj& row)
{
    return compare<Equal>(left, row);
}
inline Query operator!=(const Subexpr2<Link>& left, const ConstObj& row)
{
    return compare<NotEqual>(left, row);
}
inline Query operator==(const ConstObj& row, const Subexpr2<Link>& right)
{
    return compare<Equal>(right, row);
}
inline Query operator!=(const ConstObj& row, const Subexpr2<Link>& right)
{
    return compare<NotEqual>(right, row);
}

template <class Operator>
Query compare(const Subexpr2<Link>& left, null)
{
    static_assert(std::is_same<Operator, Equal>::value || std::is_same<Operator, NotEqual>::value,
                  "Links can only be compared for equality.");
    return make_expression<Compare<Operator, Key>>(left.clone(), make_subexpr<KeyValue>(Key{}));
}

inline Query operator==(const Subexpr2<Link>& left, null)
{
    return compare<Equal>(left, null());
}
inline Query operator!=(const Subexpr2<Link>& left, null)
{
    return compare<NotEqual>(left, null());
}
inline Query operator==(null, const Subexpr2<Link>& right)
{
    return compare<Equal>(right, null());
}
inline Query operator!=(null, const Subexpr2<Link>& right)
{
    return compare<NotEqual>(right, null());
}


template <class T>
class Columns : public Subexpr2<T> {
public:
    using ColType = typename ColumnTypeTraits<T>::column_type;
    using LeafType = typename ColumnTypeTraits<T>::cluster_leaf_type;

    Columns(size_t column, const Table* table, std::vector<size_t> links = {})
        : m_link_map(table, std::move(links))
        , m_column_name(m_link_map.target_table()->get_column_name(column))
        , m_column_ndx(column)
        , m_nullable(m_link_map.target_table()->is_nullable(m_column_ndx))
    {
    }

    Columns(const Columns& other, QueryNodeHandoverPatches* patches = nullptr)
        : m_link_map(other.m_link_map, patches)
        , m_column_name(other.m_column_name)
        , m_column_ndx(other.m_column_ndx)
        , m_nullable(other.m_nullable)
    {
    }

    Columns& operator=(const Columns& other)
    {
        if (this != &other) {
            m_link_map = other.m_link_map;
            m_column_name = other.m_column_name;
            m_column_ndx = other.m_column_ndx;
            m_nullable = other.m_nullable;
        }
        return *this;
    }

    std::unique_ptr<Subexpr> clone(QueryNodeHandoverPatches* patches) const override
    {
        return make_subexpr<Columns<T>>(*this, patches);
    }

    // See comment in base class
    void set_base_table(const Table* table) override
    {
        if (table == get_base_table())
            return;

        m_link_map.set_base_table(table);
        m_nullable = m_link_map.target_table()->is_nullable(m_column_ndx);
        m_column_name = m_link_map.target_table()->get_column_name(m_column_ndx);
    }

    void set_cluster(const Cluster* cluster) override
    {
        m_array_ptr = nullptr;
        m_leaf_ptr = nullptr;
        if (links_exist()) {
            m_link_map.set_cluster(cluster);
        }
        else {
            // Create new Leaf
            m_array_ptr = LeafPtr(new (&m_leaf_cache_storage) LeafType(get_base_table()->get_alloc()));
            cluster->init_leaf(this->m_column_ndx, m_array_ptr.get());
            m_leaf_ptr = m_array_ptr.get();
        }
    }

    void update_column() const override
    {
        // update links
        m_link_map.update_columns();
        // update target column
        m_column_ndx = m_link_map.target_table()->get_column_index(m_column_name);
        if (m_column_ndx == realm::npos) {
            throw LogicError(LogicError::column_does_not_exist);
        }
    }

    // Recursively fetch tables of columns in expression tree. Used when user first builds a stand-alone expression
    // and binds it to a Query at a later time
    const Table* get_base_table() const override
    {
        return m_link_map.base_table();
    }

    template <class LeafType2 = LeafType>
    void evaluate_internal(size_t index, ValueBase& destination)
    {
        using U = typename LeafType2::value_type;

        if (links_exist()) {
            REALM_ASSERT(m_leaf_ptr == nullptr);
            // LinkList with more than 0 values. Create Value with payload for all fields
            std::vector<Key> links = m_link_map.get_links(index);
            auto v = make_value_for_link<typename util::RemoveOptional<U>::type>(m_link_map.only_unary_links(),
                                                                                 links.size());

            for (size_t t = 0; t < links.size(); t++) {
                ConstObj obj = m_link_map.target_table()->get_object(links[t]);
                if (obj.is_null(m_column_ndx))
                    v.m_storage.set_null(t);
                else
                    v.m_storage.set(t, obj.get<U>(m_column_ndx));
            }
            destination.import(v);
        }
        else {
            REALM_ASSERT(m_leaf_ptr != nullptr);
            auto leaf = static_cast<const LeafType2*>(m_leaf_ptr);
            // Not a Link column
            size_t colsize = leaf->size();

            // Now load `ValueBase::default_size` rows from from the leaf into m_storage. If it's an integer
            // leaf, then it contains the method get_chunk() which copies these values in a super fast way (first
            // case of the `if` below. Otherwise, copy the values one by one in a for-loop (the `else` case).
            if (std::is_same<U, int64_t>::value && index + ValueBase::default_size <= colsize) {
                Value<int64_t> v;

                // If you want to modify 'default_size' then update Array::get_chunk()
                REALM_ASSERT_3(ValueBase::default_size, ==, 8);

                auto leaf_2 = static_cast<const Array*>(leaf);
                leaf_2->get_chunk(index, v.m_storage.m_first);

                destination.import(v);
            }
            else {
                size_t rows = colsize - index;
                if (rows > ValueBase::default_size)
                    rows = ValueBase::default_size;
                Value<typename util::RemoveOptional<U>::type> v(false, rows);

                for (size_t t = 0; t < rows; t++)
                    v.m_storage.set(t, leaf->get(index + t));

                destination.import(v);
            }
        }
    }

    virtual std::string description() const override
    {
        std::string desc = "";
        if (links_exist()) {
            desc = m_link_map.description() + util::serializer::value_separator;
        }
        auto target_table = m_link_map.target_table();
        if (target_table && m_column_ndx != npos) {
            desc += std::string(target_table->get_column_name(m_column_ndx));
            return desc;
        }
        return "";
    }

    // Load values from Column into destination
    void evaluate(size_t index, ValueBase& destination) override
    {
        if (m_nullable && std::is_same<typename ColType::value_type, int64_t>::value) {
            evaluate_internal<ArrayIntNull>(index, destination);
        }
        else {
            evaluate_internal<LeafType>(index, destination);
        }
    }

    void evaluate(Key key, ValueBase& destination) override
    {
        auto table = m_link_map.target_table();
        auto obj = table->get_object(key);
        if (m_nullable && std::is_same<typename ColType::value_type, int64_t>::value) {
            Value<int64_t> v(false, 1);
            v.m_storage.set(0, obj.template get<util::Optional<int64_t>>(m_column_ndx));
            destination.import(v);
        }
        else {
            Value<typename util::RemoveOptional<T>::type> v(false, 1);
            T val = obj.template get<T>(m_column_ndx);
            v.m_storage.set(0, val);
            destination.import(v);
        }
    }

    bool links_exist() const
    {
        return m_link_map.has_links();
    }

    bool is_nullable() const
    {
        return m_nullable;
    }

    size_t column_ndx() const noexcept
    {
        return m_column_ndx;
    }

private:
    LinkMap m_link_map;

    // Leaf cache
    using LeafCacheStorage = typename std::aligned_storage<sizeof(LeafType), alignof(LeafType)>::type;
    using LeafPtr = std::unique_ptr<ArrayPayload, PlacementDelete>;
    LeafCacheStorage m_leaf_cache_storage;
    LeafPtr m_array_ptr;
    const ArrayPayload* m_leaf_ptr = nullptr;

    // Column index of payload column of m_table
    std::string m_column_name;
    mutable size_t m_column_ndx;

    // set to false by default for stand-alone Columns declaration that are not yet associated with any table
    // or oclumn. Call init() to update it or use a constructor that takes table + column index as argument.
    bool m_nullable = false;
};

template <typename T, typename Operation>
class SubColumnAggregate;

template <typename T>
class SubColumns : public Subexpr {
public:
    SubColumns(Columns<T> column, LinkMap link_map)
        : m_column(std::move(column))
        , m_link_map(std::move(link_map))
    {
    }

    std::unique_ptr<Subexpr> clone(QueryNodeHandoverPatches*) const override
    {
        return make_subexpr<SubColumns<T>>(*this);
    }

    const Table* get_base_table() const override
    {
        return m_link_map.base_table();
    }

    void set_base_table(const Table* table) override
    {
        m_link_map.set_base_table(table);
        m_column.set_base_table(m_link_map.target_table());
    }

    void update_column() const override
    {
        m_link_map.update_columns();
        m_column.update_column();
    }

    void evaluate(size_t, ValueBase&) override
    {
        // SubColumns can only be used in an expression in conjunction with its aggregate methods.
        REALM_ASSERT(false);
    }

    virtual std::string description() const override
    {
        return ""; // by itself there are no conditions, see SubColumnAggregate
    }

    SubColumnAggregate<T, aggregate_operations::Minimum<T>> min() const
    {
        return {m_column, m_link_map};
    }

    SubColumnAggregate<T, aggregate_operations::Maximum<T>> max() const
    {
        return {m_column, m_link_map};
    }

    SubColumnAggregate<T, aggregate_operations::Sum<T>> sum() const
    {
        return {m_column, m_link_map};
    }

    SubColumnAggregate<T, aggregate_operations::Average<T>> average() const
    {
        return {m_column, m_link_map};
    }

private:
    Columns<T> m_column;
    LinkMap m_link_map;
};

template <typename T, typename Operation>
class SubColumnAggregate : public Subexpr2<typename Operation::ResultType> {
public:
    SubColumnAggregate(Columns<T> column, LinkMap link_map)
        : m_column(std::move(column))
        , m_link_map(std::move(link_map))
    {
    }
    SubColumnAggregate(SubColumnAggregate const& other, QueryNodeHandoverPatches* patches)
        : m_column(other.m_column, patches)
        , m_link_map(other.m_link_map, patches)
    {
    }

    std::unique_ptr<Subexpr> clone(QueryNodeHandoverPatches* patches) const override
    {
        return make_subexpr<SubColumnAggregate>(*this, patches);
    }

    const Table* get_base_table() const override
    {
        return m_link_map.base_table();
    }

    void set_base_table(const Table* table) override
    {
        m_link_map.set_base_table(table);
        m_column.set_base_table(m_link_map.target_table());
    }

    void set_cluster(const Cluster* cluster) override
    {
        m_link_map.set_cluster(cluster);
        m_column.set_cluster(cluster);
    }

    void update_column() const override
    {
        m_link_map.update_columns();
        m_column.update_column();
    }

    void evaluate(size_t index, ValueBase& destination) override
    {
        std::vector<Key> keys = m_link_map.get_links(index);
        std::sort(keys.begin(), keys.end());

        Operation op;
        for (auto key : keys) {
            Value<T> value(false, 1);
            m_column.evaluate(key, value);
            size_t value_index = 0;
            const auto& value_storage = value.m_storage;
            if (!value_storage.is_null(value_index)) {
                op.accumulate(value_storage[value_index]);
            }
        }
        if (op.is_null()) {
            destination.import(Value<null>(false, 1, null()));
        }
        else {
            destination.import(Value<typename Operation::ResultType>(false, 1, op.result()));
        }
    }

    virtual std::string description() const override
    {
        return m_link_map.description() + util::serializer::value_separator + Operation::description() +
               util::serializer::value_separator + m_column.description();
    }

private:
    Columns<T> m_column;
    LinkMap m_link_map;
};

struct SubQueryCountHandoverPatch : QueryNodeHandoverPatch {
    QueryHandoverPatch m_query;
};

class SubQueryCount : public Subexpr2<Int> {
public:
    SubQueryCount(Query q, LinkMap link_map)
        : m_query(std::move(q))
        , m_link_map(std::move(link_map))
    {
    }

    const Table* get_base_table() const override
    {
        return m_link_map.base_table();
    }

    void set_base_table(const Table* table) override
    {
        m_link_map.set_base_table(table);
    }

    void set_cluster(const Cluster* cluster) override
    {
        m_link_map.set_cluster(cluster);
    }

    void update_column() const override
    {
        m_link_map.update_columns();
    }

    void evaluate(size_t index, ValueBase& destination) override
    {
        std::vector<Key> links = m_link_map.get_links(index);
        // std::sort(links.begin(), links.end());

        size_t count = std::accumulate(links.begin(), links.end(), size_t(0), [this](size_t running_count, Key k) {
            ConstObj obj = m_link_map.target_table()->get_object(k);
            return running_count + m_query.eval_object(obj);
        });

        destination.import(Value<Int>(false, 1, size_t(count)));
    }

    virtual std::string description() const override
    {
        return m_link_map.description() + util::serializer::value_separator + "SUBQUERY(" +
               m_query.get_description() + ")" + util::serializer::value_separator + "@count";
    }

    std::unique_ptr<Subexpr> clone(QueryNodeHandoverPatches* patches) const override
    {
        if (patches)
            return std::unique_ptr<Subexpr>(new SubQueryCount(*this, patches));

        return make_subexpr<SubQueryCount>(*this);
    }

    void apply_handover_patch(QueryNodeHandoverPatches& patches, Group& group) override
    {
        REALM_ASSERT(patches.size());
        std::unique_ptr<QueryNodeHandoverPatch> abstract_patch = std::move(patches.back());
        patches.pop_back();

        auto patch = dynamic_cast<SubQueryCountHandoverPatch*>(abstract_patch.get());
        REALM_ASSERT(patch);

        m_query.apply_patch(patch->m_query, group);
    }

private:
    SubQueryCount(const SubQueryCount& other, QueryNodeHandoverPatches* patches)
        : m_link_map(other.m_link_map, patches)
    {
        std::unique_ptr<SubQueryCountHandoverPatch> patch(new SubQueryCountHandoverPatch);
        m_query = Query(other.m_query, patch->m_query, ConstSourcePayload::Copy);
        patches->emplace_back(patch.release());
    }

    Query m_query;
    LinkMap m_link_map;
};

// The unused template parameter is a hack to avoid a circular dependency between table.hpp and query_expression.hpp.
template <class>
class SubQuery {
public:
    SubQuery(Columns<Link> link_column, Query query)
        : m_query(std::move(query))
        , m_link_map(link_column.link_map())
    {
        REALM_ASSERT(m_link_map.target_table() == m_query.get_table());
    }

    SubQueryCount count() const
    {
        return SubQueryCount(m_query, m_link_map);
    }

private:
    Query m_query;
    LinkMap m_link_map;
};

namespace aggregate_operations {
template <typename T, typename Derived, typename R = T>
class BaseAggregateOperation {
    static_assert(std::is_same<T, Int>::value || std::is_same<T, Float>::value || std::is_same<T, Double>::value,
                  "Numeric aggregates can only be used with subcolumns of numeric types");

public:
    using ResultType = R;

    void accumulate(T value)
    {
        m_count++;
        m_result = Derived::apply(m_result, value);
    }

    bool is_null() const
    {
        return m_count == 0;
    }
    ResultType result() const
    {
        return m_result;
    }

protected:
    size_t m_count = 0;
    ResultType m_result = Derived::initial_value();
};

template <typename T>
class Minimum : public BaseAggregateOperation<T, Minimum<T>> {
public:
    static T initial_value()
    {
        return std::numeric_limits<T>::max();
    }
    static T apply(T a, T b)
    {
        return std::min(a, b);
    }
    static std::string description()
    {
        return "@min";
    }
};

template <typename T>
class Maximum : public BaseAggregateOperation<T, Maximum<T>> {
public:
    static T initial_value()
    {
        return std::numeric_limits<T>::min();
    }
    static T apply(T a, T b)
    {
        return std::max(a, b);
    }
    static std::string description()
    {
        return "@max";
    }
};

template <typename T>
class Sum : public BaseAggregateOperation<T, Sum<T>> {
public:
    static T initial_value()
    {
        return T();
    }
    static T apply(T a, T b)
    {
        return a + b;
    }
    bool is_null() const
    {
        return false;
    }
    static std::string description()
    {
        return "@sum";
    }
};

template <typename T>
class Average : public BaseAggregateOperation<T, Average<T>, double> {
    using Base = BaseAggregateOperation<T, Average<T>, double>;

public:
    static double initial_value()
    {
        return 0;
    }
    static double apply(double a, T b)
    {
        return a + b;
    }
    double result() const
    {
        return Base::m_result / Base::m_count;
    }
    static std::string description()
    {
        return "@avg";
    }
};
}

template <class oper, class TLeft>
class UnaryOperator : public Subexpr2<typename oper::type> {
public:
    UnaryOperator(std::unique_ptr<TLeft> left)
        : m_left(std::move(left))
    {
    }

    UnaryOperator(const UnaryOperator& other, QueryNodeHandoverPatches* patches)
        : m_left(other.m_left->clone(patches))
    {
    }

    UnaryOperator& operator=(const UnaryOperator& other)
    {
        if (this != &other) {
            m_left = other.m_left->clone();
        }
        return *this;
    }

    UnaryOperator(UnaryOperator&&) = default;
    UnaryOperator& operator=(UnaryOperator&&) = default;

    // See comment in base class
    void set_base_table(const Table* table) override
    {
        m_left->set_base_table(table);
    }

    void update_column() const override
    {
        m_left->update_column();
    }

    // Recursively fetch tables of columns in expression tree. Used when user first builds a stand-alone expression
    // and binds it to a Query at a later time
    const Table* get_base_table() const override
    {
        return m_left->get_base_table();
    }

    // destination = operator(left)
    void evaluate(size_t index, ValueBase& destination) override
    {
        Value<T> result;
        Value<T> left;
        m_left->evaluate(index, left);
        result.template fun<oper>(&left);
        destination.import(result);
    }

    virtual std::string description() const override
    {
        if (m_left) {
            return m_left->description();
        }
        return "";
    }

    std::unique_ptr<Subexpr> clone(QueryNodeHandoverPatches* patches) const override
    {
        return make_subexpr<UnaryOperator>(*this, patches);
    }

    void apply_handover_patch(QueryNodeHandoverPatches& patches, Group& group) override
    {
        m_left->apply_handover_patch(patches, group);
    }

private:
    typedef typename oper::type T;
    std::unique_ptr<TLeft> m_left;
};


template <class oper, class TLeft, class TRight>
class Operator : public Subexpr2<typename oper::type> {
public:
    Operator(std::unique_ptr<TLeft> left, std::unique_ptr<TRight> right)
        : m_left(std::move(left))
        , m_right(std::move(right))
    {
    }

    Operator(const Operator& other, QueryNodeHandoverPatches* patches)
        : m_left(other.m_left->clone(patches))
        , m_right(other.m_right->clone(patches))
    {
    }

    Operator& operator=(const Operator& other)
    {
        if (this != &other) {
            m_left = other.m_left->clone();
            m_right = other.m_right->clone();
        }
        return *this;
    }

    Operator(Operator&&) = default;
    Operator& operator=(Operator&&) = default;

    // See comment in base class
    void set_base_table(const Table* table) override
    {
        m_left->set_base_table(table);
        m_right->set_base_table(table);
    }

    void set_cluster(const Cluster* cluster) override
    {
        m_left->set_cluster(cluster);
        m_right->set_cluster(cluster);
    }

    void update_column() const override
    {
        m_left->update_column();
        m_right->update_column();
    }

    // Recursively fetch tables of columns in expression tree. Used when user first builds a stand-alone expression
    // and
    // binds it to a Query at a later time
    const Table* get_base_table() const override
    {
        const Table* l = m_left->get_base_table();
        const Table* r = m_right->get_base_table();

        // Queries do not support multiple different tables; all tables must be the same.
        REALM_ASSERT(l == nullptr || r == nullptr || l == r);

        // nullptr pointer means expression which isn't yet associated with any table, or is a Value<T>
        return l ? l : r;
    }

    // destination = operator(left, right)
    void evaluate(size_t index, ValueBase& destination) override
    {
        Value<T> result;
        Value<T> left;
        Value<T> right;
        m_left->evaluate(index, left);
        m_right->evaluate(index, right);
        result.template fun<oper>(&left, &right);
        destination.import(result);
    }

    virtual std::string description() const override
    {
        std::string s;
        if (m_left) {
            s += m_left->description();
        }
        s += oper::description();
        if (m_right) {
            s += m_right->description();
        }
        return s;
    }

    std::unique_ptr<Subexpr> clone(QueryNodeHandoverPatches* patches) const override
    {
        return make_subexpr<Operator>(*this, patches);
    }

    void apply_handover_patch(QueryNodeHandoverPatches& patches, Group& group) override
    {
        m_right->apply_handover_patch(patches, group);
        m_left->apply_handover_patch(patches, group);
    }

private:
    typedef typename oper::type T;
    std::unique_ptr<TLeft> m_left;
    std::unique_ptr<TRight> m_right;
};


template <class TCond, class T, class TLeft, class TRight>
class Compare : public Expression {
public:
    Compare(std::unique_ptr<TLeft> left, std::unique_ptr<TRight> right)
        : m_left(std::move(left))
        , m_right(std::move(right))
    {
    }

    // See comment in base class
    void set_base_table(const Table* table) override
    {
        m_left->set_base_table(table);
        m_right->set_base_table(table);
    }

    void set_cluster(const Cluster* cluster) override
    {
        m_left->set_cluster(cluster);
        m_right->set_cluster(cluster);
    }

    void update_column() const override
    {
        m_left->update_column();
        m_right->update_column();
    }

    // Recursively fetch tables of columns in expression tree. Used when user first builds a stand-alone expression
    // and
    // binds it to a Query at a later time
    const Table* get_base_table() const override
    {
        const Table* l = m_left->get_base_table();
        const Table* r = m_right->get_base_table();

        // All main tables in each subexpression of a query (table.columns() or table.link()) must be the same.
        REALM_ASSERT(l == nullptr || r == nullptr || l == r);

        // nullptr pointer means expression which isn't yet associated with any table, or is a Value<T>
        return l ? l : r;
    }

    size_t find_first(size_t start, size_t end) const override
    {
        size_t match;
        Value<T> right;
        Value<T> left;

        for (; start < end;) {
            m_left->evaluate(start, left);
            m_right->evaluate(start, right);
            match = Value<T>::template compare<TCond>(&left, &right);

            if (match != not_found && match + start < end)
                return start + match;

            size_t rows =
                (left.m_from_link_list || right.m_from_link_list) ? 1 : minimum(right.m_values, left.m_values);
            start += rows;
        }

        return not_found; // no match
    }

    virtual std::string description() const override
    {
        if (std::is_same<TCond, BeginsWith>::value || std::is_same<TCond, BeginsWithIns>::value ||
            std::is_same<TCond, EndsWith>::value || std::is_same<TCond, EndsWithIns>::value ||
            std::is_same<TCond, Contains>::value || std::is_same<TCond, ContainsIns>::value ||
            std::is_same<TCond, Like>::value || std::is_same<TCond, LikeIns>::value) {
            // these string conditions have the arguments reversed but the order is important
            // operations ==, and != can be reversed because the produce the same results both ways
            return util::serializer::print_value(m_right->description() + " " + TCond::description() + " " +
                                                 m_left->description());
        }
        return util::serializer::print_value(m_left->description() + " " + TCond::description() + " " +
                                             m_right->description());
    }

    std::unique_ptr<Expression> clone(QueryNodeHandoverPatches* patches) const override
    {
        return std::unique_ptr<Expression>(new Compare(*this, patches));
    }

    void apply_handover_patch(QueryNodeHandoverPatches& patches, Group& group) override
    {
        m_right->apply_handover_patch(patches, group);
        m_left->apply_handover_patch(patches, group);
    }

private:
    Compare(const Compare& other, QueryNodeHandoverPatches* patches)
        : m_left(other.m_left->clone(patches))
        , m_right(other.m_right->clone(patches))
    {
    }

    std::unique_ptr<TLeft> m_left;
    std::unique_ptr<TRight> m_right;
};
}
#endif // REALM_QUERY_EXPRESSION_HPP
