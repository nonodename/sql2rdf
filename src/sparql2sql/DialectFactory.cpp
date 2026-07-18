#include "sparql2sql/DialectFactory.h"

#include <stdexcept>

#include "sparql2sql/DuckDbDialect.h"

namespace sparql2sql {

std::unique_ptr<SqlDialect> createDialect(const std::string &name) {
	if (name == "duckdb") {
		return std::unique_ptr<SqlDialect>(new DuckDbDialect());
	}
	throw std::runtime_error("unknown SQL dialect: '" + name + "' (supported: duckdb)");
}

} // namespace sparql2sql
