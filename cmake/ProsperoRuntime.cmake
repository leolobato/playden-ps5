# Offline kit admission; ordinary launcher builds do not compile Wine.
set(PLAYDEN_PROSPERO_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/prospero-win" CACHE PATH
    "Exact pinned prospero-win source; overrides must match dependencies.lock.json")
option(PLAYDEN_PROSPERO_RELEASE_SOURCE "Require a published prospero source pin" OFF)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(prospero_source_check_args --source "${PLAYDEN_PROSPERO_SOURCE_DIR}")
if(PLAYDEN_PROSPERO_RELEASE_SOURCE)
    list(APPEND prospero_source_check_args --release)
endif()
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/prospero_source.py"
    ${prospero_source_check_args}
    RESULT_VARIABLE prospero_source_check_result ERROR_VARIABLE prospero_source_check_error
    OUTPUT_QUIET)
if(NOT prospero_source_check_result STREQUAL "0")
    message(FATAL_ERROR "${prospero_source_check_error}\nInitialize third_party/prospero-win or select a clean checkout at the locked revision.")
endif()

add_library(playden_runtime_publication_verifier STATIC
    "${PLAYDEN_PROSPERO_SOURCE_DIR}/native/pw_title_publication_native.c"
    "${PLAYDEN_PROSPERO_SOURCE_DIR}/src/pw_title_publication.c"
    "${PLAYDEN_PROSPERO_SOURCE_DIR}/src/pw_title_launch.c"
    "${PLAYDEN_PROSPERO_SOURCE_DIR}/src/pw_game_profile.c"
    "${PLAYDEN_PROSPERO_SOURCE_DIR}/src/pw_app_profile.c"
    "${PLAYDEN_PROSPERO_SOURCE_DIR}/src/pw_profile_catalog.c"
    "${PLAYDEN_PROSPERO_SOURCE_DIR}/src/pw_wine_launch.c"
    "${PLAYDEN_PROSPERO_SOURCE_DIR}/src/pw_publication_sha256.c"
    "${PLAYDEN_PROSPERO_SOURCE_DIR}/third_party/mbedtls-publication/library/sha256.c"
)
target_include_directories(playden_runtime_publication_verifier PUBLIC "${PLAYDEN_PROSPERO_SOURCE_DIR}/native"
    PRIVATE "${PLAYDEN_PROSPERO_SOURCE_DIR}/src" "${PLAYDEN_PROSPERO_SOURCE_DIR}/third_party/mbedtls-publication"
    "${PLAYDEN_PROSPERO_SOURCE_DIR}/third_party/mbedtls-publication/include")
target_compile_definitions(playden_runtime_publication_verifier PRIVATE
    _DEFAULT_SOURCE MBEDTLS_CONFIG_FILE="pw_publication_config.h")
target_compile_features(playden_runtime_publication_verifier PRIVATE c_std_11)

# Immutable package staging and complete publication inspection are available to
# every launcher presentation, without compiling Wine or enabling guard trust.
target_link_libraries(playden_runtime_package_installer PUBLIC playden_runtime_publication_verifier)
target_include_directories(playden_runtime_package_installer PRIVATE "${PLAYDEN_PROSPERO_SOURCE_DIR}/src")
if(BUILD_TESTING AND NOT PLAYDEN_PLATFORM_PS5)
    target_link_libraries(runtime_package_installer_native_policy_test PRIVATE playden_runtime_publication_verifier)
    foreach(package_test runtime_package_installer_test runtime_package_installer_native_policy_test)
        target_link_libraries(${package_test} PRIVATE playden_runtime_publication_contract)
        target_include_directories(${package_test} PRIVATE
            "${PLAYDEN_PROSPERO_SOURCE_DIR}/third_party/mbedtls-publication"
            "${PLAYDEN_PROSPERO_SOURCE_DIR}/third_party/mbedtls-publication/include")
        target_compile_definitions(${package_test} PRIVATE MBEDTLS_CONFIG_FILE="pw_publication_config.h")
    endforeach()
endif()

add_library(playden_runtime_publication_contract STATIC
    "${CMAKE_SOURCE_DIR}/src/runtime/publication_contract.cpp"
    "${PLAYDEN_PROSPERO_SOURCE_DIR}/src/pw_title_publication.c")
target_include_directories(playden_runtime_publication_contract PUBLIC
    "${CMAKE_SOURCE_DIR}/src" "${PLAYDEN_PROSPERO_SOURCE_DIR}/src")
target_compile_features(playden_runtime_publication_contract PUBLIC cxx_std_17 PRIVATE c_std_11)

if(BUILD_TESTING AND NOT PLAYDEN_PLATFORM_PS5)
    add_executable(runtime_publication_contract_test
        "${CMAKE_SOURCE_DIR}/tests/runtime_publication_contract_test.cpp")
    target_link_libraries(runtime_publication_contract_test PRIVATE playden_runtime_publication_contract)
    add_test(NAME runtime_publication_contract COMMAND runtime_publication_contract_test)
    add_executable(prospero_publication_sha256_contract
        "${PLAYDEN_PROSPERO_SOURCE_DIR}/tests/test_pw_publication_sha256.c"
        "${PLAYDEN_PROSPERO_SOURCE_DIR}/src/pw_publication_sha256.c"
        "${PLAYDEN_PROSPERO_SOURCE_DIR}/third_party/mbedtls-publication/library/sha256.c")
    target_compile_features(prospero_publication_sha256_contract PRIVATE c_std_11)
    target_compile_options(prospero_publication_sha256_contract PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    target_compile_definitions(prospero_publication_sha256_contract PRIVATE
        MBEDTLS_CONFIG_FILE="pw_publication_config.h")
    target_include_directories(prospero_publication_sha256_contract PRIVATE
        "${PLAYDEN_PROSPERO_SOURCE_DIR}/third_party/mbedtls-publication"
        "${PLAYDEN_PROSPERO_SOURCE_DIR}/third_party/mbedtls-publication/include")
    add_test(NAME prospero_publication_sha256_contract COMMAND prospero_publication_sha256_contract)
    # Exercise the exact pinned wrapper admission implementation with real files
    # and the host lock callback; PS5 lock qualification remains a native check.
    add_executable(prospero_title_publication_native_contract
        "${PLAYDEN_PROSPERO_SOURCE_DIR}/tests/test_pw_title_publication_native.c"
        "${PLAYDEN_PROSPERO_SOURCE_DIR}/native/pw_title_publication_native.c"
        "${PLAYDEN_PROSPERO_SOURCE_DIR}/src/pw_title_publication.c"
        "${PLAYDEN_PROSPERO_SOURCE_DIR}/src/pw_title_launch.c"
        "${PLAYDEN_PROSPERO_SOURCE_DIR}/src/pw_game_profile.c"
        "${PLAYDEN_PROSPERO_SOURCE_DIR}/src/pw_app_profile.c"
        "${PLAYDEN_PROSPERO_SOURCE_DIR}/src/pw_profile_catalog.c"
        "${PLAYDEN_PROSPERO_SOURCE_DIR}/src/pw_wine_launch.c"
        "${PLAYDEN_PROSPERO_SOURCE_DIR}/src/pw_publication_sha256.c"
        "${PLAYDEN_PROSPERO_SOURCE_DIR}/third_party/mbedtls-publication/library/sha256.c")
    target_compile_features(prospero_title_publication_native_contract PRIVATE c_std_11)
    target_compile_options(prospero_title_publication_native_contract PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    target_compile_definitions(prospero_title_publication_native_contract PRIVATE
        _DEFAULT_SOURCE MBEDTLS_CONFIG_FILE="pw_publication_config.h")
    target_include_directories(prospero_title_publication_native_contract PRIVATE
        "${PLAYDEN_PROSPERO_SOURCE_DIR}/third_party/mbedtls-publication"
        "${PLAYDEN_PROSPERO_SOURCE_DIR}/third_party/mbedtls-publication/include")
    add_test(NAME prospero_title_publication_native_contract COMMAND prospero_title_publication_native_contract)
    add_test(NAME prospero_source_pin COMMAND "${Python3_EXECUTABLE}" -m unittest discover
        -s "${CMAKE_SOURCE_DIR}/tests" -p test_prospero_source.py)
    add_test(NAME prospero_bottle_preparation_evidence COMMAND "${Python3_EXECUTABLE}" -m unittest discover
        -s "${CMAKE_SOURCE_DIR}/tests" -p test_bottle_preparation_evidence.py)
    foreach(contract title_launch title_publication title_session app_profile game_profile profile_catalog)
        set(contract_sources
            "${PLAYDEN_PROSPERO_SOURCE_DIR}/tests/test_pw_${contract}.c"
            "${PLAYDEN_PROSPERO_SOURCE_DIR}/src/pw_${contract}.c")
        if(contract STREQUAL "game_profile")
            list(APPEND contract_sources "${PLAYDEN_PROSPERO_SOURCE_DIR}/src/pw_app_profile.c")
        elseif(contract STREQUAL "profile_catalog")
            list(APPEND contract_sources "${PLAYDEN_PROSPERO_SOURCE_DIR}/src/pw_game_profile.c"
                "${PLAYDEN_PROSPERO_SOURCE_DIR}/src/pw_app_profile.c")
        endif()
        add_executable(prospero_${contract}_contract ${contract_sources})
        target_compile_features(prospero_${contract}_contract PRIVATE c_std_11)
        target_compile_options(prospero_${contract}_contract PRIVATE -Wall -Wextra -Werror -UNDEBUG)
        if(contract STREQUAL "title_session")
            find_package(Threads REQUIRED)
            target_link_libraries(prospero_${contract}_contract PRIVATE Threads::Threads)
        endif()
        add_test(NAME prospero_${contract}_contract COMMAND prospero_${contract}_contract)
    endforeach()
    # Consume records emitted by the pinned production wrapper, so durable
    # schema and identifier differences cannot be hidden by hand-built mocks.
    target_sources(runtime_session_reconciler_test PRIVATE "${PLAYDEN_PROSPERO_SOURCE_DIR}/src/pw_title_session.c")
    target_include_directories(runtime_session_reconciler_test PRIVATE "${PLAYDEN_PROSPERO_SOURCE_DIR}/src")
    target_link_libraries(runtime_session_reconciler_test PRIVATE Threads::Threads)
    foreach(session_storage_test runtime_session_storage_test runtime_session_storage_native_policy_test)
        target_sources(${session_storage_test} PRIVATE "${PLAYDEN_PROSPERO_SOURCE_DIR}/src/pw_title_session.c")
        target_include_directories(${session_storage_test} PRIVATE "${PLAYDEN_PROSPERO_SOURCE_DIR}/src")
        target_link_libraries(${session_storage_test} PRIVATE Threads::Threads)
    endforeach()
endif()

add_library(playden_runtime_kit_manifest STATIC
    "${CMAKE_SOURCE_DIR}/src/runtime/kit_manifest.cpp")
target_include_directories(playden_runtime_kit_manifest PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_compile_features(playden_runtime_kit_manifest PUBLIC cxx_std_17)
target_link_libraries(playden_runtime_kit_manifest PUBLIC
    playden_runtime_package_installer playden_steam_protocol)

add_library(playden_runtime_artifact_hash STATIC
    "${CMAKE_SOURCE_DIR}/src/runtime/artifact_hash.cpp")
target_include_directories(playden_runtime_artifact_hash PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_compile_features(playden_runtime_artifact_hash PUBLIC cxx_std_17)
target_link_libraries(playden_runtime_artifact_hash PRIVATE playden_curl_8_22 playden_managed_directory)
# gbe_fork Steamworks emulation in a game's folder (runtime/steamworks).
# Also SteamStub unpacking of a game's program (runtime/steam_stub), with mbedtls through curl.
add_library(playden_runtime_steamworks STATIC "${CMAKE_SOURCE_DIR}/src/runtime/steamworks.cpp"
    "${CMAKE_SOURCE_DIR}/src/runtime/dotnet_heap.cpp" "${CMAKE_SOURCE_DIR}/src/runtime/steam_stub.cpp")
target_compile_features(playden_runtime_steamworks PUBLIC cxx_std_17)
target_include_directories(playden_runtime_steamworks PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(playden_runtime_steamworks PUBLIC playden_runtime_artifact_hash)
target_link_libraries(playden_runtime_steamworks PRIVATE playden_curl_8_22)
add_library(playden_runtime_kit_admission STATIC
    "${CMAKE_SOURCE_DIR}/src/runtime/kit_admission.cpp")
target_include_directories(playden_runtime_kit_admission PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_compile_features(playden_runtime_kit_admission PUBLIC cxx_std_17)
target_link_libraries(playden_runtime_kit_admission PUBLIC
    playden_runtime_kit_manifest playden_runtime_artifact_hash)
add_library(playden_runtime_kit_trust STATIC "${CMAKE_SOURCE_DIR}/src/runtime/kit_trust.cpp")
target_include_directories(playden_runtime_kit_trust PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_compile_features(playden_runtime_kit_trust PUBLIC cxx_std_17)
target_link_libraries(playden_runtime_kit_trust PUBLIC playden_runtime_kit_admission)
if(BUILD_TESTING AND NOT PLAYDEN_PLATFORM_PS5)
    add_executable(runtime_kit_trust_test "${CMAKE_SOURCE_DIR}/tests/runtime_kit_trust_test.cpp")
    target_link_libraries(runtime_kit_trust_test PRIVATE playden_runtime_kit_trust)
    # The packaged kit is a local build artifact; admission is checked when it exists.
    add_test(NAME runtime_kit_trust COMMAND runtime_kit_trust_test
        "${CMAKE_SOURCE_DIR}/dependencies.lock.json" "${CMAKE_SOURCE_DIR}/build/kits/playden-kit")
    add_executable(prospero_kit_consumer_test
        "${CMAKE_SOURCE_DIR}/tests/kit_producer_consumer_test.cpp")
    target_link_libraries(prospero_kit_consumer_test PRIVATE playden_runtime_kit_admission)
    add_test(NAME prospero_kit_producer_consumer
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_kit_producer_consumer.py"
            --source "${PLAYDEN_PROSPERO_SOURCE_DIR}"
            --consumer $<TARGET_FILE:prospero_kit_consumer_test>)
    add_executable(runtime_kit_admission_test
        "${CMAKE_SOURCE_DIR}/tests/runtime_kit_manifest_test.cpp")
    target_compile_definitions(runtime_kit_admission_test PRIVATE PLAYDEN_TEST_KIT_ADMISSION=1)
    target_link_libraries(runtime_kit_admission_test PRIVATE playden_runtime_kit_admission)
    add_test(NAME runtime_kit_admission COMMAND runtime_kit_admission_test)
    add_executable(runtime_artifact_hash_test
        "${CMAKE_SOURCE_DIR}/tests/runtime_artifact_hash_test.cpp")
    target_link_libraries(runtime_artifact_hash_test PRIVATE playden_runtime_artifact_hash)
    add_test(NAME runtime_artifact_hash COMMAND runtime_artifact_hash_test)
    add_executable(runtime_artifact_hash_native_policy_test
        "${CMAKE_SOURCE_DIR}/tests/runtime_artifact_hash_test.cpp"
        "${CMAKE_SOURCE_DIR}/src/runtime/artifact_hash.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/managed_directory.cpp")
    target_include_directories(runtime_artifact_hash_native_policy_test PRIVATE "${CMAKE_SOURCE_DIR}/src")
    target_compile_features(runtime_artifact_hash_native_policy_test PRIVATE cxx_std_17)
    target_compile_definitions(runtime_artifact_hash_native_policy_test PRIVATE
        PLAYDEN_PLATFORM_PS5=1 PLAYDEN_TEST_AT_UNAVAILABLE=1)
    target_link_libraries(runtime_artifact_hash_native_policy_test PRIVATE playden_curl_8_22)
    add_test(NAME runtime_artifact_hash_native_policy COMMAND runtime_artifact_hash_native_policy_test)
endif()

if(BUILD_TESTING AND NOT PLAYDEN_PLATFORM_PS5)
    add_executable(runtime_kit_manifest_test
        "${CMAKE_SOURCE_DIR}/tests/runtime_kit_manifest_test.cpp")
    target_link_libraries(runtime_kit_manifest_test PRIVATE playden_runtime_kit_manifest)
    add_test(NAME runtime_kit_manifest COMMAND runtime_kit_manifest_test)
endif()
