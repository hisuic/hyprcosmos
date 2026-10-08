cmake_minimum_required(VERSION 3.25)
project(hyprcosmos VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
include(CTest)
option(COSMIC_BUILD_PLUGIN "Build the Hyprland 0.56.2 plugin" ON)
option(COSMIC_SANITIZERS "Enable sanitizers for standalone simulation tests" OFF)

add_library(cosmic_physics STATIC plugin/physics.cpp)
set_target_properties(cosmic_physics PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(cosmic_physics PRIVATE -Wall -Wextra -Wpedantic)
target_include_directories(cosmic_physics PUBLIC plugin)

if(COSMIC_BUILD_PLUGIN)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(HYPRLAND REQUIRED IMPORTED_TARGET hyprland=0.56.2)
  pkg_check_modules(DEPS REQUIRED IMPORTED_TARGET hyprutils hyprgraphics aquamarine lua wayland-server glesv2 pixman-1 libdrm)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/cosmic.vert"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/cosmic.frag")
  file(READ shaders/cosmic.vert COSMIC_VERTEX_SHADER)
  file(READ shaders/cosmic.frag COSMIC_FRAGMENT_SHADER)
  configure_file(plugin/shaders.hpp.in generated/shaders.hpp @ONLY)
  add_library(cosmic MODULE plugin/main.cpp plugin/renderer.cpp)
  # Link into an unpublished path. build.sh publishes immutable, content-addressed
  # artifacts so rebuilding cannot truncate a compositor's loaded shared object.
  set_target_properties(cosmic PROPERTIES PREFIX "" OUTPUT_NAME "cosmic"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/link")
  target_include_directories(cosmic PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
  target_compile_options(cosmic PRIVATE -Wall -Wextra -Wno-unused-parameter -fno-gnu-unique)
  target_link_libraries(cosmic PRIVATE cosmic_physics PkgConfig::HYPRLAND PkgConfig::DEPS)
endif()

if(BUILD_TESTING)
  add_executable(physics_test tests/physics_test.cpp)
  if(COSMIC_SANITIZERS)
    # Keep instrumentation confined to the test executable, independently of
    # whether a production compositor plugin is built in the same directory.
    add_library(cosmic_physics_sanitized STATIC plugin/physics.cpp)
    target_include_directories(cosmic_physics_sanitized PUBLIC plugin)
    target_compile_options(cosmic_physics_sanitized PRIVATE -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_libraries(physics_test PRIVATE cosmic_physics_sanitized)
    target_compile_options(physics_test PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(physics_test PRIVATE -fsanitize=address,undefined)
  else()
    target_link_libraries(physics_test PRIVATE cosmic_physics)
  endif()
  add_test(NAME physics COMMAND physics_test)
  find_program(LUA_EXECUTABLE lua)
  if(LUA_EXECUTABLE)
    add_test(NAME lua COMMAND "${LUA_EXECUTABLE}" tests/lua_test.lua)
    set_tests_properties(lua PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  endif()
  add_test(NAME install COMMAND bash tests/install_test.sh)
  set_tests_properties(install PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
endif()
