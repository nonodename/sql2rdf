#include "r2rml/SQLConnection.h"

namespace r2rml {

SQLConnection::~SQLConnection() = default;
std::string SQLConnection::getDefaultCatalog() { return std::string(); }
std::string SQLConnection::getDefaultSchema() { return std::string(); }

} // namespace r2rml
