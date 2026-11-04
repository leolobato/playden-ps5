# SPDX-License-Identifier: GPL-3.0-or-later
#
# Gives the app folder's executables mode 0755, whatever the build tools left. The names are the
# same ones tools/console_title_deploy.py and the self-update (src/update/update_service.cpp) treat
# as executable. The release zip keeps the modes of this folder.
if(NOT DEFINED APP_DIR OR NOT IS_DIRECTORY "${APP_DIR}")
    message(FATAL_ERROR "APP_DIR must be the app folder")
endif()
if(NOT EXISTS "${APP_DIR}/eboot.bin")
    message(FATAL_ERROR "A signed eboot.bin is required")
endif()
file(GLOB_RECURSE executables LIST_DIRECTORIES false
    "${APP_DIR}/eboot.bin" "${APP_DIR}/*.prx" "${APP_DIR}/*.elf")
foreach(executable IN LISTS executables)
    if(IS_SYMLINK "${executable}")
        message(FATAL_ERROR "The package must not hold a symlink: ${executable}")
    endif()
    file(CHMOD "${executable}" PERMISSIONS
        OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
endforeach()
