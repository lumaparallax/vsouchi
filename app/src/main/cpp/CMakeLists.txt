# Copyright (C) 2026 Luma Parallax
# SPDX-License-Identifier: GPL-3.0-only

cmake_minimum_required(VERSION 3.22)
project(vsouchi_ipc LANGUAGES C)

set(BUILD_SHIM "none" CACHE STRING "Shim build flavor: none, glibc, or libc")
set_property(CACHE BUILD_SHIM PROPERTY STRINGS "none" "glibc" "libc")

if(NOT BUILD_SHIM MATCHES "^(none|glibc|libc)$")
    message(FATAL_ERROR "Invalid BUILD_SHIM value '${BUILD_SHIM}'. Must be one of: none, glibc, libc")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    include
)

if(BUILD_SHIM STREQUAL "none")
    add_subdirectory(libyuv)
    include_directories(libyuv/include)

    add_library(vsouchi_bridge SHARED
        bridge/ipc.c
        bridge/v4l2/jni.c
        bridge/v4l2/state.c
        bridge/v4l2/cmd.c
        bridge/v4l2/frame.c
    )
    target_link_libraries(vsouchi_bridge log android mediandk yuv)

    add_library(vsouchi_broker SHARED
        broker/main.c
        broker/clerk.c
        broker/v4l2/monitor.c
    )
    target_link_libraries(vsouchi_broker log)

else()
    add_library(vsouchi SHARED
        shim/ipc.c
        shim/io.c
        shim/poll.c
        shim/v4l2/ioctl.c
        shim/fortify.c
    )

    # Shims must not use compiler fortification (prevents recursive wrapper loops)
    target_compile_options(vsouchi PRIVATE -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0)

    if(BUILD_SHIM STREQUAL "glibc")
        set_target_properties(vsouchi PROPERTIES OUTPUT_NAME "vsouchi-glibc")
        target_link_libraries(vsouchi ${CMAKE_DL_LIBS} pthread)
    elseif(BUILD_SHIM STREQUAL "libc")
        target_link_libraries(vsouchi log)
    endif()
endif()
