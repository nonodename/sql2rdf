#pragma once

#include <ostream>

#include "sparql-parser/ast/Query.h"

namespace sparql {

/// Write an indented tree dump of `query` to `os` - one node kind (plus key
/// fields) per line, children indented under their parent.
void print(std::ostream &os, const ast::Query &query);

std::ostream &operator<<(std::ostream &os, const ast::Query &query);

} // namespace sparql
