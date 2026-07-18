#pragma once

#include "TermMap.h"

#include <string>
#include <vector>
#include <memory>

namespace r2rml {

class GraphMap;

/**
 * A specialization of TermMap used for subjects.  In addition to the base
 * term generation behaviour it can carry rdf:class assertions and graph maps.
 */
class SubjectMap : public TermMap {
public:
	SubjectMap() = default;
	~SubjectMap() override;

	bool isValid() const override;

	std::ostream &print(std::ostream &os) const override;

	/// The term-generation strategy (rr:template/rr:column/rr:constant) that
	/// determines this subject map's value. SubjectMap only adds the
	/// rr:class/rr:graph annotations on top of a TermMap; the parser
	/// composes the value strategy by delegation rather than inheritance
	/// (see R2RMLParser.cpp's ConcreteSubjectMap) so it can reuse the same
	/// term-map-building code used for predicate/object maps. Returns null
	/// only when no value strategy has been configured (e.g. a
	/// default-constructed test double).
	virtual const TermMap *valueTermMap() const = 0;

	std::vector<std::string> classIRIs;
	std::vector<std::unique_ptr<GraphMap>> graphMaps;
};

} // namespace r2rml
