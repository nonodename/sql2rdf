#include "r2rml/TripleStore.h"

namespace r2rml {

void TripleStore::insert(const std::string &subj, const std::string &pred, ObjValue obj) {
	data_[subj][pred].push_back(std::move(obj));
}

const std::vector<ObjValue> *TripleStore::getObjects(const std::string &subj, const std::string &pred) const {
	auto si = data_.find(subj);
	if (si == data_.end()) {
		return nullptr;
	}
	auto pi = si->second.find(pred);
	if (pi == si->second.end()) {
		return nullptr;
	}
	return &pi->second;
}

std::string TripleStore::getFirstLiteral(const std::string &subj, const std::string &pred) const {
	const auto *objs = getObjects(subj, pred);
	if (!objs) {
		return {};
	}
	for (const auto &o : *objs) {
		if (o.type == ObjType::Literal) {
			return o.value;
		}
	}
	return {};
}

std::string TripleStore::getFirstUri(const std::string &subj, const std::string &pred) const {
	const auto *objs = getObjects(subj, pred);
	if (!objs) {
		return {};
	}
	for (const auto &o : *objs) {
		if (o.type == ObjType::URI) {
			return o.value;
		}
	}
	return {};
}

std::string TripleStore::objKey(const ObjValue &o) {
	if (o.type == ObjType::Blank) {
		return "_:" + o.value;
	}
	if (o.type == ObjType::URI) {
		return o.value;
	}
	return {};
}

std::string TripleStore::getFirstObjKey(const std::string &subj, const std::string &pred) const {
	const auto *objs = getObjects(subj, pred);
	if (!objs || objs->empty()) {
		return {};
	}
	return objKey(objs->front());
}

} // namespace r2rml
