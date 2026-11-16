#include "sparql2sql/DialectFactory.h"

#include <stdexcept>

#include "sparql2sql/DuckDbDialect.h"
#include "sparql2sql/DuckDbUdfDialect.h"

namespace sparql2sql {

std::unique_ptr<SqlDialect> createDialect(const std::string &name) {
	if (name == "duckdb") {
		return std::unique_ptr<SqlDialect>(new DuckDbDialect());
	}
	if (name == "duckdb-udf") {
		return std::unique_ptr<SqlDialect>(new DuckDbUdfDialect(DuckDbUdfDialect::Coverage::Measured));
	}
	if (name == "duckdb-udf-all") {
		return std::unique_ptr<SqlDialect>(new DuckDbUdfDialect(DuckDbUdfDialect::Coverage::All));
	}
	throw std::runtime_error("unknown SQL dialect: '" + name + "' (supported: duckdb, duckdb-udf, duckdb-udf-all)");
}

} // namespace sparql2sql
