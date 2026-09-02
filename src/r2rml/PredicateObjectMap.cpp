#include "r2rml/PredicateObjectMap.h"
#include "r2rml/SerdTerm.h"
#include "r2rml/TermMap.h"
#include "r2rml/ReferencingObjectMap.h"
#include "r2rml/GraphMap.h"
#include "r2rml/R2RMLMapping.h"
#include "r2rml/SQLConnection.h"
#include "r2rml/SQLRow.h"
#include "r2rml/SQLResultSet.h"

#include <algorithm>
#include <ostream>

namespace r2rml {

PredicateObjectMap::PredicateObjectMap() = default;
PredicateObjectMap::~PredicateObjectMap() = default;

void PredicateObjectMap::processRow(const SQLRow &row, const rdf::Term &subject, SerdWriter &rdfWriter,
                                    const R2RMLMapping & /*mapping*/, SQLConnection &dbConnection,
                                    const std::vector<std::unique_ptr<GraphMap>> &subjectGraphMaps) const {
	const SerdTermRef subjectNode(subject);

	// Hoisted out of the loops: generateRDFTerm clears and refills these, and
	// rdf::Term::clear() preserves capacity, so after the first few rows the
	// per-row term generation stops allocating entirely.
	rdf::Term predicate;
	rdf::Term object;

	// For each predicate/object combination, emit a triple.
	for (const auto &predMap : predicateMaps) {
		if (!predMap) {
			continue;
		}
		predMap->generateRDFTerm(row, predicate);
		if (predicate.isNull()) {
			continue; // null predicate - skip
		}
		const SerdTermRef predicateNode(predicate);

		for (const auto &objMap : objectMaps) {
			if (!objMap) {
				continue;
			}

			// Check if this object map is a ReferencingObjectMap (join).
			ReferencingObjectMap *rom = dynamic_cast<ReferencingObjectMap *>(objMap.get());

			if (rom) {
				// Join: query the parent table and use parent subject as object.
				auto parentRows = rom->getJoinedRows(dbConnection, row);
				if (!parentRows) {
					continue;
				}
				while (parentRows->next()) {
					const SQLRow &parentRow = parentRows->getCurrentRow();
					rom->generateRDFTerm(row, parentRow, object);
					if (object.isNull()) {
						continue;
					}
					const SerdTermRef objectNode(object);
					forEachGraphNode(subjectGraphMaps, graphMaps, row, [&](const rdf::Term &graph) {
						const SerdTermRef graphNode(graph);
						checkWriteStatus(serd_writer_write_statement(&rdfWriter, 0, graphNode.value(),
						                                             subjectNode.value(), predicateNode.value(),
						                                             objectNode.value(), nullptr, nullptr));
					});
				}
			} else {
				// Regular term map.
				objMap->generateRDFTerm(row, object);
				if (object.isNull()) {
					continue; // null object - skip
				}

				// The datatype and language now live ON the term, so the
				// mutually-exclusive choice is made once here rather than being
				// rebuilt as loose SerdNodes at the write site. rr:language wins
				// over any datatype, matching R2RML and RDF 1.1.
				//
				// computeDatatypeIRI() only reports a *static* rr:datatype (or,
				// for ColumnTermMap, the column value's own datatype) - it
				// returns empty for a ConstantTermMap, whose literal already
				// carries its own datatype/language from generateRDFTerm(). An
				// empty result must therefore leave the term's existing
				// datatype/language alone rather than clearing it: setDatatypeIri
				// with an empty string strips whatever is already there.
				if (object.isLiteral()) {
					if (objMap->languageTag) {
						object.setLang(*objMap->languageTag);
					} else {
						std::string dt = objMap->computeDatatypeIRI(row);
						if (!dt.empty()) {
							object.setDatatypeIri(dt);
						}
					}
				}

				const SerdTermRef objectNode(object);
				forEachGraphNode(subjectGraphMaps, graphMaps, row, [&](const rdf::Term &graph) {
					const SerdTermRef graphNode(graph);
					checkWriteStatus(serd_writer_write_statement(&rdfWriter, 0, graphNode.value(), subjectNode.value(),
					                                             predicateNode.value(), objectNode.value(),
					                                             objectNode.datatype(), objectNode.lang()));
				});
			}
		}
	}
}

bool PredicateObjectMap::isValid() const {
	if (predicateMaps.empty() || objectMaps.empty()) {
		return false;
	}
	return std::all_of(predicateMaps.begin(), predicateMaps.end(),
	                   [](const std::unique_ptr<TermMap> &pm) { return pm && pm->isValid(); }) &&
	       std::all_of(objectMaps.begin(), objectMaps.end(),
	                   [](const std::unique_ptr<TermMap> &om) { return om && om->isValid(); });
}

bool PredicateObjectMap::isValidInsideOut() const {
	if (predicateMaps.empty() || objectMaps.empty()) {
		return false;
	}
	if (!std::all_of(predicateMaps.begin(), predicateMaps.end(),
	                 [](const std::unique_ptr<TermMap> &pm) { return pm && pm->isValid(); })) {
		return false;
	}
	// rr:refObjectMap (ReferencingObjectMap) and rr:JoinCondition are not
	// supported in inside-out mode.
	return std::all_of(objectMaps.begin(), objectMaps.end(), [](const std::unique_ptr<TermMap> &om) {
		if (!om || !om->isValid()) {
			return false;
		}
		return dynamic_cast<const ReferencingObjectMap *>(om.get()) == nullptr;
	});
}

std::ostream &PredicateObjectMap::print(std::ostream &os) const {
	os << "PredicateObjectMap { predicates=[";
	for (std::size_t i = 0; i < predicateMaps.size(); ++i) {
		if (i) {
			os << ", ";
		}
		if (predicateMaps[i]) {
			os << *predicateMaps[i];
		}
	}
	os << "] objects=[";
	for (std::size_t i = 0; i < objectMaps.size(); ++i) {
		if (i) {
			os << ", ";
		}
		if (objectMaps[i]) {
			os << *objectMaps[i];
		}
	}
	os << "]";
	if (!graphMaps.empty()) {
		os << " graphMaps=[";
		for (std::size_t i = 0; i < graphMaps.size(); ++i) {
			if (i) {
				os << ", ";
			}
			if (graphMaps[i]) {
				os << *graphMaps[i];
			}
		}
		os << "]";
	}
	os << " }";
	return os;
}

std::ostream &operator<<(std::ostream &os, const PredicateObjectMap &pom) {
	return pom.print(os);
}

} // namespace r2rml
