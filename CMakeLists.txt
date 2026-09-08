cmake_minimum_required(VERSION 3.24)
project(DOS2DLSS VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32 OR NOT MSVC)
  message(FATAL_ERROR "DOS2DLSS currently supports 64-bit Windows builds with MSVC only")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(DOS2DLSSNative SHARED
  src/native/native.cpp
)
target_include_directories(DOS2DLSSNative PRIVATE include)
target_compile_definitions(DOS2DLSSNative PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(DOS2DLSSNative PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(DOS2DLSSNative PRIVATE bcrypt version)
set_target_properties(DOS2DLSSNative PROPERTIES PREFIX "" OUTPUT_NAME "DOS2DLSSNative")

add_library(dos2_dlss_addon SHARED
  src/addon/addon.cpp
)
target_include_directories(dos2_dlss_addon PRIVATE include third_party/reshade)
target_compile_definitions(dos2_dlss_addon PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(dos2_dlss_addon PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(dos2_dlss_addon PRIVATE version)
set_target_properties(dos2_dlss_addon PROPERTIES
  PREFIX ""
  OUTPUT_NAME "dos2-dlss"
  SUFFIX ".addon64"
)

add_executable(dos2dlss_status
  tools/status.cpp
)
target_include_directories(dos2dlss_status PRIVATE include)
target_compile_definitions(dos2dlss_status PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(dos2dlss_status PRIVATE /W4 /permissive- /EHsc)

install(TARGETS DOS2DLSSNative dos2_dlss_addon
  RUNTIME DESTINATION .
  LIBRARY DESTINATION .
)
install(FILES config/dos2-dlss.ini DESTINATION .)
