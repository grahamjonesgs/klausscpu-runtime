cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(doom)

set(DG ${CMAKE_CURRENT_SOURCE_DIR}/doomgeneric-src/doomgeneric)

if(NOT EXISTS ${DG}/doomgeneric.c)
  message(FATAL_ERROR
    "doomgeneric sources not found. Run apps/doom/fetch-doomgeneric.sh first.")
endif()

# Doom engine sources (the bare-metal "soso" set: no SDL, generic dummy sound),
# minus its platform backend (doomgeneric_soso.c) which we replace with our own.
set(DOOM_SRCS
  am_map doomdef doomstat dstrings d_event d_items d_iwad d_loop d_main d_mode
  d_net f_finale f_wipe g_game hu_lib hu_stuff info i_cdmus i_endoom i_joystick
  i_scale i_sound i_system i_timer memio m_argv m_bbox m_cheat m_config
  m_controls m_fixed m_menu m_misc m_random p_ceilng p_doors p_enemy p_floor
  p_inter p_lights p_map p_maputl p_mobj p_plats p_pspr p_saveg p_setup p_sight
  p_spec p_switch p_telept p_tick p_user r_bsp r_data r_draw r_main r_plane
  r_segs r_sky r_things sha1 sounds statdump st_lib st_stuff s_sound tables
  v_video wi_stuff w_checksum w_file w_main w_wad z_zone w_file_stdc i_input
  i_video doomgeneric dummy)

set(ENGINE_SOURCES "")
foreach(s ${DOOM_SRCS})
  list(APPEND ENGINE_SOURCES ${DG}/${s}.c)
endforeach()

target_sources(app PRIVATE
  ${ENGINE_SOURCES}
  src/doomgeneric_zephyr.c
  src/zephyr_stdio.c
  src/compat.c
)

# compat/ provides the POSIX headers minimal libc lacks (unistd.h, sys/stat.h,
# ...).  Engine sources include them as <...>, so it must precede the system
# include dirs — SYSTEM keeps clang from warning on the stub headers.
target_include_directories(app BEFORE PRIVATE ${DG} compat)

# Render at Doom's native 320x200 (no software 2x upscale).  Quarters the
# render, convert and VNC-send work vs the default 640x400; the VNC client
# scales it up for display.
target_compile_definitions(app PRIVATE DOOMGENERIC_RESX=320 DOOMGENERIC_RESY=200)

# Doom is full of intentional old-C idioms; silence its warnings rather than
# patch vendored sources, and force-include our stdio shim so the file
# functions minimal libc lacks (fopen/fread/...) have correct prototypes.
set_source_files_properties(${ENGINE_SOURCES} PROPERTIES COMPILE_FLAGS
  "-w -include ${CMAKE_CURRENT_SOURCE_DIR}/src/zephyr_stdio.h")
