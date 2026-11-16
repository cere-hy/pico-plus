# 内部堆取证（26.9.13 P0-d）+ 开机黑匣子（26.9.15 B1）。
# 只依赖 heap / log / freertos / esp_timer / usb_serial_jtag：谁都能调，谁调都不会把
# NimBLE 或显示那一套拖进链接闭包。
# Internal-heap evidence (26.9.13 P0-d) + boot blackbox (26.9.15 B1). Depends on heap,
# log, freertos, esp_timer and the USB-Serial-JTAG driver only, so any component can call
# it without dragging NimBLE or the display stack along.
idf_component_register(
    SRCS "pico_heapdiag.c" "pico_bootbb.c"
    INCLUDE_DIRS "include"
    REQUIRES heap log freertos esp_timer esp_driver_usb_serial_jtag
)
