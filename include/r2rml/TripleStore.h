#pragma once

#include "rdf/Term.h"

#include <map>
#include <string>
#include <vector>

namespace r2rml {

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
	using PredMap = std::map<std::string, std::vector<rdf::Term>>;
	using Map = std::map<std::string, PredMap>;
	using const_iterator = Map::const_iterator;

	/// Record one triple's object under (subj, pred).
	void insert(const std::string &subj, const std::string &pred, rdf::Term obj);

	/// All objects recorded under (subj, pred), or nullptr if none exist.
	const std::vector<rdf::Term> *getObjects(const std::string &subj, const std::string &pred) const;

	/// Lexical form of the first object under (subj, pred) that is a literal,
	/// or "" if none.
	///
	/// NOTE: this discards any datatype or language tag on that literal, which
	/// is fine for the single-valued R2RML properties it serves (rr:template,
	/// rr:column, rr:tableName, ...) but is a real loss elsewhere. Callers that
	/// need the whole term should use getFirstOfKind().
	std::string getFirstLiteral(const std::string &subj, const std::string &pred) const;

	/// Lexical form of the first object under (subj, pred) that is an IRI, or
	/// "" if none.
	std::string getFirstUri(const std::string &subj, const std::string &pred) const;

	/// First object under (subj, pred) of the given kind, or nullptr if none.
	///
	/// Unlike getFirstLiteral()/getFirstUri() this returns the whole term, so
	/// the caller keeps the datatype and language tag. The pointer is owned by
	/// the store and is invalidated by a later insert().
	const rdf::Term *getFirstOfKind(const std::string &subj, const std::string &pred, rdf::TermKind kind) const;

	/// First object under (subj, pred), as a subject-lookup key (see objKey()), or "" if none.
	std::string getFirstObjKey(const std::string &subj, const std::string &pred) const;

	/// Canonical lookup key for a collected object: "_:<id>" for blank nodes,
	/// the IRI for named nodes, empty string for literals (which cannot be used
	/// as a subject).
	///
	/// The "_:" prefix is added HERE rather than stored on the term: it is this
	/// store's key-space encoding, not part of the RDF term. rdf::Term always
	/// holds a bare blank-node label.
	static std::string objKey(const rdf::Term &o);

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
