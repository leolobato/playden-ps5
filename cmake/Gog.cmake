# The GOG client (src/gog/): sign-in, tokens and the stored session now; the library, downloads and installs later. A lower layer: no UI.

add_library(playden_gog STATIC
    "${CMAKE_SOURCE_DIR}/src/gog/api.cpp"
    "${CMAKE_SOURCE_DIR}/src/gog/auth.cpp"
    "${CMAKE_SOURCE_DIR}/src/gog/http.cpp"
    "${CMAKE_SOURCE_DIR}/src/gog/product_cache.cpp"
    "${CMAKE_SOURCE_DIR}/src/gog/session.cpp"
    "${CMAKE_SOURCE_DIR}/src/gog/session_store.cpp"
    "${CMAKE_SOURCE_DIR}/src/gog/user.cpp")
target_include_directories(playden_gog PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_compile_features(playden_gog PUBLIC cxx_std_17)
target_link_libraries(playden_gog PUBLIC playden_steam_http playden_steam_protocol playden_storage
    playden_managed_directory playden_runtime_artifact_hash Threads::Threads)
if(PLAYDEN_PLATFORM_PS5)
    target_compile_definitions(playden_gog PRIVATE PLAYDEN_PLATFORM_PS5=1)
    target_compile_options(playden_gog PRIVATE -fno-exceptions -fno-rtti)
endif()

# GOG as a store (src/stores/gog*): the account and the library, each on its own worker, over the GOG session. The
# apps make them with features.txt's gog-store line (src/homebrew/gog_wiring.*).
add_library(playden_gog_store STATIC "${CMAKE_SOURCE_DIR}/src/stores/gog.cpp"
    "${CMAKE_SOURCE_DIR}/src/stores/gog_library.cpp")
target_include_directories(playden_gog_store PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_compile_features(playden_gog_store PUBLIC cxx_std_17)
target_link_libraries(playden_gog_store PUBLIC playden_gog playden_library_service Threads::Threads)
if(PLAYDEN_PLATFORM_PS5)
    target_compile_definitions(playden_gog_store PRIVATE PLAYDEN_PLATFORM_PS5=1)
    target_compile_options(playden_gog_store PRIVATE -fno-exceptions -fno-rtti)
endif()

if(BUILD_TESTING AND NOT PLAYDEN_PLATFORM_PS5)
    add_executable(gog_auth_test "${CMAKE_SOURCE_DIR}/tests/gog_auth_test.cpp")
    target_link_libraries(gog_auth_test PRIVATE playden_gog)
    add_test(NAME gog_auth COMMAND gog_auth_test "${CMAKE_SOURCE_DIR}/fixtures/gog")
    # Each run gets its own folder in the build tree: no symlinked component (macOS /tmp is one).
    add_executable(gog_session_store_test "${CMAKE_SOURCE_DIR}/tests/gog_session_store_test.cpp")
    target_link_libraries(gog_session_store_test PRIVATE playden_gog)
    add_test(NAME gog_session_store COMMAND gog_session_store_test "${CMAKE_CURRENT_BINARY_DIR}/gog_session_store.tmp")
    # The console's path handling for the session file (no descriptor-relative opens), run on the host.
    add_executable(gog_session_store_native_policy_test
        "${CMAKE_SOURCE_DIR}/tests/gog_session_store_test.cpp"
        "${CMAKE_SOURCE_DIR}/src/gog/session_store.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/managed_directory.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/storage.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/durable_storage.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/effective_uid.cpp")
    target_include_directories(gog_session_store_native_policy_test PRIVATE "${CMAKE_SOURCE_DIR}/src")
    target_compile_features(gog_session_store_native_policy_test PRIVATE cxx_std_17)
    target_compile_definitions(gog_session_store_native_policy_test PRIVATE PLAYDEN_PLATFORM_PS5=1 PLAYDEN_TEST_AT_UNAVAILABLE=1)
    target_link_libraries(gog_session_store_native_policy_test PRIVATE Threads::Threads)
    add_test(NAME gog_session_store_native_policy COMMAND gog_session_store_native_policy_test
        "${CMAKE_CURRENT_BINARY_DIR}/gog_session_store_native_policy.tmp")
    # The token owner: restore, exchange, refresh and generations, through a stub transport and a fake clock.
    add_executable(gog_session_test "${CMAKE_SOURCE_DIR}/tests/gog_session_test.cpp")
    target_link_libraries(gog_session_test PRIVATE playden_gog)
    add_test(NAME gog_session COMMAND gog_session_test "${CMAKE_SOURCE_DIR}/fixtures/gog"
        "${CMAKE_CURRENT_BINARY_DIR}/gog_session.tmp")
    # The GOG account worker: its events, the relay's sign-in answers and sign-out.
    add_executable(gog_account_test "${CMAKE_SOURCE_DIR}/tests/gog_account_test.cpp")
    target_link_libraries(gog_account_test PRIVATE playden_gog_store)
    add_test(NAME gog_account COMMAND gog_account_test "${CMAKE_SOURCE_DIR}/fixtures/gog"
        "${CMAKE_CURRENT_BINARY_DIR}/gog_account.tmp")
    # The owned list and gamesdb entries: parsing, filters and art addresses, on recorded public answers.
    add_executable(gog_api_test "${CMAKE_SOURCE_DIR}/tests/gog_api_test.cpp")
    target_link_libraries(gog_api_test PRIVATE playden_gog)
    add_test(NAME gog_api COMMAND gog_api_test "${CMAKE_SOURCE_DIR}/fixtures/gog")
    # The saved gamesdb entries under gog/products: format, folder modes, links refused, clear.
    add_executable(gog_product_cache_test "${CMAKE_SOURCE_DIR}/tests/gog_product_cache_test.cpp")
    target_link_libraries(gog_product_cache_test PRIVATE playden_gog)
    add_test(NAME gog_product_cache COMMAND gog_product_cache_test "${CMAKE_CURRENT_BINARY_DIR}/gog_product_cache.tmp")
    # The same with the console's path handling (no descriptor-relative opens), run on the host.
    add_executable(gog_product_cache_native_policy_test
        "${CMAKE_SOURCE_DIR}/tests/gog_product_cache_test.cpp"
        "${CMAKE_SOURCE_DIR}/src/gog/product_cache.cpp"
        "${CMAKE_SOURCE_DIR}/src/gog/api.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/managed_directory.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/storage.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/durable_storage.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/effective_uid.cpp")
    target_include_directories(gog_product_cache_native_policy_test PRIVATE "${CMAKE_SOURCE_DIR}/src")
    target_compile_features(gog_product_cache_native_policy_test PRIVATE cxx_std_17)
    target_compile_definitions(gog_product_cache_native_policy_test PRIVATE PLAYDEN_PLATFORM_PS5=1 PLAYDEN_TEST_AT_UNAVAILABLE=1)
    target_link_libraries(gog_product_cache_native_policy_test PRIVATE playden_steam_protocol Threads::Threads)
    add_test(NAME gog_product_cache_native_policy COMMAND gog_product_cache_native_policy_test
        "${CMAKE_CURRENT_BINARY_DIR}/gog_product_cache_native_policy.tmp")
    # The GOG library worker: batches, ETag reuse, failures, clear and generations, through a stub transport.
    add_executable(gog_library_test "${CMAKE_SOURCE_DIR}/tests/gog_library_test.cpp")
    target_link_libraries(gog_library_test PRIVATE playden_gog_store)
    add_test(NAME gog_library COMMAND gog_library_test "${CMAKE_SOURCE_DIR}/fixtures/gog"
        "${CMAKE_CURRENT_BINARY_DIR}/gog_library.tmp")
    # gog.log after a stubbed restore, sign-in, library refresh and sign-out: no account, token, code or address.
    add_executable(gog_logs_test "${CMAKE_SOURCE_DIR}/tests/gog_logs_test.cpp")
    target_link_libraries(gog_logs_test PRIVATE playden_gog_store)
    add_test(NAME gog_logs COMMAND gog_logs_test "${CMAKE_SOURCE_DIR}/fixtures/gog"
        "${CMAKE_CURRENT_BINARY_DIR}/gog_logs.tmp")
endif()
