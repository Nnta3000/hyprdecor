# Runs on every build, not at configure time. Rewrites the header only when its text
# changes so a build with no new commit does not recompile every consumer.
execute_process(
    COMMAND git describe --always --dirty
    WORKING_DIRECTORY "${HYPRDECOR_SOURCE_DIR}"
    OUTPUT_VARIABLE COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT COMMIT)
    set(COMMIT "unknown")
endif()

string(TIMESTAMP BUILD_DATE "%Y-%m-%d" UTC)

set(CONTENT "#pragma once\n")
string(APPEND CONTENT "#define HYPRDECOR_VERSION \"${HYPRDECOR_PROJECT_VERSION}\"\n")
string(APPEND CONTENT "#define HYPRDECOR_GIT_COMMIT \"${COMMIT}\"\n")
string(APPEND CONTENT "#define HYPRDECOR_BUILD_DATE \"${BUILD_DATE}\"\n")

if(EXISTS "${HYPRDECOR_OUTPUT}")
    file(READ "${HYPRDECOR_OUTPUT}" OLD_CONTENT)
else()
    set(OLD_CONTENT "")
endif()

if(NOT OLD_CONTENT STREQUAL CONTENT)
    file(WRITE "${HYPRDECOR_OUTPUT}" "${CONTENT}")
endif()
