# Self-update: GitHub release check, resumable asset downloads, miniz unpacking and the app swap.

add_library(playden_miniz STATIC "${CMAKE_SOURCE_DIR}/third_party/miniz/miniz.c")
target_include_directories(playden_miniz PUBLIC "${CMAKE_SOURCE_DIR}/third_party/miniz")
target_compile_definitions(playden_miniz PUBLIC MINIZ_NO_TIME MINIZ_NO_ZLIB_COMPATIBLE_NAMES MINIZ_NO_ARCHIVE_WRITING_APIS)
target_compile_options(playden_miniz PRIVATE -w)

add_library(playden_self_update STATIC
    "${CMAKE_SOURCE_DIR}/src/update/version.cpp"
    "${CMAKE_SOURCE_DIR}/src/update/file_ops.cpp"
    "${CMAKE_SOURCE_DIR}/src/update/zip_extract.cpp"
    "${CMAKE_SOURCE_DIR}/src/update/release.cpp"
    "${CMAKE_SOURCE_DIR}/src/update/asset_download.cpp"
    "${CMAKE_SOURCE_DIR}/src/update/app_swap.cpp"
    "${CMAKE_SOURCE_DIR}/src/update/update_service.cpp")
target_include_directories(playden_self_update PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_compile_features(playden_self_update PUBLIC cxx_std_17)
target_compile_definitions(playden_self_update PRIVATE PLAYDEN_VERSION="${PLAYDEN_FULL_VERSION}"
    PLAYDEN_BUILD_COMMIT="${PLAYDEN_BUILD_COMMIT}")
target_link_libraries(playden_self_update PUBLIC playden_miniz playden_steam_protocol playden_steam_http Threads::Threads)
if(PLAYDEN_PLATFORM_PS5)
    target_compile_options(playden_self_update PRIVATE -fno-exceptions -fno-rtti)
endif()

if(BUILD_TESTING AND NOT PLAYDEN_PLATFORM_PS5)
    foreach(test_name self_update_version self_update_zip self_update_release self_update_download
                      self_update_swap self_update_service)
        add_executable(${test_name}_test "${CMAKE_SOURCE_DIR}/tests/${test_name}_test.cpp")
        target_link_libraries(${test_name}_test PRIVATE playden_self_update)
        target_compile_definitions(${test_name}_test PRIVATE PLAYDEN_VERSION="${PLAYDEN_FULL_VERSION}"
            PLAYDEN_BUILD_COMMIT="${PLAYDEN_BUILD_COMMIT}")
        add_test(NAME ${test_name} COMMAND ${test_name}_test)
    endforeach()
endif()

if(BUILD_TESTING AND NOT PLAYDEN_PLATFORM_PS5)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME write_update_manifest
             COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/write_update_manifest_test.py")
endif()
