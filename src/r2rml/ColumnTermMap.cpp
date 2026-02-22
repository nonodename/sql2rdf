#include "r2rml/ColumnTermMap.h"
#include "r2rml/SQLRow.h"
#include "r2rml/SQLValue.h"

namespace r2rml {

ColumnTermMap::ColumnTermMap(const std::string& column)
    : columnName(column) {}

ColumnTermMap::~ColumnTermMap() = default;

SerdNode ColumnTermMap::generateRDFTerm(const SQLRow& row,
                                        const SerdEnv& /*env*/) const
{
    SQLValue val = row.getValue(columnName);
    if (val.isNull())
        return SERD_NODE_NULL;

    cachedValue_ = val.asString();

    SerdType nodeType = (termType == TermType::IRI) ? SERD_URI : SERD_LITERAL;
    return serd_node_from_string(nodeType,
        reinterpret_cast<const uint8_t*>(cachedValue_.c_str()));
}

} // namespace r2rml
