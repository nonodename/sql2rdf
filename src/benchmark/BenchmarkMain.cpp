// -----------------------------------------------------------------------------
// sql2rdf_benchmark - a dev-only performance harness for the SPARQL-to-SQL
// pipeline.
//
// It takes an R2RML/YARRRML mapping, a DuckDB database, and a YAML file mapping
// query names to SPARQL query text; for each named query it times the two
// phases end-to-end - translation (SPARQL parse + translateQuery) and execution
// (DuckDB execute + full row drain) - over a configurable number of warmup and
// measured iterations, then prints per-query min/median/max timings and row
// counts. Execution is additionally split into DuckDB's two phases via the C
// API (prepare = parse+bind+optimize+plan; execute = run the prepared
// statement and drain its chunks), and the translated SQL's size and UNION ALL
// count are reported. With --profile each query is also run once under
// DuckDB's detailed JSON profiler to print planner/optimizer phase timings. It is meant for establishing a baseline
// against real-world (and deliberately un-checked-in) customer data before optimizing the translator.
//
// Like the CLI, this target is the only-other place besides main.cpp that
// touches DuckDB, and it is gated behind the same CMake conditions so it never
// leaks into downstream FetchContent builds or the DuckDB-free test_runner.
// -----------------------------------------------------------------------------

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <duckdb.h>
#include <yaml-cpp/yaml.h>

#include "DuckDBConnection.h"
#include "r2rml/MappingParser.h"
#include "r2rml/R2RMLMapping.h"
#include "r2rml/SQLResultSet.h"
#include "r2rml/SQLRow.h"
#include "r2rml/SQLValue.h"
#include "sparql-parser/Parser.h"
#include "sparql2sql/DialectFactory.h"
#include "sparql2sql/Translator.h"
#include "sparql2sql/TypeCatalog.h"
#include "sql2rdf/TypeCatalogLoader.h"
#include "yarrrml/YARRRMLParser.h"

namespace {

// -----------------------------------------------------------------------------
// Loads the query file: a flat YAML mapping of name -> SPARQL text (e.g.
// `queryName: "SELECT ..."`, or a block scalar for multi-line queries).
// Preserves document order (SPARQL query order is meaningful to the user) by
// returning a vector of pairs rather than a map - yaml-cpp iterates map nodes
// in parse order. Rejects anything that isn't a flat mapping of scalar values
// with a clear error, so a malformed queries file fails loudly rather than
// silently.
// -----------------------------------------------------------------------------
[[nodiscard]] std::vector<std::pair<std::string, std::string>> loadQueries(const std::string &path) {
	YAML::Node root;
	try {
		root = YAML::LoadFile(path);
	} catch (const YAML::Exception &e) {
		throw std::runtime_error(std::string("invalid queries YAML: ") + e.what());
	}
	if (!root.IsMap()) {
		throw std::runtime_error("queries YAML must be a flat mapping of name -> SPARQL text");
	}
	std::vector<std::pair<std::string, std::string>> out;
	for (YAML::const_iterator it = root.begin(); it != root.end(); ++it) {
		if (!it->second.IsScalar()) {
			throw std::runtime_error("query '" + it->first.as<std::string>() + "' value must be a YAML string");
		}
		out.emplace_back(it->first.as<std::string>(), it->second.as<std::string>());
	}
	return out;
}

// Per-query timing accumulator; both phases are recorded as milliseconds.
struct QueryResult {
	std::string name;
	bool ok = false;
	std::string error;
	std::vector<double> translateMs;
	std::vector<double> executeMs;
	std::vector<double> prepareMs;
	std::vector<double> preparedExecMs;
	int64_t rows = 0;
	std::string sql;
	std::size_t sqlBytes = 0;
	std::size_t unionAllCount = 0;
	// "name: seconds" pairs from the --profile run, in document order.
	std::vector<std::pair<std::string, double>> profile;
	std::string profileError;
};

struct Stats {
	double min = 0.0;
	double median = 0.0;
	double max = 0.0;
};

[[nodiscard]] Stats summarize(std::vector<double> samples) {
	Stats s;
	if (samples.empty()) {
		return s;
	}
	std::sort(samples.begin(), samples.end());
	s.min = samples.front();
	s.max = samples.back();
	std::size_t mid = samples.size() / 2;
	s.median = (samples.size() % 2 == 0) ? (samples[mid - 1] + samples[mid]) / 2.0 : samples[mid];
	return s;
}

double elapsedMs(std::chrono::steady_clock::time_point start, std::chrono::steady_clock::time_point end) {
	return std::chrono::duration<double, std::milli>(end - start).count();
}

// Counts `UNION ALL` operators in generated SQL, skipping single-quoted string
// literals and double-quoted identifiers so a literal containing the words
// does not inflate the count. A textual scan, not a parse - good enough for
// the translator's own output, which is what this is pointed at.
[[nodiscard]] std::size_t countUnionAll(const std::string &sql) {
	std::size_t count = 0;
	std::size_t i = 0;
	const std::size_t n = sql.size();
	auto isWord = [](char c) {
		return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
	};
	while (i < n) {
		char c = sql[i];
		if (c == '\'' || c == '"') {
			// Quote doubling ('' / "") re-enters the same literal on the next loop turn.
			++i;
			while (i < n && sql[i] != c) {
				++i;
			}
			++i;
			continue;
		}
		if ((c == 'U' || c == 'u') && (i == 0 || !isWord(sql[i - 1]))) {
			std::size_t j = i;
			bool matched = true;
			for (const char *kw : {"UNION", "ALL"}) {
				std::size_t len = std::strlen(kw);
				if (j + len > n) {
					matched = false;
					break;
				}
				for (std::size_t k = 0; k < len; ++k) {
					if (std::toupper(static_cast<unsigned char>(sql[j + k])) != kw[k]) {
						matched = false;
						break;
					}
				}
				if (!matched) {
					break;
				}
				j += len;
				if (kw[0] == 'U') {
					if (j >= n || !std::isspace(static_cast<unsigned char>(sql[j]))) {
						matched = false;
						break;
					}
					while (j < n && std::isspace(static_cast<unsigned char>(sql[j]))) {
						++j;
					}
				}
			}
			if (matched && (j >= n || !isWord(sql[j]))) {
				++count;
				i = j;
				continue;
			}
		}
		++i;
	}
	return count;
}

// RAII wrappers over the DuckDB C API objects the phase split needs.
struct PreparedStatement {
	duckdb_prepared_statement stmt = nullptr;
	PreparedStatement() = default;
	PreparedStatement(const PreparedStatement &) = delete;
	PreparedStatement &operator=(const PreparedStatement &) = delete;
	~PreparedStatement() {
		duckdb_destroy_prepare(&stmt);
	}
};

struct QueryResultGuard {
	duckdb_result result {};
	QueryResultGuard() = default;
	QueryResultGuard(const QueryResultGuard &) = delete;
	QueryResultGuard &operator=(const QueryResultGuard &) = delete;
	~QueryResultGuard() {
		duckdb_destroy_result(&result);
	}
};

void prepareOrThrow(duckdb_connection con, const std::string &sql, PreparedStatement &out) {
	if (duckdb_prepare(con, sql.c_str(), &out.stmt) == DuckDBError) {
		const char *msg = duckdb_prepare_error(out.stmt);
		throw std::runtime_error(std::string("prepare: ") + (msg ? msg : "unknown error"));
	}
}

// Runs a prepared statement and drains every result chunk, returning the row
// count. This walks chunk sizes only - it does not convert cells to strings -
// so it measures the engine's execution cost, not the CLI's value marshalling
// (which the "execute" column, going through DuckDBConnection, does include).
int64_t executeAndDrain(duckdb_prepared_statement stmt) {
	QueryResultGuard guard;
	if (duckdb_execute_prepared(stmt, &guard.result) == DuckDBError) {
		const char *msg = duckdb_result_error(&guard.result);
		throw std::runtime_error(std::string("execute: ") + (msg ? msg : "unknown error"));
	}
	int64_t rows = 0;
	idx_t chunks = duckdb_result_chunk_count(guard.result);
	for (idx_t c = 0; c < chunks; ++c) {
		duckdb_data_chunk chunk = duckdb_result_get_chunk(guard.result, c);
		rows += static_cast<int64_t>(duckdb_data_chunk_get_size(chunk));
		duckdb_destroy_data_chunk(&chunk);
	}
	return rows;
}

void runStatement(duckdb_connection con, const std::string &sql) {
	QueryResultGuard guard;
	if (duckdb_query(con, sql.c_str(), &guard.result) == DuckDBError) {
		const char *msg = duckdb_result_error(&guard.result);
		throw std::runtime_error(sql + ": " + (msg ? msg : "unknown error"));
	}
}

// Collects the planning-phase timings from a detailed JSON profile: the numeric
// entries of the top-level `parser`, `planner`, `optimizer` (one entry per
// optimizer pass plus total_time), `physical_planner` and `query` sections,
// named "section.key". The per-operator tree (`operator`), `io` and `system`
// sections are deliberately skipped - those are execution detail, not phases.
void collectPhases(const YAML::Node &root, std::vector<std::pair<std::string, double>> &out) {
	if (!root.IsMap()) {
		return;
	}
	for (const char *section : {"parser", "planner", "optimizer", "physical_planner", "query"}) {
		const YAML::Node node = root[section];
		if (!node.IsDefined() || !node.IsMap()) {
			continue;
		}
		for (YAML::const_iterator it = node.begin(); it != node.end(); ++it) {
			if (!it->second.IsScalar()) {
				continue;
			}
			try {
				out.emplace_back(std::string(section) + "." + it->first.as<std::string>(), it->second.as<double>());
			} catch (const YAML::Exception &) {
				// Non-numeric entry (e.g. query.sql); not a timing.
			}
		}
	}
}

// Runs the SQL once under DuckDB's detailed JSON profiler and returns the
// planner/optimizer phase timings (seconds).
[[nodiscard]] std::vector<std::pair<std::string, double>> profileOnce(duckdb_connection con, const std::string &sql,
                                                                      std::size_t index) {
	std::filesystem::path file =
	    std::filesystem::temp_directory_path() / ("sql2rdf_benchmark_profile_" + std::to_string(index) + ".json");
	std::error_code ec;
	std::filesystem::remove(file, ec);
	runStatement(con, "PRAGMA enable_profiling='json'");
	runStatement(con, "PRAGMA profiling_mode='detailed'");
	std::string quoted;
	for (char c : file.string()) {
		quoted += c;
		if (c == '\'') {
			quoted += '\'';
		}
	}
	runStatement(con, "PRAGMA profiling_output='" + quoted + "'");
	std::string failure;
	try {
		PreparedStatement stmt;
		// Prepare inside the profiled window so planner/optimizer phases are recorded.
		prepareOrThrow(con, sql, stmt);
		(void)executeAndDrain(stmt.stmt);
	} catch (const std::exception &e) {
		failure = e.what();
	}
	runStatement(con, "PRAGMA disable_profiling");
	if (!failure.empty()) {
		throw std::runtime_error(failure);
	}
	std::vector<std::pair<std::string, double>> out;
	try {
		YAML::Node root = YAML::LoadFile(file.string());
		collectPhases(root, out);
	} catch (const YAML::Exception &e) {
		std::filesystem::remove(file, ec);
		throw std::runtime_error(std::string("could not read profiling output: ") + e.what());
	}
	std::filesystem::remove(file, ec);
	return out;
}

[[nodiscard]] std::string statsColumn(const std::vector<double> &samples) {
	Stats s = summarize(samples);
	std::ostringstream os;
	os << std::fixed << std::setprecision(3) << s.min << "/" << s.median << "/" << s.max;
	return os.str();
}

void printHelp(const char *programName) {
	std::cerr << "Usage: " << programName << " [options] <mapping.ttl|mapping.yml> <database.duckdb> <queries.yml>\n"
	          << "\n"
	          << "Benchmarks the SPARQL-to-SQL pipeline against a real database. For each\n"
	          << "named SPARQL query it times translation (parse + translateQuery) and\n"
	          << "execution (DuckDB execute + row drain) separately, over warmup + measured\n"
	          << "iterations, and prints min/median/max timings plus row counts.\n"
	          << "\n"
	          << "Arguments:\n"
	          << "  mapping.ttl|mapping.yml   R2RML (Turtle) or YARRRML (YAML) mapping; format\n"
	          << "                            chosen by extension unless -y is given.\n"
	          << "  database.duckdb           DuckDB database file.\n"
	          << "  queries.yml               Flat YAML mapping of `name: \"SPARQL text\"`.\n"
	          << "\n"
	          << "Options:\n"
	          << "  --repeat N       Measured iterations per query (default 5).\n"
	          << "  --warmup N       Unrecorded warmup iterations per query (default 1).\n"
	          << "  --dialect NAME   SQL dialect to translate for (default: duckdb; or duckdb-udf).\n"
	          << "  --query NAME     Only benchmark the named query (default: all).\n"
	          << "  -y               Force the mapping to be parsed as YARRRML.\n"
	          << "  --print-sql      Echo each query's translated SQL to stderr.\n"
	          << "  -h, --help       Show this help message.\n";
}

[[nodiscard]] int parseCount(const char *arg, const char *flag) {
	char *end;
	int64_t value = std::strtol(arg, &end, 10);
	if (*end != '\0') {
		std::cerr << "Error: " << flag << " must be an integer\n";
		std::exit(1);
	}
	if (value < 0) {
		std::cerr << "Error: " << flag << " must be >= 0\n";
		std::exit(1);
	}
	return static_cast<int>(value);
}

} // namespace

int main(int argc, char *argv[]) {
	bool forceYarrrml = false;
	bool printSql = false;
	bool profile = false;
	int repeat = 5;
	int warmup = 1;
	const char *dialectName = "duckdb";
	const char *queryName = nullptr;
	const char *mappingFile = nullptr;
	const char *databaseFile = nullptr;
	const char *queriesFile = nullptr;

	for (int i = 1; i < argc; ++i) {
		if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
			printHelp(argv[0]);
			return 0;
		} else if (std::strcmp(argv[i], "-y") == 0) {
			forceYarrrml = true;
		} else if (std::strcmp(argv[i], "--print-sql") == 0) {
			printSql = true;
		} else if (std::strcmp(argv[i], "--profile") == 0) {
			profile = true;
		} else if (std::strcmp(argv[i], "--repeat") == 0) {
			if (++i >= argc) {
				std::cerr << "Error: --repeat requires a count argument\n";
				return 1;
			}
			repeat = parseCount(argv[i], "--repeat");
		} else if (std::strcmp(argv[i], "--warmup") == 0) {
			if (++i >= argc) {
				std::cerr << "Error: --warmup requires a count argument\n";
				return 1;
			}
			warmup = parseCount(argv[i], "--warmup");
		} else if (std::strcmp(argv[i], "--dialect") == 0) {
			if (++i >= argc) {
				std::cerr << "Error: --dialect requires a dialect name argument\n";
				return 1;
			}
			dialectName = argv[i];
		} else if (std::strcmp(argv[i], "--query") == 0) {
			if (++i >= argc) {
				std::cerr << "Error: --query requires a query name argument\n";
				return 1;
			}
			queryName = argv[i];
		} else if (argv[i][0] != '-') {
			if (!mappingFile) {
				mappingFile = argv[i];
			} else if (!databaseFile) {
				databaseFile = argv[i];
			} else if (!queriesFile) {
				queriesFile = argv[i];
			} else {
				std::cerr << "Error: unexpected argument '" << argv[i] << "'\n";
				printHelp(argv[0]);
				return 1;
			}
		} else {
			std::cerr << "Error: unknown option '" << argv[i] << "'\n";
			printHelp(argv[0]);
			return 1;
		}
	}

	if (!mappingFile || !databaseFile || !queriesFile) {
		std::cerr << "Error: mapping file, database file and queries YAML are all required.\n";
		printHelp(argv[0]);
		return 1;
	}
	if (repeat < 1) {
		std::cerr << "Error: --repeat must be >= 1\n";
		return 1;
	}

	// ---- Parse the mapping ----------------------------------------------------
	r2rml::R2RMLMapping mapping;
	try {
		std::unique_ptr<r2rml::MappingParser> parser =
		    forceYarrrml ? std::unique_ptr<r2rml::MappingParser>(new yarrrml::YARRRMLParser())
		                 : r2rml::MappingParser::create(mappingFile);
		mapping = parser->parse(mappingFile);
	} catch (const std::exception &e) {
		std::cerr << "Error: failed to parse mapping '" << mappingFile << "': " << e.what() << "\n";
		return 1;
	}
	if (!mapping.isValid()) {
		std::cerr << "Error: R2RML mapping '" << mappingFile << "' is invalid.\n";
		return 1;
	}

	// ---- Load the queries -----------------------------------------------------
	std::vector<std::pair<std::string, std::string>> queries;
	try {
		queries = loadQueries(queriesFile);
	} catch (const std::exception &e) {
		std::cerr << "Error: failed to read queries '" << queriesFile << "': " << e.what() << "\n";
		return 1;
	}
	if (queries.empty()) {
		std::cerr << "Error: no queries found in '" << queriesFile << "'\n";
		return 1;
	}
	if (queryName) {
		std::vector<std::pair<std::string, std::string>> filtered;
		for (const auto &[name, text] : queries) {
			if (name == queryName) {
				filtered.emplace_back(name, text);
				break;
			}
		}
		if (filtered.empty()) {
			std::cerr << "Error: query '" << queryName << "' not found in '" << queriesFile << "'\n";
			return 1;
		}
		queries = filtered;
	}

	// ---- Dialect + database ---------------------------------------------------
	std::unique_ptr<sparql2sql::SqlDialect> dialect;
	try {
		dialect = sparql2sql::createDialect(dialectName);
	} catch (const std::exception &e) {
		std::cerr << "Error: " << e.what() << "\n";
		return 1;
	}

	std::unique_ptr<r2rml::DuckDBConnection> dbConn;
	try {
		dbConn.reset(new r2rml::DuckDBConnection(databaseFile));
		// Registered for every dialect, so the two dialects are timed on
		// identically set-up connections.
		dbConn->registerRdfFunctions();
	} catch (const std::exception &e) {
		std::cerr << "Error: cannot open database '" << databaseFile << "': " << e.what() << "\n";
		return 1;
	}

	sparql2sql::TypeCatalog catalog;
	const sparql2sql::TypeCatalog *catalogPtr = nullptr;
	try {
		sql2rdf::loadTypeCatalog(*dbConn, &mapping, catalog);
		catalogPtr = &catalog;
	} catch (const std::exception &e) {
		std::cerr << "Warning: could not read column types (native-typed joins disabled): " << e.what() << "\n";
	}

	std::cerr << "Benchmarking " << queries.size() << " quer" << (queries.size() == 1 ? "y" : "ies") << " (" << warmup
	          << " warmup + " << repeat << " measured iteration" << (repeat == 1 ? "" : "s") << " each) against '"
	          << databaseFile << "'...\n\n";

	// ---- Run ------------------------------------------------------------------
	std::vector<QueryResult> results;
	results.reserve(queries.size());

	for (std::size_t q = 0; q < queries.size(); ++q) {
		const std::string &name = queries[q].first;
		const std::string &queryText = queries[q].second;
		QueryResult result;
		result.name = name;
		std::cerr << "[" << name << "] running:";
		// One translation up front both validates the query and produces the SQL
		// we execute; failures here are reported and the query is skipped.
		try {
			sparql::Parser parser;
			std::unique_ptr<sparql::ast::Query> query = parser.parseString(queryText);
			result.sql = sparql2sql::translateQuery(*query, mapping, *dialect, catalogPtr);
		} catch (const std::exception &e) {
			result.error = std::string("translate: ") + e.what();
			results.push_back(result);
			std::cerr << "  [" << name << "] translation failed: " << e.what() << "\n";
			continue;
		}

		result.sqlBytes = result.sql.size();
		result.unionAllCount = countUnionAll(result.sql);

		if (printSql) {
			std::cerr << "  [" << name << "] SQL:\n" << result.sql << "\n";
		}

		// Sanity-check execution once (and drain the rows) before timing.
		try {
			std::unique_ptr<r2rml::SQLResultSet> rs = dbConn->execute(result.sql);
			int64_t rows = 0;
			while (rs->next()) {
				++rows;
			}
			result.rows = rows;
		} catch (const std::exception &e) {
			result.error = std::string("execute: ") + e.what();
			results.push_back(result);
			std::cerr << "  [" << name << "] execution failed: " << e.what() << "\n";
			continue;
		}

		// Time translation: re-parse and re-translate each iteration so the
		// measured cost is the full SPARQL-text -> SQL-text path.
		for (int it = 0; it < warmup + repeat; ++it) {
			std::cerr << "." << std::flush;
			std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
			sparql::Parser parser;
			std::unique_ptr<sparql::ast::Query> query = parser.parseString(queryText);
			volatile std::size_t sink = sparql2sql::translateQuery(*query, mapping, *dialect, catalogPtr).size();
			(void)sink;
			std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
			if (it >= warmup) {
				result.translateMs.push_back(elapsedMs(start, end));
			}
		}

		// Time execution: run the already-translated SQL and drain all rows.
		for (int it = 0; it < warmup + repeat; ++it) {
			std::cerr << "." << std::flush;
			std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
			std::unique_ptr<r2rml::SQLResultSet> rs = dbConn->execute(result.sql);
			while (rs->next()) {
				// Force materialization of every row so we time the real work.
				const r2rml::SQLRow &row = rs->getCurrentRow();
				(void)row;
			}
			std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
			if (it >= warmup) {
				result.executeMs.push_back(elapsedMs(start, end));
			}
		}

		// Time DuckDB's phases separately through the C API on the same connection:
		// prepare (parse+bind+optimize+plan) and prepared execute + chunk drain.
		auto con = static_cast<duckdb_connection>(dbConn->nativeConnection());
		try {
			for (int it = 0; it < warmup + repeat; ++it) {
				std::cerr << "." << std::flush;
				std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
				PreparedStatement stmt;
				prepareOrThrow(con, result.sql, stmt);
				std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
				if (it >= warmup) {
					result.prepareMs.push_back(elapsedMs(start, end));
				}
			}
			PreparedStatement stmt;
			prepareOrThrow(con, result.sql, stmt);
			for (int it = 0; it < warmup + repeat; ++it) {
				std::cerr << "." << std::flush;
				std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
				(void)executeAndDrain(stmt.stmt);
				std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
				if (it >= warmup) {
					result.preparedExecMs.push_back(elapsedMs(start, end));
				}
			}
		} catch (const std::exception &e) {
			result.error = std::string("phase split: ") + e.what();
			results.push_back(result);
			std::cerr << "  [" << name << "] phase timing failed: " << e.what() << "\n";
			continue;
		}

		if (profile) {
			try {
				result.profile = profileOnce(con, result.sql, q);
			} catch (const std::exception &e) {
				result.profileError = e.what();
			}
		}
		std::cerr << "\n";
		result.ok = true;
		results.push_back(result);
	}

	// ---- Report ---------------------------------------------------------------
	const int nameWidth = 24;
	const int colWidth = 28;
	std::cout << std::fixed << std::setprecision(3);
	std::cout << std::left << std::setw(nameWidth) << "query" << std::right << std::setw(colWidth)
	          << "translate ms (min/med/max)" << std::setw(colWidth) << "prepare ms (min/med/max)"
	          << std::setw(colWidth) << "exec+drain ms (min/med/max)" << std::setw(colWidth)
	          << "execute ms (min/med/max)" << std::setw(10) << "rows" << std::setw(12) << "sql bytes" << std::setw(11)
	          << "union all" << "\n";
	std::cout << std::string(nameWidth + 4 * colWidth + 10 + 12 + 11, '-') << "\n";

	for (std::size_t i = 0; i < results.size(); ++i) {
		const QueryResult &r = results[i];
		std::cout << std::left << std::setw(nameWidth) << r.name << std::right;
		if (!r.ok) {
			std::cout << "  ERROR: " << r.error << "\n";
			continue;
		}
		std::cout << std::setw(colWidth) << statsColumn(r.translateMs) << std::setw(colWidth)
		          << statsColumn(r.prepareMs) << std::setw(colWidth) << statsColumn(r.preparedExecMs)
		          << std::setw(colWidth) << statsColumn(r.executeMs) << std::setw(10) << r.rows << std::setw(12)
		          << r.sqlBytes << std::setw(11) << r.unionAllCount << "\n";
	}

	if (profile) {
		std::cout << "\nDuckDB profile (detailed, one run per query; seconds):\n";
		for (std::size_t i = 0; i < results.size(); ++i) {
			const QueryResult &r = results[i];
			if (!r.ok) {
				continue;
			}
			std::cout << "[" << r.name << "]\n";
			if (!r.profileError.empty()) {
				std::cout << "  profiling failed: " << r.profileError << "\n";
				continue;
			}
			if (r.profile.empty()) {
				std::cout << "  (no planner/optimizer timings found in profiler output)\n";
			}
			for (const auto &[key, seconds] : r.profile) {
				std::cout << "  " << std::left << std::setw(44) << key << std::right << std::setw(12)
				          << std::setprecision(6) << seconds << "\n";
			}
		}
		std::cout << std::setprecision(3);
	}

	std::size_t failures = 0;
	for (std::size_t i = 0; i < results.size(); ++i) {
		if (!results[i].ok) {
			++failures;
		}
	}
	std::cout << "\n" << (results.size() - failures) << " succeeded, " << failures << " failed.\n";
	return failures == 0 ? 0 : 1;
}
