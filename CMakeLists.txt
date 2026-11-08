cmake_minimum_required(VERSION 3.24)
project(PlaydenLauncher VERSION 0.11.0 LANGUAGES C CXX)

set(PLAYDEN_PRERELEASE "" CACHE STRING "Pre-release suffix: empty, beta.N (1-999999) or nightly.YYYYMMDD.N (1-999)")
set(PLAYDEN_FULL_VERSION "${PROJECT_VERSION}")
if(NOT PLAYDEN_PRERELEASE STREQUAL "")
    if(NOT PLAYDEN_PRERELEASE MATCHES "^beta\\.[1-9][0-9]?[0-9]?[0-9]?[0-9]?[0-9]?$" AND
       NOT PLAYDEN_PRERELEASE MATCHES "^nightly\\.20[0-9][0-9](0[1-9]|1[0-2])(0[1-9]|[12][0-9]|3[01])\\.[1-9][0-9]?[0-9]?$")
        message(FATAL_ERROR "PLAYDEN_PRERELEASE must be empty, beta.N (1-999999) or nightly.YYYYMMDD.N (1-999)")
    endif()
    string(APPEND PLAYDEN_FULL_VERSION "-${PLAYDEN_PRERELEASE}")
endif()
# The release a nightly is built on: it isn't offered that release or an older one. Other builds ignore it.
set(PLAYDEN_NIGHTLY_BASE "" CACHE STRING "A nightly's base release: empty, X.Y.Z or X.Y.Z-beta.N")
if(NOT PLAYDEN_NIGHTLY_BASE STREQUAL "" AND
   NOT PLAYDEN_NIGHTLY_BASE MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+(-beta\\.[1-9][0-9]*)?$")
    message(FATAL_ERROR "PLAYDEN_NIGHTLY_BASE must be empty, X.Y.Z or X.Y.Z-beta.N")
endif()
set(PLAYDEN_BUILD_NIGHTLY_BASE "")
if(PLAYDEN_PRERELEASE MATCHES "^nightly\\.")
    set(PLAYDEN_BUILD_NIGHTLY_BASE "${PLAYDEN_NIGHTLY_BASE}")
endif()

# The commit the build is made from and whether it had local changes, for Settings › About and what Playden shares
# (BuildVersionText). Checked again on every build, not only at configure: files can change without a new commit.
find_package(Git QUIET)
set(PLAYDEN_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
set(playden_build_commit_command "${CMAKE_COMMAND}" "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}" "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
    "-DOUTPUT=${PLAYDEN_GENERATED_DIR}/playden_build_commit.h" -P "${CMAKE_SOURCE_DIR}/cmake/WriteBuildCommit.cmake")
execute_process(COMMAND ${playden_build_commit_command})
add_custom_target(playden_build_commit COMMAND ${playden_build_commit_command}
    BYPRODUCTS "${PLAYDEN_GENERATED_DIR}/playden_build_commit.h" VERBATIM)

option(PLAYDEN_UI_SIMULATION "Package selected PS5 UI with developer simulation fixtures" OFF)
option(PLAYDEN_PLATFORM_PS5 "Build a PS5 native title" OFF)
# Local testing only: read other community recipe endpoints from dev-endpoints.json in the user
# data folder (plain http and an auth header allowed for them). Never on in a release package.
option(PLAYDEN_DEV_ENDPOINTS "Compile in the dev-endpoints.json override for community recipes" OFF)
option(PLAYDEN_DEV_BUILD "A local build for testing: every experimental feature on by default; never a release" OFF)
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
include(cmake/CommunityRecipes.cmake)
