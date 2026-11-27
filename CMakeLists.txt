cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# UTC stamp shown as the firmware version and in the Web UI. CI sets
# STADIA_BUILD_TS; a local build uses .stadia-build-ts if present, otherwise
# the current UTC minute.
if(DEFINED ENV{STADIA_BUILD_TS} AND NOT "$ENV{STADIA_BUILD_TS}" STREQUAL "")
    set(PROJECT_VER "$ENV{STADIA_BUILD_TS}")
elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/.stadia-build-ts")
    file(READ "${CMAKE_CURRENT_LIST_DIR}/.stadia-build-ts" _stadia_build_ts)
    string(STRIP "${_stadia_build_ts}" PROJECT_VER)
else()
    string(TIMESTAMP PROJECT_VER "%Y%m%d-%H%M" UTC)
endif()

project(stadia-dongle)
