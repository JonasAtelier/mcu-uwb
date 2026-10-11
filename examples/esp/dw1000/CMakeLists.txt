cmake_minimum_required(VERSION 3.16)

# Pull in the component straight from the repo root. A local component is
# named after its directory, so this assumes the repo is checked out as
# uwb -- which is what main/CMakeLists.txt REQUIRES.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../..")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(uwb_dw1000)
