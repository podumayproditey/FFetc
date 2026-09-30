cmake_minimum_required(VERSION 3.18)
project(mod CXX)
set(CMAKE_CXX_STANDARD 17)

# ShadowHook распаковывается workflow в ./sh (заголовки и libshadowhook.so)
add_library(shadowhook SHARED IMPORTED)
set_target_properties(shadowhook PROPERTIES
  IMPORTED_LOCATION ${CMAKE_SOURCE_DIR}/sh/libshadowhook.so
  INTERFACE_INCLUDE_DIRECTORIES ${CMAKE_SOURCE_DIR}/sh/include)

add_library(mod SHARED libmod.cpp)
target_link_libraries(mod shadowhook log dl)
