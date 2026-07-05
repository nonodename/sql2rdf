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
// R2RML namespace prefix
// ---------------------------------------------------------------------------
static const std::string RR = "http://www.w3.org/ns/r2rml#";

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

struct ParseState {
	SerdEnv *env {nullptr};
	TripleStore triples;
};

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
// Serd callbacks
// ---------------------------------------------------------------------------

static SerdStatus cbBase(void *handle, const SerdNode *base) {
	auto *state = static_cast<ParseState *>(handle);
	serd_env_set_base_uri(state->env, base);
	return SERD_SUCCESS;
}

static SerdStatus cbPrefix(void *handle, const SerdNode *name, const SerdNode *uri) {
	auto *state = static_cast<ParseState *>(handle);
	serd_env_set_prefix(state->env, name, uri);
	return SERD_SUCCESS;
}

static SerdStatus cbStatement(void *handle, SerdStatementFlags /*flags*/, const SerdNode * /*graph*/,
                              const SerdNode *subject, const SerdNode *predicate, const SerdNode *object,
                              const SerdNode *object_datatype, const SerdNode *object_lang) {
	auto *state = static_cast<ParseState *>(handle);

	std::string subjKey = expandNode(state->env, subject);
	std::string predKey = expandNode(state->env, predicate);
	if (subjKey.empty() || predKey.empty()) {
		return SERD_SUCCESS;
	}

	ObjValue obj;
	if (object->type == SERD_BLANK) {
		obj.type = ObjType::Blank;
		obj.value = std::string(reinterpret_cast<const char *>(object->buf), object->n_bytes);
	} else if (object->type == SERD_LITERAL) {
		obj.type = ObjType::Literal;
		obj.value = std::string(reinterpret_cast<const char *>(object->buf), object->n_bytes);
		if (object_datatype && object_datatype->buf) {
			obj.datatype = expandNode(state->env, object_datatype);
		}
		if (object_lang && object_lang->buf) {
			obj.lang = std::string(reinterpret_cast<const char *>(object_lang->buf), object_lang->n_bytes);
		}
	} else {
		obj.type = ObjType::URI;
		obj.value = expandNode(state->env, object);
	}

	state->triples[subjKey][predKey].push_back(std::move(obj));
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
		std::string tt = getFirstUri(ts, nodeKey, RR + "termType");
		if (tt == RR + "IRI") {
			tm.termType = TermType::IRI;
		} else if (tt == RR + "Literal") {
			tm.termType = TermType::Literal;
		} else if (tt == RR + "BlankNode") {
			tm.termType = TermType::BlankNode;
		}
	}

	// ------------------------------------------------------------------
	// Read rr:language (if present) from `nodeKey` and set `tm`'s
	// languageTag.
	// ------------------------------------------------------------------
	void applyLanguage(const std::string &nodeKey, TermMap &tm) {
		std::string lang = getFirstLiteral(ts, nodeKey, RR + "language");
		if (!lang.empty()) {
			tm.languageTag = std::unique_ptr<std::string>(new std::string(lang));
		}
	}

	// ------------------------------------------------------------------
	// Build a LogicalTable from a blank-node or named-resource key.
	// ------------------------------------------------------------------
	std::unique_ptr<LogicalTable> buildLogicalTable(const std::string &ltKey) {
		std::string tableName = getFirstLiteral(ts, ltKey, RR + "tableName");
		if (!tableName.empty()) {
			return std::unique_ptr<BaseTableOrView>(new BaseTableOrView(tableName));
		}

		std::string sqlQuery = getFirstLiteral(ts, ltKey, RR + "sqlQuery");
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
		std::string column = getFirstLiteral(ts, nodeKey, RR + "column");
		if (!column.empty()) {
			auto tm = std::unique_ptr<ColumnTermMap>(new ColumnTermMap(column));
			std::string dt = getFirstUri(ts, nodeKey, RR + "datatype");
			if (!dt.empty()) {
				tm->datatypeIRI = std::unique_ptr<std::string>(new std::string(dt));
			}
			applyLanguage(nodeKey, *tm);
			applyExplicitTermType(nodeKey, *tm);
			return tm;
		}

		// rr:template
		std::string tmpl = getFirstLiteral(ts, nodeKey, RR + "template");
		if (!tmpl.empty()) {
			auto tm = std::unique_ptr<TemplateTermMap>(new TemplateTermMap(tmpl));
			std::string dt = getFirstUri(ts, nodeKey, RR + "datatype");
			if (!dt.empty()) {
				tm->datatypeIRI = std::unique_ptr<std::string>(new std::string(dt));
			}
			applyLanguage(nodeKey, *tm);
			applyExplicitTermType(nodeKey, *tm);
			return tm;
		}

		// rr:constant (URI or literal object)
		const auto *constObjs = getObjects(ts, nodeKey, RR + "constant");
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
		std::string parentUri = getFirstUri(ts, nodeKey, RR + "parentTriplesMap");
		if (!parentUri.empty()) {
			auto rom = std::unique_ptr<ConcreteReferencingObjectMap>(new ConcreteReferencingObjectMap());

			const auto *jcObjs = getObjects(ts, nodeKey, RR + "joinCondition");
			if (jcObjs) {
				for (const auto &jcObj : *jcObjs) {
					std::string jcKey = objKey(jcObj);
					if (jcKey.empty()) {
						continue;
					}
					std::string child = getFirstLiteral(ts, jcKey, RR + "child");
					std::string parent = getFirstLiteral(ts, jcKey, RR + "parent");
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
		std::string tmpl = getFirstLiteral(ts, smKey, RR + "template");
		std::string column = getFirstLiteral(ts, smKey, RR + "column");
		std::string constant = getFirstUri(ts, smKey, RR + "constant");

		if (!tmpl.empty()) {
			sm->valueMap = std::unique_ptr<TemplateTermMap>(new TemplateTermMap(tmpl));
		} else if (!column.empty()) {
			sm->valueMap = std::unique_ptr<ColumnTermMap>(new ColumnTermMap(column));
		} else if (!constant.empty()) {
			sm->valueMap = makeConstantUri(constant);
		}

		// rr:class assertions
		const auto *classObjs = getObjects(ts, smKey, RR + "class");
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
		const auto *predObjs = getObjects(ts, pomKey, RR + "predicate");
		if (predObjs) {
			for (const auto &p : *predObjs) {
				if (p.type == ObjType::URI) {
					pom->predicateMaps.push_back(makeConstantUri(p.value));
				}
			}
		}

		// rr:predicateMap (full predicate map)
		const auto *predMapObjs = getObjects(ts, pomKey, RR + "predicateMap");
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
		const auto *objObjs = getObjects(ts, pomKey, RR + "object");
		if (objObjs) {
			for (const auto &o : *objObjs) {
				if (o.type == ObjType::URI) {
					pom->objectMaps.push_back(makeConstantUri(o.value));
				}
			}
		}

		// rr:objectMap (full object map)
		const auto *objMapObjs = getObjects(ts, pomKey, RR + "objectMap");
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
					if (dynamic_cast<ColumnTermMap *>(tm.get()) && getFirstUri(ts, omKey, RR + "termType").empty()) {
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

		bool isTriplesMap = preds.count(RR + "logicalTable") || preds.count(RR + "subjectMap") ||
		                    preds.count(RR + "predicateObjectMap") || preds.count(RR + "subject");
		if (!isTriplesMap) {
			continue;
		}

		auto tm = std::unique_ptr<TriplesMap>(new TriplesMap());
		tm->id = subj;

		// Logical table (inline blank node or named resource)
		std::string ltKey = getFirstObjKey(triples, subj, RR + "logicalTable");
		if (!ltKey.empty()) {
			tm->logicalTable = ctx.buildLogicalTable(ltKey);
		}

		// Subject map
		std::string smKey = getFirstObjKey(triples, subj, RR + "subjectMap");
		if (!smKey.empty()) {
			tm->subjectMap = ctx.buildSubjectMap(smKey);
		}

		// Predicate-object maps (there may be several)
		const auto *pomObjs = getObjects(triples, subj, RR + "predicateObjectMap");
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
	ParseState state;
	state.env = serd_env_new(nullptr);

	SerdReader *reader =
	    serd_reader_new(SERD_TURTLE, &state, nullptr, cbBase, cbPrefix, cbStatement, /*end_sink=*/nullptr);
	serd_reader_set_error_sink(reader, cbError, nullptr);

	// Convert the filesystem path to a file URI and use it as the document base.
	SerdNode fileUriNode = serd_node_new_file_uri(reinterpret_cast<const uint8_t *>(mappingFilePath.c_str()),
	                                              /*hostname=*/nullptr, /*out=*/nullptr, /*escape=*/true);

	std::vector<std::string> preErrors;

	if (fileUriNode.buf) {
		serd_env_set_base_uri(state.env, &fileUriNode);
		serd_reader_read_file(reader, fileUriNode.buf);
		serd_node_free(&fileUriNode);
	} else {
		preErrors.push_back("R2RML parser: could not build file URI for: " + mappingFilePath);
	}

	serd_reader_free(reader);

	SerdEnv *env = state.env; // transfer ownership out of `state`
	state.env = nullptr;

	return buildMappingFromTriples(state.triples, env, std::move(preErrors), ignoreNonFatalErrors);
}

R2RMLMapping R2RMLParser::parseString(const std::string &turtleText, const std::string &baseUri,
                                      bool ignoreNonFatalErrors) {
	// -----------------------------------------------------------------------
	// Phase 1 – collect all triples via Serd, reading from an in-memory string
	// -----------------------------------------------------------------------
	ParseState state;
	state.env = serd_env_new(nullptr);

	SerdReader *reader =
	    serd_reader_new(SERD_TURTLE, &state, nullptr, cbBase, cbPrefix, cbStatement, /*end_sink=*/nullptr);
	serd_reader_set_error_sink(reader, cbError, nullptr);

	SerdNode baseNode = serd_node_from_string(SERD_URI, reinterpret_cast<const uint8_t *>(baseUri.c_str()));
	serd_env_set_base_uri(state.env, &baseNode);

	serd_reader_read_string(reader, reinterpret_cast<const uint8_t *>(turtleText.c_str()));

	serd_reader_free(reader);

	SerdEnv *env = state.env; // transfer ownership out of `state`
	state.env = nullptr;

	return buildMappingFromTriples(state.triples, env, {}, ignoreNonFatalErrors);
}

} // namespace r2rml
