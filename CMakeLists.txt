# ESP-IDF only. stm-forge never reads this file: it compiles every .c under
# src/, chip folders included, puts src/ on the include path of this
# library's sources and adds include/ -- which is why each port file, and
# every chip driver, is empty when its platform or option is off. Here the
# chip files are listed one by one, and PRIV_INCLUDE_DIRS "src" lets a chip
# folder reach battery_priv.h by plain name.
#
# The core is always built, even with CONFIG_BATTERY off; each chip driver
# only when its Kconfig option is on. Kconfig values are available here
# during the build phase, and only SRCS depends on them -- never REQUIRES,
# which IDF resolves before sdkconfig. That is also why BATTERY off still
# builds the core: an empty SRCS would need a second, differently-shaped
# register call, and an unreferenced core costs nothing once --gc-sections
# has run.
#
# REQUIRES driver, not esp_driver_*: the split components do not exist on
# IDF 5.2, and driver still pulls them in on later versions.
set(srcs "src/battery.c" "src/battery_log.c" "src/battery_port_esp.c")

if(CONFIG_BATTERY AND NOT (CONFIG_BATTERY_MAX17048 OR CONFIG_BATTERY_BQ769X0))
    message(WARNING "battery: CONFIG_BATTERY is on but no chip driver is "
                    "selected; set CONFIG_BATTERY_<CHIP>=y "
                    "(esp-forge: BATTERY_<CHIP> = y)")
endif()

if(CONFIG_BATTERY_MAX17048)
    list(APPEND srcs "src/max17048/max17048.c")
endif()
if(CONFIG_BATTERY_BQ769X0)
    list(APPEND srcs "src/bq769x0/bq769x0.c")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "src"
                       REQUIRES driver)
