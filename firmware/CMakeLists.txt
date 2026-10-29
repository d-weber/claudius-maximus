# Claudius Maximus firmware: plain ESP-IDF project (built through PlatformIO, see platformio.ini and tools/dev.py).
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Every component (our code, LVGL, ArduinoJson, the IDF itself) sees the LVGL configuration in include/ and the
# "simple include" switches that the sources use (#include <lvgl.h>, "lv_conf.h"). -O2 for the whole tree is
# CONFIG_COMPILER_OPTIMIZATION_PERF in sdkconfig.defaults.
idf_build_set_property(COMPILE_OPTIONS "-I${CMAKE_CURRENT_LIST_DIR}/include" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "LV_CONF_INCLUDE_SIMPLE" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "LV_LVGL_H_INCLUDE_SIMPLE" APPEND)

project(companion)
