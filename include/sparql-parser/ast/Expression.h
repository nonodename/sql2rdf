#pragma once

#include <memory>
#include <string>
#include <vector>

#include "sparql-parser/ast/GraphPattern.h"
#include "sparql-parser/ast/Term.h"

namespace sparql {
namespace ast {

enum class ExprKind { Binary, Unary, In, FunctionCall, BuiltInCall, Aggregate, Exists, Literal, VarRef, IriRef };

enum class UnaryOp { Not, Plus, Minus };

enum class BinaryOp { Or, And, Eq, Ne, Lt, Gt, Le, Ge, Add, Sub, Mul, Div };

enum class AggregateKind { Count, Sum, Min, Max, Avg, Sample, GroupConcat };

/// All built-in functions/operators from §17.4.1-§17.4.6 that are not
/// represented as a BinaryExpr/UnaryExpr/InExpr. `Uri` is kept distinct
/// from `Iri` (they are synonyms in the spec) purely so the printer can
/// show which spelling was used; both behave identically.
enum class BuiltinFunction {
	Str,
	Lang,
	LangMatches,
	Datatype,
	Bound,
	IriFn,
	UriFn,
	Bnode,
	Rand,
	Abs,
	Ceil,
	Floor,
	Round,
	Concat,
	Strlen,
	Substr,
	Ucase,
	Lcase,
	EncodeForUri,
	Contains,
	Strstarts,
	Strends,
	Strbefore,
	Strafter,
	Replace,
	Regex,
	Year,
	Month,
	Day,
	Hours,
	Minutes,
	Seconds,
	Timezone,
	Tz,
	Now,
	Uuid,
	Struuid,
	Md5,
	Sha1,
	Sha256,
	Sha384,
	Sha512,
	Coalesce,
	If,
	Strlang,
	Strdt,
	SameTerm,
	IsIri,
	IsUri,
	IsBlank,
	IsLiteral,
	IsNumeric
};

class Expression {
public:
	explicit Expression(ExprKind kind) : kind_(kind) {
	}
	virtual ~Expression() = default;

	ExprKind kind() const {
		return kind_;
	}

private:
	ExprKind kind_;
};

class LiteralExpr : public Expression {
public:
	explicit LiteralExpr(std::unique_ptr<RdfLiteral> literal)
	    : Expression(ExprKind::Literal), literal(std::move(literal)) {
	}

	std::unique_ptr<RdfLiteral> literal;
};

class VarExpr : public Expression {
public:
	explicit VarExpr(std::unique_ptr<Var> var) : Expression(ExprKind::VarRef), var(std::move(var)) {
	}

	std::unique_ptr<Var> var;
};

class IriExpr : public Expression {
public:
	explicit IriExpr(std::unique_ptr<Iri> iri) : Expression(ExprKind::IriRef), iri(std::move(iri)) {
	}

	std::unique_ptr<Iri> iri;
};

class UnaryExpr : public Expression {
public:
	UnaryExpr(UnaryOp op, std::unique_ptr<Expression> operand)
	    : Expression(ExprKind::Unary), op(op), operand(std::move(operand)) {
	}

	UnaryOp op;
	std::unique_ptr<Expression> operand;
};

class BinaryExpr : public Expression {
public:
	BinaryExpr(BinaryOp op, std::unique_ptr<Expression> left, std::unique_ptr<Expression> right)
	    : Expression(ExprKind::Binary), op(op), left(std::move(left)), right(std::move(right)) {
	}

	BinaryOp op;
	std::unique_ptr<Expression> left;
	std::unique_ptr<Expression> right;
};

/// `lhs IN (...)` / `lhs NOT IN (...)` (rules 17.4.1.9/10).
class InExpr : public Expression {
public:
	InExpr(std::unique_ptr<Expression> lhs, std::vector<std::unique_ptr<Expression>> list, bool negated)
	    : Expression(ExprKind::In), lhs(std::move(lhs)), list(std::move(list)), negated(negated) {
	}

	std::unique_ptr<Expression> lhs;
	std::vector<std::unique_ptr<Expression>> list;
	bool negated;
};

/// A call to a non-built-in function, named by IRI (rule 70/119's
/// iriOrFunction with a non-empty ArgList).
class FunctionCallExpr : public Expression {
public:
	FunctionCallExpr(std::unique_ptr<Iri> iri, std::vector<std::unique_ptr<Expression>> args, bool distinct)
	    : Expression(ExprKind::FunctionCall), iri(std::move(iri)), args(std::move(args)), distinct(distinct) {
	}

	std::unique_ptr<Iri> iri;
	std::vector<std::unique_ptr<Expression>> args;
	bool distinct;
};

class BuiltInCallExpr : public Expression {
public:
	BuiltInCallExpr(BuiltinFunction fn, std::vector<std::unique_ptr<Expression>> args)
	    : Expression(ExprKind::BuiltInCall), fn(fn), args(std::move(args)) {
	}

	BuiltinFunction fn;
	std::vector<std::unique_ptr<Expression>> args;
};

/// COUNT/SUM/MIN/MAX/AVG/SAMPLE/GROUP_CONCAT (rule 127). `arg` is null iff
/// `star` is true (COUNT(*)).
class AggregateExpr : public Expression {
public:
	AggregateExpr() : Expression(ExprKind::Aggregate) {
	}

	AggregateKind aggKind = AggregateKind::Count;
	bool distinct = false;
	bool star = false;
	std::unique_ptr<Expression> arg;
	bool hasSeparator = false;
	std::string separator; // GROUP_CONCAT ( ... ; SEPARATOR = "..." )
};

/// FILTER EXISTS { ... } / FILTER NOT EXISTS { ... } (rule 125/126).
class ExistsExpr : public Expression {
public:
	ExistsExpr(bool negated, std::unique_ptr<GroupGraphPattern> pattern)
	    : Expression(ExprKind::Exists), negated(negated), pattern(std::move(pattern)) {
	}

	bool negated;
	std::unique_ptr<GroupGraphPattern> pattern;
};

} // namespace ast
} // namespace sparql
