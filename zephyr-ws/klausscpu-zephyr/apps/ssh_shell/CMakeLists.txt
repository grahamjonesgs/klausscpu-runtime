cmake_minimum_required(VERSION 3.20)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(klausscpu_ssh_shell)

target_sources(app PRIVATE src/main.c)

# Loadable HTTP server extension (httpd + webapi + svc shim).
#
# httpd_ext.c is a single-source unity TU (CONFIG_LLEXT_TYPE_ELF_OBJECT allows
# only one source per llext).  It is compiled against THIS app's Zephyr config
# with LL_EXTENSION_BUILD defined, so its ABI matches the kernel that loads it
# and its LOG_* calls route to the exported ext_log().  The .llext is emitted to
# the build dir; copy it to the SD card (e.g. /SD:/httpd.llext) and load it with
# the `svc` shell command.
if(CONFIG_LLEXT)
  get_filename_component(httpd_ext_src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ssh/httpd_ext.c ABSOLUTE)
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
  add_llext_target(httpd_ext
    OUTPUT  ${CMAKE_BINARY_DIR}/httpd.llext
    SOURCES ${httpd_ext_src}
  )
  add_dependencies(app httpd_ext)
endif()
