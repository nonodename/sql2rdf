#include "sparql2sql/TagSql.h"

#include <stdexcept>
#include <vector>

#include "sparql2sql/SqlDialect.h"

namespace sparql2sql {

namespace {

/// The tag text for a literal of datatype `iri`, as a SQL string literal.
std::string datatypeTagLiteral(const char *iri, const SqlDialect &dialect) {
	return dialect.stringLiteral(kTagDatatypePrefix + std::string(iri));
}

/// `<tagSql> IN (<'D'+iri>, ...)` over a set of datatype IRIs.
std::string tagInDatatypes(const std::string &tagSql, const std::vector<const char *> &iris,
                           const SqlDialect &dialect) {
	std::string sql = tagSql + " IN (";
	for (std::size_t i = 0; i < iris.size(); ++i) {
		sql += (i > 0 ? ", " : "");
		sql += datatypeTagLiteral(iris[i], dialect);
	}
	return sql + ")";
}

/// `SUBSTR(<tagSql>, 1, 1) = '<c>'` - a first-character test. Deliberately not
/// LIKE/STARTS_WITH: a single-character SUBSTR comparison is the most portable
/// spelling of this, and it sidesteps any question of wildcards in tag text.
std::string tagStartsWith(const std::string &tagSql, char c, const SqlDialect &dialect) {
	return "SUBSTR(" + tagSql + ", 1, 1) = " + dialect.stringLiteral(std::string(1, c));
}

std::string intLiteral(int value) {
	return std::to_string(value);
}

} // namespace

std::string tagLiteral(const TermInfo &info, const SqlDialect &dialect) {
	const std::string tag = encodeTag(info);
	return tag.empty() ? std::string() : dialect.stringLiteral(tag);
}

std::string coalescedTag(const std::string &leftValueSql, const std::string &leftTagSql,
                         const std::string &rightValueSql, const std::string &rightTagSql) {
	return "(CASE WHEN " + leftValueSql + " IS NOT NULL THEN " + leftTagSql + " WHEN " + rightValueSql +
	       " IS NOT NULL THEN " + rightTagSql + " ELSE NULL END)";
}

std::string tagValueSpace(const std::string &tagSql, const SqlDialect &dialect) {
	const std::vector<const char *> numeric = {xsd::kInteger, xsd::kLong,    xsd::kInt,    xsd::kShort,
	                                           xsd::kByte,    xsd::kDecimal, xsd::kDouble, xsd::kFloat};
	const std::vector<const char *> temporal = {xsd::kDate, xsd::kDateTime};
	return "(CASE WHEN " + tagSql + " IS NULL THEN NULL" +                                                      //
	       " WHEN " + tagSql + " = " + dialect.stringLiteral(kTagIri) + " THEN " + intLiteral(kValueSpaceIri) + //
	       " WHEN " + tagSql + " = " + dialect.stringLiteral(kTagBlankNode) + " THEN " +
	       intLiteral(kValueSpaceBlankNode) +                                                                         //
	       " WHEN " + tagStartsWith(tagSql, kTagLangPrefix, dialect) + " THEN " + intLiteral(kValueSpaceLangString) + //
	       " WHEN " + tagInDatatypes(tagSql, numeric, dialect) + " THEN " + intLiteral(kValueSpaceNumeric) +          //
	       " WHEN " + tagSql + " = " + datatypeTagLiteral(xsd::kString, dialect) + " THEN " +
	       intLiteral(kValueSpaceString) + //
	       " WHEN " + tagSql + " = " + datatypeTagLiteral(xsd::kBoolean, dialect) + " THEN " +
	       intLiteral(kValueSpaceBoolean) +                                                                    //
	       " WHEN " + tagInDatatypes(tagSql, temporal, dialect) + " THEN " + intLiteral(kValueSpaceTemporal) + //
	       " ELSE " + intLiteral(kValueSpaceUnknown) + " END)";
}

std::string tagIsKind(const std::string &tagSql, RdfTermKind kind, const SqlDialect &dialect) {
	switch (kind) {
	case RdfTermKind::Iri:
		return "(" + tagSql + " = " + dialect.stringLiteral(kTagIri) + ")";
	case RdfTermKind::BlankNode:
		return "(" + tagSql + " = " + dialect.stringLiteral(kTagBlankNode) + ")";
	case RdfTermKind::Literal:
		// Everything that is not one of the two single-character non-literal
		// tags. NOT IN propagates NULL for an unbound tag, which is what an
		// isLITERAL() over an unbound term should yield.
		return "(" + tagSql + " NOT IN (" + dialect.stringLiteral(kTagIri) + ", " +
		       dialect.stringLiteral(kTagBlankNode) + "))";
	case RdfTermKind::Unknown:
		break;
	}
	throw std::logic_error("tagIsKind: RdfTermKind::Unknown is not a testable kind");
}

std::string tagDatatypeIri(const std::string &tagSql, const SqlDialect &dialect) {
	// kTagLiteralUntyped and the two non-literal tags all fall through to NULL:
	// a literal whose datatype the mapping cannot determine has no answer to
	// give, and guessing xsd:string would contradict R2RML's own natural
	// mapping. NULL is how this translator spells a type error.
	return "(CASE WHEN " + tagStartsWith(tagSql, kTagLangPrefix, dialect) + " THEN " +
	       dialect.stringLiteral(kRdfLangString) + " WHEN " + tagStartsWith(tagSql, kTagDatatypePrefix, dialect) +
	       " THEN SUBSTR(" + tagSql + ", 2) ELSE NULL END)";
}

std::string tagLang(const std::string &tagSql, const SqlDialect &dialect) {
	// A literal with no language tag has the empty tag - a known answer, not an
	// unknown one - so kTagLiteralUntyped and any D<iri> literal both yield ''.
	// Only a non-literal is an error.
	return "(CASE WHEN " + tagStartsWith(tagSql, kTagLangPrefix, dialect) + " THEN SUBSTR(" + tagSql + ", 2) WHEN " +
	       tagIsKind(tagSql, RdfTermKind::Literal, dialect) + " THEN " + dialect.stringLiteral("") + " ELSE NULL END)";
}

std::string tagIsNumeric(const std::string &tagSql, const SqlDialect &dialect) {
	return "(" + tagValueSpace(tagSql, dialect) + " = " + intLiteral(kValueSpaceNumeric) + ")";
}

std::string tagKindRank(const std::string &tagSql, const SqlDialect &dialect) {
	return "(CASE WHEN " + tagSql + " IS NULL THEN 0 WHEN " + tagSql + " = " + dialect.stringLiteral(kTagBlankNode) +
	       " THEN 1 WHEN " + tagSql + " = " + dialect.stringLiteral(kTagIri) + " THEN 2 ELSE 3 END)";
}

std::string untypedComparison(const std::string &leftSql, const std::string &rightSql, const std::string &op,
                              const SqlDialect &dialect) {
	const std::string leftNum = dialect.tryCastToDouble(leftSql);
	const std::string rightNum = dialect.tryCastToDouble(rightSql);
	return "(CASE WHEN " + leftNum + " IS NOT NULL AND " + rightNum + " IS NOT NULL THEN (" + leftNum + " " + op + " " +
	       rightNum + ") ELSE (" + leftSql + " " + op + " " + rightSql + ") END)";
}

namespace {

/// The shared prefix of both dynamic comparisons: NULL for an unbound operand,
/// the untyped fallback when either side's value space is unknown.
std::string dispatchPrefix(const std::string &leftSpace, const std::string &rightSpace,
                           const std::string &untypedFallback) {
	return "(CASE WHEN " + leftSpace + " IS NULL OR " + rightSpace + " IS NULL THEN NULL WHEN " + leftSpace + " = " +
	       intLiteral(kValueSpaceUnknown) + " OR " + rightSpace + " = " + intLiteral(kValueSpaceUnknown) + " THEN " +
	       untypedFallback;
}

/// The three by-value branches shared by equality and ordering: numeric as
/// DOUBLE, temporal as TIMESTAMP, boolean as BOOLEAN. Reached only once both
/// operands are known to share `space`.
///
/// TRY_CAST throughout, never CAST: a declared rr:datatype can lie about a dirty
/// column, and a hard cast error would kill the whole query where TRY_CAST
/// yields NULL and merely drops the row. xsd:date and xsd:dateTime share one
/// space and both compare as TIMESTAMP - a date widens to midnight, so the
/// mixed case is correct without a separate branch.
std::string typedValueBranches(const std::string &space, const std::string &leftSql, const std::string &rightSql,
                               const std::string &op, const SqlDialect &dialect) {
	return " WHEN " + space + " = " + intLiteral(kValueSpaceNumeric) + " THEN (" + dialect.tryCastToDouble(leftSql) +
	       " " + op + " " + dialect.tryCastToDouble(rightSql) + ")" + //
	       " WHEN " + space + " = " + intLiteral(kValueSpaceTemporal) + " THEN (" +
	       dialect.tryCastToTimestamp(leftSql) + " " + op + " " + dialect.tryCastToTimestamp(rightSql) + ")" + //
	       " WHEN " + space + " = " + intLiteral(kValueSpaceBoolean) + " THEN (" + dialect.tryCastToBoolean(leftSql) +
	       " " + op + " " + dialect.tryCastToBoolean(rightSql) + ")";
}

} // namespace

std::string dynamicEquality(const std::string &leftSql, const std::string &leftTag, const std::string &rightSql,
                            const std::string &rightTag, bool negated, const std::string &untypedFallback,
                            const SqlDialect &dialect) {
	const std::string ls = tagValueSpace(leftTag, dialect);
	const std::string rs = tagValueSpace(rightTag, dialect);
	const std::string op = negated ? "<>" : "=";
	// Two terms in different (known) value spaces are never the same RDF term,
	// so equality is a definite FALSE rather than a type error.
	const std::string acrossSpaces = dialect.booleanLiteral(negated);
	// A language-tagged pair must agree on the tag as well as the lexical form;
	// negation is over the conjunction, so "a"@en != "a"@fr is true.
	const std::string sameLangTerm = "(" + leftTag + " = " + rightTag + " AND " + leftSql + " = " + rightSql + ")";
	const std::string langBranch = negated ? ("(NOT " + sameLangTerm + ")") : sameLangTerm;
	return dispatchPrefix(ls, rs, untypedFallback) +                                           //
	       " WHEN " + ls + " <> " + rs + " THEN " + acrossSpaces +                             //
	       typedValueBranches(ls, leftSql, rightSql, op, dialect) +                            //
	       " WHEN " + ls + " = " + intLiteral(kValueSpaceLangString) + " THEN " + langBranch + //
	       // Remaining shared spaces - xsd:string, IRI, blank node - are all
	       // compared as their lexical text, which for terms of one kind is
	       // exactly RDF term equality.
	       " ELSE (" + leftSql + " " + op + " " + rightSql + ") END)";
}

std::string dynamicOrdering(const std::string &leftSql, const std::string &leftTag, const std::string &rightSql,
                            const std::string &rightTag, const std::string &op, const std::string &untypedFallback,
                            const SqlDialect &dialect) {
	const std::string ls = tagValueSpace(leftTag, dialect);
	const std::string rs = tagValueSpace(rightTag, dialect);
	return dispatchPrefix(ls, rs, untypedFallback) +
	       // Different value spaces, and the spaces Section 17.3's operator table
	       // leaves out (rdf:langString, IRI, blank node), are all type errors for
	       // an ordering comparison - unlike equality, where they are simply false.
	       " WHEN " + ls + " <> " + rs + " THEN NULL" +                        //
	       typedValueBranches(ls, leftSql, rightSql, op, dialect) +            //
	       " WHEN " + ls + " = " + intLiteral(kValueSpaceString) + " THEN (" + //
	       leftSql + " " + op + " " + rightSql + ") ELSE NULL END)";
}

} // namespace sparql2sql
