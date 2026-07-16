#pragma once

#include <string>
#include <memory>

#include <serd/serd.h>

namespace r2rml {

class R2RMLMapping;

/**
 * Collects RDF statements into an internal triple store, independent of
 * where those statements came from. Both R2RMLParser's own Turtle-parsing
 * paths and callers that construct SerdNodes directly (e.g. YARRRMLParser,
 * translating YARRRML without going through Turtle text) feed statements
 * into a TripleCollector; R2RMLParser::parseCollected() then builds the
 * R2RML object model from it exactly as it would from a parsed file.
 */
class TripleCollector {
public:
	TripleCollector();
	~TripleCollector();

	TripleCollector(const TripleCollector &) = delete;
	TripleCollector &operator=(const TripleCollector &) = delete;

	/// Set the document base URI, used to resolve relative references.
	void setBase(const SerdNode *base);

	/// Declare a Turtle prefix, used to expand CURIEs in later statements.
	void setPrefix(const SerdNode *name, const SerdNode *uri);

	/// Add one statement. `objectDatatype`/`objectLang` are only meaningful
	/// when `object` is a literal.
	void statement(const SerdNode *subject, const SerdNode *predicate, const SerdNode *object,
	               const SerdNode *objectDatatype = nullptr, const SerdNode *objectLang = nullptr);

	/// Record a non-fatal error (e.g. a translation warning from a caller
	/// building statements directly) to be merged with build-phase errors.
	void addError(const std::string &message);

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;

	friend class R2RMLParser;
};

/**
 * Parses an R2RML mapping file (typically Turtle) and constructs the
 * corresponding C++ object model.
 */
class R2RMLParser {
public:
	R2RMLParser();
	~R2RMLParser() = default;

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
	R2RMLMapping parse(const std::string &mappingFilePath, bool ignoreNonFatalErrors = true);

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
