cmake_minimum_required(VERSION 3.16)

# Pull in the component straight from the repo root. main needs no REQUIRES:
# IDF gives the main component every component in the build.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../..")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(uwb_dw1000)
