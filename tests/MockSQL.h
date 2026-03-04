#pragma once

/**
 * Concrete mock implementations of the abstract SQL interfaces for use in
 * unit tests.  Include this header from any test file that needs a database
 * connection without a real backend.
 */

#include "r2rml/MapSQLRow.h"
#include "r2rml/SQLConnection.h"
#include "r2rml/SQLResultSet.h"
#include "r2rml/SQLRow.h"
#include "r2rml/SQLValue.h"

#include <memory>
#include <string>
#include <vector>

namespace r2rml {
namespace testing {

// ---------------------------------------------------------------------------
// MockSQLResultSet
//
// Iterates over a fixed, in-memory vector of MapSQLRows.
// ---------------------------------------------------------------------------
class MockSQLResultSet : public SQLResultSet {
public:
	explicit MockSQLResultSet(std::vector<MapSQLRow> rows) : rows_(std::move(rows)) {
	}

	bool next() override {
		++cursor_;
		return cursor_ < static_cast<int>(rows_.size());
	}

	const SQLRow &getCurrentRow() const override {
		return rows_[static_cast<size_t>(cursor_)];
	}

private:
	std::vector<MapSQLRow> rows_;
	int cursor_ {-1};
};

// ---------------------------------------------------------------------------
// MockSQLConnection
//
// Returns pre-registered rows for any query whose text contains a registered
// key fragment.  When multiple keys match, the longest one wins (so a more
// specific fragment takes priority over a shorter substring).
//
// Usage:
//   MockSQLConnection conn;
//   conn.addResult("EMP", { makeRow({{"EMPNO", SQLValue(42)}}) });
// ---------------------------------------------------------------------------
class MockSQLConnection : public SQLConnection {
public:
	void addResult(std::string queryFragment, std::vector<MapSQLRow> rows) {
		results_.push_back({std::move(queryFragment), std::move(rows)});
	}

	std::unique_ptr<SQLResultSet> execute(const std::string &query) override {
		const std::vector<MapSQLRow> *best = nullptr;
		size_t bestLen = 0;
		for (const auto &kv : results_) {
			if (query.find(kv.first) != std::string::npos && kv.first.size() > bestLen) {
				bestLen = kv.first.size();
				best = &kv.second;
			}
		}
		if (best) {
			return std::unique_ptr<SQLResultSet>(new MockSQLResultSet(*best));
		}
		return std::unique_ptr<SQLResultSet>(new MockSQLResultSet(std::vector<MapSQLRow> {}));
	}

private:
	std::vector<std::pair<std::string, std::vector<MapSQLRow>>> results_;
};

// ---------------------------------------------------------------------------
// makeRow helper
//
// Build a MapSQLRow from an initializer-list of {column, value} pairs.
// ---------------------------------------------------------------------------
inline MapSQLRow makeRow(std::initializer_list<std::pair<const std::string, SQLValue>> cols) {
	return MapSQLRow(std::map<std::string, SQLValue>(cols));
}

} // namespace testing
} // namespace r2rml
