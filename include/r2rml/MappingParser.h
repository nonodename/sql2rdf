#pragma once

#include <memory>
#include <string>

#include <serd/serd.h>

namespace r2rml {

class R2RMLMapping;

/**
 * R2RML vocabulary URIs shared by every MappingParser implementation
 * (R2RMLParser, YARRRMLParser, ...). Plain `const char *const` rather than
 * `std::string`: at namespace scope these get internal linkage (implicit
 * `const`), so each translation unit that includes this header gets its own
 * copy with no ODR/link concerns, and — unlike std::string globals — they
 * have trivial constant initialization, so they don't trip
 * cppcoreguidelines-interfaces-global-init or static-init-order warnings.
 */
namespace vocab {
const char *const RR_TERM_TYPE = "http://www.w3.org/ns/r2rml#termType";
const char *const RR_IRI_TERM_TYPE = "http://www.w3.org/ns/r2rml#IRI";
const char *const RR_LITERAL_TERM_TYPE = "http://www.w3.org/ns/r2rml#Literal";
const char *const RR_BLANKNODE_TERM_TYPE = "http://www.w3.org/ns/r2rml#BlankNode";
const char *const RR_LANGUAGE = "http://www.w3.org/ns/r2rml#language";
const char *const RR_TABLE_NAME = "http://www.w3.org/ns/r2rml#tableName";
const char *const RR_LOGICAL_TABLE = "http://www.w3.org/ns/r2rml#logicalTable";
const char *const RR_SQL_QUERY = "http://www.w3.org/ns/r2rml#sqlQuery";
const char *const RR_DATATYPE = "http://www.w3.org/ns/r2rml#datatype";
const char *const RR_TEMPLATE = "http://www.w3.org/ns/r2rml#template";
const char *const RR_COLUMN = "http://www.w3.org/ns/r2rml#column";
const char *const RR_CONSTANT = "http://www.w3.org/ns/r2rml#constant";
const char *const RR_PARENTTRIPLESMAP = "http://www.w3.org/ns/r2rml#parentTriplesMap";
const char *const RR_JOIN_CONDITION = "http://www.w3.org/ns/r2rml#joinCondition";
const char *const RR_CHILD = "http://www.w3.org/ns/r2rml#child";
const char *const RR_PARENT = "http://www.w3.org/ns/r2rml#parent";
const char *const RR_CLASS = "http://www.w3.org/ns/r2rml#class";
const char *const RR_PREDICATE = "http://www.w3.org/ns/r2rml#predicate";
const char *const RR_PREDICATE_MAP = "http://www.w3.org/ns/r2rml#predicateMap";
const char *const RR_OBJECT = "http://www.w3.org/ns/r2rml#object";
const char *const RR_OBJECT_MAP = "http://www.w3.org/ns/r2rml#objectMap";
const char *const RR_SUBJECT = "http://www.w3.org/ns/r2rml#subject";
const char *const RR_SUBJECT_MAP = "http://www.w3.org/ns/r2rml#subjectMap";
const char *const RR_PREDICATE_OBJECT_MAP = "http://www.w3.org/ns/r2rml#predicateObjectMap";
} // namespace vocab

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
	// Not defaulted here: Impl is only forward-declared, so the destructor
	// must be defined where Impl is complete (R2RMLParser.cpp).
	~TripleCollector(); // NOLINT(performance-trivially-destructible)

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
 * Common interface implemented by every mapping format parser (R2RML
 * Turtle, YARRRML, ...): each translates some on-disk mapping document into
 * the shared R2RML object model.
 */
class MappingParser {
public:
	virtual ~MappingParser() = default;

	/**
	 * Parse the mapping document at `mappingFilePath`.
	 *
	 * @param ignoreNonFatalErrors
	 *   - true  (default): non-fatal parse issues are collected silently and
	 *     stored in R2RMLMapping::parseErrors.
	 *   - false: the first batch of non-fatal issues causes a
	 *     std::runtime_error to be thrown instead.
	 */
	virtual R2RMLMapping parse(const std::string &mappingFilePath, bool ignoreNonFatalErrors = true) = 0;

	/**
	 * Instantiate the parser appropriate for `mappingFilePath`'s extension
	 * (".ttl" -> R2RMLParser, ".yml"/".yaml"/".yarrrml" -> YARRRMLParser).
	 *
	 * Defined in the yarrrml layer (src/yarrrml/MappingParserFactory.cpp),
	 * since the core r2rml library must not depend on YARRRML/yaml-cpp:
	 * calling this requires linking sql2rdf_yarrrml even if the resolved
	 * format turns out to be R2RML.
	 *
	 * @throws std::runtime_error if no known format matches the extension.
	 */
	static std::unique_ptr<MappingParser> create(const std::string &mappingFilePath);
};

} // namespace r2rml
