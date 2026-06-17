zephyr_library()

zephyr_library_sources(vnc_server.c framebuffer.c)
zephyr_library_sources_ifdef(CONFIG_KLAUSSCPU_VNC_DISPLAY display_vnc.c)
zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR})
