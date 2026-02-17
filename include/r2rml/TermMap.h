#pragma once

#include <optional>
#include <string>

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
class TermMap {
public:
    virtual ~TermMap();

    /**
     * Given a row and a Serd environment, produce an RDF term as a SerdNode.
     */
    virtual SerdNode generateRDFTerm(const SQLRow& row,
                                     const SerdEnv& env) const = 0;

    TermType termType{TermType::IRI};
    // optional fields are implemented with unique_ptr for C++11
    std::unique_ptr<std::string> languageTag;
    std::unique_ptr<std::string> datatypeIRI;
    std::unique_ptr<std::string> inverseExpression;
};

} // namespace r2rml
