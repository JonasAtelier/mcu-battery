# Read by ESP-IDF (and so esp-forge) as a component, and by any other CMake
# project that add_subdirectory()s this repo, such as a CubeMX-generated STM32
# one. stm-forge never reads it: it compiles every .c under src/, chip folders
# included, puts src/ on the include path of this library's sources and adds
# include/ -- which is why each port file, and every chip driver, is empty
# when its platform or option is off. ESP_PLATFORM tells the two apart; IDF
# sets it in its early requirements pass as well as in the build.

if(NOT ESP_PLATFORM)
    # A static library `battery`. Every .c under src/, as stm-forge does: a
    # chip file compiles to nothing unless its CONFIG_BATTERY_<CHIP> is 1, so
    # there is no source list here to keep in step with the IDF one below.
    file(GLOB_RECURSE battery_sources CONFIGURE_DEPENDS src/*.c)
    add_library(battery STATIC ${battery_sources})
    # include/ for the app; src/ only for this library's own sources, so a
    # chip folder reaches battery_priv.h by plain name and no private header
    # is ever on the app's include path.
    target_include_directories(battery PUBLIC include PRIVATE src)
    # logf() in the BQ769x0 thermistor conversion.
    target_link_libraries(battery PUBLIC m)

    # The options are the Kconfig bools, read from Kconfig so a new chip needs
    # no line here: a true CMake variable CONFIG_BATTERY_<CHIP> (or
    # CONFIG_BATTERY_DEBUG) becomes CONFIG_BATTERY_<CHIP>=1, what stm-forge
    # passes. Only bools: an int option cannot be 1-or-absent. Each `config`
    # line in Kconfig is followed directly by its type line, which is what the
    # match relies on. PUBLIC, because battery_log.h reads
    # CONFIG_BATTERY_DEBUG in the app's sources.
    file(READ Kconfig battery_kconfig)
    string(REGEX MATCHALL "config BATTERY_[A-Z0-9_]+[ \t\r\n]+bool"
           battery_options "${battery_kconfig}")
    list(TRANSFORM battery_options REPLACE
         "config (BATTERY_[A-Z0-9_]+)[ \t\r\n]+bool" "CONFIG_\\1")
    set(battery_chips_on "")
    foreach(option IN LISTS battery_options)
        if(${option})
            target_compile_definitions(battery PUBLIC ${option}=1)
            if(NOT option STREQUAL "CONFIG_BATTERY_DEBUG")
                list(APPEND battery_chips_on ${option})
            endif()
        endif()
    endforeach()
    if(NOT battery_chips_on)
        message(WARNING "battery: no chip driver is selected; "
                        "set(CONFIG_BATTERY_<CHIP> ON) before add_subdirectory()")
    endif()

    # The HAL is the app's. MCU_HAL_TARGET names a target carrying its include
    # dirs and defines (USE_HAL_DRIVER, the device, the hal_conf.h directory)
    # -- stm32cubemx in a CubeMX-generated project; MCU_DEVICE_HEADER is the
    # family header the public headers include as CMSIS_device_header, which
    # CubeMX does not define. Both are shared by every mcu-* library, so an
    # app sets them once. Left unset, the app provides them its own way.
    #
    # Only the target's include dirs and defines are copied, the target is
    # never linked: CubeMX up to 6.13 also hangs main.c, the HAL and the
    # startup file on stm32cubemx as INTERFACE sources, which linking would
    # compile into libbattery.a. A misspelt name fails at generate time.
    if(MCU_HAL_TARGET)
        target_include_directories(battery PRIVATE
            $<TARGET_PROPERTY:${MCU_HAL_TARGET},INTERFACE_INCLUDE_DIRECTORIES>)
        target_compile_definitions(battery PRIVATE
            $<TARGET_PROPERTY:${MCU_HAL_TARGET},INTERFACE_COMPILE_DEFINITIONS>)
    endif()
    if(MCU_DEVICE_HEADER)
        target_compile_definitions(battery PUBLIC
            "CMSIS_device_header=\"${MCU_DEVICE_HEADER}\"")
    endif()
    return()
endif()

# ESP-IDF: the chip files are listed one by one, and PRIV_INCLUDE_DIRS "src"
# lets a chip folder reach battery_priv.h by plain name.
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
