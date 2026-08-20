#pragma once

#include <string>
#include <utility>
#include <vector>

namespace sparql2sql {

class SqlDialect;

/// One segment of a parsed rr:template string: either literal text copied
/// verbatim, or a {columnName} placeholder.
struct TemplateSegment {
	bool isPlaceholder;
	std::string text; // literal text, or (if isPlaceholder) the column name
};

/// Parse an rr:template string into alternating literal/placeholder
/// segments. Mirrors the exact scanning algorithm in
/// r2rml::TemplateTermMap::generateRDFTerm (src/r2rml/TemplateTermMap.cpp)
/// byte-for-byte, INCLUDING its surprising unmatched-'{' behavior: despite
/// that function's own comment ("treat rest as literal"), the code actually
/// `break`s on an unmatched '{', silently truncating everything from that
/// '{' onward rather than emitting it as literal text. Mirrored here so
/// inversion/projection stay consistent with what forward generation
/// actually produces.
std::vector<TemplateSegment> parseTemplate(const std::string &templateString);

/// The distinct column names referenced by any placeholder segment, in
/// first-occurrence order.
std::vector<std::string> referencedColumns(const std::vector<TemplateSegment> &segments);

/// Build a SQL expression that reconstructs the template's string value from
/// the given source alias's columns (string concatenation of literal text
/// and, when percentEncodeValues is true, percent-encoded CAST(...AS VARCHAR)
/// column references). Matches what forward R2RML generation produces
/// (r2rml::AbstractMap::percentEncode): R2RML 7.3 percent-encodes substituted
/// template values only for an rr:IRI term map, so callers must pass
/// percentEncodeValues = (termMap.termType == r2rml::TermType::IRI).
std::string buildProjectionSql(const std::vector<TemplateSegment> &segments, const std::string &sourceAlias,
                               const SqlDialect &dialect, bool percentEncodeValues);

enum class InversionKind { NeverMatches, PerColumnMatch, WholeTemplateMatch };

struct InversionOutcome {
	InversionKind kind = InversionKind::NeverMatches;
	// column -> percent-decoded value; only populated for PerColumnMatch.
	std::vector<std::pair<std::string, std::string>> columnValues;
};

/// Determine what, if anything, a bound term's lexical string implies about
/// the columns referenced by a template's placeholders.
///  - NeverMatches: boundValue cannot possibly have been produced by this
///    template (a literal segment doesn't fit) - the caller should discard
///    the whole candidate rather than generate SQL for it.
///  - PerColumnMatch: no two placeholders are textually adjacent (every
///    placeholder is delimited by a literal segment, or is the first/last
///    segment), so boundValue splits unambiguously; columnValues holds one
///    (column, percent-decoded value) pair per placeholder.
///  - WholeTemplateMatch: splitting is ambiguous (adjacent placeholders with
///    no delimiter between them) but the template may still match as a
///    whole; the caller should emit "WHERE buildProjectionSql(...) =
///    '<boundValue>'" instead of per-column equalities. Always correct,
///    just less indexable than PerColumnMatch - a fallback over it, not a
///    separate correctness mechanism.
///
/// Known simplification: a literal delimiter composed entirely of
/// RFC3986-unreserved characters (e.g. "-") could in principle also appear
/// inside a percent-encoded placeholder's own span, making the split
/// theoretically ambiguous even when no two placeholders are textually
/// adjacent. This case is not detected; PerColumnMatch is used whenever
/// segments simply alternate, which is correct for the common case of
/// non-unreserved delimiters (e.g. "/", ":", "=").
///
/// percentDecodeValues must agree with the term map's rr:termType, mirroring
/// buildProjectionSql above: only an rr:IRI template percent-encodes
/// substituted values in forward generation, so only for rr:IRI should the
/// extracted spans be percent-decoded here.
InversionOutcome invertTemplate(const std::vector<TemplateSegment> &segments, const std::string &boundValue,
                                bool percentDecodeValues);

/// RFC3986 percent-decode. The inverse of r2rml::TemplateTermMap.cpp's
/// translation-unit-local percentEncode() helper; re-implemented here since
/// that helper isn't exported and sparql2sql shouldn't reach into r2rml's
/// internals.
std::string percentDecode(const std::string &value);

/// Whether two rr:template strings could ever produce equal lexical values,
/// decided purely from their fixed literal anchors (the text before the first
/// placeholder and after the last one) - the same anchor-only technique
/// invertTemplate uses when adjacent placeholders make a full per-column split
/// impossible. Used to prune UNION branches that a join can provably never
/// match: if two candidate mappings' subject (or object) templates have
/// incompatible leading/trailing literals - e.g. "ex:data/PROD{code}" vs
/// "ex:data/CAT{code}" - no substitution of their placeholders can ever make
/// the two produced strings equal.
///
/// Returns true (i.e. "cannot prove incompatible, assume compatible") unless
/// the mismatch is certain, so a false positive here can only miss a pruning
/// opportunity, never drop a branch that could actually have matched.
///
/// Known simplification: only the outermost literal segments are compared: two
/// templates that agree on both anchors but differ in an interior literal
/// (e.g. "A{x}B{y}C" vs "A{x}Z{y}C") are not detected as incompatible. This
/// mirrors the "known simplification" already accepted by invertTemplate above
/// rather than introducing a new correctness posture.
bool templatesCanEverMatch(const std::string &templateA, const std::string &templateB);

/// Whether two rr:template strings have the same literal/placeholder
/// "shape": the same number of segments, in the same order, with every
/// literal segment byte-identical and every placeholder position aligned -
/// but placeholder *names* may differ, and a repeated placeholder in one
/// template (e.g. "{x}/{x}") must be repeated at exactly the same positions
/// in the other (e.g. "{y}/{y}").
///
/// Used by the native-join-key rewrite to recognize joins like
/// "ex:data/PROD/{PROD_CODE}" = "ex:data/PROD/{ITEM_PROD_CODE}" as reducible
/// to a plain column equality PROD_CODE = ITEM_PROD_CODE, without requiring
/// the placeholder column to be named identically on both sides.
///
/// A true result implies referencedColumns() on the two parsed templates
/// produces same-length, positionally-corresponding vectors: callers may
/// zip them directly.
bool sameTemplateShape(const std::string &templateA, const std::string &templateB);

} // namespace sparql2sql
