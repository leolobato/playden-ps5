# Community recipes: the latest release of playden-ps5-recipes fetched into memory, checked, kept
# by content under the user data folder (src/community/).
#
# PLAYDEN_DEV_ENDPOINTS=ON compiles in reading dev-endpoints.json (another recipes server, plain
# http to it, an auth header) for local testing. Release packages must be built with it OFF:
# the PS5 package then carries a marker that tools/write_update_manifest.py refuses.

add_library(playden_community STATIC
    "${CMAKE_SOURCE_DIR}/src/community/endpoints.cpp"
    "${CMAKE_SOURCE_DIR}/src/community/recipe_release.cpp"
    "${CMAKE_SOURCE_DIR}/src/community/recipe_store.cpp"
    "${CMAKE_SOURCE_DIR}/src/community/recipe_service.cpp"
    "${CMAKE_SOURCE_DIR}/src/community/recipe_import.cpp"
    "${CMAKE_SOURCE_DIR}/src/community/install_secret.cpp"
    "${CMAKE_SOURCE_DIR}/src/community/api_client.cpp"
    "${CMAKE_SOURCE_DIR}/src/community/share_decision.cpp"
    "${CMAKE_SOURCE_DIR}/src/community/vote_state.cpp")
target_include_directories(playden_community PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_compile_features(playden_community PUBLIC cxx_std_17)
target_compile_definitions(playden_community PRIVATE PLAYDEN_DEV_ENDPOINTS=$<BOOL:${PLAYDEN_DEV_ENDPOINTS}>)
target_link_libraries(playden_community PUBLIC playden_steam_cm_codec playden_self_update playden_storage
    playden_runtime_private_directories playden_runtime_artifact_hash playden_miniz playden_steam_protocol
    playden_steam_http playden_auth_diagnostics Threads::Threads)
if(PLAYDEN_PLATFORM_PS5)
    target_compile_definitions(playden_community PRIVATE PLAYDEN_PLATFORM_PS5=1)
    target_compile_options(playden_community PRIVATE -fno-exceptions -fno-rtti)
endif()

if(BUILD_TESTING AND NOT PLAYDEN_PLATFORM_PS5)
    # Endpoints and the HttpClient plain-http opt-in, compiled without and with dev endpoints
    # whatever the build's option: the release form must ignore the dev file and refuse http.
    foreach(variant IN ITEMS release dev)
        if(variant STREQUAL "release")
            set(test_name community_endpoints_test)
            set(dev_value 0)
        else()
            set(test_name community_endpoints_dev_test)
            set(dev_value 1)
        endif()
        add_executable(${test_name}
            "${CMAKE_SOURCE_DIR}/tests/community_endpoints_test.cpp"
            "${CMAKE_SOURCE_DIR}/src/community/endpoints.cpp"
            "${CMAKE_SOURCE_DIR}/src/steam/backend/http_client.cpp")
        target_compile_definitions(${test_name} PRIVATE PLAYDEN_DEV_ENDPOINTS=${dev_value})
        target_link_libraries(${test_name} PRIVATE playden_steam_http playden_self_update playden_steam_protocol)
        string(REGEX REPLACE "_test$" "" ctest_name "${test_name}")
        add_test(NAME ${ctest_name} COMMAND ${test_name})

        if(variant STREQUAL "release")
            set(test_name community_recipe_service_test)
        else()
            set(test_name community_recipe_service_dev_test)
        endif()
        add_executable(${test_name}
            "${CMAKE_SOURCE_DIR}/tests/community_recipe_service_test.cpp"
            "${CMAKE_SOURCE_DIR}/src/community/endpoints.cpp"
            "${CMAKE_SOURCE_DIR}/src/community/recipe_service.cpp")
        target_compile_definitions(${test_name} PRIVATE PLAYDEN_DEV_ENDPOINTS=${dev_value})
        target_link_libraries(${test_name} PRIVATE playden_community)
        string(REGEX REPLACE "_test$" "" ctest_name "${test_name}")
        add_test(NAME ${ctest_name} COMMAND ${test_name} "${CMAKE_SOURCE_DIR}/tests/fixtures/community-recipes")

        # Sharing and voting calls: request bodies, headers, answers; http refused in a release build.
        if(variant STREQUAL "release")
            set(test_name community_api_client_test)
        else()
            set(test_name community_api_client_dev_test)
        endif()
        add_executable(${test_name}
            "${CMAKE_SOURCE_DIR}/tests/community_api_client_test.cpp"
            "${CMAKE_SOURCE_DIR}/src/community/endpoints.cpp"
            "${CMAKE_SOURCE_DIR}/src/community/api_client.cpp")
        target_compile_definitions(${test_name} PRIVATE PLAYDEN_DEV_ENDPOINTS=${dev_value})
        target_link_libraries(${test_name} PRIVATE playden_community)
        string(REGEX REPLACE "_test$" "" ctest_name "${test_name}")
        add_test(NAME ${ctest_name} COMMAND ${test_name} "${CMAKE_SOURCE_DIR}/tests/fixtures/community-recipes")
    endforeach()

    # What a recipe is for: stores, keys, name keys and game names, as the recipes repository's check makes them.
    add_executable(community_game_ref_test "${CMAKE_SOURCE_DIR}/tests/community_game_ref_test.cpp")
    target_link_libraries(community_game_ref_test PRIVATE playden_library)
    target_compile_features(community_game_ref_test PRIVATE cxx_std_20)
    add_test(NAME community_game_ref COMMAND community_game_ref_test)

    add_executable(community_share_decision_test "${CMAKE_SOURCE_DIR}/tests/community_share_decision_test.cpp")
    target_link_libraries(community_share_decision_test PRIVATE playden_community)
    add_test(NAME community_share_decision COMMAND community_share_decision_test)

    # Recipe files imported over web access: made local, checked like community recipes, kept by the store.
    add_executable(community_recipe_import_test "${CMAKE_SOURCE_DIR}/tests/community_recipe_import_test.cpp")
    target_link_libraries(community_recipe_import_test PRIVATE playden_community)
    add_test(NAME community_recipe_import COMMAND community_recipe_import_test "${CMAKE_SOURCE_DIR}/tests/fixtures/community-recipes")

    # The sharing ID and the vote memory on disk, and the log redaction of the secret; also with the
    # console's file rules.
    add_executable(community_sharing_store_test "${CMAKE_SOURCE_DIR}/tests/community_sharing_store_test.cpp")
    target_link_libraries(community_sharing_store_test PRIVATE playden_community)
    add_test(NAME community_sharing_store COMMAND community_sharing_store_test)
    add_executable(community_sharing_store_native_policy_test
        "${CMAKE_SOURCE_DIR}/tests/community_sharing_store_test.cpp"
        "${CMAKE_SOURCE_DIR}/src/community/install_secret.cpp"
        "${CMAKE_SOURCE_DIR}/src/community/vote_state.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/durable_storage.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/managed_directory.cpp"
        "${CMAKE_SOURCE_DIR}/src/runtime/private_directories.cpp"
        "${CMAKE_SOURCE_DIR}/tests/directory_gateway_host.cpp")
    target_compile_definitions(community_sharing_store_native_policy_test PRIVATE
        PLAYDEN_PLATFORM_PS5=1 PLAYDEN_TEST_AT_UNAVAILABLE=1)
    target_link_libraries(community_sharing_store_native_policy_test PRIVATE playden_community ${CMAKE_DL_LIBS})
    add_test(NAME community_sharing_store_native_policy COMMAND community_sharing_store_native_policy_test)

    add_executable(community_recipe_store_test "${CMAKE_SOURCE_DIR}/tests/community_recipe_store_test.cpp")
    target_link_libraries(community_recipe_store_test PRIVATE playden_community)
    add_test(NAME community_recipe_store COMMAND community_recipe_store_test)
    # The console's file rules: no *at calls (they fail here), absolute paths only.
    add_executable(community_recipe_store_native_policy_test
        "${CMAKE_SOURCE_DIR}/tests/community_recipe_store_test.cpp"
        "${CMAKE_SOURCE_DIR}/src/community/recipe_store.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/durable_storage.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/managed_directory.cpp"
        "${CMAKE_SOURCE_DIR}/src/runtime/private_directories.cpp"
        "${CMAKE_SOURCE_DIR}/tests/directory_gateway_host.cpp")
    target_compile_definitions(community_recipe_store_native_policy_test PRIVATE
        PLAYDEN_PLATFORM_PS5=1 PLAYDEN_TEST_AT_UNAVAILABLE=1)
    target_link_libraries(community_recipe_store_native_policy_test PRIVATE playden_community ${CMAKE_DL_LIBS})
    add_test(NAME community_recipe_store_native_policy COMMAND community_recipe_store_native_policy_test)
endif()
