cmake_minimum_required(VERSION 3.25)
project(linrtx-obs VERSION 1.0.0 LANGUAGES C CXX CUDA)

option(ENABLE_VSR "Enable RTX Video Super Resolution filter" ON)
option(ENABLE_TRUEHDR "Enable RTX TrueHDR filter" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 17)
set(CMAKE_CUDA_STANDARD 17)
set(CMAKE_CUDA_ARCHITECTURES "89;80;75")
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(OpenGL REQUIRED)

find_package(libobs QUIET)
if(NOT libobs_FOUND)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBOBS REQUIRED libobs)
endif()

find_path(CUDA_INCLUDE_DIR cuda_runtime.h
    PATHS
    /opt/cuda/include
    /usr/local/cuda/include
    /usr/include
)

find_library(CUDA_CUDART_LIBRARY
    NAMES cudart_static cudart
    PATHS
    /opt/cuda/targets/x86_64-linux/lib
    /opt/cuda/lib64
    /usr/local/cuda/lib64
    /usr/lib
)

set(SOURCES
    src/plugin-main.cpp
    src/nvvfx-loader.cpp
    src/cuda-gl-interop.cpp
)

set(HEADERS
    src/nvvfx-interface.h
    src/nvvfx-loader.h
    src/cuda-gl-interop.h
)

if(ENABLE_VSR)
    list(APPEND SOURCES src/filter-vsr.cpp)
    add_compile_definitions(ENABLE_VSR=1)
endif()

if(ENABLE_TRUEHDR)
    list(APPEND SOURCES src/filter-truehdr.cpp src/kernels/format_convert.cu)
    list(APPEND HEADERS src/kernels/format_convert.h)
    add_compile_definitions(ENABLE_TRUEHDR=1)
endif()

add_library(linrtx-obs MODULE ${SOURCES} ${HEADERS})

target_include_directories(linrtx-obs PRIVATE
    src
    ${CUDA_INCLUDE_DIR}
    ${OPENGL_INCLUDE_DIR}
)

if(TARGET OBS::libobs)
    target_link_libraries(linrtx-obs PRIVATE OBS::libobs)
else()
    target_include_directories(linrtx-obs PRIVATE ${LIBOBS_INCLUDE_DIRS} ${LIBOBS_INCLUDE_DIRS}/obs /usr/include/obs)
    target_link_libraries(linrtx-obs PRIVATE ${LIBOBS_LIBRARIES})
endif()

target_link_libraries(linrtx-obs PRIVATE
    ${CUDA_CUDART_LIBRARY}
    ${OPENGL_LIBRARIES}
    ${CMAKE_DL_LIBS}
)

set_target_properties(linrtx-obs PROPERTIES
    PREFIX ""
    OUTPUT_NAME "linrtx-obs"
)

# User-level installation helper
set(USER_PLUGIN_DIR "$ENV{HOME}/.config/obs-studio/plugins/linrtx-obs")

add_custom_target(install-user
    COMMAND ${CMAKE_COMMAND} -E make_directory "${USER_PLUGIN_DIR}/bin/64bit"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${USER_PLUGIN_DIR}/data"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:linrtx-obs>" "${USER_PLUGIN_DIR}/bin/64bit/"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/data" "${USER_PLUGIN_DIR}/data"
    DEPENDS linrtx-obs
    COMMENT "Installing LinRTX-OBS to user plugins directory: ${USER_PLUGIN_DIR}"
)

# Standard CMake installation (for PKGBUILD / system install)
include(GNUInstallDirs)
install(TARGETS linrtx-obs
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/obs-plugins"
)
install(DIRECTORY data/
    DESTINATION "${CMAKE_INSTALL_DATADIR}/obs/obs-plugins/linrtx-obs"
)
