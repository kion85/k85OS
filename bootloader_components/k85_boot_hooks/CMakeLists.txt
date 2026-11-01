idf_component_register(SRCS "k85_boot_hooks.c"
                       REQUIRES bootloader_support spi_flash hal esp_rom soc log)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u bootloader_hooks_include")