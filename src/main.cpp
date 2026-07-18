#include <cerrno>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <serd/serd.h>

#include "DuckDBConnection.h"
#include "r2rml/MappingParser.h"
#include "r2rml/R2RMLMapping.h"
#include "r2rml/SQLResultSet.h"
#include "r2rml/SQLRow.h"
#include "r2rml/SQLValue.h"
#include "r2rml/TriplesMap.h"
#include "sparql-parser/Parser.h"
#include "sparql-parser/PrettyPrinter.h"
#include "sparql2sql/DialectFactory.h"
#include "sparql2sql/Translator.h"
#include "yarrrml/YARRRMLParser.h"

static void printHelp(const char *programName) {
	std::cerr << "Usage: " << programName << " [options] <mapping.ttl|mapping.yml> <database.db> <output.nt>\n"
	          << "\n"
	          << "Arguments:\n"
	          << "  mapping.ttl|mapping.yml   R2RML mapping file (Turtle) or YARRRML mapping\n"
	          << "                            file (YAML); the format is chosen from the file\n"
	          << "                            extension (.ttl -> R2RML, .yml/.yaml/.yarrrml ->\n"
	          << "                            YARRRML) unless overridden with -y.\n"
	          << "  database.db               DuckDB database file\n"
	          << "  output.nt                 Output RDF file\n"
	          << "\n"
	          << "Options:\n"
	          << "  -f ntriples|turtle   Output format (default: ntriples); ignored with -T\n"
	          << "  -y                   Force the mapping file to be parsed as YARRRML,\n"
	          << "                       regardless of its extension\n"
	          << "  -P                   Print the parsed mapping to stderr\n"
	          << "  -Q <file.rq>         Parse a SPARQL query file and print its AST to\n"
	          << "                       stdout, then exit (bypasses the mapping/database/\n"
	          << "                       output pipeline entirely)\n"
	          << "  -T <file.rq>         Translate a SPARQL query file into SQL against the\n"
	          << "                       given R2RML/YARRRML mapping (uses an R2RML mapping in\n"
	          << "                       reverse), then exit. Requires the mapping file\n"
	          << "                       positional argument; mutually exclusive with -Q.\n"
	          << "                       If the database.db positional argument is also given,\n"
	          << "                       the translated SQL is additionally executed against it\n"
	          << "                       (result rows to stdout, SQL echoed to stderr);\n"
	          << "                       otherwise the SQL alone is printed to stdout.\n"
	          << "  --dialect <name>     SQL dialect to translate for with -T (default: duckdb;\n"
	          << "                       currently the only supported dialect)\n"
	          << "  -h                   Show this help message\n";
}

int main(int argc, char *argv[]) {
	if (argc < 2) {
		printHelp(argv[0]);
		return 1;
	}

	bool printMapping = false;
	bool forceYarrrml = false;
	SerdSyntax outputFormat = SERD_NTRIPLES;
	const char *mappingFile = nullptr;
	const char *databaseFile = nullptr;
	const char *outputFile = nullptr;
	const char *sparqlQueryFile = nullptr;
	const char *translateQueryFile = nullptr;
	const char *dialectName = "duckdb";

	for (int i = 1; i < argc; ++i) {
		if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
			printHelp(argv[0]);
			return 0;
		} else if (std::strcmp(argv[i], "-P") == 0) {
			printMapping = true;
		} else if (std::strcmp(argv[i], "-y") == 0) {
			forceYarrrml = true;
		} else if (std::strcmp(argv[i], "-Q") == 0) {
			if (++i >= argc) {
				std::cerr << "Error: -Q requires a SPARQL query file argument\n";
				return 1;
			}
			sparqlQueryFile = argv[i];
		} else if (std::strcmp(argv[i], "-T") == 0) {
			if (++i >= argc) {
				std::cerr << "Error: -T requires a SPARQL query file argument\n";
				return 1;
			}
			translateQueryFile = argv[i];
		} else if (std::strcmp(argv[i], "--dialect") == 0) {
			if (++i >= argc) {
				std::cerr << "Error: --dialect requires a dialect name argument\n";
				return 1;
			}
			dialectName = argv[i];
		} else if (std::strcmp(argv[i], "-f") == 0) {
			if (++i >= argc) {
				std::cerr << "Error: -f requires a format argument"
				             " (ntriples|turtle)\n";
				return 1;
			}
			if (std::strcmp(argv[i], "ntriples") == 0) {
				outputFormat = SERD_NTRIPLES;
			} else if (std::strcmp(argv[i], "turtle") == 0) {
				outputFormat = SERD_TURTLE;
			} else {
				std::cerr << "Error: unknown format '" << argv[i] << "' (use ntriples or turtle)\n";
				return 1;
			}
		} else if (argv[i][0] != '-') {
			if (!mappingFile) {
				mappingFile = argv[i];
			} else if (!databaseFile) {
				databaseFile = argv[i];
			} else if (!outputFile) {
				outputFile = argv[i];
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

	if (sparqlQueryFile && translateQueryFile) {
		std::cerr << "Error: options -Q and -T are mutually exclusive\n";
		return 1;
	}

	if (sparqlQueryFile) {
		try {
			sparql::Parser parser;
			std::unique_ptr<sparql::ast::Query> query = parser.parseFile(sparqlQueryFile);
			sparql::print(std::cout, *query);
		} catch (const std::exception &e) {
			std::cerr << "Error: failed to parse SPARQL query '" << sparqlQueryFile << "': " << e.what() << "\n";
			return 1;
		}
		return 0;
	}

	if (translateQueryFile) {
		if (!mappingFile) {
			std::cerr << "Error: -T requires the mapping file argument\n";
			printHelp(argv[0]);
			return 1;
		}
		if (outputFile) {
			std::cerr << "Error: unexpected argument '" << outputFile << "' (an output file is not used with -T)\n";
			return 1;
		}

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

		if (printMapping) {
			std::cerr << mapping << "\n";
		}

		if (!mapping.isValid()) {
			std::cerr << "Error: R2RML mapping '" << mappingFile << "' is invalid.\n";
			return 1;
		}

		std::unique_ptr<sparql2sql::SqlDialect> dialect;
		try {
			dialect = sparql2sql::createDialect(dialectName);
		} catch (const std::exception &e) {
			std::cerr << "Error: " << e.what() << "\n";
			return 1;
		}

		std::string sql;
		try {
			sparql::Parser parser;
			std::unique_ptr<sparql::ast::Query> query = parser.parseFile(translateQueryFile);
			sql = sparql2sql::translateQuery(*query, mapping, *dialect);
		} catch (const std::exception &e) {
			std::cerr << "Error: failed to translate SPARQL query '" << translateQueryFile << "': " << e.what() << "\n";
			return 1;
		}

		if (!databaseFile) {
			std::cout << sql << "\n";
			return 0;
		}

		// A database file was also given: additionally execute the
		// translated SQL and print the result rows (the SQL itself becomes
		// a diagnostic on stderr instead of the primary stdout payload).
		std::cerr << sql << "\n";
		try {
			r2rml::DuckDBConnection dbConn(databaseFile);
			std::unique_ptr<r2rml::SQLResultSet> results = dbConn.execute(sql);
			bool printedHeader = false;
			while (results->next()) {
				const r2rml::SQLRow &row = results->getCurrentRow();
				std::vector<std::string> cols = row.columnNames();
				if (!printedHeader) {
					for (std::size_t i = 0; i < cols.size(); ++i) {
						std::cout << (i ? "\t" : "") << cols[i];
					}
					std::cout << "\n";
					printedHeader = true;
				}
				for (std::size_t i = 0; i < cols.size(); ++i) {
					std::unique_ptr<r2rml::SQLValue> value = row.getValue(cols[i]);
					std::cout << (i ? "\t" : "") << (value->isNull() ? "NULL" : value->asString());
				}
				std::cout << "\n";
			}
		} catch (const std::exception &e) {
			std::cerr << "Error: " << e.what() << "\n";
			return 1;
		}
		return 0;
	}

	if (!mappingFile || !databaseFile || !outputFile) {
		std::cerr << "Error: mapping file, database file and output file"
		             " are all required.\n";
		printHelp(argv[0]);
		return 1;
	}

	// -------------------------------------------------------------------------
	// Parse and validate the mapping (R2RML Turtle or YARRRML YAML, chosen by
	// file extension unless -y forces YARRRML).
	// -------------------------------------------------------------------------
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

	if (printMapping) {
		std::cerr << mapping << "\n";
	}

	if (!mapping.isValid()) {
		std::cerr << "Error: R2RML mapping '" << mappingFile << "' is invalid.\n";
		return 1;
	}

	// -------------------------------------------------------------------------
	// Open the DuckDB database
	// -------------------------------------------------------------------------
	std::unique_ptr<r2rml::DuckDBConnection> dbConn;
	try {
		dbConn.reset(new r2rml::DuckDBConnection(databaseFile));
	} catch (const std::exception &e) {
		std::cerr << "Error: cannot open database '" << databaseFile << "': " << e.what() << "\n";
		return 1;
	}

	// -------------------------------------------------------------------------
	// Open the output file
	// -------------------------------------------------------------------------
	FILE *outFile = std::fopen(outputFile, "w");
	if (!outFile) {
		std::cerr << "Error: cannot create output file '" << outputFile << "': " << std::strerror(errno) << "\n";
		return 1;
	}

	// -------------------------------------------------------------------------
	// Create the Serd writer
	// -------------------------------------------------------------------------
	SerdStyle style = (outputFormat == SERD_TURTLE) ? static_cast<SerdStyle>(SERD_STYLE_ABBREVIATED | SERD_STYLE_CURIED)
	                                                : static_cast<SerdStyle>(0);

	SerdWriter *writer = serd_writer_new(outputFormat, style, mapping.serdEnvironment,
	                                     /*base_uri=*/nullptr, serd_file_sink, outFile);

	// For Turtle output, emit the @prefix declarations collected by the parser
	// so that the output is compact and readable.
	if (outputFormat == SERD_TURTLE && mapping.serdEnvironment) {
		serd_env_foreach(mapping.serdEnvironment, reinterpret_cast<SerdPrefixSink>(serd_writer_set_prefix), writer);
	}

	// -------------------------------------------------------------------------
	// Execute the mapping
	// -------------------------------------------------------------------------
	int exitCode = 0;
	try {
		mapping.processDatabase(*dbConn, *writer);
	} catch (const std::exception &e) {
		std::cerr << "Error: " << e.what() << "\n";
		exitCode = 1;
	}

	// -------------------------------------------------------------------------
	// Flush, close, clean up
	// -------------------------------------------------------------------------
	serd_writer_finish(writer);
	serd_writer_free(writer);
	std::fclose(outFile);

	if (exitCode == 0) {
		std::cerr << "Written to " << outputFile << "\n";
	}

	return exitCode;
}
