cmake_minimum_required(VERSION 3.18)
project(mod CXX)

set(CMAKE_CXX_STANDARD 17)

# Dobby клонируется в ./Dobby (делает workflow)
set(DOBBY_GENERATE_SHARED OFF CACHE BOOL "" FORCE)
add_subdirectory(Dobby)

add_library(mod SHARED libmod.cpp)
target_include_directories(mod PRIVATE Dobby/include)

if(TARGET dobby_static)
  target_link_libraries(mod dobby_static log dl)
else()
  target_link_libraries(mod dobby log dl)
endif()
