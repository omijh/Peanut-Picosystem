cmake_minimum_required(VERSION 3.12)

set(PROJECT_NAME peanut_picosystem)

set(PROJECT_SOURCES
    main.cpp
)

include(pico_sdk_import.cmake)

project(${PROJECT_NAME} C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

include(picosystem_sdk_import.cmake)

picosystem_executable(
  ${PROJECT_NAME}
  ${PROJECT_SOURCES}
)

target_link_libraries(${PROJECT_NAME}
  hardware_flash
  hardware_sync
)

disable_startup_logo(${PROJECT_NAME})
no_spritesheet(${PROJECT_NAME})
# Keep default font for bezel and save notifications
