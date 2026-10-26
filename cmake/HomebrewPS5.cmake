# Direct Hui native title packaging; no Rml archives in the link graph.
function(playden_add_homebrew_ps5_title)
	if(NOT DEFINED ENV{PS5_PAYLOAD_SDK})
		message(FATAL_ERROR "PS5_PAYLOAD_SDK is unset; use scripts/build-homebrew-ps5.sh")
	endif()
	if(NOT PLAYDEN_PS5_TITLE_ID MATCHES "^(FAKE|PPSA)[0-9][0-9][0-9][0-9][0-9]$")
		message(FATAL_ERROR "PLAYDEN_PS5_TITLE_ID is required and must match FAKE12345 or PPSA12345")
	endif()

	set(sdk "$ENV{PS5_PAYLOAD_SDK}")
	set(sdk_deps "${PLAYDEN_PS5_DEPS_DIR}")
	# Native tooling, linker script and runtime shim come from the pinned boilerplate submodule.
	set(boilerplate "${CMAKE_SOURCE_DIR}/third_party/ps5-native-app-boilerplate")
	if(NOT EXISTS "${boilerplate}/tooling/native/sce_module_writer.cpp")
		message(FATAL_ERROR "Missing boilerplate submodule; run: git submodule update --init third_party/ps5-native-app-boilerplate")
	endif()
	set(gl_source "${CMAKE_SOURCE_DIR}/third_party/ps5-opengl")
	file(READ "${CMAKE_SOURCE_DIR}/dependencies.lock.json" dependency_lock)
	string(JSON gl_version GET "${dependency_lock}" ps5_opengl version)
	set(gl_sdk "${sdk_deps}/ps5-opengl/ps5-opengl-sdk-${gl_version}/sdk")
	set(out "${CMAKE_BINARY_DIR}")
	set(app "${out}/dist/${PLAYDEN_PS5_TITLE_ID}")
	set(host "${out}/host-tools")
	set(native "${host}/native-src")
	string(JSON expected_gl_sha GET "${dependency_lock}" ps5_opengl libPS5OpenGL_sha256)
	string(JSON expected_gl_manifest_sha GET "${dependency_lock}" ps5_opengl sdk_manifest_sha256)
	string(JSON expected_builtins_sha GET "${dependency_lock}" compiler_rt_builtins sha256)
	string(JSON expected_libc_sha GET "${dependency_lock}" ps5_native_app_boilerplate libc_prx_sha256)
	file(SHA256 "${gl_sdk}/lib/libPS5OpenGL.a" actual_gl_sha)
	# libPS5OpenGL.a only names the real libraries; scripts/fetch-deps.sh checks every file against this manifest.
	file(SHA256 "${gl_sdk}/manifest.sha256" actual_gl_manifest_sha)
	file(SHA256 "${sdk_deps}/compiler-rt/libclang_rt.builtins-x86_64.a" actual_builtins_sha)
	if(NOT actual_gl_sha STREQUAL expected_gl_sha)
		message(FATAL_ERROR "libPS5OpenGL.a hash mismatch: expected ${expected_gl_sha}, found ${actual_gl_sha}")
	endif()
	if(NOT actual_gl_manifest_sha STREQUAL expected_gl_manifest_sha)
		message(FATAL_ERROR "ps5-opengl SDK manifest hash mismatch: expected ${expected_gl_manifest_sha}, found ${actual_gl_manifest_sha}")
	endif()
	if(NOT actual_builtins_sha STREQUAL expected_builtins_sha)
		message(FATAL_ERROR "compiler-rt builtins hash mismatch: expected ${expected_builtins_sha}, found ${actual_builtins_sha}")
	endif()

	file(MAKE_DIRECTORY "${out}/link/tooling/native" "${out}/stubs")
	file(COPY "${boilerplate}/tooling/native/" DESTINATION "${native}")
	set(writer "${native}/sce_module_writer.cpp")
	file(READ "${writer}" writer_contents)
	set(heap_default "write_u64(result.data, result.heap_size, std::numeric_limits<std::uint64_t>::max());")
	string(FIND "${writer_contents}" "${heap_default}" heap_at)
	if(heap_at EQUAL -1)
		message(FATAL_ERROR "Could not apply the pinned 256 MiB PS5 heap patch")
	endif()
	string(REPLACE "${heap_default}" "write_u64(result.data, result.heap_size, 0x10000000ULL);"
		writer_contents "${writer_contents}")
	file(WRITE "${writer}" "${writer_contents}")
	file(COPY "${boilerplate}/tooling/native/ps5-pie.ld" DESTINATION "${out}/link/tooling/native")

	file(GLOB sdk_stubs CONFIGURE_DEPENDS "${sdk}/target/lib/*.so")
	set(link_stubs)
	foreach(stub IN LISTS sdk_stubs)
		get_filename_component(name "${stub}" NAME)
		file(CREATE_LINK "${stub}" "${out}/stubs/${name}" SYMBOLIC COPY_ON_ERROR)
		list(APPEND link_stubs "${out}/stubs/${name}")
	endforeach()
	foreach(name libSceAgc.so libSceAgcDriver.so)
		file(COPY_FILE "${gl_sdk}/lib/${name}" "${out}/stubs/${name}" ONLY_IF_DIFFERENT)
		list(APPEND link_stubs "${out}/stubs/${name}")
	endforeach()
	# The system keyboard needs sceCommonDialogInitialize, which the SDK has no stub for: a
	# link-only library named after the system module turns it into an import.
	set(common_dialog_stub "${out}/stubs/libSceCommonDialog.so")
	add_custom_command(OUTPUT "${common_dialog_stub}"
		COMMAND "${CMAKE_CXX_COMPILER}" -O2 -fPIC -fno-exceptions -fno-rtti
			-c "${CMAKE_SOURCE_DIR}/src/platform/ps5/common_dialog_stub.cpp" -o "${out}/common_dialog_stub.o"
		COMMAND "${sdk}/bin/prospero-lld" --shared -soname libSceCommonDialog.sprx
			-o "${common_dialog_stub}" "${out}/common_dialog_stub.o"
		DEPENDS "${CMAKE_SOURCE_DIR}/src/platform/ps5/common_dialog_stub.cpp" VERBATIM)
	list(APPEND link_stubs "${common_dialog_stub}")

	add_library(playden_ps5_runtime OBJECT
		"${boilerplate}/tooling/native/app_crt.cpp"
		"${gl_source}/native-app/runtime_shims.c"
		"${gl_source}/native-app/app_heap.c")
	target_compile_features(playden_ps5_runtime PRIVATE cxx_std_20)
	target_compile_options(playden_ps5_runtime PRIVATE -O2 -ffunction-sections -fdata-sections
		$<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions;-fno-rtti>)

	add_library(playden_ps5_shims OBJECT src/platform/ps5/title_shims.c src/platform/ps5/file_shims.c)
	target_include_directories(playden_ps5_shims PRIVATE "${gl_sdk}/include")
	target_compile_options(playden_ps5_shims PRIVATE -O2 -Wall -ffunction-sections -fdata-sections)
	set(shim_wraps --wrap=glDrawArrays --wrap=glDrawArraysInstanced --wrap=glDrawArraysInstancedBaseInstance
		--wrap=glDrawElements --wrap=glDrawElementsInstanced --wrap=glBindFramebuffer --wrap=glBlitFramebuffer
		--wrap=glTexImage2D --wrap=glTexSubImage2D --wrap=glClear --wrap=sceSystemServiceHideSplashScreen
        --wrap=openat --wrap=mkdirat)

	add_library(playden_ps5_app OBJECT src/homebrew/ps5.cpp src/homebrew/usb_keyboard.cpp src/platform/ps5/title_runtime.cpp
        src/platform/ps5/native_startup.cpp src/platform/ps5/console_services.cpp src/platform/ps5/system_keyboard.cpp
        "${PLAYDEN_HUI_SOURCE_DIR}/src/platform/ps5/display_egl.cpp"
        "${PLAYDEN_HUI_SOURCE_DIR}/src/platform/ps5/pad.cpp"
        "${PLAYDEN_HUI_SOURCE_DIR}/src/platform/ps5/system.cpp")
    # The runtime host (per-game bottles and titles) is part of the normal app.
    # prospero-lld receives archives explicitly; CMake PUBLIC links do not reach it.
    set(title_database_targets playden_homebrew_runtime_host playden_native_coordinator playden_publication_transaction
        playden_title_database playden_sqlite_reader playden_title_activity
        playden_runtime_settings_publication playden_runtime_settings_publication_journal playden_runtime_publication_backup
        playden_runtime_publication_contract playden_runtime_publication_verifier playden_runtime_package_installer
        playden_runtime_kit_trust playden_runtime_kit_admission playden_runtime_kit_manifest)
    set(title_database_archives)
    foreach(target IN LISTS title_database_targets)
        list(APPEND title_database_archives $<TARGET_FILE:${target}>)
    endforeach()
    option(PLAYDEN_NATIVE_RUNTIME_ACCESS "Require the native runtime client and exact title helper" ON)
    set(prospero_default_source "${CMAKE_SOURCE_DIR}/third_party/prospero-win")
    set(PLAYDEN_PROSPERO_SOURCE_DIR "${prospero_default_source}" CACHE PATH
        "prospero-win source for the native helper client")
    set(native_access_source OFF)
    if(EXISTS "${PLAYDEN_PROSPERO_SOURCE_DIR}/native/pw_lapy_elevation.h")
        file(READ "${PLAYDEN_PROSPERO_SOURCE_DIR}/native/pw_lapy_elevation.h" lapy_header)
        if(PLAYDEN_NATIVE_RUNTIME_ACCESS AND lapy_header MATCHES "pw_lapy_elevation_request_at_detailed")
            set(native_access_source ON)
            target_sources(playden_ps5_app PRIVATE
                "${PLAYDEN_PROSPERO_SOURCE_DIR}/native/pw_lapy_elevation.c"
                "${PLAYDEN_PROSPERO_SOURCE_DIR}/native/ps5log/ps5log_ps5_net.c")
            target_include_directories(playden_ps5_app PRIVATE "${PLAYDEN_PROSPERO_SOURCE_DIR}/native")
        endif()
    endif()
    if(PLAYDEN_NATIVE_RUNTIME_ACCESS AND NOT native_access_source)
        message(FATAL_ERROR "Native runtime access requires the patched prospero-win client; initialize the pinned source or supply PLAYDEN_PROSPERO_SOURCE_DIR. Launcher-only builds must explicitly set PLAYDEN_NATIVE_RUNTIME_ACCESS=OFF.")
    endif()
    target_include_directories(playden_ps5_app PRIVATE "${gl_sdk}/include" "${PLAYDEN_HUI_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/src")
    target_compile_definitions(playden_ps5_app PRIVATE GL_GLEXT_PROTOTYPES=1
        PLAYDEN_PS5_TITLE_ID="${PLAYDEN_PS5_TITLE_ID}"
        PLAYDEN_NATIVE_ACCESS_SOURCE=$<BOOL:${native_access_source}>
        PLAYDEN_UI_SIMULATION=$<BOOL:${PLAYDEN_UI_SIMULATION}>)
    target_compile_options(playden_ps5_app PRIVATE -O2 -Wall -ffunction-sections -fdata-sections
        $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions;-fno-rtti>)
    target_compile_features(playden_ps5_app PRIVATE cxx_std_20)
    target_link_libraries(playden_ps5_app PRIVATE playden_hui playden_library playden_steam_auth
        playden_homebrew_controller playden_local_session_repository playden_steam_library playden_steam_cm playden_native_access playden_runtime_layout_migration playden_local_store playden_local_copy)

	set(host_tool "${host}/ps5-native-tool")
	set(libc_prx "${host}/libc.prx")
	add_custom_command(OUTPUT "${host_tool}" "${libc_prx}"
		COMMAND "${CMAKE_COMMAND}" -DHOST_CXX=/usr/bin/c++ -DNATIVE_DIR=${native} -DHOST_DIR=${host}
			-DEXPECTED_LIBC_SHA256=${expected_libc_sha}
			-P "${CMAKE_SOURCE_DIR}/cmake/PreparePS5HostTools.cmake"
		DEPENDS "${native}/native_app_builder.cpp" "${native}/sce_module_writer.cpp" VERBATIM)

	set(pie "${out}/llvm-pie.elf")
	add_custom_command(OUTPUT "${pie}"
		COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${boilerplate}/tooling/native/ps5-pie.ld"
			"${out}/link/tooling/native/ps5-pie-base.ld"
		COMMAND "${sdk}/bin/prospero-lld" -T "${gl_source}/native-app/ps5-pie.ld" --eh-frame-hdr
			--wrap=malloc --wrap=calloc --wrap=realloc --wrap=free --wrap=posix_memalign --wrap=malloc_usable_size
			--version-script "${gl_source}/native-app/app-symbols.map" -e _start --gc-sections --build-id=sha1
			-L "${out}/stubs" -L "${sdk}/target/lib" -u ps5_agc_gate2_run ${shim_wraps} -o "${pie}"
			$<TARGET_OBJECTS:playden_ps5_runtime> $<TARGET_OBJECTS:playden_ps5_shims>
			$<TARGET_OBJECTS:playden_ps5_app>
			--start-group "${gl_sdk}/lib/libPS5OpenGL.a" "${sdk}/target/lib/libunwind.a"
			"${sdk}/target/lib/libc++abi.a" "${sdk}/target/lib/libc++.a"
			"${sdk_deps}/compiler-rt/libclang_rt.builtins-x86_64.a" $<TARGET_FILE:playden_hui> $<TARGET_FILE:playden_homebrew_ui> $<TARGET_FILE:playden_homebrew_model>
			$<TARGET_FILE:playden_steam_content_language>
			$<TARGET_FILE:playden_homebrew_qr> $<TARGET_FILE:playden_homebrew_controller>
			$<TARGET_FILE:playden_homebrew_artwork_cache> $<TARGET_FILE:playden_homebrew_artwork> $<TARGET_FILE:playden_persistence_controller> $<TARGET_FILE:playden_persistence_service> $<TARGET_FILE:playden_preferences> $<TARGET_FILE:playden_library_controller> $<TARGET_FILE:playden_library_service> $<TARGET_FILE:playden_steam_library> $<TARGET_FILE:playden_steam_cm> $<TARGET_FILE:playden_steam_cm_codec> $<TARGET_FILE:playden_depot_codec>
			$<TARGET_FILE:playden_local_session_repository> $<TARGET_FILE:playden_auth_diagnostics> $<TARGET_FILE:playden_local_store> $<TARGET_FILE:playden_local_copy>
			$<TARGET_FILE:playden_self_update> $<TARGET_FILE:playden_miniz> $<TARGET_FILE:playden_steam_http> $<TARGET_FILE:playden_steam_protocol>
			$<TARGET_FILE:playden_steam_auth>
			$<TARGET_FILE:playden_steam_core> $<TARGET_FILE:playden_diagnostic_log> $<TARGET_FILE:playden_storage>
			$<TARGET_FILE:playden_library> $<TARGET_FILE:playden_native_access> $<TARGET_FILE:playden_managed_directory> $<TARGET_FILE:playden_runtime_steamworks> $<TARGET_FILE:playden_runtime_artifact_hash> $<TARGET_FILE:playden_runtime_settings_storage> $<TARGET_FILE:playden_runtime_private_directories> $<TARGET_FILE:playden_runtime_session_reconciler> $<TARGET_FILE:playden_runtime_launch_service> $<TARGET_FILE:playden_runtime_install_service> $<TARGET_FILE:playden_runtime_installs> $<TARGET_FILE:playden_runtime_layout_migration> $<TARGET_FILE:playden_game_paths>
			${PLAYDEN_STEAM_LINK_ARCHIVES} ${title_database_archives}
			--end-group --as-needed ${link_stubs}
		WORKING_DIRECTORY "${out}/link"
		DEPENDS "${common_dialog_stub}" playden_ps5_runtime playden_ps5_shims playden_ps5_app playden_hui playden_homebrew_ui playden_homebrew_model ${title_database_targets}
			playden_steam_content_language
			playden_homebrew_qr playden_homebrew_controller playden_local_session_repository playden_auth_diagnostics playden_local_store playden_local_copy
			playden_homebrew_artwork_cache playden_homebrew_artwork playden_persistence_controller playden_persistence_service playden_preferences playden_library_controller playden_library_service playden_steam_library playden_steam_cm playden_steam_cm_codec playden_depot_codec
			playden_library playden_self_update playden_miniz playden_steam_auth playden_steam_http playden_steam_protocol playden_steam_core playden_diagnostic_log playden_storage playden_native_access playden_managed_directory playden_runtime_steamworks playden_runtime_artifact_hash playden_runtime_settings_storage playden_runtime_private_directories playden_runtime_session_reconciler playden_runtime_launch_service playden_runtime_install_service playden_runtime_installs playden_runtime_layout_migration playden_game_paths
			$<TARGET_OBJECTS:playden_ps5_runtime> $<TARGET_OBJECTS:playden_ps5_shims>
			$<TARGET_OBJECTS:playden_ps5_app>
			$<TARGET_FILE:playden_self_update> $<TARGET_FILE:playden_miniz> $<TARGET_FILE:playden_steam_http> $<TARGET_FILE:playden_steam_protocol>
			$<TARGET_FILE:playden_steam_auth>
			$<TARGET_FILE:playden_steam_core> $<TARGET_FILE:playden_diagnostic_log> $<TARGET_FILE:playden_storage>
			$<TARGET_FILE:playden_library> $<TARGET_FILE:playden_native_access> $<TARGET_FILE:playden_managed_directory> $<TARGET_FILE:playden_runtime_steamworks> $<TARGET_FILE:playden_runtime_artifact_hash> $<TARGET_FILE:playden_runtime_settings_storage> $<TARGET_FILE:playden_runtime_private_directories> $<TARGET_FILE:playden_runtime_session_reconciler> $<TARGET_FILE:playden_runtime_launch_service> $<TARGET_FILE:playden_runtime_install_service> $<TARGET_FILE:playden_runtime_installs> $<TARGET_FILE:playden_runtime_layout_migration> $<TARGET_FILE:playden_game_paths>
			$<TARGET_FILE:playden_steam_content_language>
			$<TARGET_FILE:playden_homebrew_qr> $<TARGET_FILE:playden_homebrew_controller>
			$<TARGET_FILE:playden_homebrew_artwork_cache> $<TARGET_FILE:playden_homebrew_artwork> $<TARGET_FILE:playden_persistence_controller> $<TARGET_FILE:playden_persistence_service> $<TARGET_FILE:playden_preferences> $<TARGET_FILE:playden_library_controller> $<TARGET_FILE:playden_library_service> $<TARGET_FILE:playden_steam_library> $<TARGET_FILE:playden_steam_cm> $<TARGET_FILE:playden_steam_cm_codec> $<TARGET_FILE:playden_depot_codec>
			$<TARGET_FILE:playden_local_session_repository> $<TARGET_FILE:playden_auth_diagnostics> $<TARGET_FILE:playden_local_store> $<TARGET_FILE:playden_local_copy>
			"${gl_source}/native-app/ps5-pie.ld" "${gl_source}/native-app/app-symbols.map"
			"${gl_sdk}/lib/libPS5OpenGL.a" "${sdk_deps}/compiler-rt/libclang_rt.builtins-x86_64.a"
			${PLAYDEN_STEAM_LINK_ARCHIVES}
		COMMAND_EXPAND_LISTS VERBATIM)

	set(stamp "${app}/.packaged")
	file(GLOB_RECURSE package_inputs CONFIGURE_DEPENDS
		"${CMAKE_SOURCE_DIR}/fixtures/ui-comparison/assets/*"
		"${CMAKE_SOURCE_DIR}/build/homebrew-fonts/*"
		"${CMAKE_SOURCE_DIR}/runtime/*")
	set(simulation_package_commands)
	set(native_helper_package_commands)
	set(PLAYDEN_LAPY_HELPER_ELF "" CACHE FILEPATH "Explicit locally built title-scoped Lapy helper")
	set(PLAYDEN_LAPY_HELPER_MANIFEST "" CACHE FILEPATH "Manifest for the exact Lapy helper")
	if((PLAYDEN_LAPY_HELPER_MANIFEST AND NOT PLAYDEN_LAPY_HELPER_ELF) OR
		(PLAYDEN_LAPY_HELPER_ELF AND NOT PLAYDEN_LAPY_HELPER_MANIFEST) OR
		(PLAYDEN_NATIVE_RUNTIME_ACCESS AND NOT PLAYDEN_LAPY_HELPER_ELF))
		message(FATAL_ERROR "Native runtime packaging requires both the exact local Lapy helper and its manifest; launcher-only builds must explicitly set PLAYDEN_NATIVE_RUNTIME_ACCESS=OFF")
	endif()
	if(PLAYDEN_LAPY_HELPER_ELF)
		if(NOT EXISTS "${PLAYDEN_LAPY_HELPER_MANIFEST}" OR IS_SYMLINK "${PLAYDEN_LAPY_HELPER_ELF}"
			OR IS_DIRECTORY "${PLAYDEN_LAPY_HELPER_ELF}")
			message(FATAL_ERROR "An explicit regular Lapy helper and manifest are required")
		endif()
		string(JSON expected_helper_sha ERROR_VARIABLE helper_pin_error GET "${dependency_lock}"
			prospero_launcher_helper variants "${PLAYDEN_PS5_TITLE_ID}")
		if(helper_pin_error)
			message(FATAL_ERROR "No reviewed Lapy helper pin for ${PLAYDEN_PS5_TITLE_ID}")
		endif()
		file(SHA256 "${PLAYDEN_LAPY_HELPER_ELF}" actual_helper_sha)
		file(READ "${PLAYDEN_LAPY_HELPER_MANIFEST}" helper_manifest)
		string(JSON helper_title GET "${helper_manifest}" target_title)
		string(JSON helper_manifest_sha GET "${helper_manifest}" elf_sha256)
		string(JSON helper_protocol_sha GET "${helper_manifest}" protocol_sha256)
		string(JSON expected_protocol_sha GET "${dependency_lock}" prospero_launcher_helper protocol_sha256)
		if(NOT actual_helper_sha STREQUAL expected_helper_sha OR
			NOT helper_manifest_sha STREQUAL expected_helper_sha OR
			NOT helper_title STREQUAL PLAYDEN_PS5_TITLE_ID OR
			NOT helper_protocol_sha STREQUAL expected_protocol_sha)
			message(FATAL_ERROR "Lapy helper title, artifact or protocol pin mismatch")
		endif()
		set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
			"${PLAYDEN_LAPY_HELPER_ELF}" "${PLAYDEN_LAPY_HELPER_MANIFEST}")
		list(APPEND package_inputs "${PLAYDEN_LAPY_HELPER_ELF}" "${PLAYDEN_LAPY_HELPER_MANIFEST}")
		set(native_helper_package_commands
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${PLAYDEN_LAPY_HELPER_ELF}" "${app}/lapy.elf"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${PLAYDEN_LAPY_HELPER_MANIFEST}" "${app}/lapy-manifest.json")
	endif()
	# The one-shot title remover payload (tools/title-remover), pinned like the Lapy helper.
	set(PLAYDEN_TITLE_REMOVER_ELF "" CACHE FILEPATH "Title remover payload built by scripts/build-title-remover.sh")
	if(PLAYDEN_NATIVE_RUNTIME_ACCESS)
		if(NOT PLAYDEN_TITLE_REMOVER_ELF OR NOT EXISTS "${PLAYDEN_TITLE_REMOVER_ELF}" OR IS_SYMLINK "${PLAYDEN_TITLE_REMOVER_ELF}")
			message(FATAL_ERROR "Native runtime packaging requires the title remover: run scripts/build-title-remover.sh")
		endif()
		string(JSON expected_remover_sha GET "${dependency_lock}" title_remover sha256)
		file(SHA256 "${PLAYDEN_TITLE_REMOVER_ELF}" actual_remover_sha)
		if(NOT actual_remover_sha STREQUAL expected_remover_sha)
			message(FATAL_ERROR "Title remover pin mismatch: ${actual_remover_sha}")
		endif()
		set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PLAYDEN_TITLE_REMOVER_ELF}")
		list(APPEND package_inputs "${PLAYDEN_TITLE_REMOVER_ELF}")
		list(APPEND native_helper_package_commands
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${PLAYDEN_TITLE_REMOVER_ELF}" "${app}/title-remover.elf")
	endif()
	# The self-update's relaunch payload (tools/self-update-relaunch), pinned the same way.
	set(PLAYDEN_RELAUNCH_ELF "" CACHE FILEPATH "Relaunch payload built by scripts/build-self-update-relaunch.sh")
	if(PLAYDEN_NATIVE_RUNTIME_ACCESS)
		if(NOT PLAYDEN_RELAUNCH_ELF OR NOT EXISTS "${PLAYDEN_RELAUNCH_ELF}" OR IS_SYMLINK "${PLAYDEN_RELAUNCH_ELF}")
			message(FATAL_ERROR "Native runtime packaging requires the relaunch payload: run scripts/build-self-update-relaunch.sh")
		endif()
		string(JSON expected_relaunch_sha GET "${dependency_lock}" self_update_relaunch sha256)
		file(SHA256 "${PLAYDEN_RELAUNCH_ELF}" actual_relaunch_sha)
		if(NOT actual_relaunch_sha STREQUAL expected_relaunch_sha)
			message(FATAL_ERROR "Relaunch payload pin mismatch: ${actual_relaunch_sha}")
		endif()
		set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PLAYDEN_RELAUNCH_ELF}")
		list(APPEND package_inputs "${PLAYDEN_RELAUNCH_ELF}")
		list(APPEND native_helper_package_commands
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${PLAYDEN_RELAUNCH_ELF}" "${app}/self-update-relaunch.elf")
	endif()
	set(playden_title_name "Playden")
	if(PLAYDEN_UI_SIMULATION)
		set(playden_title_name "Playden UI Simulation")
		file(GLOB_RECURSE simulation_inputs CONFIGURE_DEPENDS
			"${CMAKE_SOURCE_DIR}/fixtures/ui-comparison/*"
			"${CMAKE_SOURCE_DIR}/fixtures/library.json")
		list(APPEND package_inputs ${simulation_inputs})
		set(simulation_package_commands
			COMMAND "${CMAKE_COMMAND}" -E copy_directory
				"${CMAKE_SOURCE_DIR}/fixtures/ui-comparison" "${app}/fixtures/ui-comparison"
			COMMAND "${CMAKE_COMMAND}" -E copy
				"${CMAKE_SOURCE_DIR}/fixtures/library.json" "${app}/fixtures/library.json")
	endif()
	add_custom_command(OUTPUT "${stamp}"
		COMMAND "${host_tool}" link --in "${pie}" --out "${out}/eboot.elf" --stub-dir "${out}/stubs"
			--module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name eboot.elf
		COMMAND "${CMAKE_COMMAND}" -DAPP_DIR=${app} -DBOILERPLATE=${boilerplate}
			-DASSETS_DIR=${CMAKE_SOURCE_DIR}/fixtures/ui-comparison/assets -DFONTS_DIR=${CMAKE_SOURCE_DIR}/build/homebrew-fonts
			-DPARAM_SOURCE=${gl_source}/native-app/param.json -DPARAM_DEST=${app}/sce_sys/param.json
			-DTITLE_ID=${PLAYDEN_PS5_TITLE_ID} "-DTITLE_NAME=${playden_title_name}" -DAPP_VERSION=${PROJECT_VERSION} -DFULL_VERSION=${PLAYDEN_FULL_VERSION}
			-P "${CMAKE_SOURCE_DIR}/cmake/PackagePS5.cmake"
		${simulation_package_commands}
		COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_SOURCE_DIR}/runtime" "${app}/runtime"
		${native_helper_package_commands}
		COMMAND "${CMAKE_COMMAND}" -E make_directory "${app}/trust"
		COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${PLAYDEN_STEAM_CA_BUNDLE}" "${app}/trust/cacert.pem"
		COMMAND "${host_tool}" self --sign --in "${out}/eboot.elf" --out "${app}/eboot.bin" --magic 0x1D3D154F
		COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${libc_prx}" "${app}/sce_module/libc.prx"
		COMMAND "${CMAKE_COMMAND}" -DAPP_DIR=${app} -DTITLE_ID=${PLAYDEN_PS5_TITLE_ID}
			-P "${CMAKE_SOURCE_DIR}/cmake/WriteNativePackageIdentity.cmake"
		COMMAND "${CMAKE_COMMAND}" -E touch "${stamp}"
		DEPENDS "${pie}" "${host_tool}" "${libc_prx}" "${CMAKE_SOURCE_DIR}/cmake/PackagePS5.cmake"
			"${CMAKE_SOURCE_DIR}/cmake/WriteNativePackageIdentity.cmake"
			"${CMAKE_SOURCE_DIR}/sce_sys/icon0.png" "${CMAKE_SOURCE_DIR}/assets/branding/generic-title-icon.png"
			"${CMAKE_SOURCE_DIR}/sce_sys/pic0.dds" "${CMAKE_SOURCE_DIR}/sce_sys/pic1.dds"
			"${PLAYDEN_STEAM_CA_BUNDLE}" ${package_inputs}
		VERBATIM)
	add_custom_target(playden-homebrew-ps5-package ALL DEPENDS "${stamp}")
endfunction()
