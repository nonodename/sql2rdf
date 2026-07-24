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

} // namespace sparql2sql
