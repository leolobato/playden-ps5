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

add_library(playden_web STATIC "${CMAKE_SOURCE_DIR}/src/web/service.cpp" "${playden_web_assets_cpp}")
target_include_directories(playden_web PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_compile_features(playden_web PUBLIC cxx_std_17)
target_link_libraries(playden_web PUBLIC playden_web_logs playden_web_http)
if(PLAYDEN_PLATFORM_PS5)
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
endif()
