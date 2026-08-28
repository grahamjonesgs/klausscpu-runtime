zephyr_library()

zephyr_library_sources_ifdef(CONFIG_KLAUSSCPU_FRAMEBUFFER framebuffer.c)
zephyr_library_sources_ifdef(CONFIG_KLAUSSCPU_VNC_SERVER vnc_server.c)
zephyr_library_sources_ifdef(CONFIG_KLAUSSCPU_VNC_DISPLAY display_vnc.c)
# AMP: core 2 serves VNC; this core boots it and posts frames.  The core-2
# image is the baremetal build's core2_image.h (make core2 in baremetal/).
zephyr_library_sources_ifdef(CONFIG_KLAUSSCPU_AMP_VNC amp_host.c)
if(CONFIG_KLAUSSCPU_AMP_VNC)
  zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../../baremetal)
  zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../..)
endif()
zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR})
