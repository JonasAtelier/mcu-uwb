# ESP-IDF only. stm-forge never reads this file: it compiles every .c under
# src/, chip folders included, puts src/ on the include path of this
# library's sources and adds include/ -- which is why each port file is empty
# on the other platform. Here the chip files are listed one by one, and
# PRIV_INCLUDE_DIRS "src" lets a chip folder reach uwb_priv.h by plain name.
#
# The core is always built, even with CONFIG_UWB off; each chip driver only
# when its Kconfig option is on. Kconfig values are available here during
# the build phase, and only SRCS depends on them -- never REQUIRES, which IDF
# resolves before sdkconfig. That is also why UWB off still builds the core:
# an empty SRCS would need a second, differently-shaped register call, and an
# unreferenced core costs nothing once --gc-sections has run.
#
# REQUIRES driver, not esp_driver_spi/gpio: the split components do not exist
# on IDF 5.2, and driver still pulls them in on later versions.
set(srcs "src/uwb.c" "src/uwb_log.c" "src/uwb_port_esp.c")

if(CONFIG_UWB AND NOT CONFIG_UWB_DW1000)
    message(WARNING "uwb: CONFIG_UWB is on but no chip driver is selected; "
                    "set CONFIG_UWB_<CHIP>=y (esp-forge: UWB_<CHIP> = y)")
endif()

if(CONFIG_UWB_DW1000)
    list(APPEND srcs "src/dw1000/dw1000.c" "src/dw1000/dw1000_twr.c")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "src"
                       REQUIRES driver
                       PRIV_REQUIRES esp_timer esp_rom)
