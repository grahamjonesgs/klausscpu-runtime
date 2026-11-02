zephyr_library()

zephyr_library_sources_ifdef(CONFIG_KLAUSSCPU_FRAMEBUFFER framebuffer.c)
zephyr_library_sources_ifdef(CONFIG_KLAUSSCPU_VNC_SERVER vnc_server.c)
zephyr_library_sources_ifdef(CONFIG_KLAUSSCPU_VNC_DISPLAY display_vnc.c)
zephyr_library_sources_ifdef(CONFIG_KLAUSSCPU_VGA vga_out.c)
# vga_out.c uses the runtime-root headers (mmio.h, vga.h).
if(CONFIG_KLAUSSCPU_VGA)
  zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../..)
endif()
zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR})
