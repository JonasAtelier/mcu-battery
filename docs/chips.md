# Chip reference

[Back to README](../README.md)

Paths mentioned in this guide are relative to the repository root.

## Where the line sits

Two libraries measure batteries, and the chip decides which one it is in:

| Library | The chip | Examples |
|---|---|---|
| **mcu-battery** | knows it is on a battery: it models the cell's charge (a fuel gauge), or measures cells one by one and protects them (a monitor or AFE) | MAX17048, BQ76920/30/40 |
| **mcu-powermon** | measures a bus voltage and the current through a shunt, whatever is on the other side | INA219, INA226, INA228, INA237 |

An INA226 across a pack is still a power monitor: it has no notion of a
cell, a state of charge or a protection limit, so it stays in mcu-powermon.
A coulomb-counting gauge with a state of charge (a BQ27441, a MAX17055)
would come here. An app with both links both: no name is shared.

## MAX17048

One driver, `src/max17048/`, built under `BATTERY_MAX17048`. State of
charge for **one lithium cell**, with no sense resistor and no coulomb
counting: it runs Maxim's ModelGauge on the cell voltage and learns as it
goes. The MAX17049 is the 2-cell part and is not supported.

| Function | Does |
|---|---|
| `max17048_init(bus, &cfg, &h)` | Probe 0x36, check VERSION, set RCOMP and the empty-alert threshold, clear RI; no reset (see below) |
| `max17048_read(h, &d)` | Voltage, SoC, charge rate and the ALRT flag |
| `max17048_hours_remaining(h, &f)` | Hours to empty, or to full if charging |
| `max17048_status` / `max17048_clear_alert` | STATUS, and clearing CONFIG.ALRT |
| `max17048_quick_start(h)` | Force a re-estimate — see below |
| `max17048_set_rcomp(h, v)` | Tune the model to chemistry and temperature |
| `max17048_sleep(h, bool)` | Under 1 µA; the gauge stops modelling |
| `max17048_battery(h)` | The handle as a `struct battery_dev` |

**It takes time to converge.** Right after a battery is connected the
estimate can be several percent out; it settles over minutes.
**`max17048_quick_start()` is a trap on a loaded pack**: it re-estimates from
the terminal voltage, which under load is depressed by the cell's internal
resistance. Use it only on a resting pack, and usually not at all.

**Two registers read backwards.** The empty-alert threshold `ATHD` in
`CONFIG[4:0]` stores `32 - percent`; the driver takes and returns a
percentage. `CRATE` is signed: positive charging. It needs a few minutes of
steady load to settle, and `max17048_hours_remaining()` reports 0 at rest
rather than dividing by a near-zero rate.

**The identity check.** `VERSION` reads `0x001_`, the low nibble a
production version (p10 Table 2). Init takes any `0x0010` to `0x001F` and
refuses the rest, a stuck bus included: `-ENODEV`, `ESP_ERR_NOT_FOUND` on
ESP-IDF. A clone, or a future revision outside `0x001_`, is refused too.

**Init sends no power-on reset.** The datasheet speaks against it twice:
"The IC does not respond with an I2C ACK after this command sequence" (p14,
CMD Register), and "POR includes a quick-start, so only use it when the
battery is fully relaxed" (p9, Power-On Reset). Init reads STATUS.RI
instead: "RI (reset indicator) is set when the device powers up. Any time
this bit is set, the IC is not configured, so the model should be loaded and
the bit should be cleared" (p14).

| At init | The part | Init |
|---|---|---|
| RI = 1 | just powered up, every register at its default (p10-11 Table 2) | writes CONFIG, clears RI |
| RI = 0 | kept gauging while only the MCU restarted | writes CONFIG, which clears CONFIG.ALRT: an ALRT pin latched before the reboot is released, while the STATUS cause bits stay; SOC, MODE, HIBRT, VALRT and STATUS untouched |

So the first `battery_read()` after a power-up does not report
`BATTERY_ALERT_CHIP_RESET`, and a later one means the part powered up
again (RI, p14). A battery swap shows differently: VCELL dropping below
VRESET makes the part reset and quick-start when it returns (p9, Battery
Swap Detection; p13, VRESET/ID Register), which STATUS reports as VR only
with STATUS.EnVr set (p14); the driver does not set EnVr or map VR.

SOC after a power-up: OCV is ready 17 ms after insertion and SOC 175 ms
after that, about 192 ms (p8, Battery Insertion Debounce); the datasheet
also gives "the first update is available approximately 1s after POR"
(p10, SOC Register). Read it a second or more after power-up.

**Voltage alerts need VALRT.** VH and VL are only set outside the window in
VALRT, whose default 0x00FF (p11 Table 2), 20 mV per LSB (p13), is 0 to
5.1 V: no voltage alert until the app writes VALRT with
`max17048_reg_write()`. HD, the SoC alert, is on at 4% by default.

### Provenance

Maxim's datasheet, 19-6171 Rev 7 (11/16), in `docs/max17048.pdf`
(gitignored); pN is its printed page. Register addresses and defaults
against the register summary (p10-11 Table 2), bits against each register's
figure (MODE p11 Figure 8, CONFIG p12 Figure 10, STATUS p14 Figure 13), the
LSBs against Table 2, the I2C address and framing against p16-17, the
STATUS and ALRT clearing against p12 and p14. CRATE's sign is the one value
the datasheet leaves open; the two reference drivers the original was
checked against, Linux's `max17040_battery.c` and Zephyr's
`drivers/fuel_gauge/max17048/`, read it two's complement, positive
charging.

## BQ76920 / BQ76930 / BQ76940

One driver, `src/bq769x0/`, built under `BATTERY_BQ769X0`, serves all three:
the part is told apart by its cell count and `cell_channels`, which no
register reports. These are analog front ends, not gauges: they measure
every cell off the balance lead, count coulombs through a shunt, and carry
hardware protection that can cut the pack with no help from software. Section
and table numbers below are TI's datasheet, SLUSBK2I rev I (March 2022), in
`docs/bq769x0.pdf` (gitignored).

| Function | Does |
|---|---|
| `bq769x0_init(bus, &cfg, &h)` | Find the identity, read the factory calibration, ADC and coulomb counter on |
| `bq769x0_read(h, &d)` | Every cell, pack, current, TS1..TS3, SYS_STAT, in one burst |
| `bq769x0_cell_spread(h, ...)` | Lowest, highest and the spread between them |
| `bq769x0_status` / `bq769x0_clear_faults` | SYS_STAT, write 1 to clear |
| `bq769x0_set_ov_uv` / `bq769x0_get_ov_uv` | Overvoltage and undervoltage trip, per cell |
| `bq769x0_set_protect` / `bq769x0_get_protect` | SCD, OCD and their delays, OV and UV delays |
| `bq769x0_set_balancing(h, mask)` | Bleed resistors, by cell |
| `bq769x0_set_fets(h, chg, dsg)` | The protection FETs |
| `bq769x0_battery(h)` | The handle as a `struct battery_dev` |

**A gauge sees the pack; this sees each cell.** On a series lithium pack
the failure mode is one cell sagging while the others look fine. The pack
voltage barely moves; the cell spread does.

### Safety

This chip can switch a lithium pack. Read this before wiring the FET drivers.

- **You do not have to.** Leave the FET drivers unconnected and it is a
  monitor. Nothing in this library switches anything unless you call
  `bq769x0_set_fets()`.
- **Protection is enforced in hardware.** Once `bq769x0_set_ov_uv()` has run
  the part acts on its own, and keeps working if your code hangs.
- **The current limits start at the part's defaults.** After a power-on
  reset, discharge is cut at 8 mV across the shunt after 8 ms (Table 8-10):
  8 A on a 1 mΩ shunt. Set your own with `bq769x0_set_protect()`. An MCU
  reboot is not a power-on reset: the part keeps what was last written.
- **`bq769x0_del()` leaves the part alone**, ADC included: OV and UV
  protection only run while it is on (8.3.1.2.1). `bq769x0_init()` keeps
  the FET bits as it finds them too: an MCU fed from the pack that cleared
  DSG_ON would brown out and reboot into doing it again.
- **Faults latch.** Clear them only once you know why they set. Init is the
  exception: it logs any fault still latched from before the restart, then
  clears it.

Nothing here has been run against a real pack. Bench it with a power supply
and a resistive load before it sees a battery.

### Four identities, none readable

The part ships with one of four I2C identities — 0x08 or 0x18, each with or
without CRC (Section 5) — and no register reports which. Init probes each
address; at one that ACKs it stores two values in `CC_CFG` and reads each
back, without CRC and then with: `0x00`, the reset value, then the `0x19`
the part should run with (Table 8-14). One value is not enough: a CRC part
ACKs a write without CRC and drops it at STOP (inferred from 8.3.1.4,
which only says a bad CRC is NACKed), and answers a read without
CRC with the data byte, so after an MCU reset that left the part's
`CC_CFG` at `0x19` a single write-and-read-back of `0x19` passes on the wrong
identity, and every later write is silently dropped. An address that ACKs
but stores neither is not this part: `-ENODEV`.

With CRC every byte carries a CRC-8, x^8 + x^2 + x + 1 from 0 (8.3.1.4); the first one of a write covers the address byte and the
register too.

### Calibration, OV and UV, steps, balancing

- **Factory calibration.** Cell voltage is GAIN × ADC + OFFSET (8.3.1.1.2,
  eq. 1); GAIN is 365 µV/LSB plus ADCGAIN<4:0>, whose bits are split
  between ADCGAIN1[3:2] and ADCGAIN2[7:5] (Tables 8-21, 8-22). The pack is
  4 × GAIN × BAT + cells × OFFSET (8.3.1.1.6, eq. 9). The BAT and CC
  registers are full 16 bit, not masked 14-bit results.
- **OV and UV do not share a base.** Both hold bits 11:4 of a 14-bit
  threshold, but the implicit top bits are `0b10` for OV and `0b01` for UV,
  and the bottom four `1000` and `0000` (Tables 8-12, 8-13). OV rounds down
  and UV up, so neither cutoff is looser than asked; out of the reachable
  window (OVRANGE, UVRANGE in 7.5) is refused. Read back what was set.
- **Fixed steps.** SCD, OCD and the four delays only have the steps of
  Tables 8-9 to 8-11; each value goes down to the step at or below it, and
  below the smallest is refused. SCD and OCD share the RSNS range bit.
- **Adjacent cells never balance together** within one set of five inputs
  (8.3.1.3.3); the check counts cells, not inputs, so 3S cells 2 and 3 on
  VC2 and VC5 are refused.

### Provenance

Registers and bits against the register map (8.5, Tables 8-3 to 8-23), the
identities against Section 5, gain, offset and scale factors against
8.3.1.1, and the protection steps, cell wiring and CRC rules against the
sections cited beside them in the code. The register map also agrees with
LibreSolar's `bq769x0-arduino-library` and `bms-firmware`; where they and
the datasheet differ, the datasheet wins.

## Changes from the original repos

`max17048.h` and `bq769x0.h` keep every type and call: a token-level diff of
each preprocessed header under ESP-IDF against esp-max17048-fuelgauge
5eec8fe (the SHA esp-forge pins; the header is the same at 81df2e3) and
esp-bq769x0-bms 04dde64 removes nothing. dingo_s3 builds against this
library unchanged. What changed in behaviour:

- **Presence is decided by the address.** Each init probes its address
  before it allocates or writes anything. A MAX17048 that does not ACK is
  `ESP_ERR_NOT_FOUND`, as its header always said; before, it was whatever
  the first read returned. A stuck bus during the probe is
  `ESP_ERR_TIMEOUT` rather than `ESP_ERR_NOT_FOUND`.
- **Bus errors go through errno and back.** `ESP_ERR_TIMEOUT`,
  `INVALID_ARG`, `INVALID_STATE`, `NO_MEM`, `NOT_FOUND`, `INVALID_CRC`,
  `INVALID_SIZE` and `NOT_SUPPORTED` come back as they went in. Any other
  IDF code from the bus comes back as `ESP_FAIL`.
- **The MAX17048's init sends no POR.** esp-max17048-fuelgauge wrote
  0x5400 to CMD on every init and failed the init when that write was not
  ACKed, which per p14 it never is; and every init forced a quick-start
  (p9). Init now reads STATUS.RI and writes CONFIG either way, clearing RI
  if it was set; see [above](#max17048). The original left RI set.
- **The MAX17048's VERSION check is the datasheet's** `0x001_` (p10 Table
  2), where the original refused only `0x0000` and `0xFFFF`.
- **The BQ769x0's identity check stores two values**, `0x00` and `0x19`,
  in each mode, where the original stored `0x19` alone; see
  [four identities](#four-identities-none-readable). An address that ACKs
  but is not the part is `-ENODEV` (`ESP_ERR_NOT_FOUND`, as before).
- **One log tag.** Lines go out under the ESP-IDF tag `battery`, prefixed
  `max17048:` or `bq769x0:`; they were under `max17048` and `bq769x0`.
- New: the STM32 HAL, `max17048_battery()` and `bq769x0_battery()`.
