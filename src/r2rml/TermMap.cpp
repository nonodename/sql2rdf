#include "r2rml/TermMap.h"

#include <ostream>

namespace r2rml {

TermMap::~TermMap() = default;

static const char* termTypeName(TermType t) {
    switch (t) {
        case TermType::IRI:       return "IRI";
        case TermType::BlankNode: return "BlankNode";
        case TermType::Literal:   return "Literal";
    }
    return "?";
}

std::ostream& TermMap::print(std::ostream& os) const {
    os << "termType=" << termTypeName(termType);
    if (languageTag) os << " lang=\""     << *languageTag << '"';
    if (datatypeIRI) os << " datatype=\"" << *datatypeIRI << '"';
    return os;
}

std::ostream& operator<<(std::ostream& os, const TermMap& tm) {
    return tm.print(os);
}

} // namespace r2rml
