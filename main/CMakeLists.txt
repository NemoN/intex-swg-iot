idf_component_register(SRCS "IntexSWG.cpp" 
                            "TM1650.cpp" 
                            "utils.cpp" 
                            "RestServer.cpp" 
                    INCLUDE_DIRS "."
                    REQUIRES app_update
                             esp32-wifi-manager
                             esp_event
                             spi_flash
                             esp_http_server
                             esp_netif
                             esp_timer
                             esp_wifi
                             nvs_flash
                             esp_driver_gpio
                             esp_driver_uart)

target_add_binary_data(${COMPONENT_TARGET} "WebsiteFiles/indexOTA.html" TEXT)
