#pragma once

#include <string>

#include "r2rml/MappingParser.h"

namespace r2rml {

class R2RMLMapping;

/**
 * Resolve `path` to an absolute filesystem path (prefixing the current
 * working directory if it isn't already absolute), so that a subsequent
 * serd_node_new_file_uri() call always produces a proper "file://" URI.
 * serd only recognises a path as absolute (and therefore worth a scheme) if
 * it already starts with a directory separator / drive letter; a relative
 * path like "mapping.ttl" is returned percent-escaped but schemeless, which
 * later fails every write of an IRI resolved against it. Shared by
 * R2RMLParser::parse() and YARRRMLParser::parse(), which both build a
 * document base URI from a caller-supplied mapping file path.
 */
std::string toAbsoluteFilePath(const std::string &path);

/**
 * Parses an R2RML mapping file (typically Turtle) and constructs the
 * corresponding C++ object model.
 */
class R2RMLParser : public MappingParser {
public:
	R2RMLParser();
	~R2RMLParser() override = default;

	/**
	 * Parse the R2RML mapping at `mappingFilePath`.
	 *
	 * @param ignoreNonFatalErrors
	 *   - true  (default): logical parse errors (unresolved parentTriplesMap,
	 *     unrecognised logical-table type, unknown objectMap property) are
	 *     collected silently and stored in R2RMLMapping::parseErrors.  They
	 *     are reported when operator<< is called on the returned mapping.
	 *   - false: the first batch of logical parse errors causes a
	 *     std::runtime_error to be thrown instead.
	 */
	R2RMLMapping parse(const std::string &mappingFilePath, bool ignoreNonFatalErrors = true) override;

	/**
	 * Parse an in-memory Turtle document into the R2RML object model.
	 *
	 * @param turtleText  The Turtle document text.
	 * @param baseUri     Base URI used to resolve relative references (e.g.
	 *                    "<#TriplesMap1>") within `turtleText`.
	 * @param ignoreNonFatalErrors  See parse().
	 */
	R2RMLMapping parseString(const std::string &turtleText, const std::string &baseUri,
	                         bool ignoreNonFatalErrors = true);

	/**
	 * Build the R2RML object model from statements already gathered in
	 * `collector`, without parsing any Turtle text. Intended for callers
	 * (such as YARRRMLParser) that construct SerdNode-based statements
	 * directly rather than serializing and re-parsing Turtle.
	 *
	 * @param ignoreNonFatalErrors  See parse().
	 */
	R2RMLMapping parseCollected(TripleCollector &collector, bool ignoreNonFatalErrors = true);
};

} // namespace r2rml
