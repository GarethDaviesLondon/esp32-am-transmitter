idf_component_register(
    SRCS
        "main.c"
        "app.c"
        "button.c"
        "audio/decoder.c"
        "audio/dsp.c"
        "cli/cli_reply.c"
        "cli/cmd_net.c"
        "cli/cmd_radio.c"
        "cli/cmd_system.c"
        "cli/console.c"
        "net/discover.c"
        "net/stream.c"
        "net/webui.c"
        "net/wifi_conn.c"
        "rf/carrier.c"
        "state_doc.c"
        "store/settings.c"
        "store/settings_rf.c"
        "store/settings_stations.c"
        "store/settings_sys.c"
        "store/settings_wifi.c"
    INCLUDE_DIRS
        "."
        "audio"
    REQUIRES
        console
        driver
        esp_http_client
        esp-tls
        mbedtls
        esp_http_server
        esp_wifi
        esp_netif
        esp_event
        esp_timer
        nvs_flash
        json
        mdns
        app_update
)

# dsp.c is also built on the host by test/host/. Keep it free of ESP-IDF
# headers so that stays true.

# The house C style is enforced by reading, not tooling (TD-01). These are the
# warnings that catch what reading misses, and they are errors nowhere: a
# third-party component should never fail this project's build.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wextra
    -Wshadow
    -Wno-unused-parameter
)
