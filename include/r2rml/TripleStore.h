#pragma once

#include <map>
#include <string>
#include <vector>

namespace r2rml {

/**
 * Kind of RDF term held by an ObjValue (subjects/predicates are always plain
 * string keys, so only objects need a type tag).
 */
enum class ObjType { URI, Blank, Literal };

/**
 * A single triple's object, as collected from Turtle/YARRRML source: a URI,
 * a blank-node label (no "_:" prefix), or a literal with optional
 * datatype/language.
 */
struct ObjValue {
	ObjType type;
	std::string value;    ///< URI string, blank-node ID (no "_:" prefix), or literal text
	std::string datatype; ///< For typed literals - full URI
	std::string lang;     ///< For language-tagged literals
};

/**
 * In-memory store of triples collected from an R2RML/YARRRML source,
 * keyed by subject then predicate: subject -> predicate -> object list.
 * Subject/predicate keys are absolute URIs, or "_:<id>" for blank nodes (see
 * TripleCollector).
 *
 * Provides the lookup helpers the R2RML build phase needs (single-valued
 * "first object of this type" queries) without exposing the underlying map
 * structure, so it can be unit-tested independently of Turtle parsing.
 */
class TripleStore {
public:
	using PredMap = std::map<std::string, std::vector<ObjValue>>;
	using Map = std::map<std::string, PredMap>;
	using const_iterator = Map::const_iterator;

	/// Record one triple's object under (subj, pred).
	void insert(const std::string &subj, const std::string &pred, ObjValue obj);

	/// All objects recorded under (subj, pred), or nullptr if none exist.
	const std::vector<ObjValue> *getObjects(const std::string &subj, const std::string &pred) const;

	/// First object under (subj, pred) that is a literal, or "" if none.
	std::string getFirstLiteral(const std::string &subj, const std::string &pred) const;

	/// First object under (subj, pred) that is a URI, or "" if none.
	std::string getFirstUri(const std::string &subj, const std::string &pred) const;

	/// First object under (subj, pred), as a subject-lookup key (see objKey()), or "" if none.
	std::string getFirstObjKey(const std::string &subj, const std::string &pred) const;

	/// Canonical lookup key for an ObjValue: "_:<id>" for blank nodes, URI
	/// string for named nodes, empty string for literals (which can't be
	/// used as a subject).
	static std::string objKey(const ObjValue &o);

	bool empty() const {
		return data_.empty();
	}

	const_iterator begin() const {
		return data_.begin();
	}

	const_iterator end() const {
		return data_.end();
	}

private:
	Map data_;
};

} // namespace r2rml
