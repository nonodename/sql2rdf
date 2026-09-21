#pragma once

#include "sparql-parser/ast/GraphPattern.h"
#include "sparql2sql/TranslatedPattern.h"

namespace sparql2sql {

/// Work out which VALUES-bound variables may be constant-folded into the
/// triple patterns of one element list, given the bindings `inherited` from
/// the enclosing scope.
///
/// WHY. A triple pattern with a constant IRI in subject or object position is
/// translated by inverting the R2RML term map that could have produced it -
/// `<http://ex/emp/7369>` against `http://ex/emp/{EMPNO}` becomes
/// `EMPNO = '7369'`, a predicate the SQL engine pushes straight into the table
/// scan. The same IRI supplied through `VALUES ?s { <http://ex/emp/7369> }`
/// used to reach the translator as a *variable*, so the subject was built in
/// the forward direction instead (concatenating and URL-encoding the template's
/// columns) and the one-row VALUES relation was joined onto that constructed
/// string - which no engine can invert, so the whole pattern materialised
/// before the join pruned it. Folding the constant back into the pattern makes
/// the two spellings generate the same SQL.
///
/// The same scope is consumed in predicate position, where the payoff is a
/// different one: a bare variable predicate enumerates a candidate arm per
/// predicate-object map of every triples map that could match, so `?s ?p ?o`
/// fans out into a union over the whole mapping and pinning `?p` prunes it to
/// the arms that can produce the constant. Only an *IRI* folds there - see
/// PropertyPathTranslator.cpp's PathKind::Variable case. Graph variables are
/// never folded; that interacts with FROM NAMED restriction and is unanalysed.
///
/// WHAT IS FOLDABLE. Only a column that binds the *same* constant term in every
/// row (so: at least one row, no UNDEF cell, every row's term identical) - a
/// genuine multi-row alternatives list still translates to the union/join it
/// always did. The VALUES relation itself is emitted and joined exactly as
/// before either way, which is what keeps the variable bound, projected and
/// correctly tagged once the patterns stop binding it themselves.
///
/// WHAT IS EXCLUDED, and why each one is not merely conservative:
///  - Any variable a FILTER or BIND expression in this subtree mentions. Those
///    expressions are translated against the relation the fold produced; if a
///    pattern ahead of the VALUES element stopped binding the variable, the
///    expression would find it out of scope.
///  - Every variable, if any such expression contains EXISTS. An EXISTS body is
///    correlated to the enclosing relation on their shared variables, so a
///    variable folded out of one side would silently drop the correlation.
///  - MINUS bodies and sub-selects, which do not inherit at all - the caller
///    pushes an empty scope around them. MINUS removes a solution only when the
///    two sides *share* a variable (SPARQL 1.1 Section 18.2), so folding one out
///    of the right-hand side would turn a real anti-join into a no-op; a
///    sub-select is evaluated independently, and a LIMIT or aggregate inside it
///    makes restricting its body observably different from restricting its
///    result.
///
/// `pattern` may be null (a query with no WHERE clause); `trailingValues` is
/// the query-level VALUES clause when translating a whole query, null when
/// folding a nested element list.
ConstantBindings inlineConstantScope(const ConstantBindings &inherited, const sparql::ast::GroupGraphPattern *pattern,
                                     const sparql::ast::InlineData *trailingValues);

} // namespace sparql2sql
