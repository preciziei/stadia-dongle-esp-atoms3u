idf_component_register(
    SRCS "xbox_dev.c" "main.c" "bridge.c" "usb_xbox.c" "ble_central.c"
         "dongle_state.c" "status_led.c" "controller_manager.c"
         "config_store.c" "web_server.c" "captive_portal.c"
         "hid_extra.c" "usb_config_protocol.c"
         "button_actions.c" "board_button.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash bt esp_timer driver esp_driver_rmt esp_wifi esp_netif esp_http_server app_update esp_app_format esp_coex
    EMBED_TXTFILES "index.html"
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE "STADIA_BUILD_STAMP=\"${PROJECT_VER}\"")
