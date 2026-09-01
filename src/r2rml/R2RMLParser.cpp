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
#include "r2rml/SerdTerm.h"
#include "r2rml/TripleStore.h"

#include <serd/serd.h>

#include <cstdarg>
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

namespace r2rml {

std::string toAbsoluteFilePath(const std::string &path) {
	if (path.empty()) {
		return path;
	}

#ifdef _WIN32
	bool isAbsolute = path[0] == '\\' || path[0] == '/' || (path.size() >= 2 && path[1] == ':');
	const char separator = '\\';
#else
	bool isAbsolute = path[0] == '/';
	const char separator = '/';
#endif
	if (isAbsolute) {
		return path;
	}

	char buffer[4096];
#ifdef _WIN32
	char *cwd = _getcwd(buffer, sizeof(buffer));
#else
	char *cwd = getcwd(buffer, sizeof(buffer));
#endif
	if (!cwd) {
		// Can't determine the working directory; return as-is so the caller's
		// existing (broken) behaviour is unchanged rather than masked.
		return path;
	}

	std::string absolutePath(cwd);
	if (!absolutePath.empty() && absolutePath.back() != separator) {
		absolutePath += separator;
	}
	absolutePath += path;
	return absolutePath;
}

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
using vocab::RR_DEFAULT_GRAPH;
using vocab::RR_GRAPH;
using vocab::RR_GRAPH_MAP;
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
// Node-expansion helper
// ---------------------------------------------------------------------------

/// Convert a SerdNode to an absolute URI string (or "_:<id>" for blank nodes,
/// tagged with `blankScopePrefix` so that blank-node labels from different
/// merged source files - which commonly reuse the same labels, e.g. "_:b0" -
/// can't collide; see TripleCollector::beginSource()).
/// CURIEs are expanded using `env`; relative URIs are resolved against the base.
/// Returns an empty string if the node cannot be represented.
static std::string expandNode(SerdEnv *env, const SerdNode *node, const std::string &blankScopePrefix = "") {
	if (!node || node->type == SERD_NOTHING) {
		return {};
	}

	if (node->type == SERD_BLANK) {
		return std::string("_:") + blankScopePrefix +
		       std::string(reinterpret_cast<const char *>(node->buf), node->n_bytes);
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

/// expandNode() for an IRI already held as text rather than as a SerdNode.
/// Handles relative references (e.g. "#TriplesMap1") by resolving them against
/// the environment's base, exactly as the Turtle path does.
static std::string expandIriText(SerdEnv *env, const std::string &text) {
	SerdNode node = serd_node_from_string(SERD_URI, reinterpret_cast<const uint8_t *>(text.c_str()));
	return expandNode(env, &node);
}

/// Subject/predicate lookup key for a term supplied by a caller: blank labels
/// get the "_:" prefix and the merge-scope tag, IRIs get resolved. A literal
/// has no key, since it cannot be a subject or a predicate.
static std::string termKey(SerdEnv *env, const rdf::Term &term, const std::string &blankScopePrefix) {
	if (term.isBlankNode()) {
		return "_:" + blankScopePrefix + term.lexical();
	}
	if (term.isIri()) {
		return expandIriText(env, term.lexical());
	}
	return {};
}

/// Apply the same resolution to an object term: scope-tag a blank label,
/// resolve an IRI, and resolve a typed literal's datatype IRI. A plain or
/// language-tagged literal passes through unchanged.
static rdf::Term resolveObjectTerm(SerdEnv *env, const rdf::Term &object, const std::string &blankScopePrefix) {
	if (object.isBlankNode()) {
		return rdf::Term::blankNode(blankScopePrefix + object.lexical());
	}
	if (object.isIri()) {
		return rdf::Term::iri(expandIriText(env, object.lexical()));
	}
	if (object.isLiteral() && !object.hasLang() && !object.datatypeIri().empty()) {
		return rdf::Term::typedLiteral(object.lexical(), expandIriText(env, object.datatypeIri()));
	}
	return object;
}

// ---------------------------------------------------------------------------
// TripleCollector – gathers statements (from Serd or built directly by a
// caller) into a TripleStore, independent of their origin.
// ---------------------------------------------------------------------------

struct TripleCollector::Impl {
	SerdEnv *env {serd_env_new(nullptr)};
	TripleStore triples;
	std::vector<std::string> errors;

	// Multi-source merge bookkeeping (see TripleCollector::beginSource()).
	// A collector fed by a single source (the common case: single-file
	// parse()/parseString(), which never call beginSource()) simply never
	// populates these beyond their defaults, so behaviour there is unchanged.
	std::vector<std::string> sourceLabels;
	int currentScope {0};
	std::map<std::string, int> subjectOwner;
	std::set<std::string> warnedConflicts;
	std::vector<std::string> warnings;

	std::string blankScopePrefix() const {
		if (sourceLabels.empty()) {
			return {};
		}
		return "s" + std::to_string(currentScope) + "_";
	}

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
	const std::string blankScopePrefix = impl_->blankScopePrefix();
	std::string subjKey = expandNode(impl_->env, subject, blankScopePrefix);
	std::string predKey = expandNode(impl_->env, predicate);
	if (subjKey.empty() || predKey.empty()) {
		return;
	}

	// termFromSerdNode does the deep copy, but not the two things that need the
	// environment or this collector's state: expanding a CURIE/relative IRI
	// against the base, and tagging a blank label with the current merge scope.
	rdf::Term obj;
	if (object->type == SERD_BLANK) {
		obj = rdf::Term::blankNode(blankScopePrefix +
		                           std::string(reinterpret_cast<const char *>(object->buf), object->n_bytes));
	} else if (object->type == SERD_LITERAL) {
		const std::string text(reinterpret_cast<const char *>(object->buf), object->n_bytes);
		// A datatype IRI may itself be a CURIE, so it must be expanded rather
		// than copied straight across - which is why this cannot simply hand the
		// datatype node to termFromSerdNode. Language wins over datatype, per
		// RDF 1.1 and rdf::Term's mutually-exclusive invariant.
		if (objectLang && objectLang->buf) {
			obj = rdf::Term::langLiteral(
			    text, std::string(reinterpret_cast<const char *>(objectLang->buf), objectLang->n_bytes));
		} else if (objectDatatype && objectDatatype->buf) {
			obj = rdf::Term::typedLiteral(text, expandNode(impl_->env, objectDatatype));
		} else {
			obj = rdf::Term::literal(text);
		}
	} else {
		obj = rdf::Term::iri(expandNode(impl_->env, object));
	}

	insertResolved(subject->type == SERD_BLANK, subjKey, predKey, std::move(obj));
}

void TripleCollector::statement(const rdf::Term &subject, const rdf::Term &predicate, const rdf::Term &object) {
	const std::string blankScopePrefix = impl_->blankScopePrefix();
	const std::string subjKey = termKey(impl_->env, subject, blankScopePrefix);
	// No scope prefix for the predicate: a predicate is always an IRI, and the
	// SerdNode overload likewise expands it without one.
	const std::string predKey = termKey(impl_->env, predicate, std::string());
	if (subjKey.empty() || predKey.empty()) {
		return;
	}
	insertResolved(subject.isBlankNode(), subjKey, predKey, resolveObjectTerm(impl_->env, object, blankScopePrefix));
}

void TripleCollector::insertResolved(bool subject_is_blank, const std::string &subject_key,
                                     const std::string &predicate_key, rdf::Term object) {
	// Named-subject conflict detection across merged sources (see
	// beginSource()): blank subjects are always scope-tagged uniquely by the
	// caller, so they never reach this check.
	if (!subject_is_blank) {
		auto ownerIt = impl_->subjectOwner.find(subject_key);
		if (ownerIt == impl_->subjectOwner.end()) {
			impl_->subjectOwner.emplace(subject_key, impl_->currentScope);
		} else if (ownerIt->second != impl_->currentScope) {
			std::string conflictTag = subject_key + "\x1f" + std::to_string(impl_->currentScope);
			if (impl_->warnedConflicts.insert(conflictTag).second) {
				impl_->warnings.push_back(
				    "competing definition of <" + subject_key + "> in '" + impl_->sourceLabels[impl_->currentScope] +
				    "' ignored (kept definition from '" + impl_->sourceLabels[ownerIt->second] + "')");
			}
			return;
		}
	}

	impl_->triples.insert(subject_key, predicate_key, std::move(object));
}

void TripleCollector::addError(const std::string &message) {
	impl_->errors.push_back(message);
}

void TripleCollector::beginSource(const std::string &sourceLabel) {
	impl_->sourceLabels.push_back(sourceLabel);
	impl_->currentScope = static_cast<int>(impl_->sourceLabels.size()) - 1;
}

void TripleCollector::addWarning(const std::string &message) {
	impl_->warnings.push_back(message);
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

// Turtle syntax errors (e.g. a malformed token from corrupted/mis-encoded
// input) must not vanish silently: Serd itself recovers and keeps parsing
// past them, which can silently drop whole statements - including an
// rr:termType assertion, downgrading an intended IRI to the object-map
// default of rr:Literal with no trace. Record every error via the same
// TripleCollector used for semantic errors so it surfaces through
// R2RMLMapping::parseErrors (or throws in strict mode), then keep going so a
// single bad statement doesn't prevent the rest of a mostly-valid file from
// being reported too.
static SerdStatus cbError(void *handle, const SerdError *error) {
	if (handle && error) {
		char buf[1024];
		int n = std::vsnprintf(buf, sizeof(buf), error->fmt, *error->args);
		std::ostringstream msg;
		msg << "R2RML parser: Turtle syntax error";
		if (error->line) {
			msg << " at line " << error->line << ", column " << error->col;
		}
		msg << ": " << (n > 0 ? std::string(buf) : std::string("(unknown error)"));
		static_cast<TripleCollector *>(handle)->addError(msg.str());
	}
	return SERD_SUCCESS;
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

	void generateRDFTerm(const SQLRow &row, rdf::Term &out) const override {
		if (valueMap) {
			valueMap->generateRDFTerm(row, out);
			return;
		}
		out.clear();
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
// ConcreteGraphMap – private to this translation unit.
//
// GraphMap inherits TermMap's pure-virtual generateRDFTerm without
// overriding it, making it abstract; ConcreteGraphMap adds an inner TermMap
// (built via ParseContext::buildTermMap, so it shares the same
// column/template/constant machinery as predicate/object maps) that
// supplies the value-generation strategy.
// ---------------------------------------------------------------------------
class ConcreteGraphMap : public GraphMap {
public:
	std::unique_ptr<TermMap> valueMap;

	void generateRDFTerm(const SQLRow &row, rdf::Term &out) const override {
		if (valueMap) {
			valueMap->generateRDFTerm(row, out);
			return;
		}
		out.clear();
	}

	const TermMap *valueTermMap() const override {
		return valueMap.get();
	}

	std::ostream &print(std::ostream &os) const override {
		os << "GraphMap {";
		if (valueMap) {
			os << " valueMap=" << *valueMap;
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
	void generateRDFTerm(const SQLRow & /*row*/, rdf::Term &out) const override {
		out.clear(); // use the two-row overload for actual generation
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
	// Wrap an already-parsed literal term in a ConstantTermMap (rr:constant
	// with a literal object, e.g. `rr:constant "active"` or
	// `rr:constant "5"^^xsd:integer`). `term` carries whatever datatype/
	// language the Turtle parser attached, so it is passed straight through
	// rather than being rebuilt from the bare lexical form.
	//
	// An empty lexical form is rejected the same way the SerdNode
	// constructor rejects a zero-length node: the term map is left with no
	// constant value, isValid() returns false, and the enclosing
	// predicate-object map emits nothing - preserving the long-standing
	// behaviour that rr:constant "" produces no triple at all.
	// ------------------------------------------------------------------
	static std::unique_ptr<ConstantTermMap> makeConstantLiteral(const rdf::Term &term) {
		if (term.lexical().empty()) {
			return std::unique_ptr<ConstantTermMap>(new ConstantTermMap());
		}
		auto tm = std::unique_ptr<ConstantTermMap>(new ConstantTermMap(term));
		tm->termType = TermType::Literal;
		return tm;
	}

	// ------------------------------------------------------------------
	// Read rr:termType (if present) from `nodeKey` and, when recognised,
	// override `tm`'s termType.  Called after any default term-type has
	// already been applied so an explicit rr:termType always wins.
	// ------------------------------------------------------------------
	void applyExplicitTermType(const std::string &nodeKey, TermMap &tm) {
		std::string tt = ts.getFirstUri(nodeKey, RR_TERM_TYPE);
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
		std::string lang = ts.getFirstLiteral(nodeKey, RR_LANGUAGE);
		if (!lang.empty()) {
			tm.languageTag = std::unique_ptr<std::string>(new std::string(std::move(lang)));
		}
	}

	// ------------------------------------------------------------------
	// Build a LogicalTable from a blank-node or named-resource key.
	// ------------------------------------------------------------------
	std::unique_ptr<LogicalTable> buildLogicalTable(const std::string &ltKey) {
		std::string tableName = ts.getFirstLiteral(ltKey, RR_TABLE_NAME);
		if (!tableName.empty()) {
			return std::unique_ptr<BaseTableOrView>(new BaseTableOrView(tableName));
		}

		std::string sqlQuery = ts.getFirstLiteral(ltKey, RR_SQL_QUERY);
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
		std::string column = ts.getFirstLiteral(nodeKey, RR_COLUMN);
		if (!column.empty()) {
			auto tm = std::unique_ptr<ColumnTermMap>(new ColumnTermMap(column));
			std::string dt = ts.getFirstUri(nodeKey, RR_DATATYPE);
			if (!dt.empty()) {
				tm->datatypeIRI = std::unique_ptr<std::string>(new std::string(dt));
			}
			applyLanguage(nodeKey, *tm);
			applyExplicitTermType(nodeKey, *tm);
			return tm;
		}

		// rr:template
		std::string tmpl = ts.getFirstLiteral(nodeKey, RR_TEMPLATE);
		if (!tmpl.empty()) {
			auto tm = std::unique_ptr<TemplateTermMap>(new TemplateTermMap(tmpl));
			std::string dt = ts.getFirstUri(nodeKey, RR_DATATYPE);
			if (!dt.empty()) {
				tm->datatypeIRI = std::unique_ptr<std::string>(new std::string(dt));
			}
			applyLanguage(nodeKey, *tm);
			applyExplicitTermType(nodeKey, *tm);
			return tm;
		}

		// rr:constant (URI or literal object)
		const auto *constObjs = ts.getObjects(nodeKey, RR_CONSTANT);
		if (constObjs) {
			for (const auto &c : *constObjs) {
				if (c.isIri()) {
					return makeConstantUri(c.lexical());
				}
			}
			for (const auto &c : *constObjs) {
				if (c.isLiteral()) {
					return makeConstantLiteral(c);
				}
			}
		}

		// rr:parentTriplesMap → ReferencingObjectMap
		std::string parentUri = ts.getFirstUri(nodeKey, RR_PARENTTRIPLESMAP);
		if (!parentUri.empty()) {
			auto rom = std::unique_ptr<ConcreteReferencingObjectMap>(new ConcreteReferencingObjectMap());

			const auto *jcObjs = ts.getObjects(nodeKey, RR_JOIN_CONDITION);
			if (jcObjs) {
				for (const auto &jcObj : *jcObjs) {
					std::string jcKey = TripleStore::objKey(jcObj);
					if (jcKey.empty()) {
						continue;
					}
					std::string child = ts.getFirstLiteral(jcKey, RR_CHILD);
					std::string parent = ts.getFirstLiteral(jcKey, RR_PARENT);
					rom->joinConditions.emplace_back(child, parent);
				}
			}

			parentRefs.emplace_back(rom.get(), parentUri);
			return rom;
		}

		return nullptr; // unknown – caller may warn
	}

	// ------------------------------------------------------------------
	// Build the rr:graph / rr:graphMap annotations on `nodeKey` (a subject
	// map or predicate-object map) into a list of GraphMap instances. The
	// rr:graph shortcut takes a constant IRI directly; rr:graphMap points at
	// a full term map (rr:column/rr:template/rr:constant), built the same way
	// as any other term map.
	// ------------------------------------------------------------------
	std::vector<std::unique_ptr<GraphMap>> buildGraphMaps(const std::string &nodeKey) {
		std::vector<std::unique_ptr<GraphMap>> graphMaps;

		const auto *graphObjs = ts.getObjects(nodeKey, RR_GRAPH);
		if (graphObjs) {
			for (const auto &g : *graphObjs) {
				if (g.isIri()) {
					auto gm = std::unique_ptr<ConcreteGraphMap>(new ConcreteGraphMap());
					gm->valueMap = makeConstantUri(g.lexical());
					graphMaps.push_back(std::move(gm));
				}
			}
		}

		const auto *graphMapObjs = ts.getObjects(nodeKey, RR_GRAPH_MAP);
		if (graphMapObjs) {
			for (const auto &gmObj : *graphMapObjs) {
				std::string gmKey = TripleStore::objKey(gmObj);
				if (gmKey.empty()) {
					continue;
				}
				auto tm = buildTermMap(gmKey);
				if (tm) {
					auto gm = std::unique_ptr<ConcreteGraphMap>(new ConcreteGraphMap());
					gm->valueMap = std::move(tm);
					graphMaps.push_back(std::move(gm));
				} else {
					errors.push_back("R2RML parser: unknown graph map type for <" + gmKey + ">");
				}
			}
		}

		return graphMaps;
	}

	// ------------------------------------------------------------------
	// Build a SubjectMap from a node key.
	// ------------------------------------------------------------------
	std::unique_ptr<SubjectMap> buildSubjectMap(const std::string &smKey) {
		auto sm = std::unique_ptr<ConcreteSubjectMap>(new ConcreteSubjectMap());

		// Value-generation strategy
		std::string tmpl = ts.getFirstLiteral(smKey, RR_TEMPLATE);
		std::string column = ts.getFirstLiteral(smKey, RR_COLUMN);
		std::string constant = ts.getFirstUri(smKey, RR_CONSTANT);

		if (!tmpl.empty()) {
			sm->valueMap = std::unique_ptr<TemplateTermMap>(new TemplateTermMap(tmpl));
		} else if (!column.empty()) {
			sm->valueMap = std::unique_ptr<ColumnTermMap>(new ColumnTermMap(column));
		} else if (!constant.empty()) {
			sm->valueMap = makeConstantUri(constant);
		}

		// R2RML 7.4: a subject map's term type may only be rr:IRI (the default,
		// already set by TermMap's initializer) or rr:BlankNode - a subject is
		// never a literal. Read it here rather than relying on buildTermMap,
		// which this branch deliberately does not go through.
		if (sm->valueMap) {
			applyExplicitTermType(smKey, *sm->valueMap);
			if (sm->valueMap->termType == TermType::Literal) {
				errors.push_back("R2RML parser: rr:termType rr:Literal is not allowed on a subject map <" + smKey +
				                 ">; using rr:IRI");
				sm->valueMap->termType = TermType::IRI;
			}
		}

		// rr:class assertions
		const auto *classObjs = ts.getObjects(smKey, RR_CLASS);
		if (classObjs) {
			for (const auto &cls : *classObjs) {
				if (cls.isIri()) {
					sm->classIRIs.push_back(cls.lexical());
				}
			}
		}

		sm->graphMaps = buildGraphMaps(smKey);

		return sm;
	}

	// ------------------------------------------------------------------
	// Build a PredicateObjectMap from a blank-node key.
	// ------------------------------------------------------------------
	std::unique_ptr<PredicateObjectMap> buildPOM(const std::string &pomKey) {
		auto pom = std::unique_ptr<PredicateObjectMap>(new PredicateObjectMap());

		// rr:predicate shortcut (constant predicate)
		const auto *predObjs = ts.getObjects(pomKey, RR_PREDICATE);
		if (predObjs) {
			for (const auto &p : *predObjs) {
				if (p.isIri()) {
					pom->predicateMaps.push_back(makeConstantUri(p.lexical()));
				}
			}
		}

		// rr:predicateMap (full predicate map)
		const auto *predMapObjs = ts.getObjects(pomKey, RR_PREDICATE_MAP);
		if (predMapObjs) {
			for (const auto &pm : *predMapObjs) {
				std::string pmKey = TripleStore::objKey(pm);
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
		const auto *objObjs = ts.getObjects(pomKey, RR_OBJECT);
		if (objObjs) {
			for (const auto &o : *objObjs) {
				if (o.isIri()) {
					pom->objectMaps.push_back(makeConstantUri(o.lexical()));
				}
			}
		}

		// rr:objectMap (full object map)
		const auto *objMapObjs = ts.getObjects(pomKey, RR_OBJECT_MAP);
		if (objMapObjs) {
			for (const auto &om : *objMapObjs) {
				std::string omKey = TripleStore::objKey(om);
				if (omKey.empty()) {
					continue;
				}
				auto tm = buildTermMap(omKey);
				if (tm) {
					// Per R2RML spec: default term type for rr:column in an
					// objectMap is rr:Literal (not rr:IRI), unless an explicit
					// rr:termType was given on the object map (already applied
					// by buildTermMap()), which always wins.
					if (dynamic_cast<ColumnTermMap *>(tm.get()) && ts.getFirstUri(omKey, RR_TERM_TYPE).empty()) {
						tm->termType = TermType::Literal;
					}
					pom->objectMaps.push_back(std::move(tm));
				} else {
					errors.push_back("R2RML parser: unknown object map type for <" + omKey + ">");
				}
			}
		}

		pom->graphMaps = buildGraphMaps(pomKey);

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
		const TripleStore::PredMap &preds = entry.second;

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
		std::string ltKey = triples.getFirstObjKey(subj, RR_LOGICAL_TABLE);
		if (!ltKey.empty()) {
			tm->logicalTable = ctx.buildLogicalTable(ltKey);
		}

		// Subject map
		std::string smKey = triples.getFirstObjKey(subj, RR_SUBJECT_MAP);
		if (!smKey.empty()) {
			tm->subjectMap = ctx.buildSubjectMap(smKey);
		}

		// Predicate-object maps (there may be several)
		const auto *pomObjs = triples.getObjects(subj, RR_PREDICATE_OBJECT_MAP);
		if (pomObjs) {
			for (const auto &pomObj : *pomObjs) {
				std::string pomKey = TripleStore::objKey(pomObj);
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

void R2RMLParser::collectFile(const std::string &mappingFilePath, TripleCollector &collector) {
	// -----------------------------------------------------------------------
	// Phase 1 – collect all triples via Serd
	// -----------------------------------------------------------------------
	SerdReader *reader =
	    serd_reader_new(SERD_TURTLE, &collector, nullptr, cbBase, cbPrefix, cbStatement, /*end_sink=*/nullptr);
	serd_reader_set_error_sink(reader, cbError, &collector);

	// Convert the filesystem path to a file URI and use it as the document base.
	// serd_node_new_file_uri only recognises absolute paths; resolve relative
	// paths against the current working directory first so the base URI it
	// produces always has a "file://" scheme.
	std::string absoluteMappingFilePath = toAbsoluteFilePath(mappingFilePath);
	SerdNode fileUriNode = serd_node_new_file_uri(reinterpret_cast<const uint8_t *>(absoluteMappingFilePath.c_str()),
	                                              /*hostname=*/nullptr, /*out=*/nullptr, /*escape=*/true);

	if (fileUriNode.buf) {
		collector.setBase(&fileUriNode);
		serd_reader_read_file(reader, fileUriNode.buf);
		serd_node_free(&fileUriNode);
	} else {
		collector.addError("R2RML parser: could not build file URI for: " + mappingFilePath);
	}

	serd_reader_free(reader);
}

R2RMLMapping R2RMLParser::parse(const std::string &mappingFilePath, bool ignoreNonFatalErrors) {
	TripleCollector collector;
	collectFile(mappingFilePath, collector);
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
	serd_reader_set_error_sink(reader, cbError, &collector);

	SerdNode baseNode = serd_node_from_string(SERD_URI, reinterpret_cast<const uint8_t *>(baseUri.c_str()));
	collector.setBase(&baseNode);

	serd_reader_read_string(reader, reinterpret_cast<const uint8_t *>(turtleText.c_str()));

	serd_reader_free(reader);

	return parseCollected(collector, ignoreNonFatalErrors);
}

R2RMLMapping R2RMLParser::parseCollected(TripleCollector &collector, bool ignoreNonFatalErrors) {
	SerdEnv *env = collector.impl_->env; // transfer ownership out of the collector
	collector.impl_->env = nullptr;

	R2RMLMapping mapping = buildMappingFromTriples(collector.impl_->triples, env, std::move(collector.impl_->errors),
	                                               ignoreNonFatalErrors);
	mapping.mergeWarnings = std::move(collector.impl_->warnings);
	return mapping;
}

} // namespace r2rml
