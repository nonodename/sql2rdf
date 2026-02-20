#include "r2rml/R2RMLMapping.h"
#include "r2rml/LogicalTable.h"

#include <algorithm>
#include "r2rml/BaseTableOrView.h"
#include "r2rml/R2RMLView.h"
#include "r2rml/TriplesMap.h"
#include "r2rml/PredicateObjectMap.h"
#include "r2rml/TermMap.h"
#include "r2rml/ConstantTermMap.h"
#include "r2rml/ColumnTermMap.h"
#include "r2rml/TemplateTermMap.h"
#include "r2rml/SubjectMap.h"
#include "r2rml/PredicateMap.h"
#include "r2rml/ObjectMap.h"
#include "r2rml/GraphMap.h"
#include "r2rml/ReferencingObjectMap.h"
#include "r2rml/JoinCondition.h"
#include "r2rml/SQLConnection.h"
#include "r2rml/SQLResultSet.h"
#include "r2rml/SQLRow.h"
#include "r2rml/SQLValue.h"

namespace r2rml {

// R2RMLMapping
R2RMLMapping::R2RMLMapping() = default;
R2RMLMapping::~R2RMLMapping() {
    if (serdEnvironment) {
        serd_env_free(serdEnvironment);
    }
}
void R2RMLMapping::loadMapping(const std::string& mappingFilePath) {
    // stub
}
void R2RMLMapping::processDatabase(SQLConnection& dbConnection,
                                   SerdWriter& rdfWriter) {
    // stub
}
bool R2RMLMapping::isValid() const {
    return std::all_of(triplesMaps.begin(), triplesMaps.end(),
                       [](const std::unique_ptr<TriplesMap>& tm) {
                           return tm && tm->isValid();
                       });
}

// LogicalTable
LogicalTable::~LogicalTable() = default;

// BaseTableOrView
BaseTableOrView::BaseTableOrView(const std::string& table)
    : tableName(table) {}
BaseTableOrView::~BaseTableOrView() = default;
std::unique_ptr<SQLResultSet> BaseTableOrView::getRows(SQLConnection& dbConnection) {
    return nullptr;
}
std::vector<std::string> BaseTableOrView::getColumnNames() { return {}; }

// R2RMLView
R2RMLView::R2RMLView(const std::string& query) : sqlQuery(query) {}
R2RMLView::~R2RMLView() = default;
std::unique_ptr<SQLResultSet> R2RMLView::getRows(SQLConnection& dbConnection) {
    return nullptr;
}
std::vector<std::string> R2RMLView::getColumnNames() { return {}; }

// TriplesMap
TriplesMap::TriplesMap() = default;
TriplesMap::~TriplesMap() = default;
void TriplesMap::generateTriples(const SQLRow& row,
                                 SerdWriter& rdfWriter,
                                 const R2RMLMapping& mapping) const {
    // stub
}
bool TriplesMap::isValid() const {
    if (!logicalTable || !logicalTable->isValid()) return false;
    if (!subjectMap   || !subjectMap->isValid())   return false;
    return std::all_of(predicateObjectMaps.begin(), predicateObjectMaps.end(),
                       [](const std::unique_ptr<PredicateObjectMap>& pom) {
                           return pom && pom->isValid();
                       });
}

// PredicateObjectMap
PredicateObjectMap::PredicateObjectMap() = default;
PredicateObjectMap::~PredicateObjectMap() = default;
void PredicateObjectMap::processRow(const SQLRow& row,
                                   const SerdNode& subject,
                                   SerdWriter& rdfWriter,
                                   const R2RMLMapping& mapping) const {
    // stub
}
bool PredicateObjectMap::isValid() const {
    if (predicateMaps.empty() || objectMaps.empty()) return false;
    return std::all_of(predicateMaps.begin(), predicateMaps.end(),
                       [](const std::unique_ptr<TermMap>& pm) {
                           return pm && pm->isValid();
                       }) &&
           std::all_of(objectMaps.begin(), objectMaps.end(),
                       [](const std::unique_ptr<TermMap>& om) {
                           return om && om->isValid();
                       });
}

// TermMap
TermMap::~TermMap() = default;

// ConstantTermMap
ConstantTermMap::ConstantTermMap(const SerdNode& node) {
    if (node.type != SERD_NOTHING && node.buf && node.n_bytes > 0) {
        ownedUri_.assign(reinterpret_cast<const char*>(node.buf), node.n_bytes);
        constantValue.buf     = reinterpret_cast<const uint8_t*>(ownedUri_.c_str());
        constantValue.n_bytes = node.n_bytes;
        constantValue.n_chars = node.n_chars;
        constantValue.flags   = node.flags;
        constantValue.type    = node.type;
    }
}
ConstantTermMap::~ConstantTermMap() = default;
SerdNode ConstantTermMap::generateRDFTerm(const SQLRow&,
                                          const SerdEnv&) const {
    return constantValue;
}

// ColumnTermMap
ColumnTermMap::ColumnTermMap(const std::string& column)
    : columnName(column) {}
ColumnTermMap::~ColumnTermMap() = default;
SerdNode ColumnTermMap::generateRDFTerm(const SQLRow&,
                                        const SerdEnv&) const {
    return SerdNode{0};
}

// TemplateTermMap
TemplateTermMap::TemplateTermMap(const std::string& templ)
    : templateString(templ) {}
TemplateTermMap::~TemplateTermMap() = default;
SerdNode TemplateTermMap::generateRDFTerm(const SQLRow&,
                                          const SerdEnv&) const {
    return SerdNode{0};
}

// SubjectMap
SubjectMap::~SubjectMap() = default;
bool SubjectMap::isValid() const {
    return std::all_of(graphMaps.begin(), graphMaps.end(),
                       [](const std::unique_ptr<GraphMap>& gm) {
                           return gm && gm->isValid();
                       });
}

// PredicateMap
PredicateMap::~PredicateMap() = default;

// ObjectMap
ObjectMap::~ObjectMap() = default;

// GraphMap
GraphMap::~GraphMap() = default;

// JoinCondition
JoinCondition::JoinCondition(const std::string& childCol,
                             const std::string& parentCol)
    : childColumn(childCol), parentColumn(parentCol) {}

// ReferencingObjectMap
ReferencingObjectMap::ReferencingObjectMap() = default;
ReferencingObjectMap::~ReferencingObjectMap() = default;
bool ReferencingObjectMap::isValid() const {
    if (!parentTriplesMap) return false;
    return std::all_of(joinConditions.begin(), joinConditions.end(),
                       [](const JoinCondition& jc) { return jc.isValid(); });
}
std::unique_ptr<SQLResultSet>
ReferencingObjectMap::getJoinedRows(SQLConnection& dbConnection,
                                    const SQLRow& childRow) const {
    return nullptr;
}
SerdNode ReferencingObjectMap::generateRDFTerm(const SQLRow&,
                                               const SQLRow&,
                                               const SerdEnv&) const {
    return SerdNode{0};
}

// SQLConnection
SQLConnection::~SQLConnection() = default;
std::string SQLConnection::getDefaultCatalog() { return std::string(); }
std::string SQLConnection::getDefaultSchema() { return std::string(); }

// SQLResultSet
SQLResultSet::~SQLResultSet() = default;

// SQLRow
SQLRow::SQLRow() = default;
SQLRow::~SQLRow() = default;
SQLValue SQLRow::getValue(const std::string&) const { return SQLValue(); }
bool SQLRow::isNull(const std::string&) const { return true; }


} // namespace r2rml
