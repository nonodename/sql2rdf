#include "r2rml/TripleStore.h"

namespace r2rml {

void TripleStore::insert(const std::string &subj, const std::string &pred, rdf::Term obj) {
	data_[subj][pred].push_back(std::move(obj));
}

const std::vector<rdf::Term> *TripleStore::getObjects(const std::string &subj, const std::string &pred) const {
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

const rdf::Term *TripleStore::getFirstOfKind(const std::string &subj, const std::string &pred,
                                             rdf::TermKind kind) const {
	const auto *objs = getObjects(subj, pred);
	if (!objs) {
		return nullptr;
	}
	for (const auto &o : *objs) {
		if (o.kind() == kind) {
			return &o;
		}
	}
	return nullptr;
}

std::string TripleStore::getFirstLiteral(const std::string &subj, const std::string &pred) const {
	const rdf::Term *o = getFirstOfKind(subj, pred, rdf::TermKind::Literal);
	return o ? o->lexical() : std::string();
}

std::string TripleStore::getFirstUri(const std::string &subj, const std::string &pred) const {
	const rdf::Term *o = getFirstOfKind(subj, pred, rdf::TermKind::Iri);
	return o ? o->lexical() : std::string();
}

std::string TripleStore::objKey(const rdf::Term &o) {
	if (o.isBlankNode()) {
		return "_:" + o.lexical();
	}
	if (o.isIri()) {
		return o.lexical();
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
