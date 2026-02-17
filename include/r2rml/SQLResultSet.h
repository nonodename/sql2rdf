#pragma once

#include <memory>

namespace r2rml {

class SQLRow;

/**
 * Represents the results of executing an SQL query.  A cursor-style API allows
 * iterating over rows.
 */
class SQLResultSet {
public:
    virtual ~SQLResultSet();

    virtual bool next() = 0;
    virtual SQLRow getCurrentRow() const = 0;
};

} // namespace r2rml
