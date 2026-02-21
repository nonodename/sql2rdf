#pragma once

namespace r2rml {

class SQLRow;

/**
 * Pure abstract interface for the results of executing an SQL query.
 * A cursor-style API allows iterating over rows.
 * Implement this interface to plug in a concrete database backend.
 */
class SQLResultSet {
public:
    virtual ~SQLResultSet() = default;

    /** Advance the cursor.  Returns true while rows remain. */
    virtual bool next() = 0;

    /** Return the row at the current cursor position. */
    virtual SQLRow getCurrentRow() const = 0;
};

} // namespace r2rml
