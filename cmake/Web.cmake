# Playden's read-only web access on the local network (src/web/): the HTTP server, its routes and the page.

add_library(playden_web_http STATIC
    "${CMAKE_SOURCE_DIR}/src/web/address.cpp"
    "${CMAKE_SOURCE_DIR}/src/web/http.cpp"
    "${CMAKE_SOURCE_DIR}/src/web/router.cpp"
    "${CMAKE_SOURCE_DIR}/src/web/server.cpp"
    "${CMAKE_SOURCE_DIR}/src/web/zip.cpp")
target_include_directories(playden_web_http PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_compile_features(playden_web_http PUBLIC cxx_std_17)
target_link_libraries(playden_web_http PUBLIC Threads::Threads)
if(PLAYDEN_PLATFORM_PS5)
    target_compile_options(playden_web_http PRIVATE -fno-exceptions -fno-rtti)
endif()

# The logs a report holds, read and redacted as tools/collect_logs.py does.
add_library(playden_web_logs STATIC "${CMAKE_SOURCE_DIR}/src/web/logs_source.cpp")
target_include_directories(playden_web_logs PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_compile_features(playden_web_logs PUBLIC cxx_std_17)
target_link_libraries(playden_web_logs PUBLIC playden_web_http PRIVATE playden_diagnostic_log playden_steam_protocol)
if(PLAYDEN_PLATFORM_PS5)
    target_compile_definitions(playden_web_logs PRIVATE PLAYDEN_PLATFORM_PS5=1)
    target_compile_options(playden_web_logs PRIVATE -fno-exceptions -fno-rtti)
endif()

# The page (CSS and JS inline) and two Barlow faces (OFL), baked into the binary.
set(playden_web_fonts "${CMAKE_SOURCE_DIR}/fixtures/ui-comparison/assets/fonts")
set(playden_web_assets
    "/|html|${CMAKE_SOURCE_DIR}/src/web/page/index.html"
    "/assets/barlow-regular.ttf|ttf|${playden_web_fonts}/Barlow-Regular.ttf"
    "/assets/barlow-semibold.ttf|ttf|${playden_web_fonts}/Barlow-SemiBold.ttf"
    "/assets/barlow-OFL.txt|text|${playden_web_fonts}/barlow-OFL.txt")
set(playden_web_asset_files)
foreach(asset IN LISTS playden_web_assets)
    string(REPLACE "|" ";" parts "${asset}")
    list(GET parts 2 source)
    list(APPEND playden_web_asset_files "${source}")
endforeach()
set(playden_web_assets_cpp "${CMAKE_BINARY_DIR}/generated/web_assets.cpp")
add_custom_command(OUTPUT "${playden_web_assets_cpp}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/generated"
    COMMAND "${CMAKE_COMMAND}" "-DOUTPUT=${playden_web_assets_cpp}" "-DASSETS=${playden_web_assets}"
        -P "${CMAKE_SOURCE_DIR}/cmake/EmbedWebAssets.cmake"
    DEPENDS ${playden_web_asset_files} "${CMAKE_SOURCE_DIR}/cmake/EmbedWebAssets.cmake"
    VERBATIM)

add_library(playden_web STATIC
    "${CMAKE_SOURCE_DIR}/src/web/service.cpp"
    "${CMAKE_SOURCE_DIR}/src/web/files_api.cpp"
    "${CMAKE_SOURCE_DIR}/src/web/file_policy.cpp"
    "${CMAKE_SOURCE_DIR}/src/web/file_store.cpp"
    "${CMAKE_SOURCE_DIR}/src/web/pairing.cpp"
    "${playden_web_assets_cpp}")
target_include_directories(playden_web PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_compile_features(playden_web PUBLIC cxx_std_17)
target_link_libraries(playden_web PUBLIC playden_web_logs playden_web_http playden_managed_directory
    playden_runtime_artifact_hash playden_runtime_private_directories playden_storage)
if(PLAYDEN_PLATFORM_PS5)
    target_compile_definitions(playden_web PRIVATE PLAYDEN_PLATFORM_PS5=1)
    target_compile_options(playden_web PRIVATE -fno-exceptions -fno-rtti)
endif()

if(BUILD_TESTING AND NOT PLAYDEN_PLATFORM_PS5)
    foreach(test_name web_request web_router web_zip web_server)
        add_executable(${test_name}_test "${CMAKE_SOURCE_DIR}/tests/${test_name}_test.cpp")
        target_link_libraries(${test_name}_test PRIVATE playden_web_http)
        add_test(NAME ${test_name} COMMAND ${test_name}_test)
    endforeach()
    add_executable(web_logs_source_test "${CMAKE_SOURCE_DIR}/tests/web_logs_source_test.cpp")
    target_link_libraries(web_logs_source_test PRIVATE playden_web_logs)
    add_test(NAME web_logs_source COMMAND web_logs_source_test)
    # The console's walk by path (no descriptor-relative opens), run on the host.
    add_executable(web_logs_source_native_policy_test
        "${CMAKE_SOURCE_DIR}/tests/web_logs_source_test.cpp"
        "${CMAKE_SOURCE_DIR}/src/web/logs_source.cpp")
    target_compile_definitions(web_logs_source_native_policy_test PRIVATE PLAYDEN_PLATFORM_PS5=1)
    target_link_libraries(web_logs_source_native_policy_test PRIVATE playden_web_http playden_diagnostic_log playden_steam_protocol)
    add_test(NAME web_logs_source_native_policy COMMAND web_logs_source_native_policy_test)
    add_executable(web_service_test "${CMAKE_SOURCE_DIR}/tests/web_service_test.cpp")
    target_link_libraries(web_service_test PRIVATE playden_web)
    add_test(NAME web_service COMMAND web_service_test)
    add_executable(web_pairing_test "${CMAKE_SOURCE_DIR}/tests/web_pairing_test.cpp")
    target_link_libraries(web_pairing_test PRIVATE playden_web)
    add_test(NAME web_pairing COMMAND web_pairing_test)
    # The console's path handling for remembered browsers (no descriptor-relative opens), run on the host.
    add_executable(web_pairing_native_policy_test
        "${CMAKE_SOURCE_DIR}/tests/web_pairing_test.cpp"
        "${CMAKE_SOURCE_DIR}/src/web/pairing.cpp"
        "${CMAKE_SOURCE_DIR}/src/runtime/private_directories.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/managed_directory.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/storage.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/durable_storage.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/effective_uid.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/random_id.cpp")
    target_include_directories(web_pairing_native_policy_test PRIVATE "${CMAKE_SOURCE_DIR}/src")
    target_compile_features(web_pairing_native_policy_test PRIVATE cxx_std_17)
    target_compile_definitions(web_pairing_native_policy_test PRIVATE PLAYDEN_PLATFORM_PS5=1 PLAYDEN_TEST_AT_UNAVAILABLE=1)
    target_link_libraries(web_pairing_native_policy_test PRIVATE playden_runtime_artifact_hash Threads::Threads)
    add_test(NAME web_pairing_native_policy COMMAND web_pairing_native_policy_test)
    add_executable(web_file_policy_test "${CMAKE_SOURCE_DIR}/tests/web_file_policy_test.cpp")
    target_link_libraries(web_file_policy_test PRIVATE playden_web)
    add_test(NAME web_file_policy COMMAND web_file_policy_test)
    add_executable(web_file_store_test "${CMAKE_SOURCE_DIR}/tests/web_file_store_test.cpp")
    target_link_libraries(web_file_store_test PRIVATE playden_web)
    add_test(NAME web_file_store COMMAND web_file_store_test)
    add_executable(web_files_api_test "${CMAKE_SOURCE_DIR}/tests/web_files_api_test.cpp")
    target_link_libraries(web_files_api_test PRIVATE playden_web)
    add_test(NAME web_files_api COMMAND web_files_api_test)
    # A real server over a made-up game for a browser or a script; prints each pairing code.
    add_executable(web_files_fixture_server "${CMAKE_SOURCE_DIR}/tests/web_files_fixture_server.cpp")
    target_link_libraries(web_files_fixture_server PRIVATE playden_web)
    add_test(NAME web_files_smoke COMMAND python3 -I "${CMAKE_SOURCE_DIR}/tests/test_web_files_smoke.py" $<TARGET_FILE:web_files_fixture_server>)
    # The console's path handling (no descriptor-relative opens), run on the host.
    add_executable(web_file_store_native_policy_test
        "${CMAKE_SOURCE_DIR}/tests/web_file_store_test.cpp"
        "${CMAKE_SOURCE_DIR}/src/web/file_store.cpp"
        "${CMAKE_SOURCE_DIR}/src/web/file_policy.cpp"
        "${CMAKE_SOURCE_DIR}/src/web/zip.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/managed_directory.cpp")
    target_include_directories(web_file_store_native_policy_test PRIVATE "${CMAKE_SOURCE_DIR}/src")
    target_compile_features(web_file_store_native_policy_test PRIVATE cxx_std_17)
    target_compile_definitions(web_file_store_native_policy_test PRIVATE PLAYDEN_PLATFORM_PS5=1 PLAYDEN_TEST_AT_UNAVAILABLE=1)
    target_link_libraries(web_file_store_native_policy_test PRIVATE playden_runtime_artifact_hash playden_storage playden_curl_8_22 Threads::Threads)
    add_test(NAME web_file_store_native_policy COMMAND web_file_store_native_policy_test)
endif()
