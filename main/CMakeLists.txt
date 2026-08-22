idf_component_register(
    SRCS "k85os_idf.cpp"
         "core/config.cpp"
         "core/theme.cpp"
         "core/input.cpp"
         "core/power.cpp"
         "core/battery.cpp"
         "core/sound.cpp"
         "core/log.cpp"
         "core/rtc_ntp.cpp"
         "core/device.cpp"
         "core/boot_screen.cpp"
         "core/notifications.cpp"
         "core/profiles.cpp"
         "ui/common.cpp"
         "ui/menu.cpp"
         "ui/lock_screen.cpp"
         "ui/quick_settings.cpp"
         "ui/settings_menu.cpp"
         "ui/list_menu.cpp"
         "ui/text_input.cpp"
         "steps/step_counter.cpp"
         "system/system_info.cpp"
         "system/logs_screen.cpp"
         "net/wifi.cpp"
         "apps/store.cpp"
         "apps/tools_menu.cpp"
         "apps/games_menu.cpp"
         "apps/tools/wifi_manager.cpp"
         "apps/tools/i2c_scanner.cpp"
         "apps/tools/gpio_control.cpp"
         "apps/tools/color_test.cpp"
         "apps/tools/calculator.cpp"
         "apps/tools/melodies.cpp"
         "apps/tools/air_mouse.cpp"
         "apps/tools/files.cpp"
         "apps/tools/music_player.cpp"
         "apps/tools/mp3_decode.cpp"
         "apps/tools/mic_test.cpp"
         "apps/tools/wifi_hotspot.cpp"
         "apps/tools/bt_scan.cpp"
         "apps/tools/terminal.cpp"
         "apps/tools/ir_remote.cpp"
         "apps/clock_menu.cpp"
         "core/post_beep.cpp"
         "core/bios_theme.cpp"
         "core/boot_theme.cpp"
         "core/status_bar.cpp"
         "ui/status_bar_settings.cpp"
         "apps/wifi_menu.cpp"
         "core/lock_auth.cpp"
         "ui/lock_screen_settings.cpp"
         "net/app_repo.cpp"
         "apps/wifi_scanner.cpp"
         "apps/apps_menu.cpp"
         "apps/router_menu.cpp"
         "net/firmware_flash.cpp"
         "net/router.cpp"
         "net/ssh_server.cpp"
         "net/ssh_mem_alloc.c"
         "net/ssh_client.cpp"
         "core/shell_commands.cpp"
         "core/heavy_lock.cpp"
         "apps/k85os_menu.cpp"
         "apps/test_mode.cpp"
         "net/ota.cpp"
         "apps/games/snake.cpp"
         "apps/games/tetris.cpp"
         "apps/games/game2048.cpp"
         "apps/games/race.cpp"
         "apps/games/reaction.cpp"
         "apps/games/flappy.cpp"
         "ble_hid/hid_dev.c"
         "ble_hid/hid_device_le_prf.c"
         "ble_hid/esp_hidd_prf_api.c"
         "ble_hid/air_mouse_ble.cpp"
    INCLUDE_DIRS "." "core" "ui" "steps" "system" "net" "apps" "apps/games" "apps/tools" "ble_hid"
    PRIV_REQUIRES json esp_netif esp_driver_tsens esp_wifi nvs_flash esp_event esp_driver_i2c esp_driver_gpio bt esp_driver_rmt esp_https_ota esp_http_client app_update mbedtls esp-libhelix-mp3 mbedtls wolfssl wolfssh esp_psram
)

# Форсируем линкер считать XMALLOC/XFREE/XREALLOC нужными символами с самого
# начала — иначе во втором линковочном проходе wolfssl__wolfssh.a пулит их
# как undefined, а libmain.a (где реализация в ssh_mem_alloc.c) во втором
# проходе уже не сканируется повторно, и связь не разрешается.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,-u,XMALLOC" "-Wl,-u,XFREE" "-Wl,-u,XREALLOC")
set_source_files_properties(
    "ble_hid/hid_dev.c"
    "ble_hid/hid_device_le_prf.c"
    "ble_hid/esp_hidd_prf_api.c"
    PROPERTIES COMPILE_OPTIONS "-Wno-unused-const-variable"
)


































