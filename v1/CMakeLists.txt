cmake_minimum_required(VERSION 3.16)
project(physics_puzzle_platformer CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Strict floating point. -ffast-math would let the compiler reassociate the
# constraint solve, and -ffp-contract would fuse multiply-adds differently
# between builds; either silently invalidates every recorded replay
# (design doc 6.3). These flags are part of the physics contract.
add_compile_options(-ffp-contract=off -fno-fast-math -Wall -Wextra)

add_library(ppp_solver STATIC solver/src/solver.cpp)
target_include_directories(ppp_solver PUBLIC solver/include)

# Both libraries are linked into the GDExtension, which is a shared object,
# so their objects have to be position-independent. Without this the
# extension fails to link with "relocation R_X86_64_PC32 ... can not be used
# when making a shared object" — and it fails at the very end of a long
# build, in the one target a headless CI run never reaches.

add_library(ppp_game STATIC
  game/src/level.cpp
  game/src/player.cpp
  game/src/sim.cpp
  game/src/replay.cpp
  game/src/script.cpp
  game/src/json.cpp)
target_include_directories(ppp_game PUBLIC game/include)
target_link_libraries(ppp_game PUBLIC ppp_solver)

add_executable(ppp_tests
  solver/tests/main.cpp
  solver/tests/test_solver.cpp
  solver/tests/test_game.cpp
  solver/tests/test_determinism.cpp)
target_link_libraries(ppp_tests PRIVATE ppp_game)
target_include_directories(ppp_tests PRIVATE solver/tests)

add_executable(pppsim tools/pppsim.cpp tools/render_svg.cpp)
target_link_libraries(pppsim PRIVATE ppp_game)
target_include_directories(pppsim PRIVATE tools)

set_property(TARGET ppp_solver ppp_game PROPERTY POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------- GDExtension
#
# Optional: only built when godot-cpp is present. The solver, the game layer
# and every test build without Godot installed, which is the point of doc
# 12's layering — CI runs the replay suite on a machine with no engine.
#
#   cmake -S . -B build -DGODOT_CPP_DIR=/path/to/godot-cpp
#
# godot-cpp must already be built for the matching target (its own SCons or
# CMake build produces bin/libgodot-cpp.<platform>.<target>.<arch>.a).
set(GODOT_CPP_DIR "" CACHE PATH "Path to a built godot-cpp checkout")
if(GODOT_CPP_DIR AND EXISTS "${GODOT_CPP_DIR}/include")
  file(GLOB GODOT_CPP_LIB "${GODOT_CPP_DIR}/bin/libgodot-cpp.*.a")
  if(GODOT_CPP_LIB)
    list(GET GODOT_CPP_LIB 0 GODOT_CPP_LIB_FIRST)
    add_library(ppp_gdextension SHARED godot/src/ppp_gdextension.cpp)
    target_include_directories(ppp_gdextension PRIVATE
      "${GODOT_CPP_DIR}/include"
      "${GODOT_CPP_DIR}/gen/include"
      "${GODOT_CPP_DIR}/gdextension")
    target_link_libraries(ppp_gdextension PRIVATE ppp_game "${GODOT_CPP_LIB_FIRST}")
    set_target_properties(ppp_gdextension PROPERTIES
      OUTPUT_NAME "ppp"
      LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/godot/bin")
    message(STATUS "GDExtension enabled against ${GODOT_CPP_LIB_FIRST}")
  else()
    message(WARNING "GODOT_CPP_DIR set but no built library in ${GODOT_CPP_DIR}/bin")
  endif()
endif()
