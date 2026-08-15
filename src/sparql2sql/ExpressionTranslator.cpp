#include "sparql2sql/ExpressionTranslator.h"

#include <algorithm>
#include <iterator>
#include <set>
#include <vector>

#include "sparql2sql/ExprAnalysis.h"
#include "sparql2sql/PatternFolder.h"
#include "sparql2sql/SqlDialect.h"
#include "sparql2sql/TagDemand.h"
#include "sparql2sql/TagSql.h"
#include "sparql2sql/TermInference.h"
#include "sparql2sql/TermInfo.h"
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

TermSql trTerm(const Expression &e, const TranslatedPattern &scope, const std::string &alias, TranslationContext &ctx) {
	return translateTerm(e, scope, alias, ctx);
}

// The tag of a term-kind builtin's single argument, or a TranslationError
// explaining that neither the mapping nor a materialised tag column can answer.
// The error is the same refusal this translator has always made for these
// builtins; what changed is that it is now the last resort rather than the only
// outcome.
std::string requireTag(const TermSql &arg, const char *what) {
	if (arg.tag.empty()) {
		throw TranslationError(std::string("unsupported: ") + what +
		                       " - the RDF term dimension of its argument is neither determined by the R2RML mapping "
		                       "nor available as a runtime tag (the argument is a computed expression whose dimension "
		                       "this translator cannot derive).");
	}
	return arg.tag;
}

// Pair a constant tag with a value that may evaluate to SQL NULL.
//
// Required by TermSql's invariant: a computed value goes through TRY_CAST in
// most of this file, so it can be NULL even where the *type* it would have had
// is statically certain. Leaving a non-NULL tag beside a NULL value would make
// the term read as "bound, and of this type" when it is unbound.
std::string nullGuardedTag(const std::string &valueSql, const std::string &tagSql) {
	return "(CASE WHEN " + valueSql + " IS NULL THEN NULL ELSE " + tagSql + " END)";
}

// Whether an expression can evaluate to SQL NULL, i.e. can be unbound. Only a
// reference to an optional variable can be; a computed expression over bound
// inputs cannot introduce unboundedness in this representation.
bool mayBeUnbound(const Expression &e, const TranslatedPattern &scope) {
	std::vector<std::string> refs;
	collectVarRefs(e, refs);
	for (const auto &v : refs) {
		if (scope.optionalVars.count(v) != 0) {
			return true;
		}
	}
	return false;
}

// Guards a folded boolean constant on either operand being unbound (SQL NULL),
// mirroring foldedConstant() below but for a two-operand fold: SPARQL's
// treat-an-error-as-false semantics fall through to "unbound", so a FILTER
// over `?opt = <iri>` must still drop the row when ?opt is unbound rather than
// answering the statically-proven-different-kinds constant.
std::string foldedConstantTwoOperand(bool value, const std::string &leftSql, bool leftNullable,
                                     const std::string &rightSql, bool rightNullable, const SqlDialect &dialect) {
	if (!leftNullable && !rightNullable) {
		return dialect.booleanLiteral(value);
	}
	std::string guard = leftNullable ? (leftSql + " IS NULL") : std::string();
	if (rightNullable) {
		guard += (guard.empty() ? "" : " OR ") + rightSql + " IS NULL";
	}
	return "(CASE WHEN " + guard + " THEN NULL ELSE " + dialect.booleanLiteral(value) + " END)";
}

// A comparison specialised by whatever the R2RML mapping statically proves
// about the two operands. First match wins:
//
//   1. Kinds provably differ, and the operator is = or <> - RDF term equality
//      says an IRI never equals a literal, so fold to a constant. Deliberately
//      NOT extended to < / >, which is a genuine type error, nor to a datatype
//      mismatch within Literal (also an error, and folding it to FALSE would be
//      wrong rather than merely imprecise).
//   2. Both numeric  -> compare as DOUBLE, dropping the string fallback.
//   3. Both temporal -> compare as DATE/TIMESTAMP. A real correctness fix, not
//      just a size win: lexicographic dateTime comparison is wrong across UTC
//      offsets and non-canonical forms.
//   4. Both stringy and identically typed -> bare VARCHAR comparison, dropping
//      both the cast and the wrapper. This is the very common
//      `FILTER(?name = "SMITH")`.
//   5. Otherwise, if both operands carry a runtime tag -> dispatch on the two
//      tags per row (TagSql.h's dynamicEquality/dynamicOrdering), which is the
//      only branch that can answer correctly when the operands' dimensions vary
//      from row to row. Its own last resort for a term whose datatype the
//      mapping cannot determine is (6), so nothing regresses.
//   6. Otherwise -> the untyped fallback, exactly as before.
//
// Every typed branch keeps TRY_CAST rather than CAST: a declared rr:datatype
// can lie about a dirty column, and a hard cast error would kill the whole
// query where TRY_CAST yields NULL and merely drops the row.
std::string comparison(const TermSql &lhs, const TermSql &rhs, const std::string &op, const SqlDialect &dialect,
                       bool leftNullable, bool rightNullable) {
	const std::string &left = lhs.value;
	const std::string &right = rhs.value;
	const TermInfo &leftInfo = lhs.staticInfo;
	const TermInfo &rightInfo = rhs.staticInfo;
	const bool equality = (op == "=" || op == "<>");
	if (equality && leftInfo.kindKnown() && rightInfo.kindKnown() && leftInfo.kind != rightInfo.kind) {
		return foldedConstantTwoOperand(op == "<>", left, leftNullable, right, rightNullable, dialect);
	}
	if (leftInfo.isNumeric() && rightInfo.isNumeric()) {
		return "(" + dialect.tryCastToDouble(left) + " " + op + " " + dialect.tryCastToDouble(right) + ")";
	}
	if (leftInfo.isTemporal() && rightInfo.isTemporal()) {
		const bool bothDates = leftInfo.datatypeIri == xsd::kDate && rightInfo.datatypeIri == xsd::kDate;
		const std::string l = bothDates ? dialect.tryCastToDate(left) : dialect.tryCastToTimestamp(left);
		const std::string r = bothDates ? dialect.tryCastToDate(right) : dialect.tryCastToTimestamp(right);
		return "(" + l + " " + op + " " + r + ")";
	}
	if (leftInfo.isStringy() && rightInfo.isStringy() && leftInfo.datatypeIri == rightInfo.datatypeIri &&
	    leftInfo.lang == rightInfo.lang) {
		return "(" + left + " " + op + " " + right + ")";
	}
	std::string fallback = untypedComparison(left, right, op, dialect);
	if (!lhs.tag.empty() && !rhs.tag.empty()) {
		return equality ? dynamicEquality(left, lhs.tag, right, rhs.tag, op == "<>", fallback, dialect)
		                : dynamicOrdering(left, lhs.tag, right, rhs.tag, op, fallback, dialect);
	}
	return fallback;
}

// Arithmetic, staying integral when the mapping proves both operands are.
//
// Without this, `?a + 1` over an xsd:integer column renders "10.0": the DOUBLE
// round-trip is visible in the result text, not just in the intermediate. Note
// this is also what makes inferExprTermInfo's xsd:integer answer for integral
// addition *true* rather than a convenient fiction - the two must agree, or a
// downstream DATATYPE() would name a datatype whose canonical lexical form
// isn't the text in the column.
//
// Division is excluded deliberately: integer division is not integral.
std::string arithmetic(const std::string &left, const TermInfo &leftInfo, const std::string &right,
                       const TermInfo &rightInfo, const std::string &op, const SqlDialect &dialect) {
	if (op != "/" && leftInfo.isIntegral() && rightInfo.isIntegral()) {
		return "CAST((" + dialect.tryCastToBigInt(left) + " " + op + " " + dialect.tryCastToBigInt(right) +
		       ") AS VARCHAR)";
	}
	return "CAST((" + dialect.tryCastToDouble(left) + " " + op + " " + dialect.tryCastToDouble(right) + ") AS VARCHAR)";
}

std::string translateBinary(const BinaryExpr &b, const TranslatedPattern &scope, const std::string &alias,
                            TranslationContext &ctx) {
	const SqlDialect &dialect = ctx.dialect();
	const TermSql lhs = trTerm(*b.left, scope, alias, ctx);
	const TermSql rhs = trTerm(*b.right, scope, alias, ctx);
	const std::string &l = lhs.value;
	const std::string &r = rhs.value;
	const TermInfo &li = lhs.staticInfo;
	const TermInfo &ri = rhs.staticInfo;
	const bool leftNullable = mayBeUnbound(*b.left, scope);
	const bool rightNullable = mayBeUnbound(*b.right, scope);
	switch (b.op) {
	case BinaryOp::Or:
		return "(" + l + " OR " + r + ")";
	case BinaryOp::And:
		return "(" + l + " AND " + r + ")";
	case BinaryOp::Eq:
		return comparison(lhs, rhs, "=", dialect, leftNullable, rightNullable);
	case BinaryOp::Ne:
		return comparison(lhs, rhs, "<>", dialect, leftNullable, rightNullable);
	case BinaryOp::Lt:
		return comparison(lhs, rhs, "<", dialect, leftNullable, rightNullable);
	case BinaryOp::Gt:
		return comparison(lhs, rhs, ">", dialect, leftNullable, rightNullable);
	case BinaryOp::Le:
		return comparison(lhs, rhs, "<=", dialect, leftNullable, rightNullable);
	case BinaryOp::Ge:
		return comparison(lhs, rhs, ">=", dialect, leftNullable, rightNullable);
	case BinaryOp::Add:
		return arithmetic(l, li, r, ri, "+", dialect);
	case BinaryOp::Sub:
		return arithmetic(l, li, r, ri, "-", dialect);
	case BinaryOp::Mul:
		return arithmetic(l, li, r, ri, "*", dialect);
	case BinaryOp::Div:
		return arithmetic(l, li, r, ri, "/", dialect);
	}
	throw std::logic_error("translateBinary: unhandled BinaryOp");
}

std::string translateUnary(const UnaryExpr &u, const TranslatedPattern &scope, const std::string &alias,
                           TranslationContext &ctx) {
	const SqlDialect &dialect = ctx.dialect();
	std::string operand = tr(*u.operand, scope, alias, ctx);
	// As for binary arithmetic: keep a statically integral operand integral, so
	// -?a over an xsd:integer column renders "-10" rather than "-10.0".
	const bool integral = inferExprTermInfo(*u.operand, scope).isIntegral();
	const std::string numeric = integral ? dialect.tryCastToBigInt(operand) : dialect.tryCastToDouble(operand);
	switch (u.op) {
	case UnaryOp::Not:
		return "(NOT (" + operand + "))";
	case UnaryOp::Plus:
		return "CAST((+" + numeric + ") AS VARCHAR)";
	case UnaryOp::Minus:
		return "CAST((-" + numeric + ") AS VARCHAR)";
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
	// The EXISTS body is its own algebra tree with its own FILTERs, so it gets
	// its own tag-demand passes, in the same order as the top level's.
	markJoinKeyTagNeeds(*nestedNode, ctx);
	OptimizerOptions opts;
	opts.topLevelDistinct = true;
	opts.catalog = ctx.catalog();
	opts.ctx = &ctx;
	nestedNode = optimize(std::move(nestedNode), opts);
	markExpressionTagNeeds(*nestedNode, nullptr, ctx);
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

// A term-kind builtin resolved statically from the mapping, folded to a SQL
// constant. When the operand may be unbound the constant is NULL-guarded.
//
// SPARQL says isIRI(unbound) is an *error*. This translator spells "unbound" as
// SQL NULL, and NULL is precisely "error/unknown" in both contexts that matter:
// in a FILTER it drops the row (matching SPARQL's treat-an-error-as-false), and
// in a BIND it leaves the new variable unbound (matching "no binding on
// error"). So the guarded form is strictly more faithful than a bare FALSE, and
// it costs nothing when the operand is provably bound.
std::string foldedConstant(bool value, const std::string &operandSql, bool nullable, const SqlDialect &dialect) {
	if (!nullable) {
		return dialect.booleanLiteral(value);
	}
	return "(CASE WHEN " + operandSql + " IS NULL THEN NULL ELSE " + dialect.booleanLiteral(value) + " END)";
}

std::string foldedString(const std::string &value, const std::string &operandSql, bool nullable,
                         const SqlDialect &dialect) {
	if (!nullable) {
		return dialect.stringLiteral(value);
	}
	return "(CASE WHEN " + operandSql + " IS NULL THEN NULL ELSE " + dialect.stringLiteral(value) + " END)";
}

const char *builtinName(BuiltinFunction fn) {
	switch (fn) {
	case BuiltinFunction::Lang:
		return "lang()";
	case BuiltinFunction::Datatype:
		return "datatype()";
	case BuiltinFunction::IsIri:
		return "isIRI()";
	case BuiltinFunction::IsUri:
		return "isURI()";
	case BuiltinFunction::IsBlank:
		return "isBLANK()";
	case BuiltinFunction::IsLiteral:
		return "isLITERAL()";
	default:
		return "this builtin";
	}
}

TranslationError unknownTermKind(BuiltinFunction fn) {
	return TranslationError(std::string("unsupported: ") + builtinName(fn) +
	                        " - the RDF term kind of its argument is not statically determined by the R2RML mapping "
	                        "(the candidate term maps disagree, or the value comes from a computed expression). This "
	                        "translator resolves term-kind builtins at translation time from the mapping's "
	                        "rr:termType/rr:datatype/rr:language, so it cannot evaluate them per row.");
}

// RFC4647 basic filtering, as SPARQL's langMatches() requires. The range must
// be a constant (mirroring regex()'s flags argument); the tag need not be, so
// this still works over a BIND-ed or STRLANG-constructed tag.
std::string langMatchesSql(const std::string &tagSql, const std::string &range, const SqlDialect &dialect) {
	if (range == "*") {
		return "(" + tagSql + " <> " + dialect.stringLiteral("") + ")";
	}
	const std::string lowered = "LOWER(" + tagSql + ")";
	return "(" + lowered + " = LOWER(" + dialect.stringLiteral(range) + ") OR STARTS_WITH(" + lowered + ", LOWER(" +
	       dialect.stringLiteral(range + "-") + ")))";
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
	case BuiltinFunction::Ceil:
	case BuiltinFunction::Floor:
	case BuiltinFunction::Round: {
		// Integral in, integral out, matching arithmetic() and SUM(). Without
		// this, ABS(?n) over an xsd:integer column renders "10.0" while
		// inferExprTermInfo reports xsd:integer for it - a datatype whose
		// canonical lexical form is not the text in the column, which is the one
		// thing the term dimension must never claim. ABS is the only one that
		// changes anything numerically; CEIL/FLOOR/ROUND of an integer are the
		// integer, so for them this only fixes the rendering.
		const char *fn = call.fn == BuiltinFunction::Abs     ? "ABS("
		                 : call.fn == BuiltinFunction::Ceil  ? "CEIL("
		                 : call.fn == BuiltinFunction::Floor ? "FLOOR("
		                                                     : "ROUND(";
		if (inferExprTermInfo(*args.at(0), scope).isIntegral()) {
			return "CAST(" + std::string(fn) + dialect.tryCastToBigInt(argSql(0)) + ") AS VARCHAR)";
		}
		return "CAST(" + std::string(fn) + dialect.tryCastToDouble(argSql(0)) + ") AS VARCHAR)";
	}
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
	case BuiltinFunction::SameTerm: {
		// RDF term identity: same lexical form AND same dimension. String equality
		// alone wrongly accepts a cross-kind or cross-datatype match
		// ("1"^^xsd:integer vs "1"^^xsd:string, or an IRI whose text equals a
		// literal's). Where the mapping proves the dimensions differ, fold to
		// FALSE; where it proves they agree, string equality is exactly right and
		// the SQL is unchanged; otherwise compare the runtime tags too.
		const TermSql lhs = trTerm(*args.at(0), scope, alias, ctx);
		const TermSql rhs = trTerm(*args.at(1), scope, alias, ctx);
		const TermInfo &a = lhs.staticInfo;
		const TermInfo &b = rhs.staticInfo;
		const bool kindsDiffer = a.kindKnown() && b.kindKnown() && a.kind != b.kind;
		const bool datatypesDiffer = a.isKnownLiteral() && b.isKnownLiteral() && !a.datatypeIri.empty() &&
		                             !b.datatypeIri.empty() && a.datatypeIri != b.datatypeIri;
		const bool langsDiffer =
		    a.isKnownLiteral() && b.isKnownLiteral() && !a.lang.empty() && !b.lang.empty() && a.lang != b.lang;
		if (kindsDiffer || datatypesDiffer || langsDiffer) {
			return foldedConstantTwoOperand(false, lhs.value, mayBeUnbound(*args[0], scope), rhs.value,
			                                mayBeUnbound(*args[1], scope), dialect);
		}
		const bool dimensionsAgreeStatically =
		    isFullyDetermined(a) && isFullyDetermined(b) && encodeTag(a) == encodeTag(b);
		if (!dimensionsAgreeStatically && !lhs.tag.empty() && !rhs.tag.empty()) {
			return "(" + lhs.value + " = " + rhs.value + " AND " + lhs.tag + " = " + rhs.tag + ")";
		}
		return "(" + lhs.value + " = " + rhs.value + ")";
	}
	case BuiltinFunction::IsNumeric: {
		// Two conditions, both required: the term must be a literal in the numeric
		// value space, and its lexical form must actually parse. The tag settles
		// the first when it varies per row; without one, the cast test alone is
		// what this translator has always done.
		const TermSql arg = trTerm(*args.at(0), scope, alias, ctx);
		std::string parses = "(" + dialect.tryCastToDouble(arg.value) + " IS NOT NULL)";
		if (arg.staticInfo.isNumeric()) {
			return parses;
		}
		if (isFullyDetermined(arg.staticInfo) && encodeTag(arg.staticInfo) != kTagLiteralUntyped) {
			// Known to be an IRI, a blank node, or a literal of a known
			// non-numeric datatype - none of which is numeric however its lexical
			// form happens to parse.
			return foldedConstant(false, arg.value, mayBeUnbound(*args[0], scope), dialect);
		}
		if (!arg.tag.empty()) {
			return "(" + tagIsNumeric(arg.tag, dialect) + " AND " + parses + ")";
		}
		// kTagLiteralUntyped with no tag column: the mapping does not say what
		// this literal's datatype is, so the cast test alone - what this
		// translator has always emitted - remains the best available answer.
		return parses;
	}
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
	case BuiltinFunction::IsIri:
	case BuiltinFunction::IsUri:
	case BuiltinFunction::IsBlank:
	case BuiltinFunction::IsLiteral: {
		RdfTermKind wanted = RdfTermKind::Iri;
		if (call.fn == BuiltinFunction::IsBlank) {
			wanted = RdfTermKind::BlankNode;
		} else if (call.fn == BuiltinFunction::IsLiteral) {
			wanted = RdfTermKind::Literal;
		}
		const TermSql arg = trTerm(*args.at(0), scope, alias, ctx);
		if (arg.staticInfo.kindKnown()) {
			// The mapping settles it: fold, exactly as before tags existed.
			return foldedConstant(arg.staticInfo.kind == wanted, arg.value, mayBeUnbound(*args[0], scope), dialect);
		}
		// The kind varies from row to row (candidate term maps disagree, or a
		// UNION mixes arms). Test the runtime tag instead of refusing.
		return tagIsKind(requireTag(arg, builtinName(call.fn)), wanted, dialect);
	}
	case BuiltinFunction::Lang: {
		const TermSql arg = trTerm(*args.at(0), scope, alias, ctx);
		const TermInfo &info = arg.staticInfo;
		if (isFullyDetermined(info)) {
			if (info.kind != RdfTermKind::Literal) {
				throw TranslationError(
				    "lang(): the argument is statically an IRI or blank node, which has no language tag");
			}
			// A literal with no rr:language at all has the empty tag - which is a
			// known answer, not an unknown one.
			return foldedString(info.lang, arg.value, mayBeUnbound(*args[0], scope), dialect);
		}
		return tagLang(requireTag(arg, "lang()"), dialect);
	}
	case BuiltinFunction::Datatype: {
		const TermSql arg = trTerm(*args.at(0), scope, alias, ctx);
		const TermInfo &info = arg.staticInfo;
		if (isFullyDetermined(info) && !info.datatypeIri.empty()) {
			if (info.kind != RdfTermKind::Literal) {
				throw TranslationError(
				    "datatype(): the argument is statically an IRI or blank node, which has no datatype");
			}
			return foldedString(info.datatypeIri, arg.value, mayBeUnbound(*args[0], scope), dialect);
		}
		if (isFullyDetermined(info) && info.kind != RdfTermKind::Literal) {
			throw TranslationError(
			    "datatype(): the argument is statically an IRI or blank node, which has no datatype");
		}
		// Either the datatype varies per row, or it is a literal the mapping does
		// not type at all (a bare rr:column with no rr:datatype and no
		// TypeCatalog). tagDatatypeIri answers the first and yields SQL NULL - a
		// type error, which a FILTER drops and a BIND leaves unbound - for the
		// second, which is strictly better than refusing to translate the query.
		return tagDatatypeIri(requireTag(arg, "datatype()"), dialect);
	}
	case BuiltinFunction::LangMatches: {
		const std::string range = literalTextOrThrow(*args.at(1), "langMatches() language range");
		return langMatchesSql(argSql(0), range, dialect);
	}
	case BuiltinFunction::Strdt:
	case BuiltinFunction::Strlang:
		// Pure lexical pass-throughs: the physical representation already *is*
		// the lexical form, so the value never changes. All the work is in the
		// tag (see dynamicTagOf), which is what types every downstream
		// FILTER/ORDER BY over the result - the escape hatch for mappings that
		// declare no rr:datatype. A non-constant datatype/language argument used
		// to be rejected here; it now simply produces a computed tag.
		return "(" + argSql(0) + ")";
	case BuiltinFunction::IriFn:
	case BuiltinFunction::UriFn:
		// The lexical form of an IRI term IS its IRI string, so this is another
		// pass-through; only the tag changes (to kTagIri). No relative-IRI
		// resolution against a base is performed - see doc/api.md.
		return "(" + argSql(0) + ")";
	case BuiltinFunction::Bnode: {
		// A fresh blank node per solution. With an argument, SPARQL requires the
		// same label for equal argument values within one evaluation, which a
		// deterministic function of the argument gives directly; without one,
		// every call must differ, which uuid() gives.
		if (args.empty()) {
			return dialect.concat({dialect.stringLiteral("b"), "CAST(uuid() AS VARCHAR)"});
		}
		return dialect.concat({dialect.stringLiteral("b"), "md5(" + argSql(0) + ")"});
	}
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
	if (iri == xsd::kString) {
		return "(" + arg + ")";
	}
	if (iri == xsd::kInteger || iri == xsd::kLong || iri == xsd::kInt || iri == xsd::kShort || iri == xsd::kByte) {
		// Truncates toward zero via DuckDB's own DOUBLE->BIGINT CAST
		// semantics, not a hand-rolled XPath-conformant conversion - same
		// stance as ROUND()/CEIL()/FLOOR() above. The narrower integer
		// subtypes (long/int/short/byte) are treated identically to
		// xsd:integer - no range clamping to their narrower XSD bounds.
		return "CAST(CAST(" + dialect.tryCastToDouble(arg) + " AS BIGINT) AS VARCHAR)";
	}
	if (iri == xsd::kDecimal) {
		return "CAST(" + dialect.tryCastToDecimal(arg) + " AS VARCHAR)";
	}
	if (iri == xsd::kBoolean) {
		// DuckDB's VARCHAR->BOOLEAN cast already accepts XSD boolean's
		// lexical space (true/false/1/0), and BOOLEAN->VARCHAR renders
		// lowercase true/false, matching XSD's canonical lexical form.
		return "CAST(" + dialect.tryCastToBoolean(arg) + " AS VARCHAR)";
	}
	if (iri == xsd::kDate) {
		// DuckDB's DATE->VARCHAR cast already renders YYYY-MM-DD, matching
		// XSD date's canonical form directly.
		return "CAST(" + dialect.tryCastToDate(arg) + " AS VARCHAR)";
	}
	if (iri == xsd::kDateTime) {
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
	const TermInfo argInfo = (agg.star || !agg.arg) ? TermInfo() : inferExprTermInfo(*agg.arg, scope);

	switch (agg.aggKind) {
	case AggregateKind::Count:
		return "CAST(COUNT(" + distinctPrefix + arg + ") AS VARCHAR)";
	case AggregateKind::Sum:
		// Integral in, integral out, matching arithmetic() - otherwise SUM over
		// an xsd:integer column would render "30.0".
		if (argInfo.isIntegral()) {
			return "CAST(SUM(" + distinctPrefix + dialect.tryCastToBigInt(arg) + ") AS VARCHAR)";
		}
		return "CAST(SUM(" + distinctPrefix + dialect.tryCastToDouble(arg) + ") AS VARCHAR)";
	case AggregateKind::Avg:
		return "CAST(AVG(" + distinctPrefix + dialect.tryCastToDouble(arg) + ") AS VARCHAR)";
	case AggregateKind::Min:
	case AggregateKind::Max: {
		// Aggregate in the argument's static type when the mapping declares one;
		// otherwise fall back to a lexicographic MIN/MAX over the raw text
		// (which is what every untyped variable still gets).
		//
		// Behaviour worth knowing: a typed MIN/MAX returns the SQL engine's
		// canonical rendering of the winning value, so MIN over xsd:integer
		// values "007" and "42" yields "7", not "007". That is the correct
		// XSD-canonical answer, but it is different text than the untyped path
		// produces - see doc/api.md.
		const std::string fn = (agg.aggKind == AggregateKind::Min) ? "MIN(" : "MAX(";
		if (argInfo.isIntegral()) {
			return "CAST(" + fn + distinctPrefix + dialect.tryCastToBigInt(arg) + ") AS VARCHAR)";
		}
		if (argInfo.isNumeric()) {
			return "CAST(" + fn + distinctPrefix + dialect.tryCastToDouble(arg) + ") AS VARCHAR)";
		}
		if (argInfo.datatypeIri == xsd::kDate) {
			return "CAST(" + fn + distinctPrefix + dialect.tryCastToDate(arg) + ") AS VARCHAR)";
		}
		if (argInfo.isTemporal()) {
			// Reuse translateXsdCast's canonicalisation: DuckDB renders a
			// TIMESTAMP with a space separator, XSD dateTime wants 'T'.
			return "REPLACE(CAST(" + fn + distinctPrefix + dialect.tryCastToTimestamp(arg) + ") AS VARCHAR), ' ', 'T')";
		}
		return fn + distinctPrefix + arg + ")";
	}
	case AggregateKind::Sample:
		return dialect.anyValueAgg(arg);
	case AggregateKind::GroupConcat: {
		std::string separator = dialect.stringLiteral(agg.hasSeparator ? agg.separator : " ");
		return dialect.stringAgg(arg, separator, agg.distinct);
	}
	}
	throw std::logic_error("translateAggregate: unhandled AggregateKind");
}

// The runtime tag of an expression whose dimension the mapping does NOT settle.
// Empty when none can be produced, which every caller degrades on.
//
// Deliberately short: for all but a handful of constructs the dimension IS
// statically determined (inferExprTermInfo already knows STR() is xsd:string,
// STRLEN() xsd:integer, a cast its target IRI, ...), and translateTerm lowers
// that to a constant without coming here at all. What is left is exactly the
// constructs whose dimension can genuinely vary per row.
std::string dynamicTagOf(const Expression &expr, const TranslatedPattern &scope, const std::string &alias,
                         TranslationContext &ctx) {
	const SqlDialect &dialect = ctx.dialect();
	switch (expr.kind()) {
	case ExprKind::VarRef: {
		// The one true runtime source. Present only if the tag-demand pass asked
		// for this variable's tag column to be materialised.
		const std::string &name = static_cast<const VarExpr &>(expr).var->name;
		return ctx.needsTag(name) ? (alias + "." + mangleVarTag(name, dialect)) : std::string();
	}
	case ExprKind::BuiltInCall: {
		const auto &call = static_cast<const BuiltInCallExpr &>(expr);
		switch (call.fn) {
		case BuiltinFunction::Strdt:
		case BuiltinFunction::Strlang: {
			// The whole point of these two: they *construct* a dimension, so a
			// per-row datatype IRI or language tag is now expressible.
			if (call.args.size() < 2) {
				return std::string();
			}
			const TermSql dimension = trTerm(*call.args[1], scope, alias, ctx);
			const char prefix = call.fn == BuiltinFunction::Strdt ? kTagDatatypePrefix : kTagLangPrefix;
			return dialect.concat({dialect.stringLiteral(std::string(1, prefix)), dimension.value});
		}
		case BuiltinFunction::Coalesce: {
			// The tag has to follow whichever argument COALESCE actually returned.
			std::string sql = "(CASE";
			for (const auto &arg : call.args) {
				const TermSql a = trTerm(*arg, scope, alias, ctx);
				if (a.tag.empty()) {
					return std::string();
				}
				sql += " WHEN " + a.value + " IS NOT NULL THEN " + a.tag;
			}
			return sql + " ELSE NULL END)";
		}
		case BuiltinFunction::If: {
			if (call.args.size() < 3) {
				return std::string();
			}
			const TermSql t = trTerm(*call.args[1], scope, alias, ctx);
			const TermSql f = trTerm(*call.args[2], scope, alias, ctx);
			if (t.tag.empty() || f.tag.empty()) {
				return std::string();
			}
			return "(CASE WHEN " + tr(*call.args[0], scope, alias, ctx) + " THEN " + t.tag + " ELSE " + f.tag + " END)";
		}
		default:
			return std::string();
		}
	}
	case ExprKind::Aggregate: {
		const auto &agg = static_cast<const AggregateExpr &>(expr);
		// MIN/MAX/SAMPLE return one of their input terms verbatim, so the result's
		// dimension is that term's - but *which* row won is only known at run
		// time, so the tag has to be aggregated alongside the value rather than
		// picked statically. Every other aggregate has a fixed result type that
		// inferExprTermInfo already reports.
		if (agg.star || !agg.arg) {
			return std::string();
		}
		if (agg.aggKind != AggregateKind::Min && agg.aggKind != AggregateKind::Max &&
		    agg.aggKind != AggregateKind::Sample) {
			return std::string();
		}
		const TermSql a = trTerm(*agg.arg, scope, alias, ctx);
		if (a.tag.empty()) {
			return std::string();
		}
		if (agg.aggKind == AggregateKind::Sample) {
			return dialect.anyValueAgg(a.tag);
		}
		// Ordered by the same key the value aggregate uses (the raw lexical form -
		// this branch is only reached when the dimension is not statically known,
		// which is exactly when translateAggregate falls back to a lexicographic
		// MIN/MAX), so the tag returned is the winning row's own.
		return dialect.argMinMaxBy(a.tag, a.value, agg.aggKind == AggregateKind::Max);
	}
	default:
		return std::string();
	}
}

} // namespace

TermSql translateTerm(const sparql::ast::Expression &expr, const TranslatedPattern &scope,
                      const std::string &scopeAlias, TranslationContext &ctx) {
	TermSql out;
	out.value = translateExpression(expr, scope, scopeAlias, ctx);
	out.staticInfo = inferExprTermInfo(expr, scope);
	if (!isFullyDetermined(out.staticInfo)) {
		out.tag = dynamicTagOf(expr, scope, scopeAlias, ctx);
		return out;
	}
	// The mapping settles the dimension, so the tag is a constant - but it still
	// has to be NULL wherever the value is, or an unbound term would read as
	// bound. A materialised tag column already satisfies that; a constant beside
	// a computed value (or an optional variable) has to be guarded explicitly.
	const std::string constant = tagLiteral(out.staticInfo, ctx.dialect());
	if (expr.kind() == ExprKind::VarRef) {
		const std::string &name = static_cast<const VarExpr &>(expr).var->name;
		out.tag = ctx.needsTag(name) ? (scopeAlias + "." + mangleVarTag(name, ctx.dialect()))
		                             : nullGuardedTag(out.value, constant);
		return out;
	}
	// A literal or IRI written out in the query text is never NULL, so it needs
	// no guard - and skipping it keeps constant folding readable.
	const bool neverNull = expr.kind() == ExprKind::Literal || expr.kind() == ExprKind::IriRef;
	out.tag = neverNull ? constant : nullGuardedTag(out.value, constant);
	return out;
}

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
		if (isXsdCastIri(fc.iri->value)) {
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
