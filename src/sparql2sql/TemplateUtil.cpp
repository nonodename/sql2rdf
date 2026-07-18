#include "sparql2sql/TemplateUtil.h"

#include "sparql2sql/SqlDialect.h"

namespace sparql2sql {

namespace {

bool startsWithAt(const std::string &haystack, std::size_t pos, const std::string &needle) {
	if (pos > haystack.size()) {
		return false;
	}
	return haystack.compare(pos, needle.size(), needle) == 0;
}

bool isHexDigit(char c) {
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

int hexVal(char c) {
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return 10 + (c - 'a');
	}
	return 10 + (c - 'A');
}

} // namespace

std::vector<TemplateSegment> parseTemplate(const std::string &templateString) {
	std::vector<TemplateSegment> segments;
	std::string literalBuf;
	std::size_t i = 0;
	const std::size_t n = templateString.size();

	while (i < n) {
		if (templateString[i] == '{') {
			std::size_t end = templateString.find('}', i + 1);
			if (end == std::string::npos) {
				// Matches TemplateTermMap.cpp: an unmatched '{' truncates
				// the rest of the template rather than being treated as
				// literal text.
				break;
			}
			if (!literalBuf.empty()) {
				TemplateSegment seg;
				seg.isPlaceholder = false;
				seg.text = literalBuf;
				segments.push_back(seg);
				literalBuf.clear();
			}
			TemplateSegment seg;
			seg.isPlaceholder = true;
			seg.text = templateString.substr(i + 1, end - i - 1);
			segments.push_back(seg);
			i = end + 1;
		} else {
			literalBuf += templateString[i];
			++i;
		}
	}
	if (!literalBuf.empty()) {
		TemplateSegment seg;
		seg.isPlaceholder = false;
		seg.text = literalBuf;
		segments.push_back(seg);
	}
	return segments;
}

std::vector<std::string> referencedColumns(const std::vector<TemplateSegment> &segments) {
	std::vector<std::string> out;
	for (const auto &seg : segments) {
		if (!seg.isPlaceholder) {
			continue;
		}
		bool seen = false;
		for (const auto &c : out) {
			if (c == seg.text) {
				seen = true;
				break;
			}
		}
		if (!seen) {
			out.push_back(seg.text);
		}
	}
	return out;
}

std::string buildProjectionSql(const std::vector<TemplateSegment> &segments, const std::string &sourceAlias,
                               const SqlDialect &dialect) {
	if (segments.empty()) {
		return dialect.stringLiteral("");
	}
	std::vector<std::string> parts;
	parts.reserve(segments.size());
	for (const auto &seg : segments) {
		if (seg.isPlaceholder) {
			parts.push_back("CAST(" + sourceAlias + "." + dialect.quoteIdentifier(seg.text) + " AS VARCHAR)");
		} else {
			parts.push_back(dialect.stringLiteral(seg.text));
		}
	}
	return dialect.concat(parts);
}

InversionOutcome invertTemplate(const std::vector<TemplateSegment> &segments, const std::string &boundValue) {
	InversionOutcome outcome;

	if (segments.empty()) {
		// An empty template only "matches" an empty bound value.
		if (boundValue.empty()) {
			outcome.kind = InversionKind::WholeTemplateMatch;
		} else {
			outcome.kind = InversionKind::NeverMatches;
		}
		return outcome;
	}

	bool canSplit = true;
	for (std::size_t i = 0; i + 1 < segments.size(); ++i) {
		if (segments[i].isPlaceholder && segments[i + 1].isPlaceholder) {
			canSplit = false;
			break;
		}
	}

	if (!canSplit) {
		// Still prune using the leading/trailing literal anchors, if any.
		if (!segments.front().isPlaceholder && !startsWithAt(boundValue, 0, segments.front().text)) {
			outcome.kind = InversionKind::NeverMatches;
			return outcome;
		}
		if (!segments.back().isPlaceholder) {
			const std::string &suffix = segments.back().text;
			if (boundValue.size() < suffix.size() ||
			    boundValue.compare(boundValue.size() - suffix.size(), suffix.size(), suffix) != 0) {
				outcome.kind = InversionKind::NeverMatches;
				return outcome;
			}
		}
		outcome.kind = InversionKind::WholeTemplateMatch;
		return outcome;
	}

	std::size_t pos = 0;
	for (std::size_t i = 0; i < segments.size(); ++i) {
		const TemplateSegment &seg = segments[i];
		if (!seg.isPlaceholder) {
			if (!startsWithAt(boundValue, pos, seg.text)) {
				outcome.kind = InversionKind::NeverMatches;
				return outcome;
			}
			pos += seg.text.size();
			continue;
		}

		// Placeholder: canSplit guarantees the next segment (if any) is
		// literal, so it's safe to search for it as this placeholder's
		// ending delimiter.
		std::string span;
		if (i + 1 < segments.size()) {
			const std::string &delimiter = segments[i + 1].text;
			std::size_t found = boundValue.find(delimiter, pos);
			if (found == std::string::npos) {
				outcome.kind = InversionKind::NeverMatches;
				return outcome;
			}
			span = boundValue.substr(pos, found - pos);
			pos = found;
		} else {
			span = boundValue.substr(pos);
			pos = boundValue.size();
		}
		outcome.columnValues.emplace_back(seg.text, percentDecode(span));
	}

	if (pos != boundValue.size()) {
		// Trailing characters left over that the template never accounted
		// for.
		outcome.kind = InversionKind::NeverMatches;
		outcome.columnValues.clear();
		return outcome;
	}

	outcome.kind = InversionKind::PerColumnMatch;
	return outcome;
}

std::string percentDecode(const std::string &value) {
	std::string out;
	out.reserve(value.size());
	for (std::size_t i = 0; i < value.size(); ++i) {
		if (value[i] == '%' && i + 2 < value.size() && isHexDigit(value[i + 1]) && isHexDigit(value[i + 2])) {
			out += static_cast<char>(hexVal(value[i + 1]) * 16 + hexVal(value[i + 2]));
			i += 2;
		} else {
			out += value[i];
		}
	}
	return out;
}

} // namespace sparql2sql
