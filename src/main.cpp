#include <cstring>
#include <iostream>
#include <string>

#include "r2rml/R2RMLMapping.h"
#include "r2rml/R2RMLParser.h"
#include "r2rml/TriplesMap.h"

static void printHelp(const char* programName) {
    std::cerr << "Usage: " << programName << " [options] <mapping.ttl>\n"
              << "\n"
              << "Options:\n"
              << "  -P    Print the loaded R2RML mapping to stdout\n"
              << "  -h    Show this help message\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printHelp(argv[0]);
        return 1;
    }

    bool printMapping = false;
    const char* mappingFile = nullptr;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
            printHelp(argv[0]);
            return 0;
        } else if (std::strcmp(argv[i], "-P") == 0) {
            printMapping = true;
        } else if (argv[i][0] != '-') {
            mappingFile = argv[i];
        } else {
            std::cerr << "Unknown option: " << argv[i] << "\n";
            printHelp(argv[0]);
            return 1;
        }
    }

    if (!mappingFile) {
        std::cerr << "Error: no mapping file specified.\n";
        printHelp(argv[0]);
        return 1;
    }

    r2rml::R2RMLParser parser;
    r2rml::R2RMLMapping mapping = parser.parse(mappingFile);

    if (printMapping) {
        std::cout << mapping << "\n";
    }

    return 0;
}
