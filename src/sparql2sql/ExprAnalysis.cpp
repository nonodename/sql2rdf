#include "sparql2sql/ExprAnalysis.h"

#include "sparql-parser/ast/Expression.h"

namespace sparql2sql {

using sparql::ast::AggregateExpr;
using sparql::ast::BinaryExpr;
using sparql::ast::BinaryOp;
using sparql::ast::BuiltInCallExpr;
using sparql::ast::Expression;
using sparql::ast::ExprKind;
using sparql::ast::FunctionCallExpr;
using sparql::ast::InExpr;
using sparql::ast::UnaryExpr;
using sparql::ast::VarExpr;

void collectVarRefs(const Expression &expr, std::vector<std::string> &out) {
	switch (expr.kind()) {
	case ExprKind::VarRef:
		out.push_back(static_cast<const VarExpr &>(expr).var->name);
		return;
	case ExprKind::Literal:
	case ExprKind::IriRef:
	case ExprKind::Exists:
		return;
	case ExprKind::Unary:
		collectVarRefs(*static_cast<const UnaryExpr &>(expr).operand, out);
		return;
	case ExprKind::Binary: {
		const auto &b = static_cast<const BinaryExpr &>(expr);
		collectVarRefs(*b.left, out);
		collectVarRefs(*b.right, out);
		return;
	}
	case ExprKind::In: {
		const auto &in = static_cast<const InExpr &>(expr);
		collectVarRefs(*in.lhs, out);
		for (const auto &e : in.list) {
			collectVarRefs(*e, out);
		}
		return;
	}
	case ExprKind::FunctionCall: {
		const auto &fc = static_cast<const FunctionCallExpr &>(expr);
		for (const auto &a : fc.args) {
			collectVarRefs(*a, out);
		}
		return;
	}
	case ExprKind::BuiltInCall: {
		const auto &bc = static_cast<const BuiltInCallExpr &>(expr);
		for (const auto &a : bc.args) {
			collectVarRefs(*a, out);
		}
		return;
	}
	case ExprKind::Aggregate: {
		const auto &agg = static_cast<const AggregateExpr &>(expr);
		if (agg.arg) {
			collectVarRefs(*agg.arg, out);
		}
		return;
	}
	}
}

bool containsExists(const Expression &expr) {
	switch (expr.kind()) {
	case ExprKind::Exists:
		return true;
	case ExprKind::VarRef:
	case ExprKind::Literal:
	case ExprKind::IriRef:
		return false;
	case ExprKind::Unary:
		return containsExists(*static_cast<const UnaryExpr &>(expr).operand);
	case ExprKind::Binary: {
		const auto &b = static_cast<const BinaryExpr &>(expr);
		return containsExists(*b.left) || containsExists(*b.right);
	}
	case ExprKind::In: {
		const auto &in = static_cast<const InExpr &>(expr);
		if (containsExists(*in.lhs)) {
			return true;
		}
		for (const auto &e : in.list) {
			if (containsExists(*e)) {
				return true;
			}
		}
		return false;
	}
	case ExprKind::FunctionCall: {
		const auto &fc = static_cast<const FunctionCallExpr &>(expr);
		for (const auto &a : fc.args) {
			if (containsExists(*a)) {
				return true;
			}
		}
		return false;
	}
	case ExprKind::BuiltInCall: {
		const auto &bc = static_cast<const BuiltInCallExpr &>(expr);
		for (const auto &a : bc.args) {
			if (containsExists(*a)) {
				return true;
			}
		}
		return false;
	}
	case ExprKind::Aggregate: {
		const auto &agg = static_cast<const AggregateExpr &>(expr);
		return agg.arg && containsExists(*agg.arg);
	}
	}
	return false;
}

std::vector<const Expression *> splitConjuncts(const Expression &expr) {
	std::vector<const Expression *> out;
	if (expr.kind() == ExprKind::Binary) {
		const auto &b = static_cast<const BinaryExpr &>(expr);
		if (b.op == BinaryOp::And) {
			std::vector<const Expression *> left = splitConjuncts(*b.left);
			std::vector<const Expression *> right = splitConjuncts(*b.right);
			out.insert(out.end(), left.begin(), left.end());
			out.insert(out.end(), right.begin(), right.end());
			return out;
		}
	}
	out.push_back(&expr);
	return out;
}

} // namespace sparql2sql
