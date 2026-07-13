// YARRRML → R2RML translator.
//
// Strategy: translate the supported YARRRML subset (see YARRRMLParser.h) into
// an in-memory R2RML Turtle document, then hand that document to
// r2rml::R2RMLParser::parseString() to build the actual object model.  This
// keeps all term-map/engine semantics (template expansion, datatypes, joins,
// ...) identical between R2RML and YARRRML mappings, since YARRRML mappings
// are ultimately executed by the very same R2RML engine.
//
// Non-fatal problems encountered while translating (unsupported keys, a
// mapping with no source, an unresolved join-condition function, ...) are
// collected into a `warnings` vector rather than raised immediately; the
// caller (YARRRMLParser::parse) decides whether to merge them into the
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
#include <vector>

namespace yarrrml {

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
// Turtle string-literal escaping
// ---------------------------------------------------------------------------

/// Escape a value for use inside a short (single-quoted) Turtle string.
std::string quoted(const std::string &s) {
	std::string out;
	out.reserve(s.size() + 2);
	out += '"';
	for (char c : s) {
		switch (c) {
		case '\\':
			out += "\\\\";
			break;
		case '"':
			out += "\\\"";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\t':
			out += "\\t";
			break;
		case '\r':
			out += "\\r";
			break;
		default:
			out += c;
		}
	}
	out += '"';
	return out;
}

/// Escape a value for use inside a long/triple-quoted Turtle string (used for
/// rr:sqlQuery so multi-line SQL text stays readable).
std::string tripleQuoted(const std::string &s) {
	std::string out;
	out.reserve(s.size() + 6);
	out += "\"\"\"";
	for (char c : s) {
		if (c == '"') {
			out += "\\\"";
		} else if (c == '\\') {
			out += "\\\\";
		} else {
			out += c;
		}
	}
	out += "\"\"\"";
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

/// Render `v` as a Turtle IRI term: a bare CURIE token when it uses a known
/// prefix, otherwise an absolute (or best-effort relative) <IRI>.
std::string iriToken(const std::string &v, const std::map<std::string, std::string> &prefixes) {
	if (looksAbsoluteIri(v)) {
		return "<" + v + ">";
	}
	if (looksCurie(v, prefixes)) {
		return v;
	}
	return "<" + v + ">";
}

/// If `text` begins with "prefix:" for a known prefix (and isn't already an
/// absolute "scheme://..." IRI), replace the prefix with its full namespace
/// IRI. Needed for rr:template literal text: unlike a bare CURIE term, text
/// embedded in a Turtle string literal is never resolved by the downstream
/// Turtle parser's own prefix mechanism, so it must be expanded here.
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

/// Render a VSpec as an rr:objectMap (or rr:predicateMap-compatible) blank
/// node fragment, e.g. "[ rr:column \"COL\" ]".  `extra` is appended inside
/// the brackets (used for rr:datatype / rr:language / rr:termType).
std::string valueSpecToMapFragment(const VSpec &vs, const std::map<std::string, std::string> &prefixes, const std::string &extra) {
	switch (vs.kind) {
	case VKind::Column: {
		std::string s = "[ rr:column " + quoted(vs.text);
		if (vs.forceIri) {
			s += "; rr:termType rr:IRI";
		}
		s += extra;
		s += " ]";
		return s;
	}
	case VKind::Template:
		return "[ rr:template " + quoted(vs.text) + extra + " ]";
	case VKind::ConstIri:
		return "[ rr:constant " + iriToken(vs.text, prefixes) + " ]";
	case VKind::ConstLit:
	default:
		return "[ rr:constant " + quoted(vs.text) + extra + " ]";
	}
}

/// Translate a po shortcut's third array element ("xsd:integer" or "en~lang")
/// into an "; rr:datatype ..." or "; rr:language ..." suffix.
std::string buildDatatypeOrLangFragment(const std::string &raw, const std::map<std::string, std::string> &prefixes) {
	static const std::string suf = "~lang";
	if (raw.size() > suf.size() && raw.compare(raw.size() - suf.size(), suf.size(), suf) == 0) {
		return "; rr:language " + quoted(raw.substr(0, raw.size() - suf.size()));
	}
	return "; rr:datatype " + iriToken(raw, prefixes);
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

/// Translate a predicate value ("a", a CURIE/IRI, or a $(...) template) into
/// an "rr:predicate ..." or "rr:predicateMap [...]" fragment.
std::string buildPredicateFragment(const std::string &raw, const std::map<std::string, std::string> &prefixes) {
	if (raw == "a") {
		return "rr:predicate rdf:type";
	}
	VSpec vs = classifyValue(raw, /*allowIriSuffix=*/false, /*literalsAllowed=*/false, prefixes);
	switch (vs.kind) {
	case VKind::Template:
		return "rr:predicateMap [ rr:template " + quoted(vs.text) + " ]";
	case VKind::Column:
		return "rr:predicateMap [ rr:column " + quoted(vs.text) + " ]";
	case VKind::ConstIri:
	default:
		return "rr:predicate " + iriToken(vs.text, prefixes);
	}
}

/// Translate one object-list entry (a plain scalar, a [value, dtOrLang]
/// array, a {value:/v:, datatype:/language:} map, or a {mapping: ...,
/// condition(s): ...} mapping reference) into an rr:objectMap fragment.
std::string buildObjectFragment(const YAML::Node &objNode, const std::map<std::string, std::string> &prefixes,
                                const std::string &extraTtl, const std::string &mappingName,
                                std::vector<std::string> &warnings) {
	if (objNode.IsScalar()) {
		VSpec vs =
		    classifyValue(objNode.as<std::string>(), /*allowIriSuffix=*/true, /*literalsAllowed=*/true, prefixes);
		return valueSpecToMapFragment(vs, prefixes, extraTtl);
	}

	if (objNode.IsSequence()) {
		if (objNode.size() < 1 || !objNode[0].IsScalar()) {
			warnings.push_back("YARRRML parser: mapping '" + mappingName + "': malformed object value array, skipped");
			return "";
		}
		std::string extra = extraTtl;
		if (objNode.size() >= 2 && objNode[1].IsScalar()) {
			extra += buildDatatypeOrLangFragment(objNode[1].as<std::string>(), prefixes);
		}
		VSpec vs = classifyValue(objNode[0].as<std::string>(), true, true, prefixes);
		return valueSpecToMapFragment(vs, prefixes, extra);
	}

	if (objNode.IsMap()) {
		YAML::Node mappingRefNode = objNode["mapping"];
		if (mappingRefNode && mappingRefNode.IsScalar()) {
			std::string frag = "[ rr:parentTriplesMap <#" + mappingRefNode.as<std::string>() + ">";

			YAML::Node condNode = firstOf(objNode, {"condition", "conditions"});
			for (const YAML::Node &c : flattenList(condNode)) {
				if (!c.IsMap()) {
					continue;
				}
				YAML::Node fnNode = c["function"];
				std::string fn = (fnNode && fnNode.IsScalar()) ? fnNode.as<std::string>() : "";
				if (fn != "equal") {
					warnings.push_back("YARRRML parser: mapping '" + mappingName +
					                   "': unsupported join condition function '" + fn + "', condition skipped");
					continue;
				}
				YAML::Node paramsNode = c["parameters"];
				if (!paramsNode || !paramsNode.IsSequence() || paramsNode.size() < 2) {
					warnings.push_back("YARRRML parser: mapping '" + mappingName +
					                   "': join condition missing parameters, skipped");
					continue;
				}
				std::string childCol = extractColumnRef(paramsNode[0]);
				std::string parentCol = extractColumnRef(paramsNode[1]);
				if (childCol.empty() || parentCol.empty()) {
					warnings.push_back("YARRRML parser: mapping '" + mappingName +
					                   "': join condition parameters not recognised, skipped");
					continue;
				}
				frag += "; rr:joinCondition [ rr:child " + quoted(childCol) + "; rr:parent " + quoted(parentCol) + " ]";
			}
			frag += " ]";
			return frag;
		}

		YAML::Node valNode = firstOf(objNode, {"value", "v"});
		if (valNode && valNode.IsScalar()) {
			std::string extra = extraTtl;
			YAML::Node dtNode = objNode["datatype"];
			YAML::Node langNode = objNode["language"];
			if (dtNode && dtNode.IsScalar()) {
				extra += "; rr:datatype " + iriToken(dtNode.as<std::string>(), prefixes);
			} else if (langNode && langNode.IsScalar()) {
				extra += "; rr:language " + quoted(langNode.as<std::string>());
			}
			VSpec vs = classifyValue(valNode.as<std::string>(), true, true, prefixes);
			return valueSpecToMapFragment(vs, prefixes, extra);
		}

		warnings.push_back("YARRRML parser: mapping '" + mappingName + "': unrecognised object entry, skipped");
		return "";
	}

	warnings.push_back("YARRRML parser: mapping '" + mappingName + "': unrecognised object entry, skipped");
	return "";
}

/// Accumulated result of translating a mapping's `po`/`predicateobjects`
/// entries: rdf:type shortcuts fold into subjectMap rr:class assertions,
/// everything else becomes a regular rr:predicateObjectMap fragment.
struct PoResult {
	std::vector<std::string> classIris;
	std::vector<std::string> pomFragments;
};

void processPredObjPair(const YAML::Node &predNode, const YAML::Node &objNode, const std::string &extraTtl,
                        const std::string &mappingName, const std::map<std::string, std::string> &prefixes,
                        std::vector<std::string> &warnings, PoResult &res) {
	std::vector<std::string> preds = flattenScalarList(predNode);
	std::vector<YAML::Node> objs = flattenList(objNode);

	if (preds.empty() || objs.empty()) {
		warnings.push_back("YARRRML parser: mapping '" + mappingName +
		                   "': po entry missing predicate or object, skipped");
		return;
	}

	for (const std::string &predRaw : preds) {
		bool isRdfType = (predRaw == "a");
		std::string predFragment = buildPredicateFragment(predRaw, prefixes);

		for (const YAML::Node &obj : objs) {
			if (isRdfType && extraTtl.empty() && obj.IsScalar()) {
				VSpec vs = classifyValue(obj.as<std::string>(), false, false, prefixes);
				if (vs.kind == VKind::ConstIri) {
					res.classIris.push_back(iriToken(vs.text, prefixes));
					continue;
				}
			}
			std::string objFrag = buildObjectFragment(obj, prefixes, extraTtl, mappingName, warnings);
			if (!objFrag.empty()) {
				res.pomFragments.push_back("[ " + predFragment + "; rr:objectMap " + objFrag + " ]");
			}
		}
	}
}

PoResult buildPredicateObjectMaps(const YAML::Node &mNode, const std::string &mappingName,
                                  const std::map<std::string, std::string> &prefixes, std::vector<std::string> &warnings) {
	PoResult res;
	YAML::Node poNode = firstOf(mNode, {"po", "predicateobjects", "predicateObjects"});
	if (!poNode) {
		return res;
	}
	if (!poNode.IsSequence()) {
		warnings.push_back("YARRRML parser: mapping '" + mappingName +
		                   "': po/predicateobjects must be a list, ignored");
		return res;
	}

	for (const YAML::Node &item : poNode) {
		if (item.IsSequence()) {
			if (item.size() < 2) {
				warnings.push_back("YARRRML parser: mapping '" + mappingName +
				                   "': po entry array must have at least 2 elements, skipped");
				continue;
			}
			std::string extra;
			if (item.size() >= 3 && item[2].IsScalar()) {
				extra = buildDatatypeOrLangFragment(item[2].as<std::string>(), prefixes);
			}
			processPredObjPair(item[0], item[1], extra, mappingName, prefixes, warnings, res);
		} else if (item.IsMap()) {
			YAML::Node predNode = firstOf(item, {"predicates", "predicate", "p"});
			YAML::Node objNode = firstOf(item, {"objects", "object", "o"});
			if (!predNode || !objNode) {
				warnings.push_back("YARRRML parser: mapping '" + mappingName +
				                   "': po entry missing predicates/objects key, skipped");
				continue;
			}
			processPredObjPair(predNode, objNode, "", mappingName, prefixes, warnings, res);
		} else {
			warnings.push_back("YARRRML parser: mapping '" + mappingName + "': unrecognised po entry, skipped");
		}
	}
	return res;
}

std::string buildLogicalTable(const YAML::Node &mNode, const std::map<std::string, YAML::Node> &namedSources,
                              const std::string &mappingName, std::vector<std::string> &warnings) {
	YAML::Node sourcesNode = firstOf(mNode, {"sources", "source"});
	if (!sourcesNode) {
		warnings.push_back("YARRRML parser: mapping '" + mappingName + "' has no source; logicalTable omitted");
		return "";
	}

	std::vector<YAML::Node> sourceList = flattenList(sourcesNode);
	if (sourceList.empty()) {
		warnings.push_back("YARRRML parser: mapping '" + mappingName + "' has no source; logicalTable omitted");
		return "";
	}
	if (sourceList.size() > 1) {
		warnings.push_back("YARRRML parser: mapping '" + mappingName +
		                   "' has multiple sources; using the first and ignoring the rest");
	}

	YAML::Node src = sourceList[0];
	if (src.IsScalar()) {
		std::string refName = src.as<std::string>();
		auto it = namedSources.find(refName);
		if (it == namedSources.end()) {
			warnings.push_back("YARRRML parser: mapping '" + mappingName + "' references unknown source '" + refName +
			                   "'; logicalTable omitted");
			return "";
		}
		src = it->second;
	}

	if (!src.IsMap()) {
		warnings.push_back("YARRRML parser: mapping '" + mappingName +
		                   "' source is not a mapping; logicalTable omitted");
		return "";
	}

	YAML::Node queryNode = src["query"];
	if (queryNode && queryNode.IsScalar()) {
		return "[ rr:sqlQuery " + tripleQuoted(queryNode.as<std::string>()) + " ]";
	}
	YAML::Node tableNode = src["table"];
	if (tableNode && tableNode.IsScalar()) {
		return "[ rr:tableName " + quoted(tableNode.as<std::string>()) + " ]";
	}

	warnings.push_back("YARRRML parser: mapping '" + mappingName +
	                   "' source has neither 'table' nor 'query'; logicalTable omitted");
	return "";
}

std::string buildSubjectMap(const YAML::Node &mNode, const std::vector<std::string> &classIris,
                            const std::string &mappingName, const std::map<std::string, std::string> &prefixes,
                            std::vector<std::string> &warnings) {
	std::string valueFrag;
	YAML::Node subjNode = firstOf(mNode, {"subjects", "subject", "s"});
	if (subjNode) {
		std::vector<YAML::Node> list = flattenList(subjNode);
		if (list.size() > 1) {
			warnings.push_back("YARRRML parser: mapping '" + mappingName +
			                   "' has multiple subjects; using the first and ignoring the rest");
		}
		if (!list.empty()) {
			if (list[0].IsScalar()) {
				VSpec vs = classifyValue(list[0].as<std::string>(), /*allowIriSuffix=*/false,
				                         /*literalsAllowed=*/false, prefixes);
				switch (vs.kind) {
				case VKind::Column:
					valueFrag = "rr:column " + quoted(vs.text);
					break;
				case VKind::Template:
					valueFrag = "rr:template " + quoted(vs.text);
					break;
				case VKind::ConstIri:
				default:
					valueFrag = "rr:constant " + iriToken(vs.text, prefixes);
					break;
				}
			} else {
				warnings.push_back("YARRRML parser: mapping '" + mappingName +
				                   "' subject value must be a string, skipped");
			}
		}
	}

	if (valueFrag.empty() && classIris.empty()) {
		return "";
	}

	std::string out = "[ ";
	bool firstPart = true;
	if (!valueFrag.empty()) {
		out += valueFrag;
		firstPart = false;
	}
	for (const std::string &c : classIris) {
		if (!firstPart) {
			out += "; ";
		}
		out += "rr:class " + c;
		firstPart = false;
	}
	out += " ]";
	return out;
}

const std::set<std::string> &mappingKnownKeys() {
	static const std::set<std::string> keys = {"sources", "source",           "subjects",         "subject", "s",
	                                           "po",      "predicateobjects", "predicateObjects", "graphs",  "graph"};
	return keys;
}

void translateOneMapping(std::ostream &out, const std::string &name, const YAML::Node &mNode,
                         const std::map<std::string, YAML::Node> &namedSources, const std::map<std::string, std::string> &prefixes,
                         std::vector<std::string> &warnings) {
	std::string logicalTableFrag = buildLogicalTable(mNode, namedSources, name, warnings);
	PoResult poResult = buildPredicateObjectMaps(mNode, name, prefixes, warnings);
	std::string subjectFrag = buildSubjectMap(mNode, poResult.classIris, name, prefixes, warnings);

	if (firstOf(mNode, {"graphs", "graph"})) {
		warnings.push_back("YARRRML parser: mapping '" + name + "': graphs not supported, skipped");
	}

	for (YAML::const_iterator it = mNode.begin(); it != mNode.end(); ++it) {
		std::string key = it->first.as<std::string>();
		if (!mappingKnownKeys().count(key)) {
			warnings.push_back("YARRRML parser: mapping '" + name + "': unsupported key '" + key + "' ignored");
		}
	}

	std::vector<std::string> parts;
	if (!logicalTableFrag.empty()) {
		parts.push_back("rr:logicalTable " + logicalTableFrag);
	}
	if (!subjectFrag.empty()) {
		parts.push_back("rr:subjectMap " + subjectFrag);
	}
	for (const std::string &p : poResult.pomFragments) {
		parts.push_back("rr:predicateObjectMap " + p);
	}

	out << "<#" << name << ">\n";
	if (parts.empty()) {
		// Still a syntactically valid (but semantically inert) block: the
		// R2RML object model only recognises a resource as a TriplesMap when
		// it carries rr:logicalTable/subjectMap/predicateObjectMap/subject.
		out << "    a rr:TriplesMap .\n\n";
		return;
	}
	for (std::size_t i = 0; i < parts.size(); ++i) {
		out << "    " << parts[i] << (i + 1 < parts.size() ? " ;\n" : " .\n\n");
	}
}

std::string translateToTurtleImpl(const std::string &yamlText, std::vector<std::string> &warnings) {
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
	std::vector<std::pair<std::string, std::string>> userPrefixes;

	YAML::Node prefixesNode = root["prefixes"];
	if (prefixesNode && prefixesNode.IsMap()) {
		for (YAML::const_iterator it = prefixesNode.begin(); it != prefixesNode.end(); ++it) {
			std::string name = it->first.as<std::string>();
			std::string uri = it->second.as<std::string>();
			userPrefixes.emplace_back(name, uri);
			knownPrefixes[name] = uri;
		}
	}

	std::string baseUri;
	YAML::Node baseNode = root["base"];
	if (baseNode && baseNode.IsScalar()) {
		baseUri = baseNode.as<std::string>();
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
		warnings.push_back("YARRRML parser: unsupported top-level key '" + key + "' ignored");
	}

	std::ostringstream out;
	out << "@prefix rr: <http://www.w3.org/ns/r2rml#> .\n";
	out << "@prefix rdf: <http://www.w3.org/1999/02/22-rdf-syntax-ns#> .\n";
	out << "@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .\n";
	out << "@prefix xsd: <http://www.w3.org/2001/XMLSchema#> .\n";
	for (const auto &p : userPrefixes) {
		out << "@prefix " << p.first << ": <" << p.second << "> .\n";
	}
	if (!baseUri.empty()) {
		out << "@base <" << baseUri << "> .\n";
	}
	out << "\n";

	for (YAML::const_iterator it = mappingsNode.begin(); it != mappingsNode.end(); ++it) {
		std::string name = it->first.as<std::string>();
		YAML::Node mNode = it->second;
		if (!mNode.IsMap()) {
			warnings.push_back("YARRRML parser: mapping '" + name + "' is not a mapping node, skipped");
			continue;
		}
		translateOneMapping(out, name, mNode, namedSources, knownPrefixes, warnings);
	}

	return out.str();
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

std::string YARRRMLParser::translateToTurtle(const std::string &yamlText) {
	std::vector<std::string> warnings; // discarded: use parse() for warning reporting
	return translateToTurtleImpl(yamlText, warnings);
}

std::string YARRRMLParser::translateFileToTurtle(const std::string &yarrrmlFilePath) {
	return translateToTurtle(readFileToString(yarrrmlFilePath));
}

r2rml::R2RMLMapping YARRRMLParser::parse(const std::string &yarrrmlFilePath, bool ignoreNonFatalErrors) {
	std::string yamlText = readFileToString(yarrrmlFilePath);

	std::vector<std::string> warnings;
	std::string turtle = translateToTurtleImpl(yamlText, warnings);

	std::string baseUri = computeFileBaseUri(yarrrmlFilePath);

	r2rml::R2RMLParser parser;
	r2rml::R2RMLMapping mapping = parser.parseString(turtle, baseUri, ignoreNonFatalErrors);

	if (!warnings.empty()) {
		if (ignoreNonFatalErrors) {
			for (const std::string &w : warnings) {
				mapping.parseErrors.push_back(w);
			}
		} else {
			std::ostringstream msg;
			for (const std::string &w : warnings) {
				msg << w << "\n";
			}
			throw std::runtime_error(msg.str());
		}
	}

	return mapping;
}

} // namespace yarrrml
