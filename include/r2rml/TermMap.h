#pragma once

#include "AbstractMap.h"
#include "rdf/Term.h"

#include <string>
#include <memory>
#include <ostream>

#include <serd/serd.h>

namespace r2rml {

class SQLRow;

/**
 * Enumeration of possible term types in R2RML.
 */
enum class TermType { IRI, BlankNode, Literal };

/**
 * Abstract base class representing a term map (subject, predicate, object,
 * or graph).  Subclasses implement specific mapping strategies (constant,
 * column, template, etc.)
 */
class TermMap : public AbstractMap {
public:
	~TermMap() override;

	/**
	 * Produce this term map's RDF term for `row`, into `out`.
	 *
	 * An OUT-PARAMETER rather than a return value, deliberately. This runs once
	 * per row per term map on the bulk-conversion path, so the caller hoists a
	 * single rdf::Term above its row loop and calls out.clear() between rows;
	 * because clear() preserves the string capacity, the loop stops allocating
	 * after the first few rows. Returning by value would reintroduce up to
	 * three allocations per term per row.
	 *
	 * This is also what let the mutable cachedValue_/expanded_ members go: they
	 * existed only to keep a returned SerdNode's borrowed buffer alive, which
	 * made term maps only nominally const and meant a second call silently
	 * invalidated the first call's result. The reused buffer now belongs to the
	 * caller instead.
	 *
	 * Implementations must leave `out` absent (out.clear()) when the term
	 * cannot be produced - a NULL column, or a template with a NULL
	 * substitution - which is the replacement for returning SERD_NODE_NULL.
	 */
	virtual void generateRDFTerm(const SQLRow &row, rdf::Term &out) const = 0;

	/**
	 * Validate that the term map instance has required properties and correct cardinality.
	 * To be overridden by subclasses for specific validation logic.
	 */
	virtual bool isValid() const {
		return true;
	}

	/**
	 * Returns the effective XSD datatype IRI for a literal value derived from
	 * the given row.  If a static rr:datatype is set on this term map it takes
	 * priority; otherwise the base implementation returns empty string.
	 * ColumnTermMap overrides this to fall back to the column value's own
	 * datatype when no static mapping is present.
	 */
	virtual std::string computeDatatypeIRI(const SQLRow &row) const;

	/**
	 * Write a human-readable representation to the given stream.
	 * Subclasses should override this and call TermMap::print for base fields.
	 */
	std::ostream &print(std::ostream &os) const override;

	friend std::ostream &operator<<(std::ostream &os, const TermMap &tm);

	TermType termType {TermType::IRI};
	// optional fields are implemented with unique_ptr for C++11
	std::unique_ptr<std::string> languageTag;
	std::unique_ptr<std::string> datatypeIRI;
	std::unique_ptr<std::string> inverseExpression;
};

} // namespace r2rml
