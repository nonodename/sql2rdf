#include "sparql2sql/TermMapSql.h"

#include <stdexcept>

#include "r2rml/ColumnTermMap.h"
#include "r2rml/ConstantTermMap.h"
#include "r2rml/TemplateTermMap.h"
#include "r2rml/TermMap.h"
#include "sparql-parser/ast/Term.h"
#include "sparql2sql/SqlDialect.h"
#include "sparql2sql/TemplateUtil.h"
#include "sparql2sql/TypeCatalog.h"

namespace sparql2sql {

namespace {

std::string constantTermMapText(const r2rml::ConstantTermMap &constant) {
	if (constant.constantValue.buf == nullptr) {
		return std::string();
	}
	return std::string(reinterpret_cast<const char *>(constant.constantValue.buf), constant.constantValue.n_bytes);
}

// Render a plain rr:column reference for comparison/projection. Wrapped in
// CAST(... AS VARCHAR) unless the catalog already knows the column is a
// VARCHAR-family type, in which case the cast is a provable no-op and is
// dropped - the catalog is optional, so with none supplied this is exactly
// the old unconditional-cast behaviour.
std::string columnExpr(const std::string &sourceAlias, const std::string &columnName, const SqlDialect &dialect,
                       const TypeCatalog *catalog, const std::string &tableIdentity) {
	if (catalog != nullptr && !tableIdentity.empty() && catalog->isStringType(tableIdentity, columnName)) {
		return sourceAlias + "." + dialect.quoteIdentifier(columnName);
	}
	return "CAST(" + sourceAlias + "." + dialect.quoteIdentifier(columnName) + " AS VARCHAR)";
}

std::string qualifiedColumnRef(const std::string &sourceAlias, const std::string &columnName,
                               const SqlDialect &dialect) {
	return sourceAlias + "." + dialect.quoteIdentifier(columnName);
}

// The guards a term map actually needs, alias-qualified.
//
// Alias-qualified because a candidate's FROM clause can join multiple tables
// (e.g. a ReferencingObjectMap's child and parent logical tables), and an
// unqualified "IS NOT NULL" check on a bare column name would be ambiguous - or
// silently resolve to the wrong table - as soon as two joined tables share a
// column name (e.g. both sides of a join on a "DEPTNO" column).
//
// A column the DDL declares NOT NULL is dropped outright: R2RML's "null column
// => drop this term" rule can never fire for it, so the conjunct the caller
// would emit is unconditionally true. The catalog is optional and only ever
// reports what it knows, so with none supplied - or a view-backed source, which
// has no constraint metadata - this returns every ref, exactly as before.
std::vector<std::string> requiredNonNullRefs(const std::vector<std::string> &columnNames,
                                             const std::string &sourceAlias, const SqlDialect &dialect,
                                             const TypeCatalog *catalog, const std::string &tableIdentity) {
	std::vector<std::string> out;
	out.reserve(columnNames.size());
	for (const auto &c : columnNames) {
		if (catalog != nullptr && !tableIdentity.empty() && catalog->isNotNull(tableIdentity, c)) {
			continue;
		}
		out.push_back(qualifiedColumnRef(sourceAlias, c, dialect));
	}
	return out;
}

// Derive a term map's static term annotation. `columnName` is the term map's
// single source column, or empty for a template/constant map.
//
// Applies the *position-independent* half of R2RML 7.4 only. It must not
// re-apply the positional default ("an object map defaults to rr:Literal"):
// the parser already does that where it applies, and re-applying it here -
// where there is no notion of position - would wrongly demote a templated-IRI
// object map to a literal.
TermInfo annotate(const r2rml::TermMap &termMap, const std::string &columnName, const TypeCatalog *catalog,
                  const std::string &tableIdentity) {
	TermInfo out;
	switch (termMap.termType) {
	case r2rml::TermType::IRI:
		out.kind = RdfTermKind::Iri;
		break;
	case r2rml::TermType::BlankNode:
		out.kind = RdfTermKind::BlankNode;
		break;
	case r2rml::TermType::Literal:
		out.kind = RdfTermKind::Literal;
		break;
	}
	// R2RML 7.4: a term map with rr:language or rr:datatype is a literal,
	// whatever else was inferred. This closes a real parser gap - an object map
	// written `rr:template "..."; rr:datatype xsd:integer` keeps termType IRI,
	// because the parser only applies the Literal default to rr:column maps.
	if (termMap.languageTag || termMap.datatypeIRI) {
		out.kind = RdfTermKind::Literal;
	}
	if (out.kind != RdfTermKind::Literal) {
		return out; // An IRI or blank node carries no datatype or language.
	}
	if (termMap.languageTag && !termMap.languageTag->empty()) {
		out.lang = *termMap.languageTag;
		out.datatypeIri = kRdfLangString;
		out.maybeLangTagged = true;
		return out;
	}
	if (termMap.datatypeIRI && !termMap.datatypeIRI->empty()) {
		out.datatypeIri = *termMap.datatypeIRI;
		return out;
	}
	if (columnName.empty()) {
		// A templated literal has no single source column to type. A constant
		// literal has already lost its datatype: the parser builds it from the
		// Turtle object's bare lexical value, discarding any ^^xsd:foo. Leaving
		// the datatype unknown is the honest answer in both cases - defaulting
		// to xsd:string would be a guess DATATYPE() would then report as fact.
		return out;
	}
	if (catalog == nullptr || tableIdentity.empty()) {
		return out;
	}
	// R2RML 10.2's natural mapping. For an rr:sqlQuery view the tableIdentity
	// is a "view:<sql>" key, which a catalog only holds if its populator asked
	// the backend to describe the view's result schema (see
	// sparql2sql/LogicalTableSource.h); if it didn't, this misses and the
	// datatype stays unknown rather than guessed.
	out.datatypeIri = naturalXsdDatatype(catalog->typeOf(tableIdentity, columnName));
	return out;
}

} // namespace

TermInfo termInfoOfTerm(const sparql::ast::Term &term) {
	TermInfo out;
	switch (term.kind()) {
	case sparql::ast::TermKind::Iri:
		out.kind = RdfTermKind::Iri;
		break;
	case sparql::ast::TermKind::BlankNode:
		out.kind = RdfTermKind::BlankNode;
		break;
	case sparql::ast::TermKind::Literal: {
		const auto &literal = static_cast<const sparql::ast::RdfLiteral &>(term);
		out.kind = RdfTermKind::Literal;
		if (!literal.languageTag.empty()) {
			out.lang = literal.languageTag;
			out.datatypeIri = kRdfLangString;
			out.maybeLangTagged = true;
		} else if (literal.datatype) {
			out.datatypeIri = literal.datatype->value;
		} else {
			out.datatypeIri = xsd::kString;
		}
		break;
	}
	case sparql::ast::TermKind::Var:
		break; // Unknown: a variable's term depends on what binds it.
	}
	return out;
}

std::string termLexicalForm(const sparql::ast::Term &term) {
	using sparql::ast::BlankNode;
	using sparql::ast::Iri;
	using sparql::ast::RdfLiteral;
	using sparql::ast::TermKind;
	using sparql::ast::Var;
	switch (term.kind()) {
	case TermKind::Iri:
		return static_cast<const Iri &>(term).value;
	case TermKind::Literal:
		return static_cast<const RdfLiteral &>(term).lexicalForm;
	case TermKind::BlankNode:
		return static_cast<const BlankNode &>(term).label;
	case TermKind::Var:
		return static_cast<const Var &>(term).name;
	}
	return std::string();
}

SqlExpr termMapToSqlExpr(const r2rml::TermMap &termMap, const std::string &sourceAlias, const SqlDialect &dialect,
                         const TypeCatalog *catalog, const std::string &tableIdentity) {
	if (const auto *col = dynamic_cast<const r2rml::ColumnTermMap *>(&termMap)) {
		SqlExpr result;
		result.expr = columnExpr(sourceAlias, col->columnName, dialect, catalog, tableIdentity);
		result.requiredNonNullColumns =
		    requiredNonNullRefs({col->columnName}, sourceAlias, dialect, catalog, tableIdentity);
		result.term = annotate(termMap, col->columnName, catalog, tableIdentity);
		return result;
	}
	if (const auto *tmpl = dynamic_cast<const r2rml::TemplateTermMap *>(&termMap)) {
		std::vector<TemplateSegment> segments = parseTemplate(tmpl->templateString);
		SqlExpr result;
		result.expr = buildProjectionSql(segments, sourceAlias, dialect, tmpl->termType == r2rml::TermType::IRI);
		result.requiredNonNullColumns =
		    requiredNonNullRefs(referencedColumns(segments), sourceAlias, dialect, catalog, tableIdentity);
		result.term = annotate(termMap, std::string(), catalog, tableIdentity);
		return result;
	}
	if (const auto *constant = dynamic_cast<const r2rml::ConstantTermMap *>(&termMap)) {
		SqlExpr result;
		result.expr = dialect.stringLiteral(constantTermMapText(*constant));
		result.term = annotate(termMap, std::string(), catalog, tableIdentity);
		return result;
	}
	throw std::logic_error("termMapToSqlExpr: unrecognized TermMap subtype");
}

InversionResult invertTermMapAgainstBoundTerm(const r2rml::TermMap &termMap, const sparql::ast::Term &boundTerm,
                                              const std::string &sourceAlias, const SqlDialect &dialect,
                                              const TypeCatalog *catalog, const std::string &tableIdentity) {
	InversionResult result;
	const std::string boundValue = termLexicalForm(boundTerm);

	if (const auto *constant = dynamic_cast<const r2rml::ConstantTermMap *>(&termMap)) {
		result.possible = (constantTermMapText(*constant) == boundValue);
		return result;
	}
	if (const auto *col = dynamic_cast<const r2rml::ColumnTermMap *>(&termMap)) {
		result.possible = true;
		result.whereConditions.push_back(columnExpr(sourceAlias, col->columnName, dialect, catalog, tableIdentity) +
		                                 " = " + dialect.stringLiteral(boundValue));
		return result;
	}
	if (const auto *tmpl = dynamic_cast<const r2rml::TemplateTermMap *>(&termMap)) {
		std::vector<TemplateSegment> segments = parseTemplate(tmpl->templateString);
		const bool percentEncoded = tmpl->termType == r2rml::TermType::IRI;
		InversionOutcome outcome = invertTemplate(segments, boundValue, percentEncoded);
		if (outcome.kind == InversionKind::NeverMatches) {
			result.possible = false;
			return result;
		}
		result.possible = true;
		if (outcome.kind == InversionKind::WholeTemplateMatch) {
			result.whereConditions.push_back(buildProjectionSql(segments, sourceAlias, dialect, percentEncoded) +
			                                 " = " + dialect.stringLiteral(boundValue));
			return result;
		}
		for (const auto &columnValue : outcome.columnValues) {
			result.whereConditions.push_back(
			    columnExpr(sourceAlias, columnValue.first, dialect, catalog, tableIdentity) + " = " +
			    dialect.stringLiteral(columnValue.second));
		}
		return result;
	}
	throw std::logic_error("invertTermMapAgainstBoundTerm: unrecognized TermMap subtype");
}

} // namespace sparql2sql
