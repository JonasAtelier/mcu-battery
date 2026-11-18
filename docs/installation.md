# Installation

[Back to README](../README.md)

Paths mentioned in this guide are relative to the repository root.

The repository is `mcu-battery`. The forge package key, C API and
configuration options use the name `battery`. The clone command below names
the ESP-IDF component directory `battery` as well.

Three step-by-step guides come first: [ESP-IDF](#esp-idf-step-by-step),
[STM32 with CMake](#with-cmake-a-cubemx-cmake-project) and
[STM32 without CMake](#without-cmake-stm32cubeide-a-makefile). None
needs a forge; if you build with esp-forge or stm-forge, see
[With forge](#with-forge). The steps use the MAX17048; for the BQ769x0,
change the chip name. The reference follows the guides.

**Arduino is not supported yet.** TODO: make the library usable from
Arduino (Arduino IDE and PlatformIO).

## ESP-IDF, step by step

**You need** ESP-IDF 5.2 or newer, with `idf.py` working in your terminal, and
an ESP-IDF project: a folder with a `CMakeLists.txt` and a `main/` folder.
`idf.py create-project my-project` makes one.

ESP-IDF calls a library a **component**: a folder with its own
`CMakeLists.txt` that the build finds and compiles. This library is the
component `battery`.

1. **Get the library.** Pick one way.

   - **Component manager**, the simplest: the first build downloads the
     library into `managed_components/battery`. Create
     `main/idf_component.yml`:

     ```yaml
     dependencies:
       battery:
         git: https://github.com/JonasAtelier/mcu-battery.git
         version: stable
     ```

   - **Clone**, if you want to edit the library or build offline. In the
     project folder:

     ```sh
     git clone https://github.com/JonasAtelier/mcu-battery components/battery
     ```

     Keep the folder name `battery`: ESP-IDF names a component after its
     folder.

   ```
   my-project/
   ├── CMakeLists.txt
   ├── sdkconfig.defaults       step 2
   ├── main/
   │   ├── CMakeLists.txt       step 3
   │   ├── idf_component.yml    step 1, component manager
   │   └── my-project.c         step 4
   └── components/
       └── battery/             step 1, clone
   ```

2. **Turn your chip on.** No chip driver is built until you do. Create
   `sdkconfig.defaults` in the project folder, a text file of settings you
   choose, with one line per chip:

   ```ini
   CONFIG_BATTERY_MAX17048=y
   ```

   The other chip is `CONFIG_BATTERY_BQ769X0`. ESP-IDF copies these lines
   into `sdkconfig`, the project's full settings file, only when it creates
   that file: **if `sdkconfig` already exists, delete it** so your line is
   read. `idf.py menuconfig` → **Component config → Battery monitor
   subsystem** does the same from a menu.

3. **Let your code see the library.** A new project's `main/CMakeLists.txt`
   has no `REQUIRES` and already sees every component. If yours has a
   `REQUIRES` list, add `battery` and `driver` to it:

   ```cmake
   idf_component_register(SRCS "main.c" REQUIRES battery driver)
   ```

4. **Write the smallest test.** Copy
   [examples/esp/max17048/main/main.c](../examples/esp/max17048/main/main.c)
   over your main source file and set `PIN_SDA` and `PIN_SCL` to your
   wiring. [API usage](usage.md) explains the calls.

5. **Build.**

   ```sh
   idf.py set-target esp32    # your chip: esp32s3, esp32c3, ...
   idf.py build
   ```

   A good build ends with `Project build complete.` Flash it as any ESP-IDF
   app and open the serial monitor: the driver's first line ends in
   `battery: max17048: version 0x0012, powered up, RCOMP 0x97, empty alert at 10%`;
   the version and RCOMP are what your chip reports.

**If it fails**

| The build prints | Why | Fix |
|---|---|---|
| `Failed to resolve component 'battery' required by component 'main': unknown name.` | The clone is not in a folder named `battery` | Rename it to `components/battery` |
| `fatal error: battery.h: No such file or directory` | `main/CMakeLists.txt` has a `REQUIRES` list without `battery` | Step 3 |
| `battery: CONFIG_BATTERY is on but no chip driver is selected` (a CMake warning) | No chip turned on | Step 2 |
| ``undefined reference to `max17048_init'`` | That chip is not turned on, often because `sdkconfig` existed before you edited `sdkconfig.defaults` | Delete `sdkconfig`, build again |

## STM32, step by step

**You need** a project for an STM32 (an F4 or F7, for example) that uses
ST's **HAL**, the vendor library behind `HAL_Init()`, `HAL_I2C_Init()` and
friends, with the I2C your chip is wired to already set up. STM32CubeMX
makes such a project; with **Toolchain / IDE: CMake** it makes a CMake one,
the main case below.

Two terms. A **HAL module** is the part of the HAL for one peripheral: I2C,
SPI, UART. It is compiled only when it is switched on in **hal_conf**, the
file `Core/Inc/stm32f4xx_hal_conf.h` on an F4 (`stm32f7xx_hal_conf.h` on an
F7, and so on). CubeMX switches a module on when you enable that peripheral.

### With CMake (a CubeMX CMake project)

1. **Get the library.** In the project folder, next to the top-level
   `CMakeLists.txt`:

   ```sh
   git clone https://github.com/JonasAtelier/mcu-battery
   ```

   ```
   my-board/
   ├── CMakeLists.txt                  step 2
   ├── cmake/stm32cubemx/              made by CubeMX
   ├── Core/
   │   ├── Inc/stm32f4xx_hal_conf.h    hal_conf, step 3
   │   └── Src/main.c                  step 4
   ├── Drivers/                        the HAL, copied in by CubeMX
   └── mcu-battery/                    step 1
   ```

2. **Add it to the build.** In the top `CMakeLists.txt`, after the line
   `add_subdirectory(cmake/stm32cubemx)`, add:

   ```cmake
   set(CONFIG_BATTERY_MAX17048 ON)     # your chip; one line per chip
   set(MCU_HAL_TARGET stm32cubemx)
   set(MCU_DEVICE_HEADER stm32f4xx.h)  # your family's: stm32f7xx.h on an F7
   add_subdirectory(mcu-battery)
   ```

   and add `battery` to the link line CubeMX already wrote, so it reads:

   ```cmake
   target_link_libraries(${CMAKE_PROJECT_NAME} stm32cubemx battery)
   ```

   Do not add `PRIVATE`: CubeMX's line has no such keyword, and CMake refuses
   to mix the two forms on one target.

   `stm32cubemx` is a CMake *target*, a named bundle of settings, that CubeMX
   makes to carry the HAL's include folders and defines; `MCU_HAL_TARGET`
   lends those to the library, which works with any CubeMX version.
   `MCU_DEVICE_HEADER` is your chip family's main header.

3. **Check the HAL module.** The MAX17048 talks I2C, so hal_conf must have
   `#define HAL_I2C_MODULE_ENABLED`; enabling I2C1 (or any I2C) in CubeMX
   does that. The BQ769x0 needs the same module; see the
   [HAL module table](#stm32-hal).

4. **Write the smallest test.** It is
   [examples/stm/max17048/main.c](../examples/stm/max17048/main.c) fitted
   into CubeMX's `Core/Src/main.c`, reading through the battery API only.
   Paste each part between the `USER CODE BEGIN` and `END` comments named, so
   a regenerate keeps it. The example's `i2c1_init()` is left out: CubeMX's
   `MX_I2C1_Init()` already does it.

   After `/* USER CODE BEGIN Includes */`:

   ```c
   #include <stdio.h>
   #include <string.h>

   #include "battery.h"
   #include "battery_log.h"
   #include "max17048.h"
   ```

   After `/* USER CODE BEGIN 2 */`, which comes after `MX_I2C1_Init()`:

   ```c
     /* ALRT low at 10% left, as in examples/stm/max17048/main.c. */
     const max17048_config_t cfg = { .empty_alert_pct = 10 };
     max17048_handle_t gauge;
     int err = max17048_init(&hi2c1, &cfg, &gauge);

     if (err) {
       printf("max17048: init failed: %s\n", strerror(-err));
       Error_Handler();
     }
     struct battery_dev *bat = max17048_battery(gauge);
   ```

   After `/* USER CODE BEGIN 3 */`, inside the `while (1)` loop:

   ```c
       struct battery_sample b;

       err = battery_read(bat, &b);
       if (err)
         battery_err(bat, "read failed: %s", strerror(-err));
       else
         battery_log_sample(bat, &b);
       HAL_Delay(5000);
   ```

   The lines go out through `printf()`: send that to a UART by writing
   newlib's `_write()`, and add this to `CMakeLists.txt` so numbers are
   printed:

   ```cmake
   target_link_options(${CMAKE_PROJECT_NAME} PRIVATE -u _printf_float)
   ```

   [API usage](usage.md) explains the calls.

5. **Build** as before, for example `cmake --build` with your build folder.
   A good build shows a `Linking C executable` line and no error. On the
   UART, the driver's first line reads like
   `battery: max17048: version 0x0012, powered up, RCOMP 0x97, empty alert at 10%`.

**If it fails**

| You see | Why | Fix |
|---|---|---|
| `battery: no chip driver is selected; set(CONFIG_BATTERY_<CHIP> ON) before add_subdirectory()` (a CMake warning), then ``undefined reference to `max17048_init'`` | No chip turned on, or turned on after `add_subdirectory(mcu-battery)` | Step 2 |
| `#error "the battery chip drivers need the HAL I2C module: ...` | The HAL module is off | Enable the peripheral in CubeMX and regenerate |
| `#error "battery: CMSIS_device_header is not defined; ...` | `MCU_DEVICE_HEADER` is missing | Step 2 |
| `fatal error: stm32f4xx.h: No such file or directory` | `MCU_HAL_TARGET` is missing | Step 2 |
| `Target "stm32cubmx" not found.` (a CMake error) | `MCU_HAL_TARGET` is misspelt | Step 2 |
| `The plain signature for target_link_libraries has already been used with the target` | `PRIVATE` added to the link line | Step 2 |
| No error, but the numbers print as blanks | The C library (newlib-nano) leaves out `%f` | The `-u _printf_float` line of step 4 |

### Without CMake (STM32CubeIDE, a Makefile)

1. Copy `include/` and `src/` of this repository into your project, for
   example to `Middlewares/mcu-battery/`, keeping the chip folders inside
   `src/`.
2. Add every `.c` file under `src/` to the build, chip folders included.
3. Add two include paths, `Middlewares/mcu-battery/include` and
   `Middlewares/mcu-battery/src`, and two defines:
   `CMSIS_device_header="stm32f4xx.h"`, quotes included, and
   `CONFIG_BATTERY_MAX17048=1`, one per chip. CubeMX already defines
   `USE_HAL_DRIVER` and your device, such as `STM32F405xx`.
4. Steps 3 to 5 of the CMake guide apply as they are; `-u _printf_float`
   goes in the linker flags.

Where these settings usually live: in STM32CubeIDE, **Project → Properties →
C/C++ Build → Settings → Tool Settings → MCU GCC Compiler** (Include paths,
Preprocessor); in a CubeMX Makefile, `C_SOURCES`, `C_INCLUDES` and `C_DEFS`.
The build errors are the ones in the table above; the fix is the matching
define or include path instead of a CMake line. One more:
`#include expects "FILENAME" or <FILENAME>` means `CMSIS_device_header`
lost its quotes.

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

Setup: [ESP-IDF, step by step](#esp-idf-step-by-step). For reference:

- In `main/idf_component.yml`, `version` is a git ref; `stable` is the
  released one.
- A clone in `components/` is a local component, named after its folder;
  that name is what other components `REQUIRES`. The examples in
  `examples/esp/` need no name: their `main` lists no `REQUIRES`, and ESP-IDF
  gives `main` every component, so they build from any checkout name.
- With the subsystem on and no chip picked, the build warns.
- ESP-IDF 5.2 or newer is required for the `i2c_master` API.

## STM32 HAL

Setup: [With CMake](#with-cmake-a-cubemx-cmake-project) or
[Without CMake](#without-cmake-stm32cubeide-a-makefile). The
`add_subdirectory()` route builds a static library `battery`. For reference:

- Each `CONFIG_BATTERY_<CHIP>` (and `CONFIG_BATTERY_DEBUG`) that is true
  reaches the compiler as `CONFIG_BATTERY_<CHIP>=1`, for the library and
  your app.
- `MCU_HAL_TARGET` is a target that carries your `USE_HAL_DRIVER`, device
  define (`STM32F405xx`, ...) and the include dirs of the HAL, CMSIS and your
  `stm32<family>xx_hal_conf.h`. The library copies only those include dirs
  and defines, never the target's sources, so this works with any CubeMX
  version. `MCU_DEVICE_HEADER` becomes `CMSIS_device_header`, which CubeMX
  does not define. Every `mcu-*` library reads these two, so set them once.
  Leave them unset if your project already provides both.
- The CPU flags (`-mcpu`, `-mfloat-abi`, ...) must reach the library too;
  CubeMX's toolchain file sets them for every target.
- Without CMake, `src/` only needs to be on the include path of this
  library's own sources; the defines are what stm-forge passes.

**No chip driver is built by default.** The core is always built and each
chip is opt-in: a chip whose option is absent or 0 compiles to nothing. With
the library on and no chip picked, the CMakeLists warns (ESP-IDF and
`add_subdirectory()`) and `src/battery_port_stm.c` emits a `#warning`.

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
