# The application: everything under src/ (PlatformIO builds this directory as the project's main component).
# CONFIGURE_DEPENDS makes a build pick up new files without a manual reconfigure; the IDF also evaluates this file
# in CMake script mode (requirement scan), where that keyword is not allowed.
if(NOT CMAKE_SCRIPT_MODE_FILE)
  set(APP_GLOB_FLAGS CONFIGURE_DEPENDS)
endif()
file(GLOB_RECURSE APP_SRCS ${APP_GLOB_FLAGS} "${CMAKE_CURRENT_LIST_DIR}/*.cpp" "${CMAKE_CURRENT_LIST_DIR}/*.c")
idf_component_register(SRCS ${APP_SRCS}
                       INCLUDE_DIRS "." "../include"
                       PRIV_REQUIRES esp_lcd esp_driver_i2c esp_driver_ledc esp_driver_usb_serial_jtag esp_timer
                                     nvs_flash esp_driver_tsens esp_mm esp_psram)
# (PlatformIO reuses this component's flags for every IDF source it compiles, so C++-only warning switches cannot
# go here; the two files that trip gnu++2b deprecation notes carry a #pragma instead.)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
