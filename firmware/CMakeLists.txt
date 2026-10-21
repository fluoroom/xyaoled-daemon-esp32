cmake_minimum_required(VERSION 3.22)

# sdkconfig.defaults.local (not in git) can hold private defaults, such as
# CONFIG_XY_WIFI_SSID and CONFIG_XY_WIFI_PASSWORD. After editing either
# defaults file, delete sdkconfig so it is regenerated.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.local")
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.defaults.local")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(xyaoled)
