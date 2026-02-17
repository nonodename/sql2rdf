#pragma once

#include <string>
#include <memory>

namespace r2rml {

/**
 * Lightweight container for a SQL value.  Designed to be used with C++11
 * environments as a replacement for std::any/variant.  Internally stores
 * textual representation; numeric conversions can be added later.
 */
class SQLValue {
public:
    enum class Type { Null, Integer, Double, String, Boolean };

    SQLValue();
    explicit SQLValue(const std::string& s);
    explicit SQLValue(int i);
    explicit SQLValue(double d);
    explicit SQLValue(bool b);

    Type type() const { return type_; }
    const std::string& asString() const { return string_; }
    bool isNull() const { return type_ == Type::Null; }

private:
    Type type_{Type::Null};
    std::string string_;
};

} // namespace r2rml
