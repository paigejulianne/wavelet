cmake_minimum_required(VERSION 3.15)
project(wavelet C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_VISIBILITY_PRESET hidden)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# --- shared library: wavelet.dll (Windows) / libwavelet.so (Linux) ---
add_library(wavelet SHARED src/wavelet.c)
target_include_directories(wavelet PUBLIC include)
target_compile_definitions(wavelet PRIVATE WV_BUILD_SHARED)
set_target_properties(wavelet PROPERTIES
  VERSION 1.0.0
  SOVERSION 1
  PUBLIC_HEADER include/wavelet.h)

# link libm where required (Linux/macOS)
if(NOT WIN32)
  target_link_libraries(wavelet PRIVATE m)
endif()

# --- static library (optional, handy for the CLI on all platforms) ---
add_library(wavelet_static STATIC src/wavelet.c)
target_include_directories(wavelet_static PUBLIC include)
target_compile_definitions(wavelet_static PRIVATE WV_STATIC)
if(NOT WIN32)
  target_link_libraries(wavelet_static PRIVATE m)
endif()

# --- CLI executable, linked against the shared library ---
add_executable(wavelet_cli src/cli.c)
target_link_libraries(wavelet_cli PRIVATE wavelet)
set_target_properties(wavelet_cli PROPERTIES OUTPUT_NAME wavelet)

# --- Windows GUI (Win32 + GDI) ---
if(WIN32)
  enable_language(RC)
  add_executable(wavelet_gui WIN32
                 gui/main.c gui/image_io.c gui/png.c gui/app.rc)
  target_include_directories(wavelet_gui PRIVATE include gui)
  target_compile_definitions(wavelet_gui PRIVATE _CRT_SECURE_NO_WARNINGS)
  target_link_libraries(wavelet_gui PRIVATE wavelet comctl32 comdlg32 gdi32 user32 shell32)
  # embed the same icon in the CLI executable
  target_sources(wavelet_cli PRIVATE gui/app.rc)
endif()

# --- self-test ---
enable_testing()
add_executable(wavelet_test test/test.c)
target_link_libraries(wavelet_test PRIVATE wavelet_static)
add_test(NAME selftest COMMAND wavelet_test)

install(TARGETS wavelet wavelet_cli
        RUNTIME DESTINATION bin
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib
        PUBLIC_HEADER DESTINATION include)
