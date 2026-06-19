set(srcs "thesis.c"
          "networking.c"
          "comms.c"
          "motion.c"
          )
idf_component_register(SRCS ${srcs}
                    INCLUDE_DIRS "include"
                    REQUIRES "driver esp_timer nvs_flash esp_event esp_wifi lwip"
                    PRIV_REQUIRES "rjrp44__vl53l8cx lsm6dso"
                    )
