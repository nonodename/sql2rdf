// YARRRML → R2RML translator.
//
// Strategy: translate the supported YARRRML subset (see YARRRMLParser.h)
// straight into SerdNode-based RDF statements, fed to an r2rml::TripleCollector
// -- the same statement-insertion logic r2rml::R2RMLParser's own Turtle-parsing
// paths use -- and then hand that collector to
// r2rml::R2RMLParser::parseCollected() to build the actual object model. This
// avoids serialising to Turtle text and re-parsing it (and the string-escaping
// edge cases that come with it) while keeping all term-map/engine semantics
// (template expansion, datatypes, joins, ...) identical between R2RML and
// YARRRML mappings, since YARRRML mappings are ultimately executed by the very
// same R2RML engine.
//
// Non-fatal problems encountered while translating (unsupported keys, a
// mapping with no source, an unresolved join-condition function, ...) are
// recorded via TripleCollector::addError() rather than raised immediately;
// R2RMLParser::parseCollected() decides whether to merge them into the
// resulting R2RMLMapping::parseErrors or to throw, mirroring
// R2RMLParser::parse()'s ignoreNonFatalErrors convention.

#include "yarrrml/YARRRMLParser.h"

#include "r2rml/R2RMLMapping.h"
#include "r2rml/R2RMLParser.h"

#include <serd/serd.h>
#include <yaml-cpp/yaml.h>

#include <fstream>
#include <initializer_list>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace yarrrml {

// ---------------------------------------------------------------------------
// R2RML namespace prefix (shared with R2RMLParser.cpp; see vocab in
// MappingParser.h)
// ---------------------------------------------------------------------------
using r2rml::vocab::RR_BLANKNODE_TERM_TYPE;
using r2rml::vocab::RR_CHILD;
using r2rml::vocab::RR_CLASS;
using r2rml::vocab::RR_COLUMN;
using r2rml::vocab::RR_CONSTANT;
using r2rml::vocab::RR_DATATYPE;
using r2rml::vocab::RR_IRI_TERM_TYPE;
using r2rml::vocab::RR_JOIN_CONDITION;
using r2rml::vocab::RR_LANGUAGE;
using r2rml::vocab::RR_LITERAL_TERM_TYPE;
using r2rml::vocab::RR_LOGICAL_TABLE;
using r2rml::vocab::RR_OBJECT;
using r2rml::vocab::RR_OBJECT_MAP;
using r2rml::vocab::RR_PARENT;
using r2rml::vocab::RR_PARENTTRIPLESMAP;
using r2rml::vocab::RR_PREDICATE;
using r2rml::vocab::RR_PREDICATE_MAP;
using r2rml::vocab::RR_PREDICATE_OBJECT_MAP;
using r2rml::vocab::RR_SQL_QUERY;
using r2rml::vocab::RR_SUBJECT;
using r2rml::vocab::RR_SUBJECT_MAP;
using r2rml::vocab::RR_TABLE_NAME;
using r2rml::vocab::RR_TEMPLATE;
using r2rml::vocab::RR_TERM_TYPE;

// YARRRML-only: not part of the shared vocab since R2RMLParser.cpp has no
// use for them (it reads rr:triplesMap/rdf:type from Turtle text via Serd,
// not by emitting these as literal predicate strings).
static const char *const RR_TRIPLES_MAP = "http://www.w3.org/ns/r2rml#triplesMap";
static const char *const RDF_TYPE = "http://www.w3.org/1999/02/22-rdf-syntax-ns#type";

namespace {

// ---------------------------------------------------------------------------
// Generic YAML helpers
// ---------------------------------------------------------------------------

/// Return the first present key's value out of `keys`, or an undefined Node.
///
/// Note: YAML::Node's default constructor produces a *Null*-typed node, which
/// reports IsDefined()==true (it is "present, and empty" – not "absent").
/// Callers rely on `if (firstOf(...))` being false when nothing matched, so
/// the "not found" fallback must be a genuinely Undefined node instead.
YAML::Node firstOf(const YAML::Node &node, std::initializer_list<const char *> keys) {
	static const YAML::Node kUndefined(YAML::NodeType::Undefined);
	if (!node || !node.IsMap()) {
		return kUndefined;
	}
	for (const char *k : keys) {
		YAML::Node v = node[k];
		if (v) {
			return v;
		}
	}
	return kUndefined;
}

/// Normalise a scalar-or-sequence node into a list of its elements (a single
/// scalar/map becomes a one-element list).
std::vector<YAML::Node> flattenList(const YAML::Node &node) {
	std::vector<YAML::Node> out;
	if (!node) {
		return out;
	}
	if (node.IsSequence()) {
		for (const YAML::Node &n : node) {
			out.push_back(n);
		}
	} else {
		out.push_back(node);
	}
	return out;
}

/// Like flattenList(), but only keeps scalar elements converted to strings.
std::vector<std::string> flattenScalarList(const YAML::Node &node) {
	std::vector<std::string> out;
	for (const YAML::Node &n : flattenList(node)) {
		if (n.IsScalar()) {
			out.push_back(n.as<std::string>());
		}
	}
	return out;
}

// ---------------------------------------------------------------------------
// IRI / CURIE recognition
// ---------------------------------------------------------------------------

bool looksAbsoluteIri(const std::string &v) {
	if (v.compare(0, 4, "urn:") == 0) {
		return true;
	}
	std::size_t p = v.find("://");
	return p != std::string::npos && p > 0;
}

bool looksCurie(const std::string &v, const std::map<std::string, std::string> &prefixes) {
	std::size_t colon = v.find(':');
	if (colon == std::string::npos) {
		return false;
	}
	if (v.find("://") != std::string::npos) {
		return false;
	}
	return prefixes.count(v.substr(0, colon)) > 0;
}

/// Resolve `v` to the IRI text a SerdNode should carry: a recognised CURIE is
/// expanded to its full namespace IRI; anything else (already absolute, or a
/// relative reference such as "#TriplesMap1") is returned unchanged and left
/// for TripleCollector to resolve against the document base, exactly as an
/// angle-bracketed Turtle IRI would have been.
std::string resolveIri(const std::string &v, const std::map<std::string, std::string> &prefixes) {
	if (looksAbsoluteIri(v)) {
		return v;
	}
	if (looksCurie(v, prefixes)) {
		std::size_t colon = v.find(':');
		return prefixes.at(v.substr(0, colon)) + v.substr(colon + 1);
	}
	return v;
}

/// If `text` begins with "prefix:" for a known prefix (and isn't already an
/// absolute "scheme://..." IRI), replace the prefix with its full namespace
/// IRI. Needed for rr:template literal text: unlike a bare CURIE term, text
/// embedded in an rr:template value is never resolved by anything downstream,
/// so it must be expanded here.
std::string expandLeadingCurie(const std::string &text, const std::map<std::string, std::string> &prefixes) {
	std::size_t colon = text.find(':');
	if (colon == std::string::npos) {
		return text;
	}
	if (text.compare(colon + 1, 2, "//") == 0) {
		return text; // absolute IRI (scheme://...), not a CURIE
	}
	auto it = prefixes.find(text.substr(0, colon));
	if (it == prefixes.end()) {
		return text;
	}
	return it->second + text.substr(colon + 1);
}

// ---------------------------------------------------------------------------
// YARRRML value-string tokenisation:  "text $(COL) more text" → literal /
// column-reference tokens, honouring the "\$(" escape for a literal "$(".
// ---------------------------------------------------------------------------

struct Token {
	bool isColumn;
	std::string text;
};

std::vector<Token> tokenizeValue(const std::string &raw) {
	std::vector<Token> toks;
	std::string lit;
	std::size_t i = 0;
	const std::size_t n = raw.size();
	while (i < n) {
		if (raw[i] == '\\' && i + 2 < n && raw[i + 1] == '$' && raw[i + 2] == '(') {
			lit += "$(";
			i += 3;
			continue;
		}
		if (raw[i] == '$' && i + 1 < n && raw[i + 1] == '(') {
			std::size_t end = raw.find(')', i + 2);
			if (end != std::string::npos) {
				if (!lit.empty()) {
					toks.push_back(Token {false, lit});
					lit.clear();
				}
				toks.push_back(Token {true, raw.substr(i + 2, end - (i + 2))});
				i = end + 1;
				continue;
			}
		}
		lit += raw[i];
		++i;
	}
	if (!lit.empty()) {
		toks.push_back(Token {false, lit});
	}
	return toks;
}

/// Escape braces that appear literally (not as part of a $(...) reference) so
/// they cannot be mistaken for rr:template placeholders.
std::string escapeTemplateLiteral(const std::string &lit) {
	std::string out;
	out.reserve(lit.size());
	for (char c : lit) {
		if (c == '{' || c == '}') {
			out += '\\';
		}
		out += c;
	}
	return out;
}

enum class VKind { Column, Template, ConstIri, ConstLit };

struct VSpec {
	VKind kind {VKind::ConstLit};
	std::string text;
	bool forceIri {false}; // Column only: "$(COL)~iri" was given.
};

/// Classify a raw YARRRML value string into a Column / Template / constant
/// term.  `allowIriSuffix` enables stripping a trailing "~iri" (objects
/// only).  `literalsAllowed` controls whether a plain non-IRI-looking string
/// becomes a literal constant (objects) or is always treated as an IRI
/// (subjects/predicates, which cannot be literals).
VSpec classifyValue(const std::string &rawIn, bool allowIriSuffix, bool literalsAllowed,
                    const std::map<std::string, std::string> &prefixes) {
	std::string raw = rawIn;
	bool forceIri = false;
	if (allowIriSuffix) {
		static const std::string suf = "~iri";
		if (raw.size() > suf.size() && raw.compare(raw.size() - suf.size(), suf.size(), suf) == 0) {
			raw.resize(raw.size() - suf.size());
			forceIri = true;
		}
	}

	std::vector<Token> toks = tokenizeValue(raw);

	if (toks.size() == 1 && toks[0].isColumn) {
		VSpec vs;
		vs.kind = VKind::Column;
		vs.text = toks[0].text;
		vs.forceIri = forceIri;
		return vs;
	}

	bool hasColumn = false;
	for (const Token &t : toks) {
		if (t.isColumn) {
			hasColumn = true;
			break;
		}
	}

	if (hasColumn) {
		std::string tmpl;
		bool isFirstToken = true;
		for (const Token &t : toks) {
			if (t.isColumn) {
				tmpl += '{';
				tmpl += t.text;
				tmpl += '}';
			} else {
				std::string litText = isFirstToken ? expandLeadingCurie(t.text, prefixes) : t.text;
				tmpl += escapeTemplateLiteral(litText);
			}
			isFirstToken = false;
		}
		VSpec vs;
		vs.kind = VKind::Template;
		vs.text = tmpl;
		return vs;
	}

	std::string literalText;
	for (const Token &t : toks) {
		literalText += t.text;
	}

	VSpec vs;
	vs.text = literalText;
	if (!literalsAllowed) {
		vs.kind = VKind::ConstIri; // subjects/predicates: always an IRI.
		return vs;
	}
	vs.kind = (looksAbsoluteIri(literalText) || looksCurie(literalText, prefixes)) ? VKind::ConstIri : VKind::ConstLit;
	return vs;
}

/// Translate a po shortcut's third array element ("xsd:integer" or "en~lang")
/// into a datatype IRI or language tag.
struct ValueExtra {
	std::string datatypeIri;
	std::string language;
};

ValueExtra buildDatatypeOrLangExtra(const std::string &raw, const std::map<std::string, std::string> &prefixes) {
	static const std::string suf = "~lang";
	ValueExtra extra;
	if (raw.size() > suf.size() && raw.compare(raw.size() - suf.size(), suf.size(), suf) == 0) {
		extra.language = raw.substr(0, raw.size() - suf.size());
	} else {
		extra.datatypeIri = resolveIri(raw, prefixes);
	}
	return extra;
}

/// Extract the column name from a join-condition parameter pair such as
/// ["str1", "$(EMPNO)"], returning "" if it is not a lone $(...) reference.
std::string extractColumnRef(const YAML::Node &param) {
	if (!param || !param.IsSequence() || param.size() < 2 || !param[1].IsScalar()) {
		return "";
	}
	std::string ref = param[1].as<std::string>();
	static const std::string prefix = "$(";
	if (ref.size() > prefix.size() + 1 && ref.compare(0, prefix.size(), prefix) == 0 && ref.back() == ')') {
		return ref.substr(prefix.size(), ref.size() - prefix.size() - 1);
	}
	return "";
}

// ---------------------------------------------------------------------------
// SerdNode construction & direct statement emission
//
// Rather than serialising R2RML as Turtle text and re-parsing it, mappings
// are translated straight into SerdNode-based statements fed to a
// TripleCollector -- the same statement-insertion logic R2RMLParser's own
// Turtle-parsing paths use.
// ---------------------------------------------------------------------------

/// A reference to a node already known to a translation step: either a named
/// IRI (possibly relative, e.g. "#TriplesMap1") or a blank node minted via
/// BlankNodeMinter. `valid == false` denotes "no node" (nothing to link).
struct NodeRef {
	bool valid;
	bool isBlank;
	std::string text;

	NodeRef() : valid(false), isBlank(false) {
	}
	NodeRef(bool isBlankIn, std::string textIn) : valid(true), isBlank(isBlankIn), text(std::move(textIn)) {
	}

	static NodeRef uri(std::string v) {
		return NodeRef(false, std::move(v));
	}
	static NodeRef blank(std::string id) {
		return NodeRef(true, std::move(id));
	}
};

SerdNode toSerdNode(const NodeRef &n) {
	const auto *buf = reinterpret_cast<const uint8_t *>(n.text.c_str());
	return n.isBlank ? serd_node_from_string(SERD_BLANK, buf) : serd_node_from_string(SERD_URI, buf);
}

/// Mints sequential blank-node identifiers ("b0", "b1", ...) for one YARRRML
/// document -- replaces the anonymous node IDs Serd used to generate
/// implicitly for "[ ... ]" Turtle syntax.
class BlankNodeMinter {
public:
	NodeRef next() {
		return NodeRef::blank("b" + std::to_string(counter_++));
	}

private:
	unsigned counter_ {0};
};

void emitUriTriple(r2rml::TripleCollector &collector, const NodeRef &subject, const std::string &predicateIri,
                   const NodeRef &object) {
	SerdNode s = toSerdNode(subject);
	SerdNode p = serd_node_from_string(SERD_URI, reinterpret_cast<const uint8_t *>(predicateIri.c_str()));
	SerdNode o = toSerdNode(object);
	collector.statement(&s, &p, &o);
}

void emitLiteralTriple(r2rml::TripleCollector &collector, const NodeRef &subject, const std::string &predicateIri,
                       const std::string &literalText, const std::string &datatypeIri = std::string(),
                       const std::string &language = std::string()) {
	SerdNode s = toSerdNode(subject);
	SerdNode p = serd_node_from_string(SERD_URI, reinterpret_cast<const uint8_t *>(predicateIri.c_str()));
	SerdNode o = serd_node_from_string(SERD_LITERAL, reinterpret_cast<const uint8_t *>(literalText.c_str()));

	SerdNode dt = SERD_NODE_NULL;
	SerdNode lang = SERD_NODE_NULL;
	if (!datatypeIri.empty()) {
		dt = serd_node_from_string(SERD_URI, reinterpret_cast<const uint8_t *>(datatypeIri.c_str()));
	}
	if (!language.empty()) {
		lang = serd_node_from_string(SERD_LITERAL, reinterpret_cast<const uint8_t *>(language.c_str()));
	}
	collector.statement(&s, &p, &o, datatypeIri.empty() ? nullptr : &dt, language.empty() ? nullptr : &lang);
}

/// Mint a blank node and emit the rr:column / rr:template / rr:constant (+
/// optional rr:termType / rr:datatype / rr:language) statements describing
/// `vs` on it. Equivalent to what used to be a "[ rr:x ... ]" Turtle
/// fragment; the returned NodeRef is that blank node, ready to be linked in
/// as e.g. an rr:objectMap value.
NodeRef emitValueSpecAsMap(r2rml::TripleCollector &collector, BlankNodeMinter &blanks, const VSpec &vs,
                           const std::map<std::string, std::string> &prefixes, const ValueExtra &extra) {
	NodeRef node = blanks.next();
	switch (vs.kind) {
	case VKind::Column:
		emitLiteralTriple(collector, node, RR_COLUMN, vs.text);
		if (vs.forceIri) {
			emitUriTriple(collector, node, RR_TERM_TYPE, NodeRef::uri(RR_IRI_TERM_TYPE));
		}
		break;
	case VKind::Template:
		emitLiteralTriple(collector, node, RR_TEMPLATE, vs.text);
		break;
	case VKind::ConstIri:
		// Per R2RML, rr:constant with an IRI object carries no datatype/language.
		emitUriTriple(collector, node, RR_CONSTANT, NodeRef::uri(resolveIri(vs.text, prefixes)));
		return node;
	case VKind::ConstLit:
	default:
		emitLiteralTriple(collector, node, RR_CONSTANT, vs.text);
		break;
	}
	if (!extra.datatypeIri.empty()) {
		emitUriTriple(collector, node, RR_DATATYPE, NodeRef::uri(extra.datatypeIri));
	}
	if (!extra.language.empty()) {
		emitLiteralTriple(collector, node, RR_LANGUAGE, extra.language);
	}
	return node;
}

/// Translate a predicate value ("a", a CURIE/IRI, or a $(...) template) into
/// either a constant predicate IRI (rr:predicate) or a freshly minted
/// rr:predicateMap blank node.
struct PredicateResult {
	bool isConstant;
	std::string constantIri;
	NodeRef mapNode;
};

PredicateResult buildPredicateResult(r2rml::TripleCollector &collector, BlankNodeMinter &blanks, const std::string &raw,
                                     const std::map<std::string, std::string> &prefixes) {
	if (raw == "a") {
		return PredicateResult {true, RDF_TYPE, NodeRef()};
	}
	VSpec vs = classifyValue(raw, /*allowIriSuffix=*/false, /*literalsAllowed=*/false, prefixes);
	switch (vs.kind) {
	case VKind::Template: {
		NodeRef node = blanks.next();
		emitLiteralTriple(collector, node, RR_TEMPLATE, vs.text);
		return PredicateResult {false, "", node};
	}
	case VKind::Column: {
		NodeRef node = blanks.next();
		emitLiteralTriple(collector, node, RR_COLUMN, vs.text);
		return PredicateResult {false, "", node};
	}
	case VKind::ConstIri:
	default:
		return PredicateResult {true, resolveIri(vs.text, prefixes), NodeRef()};
	}
}

/// Translate one object-list entry (a plain scalar, a [value, dtOrLang]
/// array, a {value:/v:, datatype:/language:} map, or a {mapping: ...,
/// condition(s): ...} mapping reference) into a freshly minted rr:objectMap
/// blank node. Returns an invalid NodeRef (and records a warning) on failure.
NodeRef emitObjectNode(r2rml::TripleCollector &collector, BlankNodeMinter &blanks, const YAML::Node &objNode,
                       const std::map<std::string, std::string> &prefixes, const ValueExtra &extraIn,
                       const std::string &mappingName) {
	if (objNode.IsScalar()) {
		VSpec vs =
		    classifyValue(objNode.as<std::string>(), /*allowIriSuffix=*/true, /*literalsAllowed=*/true, prefixes);
		return emitValueSpecAsMap(collector, blanks, vs, prefixes, extraIn);
	}

	if (objNode.IsSequence()) {
		if (objNode.size() < 1 || !objNode[0].IsScalar()) {
			collector.addError("YARRRML parser: mapping '" + mappingName + "': malformed object value array, skipped");
			return NodeRef();
		}
		ValueExtra extra = extraIn;
		if (objNode.size() >= 2 && objNode[1].IsScalar()) {
			ValueExtra dtOrLang = buildDatatypeOrLangExtra(objNode[1].as<std::string>(), prefixes);
			if (!dtOrLang.datatypeIri.empty()) {
				extra.datatypeIri = dtOrLang.datatypeIri;
			}
			if (!dtOrLang.language.empty()) {
				extra.language = dtOrLang.language;
			}
		}
		VSpec vs = classifyValue(objNode[0].as<std::string>(), true, true, prefixes);
		return emitValueSpecAsMap(collector, blanks, vs, prefixes, extra);
	}

	if (objNode.IsMap()) {
		YAML::Node mappingRefNode = objNode["mapping"];
		if (mappingRefNode && mappingRefNode.IsScalar()) {
			NodeRef node = blanks.next();
			emitUriTriple(collector, node, RR_PARENTTRIPLESMAP, NodeRef::uri("#" + mappingRefNode.as<std::string>()));

			YAML::Node condNode = firstOf(objNode, {"condition", "conditions"});
			for (const YAML::Node &c : flattenList(condNode)) {
				if (!c.IsMap()) {
					continue;
				}
				YAML::Node fnNode = c["function"];
				std::string fn = (fnNode && fnNode.IsScalar()) ? fnNode.as<std::string>() : "";
				if (fn != "equal") {
					collector.addError("YARRRML parser: mapping '" + mappingName +
					                   "': unsupported join condition function '" + fn + "', condition skipped");
					continue;
				}
				YAML::Node paramsNode = c["parameters"];
				if (!paramsNode || !paramsNode.IsSequence() || paramsNode.size() < 2) {
					collector.addError("YARRRML parser: mapping '" + mappingName +
					                   "': join condition missing parameters, skipped");
					continue;
				}
				std::string childCol = extractColumnRef(paramsNode[0]);
				std::string parentCol = extractColumnRef(paramsNode[1]);
				if (childCol.empty() || parentCol.empty()) {
					collector.addError("YARRRML parser: mapping '" + mappingName +
					                   "': join condition parameters not recognised, skipped");
					continue;
				}
				NodeRef jc = blanks.next();
				emitLiteralTriple(collector, jc, RR_CHILD, childCol);
				emitLiteralTriple(collector, jc, RR_PARENT, parentCol);
				emitUriTriple(collector, node, RR_JOIN_CONDITION, jc);
			}
			return node;
		}

		YAML::Node valNode = firstOf(objNode, {"value", "v"});
		if (valNode && valNode.IsScalar()) {
			ValueExtra extra = extraIn;
			YAML::Node dtNode = objNode["datatype"];
			YAML::Node langNode = objNode["language"];
			if (dtNode && dtNode.IsScalar()) {
				extra.datatypeIri = resolveIri(dtNode.as<std::string>(), prefixes);
			} else if (langNode && langNode.IsScalar()) {
				extra.language = langNode.as<std::string>();
			}
			VSpec vs = classifyValue(valNode.as<std::string>(), true, true, prefixes);
			return emitValueSpecAsMap(collector, blanks, vs, prefixes, extra);
		}

		collector.addError("YARRRML parser: mapping '" + mappingName + "': unrecognised object entry, skipped");
		return NodeRef();
	}

	collector.addError("YARRRML parser: mapping '" + mappingName + "': unrecognised object entry, skipped");
	return NodeRef();
}

/// Accumulated result of translating a mapping's `po`/`predicateobjects`
/// entries: rdf:type shortcuts fold into subjectMap rr:class assertions,
/// everything else becomes a regular rr:predicateObjectMap blank node.
struct PoResult {
	std::vector<std::string> classIris;
	std::vector<NodeRef> pomNodes;
};

void emitPredObjPair(r2rml::TripleCollector &collector, BlankNodeMinter &blanks, const YAML::Node &predNode,
                     const YAML::Node &objNode, const ValueExtra &extra, const std::string &mappingName,
                     const std::map<std::string, std::string> &prefixes, PoResult &res) {
	std::vector<std::string> preds = flattenScalarList(predNode);
	std::vector<YAML::Node> objs = flattenList(objNode);

	if (preds.empty() || objs.empty()) {
		collector.addError("YARRRML parser: mapping '" + mappingName +
		                   "': po entry missing predicate or object, skipped");
		return;
	}

	for (const std::string &predRaw : preds) {
		bool isRdfType = (predRaw == "a");
		PredicateResult predResult = buildPredicateResult(collector, blanks, predRaw, prefixes);

		for (const YAML::Node &obj : objs) {
			if (isRdfType && extra.datatypeIri.empty() && extra.language.empty() && obj.IsScalar()) {
				VSpec vs = classifyValue(obj.as<std::string>(), false, false, prefixes);
				if (vs.kind == VKind::ConstIri) {
					res.classIris.push_back(resolveIri(vs.text, prefixes));
					continue;
				}
			}
			NodeRef objMap = emitObjectNode(collector, blanks, obj, prefixes, extra, mappingName);
			if (objMap.valid) {
				NodeRef pom = blanks.next();
				if (predResult.isConstant) {
					emitUriTriple(collector, pom, RR_PREDICATE, NodeRef::uri(predResult.constantIri));
				} else {
					emitUriTriple(collector, pom, RR_PREDICATE_MAP, predResult.mapNode);
				}
				emitUriTriple(collector, pom, RR_OBJECT_MAP, objMap);
				res.pomNodes.push_back(pom);
			}
		}
	}
}

PoResult emitPredicateObjectMaps(r2rml::TripleCollector &collector, BlankNodeMinter &blanks, const YAML::Node &mNode,
                                 const std::string &mappingName, const std::map<std::string, std::string> &prefixes) {
	PoResult res;
	YAML::Node poNode = firstOf(mNode, {"po", "predicateobjects", "predicateObjects"});
	if (!poNode) {
		return res;
	}
	if (!poNode.IsSequence()) {
		collector.addError("YARRRML parser: mapping '" + mappingName +
		                   "': po/predicateobjects must be a list, ignored");
		return res;
	}

	for (const YAML::Node &item : poNode) {
		if (item.IsSequence()) {
			if (item.size() < 2) {
				collector.addError("YARRRML parser: mapping '" + mappingName +
				                   "': po entry array must have at least 2 elements, skipped");
				continue;
			}
			ValueExtra extra;
			if (item.size() >= 3 && item[2].IsScalar()) {
				extra = buildDatatypeOrLangExtra(item[2].as<std::string>(), prefixes);
			}
			emitPredObjPair(collector, blanks, item[0], item[1], extra, mappingName, prefixes, res);
		} else if (item.IsMap()) {
			YAML::Node predNode = firstOf(item, {"predicates", "predicate", "p"});
			YAML::Node objNode = firstOf(item, {"objects", "object", "o"});
			if (!predNode || !objNode) {
				collector.addError("YARRRML parser: mapping '" + mappingName +
				                   "': po entry missing predicates/objects key, skipped");
				continue;
			}
			emitPredObjPair(collector, blanks, predNode, objNode, ValueExtra(), mappingName, prefixes, res);
		} else {
			collector.addError("YARRRML parser: mapping '" + mappingName + "': unrecognised po entry, skipped");
		}
	}
	return res;
}

/// Mint a blank node for the mapping's logical table (rr:tableName or
/// rr:sqlQuery) and emit its statements. Returns an invalid NodeRef (and
/// records a warning) if no usable source was found.
NodeRef emitLogicalTable(r2rml::TripleCollector &collector, BlankNodeMinter &blanks, const YAML::Node &mNode,
                         const std::map<std::string, YAML::Node> &namedSources, const std::string &mappingName) {
	YAML::Node sourcesNode = firstOf(mNode, {"sources", "source"});
	if (!sourcesNode) {
		collector.addError("YARRRML parser: mapping '" + mappingName + "' has no source; logicalTable omitted");
		return NodeRef();
	}

	std::vector<YAML::Node> sourceList = flattenList(sourcesNode);
	if (sourceList.empty()) {
		collector.addError("YARRRML parser: mapping '" + mappingName + "' has no source; logicalTable omitted");
		return NodeRef();
	}
	if (sourceList.size() > 1) {
		collector.addError("YARRRML parser: mapping '" + mappingName +
		                   "' has multiple sources; using the first and ignoring the rest");
	}

	YAML::Node src = sourceList[0];
	if (src.IsScalar()) {
		std::string refName = src.as<std::string>();
		auto it = namedSources.find(refName);
		if (it == namedSources.end()) {
			collector.addError("YARRRML parser: mapping '" + mappingName + "' references unknown source '" + refName +
			                   "'; logicalTable omitted");
			return NodeRef();
		}
		src = it->second;
	}

	if (!src.IsMap()) {
		collector.addError("YARRRML parser: mapping '" + mappingName +
		                   "' source is not a mapping; logicalTable omitted");
		return NodeRef();
	}

	YAML::Node queryNode = src["query"];
	if (queryNode && queryNode.IsScalar()) {
		NodeRef node = blanks.next();
		emitLiteralTriple(collector, node, RR_SQL_QUERY, queryNode.as<std::string>());
		return node;
	}
	YAML::Node tableNode = src["table"];
	if (tableNode && tableNode.IsScalar()) {
		NodeRef node = blanks.next();
		emitLiteralTriple(collector, node, RR_TABLE_NAME, tableNode.as<std::string>());
		return node;
	}

	collector.addError("YARRRML parser: mapping '" + mappingName +
	                   "' source has neither 'table' nor 'query'; logicalTable omitted");
	return NodeRef();
}

/// Mint a blank node for the mapping's subject map (value strategy + any
/// rr:class assertions) and emit its statements. Returns an invalid NodeRef
/// if there is neither a subject value nor any classes to assert.
NodeRef emitSubjectMap(r2rml::TripleCollector &collector, BlankNodeMinter &blanks, const YAML::Node &mNode,
                       const std::vector<std::string> &classIris, const std::string &mappingName,
                       const std::map<std::string, std::string> &prefixes) {
	bool haveValue = false;
	VKind valueKind = VKind::ConstIri;
	std::string valueText;

	YAML::Node subjNode = firstOf(mNode, {"subjects", "subject", "s"});
	if (subjNode) {
		std::vector<YAML::Node> list = flattenList(subjNode);
		if (list.size() > 1) {
			collector.addError("YARRRML parser: mapping '" + mappingName +
			                   "' has multiple subjects; using the first and ignoring the rest");
		}
		if (!list.empty()) {
			if (list[0].IsScalar()) {
				VSpec vs = classifyValue(list[0].as<std::string>(), /*allowIriSuffix=*/false,
				                         /*literalsAllowed=*/false, prefixes);
				haveValue = true;
				valueKind = vs.kind;
				valueText = vs.text;
			} else {
				collector.addError("YARRRML parser: mapping '" + mappingName +
				                   "' subject value must be a string, skipped");
			}
		}
	}

	if (!haveValue && classIris.empty()) {
		return NodeRef();
	}

	NodeRef node = blanks.next();
	if (haveValue) {
		switch (valueKind) {
		case VKind::Column:
			emitLiteralTriple(collector, node, RR_COLUMN, valueText);
			break;
		case VKind::Template:
			emitLiteralTriple(collector, node, RR_TEMPLATE, valueText);
			break;
		case VKind::ConstIri:
		default:
			emitUriTriple(collector, node, RR_CONSTANT, NodeRef::uri(resolveIri(valueText, prefixes)));
			break;
		}
	}
	for (const std::string &c : classIris) {
		emitUriTriple(collector, node, RR_CLASS, NodeRef::uri(c));
	}
	return node;
}

const std::set<std::string> &mappingKnownKeys() {
	static const std::set<std::string> keys = {"sources", "source",           "subjects",         "subject", "s",
	                                           "po",      "predicateobjects", "predicateObjects", "graphs",  "graph"};
	return keys;
}

void emitOneMapping(r2rml::TripleCollector &collector, BlankNodeMinter &blanks, const std::string &name,
                    const YAML::Node &mNode, const std::map<std::string, YAML::Node> &namedSources,
                    const std::map<std::string, std::string> &prefixes) {
	NodeRef logicalTable = emitLogicalTable(collector, blanks, mNode, namedSources, name);
	PoResult poResult = emitPredicateObjectMaps(collector, blanks, mNode, name, prefixes);
	NodeRef subjectMap = emitSubjectMap(collector, blanks, mNode, poResult.classIris, name, prefixes);

	if (firstOf(mNode, {"graphs", "graph"})) {
		collector.addError("YARRRML parser: mapping '" + name + "': graphs not supported, skipped");
	}

	for (YAML::const_iterator it = mNode.begin(); it != mNode.end(); ++it) {
		std::string key = it->first.as<std::string>();
		if (!mappingKnownKeys().count(key)) {
			collector.addError("YARRRML parser: mapping '" + name + "': unsupported key '" + key + "' ignored");
		}
	}

	NodeRef subject = NodeRef::uri("#" + name);

	if (!logicalTable.valid && !subjectMap.valid && poResult.pomNodes.empty()) {
		// Still a syntactically valid (but semantically inert) TriplesMap: the
		// R2RML object model only recognises a resource as a TriplesMap when it
		// carries rr:logicalTable/subjectMap/predicateObjectMap/subject.
		emitUriTriple(collector, subject, RDF_TYPE, NodeRef::uri(RR_TRIPLES_MAP));
		return;
	}

	if (logicalTable.valid) {
		emitUriTriple(collector, subject, RR_LOGICAL_TABLE, logicalTable);
	}
	if (subjectMap.valid) {
		emitUriTriple(collector, subject, RR_SUBJECT_MAP, subjectMap);
	}
	for (const NodeRef &pom : poResult.pomNodes) {
		emitUriTriple(collector, subject, RR_PREDICATE_OBJECT_MAP, pom);
	}
}

/// Parse `yamlText` and emit statements for every mapping it defines into
/// `collector`. Throws std::runtime_error on fatal problems (YAML syntax
/// errors, a missing `mappings` key, etc); non-fatal issues are recorded via
/// collector.addError().
void emitYarrrmlDocument(const std::string &yamlText, r2rml::TripleCollector &collector) {
	YAML::Node root;
	try {
		root = YAML::Load(yamlText);
	} catch (const YAML::Exception &e) {
		throw std::runtime_error(std::string("YARRRML parser: YAML syntax error: ") + e.what());
	}

	if (!root || !root.IsMap()) {
		throw std::runtime_error("YARRRML parser: document root must be a YAML mapping");
	}

	YAML::Node mappingsNode = firstOf(root, {"mappings", "mapping"});
	if (!mappingsNode || !mappingsNode.IsMap() || mappingsNode.size() == 0) {
		throw std::runtime_error("YARRRML parser: missing required 'mappings' key");
	}

	std::map<std::string, std::string> knownPrefixes = {
	    {"rr", "http://www.w3.org/ns/r2rml#"},
	    {"rdf", "http://www.w3.org/1999/02/22-rdf-syntax-ns#"},
	    {"rdfs", "http://www.w3.org/2000/01/rdf-schema#"},
	    {"xsd", "http://www.w3.org/2001/XMLSchema#"},
	};

	YAML::Node prefixesNode = root["prefixes"];
	if (prefixesNode && prefixesNode.IsMap()) {
		for (YAML::const_iterator it = prefixesNode.begin(); it != prefixesNode.end(); ++it) {
			knownPrefixes[it->first.as<std::string>()] = it->second.as<std::string>();
		}
	}

	// A document-level `base:` key overrides the file-URI base already set on
	// `collector` before translation started -- mirrors what an "@base <...> ."
	// directive occurring early in a Turtle document would have done, since
	// TripleCollector::setBase() resolves the new base against the current one.
	YAML::Node baseNode = root["base"];
	if (baseNode && baseNode.IsScalar()) {
		std::string docBase = baseNode.as<std::string>();
		SerdNode base = serd_node_from_string(SERD_URI, reinterpret_cast<const uint8_t *>(docBase.c_str()));
		collector.setBase(&base);
	}

	std::map<std::string, YAML::Node> namedSources;
	YAML::Node topSourcesNode = root["sources"];
	if (topSourcesNode && topSourcesNode.IsMap()) {
		for (YAML::const_iterator it = topSourcesNode.begin(); it != topSourcesNode.end(); ++it) {
			namedSources[it->first.as<std::string>()] = it->second;
		}
	}

	static const std::set<std::string> knownTopKeys = {"prefixes", "base",    "sources", "source",
	                                                   "mappings", "mapping", "authors"};
	for (YAML::const_iterator it = root.begin(); it != root.end(); ++it) {
		std::string key = it->first.as<std::string>();
		if (key == "authors" || knownTopKeys.count(key)) {
			continue;
		}
		collector.addError("YARRRML parser: unsupported top-level key '" + key + "' ignored");
	}

	BlankNodeMinter blanks;
	for (YAML::const_iterator it = mappingsNode.begin(); it != mappingsNode.end(); ++it) {
		std::string name = it->first.as<std::string>();
		YAML::Node mNode = it->second;
		if (!mNode.IsMap()) {
			collector.addError("YARRRML parser: mapping '" + name + "' is not a mapping node, skipped");
			continue;
		}
		emitOneMapping(collector, blanks, name, mNode, namedSources, knownPrefixes);
	}
}

std::string readFileToString(const std::string &path) {
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		throw std::runtime_error("YARRRML parser: cannot read file: " + path);
	}
	std::ostringstream ss;
	ss << in.rdbuf();
	return ss.str();
}

std::string computeFileBaseUri(const std::string &path) {
	SerdNode node = serd_node_new_file_uri(reinterpret_cast<const uint8_t *>(path.c_str()), /*hostname=*/nullptr,
	                                       /*out=*/nullptr, /*escape=*/true);
	std::string uri;
	if (node.buf) {
		uri.assign(reinterpret_cast<const char *>(node.buf), node.n_bytes);
	}
	serd_node_free(&node);
	if (uri.empty()) {
		throw std::runtime_error("YARRRML parser: could not build file URI for: " + path);
	}
	return uri;
}

} // namespace

// ---------------------------------------------------------------------------
// YARRRMLParser implementation
// ---------------------------------------------------------------------------

bool YARRRMLParser::hasYarrrmlExtension(const std::string &path) {
	static const char *const yarrrmlExtensions[] = {".yml", ".yaml", ".yarrrml"};
	for (const char *ext : yarrrmlExtensions) {
		std::size_t extLen = std::strlen(ext);
		if (path.size() >= extLen && path.compare(path.size() - extLen, extLen, ext) == 0) {
			return true;
		}
	}
	return false;
}

r2rml::R2RMLMapping YARRRMLParser::parse(const std::string &yarrrmlFilePath, bool ignoreNonFatalErrors) {
	std::string yamlText = readFileToString(yarrrmlFilePath);
	std::string baseUri = computeFileBaseUri(yarrrmlFilePath);

	r2rml::TripleCollector collector;
	SerdNode base = serd_node_from_string(SERD_URI, reinterpret_cast<const uint8_t *>(baseUri.c_str()));
	collector.setBase(&base);

	emitYarrrmlDocument(yamlText, collector);

	r2rml::R2RMLParser parser;
	return parser.parseCollected(collector, ignoreNonFatalErrors);
}

} // namespace yarrrml
