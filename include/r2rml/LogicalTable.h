#pragma once

#include <memory>
#include <ostream>
#include <string>
#include <vector>

namespace r2rml {

class SQLConnection;
class SQLResultSet;

/**
 * Abstract base representing a logical table in an R2RML mapping.  A logical
 * table can be backed by a plain table, view or arbitrary SQL query.
 */
class LogicalTable {
public:
    virtual ~LogicalTable();

    /**
     * Execute the configured SQL against the provided connection and return
     * a result set object.  Caller takes ownership of the returned pointer.
     */
    virtual std::unique_ptr<SQLResultSet> getRows(SQLConnection& dbConnection) = 0;

    /**
     * Return a list of column names that will be available when iterating rows
     * from this logical table.  This may require introspecting the query.
     */
    virtual std::vector<std::string> getColumnNames() = 0;

    /**
     * Return true if this logical table has all required properties set.
     */
    virtual bool isValid() const = 0;

    /**
     * Write a human-readable representation to the given stream.
     * Subclasses should override this.
     */
    virtual std::ostream& print(std::ostream& os) const;

    friend std::ostream& operator<<(std::ostream& os, const LogicalTable& lt);

    /**
     * The SQL text that defines this logical table.  Derived classes may
     * populate this as needed.
     */
    std::string effectiveSqlQuery;
};

} // namespace r2rml
