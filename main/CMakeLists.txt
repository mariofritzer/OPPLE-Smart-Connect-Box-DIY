idf_component_register(SRCS "main.c" "mesh.c" "mqtt_ha.c" "portal.c"
                       INCLUDE_DIRS "."
                       REQUIRES bt nvs_flash esp_wifi esp_netif esp_event esp_http_server mqtt json driver esp_timer)
