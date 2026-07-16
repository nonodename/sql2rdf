# Dependency checksums and verification for SQL2RDF++
# This file defines SHA256 hashes for FetchContent dependencies and
# provides verification for git submodules.

# SHA256 hashes for FetchContent dependencies
# To update: download the release tarball and run: shasum -a 256 <file>
set(CATCH2_SHA256 "be23a52b85cf04cd9587612147a10b023d59ed9757fa1843cc99e615d6c0893c")
set(YAML_CPP_SHA256 "25cb043240f828a8c51beb830569634bc7ac603978e0f69d6b63558dadefd49a")
set(DUCKDB_SHA256 "43645e15419c6539bae6915ba397de6569e4a7ca0d502be95d653a78fdb0bece")

# Expected commit hash for Serd submodule
# To update: run: git -C external/serd rev-parse HEAD
set(SERD_EXPECTED_COMMIT "24474530f8acef5c6cdcefa00b0049367e5d3079")

# Function to verify git submodule commit hash
function(verify_submodule_commit SUBMODULE_PATH EXPECTED_COMMIT)
    if(EXISTS "${SUBMODULE_PATH}/.git")
        execute_process(
            COMMAND git rev-parse HEAD
            WORKING_DIRECTORY "${SUBMODULE_PATH}"
            OUTPUT_VARIABLE ACTUAL_COMMIT
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        
        if(ACTUAL_COMMIT STREQUAL "")
            message(FATAL_ERROR "Failed to get commit hash for submodule at ${SUBMODULE_PATH}")
        endif()
        
        if(NOT ACTUAL_COMMIT STREQUAL EXPECTED_COMMIT)
            message(FATAL_ERROR 
                "Submodule commit hash mismatch for ${SUBMODULE_PATH}!\n"
                "Expected: ${EXPECTED_COMMIT}\n"
                "Actual:   ${ACTUAL_COMMIT}\n"
                "Run: git submodule update --init --recursive"
            )
        else()
            message(STATUS "Submodule ${SUBMODULE_PATH} verified (commit: ${ACTUAL_COMMIT})")
        endif()
    else()
        message(FATAL_ERROR "Submodule directory ${SUBMODULE_PATH} does not exist or is not initialized. Run: git submodule update --init --recursive")
    endif()
endfunction()
