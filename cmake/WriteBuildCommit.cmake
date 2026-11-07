# SPDX-License-Identifier: GPL-3.0-or-later
# Runs on every build (cmake -P): writes OUTPUT with the commit the source is at and whether tracked files, submodules
# included, differ from it. A build with local changes then says so in Settings › About and in what it shares.
# Untracked files don't count: a source file only builds once a tracked CMake file names it. OUTPUT is rewritten only
# when it changes, so an unchanged checkout rebuilds nothing.
set(commit "")
set(changes 0)
if(GIT_EXECUTABLE)
    execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --short=10 HEAD WORKING_DIRECTORY "${SOURCE_DIR}"
        OUTPUT_VARIABLE commit OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE result)
    if(NOT result EQUAL 0 OR NOT commit MATCHES "^[0-9a-f]+$")
        set(commit "")
    else()
        execute_process(COMMAND "${GIT_EXECUTABLE}" --no-optional-locks status --porcelain --untracked-files=no
            WORKING_DIRECTORY "${SOURCE_DIR}" OUTPUT_VARIABLE status ERROR_QUIET RESULT_VARIABLE result)
        # A status that can't be read counts as changed: the build can't say it is the commit.
        if(NOT result EQUAL 0 OR NOT status STREQUAL "")
            set(changes 1)
        endif()
    endif()
endif()
set(content "// Written by cmake/WriteBuildCommit.cmake on each build.\n#define PLAYDEN_BUILD_COMMIT \"${commit}\"\n#define PLAYDEN_BUILD_LOCAL_CHANGES ${changes}\n")
set(old "")
if(EXISTS "${OUTPUT}")
    file(READ "${OUTPUT}" old)
endif()
if(NOT old STREQUAL content)
    file(WRITE "${OUTPUT}" "${content}")
endif()
