cmake_minimum_required(VERSION 3.22)
project(albw_android C)

# SDL3 as a shared library (libSDL3.so), loaded by SDLActivity before libmain.so
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
add_subdirectory(${SDL_DIR} ${CMAKE_BINARY_DIR}/SDL)

# Step 1: smoke test (screen, gamepad, GL info, memory map). Replaced by the game in step 4.
add_library(main SHARED smoke.c)
target_link_libraries(main PRIVATE SDL3::SDL3 GLESv3 log)
