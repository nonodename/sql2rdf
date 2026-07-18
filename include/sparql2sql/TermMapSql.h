#pragma once

#include <string>
#include <vector>

namespace r2rml {
class TermMap;
} // namespace r2rml

namespace sparql {
namespace ast {
class Term;
} // namespace ast
} // namespace sparql

namespace sparql2sql {

class SqlDialect;

/// A SQL scalar expression together with the source columns that must be
/// non-NULL for it to be well-defined (mirrors R2RML forward generation's
/// "null column => drop this term" rule, so the caller can add the
/// corresponding "IS NOT NULL" guards). Each entry is already an
/// alias-qualified, dialect-quoted column reference (e.g. `t1."EMPNO"`),
/// not a bare column name - required so the guard stays unambiguous once a
/// candidate's FROM clause joins multiple tables that might share a column
/// name.
struct SqlExpr {
	std::string expr;
	std::vector<std::string> requiredNonNullColumns;
};

/// Extract a bound SPARQL term's lexical string form (IRI's absolute value,
/// a literal's lexical form, or a blank node's label) for comparison against
/// R2RML-generated term text. Never called with a Var in practice (callers
/// only invoke it for constant subject/predicate/object positions).
std::string termLexicalForm(const sparql::ast::Term &term);

/// Convert an R2RML term map (ColumnTermMap/TemplateTermMap/ConstantTermMap
/// - the only concrete subtypes the R2RML parser ever instantiates) into a
/// SQL scalar expression evaluated over the given source alias's columns.
/// Dispatches via dynamic_cast, matching the codebase's own established
/// idiom (e.g. PredicateObjectMap::processRow's dynamic_cast dispatch
/// against ReferencingObjectMap).
SqlExpr termMapToSqlExpr(const r2rml::TermMap &termMap, const std::string &sourceAlias, const SqlDialect &dialect);

struct InversionResult {
	bool possible = false;
	std::vector<std::string> whereConditions;
};

/// Determine whether/how a term map could produce the given bound SPARQL
/// term, returning zero or more SQL WHERE conditions that constrain the
/// term map's source row to match it. `possible == false` means the
/// candidate can be statically discarded (e.g. a constant term map whose
/// value differs from the bound term, or a template whose fixed literal
/// segments can't fit the bound term's lexical form).
InversionResult invertTermMapAgainstBoundTerm(const r2rml::TermMap &termMap, const sparql::ast::Term &boundTerm,
                                              const std::string &sourceAlias, const SqlDialect &dialect);

} // namespace sparql2sql
