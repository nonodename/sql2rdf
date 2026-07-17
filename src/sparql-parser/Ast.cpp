// Out-of-line special members for AST node types that hold a member of a
// type only forward-declared in their own header (GraphPattern.h forward
// declares Expression and Query to avoid an include cycle - see the
// comment there). Defining these here, where Expression.h/Query.h have
// been fully included, is all that's needed to make them well-formed.

#include "sparql-parser/ast/Expression.h"
#include "sparql-parser/ast/GraphPattern.h"
#include "sparql-parser/ast/Query.h"

namespace sparql {
namespace ast {

Filter::Filter() : GroupElement(ElementKind::Filter) {
}
Filter::~Filter() = default;

Bind::Bind() : GroupElement(ElementKind::Bind) {
}
Bind::~Bind() = default;

SubSelectElement::SubSelectElement(std::unique_ptr<Query> query)
    : GroupElement(ElementKind::SubSelect), query(std::move(query)) {
}
SubSelectElement::~SubSelectElement() = default;

} // namespace ast
} // namespace sparql
