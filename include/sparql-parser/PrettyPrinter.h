#pragma once

#include <ostream>

#include "sparql-parser/ast/Query.h"

namespace sparql {

/// Write an indented tree dump of `query` to `os` - one node kind (plus key
/// fields) per line, children indented under their parent.
void print(std::ostream &os, const ast::Query &query);

namespace ast {
// Defined in the ast namespace (rather than sparql) so that argument-dependent
// lookup finds it for `os << query` - ADL only associates the innermost
// namespace of the argument's class, which is sparql::ast, not sparql.
std::ostream &operator<<(std::ostream &os, const Query &query);
} // namespace ast

} // namespace sparql
