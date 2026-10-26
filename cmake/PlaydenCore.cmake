# Renderer-independent Playden storage and service targets.

find_package(Threads REQUIRED)

foreach(utf8proc_file utf8proc.c utf8proc.h utf8proc_data.c LICENSE.md)
    string(JSON expected_utf8proc_hash GET "${playden_dependency_lock}"
        utf8proc files "${utf8proc_file}")
    set(utf8proc_path "${CMAKE_SOURCE_DIR}/third_party/utf8proc/${utf8proc_file}")
    file(SHA256 "${utf8proc_path}" actual_utf8proc_hash)
    if(NOT actual_utf8proc_hash STREQUAL expected_utf8proc_hash)
        message(FATAL_ERROR "Pinned utf8proc source hash mismatch: ${utf8proc_file}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${utf8proc_path}")
endforeach()

add_library(playden_library STATIC
    "${CMAKE_SOURCE_DIR}/third_party/utf8proc/utf8proc.c"
    "${CMAKE_SOURCE_DIR}/src/library/query.cpp"
    "${CMAKE_SOURCE_DIR}/src/library/cache.cpp"
    "${CMAKE_SOURCE_DIR}/src/library/organization.cpp"
    "${CMAKE_SOURCE_DIR}/src/library/store.cpp")
target_include_directories(playden_library PUBLIC "${CMAKE_SOURCE_DIR}/src"
    PRIVATE "${CMAKE_SOURCE_DIR}/third_party/utf8proc")
target_compile_definitions(playden_library PRIVATE UTF8PROC_STATIC)
target_compile_features(playden_library PUBLIC cxx_std_17)

add_library(playden_storage STATIC
    "${CMAKE_SOURCE_DIR}/src/platform/effective_uid.cpp"
    "${CMAKE_SOURCE_DIR}/src/platform/random_id.cpp"
    "${CMAKE_SOURCE_DIR}/src/platform/storage.cpp"
    "${CMAKE_SOURCE_DIR}/src/platform/durable_storage.cpp")
target_include_directories(playden_storage PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_compile_features(playden_storage PUBLIC cxx_std_17)
target_link_libraries(playden_library PUBLIC playden_storage)

add_library(playden_steam_core STATIC
    "${CMAKE_SOURCE_DIR}/src/steam/session_store.cpp"
    "${CMAKE_SOURCE_DIR}/src/steam/service.cpp"
    "${CMAKE_SOURCE_DIR}/src/steam/app_details_service.cpp"
    "${CMAKE_SOURCE_DIR}/src/steam/store_details_service.cpp"
    "${CMAKE_SOURCE_DIR}/src/installs/install_service.cpp"
    "${CMAKE_SOURCE_DIR}/src/steam/install_volumes.cpp"
    "${CMAKE_SOURCE_DIR}/src/steam/cloud_service.cpp")
target_include_directories(playden_steam_core PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_compile_features(playden_steam_core PUBLIC cxx_std_17)
if(PLAYDEN_PLATFORM_PS5)
    target_compile_definitions(playden_steam_core PRIVATE PLAYDEN_PLATFORM_PS5=1)
endif()
target_link_libraries(playden_steam_core PUBLIC playden_library playden_storage playden_steam_protocol Threads::Threads)

add_library(playden_library_service STATIC
    "${CMAKE_SOURCE_DIR}/src/library/service.cpp"
    "${CMAKE_SOURCE_DIR}/src/stores/registry.cpp")
target_include_directories(playden_library_service PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_compile_features(playden_library_service PUBLIC cxx_std_17)
target_link_libraries(playden_library_service PUBLIC playden_steam_core)

add_library(playden_diagnostic_log STATIC "${CMAKE_SOURCE_DIR}/src/app/diagnostic_log.cpp")
target_include_directories(playden_diagnostic_log PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_compile_features(playden_diagnostic_log PUBLIC cxx_std_17)

# The languages Steam ships game content in. The depot selection, the preferences and the
# Steamworks emulator all name them, and none of those may depend on the others.
add_library(playden_steam_content_language STATIC
    "${CMAKE_SOURCE_DIR}/src/steam/backend/content_language.cpp")
target_include_directories(playden_steam_content_language PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_compile_features(playden_steam_content_language PUBLIC cxx_std_17)
add_library(playden_preferences STATIC "${CMAKE_SOURCE_DIR}/src/app/preferences.cpp")
target_include_directories(playden_preferences PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_compile_features(playden_preferences PUBLIC cxx_std_17)
target_link_libraries(playden_preferences PUBLIC playden_storage playden_steam_content_language)

add_library(playden_persistence_service STATIC "${CMAKE_SOURCE_DIR}/src/app/persistence_service.cpp")
target_include_directories(playden_persistence_service PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_compile_features(playden_persistence_service PUBLIC cxx_std_17)
target_link_libraries(playden_persistence_service PUBLIC playden_preferences playden_library Threads::Threads)

add_library(playden_game_paths STATIC "${CMAKE_SOURCE_DIR}/src/steam/backend/game_path.cpp")
target_include_directories(playden_game_paths PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_compile_features(playden_game_paths PUBLIC cxx_std_17)

add_library(playden_runtime_installs STATIC "${CMAKE_SOURCE_DIR}/src/runtime/game_install.cpp")
target_include_directories(playden_runtime_installs PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_compile_features(playden_runtime_installs PUBLIC cxx_std_17)
target_link_libraries(playden_runtime_installs PUBLIC playden_library playden_storage playden_steam_protocol playden_game_paths)

add_library(playden_runtime_install_service STATIC "${CMAKE_SOURCE_DIR}/src/runtime/install_service.cpp")
target_include_directories(playden_runtime_install_service PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_compile_features(playden_runtime_install_service PUBLIC cxx_std_17)
target_link_libraries(playden_runtime_install_service PUBLIC playden_runtime_installs)

add_library(playden_managed_directory STATIC
    "${CMAKE_SOURCE_DIR}/src/platform/managed_directory.cpp"
    "${CMAKE_SOURCE_DIR}/src/platform/managed_file_lock.cpp")
target_include_directories(playden_managed_directory PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_compile_features(playden_managed_directory PUBLIC cxx_std_17)
target_link_libraries(playden_storage PUBLIC playden_managed_directory)
if(PLAYDEN_PLATFORM_PS5)
    target_compile_definitions(playden_managed_directory PRIVATE PLAYDEN_PLATFORM_PS5=1)
endif()

add_library(playden_runtime_private_directories STATIC "${CMAKE_SOURCE_DIR}/src/runtime/private_directories.cpp")
target_include_directories(playden_runtime_private_directories PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_compile_features(playden_runtime_private_directories PUBLIC cxx_std_17)
target_link_libraries(playden_runtime_private_directories PUBLIC playden_managed_directory)

add_library(playden_runtime_layout_migration STATIC "${CMAKE_SOURCE_DIR}/src/runtime/layout_migration.cpp")
target_include_directories(playden_runtime_layout_migration PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_compile_features(playden_runtime_layout_migration PUBLIC cxx_std_17)
target_link_libraries(playden_runtime_layout_migration PUBLIC playden_runtime_installs playden_runtime_private_directories)

add_library(playden_runtime_package_installer STATIC "${CMAKE_SOURCE_DIR}/src/runtime/package_installer.cpp")
target_include_directories(playden_runtime_package_installer PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_compile_features(playden_runtime_package_installer PUBLIC cxx_std_17)
target_link_libraries(playden_runtime_package_installer PUBLIC playden_runtime_install_service playden_runtime_private_directories)

add_library(playden_runtime_launch_service STATIC "${CMAKE_SOURCE_DIR}/src/runtime/launch_service.cpp")
target_include_directories(playden_runtime_launch_service PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_compile_features(playden_runtime_launch_service PUBLIC cxx_std_17)
target_link_libraries(playden_runtime_launch_service PUBLIC playden_runtime_install_service)

add_library(playden_runtime_session_reconciler STATIC
    "${CMAKE_SOURCE_DIR}/src/runtime/session_reconciler.cpp"
    "${CMAKE_SOURCE_DIR}/src/runtime/session_storage.cpp")
target_include_directories(playden_runtime_session_reconciler PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_compile_features(playden_runtime_session_reconciler PUBLIC cxx_std_17)
target_link_libraries(playden_runtime_session_reconciler PUBLIC playden_runtime_install_service playden_storage
    PRIVATE playden_steam_protocol)

# Windows games imported from a drive or the drop folder: the folder scanner, imports.json and
# the Local store's library source.
add_library(playden_local_store STATIC
    "${CMAKE_SOURCE_DIR}/src/stores/local/scanner.cpp"
    "${CMAKE_SOURCE_DIR}/src/stores/local/import_store.cpp"
    "${CMAKE_SOURCE_DIR}/src/stores/local/library_source.cpp"
    "${CMAKE_SOURCE_DIR}/src/stores/local/drop_folder_access.cpp"
    "${CMAKE_SOURCE_DIR}/src/stores/local/copy_plan.cpp")
target_include_directories(playden_local_store PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_compile_features(playden_local_store PUBLIC cxx_std_17)
if(PLAYDEN_PLATFORM_PS5)
    target_compile_definitions(playden_local_store PRIVATE PLAYDEN_PLATFORM_PS5=1)
endif()
target_link_libraries(playden_local_store PUBLIC playden_storage playden_library Threads::Threads PRIVATE playden_steam_protocol)

# Host tool: what the import scanner sees in a game folder (tools/import-scan/main.cpp).
if(NOT PLAYDEN_PLATFORM_PS5)
    add_executable(import-scan "${CMAKE_SOURCE_DIR}/tools/import-scan/main.cpp")
    target_link_libraries(import-scan PRIVATE playden_local_store)
endif()

add_library(playden_native_access STATIC
    "${CMAKE_SOURCE_DIR}/src/platform/ps5/native_access.cpp"
    "${CMAKE_SOURCE_DIR}/src/platform/ps5/native_access_posix.cpp")
target_include_directories(playden_native_access PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_compile_features(playden_native_access PUBLIC cxx_std_17)

if(BUILD_TESTING AND NOT PLAYDEN_PLATFORM_PS5)
    add_executable(managed_file_lock_test
        "${CMAKE_SOURCE_DIR}/tests/managed_file_lock_test.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/managed_file_lock.cpp")
    target_compile_definitions(managed_file_lock_test PRIVATE flock=playden_test_flock)
    target_link_libraries(managed_file_lock_test PRIVATE playden_managed_directory)
    add_test(NAME managed_file_lock COMMAND managed_file_lock_test)
    add_executable(managed_file_lock_native_policy_test
        "${CMAKE_SOURCE_DIR}/tests/managed_file_lock_test.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/managed_file_lock.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/managed_directory.cpp")
    target_compile_definitions(managed_file_lock_native_policy_test PRIVATE PLAYDEN_PLATFORM_PS5=1)
    target_link_libraries(managed_file_lock_native_policy_test PRIVATE playden_managed_directory)
    add_test(NAME managed_file_lock_native_policy COMMAND managed_file_lock_native_policy_test)
    add_executable(durable_storage_test "${CMAKE_SOURCE_DIR}/tests/durable_storage_test.cpp")
    target_link_libraries(durable_storage_test PRIVATE playden_storage ${CMAKE_DL_LIBS})
    add_test(NAME durable_storage COMMAND durable_storage_test)
    add_executable(random_id_test "${CMAKE_SOURCE_DIR}/tests/random_id_test.cpp")
    target_link_libraries(random_id_test PRIVATE playden_storage)
    add_test(NAME random_id COMMAND random_id_test)
    add_executable(durable_storage_native_policy_test
        "${CMAKE_SOURCE_DIR}/tests/durable_storage_test.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/durable_storage.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/managed_directory.cpp")
    target_compile_definitions(durable_storage_native_policy_test PRIVATE PLAYDEN_PLATFORM_PS5=1)
    target_link_libraries(durable_storage_native_policy_test PRIVATE playden_storage ${CMAKE_DL_LIBS})
    add_test(NAME durable_storage_native_policy COMMAND durable_storage_native_policy_test)
    add_executable(managed_directory_test "${CMAKE_SOURCE_DIR}/tests/managed_directory_test.cpp")
    target_link_libraries(managed_directory_test PRIVATE playden_managed_directory)
    add_test(NAME managed_directory COMMAND managed_directory_test)
    add_executable(runtime_private_directories_native_policy_test
        "${CMAKE_SOURCE_DIR}/tests/runtime_private_directories_native_policy_test.cpp"
        "${CMAKE_SOURCE_DIR}/src/runtime/private_directories.cpp"
        "${CMAKE_SOURCE_DIR}/tests/directory_gateway_host.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/managed_directory.cpp")
    target_include_directories(runtime_private_directories_native_policy_test PRIVATE "${CMAKE_SOURCE_DIR}/src")
    target_compile_features(runtime_private_directories_native_policy_test PRIVATE cxx_std_17)
    target_compile_definitions(runtime_private_directories_native_policy_test PRIVATE PLAYDEN_PLATFORM_PS5=1)
    add_test(NAME runtime_private_directories_native_policy COMMAND runtime_private_directories_native_policy_test)
    add_executable(runtime_session_reconciler_test "${CMAKE_SOURCE_DIR}/tests/runtime_session_reconciler_test.cpp")
    target_link_libraries(runtime_session_reconciler_test PRIVATE playden_runtime_session_reconciler)
    add_test(NAME runtime_session_reconciler COMMAND runtime_session_reconciler_test)
    add_executable(runtime_session_storage_test "${CMAKE_SOURCE_DIR}/tests/runtime_session_storage_test.cpp")
    target_link_libraries(runtime_session_storage_test PRIVATE playden_runtime_session_reconciler)
    add_test(NAME runtime_session_storage COMMAND runtime_session_storage_test)
    add_executable(runtime_session_storage_native_policy_test
        "${CMAKE_SOURCE_DIR}/tests/runtime_session_storage_test.cpp"
        "${CMAKE_SOURCE_DIR}/src/runtime/session_storage.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/managed_directory.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/durable_storage.cpp")
    target_compile_definitions(runtime_session_storage_native_policy_test PRIVATE PLAYDEN_PLATFORM_PS5=1)
    target_link_libraries(runtime_session_storage_native_policy_test PRIVATE playden_runtime_session_reconciler playden_steam_protocol)
    add_test(NAME runtime_session_storage_native_policy COMMAND runtime_session_storage_native_policy_test)
    add_executable(runtime_launch_service_test "${CMAKE_SOURCE_DIR}/tests/runtime_launch_service_test.cpp")
    target_link_libraries(runtime_launch_service_test PRIVATE playden_runtime_launch_service ${CMAKE_DL_LIBS})
    add_test(NAME runtime_launch_service COMMAND runtime_launch_service_test)
    add_executable(runtime_launch_service_native_policy_test
        "${CMAKE_SOURCE_DIR}/tests/runtime_launch_service_test.cpp"
        "${CMAKE_SOURCE_DIR}/src/runtime/launch_service.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/durable_storage.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/managed_directory.cpp")
    target_compile_definitions(runtime_launch_service_native_policy_test PRIVATE
        PLAYDEN_PLATFORM_PS5=1 PLAYDEN_TEST_DURABLE_FAULTS=1 fsync=playden_test_fsync)
    target_link_libraries(runtime_launch_service_native_policy_test PRIVATE playden_runtime_launch_service ${CMAKE_DL_LIBS})
    add_test(NAME runtime_launch_service_native_policy COMMAND runtime_launch_service_native_policy_test)
    add_executable(runtime_package_installer_test "${CMAKE_SOURCE_DIR}/tests/runtime_package_installer_test.cpp")
    target_link_libraries(runtime_package_installer_test PRIVATE playden_runtime_package_installer)
    add_test(NAME runtime_package_installer COMMAND runtime_package_installer_test)
    add_executable(runtime_package_installer_native_policy_test
        "${CMAKE_SOURCE_DIR}/tests/runtime_package_installer_test.cpp"
        "${CMAKE_SOURCE_DIR}/src/runtime/package_installer.cpp"
        "${CMAKE_SOURCE_DIR}/src/runtime/private_directories.cpp"
        "${CMAKE_SOURCE_DIR}/tests/directory_gateway_host.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/managed_directory.cpp")
    target_compile_definitions(runtime_package_installer_native_policy_test PRIVATE
        PLAYDEN_PLATFORM_PS5=1 PLAYDEN_TEST_AT_UNAVAILABLE=1)
    target_link_libraries(runtime_package_installer_native_policy_test PRIVATE playden_runtime_install_service)
    add_test(NAME runtime_package_installer_native_policy COMMAND runtime_package_installer_native_policy_test)
    add_executable(runtime_install_service_test "${CMAKE_SOURCE_DIR}/tests/runtime_install_service_test.cpp")
    target_link_libraries(runtime_install_service_test PRIVATE playden_runtime_install_service)
    add_test(NAME runtime_install_service COMMAND runtime_install_service_test)
    add_executable(layout_migration_test "${CMAKE_SOURCE_DIR}/tests/layout_migration_test.cpp")
    target_link_libraries(layout_migration_test PRIVATE playden_runtime_layout_migration)
    add_test(NAME layout_migration COMMAND layout_migration_test)
    add_executable(native_access_test "${CMAKE_SOURCE_DIR}/tests/native_access_test.cpp")
    target_link_libraries(native_access_test PRIVATE playden_native_access)
    add_test(NAME native_access COMMAND native_access_test)
    add_executable(native_access_posix_test "${CMAKE_SOURCE_DIR}/tests/native_access_posix_test.cpp")
    target_link_libraries(native_access_posix_test PRIVATE playden_native_access)
    add_test(NAME native_access_posix COMMAND native_access_posix_test)
    add_executable(runtime_game_install_test "${CMAKE_SOURCE_DIR}/tests/runtime_game_install_test.cpp")
    target_link_libraries(runtime_game_install_test PRIVATE playden_runtime_installs ${CMAKE_DL_LIBS})
    add_test(NAME runtime_game_install COMMAND runtime_game_install_test)
    add_executable(local_scanner_test "${CMAKE_SOURCE_DIR}/tests/local_scanner_test.cpp")
    target_link_libraries(local_scanner_test PRIVATE playden_local_store)
    add_test(NAME local_scanner COMMAND local_scanner_test)
    # The console's walk by path (no descriptor-relative opens), run on the host.
    add_executable(local_scanner_native_policy_test
        "${CMAKE_SOURCE_DIR}/tests/local_scanner_test.cpp"
        "${CMAKE_SOURCE_DIR}/src/stores/local/scanner.cpp")
    target_include_directories(local_scanner_native_policy_test PRIVATE "${CMAKE_SOURCE_DIR}/src")
    target_compile_features(local_scanner_native_policy_test PRIVATE cxx_std_17)
    target_compile_definitions(local_scanner_native_policy_test PRIVATE PLAYDEN_PLATFORM_PS5=1)
    add_test(NAME local_scanner_native_policy COMMAND local_scanner_native_policy_test)
    add_executable(local_drop_folder_access_test "${CMAKE_SOURCE_DIR}/tests/local_drop_folder_access_test.cpp")
    target_link_libraries(local_drop_folder_access_test PRIVATE playden_local_store)
    add_test(NAME local_drop_folder_access COMMAND local_drop_folder_access_test)
    # The console's walk by path (no descriptor-relative opens), run on the host.
    add_executable(local_drop_folder_access_native_policy_test
        "${CMAKE_SOURCE_DIR}/tests/local_drop_folder_access_test.cpp"
        "${CMAKE_SOURCE_DIR}/src/stores/local/drop_folder_access.cpp")
    target_include_directories(local_drop_folder_access_native_policy_test PRIVATE "${CMAKE_SOURCE_DIR}/src")
    target_compile_features(local_drop_folder_access_native_policy_test PRIVATE cxx_std_17)
    target_compile_definitions(local_drop_folder_access_native_policy_test PRIVATE PLAYDEN_PLATFORM_PS5=1)
    add_test(NAME local_drop_folder_access_native_policy COMMAND local_drop_folder_access_native_policy_test)
    add_executable(local_import_store_test "${CMAKE_SOURCE_DIR}/tests/local_import_store_test.cpp")
    target_link_libraries(local_import_store_test PRIVATE playden_local_store)
    add_test(NAME local_import_store COMMAND local_import_store_test)
    add_executable(diagnostic_log_test "${CMAKE_SOURCE_DIR}/tests/diagnostic_log_test.cpp")
    target_link_libraries(diagnostic_log_test PRIVATE playden_diagnostic_log)
    add_test(NAME diagnostic_log COMMAND diagnostic_log_test)
    add_executable(preferences_test "${CMAKE_SOURCE_DIR}/tests/preferences_test.cpp")
    target_link_libraries(preferences_test PRIVATE playden_preferences)
    add_test(NAME preferences COMMAND preferences_test)
    add_executable(persistence_service_test "${CMAKE_SOURCE_DIR}/tests/persistence_service_test.cpp")
    target_link_libraries(persistence_service_test PRIVATE playden_persistence_service)
    add_test(NAME persistence_service COMMAND persistence_service_test)
    add_executable(library_sync_test "${CMAKE_SOURCE_DIR}/tests/library_sync_test.cpp")
    target_link_libraries(library_sync_test PRIVATE playden_library_service)
    add_test(NAME library_sync COMMAND library_sync_test)
    foreach(test_name library_query library_cache library_organization library_store)
        add_executable(${test_name}_test "${CMAKE_SOURCE_DIR}/tests/${test_name}_test.cpp")
        target_link_libraries(${test_name}_test PRIVATE playden_library)
        add_test(NAME ${test_name} COMMAND ${test_name}_test)
    endforeach()
endif()

add_library(playden_title_activity STATIC "${CMAKE_SOURCE_DIR}/src/platform/ps5/title_activity.cpp"
    "${CMAKE_SOURCE_DIR}/src/platform/ps5/title_runtime.cpp")
target_include_directories(playden_title_activity PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_compile_features(playden_title_activity PUBLIC cxx_std_17)
if(BUILD_TESTING AND NOT PLAYDEN_PLATFORM_PS5)
    add_executable(title_activity_test "${CMAKE_SOURCE_DIR}/tests/title_activity_test.cpp")
    target_link_libraries(title_activity_test PRIVATE playden_title_activity)
    add_test(NAME title_activity COMMAND title_activity_test)
endif()

foreach(sqlite_file sqlite3.c sqlite3.h)
    string(JSON expected_sqlite_hash GET "${playden_dependency_lock}" sqlite3 files "${sqlite_file}")
    set(sqlite_path "${CMAKE_SOURCE_DIR}/third_party/sqlite3/${sqlite_file}")
    file(SHA256 "${sqlite_path}" actual_sqlite_hash)
    if(NOT actual_sqlite_hash STREQUAL expected_sqlite_hash)
        message(FATAL_ERROR "Pinned SQLite source hash mismatch: ${sqlite_file}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${sqlite_path}")
endforeach()
add_library(playden_sqlite_reader STATIC "${CMAKE_SOURCE_DIR}/third_party/sqlite3/sqlite3.c")
target_include_directories(playden_sqlite_reader PUBLIC "${CMAKE_SOURCE_DIR}/third_party/sqlite3")
target_compile_definitions(playden_sqlite_reader PRIVATE SQLITE_OMIT_LOAD_EXTENSION=1 SQLITE_THREADSAFE=0
    SQLITE_TEMP_STORE=3 SQLITE_MAX_MMAP_SIZE=0 SQLITE_USE_URI=0)
add_library(playden_title_database STATIC "${CMAKE_SOURCE_DIR}/src/platform/ps5/title_database.cpp"
    "${CMAKE_SOURCE_DIR}/src/platform/ps5/title_runtime.cpp")
target_include_directories(playden_title_database PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_compile_features(playden_title_database PUBLIC cxx_std_17)
target_link_libraries(playden_title_database PRIVATE playden_sqlite_reader Threads::Threads)
if(BUILD_TESTING AND NOT PLAYDEN_PLATFORM_PS5)
    add_executable(title_database_test "${CMAKE_SOURCE_DIR}/tests/title_database_test.cpp")
    target_link_libraries(title_database_test PRIVATE playden_title_database playden_sqlite_reader)
    add_test(NAME title_database COMMAND title_database_test)
endif()

add_library(playden_publication_transaction STATIC "${CMAKE_SOURCE_DIR}/src/runtime/publication_transaction.cpp")
target_include_directories(playden_publication_transaction PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_compile_features(playden_publication_transaction PUBLIC cxx_std_17)
add_library(playden_native_coordinator STATIC "${CMAKE_SOURCE_DIR}/src/platform/ps5/native_coordinator.cpp"
    "${CMAKE_SOURCE_DIR}/src/platform/ps5/title_registry.cpp" "${CMAKE_SOURCE_DIR}/src/platform/ps5/title_remover.cpp"
    "${CMAKE_SOURCE_DIR}/src/platform/ps5/title_allow_list.cpp")
target_include_directories(playden_native_coordinator PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_compile_features(playden_native_coordinator PUBLIC cxx_std_17)
target_link_libraries(playden_native_coordinator PUBLIC playden_runtime_install_service playden_title_database
    playden_title_activity playden_managed_directory playden_steam_protocol playden_publication_transaction)
if(BUILD_TESTING AND NOT PLAYDEN_PLATFORM_PS5)
    add_executable(native_coordinator_test "${CMAKE_SOURCE_DIR}/tests/native_coordinator_test.cpp")
    target_link_libraries(native_coordinator_test PRIVATE playden_native_coordinator)
    add_test(NAME native_coordinator COMMAND native_coordinator_test)
    add_executable(title_remover_test "${CMAKE_SOURCE_DIR}/tests/title_remover_test.cpp")
    target_link_libraries(title_remover_test PRIVATE playden_native_coordinator)
    add_test(NAME title_remover COMMAND title_remover_test)
    add_executable(private_storage_uid_test "${CMAKE_SOURCE_DIR}/tests/private_storage_uid_test.cpp")
    target_link_libraries(private_storage_uid_test PRIVATE playden_native_coordinator playden_storage Threads::Threads)
    add_test(NAME private_storage_uid COMMAND private_storage_uid_test)
    add_executable(title_allow_list_test "${CMAKE_SOURCE_DIR}/tests/title_allow_list_test.cpp")
    target_link_libraries(title_allow_list_test PRIVATE playden_native_coordinator)
    add_test(NAME title_allow_list COMMAND title_allow_list_test)
endif()
