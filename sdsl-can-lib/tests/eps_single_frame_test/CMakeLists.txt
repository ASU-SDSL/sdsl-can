cmake_minimum_required(VERSION 4.2)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(eps_single_frame_test)

target_sources(app PRIVATE src/main.c)
