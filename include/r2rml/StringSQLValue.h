#pragma once

#include "SQLValue.h"
#include <string>

namespace r2rml {

/**
 * Concrete SQLValue backed by an internal string representation.  This is the
 * default value type used by tests and by the DuckDB connection when values
 * are eagerly materialised.
 */
class StringSQLValue : public SQLValue {
public:
	StringSQLValue();
	explicit StringSQLValue(const std::string &s);
	explicit StringSQLValue(int i);
	explicit StringSQLValue(double d);
	explicit StringSQLValue(bool b);

	Type type() const override {
		return type_;
	}
	const std::string &asString() const override {
		return string_;
	}
	bool isNull() const override {
		return type_ == Type::Null;
	}
	std::unique_ptr<SQLValue> clone() const override;
	std::string datatypeIRI() const override;

private:
	Type type_ {Type::Null};
	std::string string_;
};

} // namespace r2rml
