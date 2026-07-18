#pragma once

#include <memory>
#include <string>

#include "sparql2sql/SqlDialect.h"

namespace sparql2sql {

/// Create a SqlDialect by name (currently only "duckdb"). Throws
/// std::runtime_error naming the supported set for any other name. This is
/// the single seam future dialects (Postgres, ANSI, ...) plug into.
std::unique_ptr<SqlDialect> createDialect(const std::string &name);

} // namespace sparql2sql
