#include "r2rml/TriplesMap.h"
#include "r2rml/SerdTerm.h"
#include "r2rml/LogicalTable.h"
#include "r2rml/SubjectMap.h"
#include "r2rml/GraphMap.h"
#include "r2rml/PredicateObjectMap.h"
#include "r2rml/R2RMLMapping.h"
#include "r2rml/SQLConnection.h"
#include "r2rml/SQLRow.h"

#include <algorithm>
#include <ostream>

namespace r2rml {

TriplesMap::TriplesMap() = default;
TriplesMap::~TriplesMap() = default;

void TriplesMap::generateTriples(const SQLRow &row, SerdWriter &rdfWriter, const R2RMLMapping &mapping,
                                 SQLConnection &dbConnection) const {
	if (!subjectMap) {
		return;
	}

	// Generate the subject node for this row.
	rdf::Term subject;
	subjectMap->generateRDFTerm(row, subject);
	if (subject.isNull()) {
		return; // null subject - skip row
	}
	const SerdTermRef subjectNode(subject);

	// Emit rdf:type triples for each rr:class. Only the subject map's own
	// graph maps apply here - there is no predicate-object map involved.
	static const std::vector<std::unique_ptr<GraphMap>> noGraphMaps;
	if (!subjectMap->classIRIs.empty()) {
		const SerdTermRef rdfTypeNode(rdf::Term::iri(rdf::RDF_TYPE));
		for (const std::string &classIRI : subjectMap->classIRIs) {
			const SerdTermRef classNode(rdf::Term::iri(classIRI));
			forEachGraphNode(subjectMap->graphMaps, noGraphMaps, row, [&](const rdf::Term &graph) {
				const SerdTermRef graphNode(graph);
				checkWriteStatus(serd_writer_write_statement(&rdfWriter, 0, graphNode.value(), subjectNode.value(),
				                                             rdfTypeNode.value(), classNode.value(), nullptr, nullptr));
			});
		}
	}

	// Process each predicate-object map.
	for (const auto &pom : predicateObjectMaps) {
		if (pom) {
			pom->processRow(row, subject, rdfWriter, mapping, dbConnection, subjectMap->graphMaps);
		}
	}
}

bool TriplesMap::isValid() const {
	if (!logicalTable || !logicalTable->isValid()) {
		return false;
	}
	if (!subjectMap || !subjectMap->isValid()) {
		return false;
	}
	return std::all_of(predicateObjectMaps.begin(), predicateObjectMaps.end(),
	                   [](const std::unique_ptr<PredicateObjectMap> &pom) { return pom && pom->isValid(); });
}

bool TriplesMap::isValidInsideOut() const {
	// rr:LogicalTable (including rr:sqlQuery) is not supported inside-out.
	if (logicalTable) {
		return false;
	}
	if (!subjectMap || !subjectMap->isValid()) {
		return false;
	}
	return std::all_of(predicateObjectMaps.begin(), predicateObjectMaps.end(),
	                   [](const std::unique_ptr<PredicateObjectMap> &pom) { return pom && pom->isValidInsideOut(); });
}

std::ostream &TriplesMap::print(std::ostream &os) const {
	os << "TriplesMap <" << id << "> {\n";
	os << "  logicalTable: ";
	if (logicalTable) {
		os << *logicalTable;
	} else {
		os << "(none)";
	}
	os << "\n";
	os << "  subjectMap: ";
	if (subjectMap) {
		os << *subjectMap;
	} else {
		os << "(none)";
	}
	os << "\n";
	for (std::size_t i = 0; i < predicateObjectMaps.size(); ++i) {
		os << "  predicateObjectMap[" << i << "]: ";
		if (predicateObjectMaps[i]) {
			os << *predicateObjectMaps[i];
		} else {
			os << "(none)";
		}
		os << "\n";
	}
	os << "}";
	return os;
}

std::ostream &operator<<(std::ostream &os, const TriplesMap &tm) {
	return tm.print(os);
}

} // namespace r2rml
