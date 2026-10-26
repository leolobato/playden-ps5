# Playden - Direct ps5-homebrew-ui candidate integration.
# Copyright (C) 2026 Leo Lobato
# SPDX-License-Identifier: GPL-3.0-or-later

set(PLAYDEN_HOMEBREW_UI_REVISION "0a6fb4b851da86cd4e9512aae3abf1352a96086a")
set(PLAYDEN_HUI_SOURCE_DIR
    "${CMAKE_SOURCE_DIR}/third_party/ps5-homebrew-ui" CACHE PATH
    "ps5-homebrew-ui checkout (the pinned submodule, from its fork's main)")
set(PLAYDEN_HOMEBREW_UI_SOURCE_DIR "${PLAYDEN_HUI_SOURCE_DIR}" CACHE PATH
    "Compatibility alias for PLAYDEN_HUI_SOURCE_DIR" FORCE)
set(PLAYDEN_STB_IMAGE_INCLUDE_DIR
    "${CMAKE_SOURCE_DIR}/third_party/stb" CACHE PATH
    "Directory containing the pinned header-only stb_image.h decoder")

string(JSON expected_hui_revision GET "${playden_dependency_lock}" homebrew_ui revision)
if(NOT expected_hui_revision STREQUAL PLAYDEN_HOMEBREW_UI_REVISION)
    message(FATAL_ERROR "CMake Hui revision does not match dependencies.lock.json")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/dependencies.lock.json")

if(NOT EXISTS "${PLAYDEN_HUI_SOURCE_DIR}/src/gfx/renderer.cpp")
    message(FATAL_ERROR
        "ps5-homebrew-ui is missing. Run: git submodule update --init third_party/ps5-homebrew-ui")
endif()

execute_process(
    COMMAND git -C "${PLAYDEN_HUI_SOURCE_DIR}" rev-parse HEAD
    OUTPUT_VARIABLE playden_hui_revision
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE playden_hui_revision_status)
if(NOT playden_hui_revision_status EQUAL 0 OR
   NOT playden_hui_revision STREQUAL PLAYDEN_HOMEBREW_UI_REVISION)
    message(FATAL_ERROR
        "ps5-homebrew-ui must be ${PLAYDEN_HOMEBREW_UI_REVISION}; found '${playden_hui_revision}'")
endif()
execute_process(
    COMMAND git -C "${PLAYDEN_HUI_SOURCE_DIR}" status --porcelain --untracked-files=all
    OUTPUT_VARIABLE actual_hui_changes OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY)
if(NOT actual_hui_changes STREQUAL "")
    message(FATAL_ERROR "ps5-homebrew-ui has local changes; commit them on its main branch")
endif()

# Every source/header that can enter the Hui libraries or platform front ends triggers a fresh
# configure, where the pinned revision and a clean checkout are checked again.
file(GLOB_RECURSE playden_hui_trusted_sources CONFIGURE_DEPENDS LIST_DIRECTORIES false
    "${PLAYDEN_HUI_SOURCE_DIR}/src/*"
    "${PLAYDEN_HUI_SOURCE_DIR}/host/*"
    "${PLAYDEN_HUI_SOURCE_DIR}/third_party/*"
    "${PLAYDEN_HUI_SOURCE_DIR}/tools/font-baker/*")
foreach(trusted_source IN LISTS playden_hui_trusted_sources)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${trusted_source}")
endforeach()
if(NOT EXISTS "${PLAYDEN_STB_IMAGE_INCLUDE_DIR}/stb_image.h")
    message(FATAL_ERROR "PLAYDEN_STB_IMAGE_INCLUDE_DIR must contain stb_image.h")
endif()
string(JSON expected_stb_hash GET "${playden_dependency_lock}" homebrew_ui stb_image sha256)
file(SHA256 "${PLAYDEN_STB_IMAGE_INCLUDE_DIR}/stb_image.h" actual_stb_hash)
if(NOT actual_stb_hash STREQUAL expected_stb_hash)
    message(FATAL_ERROR "Pinned stb_image.h hash mismatch")
endif()
add_library(playden_steam_cm_codec STATIC
    "${CMAKE_SOURCE_DIR}/src/steam/backend/gzip.cpp"
    "${CMAKE_SOURCE_DIR}/src/steam/backend/binary_kv.cpp"
    "${CMAKE_SOURCE_DIR}/src/steam/backend/vdf.cpp"
    "${CMAKE_SOURCE_DIR}/src/steam/backend/app_info.cpp"
    "${CMAKE_SOURCE_DIR}/src/runtime/prospero_profile.cpp"
    "${CMAKE_SOURCE_DIR}/src/runtime/game_settings.cpp"
    "${CMAKE_SOURCE_DIR}/src/runtime/install_script.cpp"
    "${CMAKE_SOURCE_DIR}/src/runtime/msi_package.cpp"
    "${CMAKE_SOURCE_DIR}/src/runtime/burn_bundle.cpp"
    "${CMAKE_SOURCE_DIR}/src/runtime/redist_install.cpp"
    "${CMAKE_SOURCE_DIR}/src/runtime/game_probe.cpp")
target_include_directories(playden_steam_cm_codec PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_include_directories(playden_steam_cm_codec PRIVATE "${PLAYDEN_STB_IMAGE_INCLUDE_DIR}")
target_compile_features(playden_steam_cm_codec PUBLIC cxx_std_17)
target_link_libraries(playden_steam_cm_codec PUBLIC playden_game_paths playden_steam_protocol playden_managed_directory
    playden_runtime_steamworks playden_runtime_artifact_hash playden_miniz playden_library playden_steam_content_language)
if(PLAYDEN_PLATFORM_PS5)
    target_compile_options(playden_steam_cm_codec PRIVATE -fno-exceptions -fno-rtti)
endif()

add_library(playden_runtime_settings_publication STATIC "${CMAKE_SOURCE_DIR}/src/runtime/settings_publication.cpp"
    "${CMAKE_SOURCE_DIR}/src/runtime/settings_publication_owner.cpp")
target_include_directories(playden_runtime_settings_publication PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_compile_features(playden_runtime_settings_publication PUBLIC cxx_std_17)
target_link_libraries(playden_runtime_settings_publication PUBLIC playden_runtime_settings_publication_journal playden_runtime_publication_verifier playden_steam_cm_codec playden_runtime_install_service)
if(BUILD_TESTING AND NOT PLAYDEN_PLATFORM_PS5)
    foreach(package_test runtime_package_installer_test runtime_package_installer_native_policy_test)
        target_compile_definitions(${package_test} PRIVATE PLAYDEN_TEST_SETTINGS_OWNER_INTEGRATION=1)
        target_link_libraries(${package_test} PRIVATE playden_runtime_settings_publication)
    endforeach()
endif()

add_library(playden_runtime_settings_publication_journal STATIC
    "${CMAKE_SOURCE_DIR}/src/runtime/settings_publication_journal.cpp"
    "${PLAYDEN_PROSPERO_SOURCE_DIR}/src/pw_title_launch.c"
    "${PLAYDEN_PROSPERO_SOURCE_DIR}/src/pw_game_profile.c"
    "${PLAYDEN_PROSPERO_SOURCE_DIR}/src/pw_app_profile.c"
    "${PLAYDEN_PROSPERO_SOURCE_DIR}/src/pw_wine_launch.c")
target_include_directories(playden_runtime_settings_publication_journal PUBLIC "${CMAKE_SOURCE_DIR}/src"
    PRIVATE "${PLAYDEN_PROSPERO_SOURCE_DIR}/src")
target_compile_features(playden_runtime_settings_publication_journal PUBLIC cxx_std_17 PRIVATE c_std_11)
target_link_libraries(playden_runtime_settings_publication_journal PUBLIC playden_publication_transaction
    playden_runtime_installs playden_runtime_artifact_hash playden_runtime_publication_contract playden_runtime_publication_backup)

add_library(playden_runtime_publication_backup STATIC "${CMAKE_SOURCE_DIR}/src/runtime/publication_backup.cpp")
target_include_directories(playden_runtime_publication_backup PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_compile_features(playden_runtime_publication_backup PUBLIC cxx_std_17)
target_link_libraries(playden_runtime_publication_backup PUBLIC playden_runtime_artifact_hash playden_storage)

add_library(playden_runtime_settings_storage STATIC "${CMAKE_SOURCE_DIR}/src/runtime/settings_storage.cpp")
target_include_directories(playden_runtime_settings_storage PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_compile_features(playden_runtime_settings_storage PUBLIC cxx_std_17)
target_link_libraries(playden_runtime_settings_storage PUBLIC playden_steam_cm_codec playden_runtime_private_directories)

set(PLAYDEN_ZSTD_DIR "${CMAKE_SOURCE_DIR}/third_party/zstd")
add_library(playden_depot_codec STATIC
    "${CMAKE_SOURCE_DIR}/src/steam/backend/depot_chunk.cpp"
    "${CMAKE_SOURCE_DIR}/src/steam/backend/depot_manifest.cpp"
    "${CMAKE_SOURCE_DIR}/third_party/lzma-sdk/LzmaDec.c"
    "${PLAYDEN_ZSTD_DIR}/common/debug.c"
    "${PLAYDEN_ZSTD_DIR}/common/entropy_common.c"
    "${PLAYDEN_ZSTD_DIR}/common/error_private.c"
    "${PLAYDEN_ZSTD_DIR}/common/fse_decompress.c"
    "${PLAYDEN_ZSTD_DIR}/common/xxhash.c"
    "${PLAYDEN_ZSTD_DIR}/common/zstd_common.c"
    "${PLAYDEN_ZSTD_DIR}/decompress/huf_decompress.c"
    "${PLAYDEN_ZSTD_DIR}/decompress/zstd_ddict.c"
    "${PLAYDEN_ZSTD_DIR}/decompress/zstd_decompress.c"
    "${PLAYDEN_ZSTD_DIR}/decompress/zstd_decompress_block.c")
target_include_directories(playden_depot_codec PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_include_directories(playden_depot_codec PRIVATE
    "${CMAKE_SOURCE_DIR}/third_party/lzma-sdk"
    "${PLAYDEN_ZSTD_DIR}"
    "${PLAYDEN_ZSTD_DIR}/common"
    "${PLAYDEN_STB_IMAGE_INCLUDE_DIR}")
target_compile_definitions(playden_depot_codec PRIVATE ZSTD_DISABLE_ASM=1 ZSTD_LEGACY_SUPPORT=0 ZSTD_TRACE=0)
target_compile_features(playden_depot_codec PUBLIC cxx_std_17)
target_link_libraries(playden_depot_codec PUBLIC playden_curl_8_22 playden_steam_protocol)
if(PLAYDEN_PLATFORM_PS5)
    target_compile_options(playden_depot_codec PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>)
endif()
foreach(font_file
        fixtures/ui-comparison/assets/fonts/Barlow-Regular.ttf
        fixtures/ui-comparison/assets/fonts/Barlow-Medium.ttf
        fixtures/ui-comparison/assets/fonts/Barlow-SemiBold.ttf
        fixtures/ui-comparison/assets/fonts/BarlowCondensed-Medium.ttf
        fixtures/ui-comparison/assets/fonts/BarlowCondensed-SemiBold.ttf
        fixtures/ui-comparison/assets/fonts/NotoSansCJKsc-Regular.otf
        fixtures/ui-comparison/assets/fonts/NotoSansArabic.ttf
        fixtures/ui-comparison/assets/fonts/barlow-OFL.txt
        fixtures/ui-comparison/assets/fonts/barlowcondensed-OFL.txt
        fixtures/ui-comparison/assets/fonts/noto-cjk-LICENSE.txt
        fixtures/ui-comparison/assets/fonts/noto-arabic-OFL.txt
        assets/fonts/NotoSans-Regular.ttf
        assets/fonts/NotoSans-OFL.txt)
    string(JSON expected_font_hash GET "${playden_dependency_lock}" homebrew_ui assets "${font_file}")
    file(SHA256 "${CMAKE_SOURCE_DIR}/${font_file}" actual_font_hash)
    if(NOT actual_font_hash STREQUAL expected_font_hash)
        message(FATAL_ERROR "Pinned Hui font/license hash mismatch: ${font_file}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/${font_file}")
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${PLAYDEN_STB_IMAGE_INCLUDE_DIR}/stb_image.h")

file(GLOB playden_hui_gfx_sources CONFIGURE_DEPENDS
    "${PLAYDEN_HUI_SOURCE_DIR}/src/gfx/*.cpp")
file(GLOB playden_hui_widget_sources CONFIGURE_DEPENDS
    "${PLAYDEN_HUI_SOURCE_DIR}/src/ui/*.cpp"
    "${PLAYDEN_HUI_SOURCE_DIR}/src/ui/components/*.cpp")

add_library(playden_hui STATIC
    ${playden_hui_gfx_sources}
    ${playden_hui_widget_sources}
    "${PLAYDEN_HUI_SOURCE_DIR}/src/core/input.cpp")
target_compile_features(playden_hui PUBLIC cxx_std_20)
target_include_directories(playden_hui PUBLIC
    "${PLAYDEN_HUI_SOURCE_DIR}/src"
    "${PLAYDEN_STB_IMAGE_INCLUDE_DIR}"
    "${PLAYDEN_HUI_SOURCE_DIR}/third_party/stb")

add_library(playden_homebrew_model STATIC
    "${CMAKE_SOURCE_DIR}/src/homebrew/simulation.cpp"
    "${CMAKE_SOURCE_DIR}/src/homebrew/storage_sim.cpp"
    "${CMAKE_SOURCE_DIR}/src/homebrew/queue_layout.cpp"
    "${CMAKE_SOURCE_DIR}/src/homebrew/browse.cpp"
    "${CMAKE_SOURCE_DIR}/src/homebrew/store_art.cpp")
target_compile_features(playden_homebrew_model PUBLIC cxx_std_20)
target_link_libraries(playden_homebrew_model PUBLIC playden_library playden_diagnostic_log playden_steam_cm_codec)
target_include_directories(playden_homebrew_model PUBLIC "${CMAKE_SOURCE_DIR}/src")

foreach(qr_file qrcodegen.c qrcodegen.h LICENSE README.md)
    set(qr_path "${CMAKE_SOURCE_DIR}/third_party/qrcodegen/${qr_file}")
    string(JSON expected_qr_hash GET "${playden_dependency_lock}" qrcodegen files "${qr_file}")
    file(SHA256 "${qr_path}" actual_qr_hash)
    if(NOT actual_qr_hash STREQUAL expected_qr_hash)
        message(FATAL_ERROR "Pinned qrcodegen file hash mismatch: ${qr_file}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${qr_path}")
endforeach()

add_library(playden_homebrew_qr STATIC
    "${CMAKE_SOURCE_DIR}/src/homebrew/qr.cpp"
    "${CMAKE_SOURCE_DIR}/third_party/qrcodegen/qrcodegen.c")
target_compile_features(playden_homebrew_qr PUBLIC cxx_std_20 PRIVATE c_std_99)
target_include_directories(playden_homebrew_qr PUBLIC "${CMAKE_SOURCE_DIR}/src"
    PRIVATE "${CMAKE_SOURCE_DIR}/third_party/qrcodegen")

add_library(playden_auth_diagnostics STATIC
    "${CMAKE_SOURCE_DIR}/src/diagnostics/redaction.cpp")
target_compile_features(playden_auth_diagnostics PUBLIC cxx_std_17)
target_include_directories(playden_auth_diagnostics PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(playden_auth_diagnostics PUBLIC playden_steam_core)

add_library(playden_local_session_repository STATIC
    "${CMAKE_SOURCE_DIR}/src/steam/local_session_repository.cpp")
target_compile_features(playden_local_session_repository PUBLIC cxx_std_17)
target_include_directories(playden_local_session_repository PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(playden_local_session_repository PUBLIC playden_steam_core)

add_library(playden_library_controller STATIC
    "${CMAKE_SOURCE_DIR}/src/app/library_controller.cpp")
target_compile_features(playden_library_controller PUBLIC cxx_std_20)
target_include_directories(playden_library_controller PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(playden_library_controller PUBLIC playden_homebrew_model playden_library_service)

add_library(playden_persistence_controller STATIC "${CMAKE_SOURCE_DIR}/src/app/persistence_controller.cpp")
target_compile_features(playden_persistence_controller PUBLIC cxx_std_20)
target_include_directories(playden_persistence_controller PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(playden_persistence_controller PUBLIC playden_homebrew_model playden_persistence_service)

add_library(playden_homebrew_controller STATIC
    "${CMAKE_SOURCE_DIR}/src/homebrew/controller.cpp" "${CMAKE_SOURCE_DIR}/src/homebrew/uninstall_flow.cpp")
target_compile_features(playden_homebrew_controller PUBLIC cxx_std_20)
target_include_directories(playden_homebrew_controller PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(playden_homebrew_controller PUBLIC
    playden_homebrew_model playden_steam_core playden_auth_diagnostics playden_library_controller playden_persistence_controller playden_runtime_settings_storage
    playden_homebrew_runtime_host playden_self_update playden_local_store)

# Worker-owned per-game runtime composition (OWNER records, native inventory, kit layer).
add_library(playden_homebrew_runtime_host STATIC "${CMAKE_SOURCE_DIR}/src/homebrew/runtime_host.cpp"
    "${CMAKE_SOURCE_DIR}/src/runtime/writable_folders.cpp")
target_compile_features(playden_homebrew_runtime_host PUBLIC cxx_std_20)
target_include_directories(playden_homebrew_runtime_host PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(playden_homebrew_runtime_host PUBLIC playden_native_coordinator playden_runtime_settings_publication
    playden_runtime_installs playden_runtime_install_service playden_runtime_package_installer
    playden_runtime_private_directories playden_runtime_launch_service playden_runtime_session_reconciler
    playden_storage playden_steam_cm_codec playden_runtime_steamworks playden_runtime_layout_migration playden_local_store)

add_library(playden_homebrew_artwork STATIC
    "${CMAKE_SOURCE_DIR}/src/homebrew/artwork.cpp"
    "${CMAKE_SOURCE_DIR}/src/homebrew/artwork_backend.cpp"
    "${CMAKE_SOURCE_DIR}/src/homebrew/title_icon.cpp")
target_compile_features(playden_homebrew_artwork PUBLIC cxx_std_20)
target_include_directories(playden_homebrew_artwork PUBLIC "${CMAKE_SOURCE_DIR}/src"
    PRIVATE "${PLAYDEN_STB_IMAGE_INCLUDE_DIR}" "${CMAKE_SOURCE_DIR}/third_party/ps5-homebrew-ui/third_party/stb")
target_link_libraries(playden_homebrew_artwork PUBLIC playden_library playden_steam_http)

add_library(playden_homebrew_artwork_cache STATIC "${CMAKE_SOURCE_DIR}/src/homebrew/artwork_cache.cpp")
target_compile_features(playden_homebrew_artwork_cache PUBLIC cxx_std_20)
target_include_directories(playden_homebrew_artwork_cache PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(playden_homebrew_artwork_cache PUBLIC playden_homebrew_artwork)

add_library(playden_homebrew_ui STATIC
    "${CMAKE_SOURCE_DIR}/src/homebrew/view.cpp")
target_compile_features(playden_homebrew_ui PUBLIC cxx_std_20)
target_include_directories(playden_homebrew_ui PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(playden_homebrew_ui PUBLIC
    playden_homebrew_model playden_homebrew_qr playden_hui playden_homebrew_artwork_cache)

if(PLAYDEN_PLATFORM_PS5)
    file(READ "${CMAKE_SOURCE_DIR}/dependencies.lock.json" playden_dependency_lock)
    string(JSON playden_gl_version GET "${playden_dependency_lock}" ps5_opengl version)
    target_include_directories(playden_hui PUBLIC
        "${PLAYDEN_PS5_DEPS_DIR}/ps5-opengl/ps5-opengl-sdk-${playden_gl_version}/sdk/include")
    target_compile_definitions(playden_hui PUBLIC GL_GLEXT_PROTOTYPES=1)
    target_compile_options(playden_hui PUBLIC -fno-exceptions -fno-rtti)
    target_compile_options(playden_homebrew_model PUBLIC -fno-exceptions -fno-rtti)
    target_compile_options(playden_homebrew_qr PUBLIC
        $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions;-fno-rtti>)
    target_compile_options(playden_auth_diagnostics PUBLIC -fno-exceptions -fno-rtti)
    target_compile_options(playden_local_session_repository PUBLIC -fno-exceptions -fno-rtti)
    target_compile_options(playden_homebrew_controller PUBLIC -fno-exceptions -fno-rtti)
    target_compile_options(playden_library_controller PUBLIC -fno-exceptions -fno-rtti)
    target_compile_options(playden_persistence_controller PUBLIC -fno-exceptions -fno-rtti)
    target_compile_options(playden_homebrew_artwork PUBLIC -fno-exceptions -fno-rtti)
    target_compile_options(playden_homebrew_artwork_cache PUBLIC -fno-exceptions -fno-rtti)
    target_compile_options(playden_homebrew_ui PUBLIC -fno-exceptions -fno-rtti)
    include("${CMAKE_CURRENT_LIST_DIR}/HomebrewPS5.cmake")
    playden_add_homebrew_ps5_title()
    return()
endif()

if(NOT APPLE)
    message(FATAL_ERROR "The direct ps5-homebrew-ui host currently supports macOS only")
endif()

find_package(OpenGL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET "sdl2>=2.0")
target_sources(playden_hui PRIVATE
    "${PLAYDEN_HUI_SOURCE_DIR}/host/platform_host.cpp")
target_compile_definitions(playden_hui PUBLIC GL_SILENCE_DEPRECATION=1)
target_link_libraries(playden_hui PUBLIC OpenGL::GL)
target_compile_options(playden_homebrew_ui PRIVATE -Wno-deprecated-declarations)

set(playden_homebrew_assets "${CMAKE_BINARY_DIR}/homebrew-assets")
set(playden_homebrew_fonts "${CMAKE_BINARY_DIR}/homebrew-fonts")
set(PLAYDEN_HOMEBREW_FONT_DIR "${playden_homebrew_fonts}" CACHE INTERNAL
    "Baked Barlow atlas directory")
set(playden_hui_baker "${CMAKE_BINARY_DIR}/hui-bake-font")

add_executable(playden_hui_font_baker EXCLUDE_FROM_ALL
    "${PLAYDEN_HUI_SOURCE_DIR}/tools/font-baker/bake_font.cpp")
target_compile_features(playden_hui_font_baker PRIVATE cxx_std_20)
set_target_properties(playden_hui_font_baker PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    OUTPUT_NAME "hui-bake-font")

set(playden_barlow_outputs)
foreach(font_spec IN ITEMS
        "Barlow-Regular.ttf|barlow-regular.huifont"
        "Barlow-Medium.ttf|barlow-medium.huifont"
        "Barlow-SemiBold.ttf|barlow-semibold.huifont"
        "BarlowCondensed-Medium.ttf|barlow-condensed-medium.huifont"
        "BarlowCondensed-SemiBold.ttf|barlow-condensed-semibold.huifont")
    string(REPLACE "|" ";" font_fields "${font_spec}")
    list(GET font_fields 0 font_source)
    list(GET font_fields 1 font_output)
    set(font_input "${CMAKE_SOURCE_DIR}/fixtures/ui-comparison/assets/fonts/${font_source}")
    set(font_file "${playden_homebrew_fonts}/${font_output}")
    add_custom_command(
        OUTPUT "${font_file}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${playden_homebrew_fonts}"
        COMMAND "$<TARGET_FILE:playden_hui_font_baker>" "${font_input}" "${font_file}"
            56 8 2048 european
        DEPENDS playden_hui_font_baker "${font_input}"
        COMMENT "Baking ${font_output} with Latin, Greek, and Cyrillic glyphs"
        VERBATIM)
    list(APPEND playden_barlow_outputs "${font_file}")
endforeach()

set(playden_catalog "${CMAKE_SOURCE_DIR}/fixtures/ui-comparison/catalog.json")
foreach(font_spec IN ITEMS
        "NotoSansCJKsc-Regular.otf|noto-cjk-fallback.huifont|european"
        "NotoSansArabic.ttf|noto-arabic-fallback.huifont|basic")
    string(REPLACE "|" ";" font_fields "${font_spec}")
    list(GET font_fields 0 font_source)
    list(GET font_fields 1 font_output)
    list(GET font_fields 2 font_glyphs)
    set(font_input "${CMAKE_SOURCE_DIR}/fixtures/ui-comparison/assets/fonts/${font_source}")
    set(font_file "${playden_homebrew_fonts}/${font_output}")
    add_custom_command(
        OUTPUT "${font_file}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${playden_homebrew_fonts}"
        COMMAND "$<TARGET_FILE:playden_hui_font_baker>" "${font_input}" "${font_file}"
            56 8 2048 "${font_glyphs},text=${playden_catalog}"
        DEPENDS playden_hui_font_baker "${font_input}" "${playden_catalog}"
        COMMENT "Baking ${font_output} for shared international fixture text"
        VERBATIM)
    list(APPEND playden_barlow_outputs "${font_file}")
endforeach()

# Supplemental Greek coverage lives outside the immutable comparison assets.
set(playden_greek_font "${playden_homebrew_fonts}/noto-greek-fallback.huifont")
add_custom_command(
    OUTPUT "${playden_greek_font}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${playden_homebrew_fonts}"
    COMMAND "$<TARGET_FILE:playden_hui_font_baker>"
        "${CMAKE_SOURCE_DIR}/assets/fonts/NotoSans-Regular.ttf" "${playden_greek_font}"
        56 8 2048 "european,text=${playden_catalog}"
    DEPENDS playden_hui_font_baker "${CMAKE_SOURCE_DIR}/assets/fonts/NotoSans-Regular.ttf"
        "${CMAKE_SOURCE_DIR}/assets/fonts/NotoSans-OFL.txt" "${playden_catalog}"
    COMMENT "Baking complete Greek fallback coverage"
    VERBATIM)
list(APPEND playden_barlow_outputs "${playden_greek_font}")

add_custom_target(playden_homebrew_assets ALL
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_SOURCE_DIR}/assets/fonts/NotoSans-OFL.txt" "${playden_homebrew_fonts}/NotoSans-OFL.txt"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_SOURCE_DIR}/fixtures/ui-comparison/assets" "${playden_homebrew_assets}"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${playden_homebrew_fonts}" "${playden_homebrew_assets}/fonts"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_SOURCE_DIR}/runtime" "${playden_homebrew_assets}/runtime"
    DEPENDS ${playden_barlow_outputs}
    VERBATIM)

add_executable(playden-homebrew "${CMAKE_SOURCE_DIR}/src/homebrew/host.cpp")
target_compile_features(playden-homebrew PRIVATE cxx_std_20)
target_compile_definitions(playden-homebrew PRIVATE
    GL_SILENCE_DEPRECATION=1
    PLAYDEN_HOMEBREW_ASSETS_DIR="${playden_homebrew_assets}"
    PLAYDEN_HOMEBREW_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/fixtures/ui-comparison"
    PLAYDEN_HOMEBREW_DEFAULT_PROFILE="${CMAKE_BINARY_DIR}/homebrew-profile.json"
    PLAYDEN_HOMEBREW_DEFAULT_DATA_DIR="${CMAKE_BINARY_DIR}/playden-data"
    PLAYDEN_HOMEBREW_CA_BUNDLE="${playden_staged_ca}")
target_link_libraries(playden-homebrew PRIVATE
    playden_homebrew_ui playden_homebrew_controller playden_local_session_repository
    playden_library playden_steam_auth playden_steam_library playden_steam_cm playden_local_copy PkgConfig::SDL2 OpenGL::GL)
add_dependencies(playden-homebrew playden_homebrew_assets playden_steam_trust)
set_target_properties(playden-homebrew PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

if(BUILD_TESTING)
    add_executable(homebrew_focus_motion_test "${CMAKE_SOURCE_DIR}/tests/homebrew_focus_motion_test.cpp")
    target_compile_features(homebrew_focus_motion_test PRIVATE cxx_std_20)
    target_include_directories(homebrew_focus_motion_test PRIVATE "${CMAKE_SOURCE_DIR}/src")
    add_test(NAME homebrew_focus_motion COMMAND homebrew_focus_motion_test)
    add_executable(homebrew_usb_keyboard_test "${CMAKE_SOURCE_DIR}/tests/homebrew_usb_keyboard_test.cpp"
        "${CMAKE_SOURCE_DIR}/src/homebrew/usb_keyboard.cpp")
    target_compile_features(homebrew_usb_keyboard_test PRIVATE cxx_std_20)
    target_include_directories(homebrew_usb_keyboard_test PRIVATE "${CMAKE_SOURCE_DIR}/src")
    add_test(NAME homebrew_usb_keyboard COMMAND homebrew_usb_keyboard_test)
    add_executable(homebrew_storage_test "${CMAKE_SOURCE_DIR}/tests/homebrew_storage_test.cpp")
    target_compile_features(homebrew_storage_test PRIVATE cxx_std_20)
    target_link_libraries(homebrew_storage_test PRIVATE playden_homebrew_model)
    add_test(NAME homebrew_storage COMMAND homebrew_storage_test)
    add_executable(homebrew_runtime_host_test "${CMAKE_SOURCE_DIR}/tests/homebrew_runtime_host_test.cpp")
    target_link_libraries(homebrew_runtime_host_test PRIVATE playden_homebrew_runtime_host playden_runtime_artifact_hash)
    add_test(NAME homebrew_runtime_host COMMAND homebrew_runtime_host_test "${CMAKE_SOURCE_DIR}/runtime")
    add_executable(homebrew_installed_locations_test "${CMAKE_SOURCE_DIR}/tests/homebrew_installed_locations_test.cpp")
    target_link_libraries(homebrew_installed_locations_test PRIVATE playden_homebrew_controller playden_runtime_artifact_hash)
    add_test(NAME homebrew_installed_locations COMMAND homebrew_installed_locations_test "${CMAKE_SOURCE_DIR}/runtime")
    add_executable(runtime_writable_folders_test "${CMAKE_SOURCE_DIR}/tests/runtime_writable_folders_test.cpp"
        "${CMAKE_SOURCE_DIR}/src/runtime/writable_folders.cpp")
    target_compile_features(runtime_writable_folders_test PRIVATE cxx_std_20)
    target_include_directories(runtime_writable_folders_test PRIVATE "${CMAKE_SOURCE_DIR}/src")
    add_test(NAME runtime_writable_folders COMMAND runtime_writable_folders_test)
    add_executable(runtime_writable_folders_native_policy_test
        "${CMAKE_SOURCE_DIR}/tests/runtime_writable_folders_test.cpp"
        "${CMAKE_SOURCE_DIR}/tests/directory_gateway_host.cpp"
        "${CMAKE_SOURCE_DIR}/src/runtime/writable_folders.cpp")
    target_compile_features(runtime_writable_folders_native_policy_test PRIVATE cxx_std_20)
    target_include_directories(runtime_writable_folders_native_policy_test PRIVATE "${CMAKE_SOURCE_DIR}/src")
    target_compile_definitions(runtime_writable_folders_native_policy_test PRIVATE PLAYDEN_PLATFORM_PS5=1)
    add_test(NAME runtime_writable_folders_native_policy COMMAND runtime_writable_folders_native_policy_test)
    add_executable(homebrew_title_icon_test "${CMAKE_SOURCE_DIR}/tests/homebrew_title_icon_test.cpp")
    target_compile_features(homebrew_title_icon_test PRIVATE cxx_std_20)
    target_include_directories(homebrew_title_icon_test PRIVATE "${PLAYDEN_STB_IMAGE_INCLUDE_DIR}")
    target_link_libraries(homebrew_title_icon_test PRIVATE playden_homebrew_artwork)
    add_test(NAME homebrew_title_icon COMMAND homebrew_title_icon_test)
    add_executable(homebrew_controller_tour_test "${CMAKE_SOURCE_DIR}/tests/homebrew_controller_tour_test.cpp")
    target_compile_features(homebrew_controller_tour_test PRIVATE cxx_std_20)
    target_link_libraries(homebrew_controller_tour_test PRIVATE playden_homebrew_model)
    add_test(NAME homebrew_controller_tour COMMAND homebrew_controller_tour_test "${CMAKE_SOURCE_DIR}/fixtures/ui-comparison")
    add_executable(homebrew_management_tour_test "${CMAKE_SOURCE_DIR}/tests/homebrew_management_tour_test.cpp")
    target_compile_features(homebrew_management_tour_test PRIVATE cxx_std_20)
    target_link_libraries(homebrew_management_tour_test PRIVATE playden_homebrew_model)
    add_test(NAME homebrew_management_tour COMMAND homebrew_management_tour_test "${CMAKE_SOURCE_DIR}/fixtures/ui-comparison")
    add_executable(homebrew_host_input_test "${CMAKE_SOURCE_DIR}/tests/homebrew_host_input_test.cpp")
    target_compile_features(homebrew_host_input_test PRIVATE cxx_std_20)
    target_compile_definitions(homebrew_host_input_test PRIVATE
        PLAYDEN_HOMEBREW_DEFAULT_PROFILE="unused"
        PLAYDEN_HOMEBREW_DEFAULT_DATA_DIR="unused-live-data"
        PLAYDEN_HOMEBREW_CA_BUNDLE="${playden_staged_ca}"
        PLAYDEN_HOMEBREW_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/fixtures/ui-comparison"
        PLAYDEN_HOMEBREW_ASSETS_DIR="${playden_homebrew_assets}")
    target_link_libraries(homebrew_host_input_test PRIVATE playden_homebrew_ui
        playden_homebrew_controller playden_local_session_repository playden_library playden_steam_auth playden_steam_library playden_steam_cm
        playden_local_copy PkgConfig::SDL2 OpenGL::GL)
    add_test(NAME homebrew_host_input COMMAND homebrew_host_input_test "${CMAKE_SOURCE_DIR}/fixtures/ui-comparison")
    add_executable(homebrew_browse_test "${CMAKE_SOURCE_DIR}/tests/homebrew_browse_test.cpp")
    target_compile_features(homebrew_browse_test PRIVATE cxx_std_20)
    target_link_libraries(homebrew_browse_test PRIVATE playden_homebrew_model)
    add_test(NAME homebrew_browse COMMAND homebrew_browse_test "${CMAKE_SOURCE_DIR}/fixtures/ui-comparison")
    add_executable(homebrew_simulation_test
        "${CMAKE_SOURCE_DIR}/tests/homebrew_simulation_test.cpp")
    target_compile_features(homebrew_simulation_test PRIVATE cxx_std_20)
    target_link_libraries(homebrew_simulation_test PRIVATE playden_homebrew_model)
    add_test(NAME homebrew_simulation
        COMMAND homebrew_simulation_test "${CMAKE_SOURCE_DIR}/fixtures/ui-comparison")

    add_executable(homebrew_live_model_test
        "${CMAKE_SOURCE_DIR}/tests/homebrew_live_model_test.cpp")
    target_compile_features(homebrew_live_model_test PRIVATE cxx_std_20)
    target_link_libraries(homebrew_live_model_test PRIVATE playden_homebrew_model)
    add_test(NAME homebrew_live_model COMMAND homebrew_live_model_test)

    add_executable(homebrew_library_model_test
        "${CMAKE_SOURCE_DIR}/tests/homebrew_library_model_test.cpp")
    target_compile_features(homebrew_library_model_test PRIVATE cxx_std_20)
    target_link_libraries(homebrew_library_model_test PRIVATE playden_homebrew_model)
    add_test(NAME homebrew_library_model COMMAND homebrew_library_model_test)
    add_executable(homebrew_uninstall_flow_test "${CMAKE_SOURCE_DIR}/tests/homebrew_uninstall_flow_test.cpp"
        "${CMAKE_SOURCE_DIR}/src/homebrew/uninstall_flow.cpp")
    target_compile_features(homebrew_uninstall_flow_test PRIVATE cxx_std_20)
    target_link_libraries(homebrew_uninstall_flow_test PRIVATE playden_homebrew_model)
    add_test(NAME homebrew_uninstall_flow COMMAND homebrew_uninstall_flow_test)

    add_executable(homebrew_library_graphical_test
        "${CMAKE_SOURCE_DIR}/tests/homebrew_library_graphical_test.cpp")
    target_compile_features(homebrew_library_graphical_test PRIVATE cxx_std_20)
    target_compile_definitions(homebrew_library_graphical_test PRIVATE
        PLAYDEN_HOMEBREW_ASSETS_DIR="${playden_homebrew_assets}")
    target_link_libraries(homebrew_library_graphical_test PRIVATE
        playden_persistence_controller playden_library_controller playden_homebrew_ui PkgConfig::SDL2 OpenGL::GL)
    add_dependencies(homebrew_library_graphical_test playden_homebrew_assets)
    add_test(NAME homebrew_library_graphical COMMAND homebrew_library_graphical_test
        "${CMAKE_BINARY_DIR}/synthetic-live-library")

    add_executable(homebrew_artwork_graphical_test "${CMAKE_SOURCE_DIR}/tests/homebrew_artwork_graphical_test.cpp")
    target_compile_features(homebrew_artwork_graphical_test PRIVATE cxx_std_20)
    target_compile_definitions(homebrew_artwork_graphical_test PRIVATE PLAYDEN_HOMEBREW_ASSETS_DIR="${playden_homebrew_assets}")
    target_link_libraries(homebrew_artwork_graphical_test PRIVATE playden_homebrew_ui PkgConfig::SDL2 OpenGL::GL)
    add_dependencies(homebrew_artwork_graphical_test playden_homebrew_assets)
    add_test(NAME homebrew_artwork_graphical COMMAND homebrew_artwork_graphical_test "${CMAKE_BINARY_DIR}/synthetic-live-artwork")
    add_executable(homebrew_glyph_fallback_graphical_test "${CMAKE_SOURCE_DIR}/tests/homebrew_glyph_fallback_graphical_test.cpp")
    target_compile_features(homebrew_glyph_fallback_graphical_test PRIVATE cxx_std_20)
    target_compile_definitions(homebrew_glyph_fallback_graphical_test PRIVATE PLAYDEN_HOMEBREW_ASSETS_DIR="${playden_homebrew_assets}")
    target_link_libraries(homebrew_glyph_fallback_graphical_test PRIVATE playden_homebrew_ui PkgConfig::SDL2 OpenGL::GL)
    add_dependencies(homebrew_glyph_fallback_graphical_test playden_homebrew_assets)
    add_test(NAME homebrew_glyph_fallback_graphical COMMAND homebrew_glyph_fallback_graphical_test "${CMAKE_BINARY_DIR}/glyph-fallback")

    add_executable(title_runtime_test tests/title_runtime_test.cpp src/platform/ps5/title_runtime.cpp)
    target_include_directories(title_runtime_test PRIVATE "${CMAKE_SOURCE_DIR}/src")
    target_compile_features(title_runtime_test PRIVATE cxx_std_17)
    add_test(NAME title_runtime COMMAND title_runtime_test)
    add_executable(title_registry_test tests/title_registry_test.cpp src/platform/ps5/title_registry.cpp src/platform/ps5/title_runtime.cpp)
    target_link_libraries(title_registry_test PRIVATE playden_steam_protocol)
    target_include_directories(title_registry_test PRIVATE "${CMAKE_SOURCE_DIR}/src")
    target_compile_features(title_registry_test PRIVATE cxx_std_17)
    add_test(NAME title_registry COMMAND title_registry_test)
    foreach(test_name steam_gzip steam_binary_kv steam_vdf steam_app_info prospero_profile game_settings game_probe)
        add_executable(${test_name}_test "${CMAKE_SOURCE_DIR}/tests/${test_name}_test.cpp")
        target_link_libraries(${test_name}_test PRIVATE playden_steam_cm_codec)
        target_compile_features(${test_name}_test PRIVATE cxx_std_20)
    endforeach()
    foreach(test_name steam_gzip steam_binary_kv steam_vdf steam_app_info)
        add_test(NAME ${test_name} COMMAND ${test_name}_test)
    endforeach()
    add_test(NAME prospero_profile COMMAND prospero_profile_test "${CMAKE_SOURCE_DIR}/fixtures/prospero-win-profiles")
    add_test(NAME game_settings COMMAND game_settings_test "${CMAKE_SOURCE_DIR}")
    add_test(NAME game_probe COMMAND game_probe_test)
    add_executable(runtime_redist_test tests/runtime_redist_test.cpp)
    target_link_libraries(runtime_redist_test PRIVATE playden_steam_cm_codec)
    target_compile_features(runtime_redist_test PRIVATE cxx_std_20)
    add_test(NAME runtime_redist COMMAND runtime_redist_test)
    add_executable(runtime_steamworks_test tests/runtime_steamworks_test.cpp)
    target_link_libraries(runtime_steamworks_test PRIVATE playden_runtime_steamworks)
    add_test(NAME runtime_steamworks COMMAND runtime_steamworks_test "${CMAKE_SOURCE_DIR}/runtime/steamworks")
    add_executable(runtime_steam_stub_test tests/runtime_steam_stub_test.cpp)
    target_link_libraries(runtime_steam_stub_test PRIVATE playden_runtime_steamworks playden_curl_8_22)
    add_test(NAME runtime_steam_stub COMMAND runtime_steam_stub_test)
    target_link_libraries(game_probe_test PRIVATE playden_curl_8_22)
    add_executable(runtime_dotnet_heap_test tests/runtime_dotnet_heap_test.cpp)
    target_link_libraries(runtime_dotnet_heap_test PRIVATE playden_runtime_steamworks)
    add_test(NAME runtime_dotnet_heap COMMAND runtime_dotnet_heap_test)
    # Inject failures only at the POSIX I/O boundary of this test's production
    # compilation; the shipped settings implementation has ordinary syscalls.
    add_executable(runtime_settings_publication_test tests/runtime_settings_publication_test.cpp)
    target_link_libraries(runtime_settings_publication_test PRIVATE playden_runtime_settings_publication)
    add_test(NAME runtime_settings_publication COMMAND runtime_settings_publication_test)
    add_executable(runtime_settings_publication_journal_test tests/runtime_settings_publication_journal_test.cpp)
    target_link_libraries(runtime_settings_publication_journal_test PRIVATE playden_runtime_settings_publication_journal)
    add_test(NAME runtime_settings_publication_journal COMMAND runtime_settings_publication_journal_test)
    # Files from before games were named by store load and are written back unchanged.
    add_executable(identity_fixtures_test tests/identity_fixtures_test.cpp)
    target_link_libraries(identity_fixtures_test PRIVATE playden_steam_core playden_runtime_installs
        playden_runtime_settings_publication_journal playden_steam_cm_codec playden_library)
    add_test(NAME identity_fixtures COMMAND identity_fixtures_test "${CMAKE_SOURCE_DIR}/tests/fixtures/identity")
    add_executable(runtime_publication_backup_test tests/runtime_publication_backup_test.cpp)
    target_link_libraries(runtime_publication_backup_test PRIVATE playden_runtime_publication_backup)
    add_test(NAME runtime_publication_backup COMMAND runtime_publication_backup_test)
    add_executable(runtime_settings_storage_test tests/runtime_settings_storage_test.cpp)
    target_link_libraries(runtime_settings_storage_test PRIVATE playden_runtime_settings_storage)
    add_test(NAME runtime_settings_storage COMMAND runtime_settings_storage_test)
    add_executable(game_settings_io_test tests/game_settings_io_test.cpp src/runtime/game_settings.cpp src/runtime/game_probe.cpp src/platform/managed_directory.cpp src/library/store.cpp)
    target_link_libraries(game_settings_io_test PRIVATE playden_steam_cm_codec)
    target_compile_features(game_settings_io_test PRIVATE cxx_std_20)
    target_compile_definitions(game_settings_io_test PRIVATE
        PLAYDEN_PLATFORM_PS5=1 openat=playden_test_openat
        fsync=playden_test_fsync rename=playden_test_rename unlink=playden_test_unlink
        renameat=playden_test_renameat unlinkat=playden_test_unlinkat)
    add_test(NAME game_settings_io COMMAND game_settings_io_test "${CMAKE_SOURCE_DIR}")
    add_executable(steam_depot_chunk_test "${CMAKE_SOURCE_DIR}/tests/steam_depot_chunk_test.cpp")
    target_link_libraries(steam_depot_chunk_test PRIVATE playden_depot_codec)
    target_compile_features(steam_depot_chunk_test PRIVATE cxx_std_20)
    add_test(NAME steam_depot_chunk COMMAND steam_depot_chunk_test)
    add_executable(steam_depot_manifest_test "${CMAKE_SOURCE_DIR}/tests/steam_depot_manifest_test.cpp")
    target_link_libraries(steam_depot_manifest_test PRIVATE playden_depot_codec)
    target_compile_features(steam_depot_manifest_test PRIVATE cxx_std_20)
    add_test(NAME steam_depot_manifest COMMAND steam_depot_manifest_test "${CMAKE_SOURCE_DIR}/fixtures/steam/manifest")
    foreach(test_name homebrew_artwork homebrew_artwork_backend homebrew_artwork_cache)
        add_executable(${test_name}_test "${CMAKE_SOURCE_DIR}/tests/${test_name}_test.cpp")
        target_link_libraries(${test_name}_test PRIVATE playden_homebrew_artwork_cache)
        target_compile_features(${test_name}_test PRIVATE cxx_std_20)
        add_test(NAME ${test_name} COMMAND ${test_name}_test)
    endforeach()
    foreach(test_name homebrew_persistence_model persistence_controller homebrew_persistence_integration)
        add_executable(${test_name}_test "${CMAKE_SOURCE_DIR}/tests/${test_name}_test.cpp")
        target_link_libraries(${test_name}_test PRIVATE playden_homebrew_controller)
        target_compile_features(${test_name}_test PRIVATE cxx_std_20)
        add_test(NAME ${test_name} COMMAND ${test_name}_test)
    endforeach()
    add_executable(homebrew_account_test
        "${CMAKE_SOURCE_DIR}/tests/homebrew_account_test.cpp")
    target_compile_features(homebrew_account_test PRIVATE cxx_std_20)
    target_link_libraries(homebrew_account_test PRIVATE playden_homebrew_controller playden_local_session_repository)
    add_test(NAME homebrew_account COMMAND homebrew_account_test)

    add_executable(library_controller_test "${CMAKE_SOURCE_DIR}/tests/library_controller_test.cpp")
    target_link_libraries(library_controller_test PRIVATE playden_library_controller)
    add_test(NAME library_controller COMMAND library_controller_test)

    add_executable(local_session_repository_test
        "${CMAKE_SOURCE_DIR}/tests/local_session_repository_test.cpp")
    target_compile_features(local_session_repository_test PRIVATE cxx_std_17)
    target_link_libraries(local_session_repository_test PRIVATE playden_local_session_repository)
    add_test(NAME local_session_repository COMMAND local_session_repository_test)

    add_executable(auth_redaction_test
        "${CMAKE_SOURCE_DIR}/tests/auth_redaction_test.cpp")
    target_compile_features(auth_redaction_test PRIVATE cxx_std_17)
    target_link_libraries(auth_redaction_test PRIVATE playden_auth_diagnostics)
    add_test(NAME auth_redaction COMMAND auth_redaction_test)

    add_executable(homebrew_qr_test
        "${CMAKE_SOURCE_DIR}/tests/homebrew_qr_test.cpp")
    target_compile_features(homebrew_qr_test PRIVATE cxx_std_20)
    target_compile_definitions(homebrew_qr_test PRIVATE
        PLAYDEN_QR_GRAPHICAL_TEST=1
        PLAYDEN_HOMEBREW_ASSETS_DIR="${playden_homebrew_assets}")
    target_link_libraries(homebrew_qr_test PRIVATE
        playden_homebrew_ui PkgConfig::SDL2 OpenGL::GL)
    add_dependencies(homebrew_qr_test playden_homebrew_assets)
    add_test(NAME homebrew_qr COMMAND homebrew_qr_test)
    add_test(NAME homebrew_qr_graphical COMMAND homebrew_qr_test
        "${CMAKE_BINARY_DIR}/qr-roundtrip.png" 1920 1080)

    add_executable(homebrew_font_atlas_test "${CMAKE_SOURCE_DIR}/tests/homebrew_font_atlas_test.cpp")
    target_compile_features(homebrew_font_atlas_test PRIVATE cxx_std_20)
    target_link_libraries(homebrew_font_atlas_test PRIVATE playden_hui)
    add_dependencies(homebrew_font_atlas_test playden_homebrew_assets)
    foreach(font_spec IN ITEMS
            "Barlow-Regular.ttf|barlow-regular.huifont|latin"
            "NotoSansCJKsc-Regular.otf|noto-cjk-fallback.huifont|cff"
            "NotoSansArabic.ttf|noto-arabic-fallback.huifont|arabic")
        string(REPLACE "|" ";" font_fields "${font_spec}")
        list(GET font_fields 0 font_source)
        list(GET font_fields 1 font_output)
        list(GET font_fields 2 font_test)
        add_test(NAME homebrew_font_${font_test}
            COMMAND homebrew_font_atlas_test
                "${CMAKE_SOURCE_DIR}/fixtures/ui-comparison/assets/fonts/${font_source}"
                "${playden_homebrew_fonts}/${font_output}" "?")
    endforeach()

    file(READ "${playden_catalog}" playden_font_catalog_json)
    string(JSON playden_font_game_count LENGTH "${playden_font_catalog_json}" games)
    math(EXPR playden_font_game_last "${playden_font_game_count} - 1")
    set(playden_greek_title "")
    foreach(game_index RANGE 0 ${playden_font_game_last})
        string(JSON game_id GET "${playden_font_catalog_json}" games ${game_index} id)
        if(game_id EQUAL 900003)
            string(JSON playden_greek_title GET "${playden_font_catalog_json}" games ${game_index} title)
        endif()
    endforeach()
    if(playden_greek_title STREQUAL "")
        message(FATAL_ERROR "Greek fixture title missing from font coverage test")
    endif()
    add_test(NAME homebrew_font_greek
        COMMAND homebrew_font_atlas_test
            "${CMAKE_SOURCE_DIR}/assets/fonts/NotoSans-Regular.ttf"
            "${playden_greek_font}" "${playden_greek_title}?")

    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME native_startup_order COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_native_startup_order.py")
    add_test(NAME console_kit_upload COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_console_kit_upload.py")
    add_test(NAME ui_comparison_fixture
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/verify_ui_comparison.py"
            "${CMAKE_SOURCE_DIR}/fixtures/ui-comparison")
    set_tests_properties(ui_comparison_fixture PROPERTIES
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
endif()
