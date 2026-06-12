cmake_minimum_required(VERSION 3.20)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(klausscpu_ssh_shell)

target_sources(app PRIVATE src/main.c)

# Loadable API backend extension (the /api/* route logic).
#
# httpd + httpsd stay in the kernel; only the backend handler is loadable —
# apibackend.c registers it via webapi_register() at svc_start (see webapi.c).
# It is a single-source ELF-object llext (CONFIG_LLEXT_TYPE_ELF_OBJECT allows
# only one source per llext), compiled against THIS app's Zephyr config so its
# ABI matches the kernel that loads it.  The .llext is emitted to the build dir;
# copy it to the SD card (/SD:/apibackend.llext) and load it with `svc load`.
if(CONFIG_LLEXT)
  get_filename_component(apibackend_src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ssh/apibackend.c ABSOLUTE)
  # Override two inherited Zephyr flags for the extension (LLEXT_APPEND_FLAGS is
  # appended after them by add_llext_target, so these win):
  #   -fno-data-sections : the default -fdata-sections puts every global in its
  #       own .bss.<name>, but the loader allows only one SHT_NOBITS section
  #       ("Multiple SHT_NOBITS sections are not supported") — coalesce to one .bss.
  #   -g0                : the default -g -gdwarf-4 emits .debug_* sections WITH
  #       .rela.debug_* relocation sections; the loader then tries to relocate a
  #       non-allocated debug section and fails ("Section N not loaded in any
  #       memory region").  Dropping debug info removes those rela sections.
  set(LLEXT_APPEND_FLAGS -fno-data-sections -g0)
  add_llext_target(apibackend
    OUTPUT  ${CMAKE_BINARY_DIR}/apibackend.llext
    SOURCES ${apibackend_src}
  )
  add_dependencies(app apibackend)

  # Example second backend (claims "/ctrl/") to demonstrate prefix-keyed
  # dispatch — load it alongside apibackend.llext. Same flags apply.
  get_filename_component(ctrlbackend_src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ssh/ctrlbackend.c ABSOLUTE)
  add_llext_target(ctrlbackend
    OUTPUT  ${CMAKE_BINARY_DIR}/ctrl.llext
    SOURCES ${ctrlbackend_src}
  )
  add_dependencies(app ctrlbackend)
endif()
