#pragma once

#include "sparql-parser/ast/Expression.h"
#include "sparql2sql/TermInfo.h"
#include "sparql2sql/TranslatedPattern.h"

namespace sparql2sql {

/// The TermInfo of the value that `expr`'s translated SQL will produce, given
/// the relation it is evaluated against.
///
/// Pure: it emits no SQL, allocates no aliases, touches no TranslationContext,
/// and **never throws**. Anything it cannot analyse - including a reference to
/// a variable that `scope` does not bind - degrades to Unknown.
/// translateExpression remains the sole place that rejects an expression.
///
/// It reports **SQL truth, not SPARQL-abstract truth**: the datatype of the
/// lexical form this translator actually emits for the expression. SPARQL
/// 17.4's numeric type promotion would call `?a + ?b` over two xsd:integers an
/// xsd:integer, and so does this function - but only because the emission is
/// made integral to match (see arithmetic() in ExpressionTranslator.cpp). Where
/// emission is lossy the annotation follows the emission, because a downstream
/// DATATYPE() must not name a datatype whose canonical lexical form differs
/// from the text sitting in the column.
///
/// Keep this function's switch in the same order as translateExpression's: the
/// two are parallel switches over the same enums, and a builtin added to one
/// and forgotten in the other is the most likely way for them to drift.
TermInfo inferExprTermInfo(const sparql::ast::Expression &expr, const TranslatedPattern &scope);

} // namespace sparql2sql
