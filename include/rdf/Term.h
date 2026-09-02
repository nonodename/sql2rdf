#pragma once

#include "rdf/TermKind.h"

#include <ostream>
#include <string>

namespace rdf {

/// rdf:langString - the implicit datatype of every language-tagged literal
/// (RDF 1.1 Section 3.3).
// extern const char *const RDF_LANG_STRING;
constexpr const char *const RDF_LANG_STRING = "http://www.w3.org/1999/02/22-rdf-syntax-ns#langString";
constexpr const char *const XSD_INTEGER = "http://www.w3.org/2001/XMLSchema#integer";
constexpr const char *const XSD_DOUBLE = "http://www.w3.org/2001/XMLSchema#double";
constexpr const char *const XSD_BOOLEAN = "http://www.w3.org/2001/XMLSchema#boolean";
constexpr const char *const XSD_DECIMAL = "http://www.w3.org/2001/XMLSchema#decimal";
constexpr const char *const RDF_NAMESPACE = "http://www.w3.org/1999/02/22-rdf-syntax-ns#";
constexpr const char *const RDFS_NAMESPACE = "http://www.w3.org/2000/01/rdf-schema#";
constexpr const char *const XSD_NAMESPACE = "http://www.w3.org/2001/XMLSchema#";
constexpr const char *const RDF_TYPE = "http://www.w3.org/1999/02/22-rdf-syntax-ns#type";
/**
 * An owning, self-contained RDF term: a kind, a lexical form, and - for
 * literals - either a datatype IRI or a language tag.
 *
 * This is the project's single value representation of "an RDF term".  It
 * subsumes the parse-time r2rml::ObjValue and the loose
 * {SerdNode object, const SerdNode *datatype, const SerdNode *lang} triple that
 * PredicateObjectMap::processRow otherwise has to assemble by hand.
 *
 * Note what it is NOT: a wrapper around SerdNode.  Serd 0.x's SerdNode is
 * {buf, n_bytes, n_chars, flags, type} - it carries no datatype and no
 * language, since serd passes those as separate nodes alongside the statement.
 * It is also non-owning: serd_node_from_string only measures its argument, so
 * every SerdNode borrows a buffer somebody else must keep alive.  Term is
 * therefore a superset, not a shell, and owning its bytes is the point.
 *
 * It knows nothing about Serd, SQL or SPARQL.  Conversion to and from SerdNode
 * lives in <r2rml/SerdTerm.h>, so this header stays usable from layers that
 * must not depend on a serialisation library.
 *
 * REPRESENTATION INVARIANTS - established by the named constructors and the
 * assign* mutators, and relied upon by the Serd bridge:
 *
 *   - kind() == Unknown  <=>  the term is absent, and lexical/datatype/lang are
 *     all empty.
 *   - lexical() is the BARE value: an absolute IRI with no angle brackets, a
 *     blank-node label with NO "_:" prefix, or a literal's lexical form with no
 *     quoting and no escaping.  Consumers needing a prefixed blank-node form
 *     (r2rml::TripleStore's subject keys) add "_:" themselves - that prefix is a
 *     key-space encoding, not part of the term.
 *   - datatypeIri() and lang() are empty unless kind() == Literal.
 *   - datatypeIri() and lang() are never both non-empty.  A language-tagged
 *     literal stores only the tag; its datatype is rdf:langString implicitly and
 *     is reported by effectiveDatatypeIri().  This mirrors Turtle and
 *     N-Triples, where "x"@en^^rdf:langString is not writable, and it is what
 *     makes the datatype/lang branch at the serd boundary total rather than a
 *     guess.
 *
 * Copying a Term copies up to three std::strings.  On the per-row generation
 * path that is worth avoiding: hoist one Term out of the row loop and pass it
 * to generateRDFTerm() as an out-parameter, calling clear() between rows.
 * clear() keeps the string capacities, so after a few rows the loop stops
 * allocating - the same property the mutable cachedValue_/expanded_ members
 * used to provide, but without the shared mutable state that made term maps
 * only nominally const.
 */
class Term {
public:
	/// The absent term.
	Term() = default;

	// ---- Named constructors ---------------------------------------------

	static Term iri(const std::string &value);

	/// `label` must NOT carry a "_:" prefix; see the invariants above.
	static Term blankNode(const std::string &label);

	/// A literal with neither datatype nor language tag.  This is NOT the same
	/// as an xsd:string literal: an empty datatype means "no datatype stated",
	/// which R2RML's natural mapping treats as distinct from a known xsd:string.
	static Term literal(const std::string &lexical_form);

	/// A literal with an explicit datatype.  An empty `datatype_iri` degrades to
	/// literal().  So does RDF_LANG_STRING: a langString with no tag is not a
	/// well-formed term - use langLiteral().
	static Term typedLiteral(const std::string &lexical_form, const std::string &datatype_iri);

	/// A language-tagged literal.  An empty `language_tag` degrades to literal().
	static Term langLiteral(const std::string &lexical_form, const std::string &language_tag);

	// ---- Accessors -------------------------------------------------------

	TermKind kind() const {
		return kind_;
	}
	bool isNull() const {
		return kind_ == TermKind::Unknown;
	}
	bool isIri() const {
		return kind_ == TermKind::Iri;
	}
	bool isBlankNode() const {
		return kind_ == TermKind::BlankNode;
	}
	bool isLiteral() const {
		return kind_ == TermKind::Literal;
	}

	/// Explicit so that `if (term)` reads well without Term silently converting
	/// to int in arithmetic or comparisons.
	explicit operator bool() const {
		return kind_ != TermKind::Unknown;
	}

	const std::string &lexical() const {
		return lexical_;
	}

	/// The STORED datatype: empty for a non-literal, for a plain literal, and
	/// for a language-tagged literal.  Use this when deciding what to write.
	const std::string &datatypeIri() const {
		return datatype_;
	}

	const std::string &lang() const {
		return lang_;
	}
	bool hasLang() const {
		return !lang_.empty();
	}

	/// rdf:langString when a language tag is present, otherwise datatypeIri().
	/// Use this when reasoning about the term's abstract datatype (SPARQL
	/// DATATYPE(), term equality); use datatypeIri() when serialising.
	std::string effectiveDatatypeIri() const;

	// ---- In-place mutation, for the per-row generation path ---------------

	/// Reset to the absent term WITHOUT releasing the string buffers.
	void clear();

	void assignIri(const std::string &value);
	void assignBlankNode(const std::string &label);
	void assignLiteral(const std::string &lexical_form);
	void assignTypedLiteral(const std::string &lexical_form, const std::string &datatype_iri);
	void assignLangLiteral(const std::string &lexical_form, const std::string &language_tag);

	/// Attach a datatype to an existing literal, preserving its lexical form
	/// and clearing any language tag. Ignored for a non-literal, where a
	/// datatype is meaningless. An empty `datatype_iri` just removes the
	/// datatype, leaving a plain literal.
	///
	/// This exists so callers that annotate a term after generating it (the
	/// object position, where R2RML's rr:datatype and rr:language apply) do not
	/// have to pass the term's own lexical() back into assignTypedLiteral().
	void setDatatypeIri(const std::string &datatype_iri);

	/// Attach a language tag to an existing literal, preserving its lexical
	/// form and clearing any datatype. Ignored for a non-literal.
	void setLang(const std::string &language_tag);

	/// Mutable access to the lexical buffer, for callers that build the value
	/// piecewise rather than handing over a finished string - TemplateTermMap's
	/// placeholder expansion is the reason this exists.
	///
	/// Contract: clear() first (which empties the datatype and language, so it
	/// cannot leave a non-literal carrying a stale tag), append, then setKind().
	/// Do not use it to edit a term that already has a kind.
	std::string &mutableLexical() {
		return lexical_;
	}
	void setKind(TermKind kind) {
		kind_ = kind;
	}

	// ---- Comparison -------------------------------------------------------

	/// RDF term equality on the abstract term rather than on the
	/// representation: two literals are equal when their lexical forms and
	/// their EFFECTIVE datatypes agree.  Two absent terms are equal.
	///
	/// Language tags compare case-sensitively.  BCP 47 says they should not,
	/// but nothing here relies on case-insensitive matching, folding correctly
	/// is a table this layer has no business owning, and both parsers preserve
	/// the mapping author's spelling, so tags round-trip byte-for-byte.
	bool operator==(const Term &other) const;
	bool operator!=(const Term &other) const {
		return !(*this == other);
	}

	/// Diagnostic rendering, roughly N-Triples-shaped:
	///     <http://example/x>   _:b0   "text"   "5"^^<...#integer>   "hi"@en
	/// and "(null)" for an absent term.
	///
	/// Deliberately does NOT escape the lexical form: this is for logs, print()
	/// output and test failure messages, matching the print()/operator<<
	/// convention on r2rml::AbstractMap.  It is NOT a serialiser - real output
	/// goes through Serd, which escapes properly.
	std::ostream &print(std::ostream &os) const;

private:
	TermKind kind_ {TermKind::Unknown};
	std::string lexical_;
	std::string datatype_;
	std::string lang_;
};

std::ostream &operator<<(std::ostream &os, const Term &term);

} // namespace rdf
