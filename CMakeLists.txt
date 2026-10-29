cmake_minimum_required(VERSION 3.24)
project(PlaydenLauncher VERSION 0.9.0 LANGUAGES C CXX)

set(PLAYDEN_PRERELEASE "" CACHE STRING "Pre-release suffix: empty, beta.N (1-999999) or nightly.YYYYMMDD.N (1-999)")
set(PLAYDEN_FULL_VERSION "${PROJECT_VERSION}")
if(NOT PLAYDEN_PRERELEASE STREQUAL "")
    if(NOT PLAYDEN_PRERELEASE MATCHES "^beta\\.[1-9][0-9]?[0-9]?[0-9]?[0-9]?[0-9]?$" AND
       NOT PLAYDEN_PRERELEASE MATCHES "^nightly\\.20[0-9][0-9](0[1-9]|1[0-2])(0[1-9]|[12][0-9]|3[01])\\.[1-9][0-9]?[0-9]?$")
        message(FATAL_ERROR "PLAYDEN_PRERELEASE must be empty, beta.N (1-999999) or nightly.YYYYMMDD.N (1-999)")
    endif()
    string(APPEND PLAYDEN_FULL_VERSION "-${PLAYDEN_PRERELEASE}")
endif()

# The commit the build is made from, for Settings › About. Configure runs again whenever HEAD moves.
set(PLAYDEN_BUILD_COMMIT "")
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --short=10 HEAD WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE PLAYDEN_BUILD_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE playden_git_result)
    if(NOT playden_git_result EQUAL 0 OR NOT PLAYDEN_BUILD_COMMIT MATCHES "^[0-9a-f]+$")
        set(PLAYDEN_BUILD_COMMIT "")
    endif()
    # HEAD names the branch; the branch's ref (loose or packed) and HEAD's reflog change with each commit.
    execute_process(COMMAND "${GIT_EXECUTABLE}" symbolic-ref -q HEAD WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE playden_head_ref OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    set(playden_git_paths HEAD logs/HEAD packed-refs)
    if(playden_head_ref)
        list(APPEND playden_git_paths "${playden_head_ref}")
    endif()
    foreach(playden_git_path IN LISTS playden_git_paths)
        execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --path-format=absolute --git-path "${playden_git_path}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" OUTPUT_VARIABLE playden_git_file OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(playden_git_file AND EXISTS "${playden_git_file}")
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${playden_git_file}")
        endif()
    endforeach()
endif()

option(PLAYDEN_UI_SIMULATION "Package selected PS5 UI with developer simulation fixtures" OFF)
option(PLAYDEN_PLATFORM_PS5 "Build a PS5 native title" OFF)
set(PLAYDEN_PS5_TITLE_ID "" CACHE STRING "Explicit PS5 title id (required for PS5 packaging)")

file(READ "${CMAKE_SOURCE_DIR}/dependencies.lock.json" playden_dependency_lock)
include(CTest)
include(cmake/PlaydenCore.cmake)
include(cmake/SteamBackend.cmake)
playden_register_steam_backend_tests()
include(cmake/Web.cmake)
include(cmake/ProsperoRuntime.cmake)
include(cmake/SelfUpdate.cmake)
include(cmake/HomebrewUI.cmake)
