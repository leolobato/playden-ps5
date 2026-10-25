# Renderer-independent Steam protocol and verified HTTP transport targets.

add_library(playden_steam_protocol STATIC
    "${CMAKE_SOURCE_DIR}/src/steam/backend/proto_wire.cpp"
    "${CMAKE_SOURCE_DIR}/src/steam/backend/auth_messages.cpp"
    "${CMAKE_SOURCE_DIR}/src/steam/backend/cm_messages.cpp"
    "${CMAKE_SOURCE_DIR}/src/steam/backend/game_tickets.cpp"
    "${CMAKE_SOURCE_DIR}/src/steam/backend/json.cpp"
    "${CMAKE_SOURCE_DIR}/src/steam/backend/token_claims.cpp")
target_include_directories(playden_steam_protocol PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_compile_features(playden_steam_protocol PUBLIC cxx_std_17)
add_library(playden_steam_protobuf ALIAS playden_steam_protocol)

if(PLAYDEN_PLATFORM_PS5)
    set(playden_steam_dependency_platform ps5)
else()
    set(playden_steam_dependency_platform host)
endif()

add_library(playden_steam_auth STATIC
    "${CMAKE_SOURCE_DIR}/src/steam/backend/auth_client.cpp"
    "${CMAKE_SOURCE_DIR}/src/steam/backend/password_crypto.cpp")
target_include_directories(playden_steam_auth PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_compile_features(playden_steam_auth PUBLIC cxx_std_17)
target_link_libraries(playden_steam_auth PUBLIC
    playden_steam_http playden_steam_protocol playden_steam_core)
if(PLAYDEN_PLATFORM_PS5)
    target_compile_options(playden_steam_auth PRIVATE -fno-exceptions -fno-rtti)
endif()
add_library(playden_steam_library STATIC
    "${CMAKE_SOURCE_DIR}/src/steam/backend/library_client.cpp")
target_include_directories(playden_steam_library PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_compile_features(playden_steam_library PUBLIC cxx_std_17)
target_link_libraries(playden_steam_library PUBLIC
    playden_steam_http playden_steam_protocol playden_library_service)
if(PLAYDEN_PLATFORM_PS5)
    target_compile_options(playden_steam_library PRIVATE -fno-exceptions -fno-rtti)
endif()
set(PLAYDEN_STEAM_PREFIX
    "${CMAKE_SOURCE_DIR}/scripts/network-deps/.deps/${playden_steam_dependency_platform}"
    CACHE PATH "Pinned curl/Mbed TLS prefix prepared by scripts/network-deps")

foreach(required_path IN ITEMS
        "${PLAYDEN_STEAM_PREFIX}/include/curl/curl.h"
        "${PLAYDEN_STEAM_PREFIX}/lib/libcurl.a"
        "${PLAYDEN_STEAM_PREFIX}/cacert.pem")
    if(NOT EXISTS "${required_path}")
        message(FATAL_ERROR
            "Pinned Steam dependency is missing: ${required_path}. "
            "Run scripts/network-deps/prepare-${playden_steam_dependency_platform}-deps.sh")
    endif()
endforeach()

# Threading changes public context layouts, so headers and all linked archives must be rebuilt together.
set(playden_mbedtls_config "${PLAYDEN_STEAM_PREFIX}/include/mbedtls/mbedtls_config.h")
string(JSON expected_mbedtls_config_hash GET "${playden_dependency_lock}"
    steam_network_probe mbedtls config_sha256)
file(SHA256 "${playden_mbedtls_config}" actual_mbedtls_config_hash)
if(NOT actual_mbedtls_config_hash STREQUAL expected_mbedtls_config_hash)
    message(FATAL_ERROR "Pinned Mbed TLS pthread configuration mismatch. "
        "Rebuild with scripts/network-deps/prepare-${playden_steam_dependency_platform}-deps.sh")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${playden_mbedtls_config}")

file(GLOB playden_curl_headers CONFIGURE_DEPENDS
    "${PLAYDEN_STEAM_PREFIX}/include/curl/*.h")
string(JSON expected_header_count LENGTH "${playden_dependency_lock}"
    steam_network_probe public_headers)
list(LENGTH playden_curl_headers actual_header_count)
if(NOT actual_header_count EQUAL expected_header_count)
    message(FATAL_ERROR "Pinned curl public-header set mismatch")
endif()
foreach(header_path IN LISTS playden_curl_headers)
    file(RELATIVE_PATH header_key "${PLAYDEN_STEAM_PREFIX}/include" "${header_path}")
    string(JSON expected_hash GET "${playden_dependency_lock}"
        steam_network_probe public_headers "${header_key}")
    file(SHA256 "${header_path}" actual_hash)
    if(NOT actual_hash STREQUAL expected_hash)
        message(FATAL_ERROR "Pinned curl header hash mismatch: ${header_key}")
    endif()
endforeach()
set(playden_steam_archives)
foreach(archive libcurl.a libeverest.a libmbedcrypto.a libmbedtls.a
                libmbedx509.a libp256m.a)
    set(archive_path "${PLAYDEN_STEAM_PREFIX}/lib/${archive}")
    # PS5 archives come from the pinned SDK and LLVM 18 with relative source paths, so they are
    # byte-reproducible and pinned. Host archives depend on the local Xcode toolchain; their
    # pinned sources are checked when prepare-host-deps.sh builds them.
    if(PLAYDEN_PLATFORM_PS5)
        string(JSON expected_hash GET "${playden_dependency_lock}"
            steam_network_probe ps5_archives "${archive}")
        file(SHA256 "${archive_path}" actual_hash)
        if(NOT actual_hash STREQUAL expected_hash)
            message(FATAL_ERROR "Pinned ps5 archive hash mismatch: ${archive}. "
                "Rebuild with scripts/network-deps/prepare-ps5-deps.sh")
        endif()
    endif()
    list(APPEND playden_steam_archives "${archive_path}")
endforeach()
string(JSON expected_ca_hash GET "${playden_dependency_lock}"
    steam_network_probe ca_bundle sha256)
file(SHA256 "${PLAYDEN_STEAM_PREFIX}/cacert.pem" actual_ca_hash)
if(NOT actual_ca_hash STREQUAL expected_ca_hash)
    message(FATAL_ERROR "Pinned Steam CA bundle hash mismatch")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/dependencies.lock.json" ${playden_curl_headers}
    ${playden_steam_archives} "${PLAYDEN_STEAM_PREFIX}/cacert.pem")

add_library(playden_curl_8_22 STATIC IMPORTED GLOBAL)
set_target_properties(playden_curl_8_22 PROPERTIES
    IMPORTED_LOCATION "${PLAYDEN_STEAM_PREFIX}/lib/libcurl.a"
    INTERFACE_INCLUDE_DIRECTORIES "${PLAYDEN_STEAM_PREFIX}/include"
    INTERFACE_COMPILE_DEFINITIONS CURL_STATICLIB
    INTERFACE_LINK_LIBRARIES
        "${PLAYDEN_STEAM_PREFIX}/lib/libmbedtls.a;${PLAYDEN_STEAM_PREFIX}/lib/libmbedx509.a;${PLAYDEN_STEAM_PREFIX}/lib/libmbedcrypto.a;${PLAYDEN_STEAM_PREFIX}/lib/libeverest.a;${PLAYDEN_STEAM_PREFIX}/lib/libp256m.a;Threads::Threads")
if(APPLE AND NOT PLAYDEN_PLATFORM_PS5)
    set_property(TARGET playden_curl_8_22 APPEND PROPERTY INTERFACE_LINK_LIBRARIES
        "-framework SystemConfiguration" "-framework CoreFoundation"
        "-framework CoreServices")
endif()

# Require the threading implementation to link, catching new headers with old host archives.
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_LIBRARIES playden_curl_8_22)
unset(playden_mbedtls_threading_links CACHE)
check_cxx_source_compiles("#include <mbedtls/threading.h>
int main() { mbedtls_threading_mutex_t mutex; mbedtls_mutex_init(&mutex);
  int result = mbedtls_mutex_lock(&mutex); mbedtls_mutex_unlock(&mutex);
  mbedtls_mutex_free(&mutex); return result; }" playden_mbedtls_threading_links)
unset(CMAKE_REQUIRED_LIBRARIES)
if(NOT playden_mbedtls_threading_links)
    message(FATAL_ERROR "Mbed TLS pthread headers/libraries mismatch. Rebuild network dependencies together.")
endif()

add_library(playden_steam_http STATIC
    "${CMAKE_SOURCE_DIR}/src/steam/backend/http_client.cpp"
    "${CMAKE_SOURCE_DIR}/src/steam/backend/ws_client.cpp"
    "${CMAKE_SOURCE_DIR}/src/steam/backend/native_network.cpp")
target_include_directories(playden_steam_http PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_compile_features(playden_steam_http PUBLIC cxx_std_17)
target_link_libraries(playden_steam_http PUBLIC playden_curl_8_22 Threads::Threads)
if(PLAYDEN_PLATFORM_PS5)
    target_compile_definitions(playden_steam_http PRIVATE PLAYDEN_PLATFORM_PS5=1)
    target_compile_options(playden_steam_http PRIVATE -fno-exceptions -fno-rtti)
endif()

# Authenticated CM session for the CM-only library facts (acquisition dates).
add_library(playden_steam_cm STATIC
    "${CMAKE_SOURCE_DIR}/src/steam/backend/cm_client.cpp"
    "${CMAKE_SOURCE_DIR}/src/steam/backend/entitled_library.cpp"
    "${CMAKE_SOURCE_DIR}/src/steam/backend/cm_app_details.cpp"
    "${CMAKE_SOURCE_DIR}/src/steam/backend/cdn_client.cpp"
    "${CMAKE_SOURCE_DIR}/src/steam/backend/depot_installer.cpp"
    "${CMAKE_SOURCE_DIR}/src/steam/backend/steam_install_runner.cpp"
    "${CMAKE_SOURCE_DIR}/src/steam/backend/cloud_messages.cpp"
    "${CMAKE_SOURCE_DIR}/src/steam/backend/cloud_mapping.cpp"
    "${CMAKE_SOURCE_DIR}/src/steam/backend/cloud_sync.cpp"
    "${CMAKE_SOURCE_DIR}/src/steam/backend/steam_cloud_runner.cpp")
target_include_directories(playden_steam_cm PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_compile_features(playden_steam_cm PUBLIC cxx_std_17)
target_link_libraries(playden_steam_cm PUBLIC
    playden_steam_http playden_steam_protocol playden_steam_cm_codec playden_depot_codec playden_steam_core
    playden_runtime_steamworks)
if(PLAYDEN_PLATFORM_PS5)
    target_compile_definitions(playden_steam_cm PRIVATE PLAYDEN_PLATFORM_PS5=1)
    target_compile_options(playden_steam_cm PRIVATE -fno-exceptions -fno-rtti)
endif()

# "Copy into Playden" for imported games: the install service's runner for Store::kLocal (it
# removes partial copies with the depot installer's RemoveManagedInstall).
add_library(playden_local_copy STATIC "${CMAKE_SOURCE_DIR}/src/stores/local/copy_runner.cpp")
target_include_directories(playden_local_copy PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_compile_features(playden_local_copy PUBLIC cxx_std_17)
target_link_libraries(playden_local_copy PUBLIC playden_local_store playden_steam_cm playden_steam_core)
if(PLAYDEN_PLATFORM_PS5)
    target_compile_definitions(playden_local_copy PRIVATE PLAYDEN_PLATFORM_PS5=1)
    target_compile_options(playden_local_copy PRIVATE -fno-exceptions -fno-rtti)
endif()

set(PLAYDEN_STEAM_CA_BUNDLE "${PLAYDEN_STEAM_PREFIX}/cacert.pem"
    CACHE INTERNAL "Verified Steam HTTPS trust bundle")
set(PLAYDEN_STEAM_LINK_ARCHIVES
    "${PLAYDEN_STEAM_PREFIX}/lib/libcurl.a"
    "${PLAYDEN_STEAM_PREFIX}/lib/libmbedtls.a"
    "${PLAYDEN_STEAM_PREFIX}/lib/libmbedx509.a"
    "${PLAYDEN_STEAM_PREFIX}/lib/libmbedcrypto.a"
    "${PLAYDEN_STEAM_PREFIX}/lib/libeverest.a"
    "${PLAYDEN_STEAM_PREFIX}/lib/libp256m.a"
    CACHE INTERNAL "Verified native Steam transport archives")

set(playden_staged_ca "${CMAKE_BINARY_DIR}/trust/cacert.pem")
add_custom_command(OUTPUT "${playden_staged_ca}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/trust"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${PLAYDEN_STEAM_CA_BUNDLE}" "${playden_staged_ca}"
    DEPENDS "${PLAYDEN_STEAM_CA_BUNDLE}" VERBATIM)
add_custom_target(playden_steam_trust ALL DEPENDS "${playden_staged_ca}")

function(playden_register_steam_backend_tests)
    if(NOT BUILD_TESTING OR PLAYDEN_PLATFORM_PS5)
        return()
    endif()

    foreach(test_name proto_wire auth_messages)
        add_executable(${test_name}_test "${CMAKE_SOURCE_DIR}/tests/${test_name}_test.cpp")
        target_link_libraries(${test_name}_test PRIVATE playden_steam_protocol)
    endforeach()
    add_test(NAME proto_wire COMMAND proto_wire_test)
    add_test(NAME auth_messages COMMAND auth_messages_test
        "${CMAKE_SOURCE_DIR}/fixtures/steam/auth")

    foreach(test_name steam_json token_claims)
        add_executable(${test_name}_test "${CMAKE_SOURCE_DIR}/tests/${test_name}_test.cpp")
        target_link_libraries(${test_name}_test PRIVATE playden_steam_protocol)
        add_test(NAME ${test_name} COMMAND ${test_name}_test)
    endforeach()

    foreach(test_name session_store steam_auth)
        add_executable(${test_name}_test "${CMAKE_SOURCE_DIR}/tests/${test_name}_test.cpp")
        target_link_libraries(${test_name}_test PRIVATE playden_steam_core)
        add_test(NAME ${test_name} COMMAND ${test_name}_test)
    endforeach()

    if(TARGET playden_steam_http)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        add_executable(native_resolution_cache_test "${CMAKE_SOURCE_DIR}/tests/native_resolution_cache_test.cpp")
        target_link_libraries(native_resolution_cache_test PRIVATE playden_steam_http)
        add_test(NAME native_resolution_cache COMMAND native_resolution_cache_test)

        add_executable(mbedtls_threading_test "${CMAKE_SOURCE_DIR}/tests/mbedtls_threading_test.cpp")
        target_link_libraries(mbedtls_threading_test PRIVATE playden_steam_http)
        add_test(NAME mbedtls_threading COMMAND mbedtls_threading_test)

        add_executable(http_client_test "${CMAKE_SOURCE_DIR}/tests/http_client_test.cpp")
        target_link_libraries(http_client_test PRIVATE playden_steam_http)
        add_test(NAME http_client
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/http_fixture_server.py"
                "$<TARGET_FILE:http_client_test>" "${CMAKE_SOURCE_DIR}/tests/fixtures/http")
        set_tests_properties(http_client PROPERTIES TIMEOUT 15)

        add_executable(auth_client_test "${CMAKE_SOURCE_DIR}/tests/auth_client_test.cpp")
        target_link_libraries(auth_client_test PRIVATE playden_steam_auth)
        add_test(NAME auth_client COMMAND auth_client_test)
        add_executable(password_crypto_test "${CMAKE_SOURCE_DIR}/tests/password_crypto_test.cpp")
        target_link_libraries(password_crypto_test PRIVATE playden_steam_auth)
        add_test(NAME password_crypto COMMAND password_crypto_test)

        foreach(test_name cm_messages cm_client)
            add_executable(${test_name}_test "${CMAKE_SOURCE_DIR}/tests/${test_name}_test.cpp")
            target_link_libraries(${test_name}_test PRIVATE playden_steam_cm)
            add_test(NAME ${test_name} COMMAND ${test_name}_test "${CMAKE_SOURCE_DIR}/fixtures/steam/cm")
        endforeach()
        add_executable(steam_game_tickets_test "${CMAKE_SOURCE_DIR}/tests/steam_game_tickets_test.cpp")
        target_link_libraries(steam_game_tickets_test PRIVATE playden_steam_protocol)
        add_test(NAME steam_game_tickets COMMAND steam_game_tickets_test)
        add_executable(download_rate_test "${CMAKE_SOURCE_DIR}/tests/download_rate_test.cpp")
        target_link_libraries(download_rate_test PRIVATE playden_steam_core)
        add_test(NAME download_rate COMMAND download_rate_test)
        add_executable(install_service_test "${CMAKE_SOURCE_DIR}/tests/install_service_test.cpp")
        target_link_libraries(install_service_test PRIVATE playden_steam_core)
        add_test(NAME install_service COMMAND install_service_test)
        add_executable(cloud_service_test "${CMAKE_SOURCE_DIR}/tests/cloud_service_test.cpp")
        target_link_libraries(cloud_service_test PRIVATE playden_steam_core)
        add_test(NAME cloud_service COMMAND cloud_service_test)
        add_executable(install_volumes_test "${CMAKE_SOURCE_DIR}/tests/install_volumes_test.cpp")
        target_link_libraries(install_volumes_test PRIVATE playden_steam_core)
        add_test(NAME install_volumes COMMAND install_volumes_test)
        add_executable(local_copy_test "${CMAKE_SOURCE_DIR}/tests/local_copy_test.cpp")
        target_link_libraries(local_copy_test PRIVATE playden_local_copy)
        add_test(NAME local_copy COMMAND local_copy_test)
        # The console's walk by path (no descriptor-relative opens), run on the host.
        add_executable(local_copy_native_policy_test "${CMAKE_SOURCE_DIR}/tests/local_copy_test.cpp"
            "${CMAKE_SOURCE_DIR}/src/stores/local/copy_runner.cpp" "${CMAKE_SOURCE_DIR}/src/stores/local/copy_plan.cpp"
            "${CMAKE_SOURCE_DIR}/src/stores/local/scanner.cpp" "${CMAKE_SOURCE_DIR}/src/stores/local/import_store.cpp"
            "${CMAKE_SOURCE_DIR}/src/stores/local/library_source.cpp"
            "${CMAKE_SOURCE_DIR}/src/stores/local/drop_folder_access.cpp")
        target_compile_definitions(local_copy_native_policy_test PRIVATE PLAYDEN_PLATFORM_PS5=1)
        target_link_libraries(local_copy_native_policy_test PRIVATE playden_steam_cm playden_storage playden_library)
        add_test(NAME local_copy_native_policy COMMAND local_copy_native_policy_test)
        add_executable(depot_installer_test "${CMAKE_SOURCE_DIR}/tests/depot_installer_test.cpp")
        target_link_libraries(depot_installer_test PRIVATE playden_steam_cm)
        add_test(NAME depot_installer COMMAND depot_installer_test)
        add_executable(steam_cloud_test "${CMAKE_SOURCE_DIR}/tests/steam_cloud_test.cpp")
        target_link_libraries(steam_cloud_test PRIVATE playden_steam_cm)
        target_compile_features(steam_cloud_test PRIVATE cxx_std_20)
        add_test(NAME steam_cloud COMMAND steam_cloud_test)
        add_executable(cdn_client_test "${CMAKE_SOURCE_DIR}/tests/cdn_client_test.cpp")
        target_link_libraries(cdn_client_test PRIVATE playden_steam_cm)
        add_test(NAME cdn_client COMMAND cdn_client_test)
        add_test(NAME download_speed_report
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_download_speed_report.py")
        add_executable(store_details_test "${CMAKE_SOURCE_DIR}/tests/store_details_test.cpp")
        target_link_libraries(store_details_test PRIVATE playden_steam_core)
        add_test(NAME store_details COMMAND store_details_test)
        add_executable(app_details_test "${CMAKE_SOURCE_DIR}/tests/app_details_test.cpp")
        target_link_libraries(app_details_test PRIVATE playden_steam_cm)
        add_test(NAME app_details COMMAND app_details_test)
        add_executable(entitled_library_test "${CMAKE_SOURCE_DIR}/tests/entitled_library_test.cpp")
        target_link_libraries(entitled_library_test PRIVATE playden_steam_cm)
        add_test(NAME entitled_library COMMAND entitled_library_test)

        add_executable(library_client_test "${CMAKE_SOURCE_DIR}/tests/library_client_test.cpp")
        target_link_libraries(library_client_test PRIVATE playden_steam_library)
        add_test(NAME library_client COMMAND library_client_test
            "${CMAKE_SOURCE_DIR}/fixtures/steam/library")
    endif()
endfunction()
