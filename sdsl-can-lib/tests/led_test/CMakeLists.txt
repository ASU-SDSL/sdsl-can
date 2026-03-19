# sdsl-can-lib/tests/led_tests/CMakeLists.txt

cmake_minimum_required(VERSION 4.2)

# 1. Boilerplate to find Zephyr
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(sdsl_led_tests)

# 2. Add the test source file
target_sources(app PRIVATE src/main.c)