#include "sparql2sql/TranslatedPattern.h"

#include "sparql2sql/SqlDialect.h"

namespace sparql2sql {

std::string mangleVar(const std::string &sparqlVarName, const SqlDialect &dialect) {
	return dialect.quoteIdentifier("v_" + sparqlVarName);
}

} // namespace sparql2sql
