#include "sparql2sql/ExpressionTranslator.h"

#include <algorithm>
#include <iterator>
#include <set>
#include <vector>

#include "sparql2sql/PatternFolder.h"
#include "sparql2sql/SqlDialect.h"
#include "sparql2sql/TranslationError.h"
#include "sparql2sql/ir/Optimizer.h"
#include "sparql2sql/ir/RelNode.h"
#include "sparql2sql/ir/SqlRenderer.h"

namespace sparql2sql {

namespace {

using sparql::ast::AggregateExpr;
using sparql::ast::AggregateKind;
using sparql::ast::BinaryExpr;
using sparql::ast::BinaryOp;
using sparql::ast::BuiltInCallExpr;
using sparql::ast::BuiltinFunction;
using sparql::ast::ExistsExpr;
using sparql::ast::Expression;
using sparql::ast::ExprKind;
using sparql::ast::FunctionCallExpr;
using sparql::ast::InExpr;
using sparql::ast::IriExpr;
using sparql::ast::LiteralExpr;
using sparql::ast::UnaryExpr;
using sparql::ast::UnaryOp;
using sparql::ast::VarExpr;

std::string tr(const Expression &e, const TranslatedPattern &scope, const std::string &alias, TranslationContext &ctx) {
	return translateExpression(e, scope, alias, ctx);
}

// A numeric-aware comparison: compares both operands as DOUBLE when both
// TRY_CAST successfully, otherwise falls back to a plain VARCHAR
// comparison. This lets both `FILTER(?price > 10)` (numeric) and
// `FILTER(?name < "M")` (lexicographic string ordering) work correctly
// without any static type inference across the VARCHAR-only representation
// (see doc/api.md's "Known limitations").
std::string numericAwareComparison(const std::string &left, const std::string &right, const std::string &op,
                                   const SqlDialect &dialect) {
	std::string leftNum = dialect.tryCastToDouble(left);
	std::string rightNum = dialect.tryCastToDouble(right);
	return "(CASE WHEN " + leftNum + " IS NOT NULL AND " + rightNum + " IS NOT NULL THEN (" + leftNum + " " + op + " " +
	       rightNum + ") ELSE (" + left + " " + op + " " + right + ") END)";
}

std::string arithmetic(const std::string &left, const std::string &right, const std::string &op,
                       const SqlDialect &dialect) {
	return "CAST((" + dialect.tryCastToDouble(left) + " " + op + " " + dialect.tryCastToDouble(right) + ") AS VARCHAR)";
}

std::string translateBinary(const BinaryExpr &b, const TranslatedPattern &scope, const std::string &alias,
                            TranslationContext &ctx) {
	const SqlDialect &dialect = ctx.dialect();
	std::string l = tr(*b.left, scope, alias, ctx);
	std::string r = tr(*b.right, scope, alias, ctx);
	switch (b.op) {
	case BinaryOp::Or:
		return "(" + l + " OR " + r + ")";
	case BinaryOp::And:
		return "(" + l + " AND " + r + ")";
	case BinaryOp::Eq:
		return numericAwareComparison(l, r, "=", dialect);
	case BinaryOp::Ne:
		return numericAwareComparison(l, r, "<>", dialect);
	case BinaryOp::Lt:
		return numericAwareComparison(l, r, "<", dialect);
	case BinaryOp::Gt:
		return numericAwareComparison(l, r, ">", dialect);
	case BinaryOp::Le:
		return numericAwareComparison(l, r, "<=", dialect);
	case BinaryOp::Ge:
		return numericAwareComparison(l, r, ">=", dialect);
	case BinaryOp::Add:
		return arithmetic(l, r, "+", dialect);
	case BinaryOp::Sub:
		return arithmetic(l, r, "-", dialect);
	case BinaryOp::Mul:
		return arithmetic(l, r, "*", dialect);
	case BinaryOp::Div:
		return arithmetic(l, r, "/", dialect);
	}
	throw std::logic_error("translateBinary: unhandled BinaryOp");
}

std::string translateUnary(const UnaryExpr &u, const TranslatedPattern &scope, const std::string &alias,
                           TranslationContext &ctx) {
	const SqlDialect &dialect = ctx.dialect();
	std::string operand = tr(*u.operand, scope, alias, ctx);
	switch (u.op) {
	case UnaryOp::Not:
		return "(NOT (" + operand + "))";
	case UnaryOp::Plus:
		return "CAST((+" + dialect.tryCastToDouble(operand) + ") AS VARCHAR)";
	case UnaryOp::Minus:
		return "CAST((-" + dialect.tryCastToDouble(operand) + ") AS VARCHAR)";
	}
	throw std::logic_error("translateUnary: unhandled UnaryOp");
}

std::string translateIn(const InExpr &in, const TranslatedPattern &scope, const std::string &alias,
                        TranslationContext &ctx) {
	std::string lhs = tr(*in.lhs, scope, alias, ctx);
	std::string sql = "(" + lhs + (in.negated ? " NOT IN (" : " IN (");
	for (std::size_t i = 0; i < in.list.size(); ++i) {
		if (i > 0) {
			sql += ", ";
		}
		sql += tr(*in.list[i], scope, alias, ctx);
	}
	sql += "))";
	return sql;
}

std::set<std::string> setIntersect(const std::set<std::string> &a, const std::set<std::string> &b) {
	std::set<std::string> out;
	std::set_intersection(a.begin(), a.end(), b.begin(), b.end(), std::inserter(out, out.begin()));
	return out;
}

std::string translateExists(const ExistsExpr &ex, const TranslatedPattern &scope, const std::string &alias,
                            TranslationContext &ctx) {
	RelNodePtr nestedNode = fold(*ex.pattern, ctx);
	// Optimize the correlated subquery body like a top-level pattern: an
	// existence check never cares about duplicate rows, so topLevelDistinct is
	// safe here (as for ASK). Without this, EXISTS bodies would miss flatten,
	// self-join elimination, and filter pushdown.
	//
	// NOTE: dropping the body's DISTINCT is only a *win* because the correlation
	// below is a plain equality. Against the older null-tolerant OR form the
	// engine could not decorrelate, so the inner relation was scanned per outer
	// row and the DISTINCT was load-bearing - stripping it there cost ~2.2x on a
	// 10M-row body. Do not reintroduce an OR'd correlation without revisiting
	// this flag.
	OptimizerOptions opts;
	opts.topLevelDistinct = true;
	opts.catalog = ctx.catalog();
	opts.ctx = &ctx;
	nestedNode = optimize(std::move(nestedNode), opts);
	TranslatedPattern nested = renderRelation(*nestedNode, ctx);
	std::set<std::string> shared = setIntersect(scope.allVars(), nested.allVars());
	std::string innerAlias = ctx.nextAlias();
	std::string innerSql;
	if (shared.empty()) {
		innerSql = "SELECT 1 FROM (" + nested.sql + ") AS " + innerAlias;
	} else {
		// Correlate on each shared variable, emitting an IS NULL disjunct only
		// for a side that can actually be unbound. An outer NULL means the
		// variable is not in the current solution's domain, so it constrains
		// nothing and must match any inner row; an inner NULL likewise. But when
		// both sides are guaranteed bound those disjuncts are dead, and dropping
		// them is what lets the engine decorrelate this into a hash semi-join
		// instead of a nested loop over the whole inner relation.
		std::string cond;
		bool first = true;
		for (const auto &v : shared) {
			std::string outerCol = alias + "." + mangleVar(v, ctx.dialect());
			std::string innerCol = innerAlias + "." + mangleVar(v, ctx.dialect());
			std::string one = outerCol + " = " + innerCol;
			if (scope.optionalVars.count(v) != 0) {
				one += " OR " + outerCol + " IS NULL";
			}
			if (nested.optionalVars.count(v) != 0) {
				one += " OR " + innerCol + " IS NULL";
			}
			if (one.find(" OR ") != std::string::npos) {
				one = "(" + one + ")";
			}
			if (!first) {
				cond += " AND ";
			}
			first = false;
			cond += one;
		}
		innerSql = "SELECT 1 FROM (" + nested.sql + ") AS " + innerAlias + " WHERE " + cond;
	}
	return ctx.dialect().existsClause(ex.negated, innerSql);
}

std::string literalTextOrThrow(const Expression &expr, const char *context) {
	if (expr.kind() != ExprKind::Literal) {
		throw TranslationError(std::string(context) + ": expected a string literal argument");
	}
	return static_cast<const LiteralExpr &>(expr).literal->lexicalForm;
}

std::string translateBuiltIn(const BuiltInCallExpr &call, const TranslatedPattern &scope, const std::string &alias,
                             TranslationContext &ctx) {
	const SqlDialect &dialect = ctx.dialect();
	const auto &args = call.args;

	auto argSql = [&](std::size_t i) {
		return tr(*args.at(i), scope, alias, ctx);
	};

	switch (call.fn) {
	case BuiltinFunction::Str:
		return "(" + argSql(0) + ")";
	case BuiltinFunction::Bound: {
		if (args.empty() || args[0]->kind() != ExprKind::VarRef) {
			throw TranslationError("bound(): argument must be a variable");
		}
		const std::string &name = static_cast<const VarExpr &>(*args[0]).var->name;
		if (!scope.allVars().count(name)) {
			throw TranslationError("bound(?" + name + "): variable not in scope");
		}
		return "(" + alias + "." + mangleVar(name, dialect) + " IS NOT NULL)";
	}
	case BuiltinFunction::Concat: {
		std::vector<std::string> parts;
		parts.reserve(args.size());
		for (std::size_t i = 0; i < args.size(); ++i) {
			parts.push_back(argSql(i));
		}
		return dialect.concat(parts);
	}
	case BuiltinFunction::Strlen:
		return "CAST(LENGTH(" + argSql(0) + ") AS VARCHAR)";
	case BuiltinFunction::Substr: {
		std::string source = argSql(0);
		std::string start = "CAST(" + dialect.tryCastToDouble(argSql(1)) + " AS BIGINT)";
		if (args.size() >= 3) {
			std::string len = "CAST(" + dialect.tryCastToDouble(argSql(2)) + " AS BIGINT)";
			return "SUBSTR(" + source + ", " + start + ", " + len + ")";
		}
		return "SUBSTR(" + source + ", " + start + ")";
	}
	case BuiltinFunction::Ucase:
		return "UPPER(" + argSql(0) + ")";
	case BuiltinFunction::Lcase:
		return "LOWER(" + argSql(0) + ")";
	case BuiltinFunction::Contains:
		return "CONTAINS(" + argSql(0) + ", " + argSql(1) + ")";
	case BuiltinFunction::Strstarts:
		return "STARTS_WITH(" + argSql(0) + ", " + argSql(1) + ")";
	case BuiltinFunction::Strends:
		return "ENDS_WITH(" + argSql(0) + ", " + argSql(1) + ")";
	case BuiltinFunction::Strbefore: {
		std::string a = argSql(0);
		std::string b = argSql(1);
		return "(CASE WHEN strpos(" + a + ", " + b + ") = 0 THEN '' ELSE SUBSTR(" + a + ", 1, strpos(" + a + ", " + b +
		       ") - 1) END)";
	}
	case BuiltinFunction::Strafter: {
		std::string a = argSql(0);
		std::string b = argSql(1);
		return "(CASE WHEN strpos(" + a + ", " + b + ") = 0 THEN '' ELSE SUBSTR(" + a + ", strpos(" + a + ", " + b +
		       ") + LENGTH(" + b + ")) END)";
	}
	case BuiltinFunction::Replace: {
		std::string a = argSql(0);
		std::string pattern = argSql(1);
		std::string replacement = argSql(2);
		return "regexp_replace(" + a + ", " + pattern + ", " + replacement +
		       (args.size() >= 4 ? ", " + argSql(3) : "") + ")";
	}
	case BuiltinFunction::Regex: {
		std::string text = argSql(0);
		std::string pattern = argSql(1);
		std::string flags = args.size() >= 3 ? literalTextOrThrow(*args[2], "regex() flags") : std::string();
		return dialect.regexMatch(text, pattern, flags, /*negated=*/false);
	}
	case BuiltinFunction::Abs:
		return "CAST(ABS(" + dialect.tryCastToDouble(argSql(0)) + ") AS VARCHAR)";
	case BuiltinFunction::Ceil:
		return "CAST(CEIL(" + dialect.tryCastToDouble(argSql(0)) + ") AS VARCHAR)";
	case BuiltinFunction::Floor:
		return "CAST(FLOOR(" + dialect.tryCastToDouble(argSql(0)) + ") AS VARCHAR)";
	case BuiltinFunction::Round:
		return "CAST(ROUND(" + dialect.tryCastToDouble(argSql(0)) + ") AS VARCHAR)";
	case BuiltinFunction::Coalesce: {
		std::string sql = "COALESCE(";
		for (std::size_t i = 0; i < args.size(); ++i) {
			if (i > 0) {
				sql += ", ";
			}
			sql += argSql(i);
		}
		sql += ")";
		return sql;
	}
	case BuiltinFunction::If:
		return "(CASE WHEN " + argSql(0) + " THEN " + argSql(1) + " ELSE " + argSql(2) + " END)";
	case BuiltinFunction::SameTerm:
		return "(" + argSql(0) + " = " + argSql(1) + ")";
	case BuiltinFunction::IsNumeric:
		return "(" + dialect.tryCastToDouble(argSql(0)) + " IS NOT NULL)";
	case BuiltinFunction::Md5:
		return "md5(" + argSql(0) + ")";
	case BuiltinFunction::Sha1:
		return "sha1(" + argSql(0) + ")";
	case BuiltinFunction::Sha256:
		return "sha256(" + argSql(0) + ")";
	case BuiltinFunction::Sha384:
		throw TranslationError("unsupported: SHA384() - DuckDB has no built-in sha384() scalar function");
	case BuiltinFunction::Sha512:
		throw TranslationError("unsupported: SHA512() - DuckDB has no built-in sha512() scalar function");
	case BuiltinFunction::Lang:
	case BuiltinFunction::LangMatches:
	case BuiltinFunction::Datatype:
	case BuiltinFunction::Strlang:
	case BuiltinFunction::Strdt:
	case BuiltinFunction::IsIri:
	case BuiltinFunction::IsUri:
	case BuiltinFunction::IsBlank:
	case BuiltinFunction::IsLiteral:
		throw TranslationError(
		    "unsupported: this builtin requires RDF term-kind/datatype/language tracking, which this translator "
		    "does not implement (every variable is represented as a plain SQL VARCHAR of the term's lexical form)");
	case BuiltinFunction::IriFn:
	case BuiltinFunction::UriFn:
	case BuiltinFunction::Bnode:
		throw TranslationError("unsupported: RDF term-construction functions (IRI()/URI()/BNODE()) are not supported");
	case BuiltinFunction::EncodeForUri:
		return dialect.percentEncode(argSql(0));
	case BuiltinFunction::Year:
		return "CAST(EXTRACT(YEAR FROM " + dialect.tryCastToTimestamp(argSql(0)) + ") AS VARCHAR)";
	case BuiltinFunction::Month:
		return "CAST(EXTRACT(MONTH FROM " + dialect.tryCastToTimestamp(argSql(0)) + ") AS VARCHAR)";
	case BuiltinFunction::Day:
		return "CAST(EXTRACT(DAY FROM " + dialect.tryCastToTimestamp(argSql(0)) + ") AS VARCHAR)";
	case BuiltinFunction::Hours:
		return "CAST(EXTRACT(HOUR FROM " + dialect.tryCastToTimestamp(argSql(0)) + ") AS VARCHAR)";
	case BuiltinFunction::Minutes:
		return "CAST(EXTRACT(MINUTE FROM " + dialect.tryCastToTimestamp(argSql(0)) + ") AS VARCHAR)";
	case BuiltinFunction::Seconds:
		return "CAST(EXTRACT(SECOND FROM " + dialect.tryCastToTimestamp(argSql(0)) + ") AS VARCHAR)";
	case BuiltinFunction::Timezone:
	case BuiltinFunction::Tz:
		throw TranslationError("unsupported: TIMEZONE()/TZ() require preserving a UTC offset, which this translator's "
		                       "plain-VARCHAR lexical-form term representation cannot carry");
	case BuiltinFunction::Now:
		return dialect.stringLiteral(ctx.nowLiteral());
	case BuiltinFunction::Rand:
		return "CAST(random() AS VARCHAR)";
	case BuiltinFunction::Uuid:
		return dialect.concat({dialect.stringLiteral("urn:uuid:"), "CAST(uuid() AS VARCHAR)"});
	case BuiltinFunction::Struuid:
		return "CAST(uuid() AS VARCHAR)";
	}
	throw std::logic_error("translateBuiltIn: unhandled BuiltinFunction");
}

const char *const kXsdInteger = "http://www.w3.org/2001/XMLSchema#integer";
const char *const kXsdDecimal = "http://www.w3.org/2001/XMLSchema#decimal";
const char *const kXsdDouble = "http://www.w3.org/2001/XMLSchema#double";
const char *const kXsdFloat = "http://www.w3.org/2001/XMLSchema#float";
const char *const kXsdString = "http://www.w3.org/2001/XMLSchema#string";
const char *const kXsdBoolean = "http://www.w3.org/2001/XMLSchema#boolean";
const char *const kXsdDateTime = "http://www.w3.org/2001/XMLSchema#dateTime";
const char *const kXsdDate = "http://www.w3.org/2001/XMLSchema#date";
const char *const kXsdLong = "http://www.w3.org/2001/XMLSchema#long";
const char *const kXsdInt = "http://www.w3.org/2001/XMLSchema#int";
const char *const kXsdShort = "http://www.w3.org/2001/XMLSchema#short";
const char *const kXsdByte = "http://www.w3.org/2001/XMLSchema#byte";

bool isXsdCastFunction(const std::string &iri) {
	return iri == kXsdInteger || iri == kXsdDecimal || iri == kXsdDouble || iri == kXsdFloat || iri == kXsdString ||
	       iri == kXsdBoolean || iri == kXsdDateTime || iri == kXsdDate || iri == kXsdLong || iri == kXsdInt ||
	       iri == kXsdShort || iri == kXsdByte;
}

// xsd:double/float round-trip through DOUBLE (the correct IEEE
// floating-point model for both); xsd:decimal gets its own fixed-point
// DECIMAL(38,18) path via dialect.tryCastToDecimal() instead, since DOUBLE
// silently loses precision on values like financial data that decimal is
// meant to represent exactly (up to its fixed scale).
std::string translateXsdCast(const std::string &iri, const FunctionCallExpr &fc, const TranslatedPattern &scope,
                             const std::string &alias, TranslationContext &ctx) {
	if (fc.args.size() != 1) {
		throw TranslationError("<" + iri + ">: expected exactly 1 argument, got " + std::to_string(fc.args.size()));
	}
	const SqlDialect &dialect = ctx.dialect();
	std::string arg = tr(*fc.args[0], scope, alias, ctx);
	if (iri == kXsdString) {
		return "(" + arg + ")";
	}
	if (iri == kXsdInteger || iri == kXsdLong || iri == kXsdInt || iri == kXsdShort || iri == kXsdByte) {
		// Truncates toward zero via DuckDB's own DOUBLE->BIGINT CAST
		// semantics, not a hand-rolled XPath-conformant conversion - same
		// stance as ROUND()/CEIL()/FLOOR() above. The narrower integer
		// subtypes (long/int/short/byte) are treated identically to
		// xsd:integer - no range clamping to their narrower XSD bounds.
		return "CAST(CAST(" + dialect.tryCastToDouble(arg) + " AS BIGINT) AS VARCHAR)";
	}
	if (iri == kXsdDecimal) {
		return "CAST(" + dialect.tryCastToDecimal(arg) + " AS VARCHAR)";
	}
	if (iri == kXsdBoolean) {
		// DuckDB's VARCHAR->BOOLEAN cast already accepts XSD boolean's
		// lexical space (true/false/1/0), and BOOLEAN->VARCHAR renders
		// lowercase true/false, matching XSD's canonical lexical form.
		return "CAST(" + dialect.tryCastToBoolean(arg) + " AS VARCHAR)";
	}
	if (iri == kXsdDate) {
		// DuckDB's DATE->VARCHAR cast already renders YYYY-MM-DD, matching
		// XSD date's canonical form directly.
		return "CAST(" + dialect.tryCastToDate(arg) + " AS VARCHAR)";
	}
	if (iri == kXsdDateTime) {
		// DuckDB renders TIMESTAMP->VARCHAR as "YYYY-MM-DD HH:MM:SS[.ffffff]";
		// swapping the separator space for 'T' reaches XSD dateTime's
		// canonical "YYYY-MM-DDTHH:MM:SS" form. Not a hand-rolled
		// XPath-conformant conversion (no explicit fractional-second/
		// timezone-offset handling beyond what DuckDB's own TIMESTAMP
		// rendering gives) - same fidelity stance as the integer cast above.
		return "REPLACE(CAST(" + dialect.tryCastToTimestamp(arg) + " AS VARCHAR), ' ', 'T')";
	}
	return "CAST(" + dialect.tryCastToDouble(arg) + " AS VARCHAR)";
}

std::string translateAggregate(const AggregateExpr &agg, const TranslatedPattern &scope, const std::string &alias,
                               TranslationContext &ctx) {
	const SqlDialect &dialect = ctx.dialect();
	std::string arg = agg.star ? "*" : tr(*agg.arg, scope, alias, ctx);
	std::string distinctPrefix = agg.distinct ? "DISTINCT " : "";

	switch (agg.aggKind) {
	case AggregateKind::Count:
		return "CAST(COUNT(" + distinctPrefix + arg + ") AS VARCHAR)";
	case AggregateKind::Sum:
		return "CAST(SUM(" + distinctPrefix + dialect.tryCastToDouble(arg) + ") AS VARCHAR)";
	case AggregateKind::Avg:
		return "CAST(AVG(" + distinctPrefix + dialect.tryCastToDouble(arg) + ") AS VARCHAR)";
	case AggregateKind::Min:
		// Lexicographic, not numeric-typed: a documented V1 limitation (no
		// static per-variable type inference across the VARCHAR-only
		// representation - see doc/api.md).
		return "MIN(" + distinctPrefix + arg + ")";
	case AggregateKind::Max:
		return "MAX(" + distinctPrefix + arg + ")";
	case AggregateKind::Sample:
		return dialect.anyValueAgg(arg);
	case AggregateKind::GroupConcat: {
		std::string separator = dialect.stringLiteral(agg.hasSeparator ? agg.separator : " ");
		return dialect.stringAgg(arg, separator, agg.distinct);
	}
	}
	throw std::logic_error("translateAggregate: unhandled AggregateKind");
}

} // namespace

std::string translateExpression(const sparql::ast::Expression &expr, const TranslatedPattern &scope,
                                const std::string &scopeAlias, TranslationContext &ctx) {
	switch (expr.kind()) {
	case ExprKind::Literal:
		return ctx.dialect().stringLiteral(static_cast<const LiteralExpr &>(expr).literal->lexicalForm);
	case ExprKind::VarRef: {
		const std::string &name = static_cast<const VarExpr &>(expr).var->name;
		if (!scope.allVars().count(name)) {
			throw TranslationError("FILTER/BIND/ORDER BY/HAVING reference to out-of-scope variable ?" + name);
		}
		return scopeAlias + "." + mangleVar(name, ctx.dialect());
	}
	case ExprKind::IriRef:
		return ctx.dialect().stringLiteral(static_cast<const IriExpr &>(expr).iri->value);
	case ExprKind::Unary:
		return translateUnary(static_cast<const UnaryExpr &>(expr), scope, scopeAlias, ctx);
	case ExprKind::Binary:
		return translateBinary(static_cast<const BinaryExpr &>(expr), scope, scopeAlias, ctx);
	case ExprKind::In:
		return translateIn(static_cast<const InExpr &>(expr), scope, scopeAlias, ctx);
	case ExprKind::FunctionCall: {
		const auto &fc = static_cast<const FunctionCallExpr &>(expr);
		if (isXsdCastFunction(fc.iri->value)) {
			return translateXsdCast(fc.iri->value, fc, scope, scopeAlias, ctx);
		}
		throw TranslationError("unsupported: non-builtin function call <" + fc.iri->value + "> is not supported");
	}
	case ExprKind::BuiltInCall:
		return translateBuiltIn(static_cast<const BuiltInCallExpr &>(expr), scope, scopeAlias, ctx);
	case ExprKind::Aggregate:
		return translateAggregate(static_cast<const AggregateExpr &>(expr), scope, scopeAlias, ctx);
	case ExprKind::Exists:
		return translateExists(static_cast<const ExistsExpr &>(expr), scope, scopeAlias, ctx);
	}
	throw std::logic_error("translateExpression: unhandled ExprKind");
}

} // namespace sparql2sql
