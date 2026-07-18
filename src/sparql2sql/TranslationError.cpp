#include "sparql2sql/TranslationError.h"

namespace sparql2sql {

TranslationError::TranslationError(const std::string &message) : std::runtime_error(message) {
}

} // namespace sparql2sql
