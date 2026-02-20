// R2RML Turtle parser – populates an R2RMLMapping by reading triples via Serd.
//
// Strategy (two-phase):
//  1. Collect phase  – Serd callbacks store every triple in a TripleStore
//                      (map from subject key → predicate URI → object list).
//  2. Build phase    – Walk the TripleStore and construct the C++ object model.
//
// Named resources that carry at least one of rr:logicalTable / rr:subjectMap /
// rr:predicateObjectMap are treated as TriplesMaps.  Everything else (e.g. a
// named R2RML view like <#DeptTableView>) is treated as a logical-table helper
// and is resolved when a TriplesMap references it via rr:logicalTable.
//
// ReferencingObjectMap parentTriplesMap pointers are resolved in a second walk
// after all TriplesMaps have been created.

#include "r2rml/R2RMLParser.h"

#include "r2rml/BaseTableOrView.h"
#include "r2rml/ColumnTermMap.h"
#include "r2rml/ConstantTermMap.h"
#include "r2rml/GraphMap.h"
#include "r2rml/JoinCondition.h"
#include "r2rml/R2RMLMapping.h"
#include "r2rml/R2RMLView.h"
#include "r2rml/ReferencingObjectMap.h"
#include "r2rml/SubjectMap.h"
#include "r2rml/TemplateTermMap.h"
#include "r2rml/TermMap.h"
#include "r2rml/TriplesMap.h"
#include "r2rml/PredicateObjectMap.h"

#include <serd/serd.h>

#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace r2rml {

// ---------------------------------------------------------------------------
// R2RML namespace prefix
// ---------------------------------------------------------------------------
static const std::string RR = "http://www.w3.org/ns/r2rml#";

// ---------------------------------------------------------------------------
// Raw triple-store types
// ---------------------------------------------------------------------------
enum class ObjType { URI, Blank, Literal };

struct ObjValue {
    ObjType     type;
    std::string value;    ///< URI string, blank-node ID (no "_:" prefix), or literal text
    std::string datatype; ///< For typed literals – full URI
    std::string lang;     ///< For language-tagged literals
};

using PredMap    = std::map<std::string, std::vector<ObjValue>>;
using TripleStore = std::map<std::string, PredMap>;

struct ParseState {
    SerdEnv*    env{nullptr};
    TripleStore triples;
};

// ---------------------------------------------------------------------------
// Node-expansion helper
// ---------------------------------------------------------------------------

/// Convert a SerdNode to an absolute URI string (or "_:<id>" for blank nodes).
/// CURIEs are expanded using `env`; relative URIs are resolved against the base.
/// Returns an empty string if the node cannot be represented.
static std::string expandNode(SerdEnv* env, const SerdNode* node)
{
    if (!node || node->type == SERD_NOTHING) return {};

    if (node->type == SERD_BLANK) {
        return std::string("_:") +
               std::string(reinterpret_cast<const char*>(node->buf), node->n_bytes);
    }

    // URI or CURIE – ask the environment to expand/resolve
    SerdNode expanded = serd_env_expand_node(env, node);
    if (expanded.type != SERD_NOTHING && expanded.buf) {
        std::string result(reinterpret_cast<const char*>(expanded.buf), expanded.n_bytes);
        serd_node_free(&expanded);
        return result;
    }

    // Fallback: return the raw value (handles already-absolute URIs)
    if (node->buf)
        return std::string(reinterpret_cast<const char*>(node->buf), node->n_bytes);
    return {};
}

// ---------------------------------------------------------------------------
// Serd callbacks
// ---------------------------------------------------------------------------

static SerdStatus cbBase(void* handle, const SerdNode* base)
{
    auto* state = static_cast<ParseState*>(handle);
    serd_env_set_base_uri(state->env, base);
    return SERD_SUCCESS;
}

static SerdStatus cbPrefix(void* handle, const SerdNode* name, const SerdNode* uri)
{
    auto* state = static_cast<ParseState*>(handle);
    serd_env_set_prefix(state->env, name, uri);
    return SERD_SUCCESS;
}

static SerdStatus cbStatement(void*              handle,
                               SerdStatementFlags /*flags*/,
                               const SerdNode*   /*graph*/,
                               const SerdNode*    subject,
                               const SerdNode*    predicate,
                               const SerdNode*    object,
                               const SerdNode*    object_datatype,
                               const SerdNode*    object_lang)
{
    auto* state = static_cast<ParseState*>(handle);

    std::string subjKey = expandNode(state->env, subject);
    std::string predKey = expandNode(state->env, predicate);
    if (subjKey.empty() || predKey.empty()) return SERD_SUCCESS;

    ObjValue obj;
    if (object->type == SERD_BLANK) {
        obj.type  = ObjType::Blank;
        obj.value = std::string(reinterpret_cast<const char*>(object->buf),
                                object->n_bytes);
    } else if (object->type == SERD_LITERAL) {
        obj.type  = ObjType::Literal;
        obj.value = std::string(reinterpret_cast<const char*>(object->buf),
                                object->n_bytes);
        if (object_datatype && object_datatype->buf)
            obj.datatype = expandNode(state->env, object_datatype);
        if (object_lang && object_lang->buf)
            obj.lang = std::string(reinterpret_cast<const char*>(object_lang->buf),
                                   object_lang->n_bytes);
    } else {
        obj.type  = ObjType::URI;
        obj.value = expandNode(state->env, object);
    }

    state->triples[subjKey][predKey].push_back(std::move(obj));
    return SERD_SUCCESS;
}

static SerdStatus cbError(void* /*handle*/, const SerdError* error)
{
    vfprintf(stderr, error->fmt, *error->args);
    return SERD_SUCCESS; // non-fatal: keep going
}

// ---------------------------------------------------------------------------
// Triple-store query helpers
// ---------------------------------------------------------------------------

static const std::vector<ObjValue>*
getObjects(const TripleStore& ts, const std::string& subj, const std::string& pred)
{
    auto si = ts.find(subj);
    if (si == ts.end()) return nullptr;
    auto pi = si->second.find(pred);
    if (pi == si->second.end()) return nullptr;
    return &pi->second;
}

static std::string
getFirstLiteral(const TripleStore& ts, const std::string& subj, const std::string& pred)
{
    const auto* objs = getObjects(ts, subj, pred);
    if (!objs) return {};
    for (const auto& o : *objs)
        if (o.type == ObjType::Literal) return o.value;
    return {};
}

static std::string
getFirstUri(const TripleStore& ts, const std::string& subj, const std::string& pred)
{
    const auto* objs = getObjects(ts, subj, pred);
    if (!objs) return {};
    for (const auto& o : *objs)
        if (o.type == ObjType::URI) return o.value;
    return {};
}

/// Return the canonical lookup key for an ObjValue: "_:<id>" for blank nodes,
/// URI string for named nodes, empty string for literals.
static std::string objKey(const ObjValue& o)
{
    if (o.type == ObjType::Blank) return "_:" + o.value;
    if (o.type == ObjType::URI)   return o.value;
    return {};
}

/// Return the first object of a predicate as a subject-lookup key.
static std::string
getFirstObjKey(const TripleStore& ts, const std::string& subj, const std::string& pred)
{
    const auto* objs = getObjects(ts, subj, pred);
    if (!objs || objs->empty()) return {};
    return objKey(objs->front());
}

// ---------------------------------------------------------------------------
// ConcreteSubjectMap – private to this translation unit.
//
// SubjectMap inherits TermMap's pure-virtual generateRDFTerm without
// overriding it, making SubjectMap abstract.  ConcreteSubjectMap adds an
// inner TermMap that supplies the value-generation strategy.
// ---------------------------------------------------------------------------
class ConcreteSubjectMap : public SubjectMap {
public:
    std::unique_ptr<TermMap> valueMap;

    SerdNode generateRDFTerm(const SQLRow& row, const SerdEnv& env) const override
    {
        if (valueMap) return valueMap->generateRDFTerm(row, env);
        return SERD_NODE_NULL;
    }
};

// ---------------------------------------------------------------------------
// ConcreteReferencingObjectMap – private to this translation unit.
//
// ReferencingObjectMap only declares the two-row generateRDFTerm variant, so
// it remains abstract with respect to TermMap's single-row pure virtual.
// This wrapper satisfies the interface; actual resolution requires both rows.
// ---------------------------------------------------------------------------
class ConcreteReferencingObjectMap : public ReferencingObjectMap {
public:
    SerdNode generateRDFTerm(const SQLRow& /*row*/, const SerdEnv& /*env*/) const override
    {
        return SERD_NODE_NULL; // use the two-row overload for actual generation
    }
};

// ---------------------------------------------------------------------------
// Build helpers
// ---------------------------------------------------------------------------

/// Build a LogicalTable from a blank-node or named-resource key.
static std::unique_ptr<LogicalTable>
buildLogicalTable(const TripleStore& ts, const std::string& ltKey)
{
    std::string tableName = getFirstLiteral(ts, ltKey, RR + "tableName");
    if (!tableName.empty())
        return std::unique_ptr<BaseTableOrView>(new BaseTableOrView(tableName));

    std::string sqlQuery = getFirstLiteral(ts, ltKey, RR + "sqlQuery");
    if (!sqlQuery.empty())
        return std::unique_ptr<R2RMLView>(new R2RMLView(sqlQuery));

    std::cerr << "R2RML parser: unrecognised logical table <" << ltKey << ">\n";
    return nullptr;
}

/// Wrap a URI string in a ConstantTermMap (which will own the string data).
static std::unique_ptr<ConstantTermMap> makeConstantUri(const std::string& uri)
{
    SerdNode node = serd_node_from_string(
        SERD_URI, reinterpret_cast<const uint8_t*>(uri.c_str()));
    return std::unique_ptr<ConstantTermMap>(new ConstantTermMap(node));
}

/// Build a generic TermMap (Column / Template / Constant / ReferencingObjectMap)
/// from a node key.  Appends to `parentRefs` for later resolution.
static std::unique_ptr<TermMap>
buildTermMap(const TripleStore& ts, const std::string& nodeKey,
             std::vector<std::pair<ReferencingObjectMap*, std::string>>& parentRefs)
{
    // rr:column
    std::string column = getFirstLiteral(ts, nodeKey, RR + "column");
    if (!column.empty())
        return std::unique_ptr<ColumnTermMap>(new ColumnTermMap(column));

    // rr:template
    std::string tmpl = getFirstLiteral(ts, nodeKey, RR + "template");
    if (!tmpl.empty())
        return std::unique_ptr<TemplateTermMap>(new TemplateTermMap(tmpl));

    // rr:constant (URI object)
    std::string constant = getFirstUri(ts, nodeKey, RR + "constant");
    if (!constant.empty())
        return makeConstantUri(constant);

    // rr:parentTriplesMap → ReferencingObjectMap
    std::string parentUri = getFirstUri(ts, nodeKey, RR + "parentTriplesMap");
    if (!parentUri.empty()) {
        auto rom = std::unique_ptr<ConcreteReferencingObjectMap>(new ConcreteReferencingObjectMap());

        const auto* jcObjs = getObjects(ts, nodeKey, RR + "joinCondition");
        if (jcObjs) {
            for (const auto& jcObj : *jcObjs) {
                std::string jcKey = objKey(jcObj);
                if (jcKey.empty()) continue;
                std::string child  = getFirstLiteral(ts, jcKey, RR + "child");
                std::string parent = getFirstLiteral(ts, jcKey, RR + "parent");
                rom->joinConditions.emplace_back(child, parent);
            }
        }

        parentRefs.emplace_back(rom.get(), parentUri);
        return rom;
    }

    return nullptr; // unknown – caller may warn
}

/// Build a SubjectMap (and its ConcreteSubjectMap wrapper) from a node key.
static std::unique_ptr<SubjectMap>
buildSubjectMap(const TripleStore& ts, const std::string& smKey,
                std::vector<std::pair<ReferencingObjectMap*, std::string>>& parentRefs)
{
    auto sm = std::unique_ptr<ConcreteSubjectMap>(new ConcreteSubjectMap());

    // Value-generation strategy
    std::string tmpl     = getFirstLiteral(ts, smKey, RR + "template");
    std::string column   = getFirstLiteral(ts, smKey, RR + "column");
    std::string constant = getFirstUri(ts, smKey, RR + "constant");

    if (!tmpl.empty())
        sm->valueMap = std::unique_ptr<TemplateTermMap>(new TemplateTermMap(tmpl));
    else if (!column.empty())
        sm->valueMap = std::unique_ptr<ColumnTermMap>(new ColumnTermMap(column));
    else if (!constant.empty())
        sm->valueMap = makeConstantUri(constant);

    // rr:class assertions
    const auto* classObjs = getObjects(ts, smKey, RR + "class");
    if (classObjs) {
        for (const auto& cls : *classObjs)
            if (cls.type == ObjType::URI)
                sm->classIRIs.push_back(cls.value);
    }

    return sm;
}

/// Build a PredicateObjectMap from a blank-node key.
static std::unique_ptr<PredicateObjectMap>
buildPOM(const TripleStore& ts, const std::string& pomKey,
         std::vector<std::pair<ReferencingObjectMap*, std::string>>& parentRefs)
{
    auto pom = std::unique_ptr<PredicateObjectMap>(new PredicateObjectMap());

    // rr:predicate shortcut (constant predicate)
    const auto* predObjs = getObjects(ts, pomKey, RR + "predicate");
    if (predObjs) {
        for (const auto& p : *predObjs)
            if (p.type == ObjType::URI)
                pom->predicateMaps.push_back(makeConstantUri(p.value));
    }

    // rr:predicateMap (full predicate map)
    const auto* predMapObjs = getObjects(ts, pomKey, RR + "predicateMap");
    if (predMapObjs) {
        for (const auto& pm : *predMapObjs) {
            std::string pmKey = objKey(pm);
            if (pmKey.empty()) continue;
            auto tm = buildTermMap(ts, pmKey, parentRefs);
            if (tm) pom->predicateMaps.push_back(std::move(tm));
        }
    }

    // rr:object shortcut (constant URI object)
    const auto* objObjs = getObjects(ts, pomKey, RR + "object");
    if (objObjs) {
        for (const auto& o : *objObjs)
            if (o.type == ObjType::URI)
                pom->objectMaps.push_back(makeConstantUri(o.value));
    }

    // rr:objectMap (full object map)
    const auto* objMapObjs = getObjects(ts, pomKey, RR + "objectMap");
    if (objMapObjs) {
        for (const auto& om : *objMapObjs) {
            std::string omKey = objKey(om);
            if (omKey.empty()) continue;
            auto tm = buildTermMap(ts, omKey, parentRefs);
            if (tm)
                pom->objectMaps.push_back(std::move(tm));
            else
                std::cerr << "R2RML parser: unknown object map type for <"
                          << omKey << ">\n";
        }
    }

    return pom;
}

// ---------------------------------------------------------------------------
// R2RMLParser implementation
// ---------------------------------------------------------------------------

R2RMLParser::R2RMLParser()  = default;
R2RMLParser::~R2RMLParser() = default;

R2RMLMapping R2RMLParser::parse(const std::string& mappingFilePath)
{
    // -----------------------------------------------------------------------
    // Phase 1 – collect all triples via Serd
    // -----------------------------------------------------------------------
    ParseState state;
    state.env = serd_env_new(nullptr);

    SerdReader* reader = serd_reader_new(
        SERD_TURTLE, &state, nullptr,
        cbBase, cbPrefix, cbStatement, /*end_sink=*/nullptr);
    serd_reader_set_error_sink(reader, cbError, nullptr);

    // Convert the filesystem path to a file URI and use it as the document base.
    SerdNode fileUriNode = serd_node_new_file_uri(
        reinterpret_cast<const uint8_t*>(mappingFilePath.c_str()),
        /*hostname=*/nullptr, /*out=*/nullptr, /*escape=*/true);

    if (fileUriNode.buf) {
        serd_env_set_base_uri(state.env, &fileUriNode);
        serd_reader_read_file(reader, fileUriNode.buf);
        serd_node_free(&fileUriNode);
    } else {
        std::cerr << "R2RML parser: could not build file URI for: "
                  << mappingFilePath << "\n";
    }

    serd_reader_free(reader);

    // -----------------------------------------------------------------------
    // Phase 2 – build the R2RMLMapping from the collected triples
    // -----------------------------------------------------------------------
    R2RMLMapping mapping;
    mapping.serdEnvironment = state.env; // transfer ownership
    state.env = nullptr;

    // Accumulate (rom*, parentUri) pairs; resolve after all TMs are built.
    std::vector<std::pair<ReferencingObjectMap*, std::string>> parentRefs;

    // Identify TriplesMap subjects: any non-blank named resource carrying at
    // least one characteristic R2RML TriplesMap predicate.
    for (const auto& entry : state.triples) {
        const std::string& subj  = entry.first;
        const PredMap&     preds = entry.second;

        // Skip blank nodes – they appear only as parts of maps, not TM subjects.
        if (subj.size() >= 2 && subj[0] == '_' && subj[1] == ':') continue;

        bool isTriplesMap =
            preds.count(RR + "logicalTable")      ||
            preds.count(RR + "subjectMap")         ||
            preds.count(RR + "predicateObjectMap") ||
            preds.count(RR + "subject");
        if (!isTriplesMap) continue;

        auto tm = std::unique_ptr<TriplesMap>(new TriplesMap());
        tm->id = subj;

        // Logical table (inline blank node or named resource)
        std::string ltKey = getFirstObjKey(state.triples, subj, RR + "logicalTable");
        if (!ltKey.empty())
            tm->logicalTable = buildLogicalTable(state.triples, ltKey);

        // Subject map
        std::string smKey = getFirstObjKey(state.triples, subj, RR + "subjectMap");
        if (!smKey.empty())
            tm->subjectMap = buildSubjectMap(state.triples, smKey, parentRefs);

        // Predicate-object maps (there may be several)
        const auto* pomObjs =
            getObjects(state.triples, subj, RR + "predicateObjectMap");
        if (pomObjs) {
            for (const auto& pomObj : *pomObjs) {
                std::string pomKey = objKey(pomObj);
                if (pomKey.empty()) continue;
                auto pom = buildPOM(state.triples, pomKey, parentRefs);
                if (pom) tm->predicateObjectMaps.push_back(std::move(pom));
            }
        }

        mapping.triplesMaps.push_back(std::move(tm));
    }

    // -----------------------------------------------------------------------
    // Phase 3 – resolve parentTriplesMap back-references
    // -----------------------------------------------------------------------
    for (auto& ref : parentRefs) {
        bool found = false;
        for (const auto& tm : mapping.triplesMaps) {
            if (tm->id == ref.second) {
                ref.first->parentTriplesMap = tm.get();
                found = true;
                break;
            }
        }
        if (!found)
            std::cerr << "R2RML parser: unresolved parentTriplesMap <"
                      << ref.second << ">\n";
    }

    return mapping;
}

} // namespace r2rml
