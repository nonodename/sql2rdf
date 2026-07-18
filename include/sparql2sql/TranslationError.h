#pragma once

#include <stdexcept>
#include <string>

namespace sparql2sql {

/// Thrown for any SPARQL construct or expression that the translator cannot
/// express as SQL against the supplied R2RML mapping (e.g. an unsupported
/// property path, GRAPH/SERVICE, or an out-of-scope FILTER builtin).
/// Derives from std::runtime_error so existing `catch (const
/// std::exception &)` call sites keep working unchanged.
class TranslationError : public std::runtime_error {
public:
	explicit TranslationError(const std::string &message);
};

} // namespace sparql2sql
