#include "sparql2sql/TermInference.h"

#include "sparql2sql/TermMapSql.h"

namespace sparql2sql {

namespace {

using sparql::ast::AggregateExpr;
using sparql::ast::AggregateKind;
using sparql::ast::BinaryExpr;
using sparql::ast::BinaryOp;
using sparql::ast::BuiltInCallExpr;
using sparql::ast::BuiltinFunction;
using sparql::ast::Expression;
using sparql::ast::ExprKind;
using sparql::ast::FunctionCallExpr;
using sparql::ast::IriExpr;
using sparql::ast::LiteralExpr;
using sparql::ast::UnaryExpr;
using sparql::ast::UnaryOp;
using sparql::ast::VarExpr;

TermInfo literalOf(const char *datatypeIri) {
	TermInfo out;
	out.kind = RdfTermKind::Literal;
	out.datatypeIri = datatypeIri;
	return out;
}

TermInfo iriTerm() {
	TermInfo out;
	out.kind = RdfTermKind::Iri;
	return out;
}

TermInfo infer(const Expression &e, const TranslatedPattern &scope);

// Numeric result of an arithmetic operator, matching what arithmetic() emits:
// integral in, integral out (via TRY_CAST(... AS BIGINT)); otherwise DOUBLE.
// Anything not provably numeric is Unknown - the emission still casts, but the
// result's datatype genuinely isn't known, because a non-castable operand
// yields NULL rather than a number.
TermInfo arithmeticResult(const TermInfo &l, const TermInfo &r) {
	if (l.isIntegral() && r.isIntegral()) {
		return literalOf(xsd::kInteger);
	}
	if (l.isNumeric() && r.isNumeric()) {
		return literalOf(xsd::kDouble);
	}
	return TermInfo();
}

TermInfo inferBinary(const BinaryExpr &b, const TranslatedPattern &scope) {
	switch (b.op) {
	case BinaryOp::Or:
	case BinaryOp::And:
	case BinaryOp::Eq:
	case BinaryOp::Ne:
	case BinaryOp::Lt:
	case BinaryOp::Gt:
	case BinaryOp::Le:
	case BinaryOp::Ge:
		return literalOf(xsd::kBoolean);
	case BinaryOp::Add:
	case BinaryOp::Sub:
	case BinaryOp::Mul:
		return arithmeticResult(infer(*b.left, scope), infer(*b.right, scope));
	case BinaryOp::Div:
		// SPARQL says integer/integer is xsd:decimal; the emission goes through
		// DOUBLE, so that is what the produced lexical form is.
		if (infer(*b.left, scope).isNumeric() && infer(*b.right, scope).isNumeric()) {
			return literalOf(xsd::kDouble);
		}
		return TermInfo();
	}
	return TermInfo();
}

TermInfo inferUnary(const UnaryExpr &u, const TranslatedPattern &scope) {
	switch (u.op) {
	case UnaryOp::Not:
		return literalOf(xsd::kBoolean);
	case UnaryOp::Plus:
	case UnaryOp::Minus: {
		const TermInfo operand = infer(*u.operand, scope);
		return arithmeticResult(operand, operand);
	}
	}
	return TermInfo();
}

TermInfo inferAggregate(const AggregateExpr &agg, const TranslatedPattern &scope) {
	switch (agg.aggKind) {
	case AggregateKind::Count:
		return literalOf(xsd::kInteger);
	case AggregateKind::Sum: {
		const TermInfo arg = agg.star || !agg.arg ? TermInfo() : infer(*agg.arg, scope);
		return arithmeticResult(arg, arg);
	}
	case AggregateKind::Avg:
		return literalOf(xsd::kDouble);
	case AggregateKind::Min:
	case AggregateKind::Max:
	case AggregateKind::Sample:
		// These pass one of the input values through unchanged, so they carry
		// its annotation. (For a typed MIN/MAX the value comes back in the SQL
		// engine's canonical rendering of that same datatype - still the
		// declared datatype, just possibly different text.)
		return agg.star || !agg.arg ? TermInfo() : infer(*agg.arg, scope);
	case AggregateKind::GroupConcat:
		return literalOf(xsd::kString);
	}
	return TermInfo();
}

// The second argument of STRDT()/STRLANG(), when it is a constant. Returns
// false for a per-row value, which the translator cannot type statically.
bool constantIri(const Expression &e, std::string &out) {
	if (e.kind() != ExprKind::IriRef) {
		return false;
	}
	out = static_cast<const IriExpr &>(e).iri->value;
	return true;
}

bool constantString(const Expression &e, std::string &out) {
	if (e.kind() != ExprKind::Literal) {
		return false;
	}
	out = static_cast<const LiteralExpr &>(e).literal->lexicalForm;
	return true;
}

TermInfo inferBuiltIn(const BuiltInCallExpr &b, const TranslatedPattern &scope) {
	const auto argInfo = [&](std::size_t i) {
		return i < b.args.size() ? infer(*b.args[i], scope) : TermInfo();
	};

	switch (b.fn) {
	// --- String-valued ---
	case BuiltinFunction::Str:
	case BuiltinFunction::Lang: // The tag itself is a simple literal.
	case BuiltinFunction::Concat:
	case BuiltinFunction::Substr:
	case BuiltinFunction::Ucase:
	case BuiltinFunction::Lcase:
	case BuiltinFunction::EncodeForUri:
	case BuiltinFunction::Strbefore:
	case BuiltinFunction::Strafter:
	case BuiltinFunction::Replace:
	case BuiltinFunction::Md5:
	case BuiltinFunction::Sha1:
	case BuiltinFunction::Sha256:
	case BuiltinFunction::Sha384:
	case BuiltinFunction::Sha512:
	case BuiltinFunction::Struuid:
		return literalOf(xsd::kString);

	// --- Boolean-valued ---
	case BuiltinFunction::Bound:
	case BuiltinFunction::Contains:
	case BuiltinFunction::Strstarts:
	case BuiltinFunction::Strends:
	case BuiltinFunction::Regex:
	case BuiltinFunction::LangMatches:
	case BuiltinFunction::SameTerm:
	case BuiltinFunction::IsIri:
	case BuiltinFunction::IsUri:
	case BuiltinFunction::IsBlank:
	case BuiltinFunction::IsLiteral:
	case BuiltinFunction::IsNumeric:
		return literalOf(xsd::kBoolean);

	// --- Integer-valued ---
	case BuiltinFunction::Strlen:
	case BuiltinFunction::Year:
	case BuiltinFunction::Month:
	case BuiltinFunction::Day:
	case BuiltinFunction::Hours:
	case BuiltinFunction::Minutes:
	case BuiltinFunction::Seconds:
		// SPARQL types SECONDS() as xsd:decimal, but EXTRACT(SECOND ...) here
		// yields a whole number - annotate what is produced, not what the spec
		// would produce (see doc/api.md).
		return literalOf(xsd::kInteger);

	// --- Numeric-valued ---
	case BuiltinFunction::Abs:
	case BuiltinFunction::Ceil:
	case BuiltinFunction::Floor:
	case BuiltinFunction::Round: {
		const TermInfo arg = argInfo(0);
		return arithmeticResult(arg, arg);
	}
	case BuiltinFunction::Rand:
		return literalOf(xsd::kDouble);

	case BuiltinFunction::Now:
		return literalOf(xsd::kDateTime);

	// --- Term-producing ---
	case BuiltinFunction::Datatype:
		return iriTerm();
	case BuiltinFunction::Uuid:
		// Emits "urn:uuid:..." - an IRI. STRUUID() is the string-valued one.
		return iriTerm();
	case BuiltinFunction::IriFn:
	case BuiltinFunction::UriFn:
		return iriTerm();
	case BuiltinFunction::Bnode: {
		TermInfo out;
		out.kind = RdfTermKind::BlankNode;
		return out;
	}

	// --- Re-tagging: only statically analysable with a constant 2nd argument.
	case BuiltinFunction::Strdt: {
		std::string datatypeIri;
		if (b.args.size() == 2 && constantIri(*b.args[1], datatypeIri)) {
			TermInfo out;
			out.kind = RdfTermKind::Literal;
			out.datatypeIri = datatypeIri;
			return out;
		}
		return TermInfo();
	}
	case BuiltinFunction::Strlang: {
		std::string lang;
		if (b.args.size() == 2 && constantString(*b.args[1], lang)) {
			TermInfo out;
			out.kind = RdfTermKind::Literal;
			out.datatypeIri = kRdfLangString;
			out.lang = lang;
			out.maybeLangTagged = true;
			return out;
		}
		return TermInfo();
	}

	// --- Selecting one of their arguments ---
	case BuiltinFunction::Coalesce: {
		if (b.args.empty()) {
			return TermInfo();
		}
		TermInfo out = argInfo(0);
		for (std::size_t i = 1; i < b.args.size(); ++i) {
			out = meet(out, argInfo(i));
		}
		return out;
	}
	case BuiltinFunction::If:
		// The condition (arg 0) never becomes the result.
		return b.args.size() == 3 ? meet(argInfo(1), argInfo(2)) : TermInfo();

	// --- Unsupported: translateExpression throws on these anyway ---
	case BuiltinFunction::Timezone:
	case BuiltinFunction::Tz:
		return TermInfo();
	}
	return TermInfo();
}

TermInfo infer(const Expression &e, const TranslatedPattern &scope) {
	switch (e.kind()) {
	case ExprKind::Literal:
		return termInfoOfTerm(*static_cast<const LiteralExpr &>(e).literal);
	case ExprKind::VarRef:
		return scope.termInfoOf(static_cast<const VarExpr &>(e).var->name);
	case ExprKind::IriRef:
		return iriTerm();
	case ExprKind::Binary:
		return inferBinary(static_cast<const BinaryExpr &>(e), scope);
	case ExprKind::Unary:
		return inferUnary(static_cast<const UnaryExpr &>(e), scope);
	case ExprKind::In:
	case ExprKind::Exists:
		return literalOf(xsd::kBoolean);
	case ExprKind::FunctionCall: {
		const auto &fc = static_cast<const FunctionCallExpr &>(e);
		// The twelve XSD constructor casts are the only supported IRI-named
		// calls, and are the highest-value rule here: they let a query type an
		// otherwise-untyped column at the point of use.
		if (isXsdCastIri(fc.iri->value)) {
			TermInfo out;
			out.kind = RdfTermKind::Literal;
			out.datatypeIri = fc.iri->value;
			return out;
		}
		return TermInfo();
	}
	case ExprKind::BuiltInCall:
		return inferBuiltIn(static_cast<const BuiltInCallExpr &>(e), scope);
	case ExprKind::Aggregate:
		return inferAggregate(static_cast<const AggregateExpr &>(e), scope);
	}
	return TermInfo();
}

} // namespace

TermInfo inferExprTermInfo(const sparql::ast::Expression &expr, const TranslatedPattern &scope) {
	return infer(expr, scope);
}

} // namespace sparql2sql
