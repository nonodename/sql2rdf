#pragma once

#include <string>

namespace sparql2sql {

/// What kind of RDF term a column or expression yields, when the R2RML mapping
/// determines that statically.
///
/// Named RdfTermKind rather than TermKind to stay readable alongside
/// sparql::ast::TermKind, which several translator translation units pull in
/// unqualified.
///
/// `Unknown` is the lattice's absorbing element and the default everywhere: it
/// means "this translator cannot prove anything about this term", which is
/// exactly the behaviour every column had before term tracking existed. Every
/// consumer must degrade gracefully to it.
enum class RdfTermKind { Unknown, Iri, BlankNode, Literal };

/// The statically-known RDF term dimension of one column or expression.
///
/// IMPORTANT invariant: this describes the term whose lexical form the
/// generated SQL *actually produces*, not the term SPARQL's abstract semantics
/// would produce. Where the two differ (see inferExprTermInfo), SQL truth wins
/// - otherwise DATATYPE() would report a datatype whose canonical lexical form
/// is not the text sitting in the column.
///
/// An empty `datatypeIri`/`lang` means **"not statically known"**, never "known
/// to be absent by default". A known Literal with an empty datatypeIri is a
/// literal whose datatype the mapping does not pin down (a bare rr:column with
/// no rr:datatype and no TypeCatalog entry): isLITERAL() may fold, but
/// DATATYPE() must still refuse it. Defaulting such a literal to xsd:string
/// would contradict R2RML's own natural mapping, which would have derived a
/// datatype from the column's SQL type.
///
/// A language-tagged literal carries both: `lang` is the tag and `datatypeIri`
/// is rdf:langString.
struct TermInfo {
	RdfTermKind kind = RdfTermKind::Unknown;
	std::string datatypeIri;
	std::string lang;

	/// Sticky across meet(): true if ANY contributing term map is known to be
	/// language-tagged (rr:language), even once a disagreeing sibling (a
	/// different tag, or no tag at all) has degraded datatypeIri/lang back to
	/// "not statically known". Without this, a literal that is @en in one
	/// union arm and untagged in another becomes indistinguishable from a
	/// literal that carries no rr:language anywhere - and LANG() would fold
	/// silently to "" instead of refusing an answer it cannot prove.
	bool maybeLangTagged = false;

	/// Set by meet() whenever any field actually disagreed, and sticky
	/// thereafter. Distinguishes "the mapping declares nothing here" from "the
	/// contributing term maps declare different things", which the three fields
	/// above cannot: meet({Literal, integer}, {Literal, string}) leaves exactly
	/// the same kind/datatype/language as a bare rr:column with no rr:datatype.
	///
	/// Only the runtime lowering needs the difference, and it needs it badly. The
	/// first case has no single honest tag - the datatype varies per row, so the
	/// tag must come from a tag column - whereas the second has one: "L", meaning
	/// "the mapping genuinely does not determine this datatype". Encoding the
	/// first as "L" would tell DATATYPE() to answer "no datatype" for a term that
	/// has a perfectly good one in every row. See isFullyDetermined().
	bool degraded = false;

	bool kindKnown() const {
		return kind != RdfTermKind::Unknown;
	}
	bool isKnownLiteral() const {
		return kind == RdfTermKind::Literal;
	}

	/// A literal whose datatype is known and is one of the XSD numeric types.
	bool isNumeric() const;
	/// A literal whose datatype is known and is xsd:integer or one of its
	/// narrower integral subtypes - the ones for which integral SQL arithmetic
	/// reproduces the correct canonical lexical form.
	bool isIntegral() const;
	/// A literal whose datatype is known and is xsd:date or xsd:dateTime.
	bool isTemporal() const;
	/// A literal known to be xsd:string, or a language-tagged string: the types
	/// for which a bare VARCHAR comparison is exactly right.
	bool isStringy() const;
};

/// Whether this annotation pins the term's dimension down completely enough to
/// be lowered to a single runtime tag by encodeTag().
///
/// False for an Unknown kind, and for the two Literal shapes that record
/// *partial* knowledge rather than a definite answer: rdf:langString with no
/// specific tag (two arms disagreed on rr:language), and no datatype at all but
/// maybeLangTagged set (a tagged arm met an untagged one). Both of those must
/// stay dynamic - a tag column decides them per row - whereas a bare literal
/// with no rr:language anywhere IS fully determined: its tag is kLiteralUntyped,
/// meaning "the mapping genuinely does not know this datatype", which is a
/// definite fact about the mapping and not a partial one.
///
/// Also false for anything `degraded`, which is what makes this safe to call on
/// a meet result rather than only on a single term map's own annotation.
bool isFullyDetermined(const TermInfo &info);

/// Lowering of a fully-determined TermInfo to the single-VARCHAR runtime tag the
/// generated SQL carries beside a variable's lexical form (see mangleVarTag).
/// The encoding is discriminated by the first character:
///
///     "I"        an IRI
///     "B"        a blank node
///     "L"        a literal whose datatype the mapping cannot determine
///     "D<iri>"   a literal with datatype <iri>
///     "@<tag>"   a language-tagged literal (datatype is implicitly rdf:langString)
///
/// SQL NULL - never produced here - is the sixth case, "unbound".
///
/// Returns the empty string when !isFullyDetermined(info): there is no single
/// constant that would be honest, and the caller must obtain the tag from a
/// contributing arm's own tag column instead.
std::string encodeTag(const TermInfo &info);

/// Inverse of encodeTag. An unrecognised or empty tag yields a default
/// (Unknown) TermInfo. Round-trips every fully-determined annotation.
TermInfo decodeTag(const std::string &tag);

/// The three single-character tags, named so no consumer spells them inline.
extern const char *const kTagIri;
extern const char *const kTagBlankNode;
extern const char *const kTagLiteralUntyped;
/// Prefix characters of the two parameterised tags.
extern const char kTagDatatypePrefix;
extern const char kTagLangPrefix;

/// Greatest lower bound of two term annotations. Identical infos meet to
/// themselves; disagreement degrades field by field and never guesses:
///
///     meet({Iri}, {Literal})                        -> {Unknown, "",             ""}
///     meet({Literal, integer}, {Literal, string})   -> {Literal, "",             ""}
///     meet({Literal, langString, "en"}, {..., "fr"})-> {Literal, rdf:langString, ""}
///     meet({Literal, langString, "en"}, {Literal, "", ""})
///                                                    -> {Literal, "", "", maybeLangTagged=true}
///
/// Commutative, associative and idempotent, with a default-constructed
/// TermInfo as the absorbing element. An Unknown kind additionally clears the
/// datatype and language: a term whose kind is not known cannot have a
/// trustworthy datatype either.
///
/// This is the propagation rule for every algebra node whose output column is
/// fed by more than one input: a UNION's arms, the several candidate term maps
/// of a single triple pattern, and an OPTIONAL join's COALESCEd column.
TermInfo meet(const TermInfo &a, const TermInfo &b);

/// Canonical XSD and RDF datatype IRIs, shared by the mapping-side natural
/// mapping inference, the expression translator and the typed consumers.
namespace xsd {

extern const char *const kInteger;
extern const char *const kDecimal;
extern const char *const kDouble;
extern const char *const kFloat;
extern const char *const kString;
extern const char *const kBoolean;
extern const char *const kDateTime;
extern const char *const kDate;
extern const char *const kTime;
extern const char *const kLong;
extern const char *const kInt;
extern const char *const kShort;
extern const char *const kByte;
extern const char *const kHexBinary;

} // namespace xsd

/// rdf:langString - the datatype of every language-tagged literal (RDF 1.1).
extern const char *const kRdfLangString;

bool isXsdNumericIri(const std::string &iri);
bool isXsdIntegralIri(const std::string &iri);
bool isXsdTemporalIri(const std::string &iri);
/// True for the twelve IRIs usable as XSD constructor-function casts.
bool isXsdCastIri(const std::string &iri);

} // namespace sparql2sql
