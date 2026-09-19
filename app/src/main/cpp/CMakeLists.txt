# Copyright (C) 2026 Luma Parallax
# SPDX-License-Identifier: GPL-3.0-only

cmake_minimum_required(VERSION 3.22)
project(vsouchi_ipc LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_library(log-lib log)
find_library(android-lib android)

include_directories(
  ${CMAKE_CURRENT_SOURCE_DIR}
  include
)

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

add_library(vsouchi SHARED
    shim/ipc.c
    shim/io.c
    shim/poll.c
    shim/v4l2/ioctl.c
)
target_link_libraries(vsouchi log)
