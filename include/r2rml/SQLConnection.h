#pragma once

#include <memory>
#include <string>

namespace r2rml {

class SQLResultSet;

/**
 * Pure abstract interface representing a relational database connection.
 * Implement this interface to plug in a concrete database backend.
 */
class SQLConnection {
public:
	virtual ~SQLConnection() = default;

	/**
	 * Execute a query and return a result set.  Caller owns the returned
	 * pointer.
	 */
	virtual std::unique_ptr<SQLResultSet> execute(const std::string &sqlQuery) = 0;

	/** Returns the default catalog name, or empty string if not applicable. */
	virtual std::string getDefaultCatalog() {
		return {};
	}

	/** Returns the default schema name, or empty string if not applicable. */
	virtual std::string getDefaultSchema() {
		return {};
	}
};

} // namespace r2rml
