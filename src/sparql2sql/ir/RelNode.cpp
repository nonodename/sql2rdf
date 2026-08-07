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

} // namespace sparql2sql
