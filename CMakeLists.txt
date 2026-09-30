cmake_minimum_required(VERSION 3.18)
project(mod C CXX ASM)

set(CMAKE_CXX_STANDARD 17)

# Dobby клонируется в ./Dobby (делает workflow). Отключаем всё лишнее.
set(DOBBY_GENERATE_SHARED OFF CACHE BOOL "" FORCE)
set(DOBBY_DEBUG OFF CACHE BOOL "" FORCE)
set(Plugin.SymbolResolver OFF CACHE BOOL "" FORCE)
set(Plugin.ImportTableReplace OFF CACHE BOOL "" FORCE)
set(Plugin.Android.BionicLinkerUtil OFF CACHE BOOL "" FORCE)
set(DOBBY_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(DOBBY_BUILD_TEST OFF CACHE BOOL "" FORCE)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-error")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-error")
add_subdirectory(Dobby EXCLUDE_FROM_ALL)

add_library(mod SHARED libmod.cpp)
target_include_directories(mod PRIVATE Dobby/include)

if(TARGET dobby_static)
  target_link_libraries(mod dobby_static log dl)
else()
  target_link_libraries(mod dobby log dl)
endif()
