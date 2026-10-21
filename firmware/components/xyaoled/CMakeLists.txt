# Integration-test build: an in-process simulated panel instead of NimBLE.
# Requirements cannot follow the config, so bt is always listed.
if(CONFIG_XY_QEMU)
    set(panel "xy_panel_sim.c")
else()
    set(panel "xy_panel.c")
endif()

idf_component_register(SRCS "xy_proto.c" "xy_upload.c" ${panel}
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common
                       PRIV_REQUIRES bt esp_timer)
