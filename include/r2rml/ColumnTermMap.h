#pragma once

#include "TermMap.h"
#include <memory>

namespace r2rml {

/**
 * A term map that derives its value from a column in the logical table.
 */
class ColumnTermMap : public TermMap {
public:
    ColumnTermMap() = default;
    explicit ColumnTermMap(const std::string& column);
    ~ColumnTermMap() override;

    SerdNode generateRDFTerm(const SQLRow& row,
                             const SerdEnv& env) const override;

    bool isValid() const override {
        // columnName must not be empty
        return !columnName.empty();
    }

    std::ostream& print(std::ostream& os) const override;

    std::string columnName;

private:
    /// Buffer for the last column value; keeps buf pointer in returned SerdNode valid.
    mutable std::string cachedValue_;
};

} // namespace r2rml
