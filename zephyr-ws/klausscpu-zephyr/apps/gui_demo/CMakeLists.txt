cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(gui_demo)

target_sources(app PRIVATE src/main.c src/gui.c)
target_include_directories(app PRIVATE src)
