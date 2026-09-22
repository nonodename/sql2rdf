#include "sparql2sql/ir/RelNode.h"

namespace sparql2sql {

RelNode::~RelNode() = default;

std::set<std::string> RelNode::boundVars() const {
	std::set<std::string> out;
	for (const auto &c : schema_) {
		if (c.nonNull) {
			out.insert(c.var);
		}
	}
	return out;
}

std::set<std::string> RelNode::optionalVars() const {
	std::set<std::string> out;
	for (const auto &c : schema_) {
		if (!c.nonNull) {
			out.insert(c.var);
		}
	}
	return out;
}

std::set<std::string> RelNode::allVars() const {
	std::set<std::string> out;
	for (const auto &c : schema_) {
		out.insert(c.var);
	}
	return out;
}

const ColumnInfo *RelNode::column(const std::string &var) const {
	for (const auto &c : schema_) {
		if (c.var == var) {
			return &c;
		}
	}
	return nullptr;
}

TermInfo meetColumns(const std::vector<const ColumnInfo *> &sources) {
	// `seeded` is load-bearing. Unknown is the lattice's absorbing element, so
	// an accumulator initialised to a default TermInfo would meet to Unknown
	// unconditionally - and, because Unknown is *defined* as the pre-annotation
	// behaviour, every other test in the suite would still pass. Any
	// "simplification" that drops this must fail a test; see
	// test_sparql2sql_terminfo.cpp's agreement cases.
	bool seeded = false;
	TermInfo acc;
	for (const ColumnInfo *src : sources) {
		if (src == nullptr) {
			continue; // Not bound here: NULL denotes no term, so it says nothing.
		}
		if (!seeded) {
			seeded = true;
			acc = src->term;
			continue;
		}
		acc = meet(acc, src->term);
	}
	return acc;
}

TermInfo meetAcrossArms(const std::string &var, const std::vector<RelNodePtr> &arms) {
	std::vector<const ColumnInfo *> sources;
	sources.reserve(arms.size());
	for (const auto &arm : arms) {
		sources.push_back(arm ? arm->column(var) : nullptr);
	}
	return meetColumns(sources);
}

namespace {

// A tag minted by tagLiteral is a plain single-quoted SQL string literal, so it
// means the same thing in any scope. Every other tag expression (mergeInner's
// CASE, a folded BIND's rewritten expression) references its own block's
// aliases and must never escape the arm that produced it - hence this guard
// rather than a bare textual-agreement test.
bool isConstantTag(const std::string &tag) {
	return tag.size() >= 2 && tag[0] == '\'' && tag[tag.size() - 1] == '\'' && tag.find('\'', 1) == tag.size() - 1;
}

} // namespace

void annotateFromArms(ColumnInfo &col, const std::vector<RelNodePtr> &arms) {
	col.term = meetAcrossArms(col.var, arms);
	col.tagExpr.clear();
	col.tagProjectable = false;

	bool everyArmHasTag = true;
	bool allAgreeOnConstant = true;
	std::string agreed;
	for (const auto &arm : arms) {
		// A null column is an arm that doesn't bind the variable at all: NULL
		// denotes no term, so like meetColumns it contributes nothing.
		const ColumnInfo *c = arm ? arm->column(col.var) : nullptr;
		if (c == nullptr) {
			continue;
		}
		if (c->tagExpr.empty()) {
			everyArmHasTag = false;
			allAgreeOnConstant = false;
			continue;
		}
		if (!isConstantTag(c->tagExpr) || (!agreed.empty() && agreed != c->tagExpr)) {
			allAgreeOnConstant = false;
		} else {
			agreed = c->tagExpr;
		}
	}
	if (allAgreeOnConstant) {
		col.tagExpr = agreed;
		return;
	}
	// Arms disagree (or aren't all constants): no single constant to hoist, but
	// if every arm still mints its own tag, renderUnion can still project a
	// correct per-row d_<var> from each arm's own scope on demand.
	col.tagProjectable = everyArmHasTag;
}

bool hasRuntimeTag(const ColumnInfo &col) {
	return !col.tagExpr.empty() || col.tagProjectable;
}

bool tagsMayDiffer(const ColumnInfo &a, const ColumnInfo &b) {
	if (!hasRuntimeTag(a) || !hasRuntimeTag(b)) {
		return false;
	}
	if (!a.tagExpr.empty() && !b.tagExpr.empty() && a.tagExpr == b.tagExpr) {
		return false; // Both hoisted to the identical constant: provably equal.
	}
	return true;
}

} // namespace sparql2sql
