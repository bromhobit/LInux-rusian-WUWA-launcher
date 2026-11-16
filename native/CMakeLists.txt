cmake_minimum_required(VERSION 3.20)
project(echo_native VERSION 0.3.5 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 6.2 REQUIRED COMPONENTS Widgets Network)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
find_path(XXHASH_INCLUDE_DIR xxhash.h REQUIRED)
find_library(XXHASH_LIBRARY xxhash REQUIRED)
find_package(SQLite3 REQUIRED)

add_executable(echo-native src/main.cpp src/auto_install.cpp src/auto_install.h src/controller.cpp src/controller.h src/steam_options.cpp src/steam_options.h src/steam_config.cpp src/steam_config.h src/fps_unlock.cpp src/fps_unlock.h)
target_include_directories(echo-native PRIVATE ${XXHASH_INCLUDE_DIR})
target_link_libraries(echo-native PRIVATE Qt6::Widgets Qt6::Network PkgConfig::SDL3 ${XXHASH_LIBRARY} SQLite::SQLite3)
target_compile_options(echo-native PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(echo-native PRIVATE ECHO_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/..")
install(TARGETS echo-native RUNTIME DESTINATION bin)

option(ECHO_BUILD_FPS_TESTS "Build the FPS settings probe" OFF)
if(ECHO_BUILD_FPS_TESTS)
  add_executable(echo-fps-probe ../tests/fps_probe.cpp src/fps_unlock.cpp)
  target_include_directories(echo-fps-probe PRIVATE src)
  target_link_libraries(echo-fps-probe PRIVATE Qt6::Core SQLite::SQLite3)
endif()



