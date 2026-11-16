set(read_pico_requires epdiy e0470_epaper_waveform sc7a20h cst836u read_pico_pmu fatfs sy7636a fca9555
                   esp_driver_gpio esp_driver_i2c esp_driver_ledc esp_driver_rmt esp_driver_sdmmc pwm_audio
                   # 26.9.18 S3：挂载阶段打点写进黑匣子。pico_heapdiag 只依赖 heap/log/
                   # freertos/esp_timer/usb_serial_jtag，不会把 NimBLE 或显示拖进链接闭包。
                   # 26.9.18 S3: mount-stage notes go into the blackbox. pico_heapdiag pulls
                   # nothing but heap/log/freertos/esp_timer/usb_serial_jtag.
                   pico_heapdiag)

idf_component_register(
    SRCS "read_pico_init.c" "read_pico_board.c" "read_pico_epd_timing.c" "read_pico_buzzer.c" "buzzer_score.c" "buzzer_duty_map.c" "buzzer_1bit.c" "read_pico_sd.c" "read_pico_flash_hpm.c" "read_pico_crashlog.c"
    INCLUDE_DIRS "include"
    REQUIRES ${read_pico_requires}
    PRIV_REQUIRES bootloader_support spi_flash freertos esp_timer
    # flash_hpm_zb.c 只定义了一个覆盖 IDF weak 符号的表，没有任何
    # 代码引用它，不整包链接的话这个目标文件会被丢掉，覆盖就不生效。
    WHOLE_ARCHIVE
)
