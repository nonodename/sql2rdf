#pragma once

#include <memory>
#include <string>

namespace r2rml {

/**
 * Abstract interface for a single SQL value.  Implement this interface to
 * provide backend-specific value representations (e.g. lazy DuckDB conversion
 * or a simple string-backed value for tests).
 */
class SQLValue {
public:
	enum class Type { Null, Integer, Double, String, Boolean };

	virtual ~SQLValue() = default;

	virtual Type type() const = 0;
	virtual const std::string &asString() const = 0;
	virtual bool isNull() const = 0;

	/** Deep-copy this value. */
	virtual std::unique_ptr<SQLValue> clone() const = 0;
};

} // namespace r2rml
