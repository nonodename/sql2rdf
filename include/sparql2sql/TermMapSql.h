#pragma once

#include <string>
#include <vector>

#include "sparql2sql/TermInfo.h"

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
struct TypeCatalog;

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

	/// What the term map statically declares about the RDF term `expr` builds,
	/// from its own rr:termType/rr:datatype/rr:language plus (for a plain
	/// rr:column literal) the optional TypeCatalog. Copied onto the ColumnInfo
	/// the caller projects. Unknown means "not inferable", which is what every
	/// column carried before term tracking existed.
	TermInfo term;
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
///
/// `catalog` and `tableIdentity` are optional and affect only the returned
/// SqlExpr's `term` annotation, never its SQL: for an rr:column literal term
/// map that declares neither rr:datatype nor rr:language, the column's SQL type
/// is looked up in the catalog and run through R2RML Section 10.2's natural
/// mapping. Both are inert for a template or constant term map (no single
/// source column). For an rr:sqlQuery view the tableIdentity is a "view:<sql>"
/// key, which only a catalog whose populator described the view's result
/// schema contains (see sparql2sql/LogicalTableSource.h); otherwise the lookup
/// misses and the datatype stays unknown.
SqlExpr termMapToSqlExpr(const r2rml::TermMap &termMap, const std::string &sourceAlias, const SqlDialect &dialect,
                         const TypeCatalog *catalog = nullptr, const std::string &tableIdentity = std::string());

/// The RDF term kind (plus a literal's datatype and language) of a bound SPARQL
/// term - the AST counterpart of the TermMap-driven annotation above, used for
/// inline VALUES cells and constant subject/predicate/object positions. A Var
/// yields Unknown.
///
/// Per RDF 1.1 a plain literal with neither language tag nor datatype IS an
/// xsd:string, and a language-tagged literal's datatype IS rdf:langString, so
/// both are filled in here rather than left empty. That is sound because the
/// term is written out in the query: unlike an rr:column literal, there is no
/// hidden SQL type that could have implied something else.
TermInfo termInfoOfTerm(const sparql::ast::Term &term);

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
