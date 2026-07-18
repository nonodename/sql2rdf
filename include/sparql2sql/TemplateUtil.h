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
/// and CAST(...AS VARCHAR) column references). V1 does not percent-encode
/// substituted column values (documented limitation: assumes
/// template-referenced columns hold only RFC3986-unreserved characters).
std::string buildProjectionSql(const std::vector<TemplateSegment> &segments, const std::string &sourceAlias,
                               const SqlDialect &dialect);

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
InversionOutcome invertTemplate(const std::vector<TemplateSegment> &segments, const std::string &boundValue);

/// RFC3986 percent-decode. The inverse of r2rml::TemplateTermMap.cpp's
/// translation-unit-local percentEncode() helper; re-implemented here since
/// that helper isn't exported and sparql2sql shouldn't reach into r2rml's
/// internals.
std::string percentDecode(const std::string &value);

} // namespace sparql2sql
