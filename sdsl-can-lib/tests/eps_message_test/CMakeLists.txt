cmake_minimum_required(VERSION 4.2)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(eps)

list(APPEND CMAKE_MODULE_PATH ${ZEPHYR_BASE}/modules/nanopb)
include(nanopb)

target_include_directories(app PRIVATE ../../include)

zephyr_nanopb_sources(app ../../proto/link.proto)
target_sources(app PRIVATE
	src/main.c
	../../src/can_link.c
)