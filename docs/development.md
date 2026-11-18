# Architecture, driver development and tests

[Back to README](../README.md)

Paths mentioned in this guide are relative to the repository root.

## Repository layout

```
include/                    public API, the only directory apps include
  battery.h                 struct battery_dev, the sample, BATTERY_ALERT_*
  battery_log.h             battery_err/warn/info/dbg, battery_log_sample()
  max17048.h                esp-max17048-fuelgauge's API, max17048_battery()
  bq769x0.h                 esp-bq769x0-bms's API, bq769x0_battery()
src/                        private to the library
  battery.c                 the core: battery_read(), battery_clear_alerts()
  battery_log.c             battery_log_sample()
  battery_priv.h            bus ops, attach and probe, errno maps
  battery_port_esp.c        all of that on ESP-IDF; empty elsewhere
  battery_port_stm.c        all of that on the STM32 HAL; empty under ESP-IDF
  max17048/                 max17048.c  max17048_regs.h  max17048_math.h
  bq769x0/                  bq769x0.c  bq769x0_regs.h  bq769x0_math.h
test/                       host tests, `make -C test test`
  test_max17048_math.c      esp-max17048-fuelgauge's test_math.c
  test_bq769x0_math.c       esp-bq769x0-bms's test_math.c
  test_names.c              every public header in one translation unit
  core_cases.h              core cases, shared by esp/ and stm/
  max17048_cases.h          the fake MAX17048 and its cases, shared
  bq769x0_cases.h           the fake BQ769x0 and its cases, shared
  esp/                      suites on idf_stubs/, through battery_port_esp.c
  stm/                      suites on hal_stubs/, through battery_port_stm.c
  idf_stubs/  hal_stubs/    the slice of ESP-IDF and of the HAL the code calls
examples/
  esp/<chip>/               ESP-IDF projects: max17048, bq769x0
  stm/<chip>/               STM32 snippets: max17048, bq769x0
docs/                       the five guides; the datasheets here are gitignored
assets/                     README and wiring pictures
CMakeLists.txt              ESP-IDF component; elsewhere a static library for add_subdirectory(); stm-forge globs src/**/*.c instead
                            the static library's options are Kconfig's bool entries, found by the type line directly under each `config`
Kconfig                     BATTERY and BATTERY_<CHIP>, for ESP-IDF and both forges' menus
idf_component.yml           ESP-IDF component manifest
LICENSE                     MIT
```

Naming inside `src/<chip>/`, the same in every category library:

| File | Holds |
|---|---|
| `<chip>.c` | the driver: init, the ops, the chip's calls |
| `<chip>_regs.h` | register map, each value with its source citation |
| `<chip>_proto.h` | serial protocol, for parts without registers |
| `<chip>_math.h` | pure conversions, compiled and asserted on the host |
| `<chip>_priv.h` | driver state, only if needed |

Neither chip needs a `_priv.h`: each handle struct lives in its `.c`. The
two chip folders keep the originals' four-space style; the library's own
files use tabs.

**Names.** Every global is `battery_*`, `max17048_*` or `bq769x0_*`, and
every macro `BATTERY_*`, `MAX17048_*` or `BQ769X0_*`, so the copied bus
layer cannot clash with another category library's in one firmware.
dingo_s3 defines `BATTERY_LOW`, `BATTERY_LOW_PERCENT`,
`BATTERY_RECOVERED_PERCENT`, `BATTERY_TASK_*` and `battery_task`; nothing
here uses those names.

## Handles first

The core is two calls on `struct battery_dev`: `battery_read()`, which
zeroes the sample, calls the op and copies it out only on success; and
`battery_clear_alerts()`, which refuses a bit that is no alert,
`-EOPNOTSUPP` without the op, and writes nothing for 0. Everything else is
the chip's own API, kept from its original repo: an init that allocates a
handle, a del that frees it, and the handle's `struct battery_dev` handed
out by `<chip>_battery()`, as mcu-can's `mcp2515_can()` does. There is no
second probe/remove API beside it.

No clock is read and nothing waits: samples carry no timestamp, and the
MAX17048's init sends no reset to wait for (see [chips](chips.md#max17048)).

## Platform ports

The drivers are the same C on both platforms; what differs is in
`src/battery_port_esp.c` and `src/battery_port_stm.c`: the bus ops, attach,
and `battery_i2c_probe()`. The chips' handle APIs
return `esp_err_t` on ESP-IDF and errno on the HAL: each driver works in
errno and converts once, where each public call returns, through
`BATTERY_HANDLE_RET()`.

**Errno maps.** `battery_errno()` turns an `esp_err_t` into an errno at the
bus boundary; `battery_esp_err()` turns it back for the handle APIs. The
pair is a round trip for every code the original drivers could pass up —
`ESP_ERR_INVALID_STATE` is `-EBUSY` and `ESP_ERR_INVALID_CRC` `-EBADMSG`
— which `test/esp/test_port_esp.c` holds.

## Bus abstraction

The bus layer is mcu-expander's: `struct battery_bus_ops` with
`transfer()`, `exchange()`, `receive()` and `detach()`. Every category
library offers I2C, SPI and UART, used or not, and
`test/{esp,stm}/test_port_*` check all three. Both chips here use I2C
`transfer()` alone: a register read is cmd, repeated start, read; a write is
cmd and data in one transaction. The BQ769x0's CRC bytes ride inside the
data, so the bus layer knows nothing of them.

## Adding a chip

1. `include/foo.h`: the chip's API, platform types behind
   `#ifdef ESP_PLATFORM`, and `foo_battery()` handing out the
   `struct battery_dev`.
2. `src/foo/foo.c`: include `battery_priv.h` first, then wrap in
   `#if CONFIG_BATTERY_FOO`.
   - Decide presence with `battery_i2c_probe()` before allocating.
   - Embed a `struct battery_dev`; set `name`, `ops` and `priv` once the
     chip is ready.
   - Give it `read`, filling what the chip has and `NAN` for the rest, and
     `clear_alerts` if it latches any.
3. Cite every register value to its datasheet section or table in
   `src/foo/foo_regs.h`, and hold the conversions in a `test_foo_math.c`.
4. Add `CONFIG_BATTERY_FOO` to:
   - the no-chip checks in `CMakeLists.txt` (its ESP-IDF part; the
     `add_subdirectory()` part reads `Kconfig`) and `src/battery_port_stm.c`;
   - the HAL module `#error` in `src/battery_priv.h`;
   - its sources in `CMakeLists.txt`;
   - `Kconfig`;
   - both forges' defconfigs;
   - `test/Makefile`.

## Tests

```sh
make -C test test
```

Host-only; no toolchain, HAL, ESP-IDF or battery needed. Nothing is checked
against a real chip.

Each change is checked by a test that fails without it: 45 mutations of the
source were run, each on a scratch copy, and all 45 fail the suite — the
errno round trip both ways, the core's zeroing and checks, every alert
mapping and clear, the presence probes, the MAX17048 init (a POR sent, its
NACK ignored, the STATUS read skipped, RI cleared always or never, CONFIG
skipped after an MCU reboot, the VERSION check loosened), datasheet values
(RI, ALRT, the CRATE LSB), the BQ769x0 identity check's two values, their
order and its error choice, the detach after a failed identity, the OV/UV
clamp, the unit conversions into the sample, and the log line's bound.

| Binary | Checks |
|---|---|
| `test_max17048_math` | esp-max17048-fuelgauge's test_math.c: the VCELL scale against both reference drivers, SoC, CRATE sign, RCOMP and ATHD round trips, hours |
| `test_bq769x0_math` | esp-bq769x0-bms's test_math.c: gain assembly, offset sign, cell and pack, CC, thermistor, OV/UV bases and windows, CRC by hand |
| `test_names` | every public header in one ESP-IDF translation unit under `-Werror` |
| `esp/test_core`, `stm/test_core` | `battery_read()`: the zeroed sample, errors passed through, the sample untouched on failure; `battery_clear_alerts()`: refused bits, 0, `-EOPNOTSUPP`; the log prefix and `battery_log_sample()` |
| `esp/test_max17048`, `stm/test_max17048` | the original test_driver.c on both platforms; **no allocation and no transfer when nothing ACKs**; leaks after a stuck VERSION or a failed write; NACKs coming back; **init after a power-up (RI set: CONFIG, RI cleared) and after an MCU reboot (RI clear: SOC, MODE, HIBRT, VALRT, STATUS kept), never a CMD write; a raw POR NACKed as on the part (p14) and init succeeding after it; VERSION outside `0x001_` refused**; a STATUS read failure at init; the `battery_dev`: one cell, SoC, `NAN`s, **each STATUS bit to its alert**, clearing STATUS and ALRT but not RCOMP. ESP-IDF: **dingo_s3's two calls as it makes them**, `scl_hz`, every `esp_err_t` by name, no wait. HAL: no wait, and the log line for both starts |
| `esp/test_bq769x0`, `stm/test_bq769x0` | the original test_driver.c on both platforms: the four identities, calibration, the one-burst read, the '20's skipped registers, the cell wiring tables, TS2/TS3, CC_READY, OV/UV, protection steps and write order, balancing, FETs kept, CRC by hand; **a CRC part found as CRC at either address after an MCU-only reset, with CC_CFG still 0x19, and its writes landing**; **a part that ACKs but keeps no CC_CFG let go of, `-ENODEV`**; a stuck bus as `-ETIMEDOUT`; OV/UV at `INT_MIN`/`INT_MAX` refused, not overflowed; no leaks; the `battery_dev`: volts, amperes, **each SYS_STAT bit to its alert, CC_READY none**, write-1-to-clear. ESP-IDF: every `esp_err_t` by name, the 100 kHz default |
| `esp/test_port_esp`, `stm/test_port_stm` | the bus layer, as in every category library; **the errno round trip** |

All build under `-std=gnu11 -Wall -Wextra -Werror -pedantic` with UBSan, and
`-Wl,--wrap` (GNU ld) to count `calloc()`/`free()` and capture `printf()` on
the HAL side.

**Builds.** Both ESP-IDF examples build for esp32s3 on ESP-IDF v5.5.5 with
no warning. dingo_s3, unchanged, builds for esp32s3 against this library in
place of esp-max17048-fuelgauge with no warning, as it does against the
original. `nm -g --defined-only` over `libbattery.a` finds no global outside
`battery_*` and `max17048_*` in that build, nor outside `battery_*` and
`bq769x0_*` in the bq769x0 example's.
