zephyr_library()

zephyr_library_sources(vnc_server.c framebuffer.c)
zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR})
