#include "r2rml/SQLValue.h"

namespace r2rml {

SQLValue::SQLValue() : type_(Type::Null) {}
SQLValue::SQLValue(const std::string& s)
    : type_(Type::String), string_(s) {}
SQLValue::SQLValue(int i)
    : type_(Type::Integer), string_(std::to_string(i)) {}
SQLValue::SQLValue(double d)
    : type_(Type::Double), string_(std::to_string(d)) {}
SQLValue::SQLValue(bool b)
    : type_(Type::Boolean), string_(b ? "true" : "false") {}

} // namespace r2rml
