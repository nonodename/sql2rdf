#include "r2rml/ColumnTermMap.h"

namespace r2rml {

ColumnTermMap::ColumnTermMap(const std::string& column)
    : columnName(column) {}
ColumnTermMap::~ColumnTermMap() = default;
SerdNode ColumnTermMap::generateRDFTerm(const SQLRow&,
                                        const SerdEnv&) const {
    return SerdNode{0};
}

} // namespace r2rml
