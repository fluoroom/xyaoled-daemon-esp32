idf_component_register(SRCS "xy_proto.c" "xy_panel.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common
                       PRIV_REQUIRES bt esp_timer)
