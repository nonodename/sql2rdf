#pragma once

#include <memory>
#include <string>
#include <memory>

namespace r2rml {

class SQLResultSet;

/**
 * Generic interface representing a relational database connection.
 */
class SQLConnection {
public:
    virtual ~SQLConnection();

    /**
     * Execute a query and return a result set.  Caller owns the returned
     * pointer.
     */
    virtual std::unique_ptr<SQLResultSet>
    execute(const std::string& sqlQuery) = 0;

    virtual std::string getDefaultCatalog();
    virtual std::string getDefaultSchema();
};

} // namespace r2rml
