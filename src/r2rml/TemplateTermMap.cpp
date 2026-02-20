#include "r2rml/TemplateTermMap.h"

namespace r2rml {

TemplateTermMap::TemplateTermMap(const std::string& templ)
    : templateString(templ) {}
TemplateTermMap::~TemplateTermMap() = default;
SerdNode TemplateTermMap::generateRDFTerm(const SQLRow&,
                                          const SerdEnv&) const {
    return SerdNode{0};
}

} // namespace r2rml
