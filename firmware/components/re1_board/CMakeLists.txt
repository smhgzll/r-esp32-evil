get_filename_component(RE1_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
# bt / esp_hid / nvs_flash: the Bluetooth LE controller (gamepad_ble.cpp). Not
# conditional on CONFIG_BT_ENABLED - requirements are resolved before the
# configuration is - and with Bluetooth off (QEMU) they build next to nothing.
set(re1_board_requires esp_lcd fatfs sdmmc esp_driver_sdspi esp_driver_spi
                       esp_driver_i2s esp_driver_gpio esp_driver_uart esp_adc
                       esp_driver_usb_serial_jtag esp_timer vfs esp_psram
                       bt esp_hid nvs_flash)
idf_component_register(SRCS "target.cpp" "taskswitch.cpp" "display_st7735.cpp" "storage.cpp"
                            "audio_i2s.cpp" "input.cpp" "remote_uart.cpp" "semihost_vfs.cpp"
                            "newdelete.cpp" "gamepad_ble.cpp"
                       INCLUDE_DIRS "include" ${RE1_ROOT}/port/include
                       REQUIRES ${re1_board_requires})
