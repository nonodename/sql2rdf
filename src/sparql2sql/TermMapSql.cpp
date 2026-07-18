#include "sparql2sql/TermMapSql.h"

#include <stdexcept>

#include "r2rml/ColumnTermMap.h"
#include "r2rml/ConstantTermMap.h"
#include "r2rml/TemplateTermMap.h"
#include "r2rml/TermMap.h"
#include "sparql-parser/ast/Term.h"
#include "sparql2sql/SqlDialect.h"
#include "sparql2sql/TemplateUtil.h"

namespace sparql2sql {

namespace {

std::string constantTermMapText(const r2rml::ConstantTermMap &constant) {
	if (constant.constantValue.buf == nullptr) {
		return std::string();
	}
	return std::string(reinterpret_cast<const char *>(constant.constantValue.buf), constant.constantValue.n_bytes);
}

std::string columnExpr(const std::string &sourceAlias, const std::string &columnName, const SqlDialect &dialect) {
	return "CAST(" + sourceAlias + "." + dialect.quoteIdentifier(columnName) + " AS VARCHAR)";
}

std::string qualifiedColumnRef(const std::string &sourceAlias, const std::string &columnName,
                               const SqlDialect &dialect) {
	return sourceAlias + "." + dialect.quoteIdentifier(columnName);
}

// requiredNonNullColumns must be alias-qualified: a candidate's FROM clause
// can join multiple tables (e.g. a ReferencingObjectMap's child and parent
// logical tables), and an unqualified "IS NOT NULL" check on a bare column
// name would be ambiguous - or silently resolve to the wrong table - as
// soon as two joined tables share a column name (e.g. both sides of a join
// on a "DEPTNO" column).
std::vector<std::string> qualifiedColumnRefs(const std::vector<std::string> &columnNames,
                                             const std::string &sourceAlias, const SqlDialect &dialect) {
	std::vector<std::string> out;
	out.reserve(columnNames.size());
	for (const auto &c : columnNames) {
		out.push_back(qualifiedColumnRef(sourceAlias, c, dialect));
	}
	return out;
}

} // namespace

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

SqlExpr termMapToSqlExpr(const r2rml::TermMap &termMap, const std::string &sourceAlias, const SqlDialect &dialect) {
	if (const auto *col = dynamic_cast<const r2rml::ColumnTermMap *>(&termMap)) {
		SqlExpr result;
		result.expr = columnExpr(sourceAlias, col->columnName, dialect);
		result.requiredNonNullColumns.push_back(qualifiedColumnRef(sourceAlias, col->columnName, dialect));
		return result;
	}
	if (const auto *tmpl = dynamic_cast<const r2rml::TemplateTermMap *>(&termMap)) {
		std::vector<TemplateSegment> segments = parseTemplate(tmpl->templateString);
		SqlExpr result;
		result.expr = buildProjectionSql(segments, sourceAlias, dialect);
		result.requiredNonNullColumns = qualifiedColumnRefs(referencedColumns(segments), sourceAlias, dialect);
		return result;
	}
	if (const auto *constant = dynamic_cast<const r2rml::ConstantTermMap *>(&termMap)) {
		SqlExpr result;
		result.expr = dialect.stringLiteral(constantTermMapText(*constant));
		return result;
	}
	throw std::logic_error("termMapToSqlExpr: unrecognized TermMap subtype");
}

InversionResult invertTermMapAgainstBoundTerm(const r2rml::TermMap &termMap, const sparql::ast::Term &boundTerm,
                                              const std::string &sourceAlias, const SqlDialect &dialect) {
	InversionResult result;
	const std::string boundValue = termLexicalForm(boundTerm);

	if (const auto *constant = dynamic_cast<const r2rml::ConstantTermMap *>(&termMap)) {
		result.possible = (constantTermMapText(*constant) == boundValue);
		return result;
	}
	if (const auto *col = dynamic_cast<const r2rml::ColumnTermMap *>(&termMap)) {
		result.possible = true;
		result.whereConditions.push_back(columnExpr(sourceAlias, col->columnName, dialect) + " = " +
		                                 dialect.stringLiteral(boundValue));
		return result;
	}
	if (const auto *tmpl = dynamic_cast<const r2rml::TemplateTermMap *>(&termMap)) {
		std::vector<TemplateSegment> segments = parseTemplate(tmpl->templateString);
		InversionOutcome outcome = invertTemplate(segments, boundValue);
		if (outcome.kind == InversionKind::NeverMatches) {
			result.possible = false;
			return result;
		}
		result.possible = true;
		if (outcome.kind == InversionKind::WholeTemplateMatch) {
			result.whereConditions.push_back(buildProjectionSql(segments, sourceAlias, dialect) + " = " +
			                                 dialect.stringLiteral(boundValue));
			return result;
		}
		for (const auto &columnValue : outcome.columnValues) {
			result.whereConditions.push_back(columnExpr(sourceAlias, columnValue.first, dialect) + " = " +
			                                 dialect.stringLiteral(columnValue.second));
		}
		return result;
	}
	throw std::logic_error("invertTermMapAgainstBoundTerm: unrecognized TermMap subtype");
}

} // namespace sparql2sql
