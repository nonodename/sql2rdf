#include "r2rml/StringSQLValue.h"
#include "rdf/Term.h"
namespace r2rml {

StringSQLValue::StringSQLValue() : type_(Type::Null) {
}
StringSQLValue::StringSQLValue(const std::string &s) : type_(Type::String), string_(s) {
}
StringSQLValue::StringSQLValue(int i) : type_(Type::Integer), string_(std::to_string(i)) {
}
StringSQLValue::StringSQLValue(double d) : type_(Type::Double), string_(std::to_string(d)) {
}
StringSQLValue::StringSQLValue(bool b) : type_(Type::Boolean), string_(b ? "true" : "false") {
}

std::unique_ptr<SQLValue> StringSQLValue::clone() const {
	return std::unique_ptr<SQLValue>(new StringSQLValue(*this));
}

std::string StringSQLValue::datatypeIRI() const {
	switch (type_) {
	case Type::Integer:
		return rdf::XSD_INTEGER;
	case Type::Double:
		return rdf::XSD_DOUBLE;
	case Type::Boolean:
		return rdf::XSD_BOOLEAN;
	default:
		return std::string();
	}
}

} // namespace r2rml
