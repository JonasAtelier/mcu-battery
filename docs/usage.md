# API usage

[Back to README](../README.md)

Paths mentioned in this guide are relative to the repository root.

## Device lifecycle

Each chip keeps the API its original repo had: an init that allocates a
handle, calls on that handle, and a del that frees it. The handle holds a
`struct battery_dev` and hands it out:

| Chip | Open | Close | As a battery |
|---|---|---|---|
| MAX17048 | `max17048_init(bus, &cfg, &h)` | `max17048_del(h)` | `max17048_battery(h)` |
| BQ769x0 | `bq769x0_init(bus, &cfg, &h)` | `bq769x0_del(h)` | `bq769x0_battery(h)` |

`bus` is, on ESP-IDF, an `i2c_master_bus_handle_t` the app has already
created; the driver adds its device to it. On the STM32 HAL it is an
`I2C_HandleTypeDef *` the app has already `HAL_I2C_Init()`ed; the bus speed
is the handle's. Each init first checks that something ACKs at the chip's
address, before it allocates anything.

The `struct battery_dev *` is valid until the del. `NULL` for a `NULL`
handle.

```c
struct battery_dev *bat = bq769x0_battery(bms);
struct battery_sample s;

if (battery_read(bat, &s) == 0) {
    for (int i = 0; i < s.cells; i++)
        printf("cell %d: %.3f V\n", i + 1, (double)s.cell_v[i]);
    if (s.alerts & BATTERY_ALERT_UNDERVOLTAGE)
        stop_the_load();
}
```

A handle belongs to one task: its calls are not locked against each other.
The bus underneath may be shared with other devices.

## The sample

| Field | Unit | MAX17048 | BQ769x0 |
|---|---|---|---|
| `pack_v` | V | the cell | BAT |
| `soc_percent` | % | SOC | `NAN` |
| `current_a` | A, positive charging | `NAN` | the coulomb counter through `shunt_mohm` |
| `temperature_c` | °C | `NAN` | TS1 (`NAN` if open) |
| `cells`, `cell_v[]` | V | 1, the cell | every connected cell, bottom first |
| `alerts` | `BATTERY_ALERT_*` | see below | see below |

`battery_read()` writes `*s` only on success. The chip's own read gives
more: `max17048_read()` adds the charge rate, `bq769x0_read()` TS2 and TS3
and the raw SYS_STAT.

## Alerts

| Alert | MAX17048 STATUS | BQ769x0 SYS_STAT |
|---|---|---|
| `BATTERY_ALERT_OVERVOLTAGE` | VH | OV |
| `BATTERY_ALERT_UNDERVOLTAGE` | VL | UV |
| `BATTERY_ALERT_OVERCURRENT` | — | OCD |
| `BATTERY_ALERT_SHORT_CIRCUIT` | — | SCD |
| `BATTERY_ALERT_LOW_CHARGE` | HD | — |
| `BATTERY_ALERT_CHIP_FAULT` | — | DEVICE_XREADY |
| `BATTERY_ALERT_CHIP_RESET` | RI | — |
| `BATTERY_ALERT_EXTERNAL` | — | OVRD_ALERT |

They latch in the chip. `battery_clear_alerts(bat, mask)` clears the ones
named: on the BQ769x0 it writes those SYS_STAT bits back (write 1 to clear,
SLUSBK2I Table 8-3); on the MAX17048 it clears those STATUS bits and
CONFIG.ALRT, keeping RCOMP and the threshold beside it. An alert the chip
never raises is nothing to clear: 0, and no transfer. A bit that is no
`BATTERY_ALERT_*` is `-EINVAL`.

The BQ769x0's CC_READY is not an alert; every read consumes it, as the
original did.

## The chip APIs

`max17048.h` and `bq769x0.h` keep every type and call of
`esp-max17048-fuelgauge` and `esp-bq769x0-bms`. On ESP-IDF they return
`esp_err_t`, as they always did; on the STM32 HAL the same calls return 0 or
a negative errno. The battery API returns a negative errno on both.

What only one chip has stays there:

- MAX17048: `max17048_read()` with the charge rate, `max17048_hours_remaining()`,
  `max17048_quick_start()`, `max17048_set_rcomp()`, `max17048_sleep()`,
  `max17048_status()` and `max17048_clear_alert()`.
- BQ769x0: `bq769x0_set_ov_uv()`, `bq769x0_set_protect()`,
  `bq769x0_set_balancing()`, `bq769x0_set_fets()`, `bq769x0_cell_spread()`,
  `bq769x0_clear_faults()`. Read [safety](chips.md#safety) before the FETs.

## Errors

| errno | ESP-IDF `esp_err_t` | means |
|---|---|---|
| `-EINVAL` | `ESP_ERR_INVALID_ARG` | NULL argument, a config out of range, an alert bit that is none |
| `-ENXIO` | `ESP_ERR_NOT_FOUND` | nothing ACKed at the chip's address (every BQ769x0 identity tried) |
| `-ENODEV` | `ESP_ERR_NOT_FOUND` | something answered but is not the chip: a MAX17048 VERSION outside `0x001_`, a BQ769x0 address that keeps no `CC_CFG` |
| `-ETIMEDOUT` | `ESP_ERR_TIMEOUT` | a bus transfer or the presence probe timed out |
| `-EBUSY` | `ESP_ERR_INVALID_STATE` | ESP-IDF: the device NACKed part way through a transfer; IDF 5.x reports that as INVALID_STATE |
| `-EBADMSG` | `ESP_ERR_INVALID_CRC` | BQ769x0 with CRC: a byte came back with a bad CRC |
| `-ENOMEM` | `ESP_ERR_NO_MEM` | out of heap |
| `-EMSGSIZE` | `ESP_ERR_INVALID_SIZE` | a block read past the register file |
| `-EOPNOTSUPP` | `ESP_ERR_NOT_SUPPORTED` | `battery_clear_alerts()` on a chip without the op |
| `-EAGAIN` | — | HAL only: `HAL_BUSY`, the bus or the handle in use |
| `-EIO` | `ESP_FAIL` | any other bus failure; on the HAL a mid-transfer NACK too |

Every esp_err_t in the table makes the round trip: the bus layer turns it
into the errno beside it, and the chip API on ESP-IDF turns that errno back
into the same code, so a caller of the old API sees what IDF reported.
`test/esp/test_port_esp.c` holds the table.

`strerror(-ret)` names any errno. A failed init frees whatever it allocated
and removes any device it added.
