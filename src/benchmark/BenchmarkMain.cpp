// -----------------------------------------------------------------------------
// sql2rdf_benchmark - a dev-only performance harness for the SPARQL-to-SQL
// pipeline.
//
// It takes an R2RML/YARRRML mapping, a DuckDB database, and a YAML file mapping
// query names to SPARQL query text; for each named query it times the two
// phases end-to-end - translation (SPARQL parse + translateQuery) and execution
// (DuckDB execute + full row drain) - over a configurable number of warmup and
// measured iterations, then prints per-query min/median/max timings and row
// counts. It is meant for establishing a baseline against real-world (and
// deliberately un-checked-in) customer data before optimizing the translator.
//
// Like the CLI, this target is the only-other place besides main.cpp that
// touches DuckDB, and it is gated behind the same CMake conditions so it never
// leaks into downstream FetchContent builds or the DuckDB-free test_runner.
// -----------------------------------------------------------------------------

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

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
std::vector<std::pair<std::string, std::string>> loadQueries(const std::string &path) {
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
	int64_t rows = 0;
	std::string sql;
};

struct Stats {
	double min = 0.0;
	double median = 0.0;
	double max = 0.0;
};

Stats summarize(std::vector<double> samples) {
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
	          << "  --dialect NAME   SQL dialect to translate for (default: duckdb).\n"
	          << "  --query NAME     Only benchmark the named query (default: all).\n"
	          << "  -y               Force the mapping to be parsed as YARRRML.\n"
	          << "  --print-sql      Echo each query's translated SQL to stderr.\n"
	          << "  -h, --help       Show this help message.\n";
}

int parseCount(const char *arg, const char *flag) {
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
		for (std::size_t i = 0; i < queries.size(); ++i) {
			if (queries[i].first == queryName) {
				filtered.push_back(queries[i]);
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
		std::cerr << "\n";
		result.ok = true;
		results.push_back(result);
	}

	// ---- Report ---------------------------------------------------------------
	const int nameWidth = 24;
	std::cout << std::left << std::setw(nameWidth) << "query" << std::right << std::setw(30)
	          << "translate ms (min/med/max)" << std::setw(30) << "execute ms (min/med/max)" << std::setw(12) << "rows"
	          << "\n";
	std::cout << std::string(nameWidth + 30 + 30 + 12, '-') << "\n";

	std::cout << std::fixed << std::setprecision(3);
	for (std::size_t i = 0; i < results.size(); ++i) {
		const QueryResult &r = results[i];
		std::cout << std::left << std::setw(nameWidth) << r.name << std::right;
		if (!r.ok) {
			std::cout << "  ERROR: " << r.error << "\n";
			continue;
		}
		Stats t = summarize(r.translateMs);
		Stats e = summarize(r.executeMs);
		std::ostringstream tcol;
		tcol << std::fixed << std::setprecision(3) << t.min << "/" << t.median << "/" << t.max;
		std::ostringstream ecol;
		ecol << std::fixed << std::setprecision(3) << e.min << "/" << e.median << "/" << e.max;
		std::cout << std::setw(30) << tcol.str() << std::setw(30) << ecol.str() << std::setw(12) << r.rows << "\n";
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
