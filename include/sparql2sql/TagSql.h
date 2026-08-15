#pragma once

#include <string>

#include "sparql2sql/TermInfo.h"

namespace sparql2sql {

class SqlDialect;

/// SQL builders that inspect a runtime **type tag** - the companion VARCHAR
/// column (see mangleVarTag) carrying an RDF term's kind/datatype/language
/// beside its lexical form, in the encoding encodeTag() produces.
///
/// Every builder in this file takes `tagSql`: an already-valid SQL scalar
/// expression yielding a tag (usually a column reference like `t1."d_x"`, or a
/// constant string literal when the mapping determines the tag statically).
/// Nothing here hand-rolls the encoding at its call site - that is the whole
/// point of the file - and nothing here needs a null guard, because every
/// expression below propagates SQL NULL from an unbound tag outward, which is
/// exactly SPARQL's "unbound argument is a type error" behaviour once a FILTER
/// drops the row / a BIND leaves the variable unbound.
///
/// The `kValueSpace*` ids below classify a tag into the value space its terms
/// compare in. Comparison is defined *within* a space, false-or-error *across*
/// two known spaces, and deliberately undefined for kValueSpaceUnknown, whose
/// operands fall back to the untyped VARCHAR/DOUBLE comparison this translator
/// used before tags existed. That fallback is load-bearing, not a leftover: a
/// bare rr:column with no rr:datatype and no TypeCatalog entry tags as
/// kTagLiteralUntyped, and `FILTER(?price > 10)` over such a column must keep
/// working exactly as it does today.
enum {
	kValueSpaceUnknown = 0, ///< kTagLiteralUntyped, or a datatype outside the table below.
	kValueSpaceNumeric = 1,
	kValueSpaceString = 2, ///< xsd:string only - rdf:langString is its own space.
	kValueSpaceBoolean = 3,
	kValueSpaceTemporal = 4, ///< xsd:date and xsd:dateTime together (compared as timestamps).
	kValueSpaceLangString = 5,
	kValueSpaceIri = 6,
	kValueSpaceBlankNode = 7
};

/// `info`'s tag as a SQL string literal, ready to project as a column - or the
/// empty string when `info` is not fully determined, in which case there is no
/// honest constant and the tag has to come from a contributing arm's own tag
/// column. The one place a constant tag is minted; every ColumnInfo::tagExpr
/// producer goes through here.
std::string tagLiteral(const TermInfo &info, const SqlDialect &dialect);

/// The tag that accompanies `COALESCE(leftValue, rightValue)` - the projection a
/// null-tolerant (OPTIONAL) join uses for a shared variable.
///
/// Deliberately NOT `COALESCE(leftTag, rightTag)`: a tag is very often a
/// *constant* string literal, which is never NULL, so a plain COALESCE would
/// always answer the left tag even on the rows where the value came from the
/// right. Selecting on the *values*' nullness instead reproduces COALESCE's
/// choice exactly, and falling through to NULL when both values are NULL is what
/// keeps "the tag is NULL iff the value is NULL" true across an outer join.
std::string coalescedTag(const std::string &leftValueSql, const std::string &leftTagSql,
                         const std::string &rightValueSql, const std::string &rightTagSql);

/// An integer-valued expression giving `tagSql`'s value space (one of the
/// kValueSpace* ids above), or SQL NULL when the tag is NULL.
std::string tagValueSpace(const std::string &tagSql, const SqlDialect &dialect);

/// A boolean expression testing whether `tagSql` denotes a term of `kind` -
/// the runtime form of isIRI()/isBLANK()/isLITERAL(). `RdfTermKind::Unknown`
/// is not a valid argument.
std::string tagIsKind(const std::string &tagSql, RdfTermKind kind, const SqlDialect &dialect);

/// The datatype IRI of the literal `tagSql` describes, as a string expression:
/// rdf:langString for a language-tagged literal, the declared IRI for a typed
/// one, and SQL NULL for a non-literal *or* for kTagLiteralUntyped (whose
/// datatype the mapping genuinely does not know - DATATYPE() has no answer, and
/// NULL is how this translator spells "error").
std::string tagDatatypeIri(const std::string &tagSql, const SqlDialect &dialect);

/// The language tag of the literal `tagSql` describes: the tag text for a
/// language-tagged literal, the empty string for any other literal (which is
/// LANG()'s correct answer, not an absence of one), and SQL NULL for a
/// non-literal.
std::string tagLang(const std::string &tagSql, const SqlDialect &dialect);

/// A boolean expression testing whether `tagSql` is a literal in the numeric
/// value space - the runtime half of isNUMERIC().
std::string tagIsNumeric(const std::string &tagSql, const SqlDialect &dialect);

/// SPARQL 1.1 Section 15.1's ordering rank across term kinds: unbound (0) <
/// blank node (1) < IRI (2) < literal (3). The first sort key of a
/// dynamically-typed ORDER BY; tagValueSpace() serves as the second, since its
/// ids already group comparable literals together and order them consistently.
std::string tagKindRank(const std::string &tagSql, const SqlDialect &dialect);

/// Compare two terms by RDF term equality / SPARQL value equality, dispatching
/// on both operands' tags at run time. `negated` renders `!=` rather than `=`.
///
/// Within one known value space the comparison is by value (numeric as DOUBLE,
/// temporal as TIMESTAMP, boolean as BOOLEAN, string as VARCHAR); a
/// language-tagged pair additionally requires identical tags; two *different*
/// known value spaces are unequal by RDF term inequality; and if either side is
/// kValueSpaceUnknown the whole thing degrades to `untypedFallback`, which the
/// caller supplies already rendered (the pre-tag numeric-aware comparison).
std::string dynamicEquality(const std::string &leftSql, const std::string &leftTag, const std::string &rightSql,
                            const std::string &rightTag, bool negated, const std::string &untypedFallback,
                            const SqlDialect &dialect);

/// Compare two terms with an ordering operator (`op` is one of < > <= >=),
/// dispatching on both operands' tags at run time.
///
/// SPARQL 1.1 Section 17.3's operator table defines the ordering comparisons
/// only over the numeric, xsd:string, xsd:boolean and date/dateTime value
/// spaces. Anything else - two IRIs, two language-tagged literals, or two
/// different value spaces - is a type error, spelled SQL NULL. As for equality,
/// a kValueSpaceUnknown operand degrades to `untypedFallback` rather than
/// erroring: refusing `FILTER(?name < "M")` over an undeclared column would be
/// a regression, not a conformance win.
std::string dynamicOrdering(const std::string &leftSql, const std::string &leftTag, const std::string &rightSql,
                            const std::string &rightTag, const std::string &op, const std::string &untypedFallback,
                            const SqlDialect &dialect);

/// Numeric-aware untyped comparison: compare as DOUBLE when both sides
/// TRY_CAST successfully, else as plain VARCHAR. This is what every column got
/// before the term dimension existed, and it stays the fallback for any operand
/// whose value space is unknown.
std::string untypedComparison(const std::string &leftSql, const std::string &rightSql, const std::string &op,
                              const SqlDialect &dialect);

} // namespace sparql2sql
