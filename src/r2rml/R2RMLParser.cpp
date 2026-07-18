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

#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace r2rml {

// ---------------------------------------------------------------------------
// R2RML namespace prefix (shared with YARRRMLParser.cpp; see vocab in
// MappingParser.h)
// ---------------------------------------------------------------------------
using vocab::RR_BLANKNODE_TERM_TYPE;
using vocab::RR_CHILD;
using vocab::RR_CLASS;
using vocab::RR_COLUMN;
using vocab::RR_CONSTANT;
using vocab::RR_DATATYPE;
using vocab::RR_IRI_TERM_TYPE;
using vocab::RR_JOIN_CONDITION;
using vocab::RR_LANGUAGE;
using vocab::RR_LITERAL_TERM_TYPE;
using vocab::RR_LOGICAL_TABLE;
using vocab::RR_OBJECT;
using vocab::RR_OBJECT_MAP;
using vocab::RR_PARENT;
using vocab::RR_PARENTTRIPLESMAP;
using vocab::RR_PREDICATE;
using vocab::RR_PREDICATE_MAP;
using vocab::RR_PREDICATE_OBJECT_MAP;
using vocab::RR_SQL_QUERY;
using vocab::RR_SUBJECT;
using vocab::RR_SUBJECT_MAP;
using vocab::RR_TABLE_NAME;
using vocab::RR_TEMPLATE;
using vocab::RR_TERM_TYPE;

// ---------------------------------------------------------------------------
// Raw triple-store types
// ---------------------------------------------------------------------------
enum class ObjType { URI, Blank, Literal };

struct ObjValue {
	ObjType type;
	std::string value;    ///< URI string, blank-node ID (no "_:" prefix), or literal text
	std::string datatype; ///< For typed literals – full URI
	std::string lang;     ///< For language-tagged literals
};

using PredMap = std::map<std::string, std::vector<ObjValue>>;
using TripleStore = std::map<std::string, PredMap>;

// ---------------------------------------------------------------------------
// Node-expansion helper
// ---------------------------------------------------------------------------

/// Convert a SerdNode to an absolute URI string (or "_:<id>" for blank nodes).
/// CURIEs are expanded using `env`; relative URIs are resolved against the base.
/// Returns an empty string if the node cannot be represented.
static std::string expandNode(SerdEnv *env, const SerdNode *node) {
	if (!node || node->type == SERD_NOTHING) {
		return {};
	}

	if (node->type == SERD_BLANK) {
		return std::string("_:") + std::string(reinterpret_cast<const char *>(node->buf), node->n_bytes);
	}

	// URI or CURIE – ask the environment to expand/resolve
	SerdNode expanded = serd_env_expand_node(env, node);
	if (expanded.type != SERD_NOTHING && expanded.buf) {
		std::string result(reinterpret_cast<const char *>(expanded.buf), expanded.n_bytes);
		serd_node_free(&expanded);
		return result;
	}

	// Fallback: return the raw value (handles already-absolute URIs)
	if (node->buf) {
		return std::string(reinterpret_cast<const char *>(node->buf), node->n_bytes);
	}
	return {};
}

// ---------------------------------------------------------------------------
// TripleCollector – gathers statements (from Serd or built directly by a
// caller) into a TripleStore, independent of their origin.
// ---------------------------------------------------------------------------

struct TripleCollector::Impl {
	SerdEnv *env {serd_env_new(nullptr)};
	TripleStore triples;
	std::vector<std::string> errors;

	~Impl() {
		if (env) {
			serd_env_free(env);
		}
	}
};

TripleCollector::TripleCollector() : impl_(new Impl()) {
}

TripleCollector::~TripleCollector() = default;

void TripleCollector::setBase(const SerdNode *base) {
	serd_env_set_base_uri(impl_->env, base);
}

void TripleCollector::setPrefix(const SerdNode *name, const SerdNode *uri) {
	serd_env_set_prefix(impl_->env, name, uri);
}

void TripleCollector::statement(const SerdNode *subject, const SerdNode *predicate, const SerdNode *object,
                                const SerdNode *objectDatatype, const SerdNode *objectLang) {
	std::string subjKey = expandNode(impl_->env, subject);
	std::string predKey = expandNode(impl_->env, predicate);
	if (subjKey.empty() || predKey.empty()) {
		return;
	}

	ObjValue obj;
	if (object->type == SERD_BLANK) {
		obj.type = ObjType::Blank;
		obj.value = std::string(reinterpret_cast<const char *>(object->buf), object->n_bytes);
	} else if (object->type == SERD_LITERAL) {
		obj.type = ObjType::Literal;
		obj.value = std::string(reinterpret_cast<const char *>(object->buf), object->n_bytes);
		if (objectDatatype && objectDatatype->buf) {
			obj.datatype = expandNode(impl_->env, objectDatatype);
		}
		if (objectLang && objectLang->buf) {
			obj.lang = std::string(reinterpret_cast<const char *>(objectLang->buf), objectLang->n_bytes);
		}
	} else {
		obj.type = ObjType::URI;
		obj.value = expandNode(impl_->env, object);
	}

	impl_->triples[subjKey][predKey].push_back(std::move(obj));
}

void TripleCollector::addError(const std::string &message) {
	impl_->errors.push_back(message);
}

// ---------------------------------------------------------------------------
// Serd callbacks – forward into a TripleCollector so the text-parsing paths
// (parse(), parseString()) share the exact same statement-insertion logic
// as callers that build statements directly (parseCollected()).
// ---------------------------------------------------------------------------

static SerdStatus cbBase(void *handle, const SerdNode *base) {
	static_cast<TripleCollector *>(handle)->setBase(base);
	return SERD_SUCCESS;
}

static SerdStatus cbPrefix(void *handle, const SerdNode *name, const SerdNode *uri) {
	static_cast<TripleCollector *>(handle)->setPrefix(name, uri);
	return SERD_SUCCESS;
}

static SerdStatus cbStatement(void *handle, SerdStatementFlags /*flags*/, const SerdNode * /*graph*/,
                              const SerdNode *subject, const SerdNode *predicate, const SerdNode *object,
                              const SerdNode *object_datatype, const SerdNode *object_lang) {
	static_cast<TripleCollector *>(handle)->statement(subject, predicate, object, object_datatype, object_lang);
	return SERD_SUCCESS;
}

static SerdStatus cbError(void * /*handle*/, const SerdError * /*error*/) {
	return SERD_SUCCESS; // non-fatal: keep going
}

// ---------------------------------------------------------------------------
// Triple-store query helpers (pure utilities – no build state needed)
// ---------------------------------------------------------------------------

static const std::vector<ObjValue> *getObjects(const TripleStore &ts, const std::string &subj,
                                               const std::string &pred) {
	auto si = ts.find(subj);
	if (si == ts.end()) {
		return nullptr;
	}
	auto pi = si->second.find(pred);
	if (pi == si->second.end()) {
		return nullptr;
	}
	return &pi->second;
}

static std::string getFirstLiteral(const TripleStore &ts, const std::string &subj, const std::string &pred) {
	const auto *objs = getObjects(ts, subj, pred);
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

static std::string getFirstUri(const TripleStore &ts, const std::string &subj, const std::string &pred) {
	const auto *objs = getObjects(ts, subj, pred);
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

/// Return the canonical lookup key for an ObjValue: "_:<id>" for blank nodes,
/// URI string for named nodes, empty string for literals.
static std::string objKey(const ObjValue &o) {
	if (o.type == ObjType::Blank) {
		return "_:" + o.value;
	}
	if (o.type == ObjType::URI) {
		return o.value;
	}
	return {};
}

/// Return the first object of a predicate as a subject-lookup key.
static std::string getFirstObjKey(const TripleStore &ts, const std::string &subj, const std::string &pred) {
	const auto *objs = getObjects(ts, subj, pred);
	if (!objs || objs->empty()) {
		return {};
	}
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

	SerdNode generateRDFTerm(const SQLRow &row, const SerdEnv &env) const override {
		if (valueMap) {
			return valueMap->generateRDFTerm(row, env);
		}
		return SERD_NODE_NULL;
	}

	const TermMap *valueTermMap() const override {
		return valueMap.get();
	}

	std::ostream &print(std::ostream &os) const override {
		os << "SubjectMap {";
		if (valueMap) {
			os << " valueMap=" << *valueMap;
		}
		if (!classIRIs.empty()) {
			os << " classes=[";
			for (std::size_t i = 0; i < classIRIs.size(); ++i) {
				if (i) {
					os << ", ";
				}
				os << classIRIs[i];
			}
			os << "]";
		}
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
	SerdNode generateRDFTerm(const SQLRow & /*row*/, const SerdEnv & /*env*/) const override {
		return SERD_NODE_NULL; // use the two-row overload for actual generation
	}
};

// ---------------------------------------------------------------------------
// ParseContext – owns build-phase state and exposes the build methods.
//
// Holding errors and parentRefs as members eliminates the need to thread them
// through every build-method signature.
// ---------------------------------------------------------------------------
class ParseContext {
public:
	const TripleStore &ts;
	std::vector<std::string> errors;
	std::vector<std::pair<ReferencingObjectMap *, std::string>> parentRefs;

	explicit ParseContext(const TripleStore &ts) : ts(ts) {
	}

	// ------------------------------------------------------------------
	// Wrap a URI string in a ConstantTermMap.
	// ------------------------------------------------------------------
	static std::unique_ptr<ConstantTermMap> makeConstantUri(const std::string &uri) {
		SerdNode node = serd_node_from_string(SERD_URI, reinterpret_cast<const uint8_t *>(uri.c_str()));
		return std::unique_ptr<ConstantTermMap>(new ConstantTermMap(node));
	}

	// ------------------------------------------------------------------
	// Wrap a literal string in a ConstantTermMap (rr:constant with a literal
	// object, e.g. `rr:constant "active"`).
	// ------------------------------------------------------------------
	static std::unique_ptr<ConstantTermMap> makeConstantLiteral(const std::string &literal) {
		SerdNode node = serd_node_from_string(SERD_LITERAL, reinterpret_cast<const uint8_t *>(literal.c_str()));
		auto tm = std::unique_ptr<ConstantTermMap>(new ConstantTermMap(node));
		tm->termType = TermType::Literal;
		return tm;
	}

	// ------------------------------------------------------------------
	// Read rr:termType (if present) from `nodeKey` and, when recognised,
	// override `tm`'s termType.  Called after any default term-type has
	// already been applied so an explicit rr:termType always wins.
	// ------------------------------------------------------------------
	void applyExplicitTermType(const std::string &nodeKey, TermMap &tm) {
		std::string tt = getFirstUri(ts, nodeKey, RR_TERM_TYPE);
		if (tt == RR_IRI_TERM_TYPE) {
			tm.termType = TermType::IRI;
		} else if (tt == RR_LITERAL_TERM_TYPE) {
			tm.termType = TermType::Literal;
		} else if (tt == RR_BLANKNODE_TERM_TYPE) {
			tm.termType = TermType::BlankNode;
		}
	}

	// ------------------------------------------------------------------
	// Read rr:language (if present) from `nodeKey` and set `tm`'s
	// languageTag.
	// ------------------------------------------------------------------
	void applyLanguage(const std::string &nodeKey, TermMap &tm) {
		std::string lang = getFirstLiteral(ts, nodeKey, RR_LANGUAGE);
		if (!lang.empty()) {
			tm.languageTag = std::unique_ptr<std::string>(new std::string(lang));
		}
	}

	// ------------------------------------------------------------------
	// Build a LogicalTable from a blank-node or named-resource key.
	// ------------------------------------------------------------------
	std::unique_ptr<LogicalTable> buildLogicalTable(const std::string &ltKey) {
		std::string tableName = getFirstLiteral(ts, ltKey, RR_TABLE_NAME);
		if (!tableName.empty()) {
			return std::unique_ptr<BaseTableOrView>(new BaseTableOrView(tableName));
		}

		std::string sqlQuery = getFirstLiteral(ts, ltKey, RR_SQL_QUERY);
		if (!sqlQuery.empty()) {
			return std::unique_ptr<R2RMLView>(new R2RMLView(sqlQuery));
		}

		errors.push_back("R2RML parser: unrecognised logical table <" + ltKey + ">");
		return nullptr;
	}

	// ------------------------------------------------------------------
	// Build a generic TermMap (Column / Template / Constant /
	// ReferencingObjectMap) from a node key.  Appends to parentRefs for
	// later resolution.
	// ------------------------------------------------------------------
	std::unique_ptr<TermMap> buildTermMap(const std::string &nodeKey) {
		// rr:column
		std::string column = getFirstLiteral(ts, nodeKey, RR_COLUMN);
		if (!column.empty()) {
			auto tm = std::unique_ptr<ColumnTermMap>(new ColumnTermMap(column));
			std::string dt = getFirstUri(ts, nodeKey, RR_DATATYPE);
			if (!dt.empty()) {
				tm->datatypeIRI = std::unique_ptr<std::string>(new std::string(dt));
			}
			applyLanguage(nodeKey, *tm);
			applyExplicitTermType(nodeKey, *tm);
			return tm;
		}

		// rr:template
		std::string tmpl = getFirstLiteral(ts, nodeKey, RR_TEMPLATE);
		if (!tmpl.empty()) {
			auto tm = std::unique_ptr<TemplateTermMap>(new TemplateTermMap(tmpl));
			std::string dt = getFirstUri(ts, nodeKey, RR_DATATYPE);
			if (!dt.empty()) {
				tm->datatypeIRI = std::unique_ptr<std::string>(new std::string(dt));
			}
			applyLanguage(nodeKey, *tm);
			applyExplicitTermType(nodeKey, *tm);
			return tm;
		}

		// rr:constant (URI or literal object)
		const auto *constObjs = getObjects(ts, nodeKey, RR_CONSTANT);
		if (constObjs) {
			for (const auto &c : *constObjs) {
				if (c.type == ObjType::URI) {
					return makeConstantUri(c.value);
				}
			}
			for (const auto &c : *constObjs) {
				if (c.type == ObjType::Literal) {
					return makeConstantLiteral(c.value);
				}
			}
		}

		// rr:parentTriplesMap → ReferencingObjectMap
		std::string parentUri = getFirstUri(ts, nodeKey, RR_PARENTTRIPLESMAP);
		if (!parentUri.empty()) {
			auto rom = std::unique_ptr<ConcreteReferencingObjectMap>(new ConcreteReferencingObjectMap());

			const auto *jcObjs = getObjects(ts, nodeKey, RR_JOIN_CONDITION);
			if (jcObjs) {
				for (const auto &jcObj : *jcObjs) {
					std::string jcKey = objKey(jcObj);
					if (jcKey.empty()) {
						continue;
					}
					std::string child = getFirstLiteral(ts, jcKey, RR_CHILD);
					std::string parent = getFirstLiteral(ts, jcKey, RR_PARENT);
					rom->joinConditions.emplace_back(child, parent);
				}
			}

			parentRefs.emplace_back(rom.get(), parentUri);
			return rom;
		}

		return nullptr; // unknown – caller may warn
	}

	// ------------------------------------------------------------------
	// Build a SubjectMap from a node key.
	// ------------------------------------------------------------------
	std::unique_ptr<SubjectMap> buildSubjectMap(const std::string &smKey) {
		auto sm = std::unique_ptr<ConcreteSubjectMap>(new ConcreteSubjectMap());

		// Value-generation strategy
		std::string tmpl = getFirstLiteral(ts, smKey, RR_TEMPLATE);
		std::string column = getFirstLiteral(ts, smKey, RR_COLUMN);
		std::string constant = getFirstUri(ts, smKey, RR_CONSTANT);

		if (!tmpl.empty()) {
			sm->valueMap = std::unique_ptr<TemplateTermMap>(new TemplateTermMap(tmpl));
		} else if (!column.empty()) {
			sm->valueMap = std::unique_ptr<ColumnTermMap>(new ColumnTermMap(column));
		} else if (!constant.empty()) {
			sm->valueMap = makeConstantUri(constant);
		}

		// rr:class assertions
		const auto *classObjs = getObjects(ts, smKey, RR_CLASS);
		if (classObjs) {
			for (const auto &cls : *classObjs) {
				if (cls.type == ObjType::URI) {
					sm->classIRIs.push_back(cls.value);
				}
			}
		}

		return sm;
	}

	// ------------------------------------------------------------------
	// Build a PredicateObjectMap from a blank-node key.
	// ------------------------------------------------------------------
	std::unique_ptr<PredicateObjectMap> buildPOM(const std::string &pomKey) {
		auto pom = std::unique_ptr<PredicateObjectMap>(new PredicateObjectMap());

		// rr:predicate shortcut (constant predicate)
		const auto *predObjs = getObjects(ts, pomKey, RR_PREDICATE);
		if (predObjs) {
			for (const auto &p : *predObjs) {
				if (p.type == ObjType::URI) {
					pom->predicateMaps.push_back(makeConstantUri(p.value));
				}
			}
		}

		// rr:predicateMap (full predicate map)
		const auto *predMapObjs = getObjects(ts, pomKey, RR_PREDICATE_MAP);
		if (predMapObjs) {
			for (const auto &pm : *predMapObjs) {
				std::string pmKey = objKey(pm);
				if (pmKey.empty()) {
					continue;
				}
				auto tm = buildTermMap(pmKey);
				if (tm) {
					pom->predicateMaps.push_back(std::move(tm));
				}
			}
		}

		// rr:object shortcut (constant URI object)
		const auto *objObjs = getObjects(ts, pomKey, RR_OBJECT);
		if (objObjs) {
			for (const auto &o : *objObjs) {
				if (o.type == ObjType::URI) {
					pom->objectMaps.push_back(makeConstantUri(o.value));
				}
			}
		}

		// rr:objectMap (full object map)
		const auto *objMapObjs = getObjects(ts, pomKey, RR_OBJECT_MAP);
		if (objMapObjs) {
			for (const auto &om : *objMapObjs) {
				std::string omKey = objKey(om);
				if (omKey.empty()) {
					continue;
				}
				auto tm = buildTermMap(omKey);
				if (tm) {
					// Per R2RML spec: default term type for rr:column in an
					// objectMap is rr:Literal (not rr:IRI), unless an explicit
					// rr:termType was given on the object map (already applied
					// by buildTermMap()), which always wins.
					if (dynamic_cast<ColumnTermMap *>(tm.get()) && getFirstUri(ts, omKey, RR_TERM_TYPE).empty()) {
						tm->termType = TermType::Literal;
					}
					pom->objectMaps.push_back(std::move(tm));
				} else {
					errors.push_back("R2RML parser: unknown object map type for <" + omKey + ">");
				}
			}
		}

		return pom;
	}
};

// ---------------------------------------------------------------------------
// Shared build phase (phases 2-4): construct the R2RMLMapping object model
// from a fully-populated TripleStore, resolve parentTriplesMap references,
// and report/throw any collected non-fatal errors.  Used by both parse() and
// parseString() so their behaviour (aside from how triples are collected) is
// identical.
//
// `env` ownership is transferred to the returned mapping.  `preErrors` are
// errors collected during phase 1 (e.g. a malformed file URI) and are merged
// with any build-phase errors before phase 4 reporting.
// ---------------------------------------------------------------------------
static R2RMLMapping buildMappingFromTriples(TripleStore &triples, SerdEnv *env, std::vector<std::string> preErrors,
                                            bool ignoreNonFatalErrors) {
	R2RMLMapping mapping;
	mapping.serdEnvironment = env; // transfer ownership

	ParseContext ctx(triples);
	ctx.errors = std::move(preErrors);

	// Identify TriplesMap subjects: any non-blank named resource carrying at
	// least one characteristic R2RML TriplesMap predicate.
	for (const auto &entry : triples) {
		const std::string &subj = entry.first;
		const PredMap &preds = entry.second;

		// Skip blank nodes – they appear only as parts of maps, not TM subjects.
		if (subj.size() >= 2 && subj[0] == '_' && subj[1] == ':') {
			continue;
		}

		bool isTriplesMap = preds.count(RR_LOGICAL_TABLE) || preds.count(RR_SUBJECT_MAP) ||
		                    preds.count(RR_PREDICATE_OBJECT_MAP) || preds.count(RR_SUBJECT);
		if (!isTriplesMap) {
			continue;
		}

		auto tm = std::unique_ptr<TriplesMap>(new TriplesMap());
		tm->id = subj;

		// Logical table (inline blank node or named resource)
		std::string ltKey = getFirstObjKey(triples, subj, RR_LOGICAL_TABLE);
		if (!ltKey.empty()) {
			tm->logicalTable = ctx.buildLogicalTable(ltKey);
		}

		// Subject map
		std::string smKey = getFirstObjKey(triples, subj, RR_SUBJECT_MAP);
		if (!smKey.empty()) {
			tm->subjectMap = ctx.buildSubjectMap(smKey);
		}

		// Predicate-object maps (there may be several)
		const auto *pomObjs = getObjects(triples, subj, RR_PREDICATE_OBJECT_MAP);
		if (pomObjs) {
			for (const auto &pomObj : *pomObjs) {
				std::string pomKey = objKey(pomObj);
				if (pomKey.empty()) {
					continue;
				}
				auto pom = ctx.buildPOM(pomKey);
				if (pom) {
					tm->predicateObjectMaps.push_back(std::move(pom));
				}
			}
		}

		mapping.triplesMaps.push_back(std::move(tm));
	}

	// -----------------------------------------------------------------------
	// Phase 3 – resolve parentTriplesMap back-references
	// -----------------------------------------------------------------------
	for (auto &ref : ctx.parentRefs) {
		bool found = false;
		for (const auto &tm : mapping.triplesMaps) {
			if (tm->id == ref.second) {
				ref.first->parentTriplesMap = tm.get();
				found = true;
				break;
			}
		}
		if (!found) {
			ctx.errors.push_back("R2RML parser: unresolved parentTriplesMap <" + ref.second + ">");
		}
	}

	// -----------------------------------------------------------------------
	// Phase 4 – report any collected errors
	// -----------------------------------------------------------------------
	if (!ctx.errors.empty()) {
		if (ignoreNonFatalErrors) {
			mapping.parseErrors = std::move(ctx.errors);
		} else {
			std::ostringstream msg;
			for (const auto &e : ctx.errors) {
				msg << e << "\n";
			}
			throw std::runtime_error(msg.str());
		}
	}

	return mapping;
}

// ---------------------------------------------------------------------------
// R2RMLParser implementation
// ---------------------------------------------------------------------------

R2RMLParser::R2RMLParser() = default;

R2RMLMapping R2RMLParser::parse(const std::string &mappingFilePath, bool ignoreNonFatalErrors) {
	// -----------------------------------------------------------------------
	// Phase 1 – collect all triples via Serd
	// -----------------------------------------------------------------------
	TripleCollector collector;

	SerdReader *reader =
	    serd_reader_new(SERD_TURTLE, &collector, nullptr, cbBase, cbPrefix, cbStatement, /*end_sink=*/nullptr);
	serd_reader_set_error_sink(reader, cbError, nullptr);

	// Convert the filesystem path to a file URI and use it as the document base.
	SerdNode fileUriNode = serd_node_new_file_uri(reinterpret_cast<const uint8_t *>(mappingFilePath.c_str()),
	                                              /*hostname=*/nullptr, /*out=*/nullptr, /*escape=*/true);

	if (fileUriNode.buf) {
		collector.setBase(&fileUriNode);
		serd_reader_read_file(reader, fileUriNode.buf);
		serd_node_free(&fileUriNode);
	} else {
		collector.addError("R2RML parser: could not build file URI for: " + mappingFilePath);
	}

	serd_reader_free(reader);

	return parseCollected(collector, ignoreNonFatalErrors);
}

R2RMLMapping R2RMLParser::parseString(const std::string &turtleText, const std::string &baseUri,
                                      bool ignoreNonFatalErrors) {
	// -----------------------------------------------------------------------
	// Phase 1 – collect all triples via Serd, reading from an in-memory string
	// -----------------------------------------------------------------------
	TripleCollector collector;

	SerdReader *reader =
	    serd_reader_new(SERD_TURTLE, &collector, nullptr, cbBase, cbPrefix, cbStatement, /*end_sink=*/nullptr);
	serd_reader_set_error_sink(reader, cbError, nullptr);

	SerdNode baseNode = serd_node_from_string(SERD_URI, reinterpret_cast<const uint8_t *>(baseUri.c_str()));
	collector.setBase(&baseNode);

	serd_reader_read_string(reader, reinterpret_cast<const uint8_t *>(turtleText.c_str()));

	serd_reader_free(reader);

	return parseCollected(collector, ignoreNonFatalErrors);
}

R2RMLMapping R2RMLParser::parseCollected(TripleCollector &collector, bool ignoreNonFatalErrors) {
	SerdEnv *env = collector.impl_->env; // transfer ownership out of the collector
	collector.impl_->env = nullptr;

	return buildMappingFromTriples(collector.impl_->triples, env, std::move(collector.impl_->errors),
	                               ignoreNonFatalErrors);
}

} // namespace r2rml
