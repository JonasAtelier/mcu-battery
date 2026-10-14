# Installation

[Back to README](../README.md)

Paths mentioned in this guide are relative to the repository root.

The repository is `mcu-battery`. The forge package key, C API and
configuration options use the name `battery`. The clone command below names
the ESP-IDF component directory `battery` as well.

## With forge

With [esp-forge](https://github.com/JonasAtelier/esp-forge) or
[stm-forge](https://github.com/JonasAtelier/stm-forge). The catalog line in
`profiles/pkgversion` is the same for both:

```make
battery_GIT=https://github.com/JonasAtelier/mcu-battery@stable
```

and a target turns the library and its chips on in `configs/<target>_defconfig`:

```make
PKG_battery         = y
BATTERY_MAX17048    = y
# BATTERY_BQ769X0 is not set
```

or with `make menu`. esp-forge turns each line into
`CONFIG_BATTERY_<CHIP>=y` in `sdkconfig.defaults`; stm-forge passes it as
`-DCONFIG_BATTERY_<CHIP>=1`. `BATTERY_DEBUG = y` turns on `battery_dbg()`
lines on the HAL; on ESP-IDF the log level decides. stm-forge must build the
chip folders in `src/` (stm-forge `ee921c3` or later).

### From max17048 or bq769x0

The old libraries were the forge keys `max17048` and `bq769x0`, one source
file each with no options. The key is `battery` now, and each chip is
opt-in. A target moves over with two defconfig lines and no code change:

```make
# was: PKG_max17048 = y
PKG_battery         = y
BATTERY_MAX17048    = y
```

```make
# was: PKG_bq769x0 = y
PKG_battery         = y
BATTERY_BQ769X0     = y
```

and the catalog lines `max17048_GIT=...` and `bq769x0_GIT=...` in
`profiles/pkgversion` become the `battery_GIT=` line above. `max17048.h`,
`bq769x0.h` and every call in them are still there; what changed in
behaviour is in [chips](chips.md#changes-from-the-original-repos).

## ESP-IDF

Clone into your project's `components/` directory. A local component is
named after its directory, and that name is what you `REQUIRES`:

```sh
git clone https://github.com/JonasAtelier/mcu-battery components/battery
```

```cmake
idf_component_register(SRCS "main.c" REQUIRES battery driver)
```

Enable the chips under `idf.py menuconfig` → **Component config → Battery
monitor subsystem**, or put `CONFIG_BATTERY_<CHIP>=y` in your
`sdkconfig.defaults` as the examples in `examples/esp/` do. With the
subsystem on and no chip picked, the build warns. ESP-IDF 5.2 or newer is
required for the `i2c_master` API.

## STM32 HAL

Copy `include/` and `src/` into an STM32 project, compile every `.c` under
`src/`, chip folders included, add `include/` and `src/` to the include path
(`src/` only needs to reach this library's own sources), and define what the
forge would: `CMSIS_device_header` as your family header (`"stm32f4xx.h"`,
`"stm32f7xx.h"`, ...), `USE_HAL_DRIVER`, and `CONFIG_BATTERY_<CHIP>=1` for
each chip you use.

**No chip driver is built by default.** The core is always built and each
chip is opt-in: a chip whose option is absent or 0 compiles to nothing. With
the library on and no chip picked, the ESP-IDF CMakeLists warns and
`src/battery_port_stm.c` emits a `#warning`.

**The HAL module is yours to enable.** Both drivers call the HAL's I2C, so
your app's `stm32<family>xx_hal_conf.h` must define it, and its `.c` must be
in the build:

| Option | Needs in hal_conf | HAL source |
|---|---|---|
| `BATTERY_MAX17048`, `BATTERY_BQ769X0` | `HAL_I2C_MODULE_ENABLED` | `stm32<family>xx_hal_i2c.c` |

A chip option on with the module off stops the build with an `#error` that
says which define is missing. The bus layer's SPI and UART code sits under
`HAL_SPI_MODULE_ENABLED` and `HAL_UART_MODULE_ENABLED`, so an app without
those modules builds as it is.

**Heap.** Each chip handle is one `calloc()`, as in the original drivers.

**printf() is the log.** The `battery_*` log macros and
`battery_log_sample()` go out through `printf()`, so retarget newlib's
`_write()` to a UART. stm-forge links newlib-nano, whose `printf()` drops
`%f` unless the app links with `-u _printf_float`.
