#include "sparql2sql/ir/NativeKey.h"

#include "sparql2sql/TypeCatalog.h"

namespace sparql2sql {

std::vector<NativeKeyPair> nativeKeyPairs(const ColumnInfo &lc, const ColumnInfo &rc, const TypeCatalog *catalog) {
	std::vector<NativeKeyPair> pairs;
	if (catalog == nullptr) {
		return pairs;
	}

	if (lc.prov == Provenance::PureColumn && rc.prov == Provenance::PureColumn && !lc.nativeColumnRef.empty() &&
	    !rc.nativeColumnRef.empty()) {
		if (!catalog->comparable(lc.tableIdentity, lc.columnName, rc.tableIdentity, rc.columnName)) {
			return pairs;
		}
		NativeKeyPair p;
		p.leftRef = lc.nativeColumnRef;
		p.rightRef = rc.nativeColumnRef;
		pairs.push_back(p);
		return pairs;
	}

	if (lc.prov != Provenance::TemplateExpr || rc.prov != Provenance::TemplateExpr) {
		return pairs;
	}
	if (lc.templateString.empty() || lc.templateString != rc.templateString) {
		return pairs;
	}
	if (!lc.templateInvertible || !rc.templateInvertible) {
		return pairs;
	}
	// Identical template strings imply identical placeholder lists, so the two
	// ref vectors line up positionally; verify rather than assume.
	if (lc.templateColumnRefs.empty() || lc.templateColumnRefs.size() != rc.templateColumnRefs.size() ||
	    lc.templateColumnNames != rc.templateColumnNames) {
		return pairs;
	}
	for (std::size_t i = 0; i < lc.templateColumnRefs.size(); ++i) {
		if (!catalog->comparable(lc.tableIdentity, lc.templateColumnNames[i], rc.tableIdentity,
		                         rc.templateColumnNames[i])) {
			return std::vector<NativeKeyPair>(); // All-or-nothing: a partial rewrite would be wrong.
		}
		NativeKeyPair p;
		p.leftRef = lc.templateColumnRefs[i];
		p.rightRef = rc.templateColumnRefs[i];
		pairs.push_back(p);
	}
	return pairs;
}

} // namespace sparql2sql
