# MicroPython core for the `python` command in ssh_shell
# (CONFIG_KLAUSSCPU_PYTHON).  Mirrors ports/zephyr/CMakeLists.txt, with this
# directory's config and HAL and only the port files that are independent of
# its console and drivers.
set(MICROPY_PORT_DIR   ${CMAKE_CURRENT_SOURCE_DIR})
set(MICROPY_TARGET     klausscpu_python)
set(MICROPY_BOARD      KLAUSSCPU_SSH_SHELL)
set(MICROPY_ZEPHYR_DIR ${MICROPY_DIR}/ports/zephyr)

# micropython-lib: the west project at the commit v1.29.0 pins (the
# in-tree lib/micropython-lib submodule is not fetched).
set(MICROPY_LIB_DIR ${ZEPHYR_BASE}/../modules/lib/micropython-lib)

include(${MICROPY_DIR}/py/py.cmake)
include(${MICROPY_DIR}/extmod/extmod.cmake)

# Frozen Python modules (manifest.py: asyncio).  Set before usermod.cmake,
# as ports/zephyr does; mkrules.cmake builds the host's mpy-cross if needed.
set(MICROPY_FROZEN_MANIFEST ${MICROPY_PORT_DIR}/manifest.py)
include(${MICROPY_DIR}/py/usermod.cmake)

# Upstream files with a fix of ours (micropython/patches): build a patched
# copy, leaving the fetched tree untouched.  Fails if the patch no longer
# applies (a new MicroPython: refresh or drop the patch).
set(MP_PATCHED_DIR ${CMAKE_CURRENT_BINARY_DIR}/patched)
file(MAKE_DIRECTORY ${MP_PATCHED_DIR})
foreach(name modsocket zephyr_filesystem)
  set(patch ${CMAKE_CURRENT_SOURCE_DIR}/../patches/${name}.patch)
  configure_file(${MICROPY_ZEPHYR_DIR}/${name}.c ${MP_PATCHED_DIR}/${name}.c COPYONLY)
  execute_process(
    COMMAND patch -s -p1 -i ${patch}
    WORKING_DIRECTORY ${MP_PATCHED_DIR}
    RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${patch} does not apply to ${MICROPY_ZEPHYR_DIR}/${name}.c")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${patch})
endforeach()

set(MICROPY_SOURCE_PORT
  ${MICROPY_PORT_DIR}/python_cmd.c
  ${MP_PATCHED_DIR}/modsocket.c
  ${MICROPY_ZEPHYR_DIR}/modzephyr.c
  ${MP_PATCHED_DIR}/zephyr_filesystem.c
  ${MICROPY_ZEPHYR_DIR}/zephyr_storage.c
)

set(MICROPY_SOURCE_SHARED
  readline/readline.c
  runtime/gchelper_generic.c
  runtime/interrupt_char.c
  runtime/pyexec.c
  runtime/stdout_helpers.c
  runtime/sys_stdio_mphal.c
  timeutils/timeutils.c
)
list(TRANSFORM MICROPY_SOURCE_SHARED PREPEND ${MICROPY_DIR}/shared/)

set(MICROPY_QSTRDEFS_PORT ${MICROPY_PORT_DIR}/qstrdefsport.h)

set(MICROPY_SOURCE_QSTR
  ${MICROPY_SOURCE_PY}
  ${MICROPY_SOURCE_EXTMOD}
  ${MICROPY_SOURCE_USERMOD}
  ${MICROPY_SOURCE_SHARED}
  ${MICROPY_SOURCE_PORT}
)

zephyr_get_include_directories_for_lang(C includes)
zephyr_get_system_include_directories_for_lang(C system_includes)
zephyr_get_compile_definitions_for_lang(C definitions)
zephyr_get_compile_options_for_lang(C options)
set(MICROPY_CPP_FLAGS_EXTRA ${includes} ${system_includes} ${definitions} ${options})

zephyr_library_named(${MICROPY_TARGET})
zephyr_library_include_directories(
  ${MICROPY_INC_CORE}
  ${MICROPY_PORT_DIR}
  ${CMAKE_BINARY_DIR}		# genhdr/ (py/mkrules.cmake)
)
zephyr_library_compile_options(-std=gnu99 -fomit-frame-pointer)
zephyr_library_compile_definitions(
  NDEBUG
  MP_CONFIGFILE=<mpconfigport_ssh.h>
  MICROPY_HEAP_SIZE=${CONFIG_KLAUSSCPU_PYTHON_HEAP_SIZE}
  FFCONF_H=\"${MICROPY_OOFATFS_DIR}/ffconf.h\"
  MICROPY_VFS_FAT=0
  MICROPY_VFS_LFS1=0
  MICROPY_VFS_LFS2=0
)
zephyr_library_sources(${MICROPY_SOURCE_QSTR})
# Interruptible blocking socket calls for the Python thread (socket_wrap.c).
zephyr_library_sources(${MICROPY_PORT_DIR}/socket_wrap.c)
zephyr_ld_options(
  -Wl,--wrap=z_impl_zsock_accept
  -Wl,--wrap=z_impl_zsock_recvfrom
  -Wl,--wrap=z_impl_zsock_sendto
)
zephyr_library_link_libraries(kernel)

include(${MICROPY_DIR}/py/mkrules.cmake)

add_dependencies(BUILD_VERSION_HEADER zephyr_generated_headers)
add_dependencies(${MICROPY_TARGET} zephyr_generated_headers)
