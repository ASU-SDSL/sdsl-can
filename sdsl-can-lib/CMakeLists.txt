# sdsl-can-lib/CMakeLists.txt

if(CONFIG_SDSL_CAN)
  # 1. Define the Zephyr library
  zephyr_library()

  # 2. Add your source files (using underscores for files as discussed!)
  zephyr_library_sources(src/can_link.c)

  # 3. Tell Zephyr where the header files are
  zephyr_library_include_directories(include)
endif()