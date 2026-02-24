#include <cerrno>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

#include <serd/serd.h>

#include "r2rml/DuckDBConnection.h"
#include "r2rml/R2RMLMapping.h"
#include "r2rml/R2RMLParser.h"
#include "r2rml/TriplesMap.h"

static void printHelp(const char* programName) {
    std::cerr
        << "Usage: " << programName
        << " [options] <mapping.ttl> <database.db> <output.nt>\n"
        << "\n"
        << "Arguments:\n"
        << "  mapping.ttl    R2RML mapping file (Turtle format)\n"
        << "  database.db    DuckDB database file\n"
        << "  output.nt      Output RDF file\n"
        << "\n"
        << "Options:\n"
        << "  -f ntriples|turtle   Output format (default: ntriples)\n"
        << "  -P                   Print the parsed mapping to stderr\n"
        << "  -h                   Show this help message\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printHelp(argv[0]);
        return 1;
    }

    bool        printMapping = false;
    SerdSyntax  outputFormat = SERD_NTRIPLES;
    const char* mappingFile  = nullptr;
    const char* databaseFile = nullptr;
    const char* outputFile   = nullptr;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-h") == 0 ||
                std::strcmp(argv[i], "--help") == 0) {
            printHelp(argv[0]);
            return 0;
        } else if (std::strcmp(argv[i], "-P") == 0) {
            printMapping = true;
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
                std::cerr << "Error: unknown format '" << argv[i]
                          << "' (use ntriples or turtle)\n";
                return 1;
            }
        } else if (argv[i][0] != '-') {
            if      (!mappingFile)  mappingFile  = argv[i];
            else if (!databaseFile) databaseFile = argv[i];
            else if (!outputFile)   outputFile   = argv[i];
            else {
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

    if (!mappingFile || !databaseFile || !outputFile) {
        std::cerr << "Error: mapping file, database file and output file"
                     " are all required.\n";
        printHelp(argv[0]);
        return 1;
    }

    // -------------------------------------------------------------------------
    // Parse and validate the R2RML mapping
    // -------------------------------------------------------------------------
    r2rml::R2RMLMapping mapping;
    try {
        r2rml::R2RMLParser parser;
        mapping = parser.parse(mappingFile);
    } catch (const std::exception& e) {
        std::cerr << "Error: failed to parse mapping '" << mappingFile
                  << "': " << e.what() << "\n";
        return 1;
    }

    if (printMapping) {
        std::cerr << mapping << "\n";
    }

    if (!mapping.isValid()) {
        std::cerr << "Error: R2RML mapping '" << mappingFile
                  << "' is invalid.\n";
        return 1;
    }

    // -------------------------------------------------------------------------
    // Open the DuckDB database
    // -------------------------------------------------------------------------
    std::unique_ptr<r2rml::DuckDBConnection> dbConn;
    try {
        dbConn.reset(new r2rml::DuckDBConnection(databaseFile));
    } catch (const std::exception& e) {
        std::cerr << "Error: cannot open database '" << databaseFile
                  << "': " << e.what() << "\n";
        return 1;
    }

    // -------------------------------------------------------------------------
    // Open the output file
    // -------------------------------------------------------------------------
    FILE* outFile = std::fopen(outputFile, "w");
    if (!outFile) {
        std::cerr << "Error: cannot create output file '" << outputFile
                  << "': " << std::strerror(errno) << "\n";
        return 1;
    }

    // -------------------------------------------------------------------------
    // Create the Serd writer
    // -------------------------------------------------------------------------
    SerdStyle style = (outputFormat == SERD_TURTLE)
        ? static_cast<SerdStyle>(SERD_STYLE_ABBREVIATED | SERD_STYLE_CURIED)
        : static_cast<SerdStyle>(0);

    SerdWriter* writer = serd_writer_new(
        outputFormat, style, mapping.serdEnvironment,
        /*base_uri=*/nullptr, serd_file_sink, outFile);

    // For Turtle output, emit the @prefix declarations collected by the parser
    // so that the output is compact and readable.
    if (outputFormat == SERD_TURTLE && mapping.serdEnvironment) {
        serd_env_foreach(
            mapping.serdEnvironment,
            reinterpret_cast<SerdPrefixSink>(serd_writer_set_prefix),
            writer);
    }

    // -------------------------------------------------------------------------
    // Execute the mapping
    // -------------------------------------------------------------------------
    int exitCode = 0;
    try {
        mapping.processDatabase(*dbConn, *writer);
    } catch (const std::exception& e) {
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
