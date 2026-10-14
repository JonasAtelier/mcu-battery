/*
 * bq769x0 -- ESP-IDF driver for the TI bq76920 / bq76930 / bq76940 battery
 * monitors.
 *
 * These are analog front ends, not fuel gauges. They measure every cell in a
 * series pack individually off the balance lead, count coulombs through a
 * shunt, and carry hardware protection that can cut the pack without any help
 * from software.
 *
 *   bq76920   3 to 5 cells
 *   bq76930   6 to 10 cells   <- a 6S LiPo lives here
 *   bq76940   9 to 15 cells
 *
 * You can use it purely as a monitor: leave the FET drivers unconnected and
 * nothing in this driver will switch anything.
 *
 * Register provenance is in src/bq769x0/bq769x0_regs.h. Nothing here has
 * been run against a physical chip, and this drives a lithium pack -- read
 * the safety notes in docs/chips.md before wiring FETs to it.
 */
/*
 * In mcu-battery: the same calls on the STM32 HAL, where they return 0 or a
 * negative errno and the part hangs off an I2C handle the app has already
 * HAL_I2C_Init()ed, whose clock wins over scl_hz; and bq769x0_battery(),
 * the handle as a struct battery_dev. A CRC mismatch is -EBADMSG on the
 * HAL and ESP_ERR_INVALID_CRC on ESP-IDF, as before. See docs/chips.md.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef ESP_PLATFORM
#include "driver/i2c_master.h"
#include "esp_err.h"
#else
#include CMSIS_device_header
#endif

#include "battery.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BQ769X0_CELLS_MAX 15

/* A handle belongs to one task: its calls are not locked against each other.
 * The I2C bus underneath is shared safely with other devices. */
typedef struct bq769x0_t *bq769x0_handle_t;

typedef struct {
    uint32_t scl_hz;        /* 0 selects 100 kHz; this part is not fast */
    int      cells;         /* how many are actually connected, 3..15 */
    uint16_t cell_channels; /* the VC inputs that carry a cell, bit 0 = VC1;
                               0 selects TI's wiring for `cells` (README) */
    float    shunt_mohm;    /* the sense resistor; without it there is no current */
    float    thermistor_beta;  /* 0 selects 3435, a common 10k NTC */
} bq769x0_config_t;

/* SYS_STAT bits, as in bq769x0_data_t.status (SLUSBK2I Table 8-3). */
#define BQ769X0_STAT_CC_READY      0x80
#define BQ769X0_STAT_DEVICE_XREADY 0x20   /* internal fault; latches the FETs off */
#define BQ769X0_STAT_OVRD_ALERT    0x10
#define BQ769X0_STAT_UV            0x08
#define BQ769X0_STAT_OV            0x04
#define BQ769X0_STAT_SCD           0x02   /* short circuit in discharge */
#define BQ769X0_STAT_OCD           0x01   /* overcurrent in discharge */
#define BQ769X0_STAT_FAULTS        0x3F

typedef struct {
    int   cell_mv[BQ769X0_CELLS_MAX];   /* per cell, millivolts */
    int   cells;                        /* how many are populated */
    int   pack_mv;
    float current_ma;                   /* signed, positive charging */
    float temperature_c;                /* TS1; NAN if the thermistor is open */
    float temperature2_c;               /* TS2, bq76930/40; NAN on a bq76920 */
    float temperature3_c;               /* TS3, bq76940; NAN on the others */
    uint8_t status;                     /* BQ769X0_STAT_* */
} bq769x0_data_t;

#ifdef ESP_PLATFORM
/*
 * Find the device, set CC_CFG, read the factory ADC calibration and start
 * continuous measurement.
 *
 * The part has four possible identities -- two I2C addresses, each with or
 * without CRC, fixed at the factory and not readable. This tries all four.
 *
 * ESP_ERR_NOT_FOUND if none of them answer.
 * ESP_ERR_INVALID_ARG for a cell count outside 3..15 or a missing shunt value.
 */
esp_err_t bq769x0_init(i2c_master_bus_handle_t bus, const bq769x0_config_t *cfg,
                       bq769x0_handle_t *out);
esp_err_t bq769x0_del(bq769x0_handle_t dev);

/* Everything in one pass. */
esp_err_t bq769x0_read(bq769x0_handle_t dev, bq769x0_data_t *out);

/* The lowest and highest cell, and the spread between them. A healthy pack
 * sits within a few tens of millivolts; a growing spread is the first sign of
 * a cell going bad, long before the pack voltage shows anything. */
esp_err_t bq769x0_cell_spread(bq769x0_handle_t dev, int *min_mv, int *max_mv,
                              int *spread_mv);

/* SYS_STAT, and clearing it. Faults latch until cleared. */
esp_err_t bq769x0_status(bq769x0_handle_t dev, uint8_t *status);
esp_err_t bq769x0_clear_faults(bq769x0_handle_t dev, uint8_t mask);

/*
 * Overvoltage and undervoltage cutoffs, in millivolts per cell.
 *
 * The registers hold only the middle 8 bits of a 14-bit threshold, so what
 * you set is rounded, OV down and UV up so that neither is looser than asked
 * -- read it back with bq769x0_get_ov_uv() and use that, not what you asked
 * for.
 */
esp_err_t bq769x0_set_ov_uv(bq769x0_handle_t dev, int ov_mv, int uv_mv);
esp_err_t bq769x0_get_ov_uv(bq769x0_handle_t dev, int *ov_mv, int *uv_mv);

#else
/*
 * Find the device, set CC_CFG, read the factory ADC calibration and start
 * continuous measurement.
 *
 * The part has four possible identities -- two I2C addresses, each with or
 * without CRC, fixed at the factory and not readable. This tries all four.
 *
 * -ENXIO if none of them answer.
 * -EINVAL for a cell count outside 3..15 or a missing shunt value.
 */
int bq769x0_init(I2C_HandleTypeDef *bus, const bq769x0_config_t *cfg,
                 bq769x0_handle_t *out);
int bq769x0_del(bq769x0_handle_t dev);

/* Everything in one pass. */
int bq769x0_read(bq769x0_handle_t dev, bq769x0_data_t *out);

/* The lowest and highest cell, and the spread between them. A healthy pack
 * sits within a few tens of millivolts; a growing spread is the first sign of
 * a cell going bad, long before the pack voltage shows anything. */
int bq769x0_cell_spread(bq769x0_handle_t dev, int *min_mv, int *max_mv,
                        int *spread_mv);

/* SYS_STAT, and clearing it. Faults latch until cleared. */
int bq769x0_status(bq769x0_handle_t dev, uint8_t *status);
int bq769x0_clear_faults(bq769x0_handle_t dev, uint8_t mask);

/*
 * Overvoltage and undervoltage cutoffs, in millivolts per cell.
 *
 * The registers hold only the middle 8 bits of a 14-bit threshold, so what
 * you set is rounded, OV down and UV up so that neither is looser than asked
 * -- read it back with bq769x0_get_ov_uv() and use that, not what you asked
 * for.
 */
int bq769x0_set_ov_uv(bq769x0_handle_t dev, int ov_mv, int uv_mv);
int bq769x0_get_ov_uv(bq769x0_handle_t dev, int *ov_mv, int *uv_mv);

#endif

/*
 * Discharge current protection, and how long an OV or UV must last.
 *
 * Currents are in amps through the configured shunt. The part only has fixed
 * steps, so each value goes down to the nearest step at or below it -- never
 * a looser limit or a longer delay than asked -- and anything below the
 * smallest step is refused. SCD and OCD share one range bit: of the two
 * ranges where both requests have such a step, the one whose steps land
 * closest to the requests in total is used, the lower on a tie. Read back
 * what got set with bq769x0_get_protect().
 *
 * After a power-on reset the part runs its defaults, which cut discharge at
 * 8 mV across the shunt after 8 ms: 8 A on a 1 mOhm shunt. An ESP32 reboot is
 * not one; the part keeps what was last written. bq769x0_init() also leaves
 * DELAY_DIS as it finds it, and set, that bypasses these delays and the OV
 * and UV ones (SLUSBK2I Table 8-8).
 */
typedef struct {
    float scd_a;          /* short circuit in discharge */
    int   scd_delay_us;   /* 70, 100, 200; 400 only with 1 kOhm cell filters */
    float ocd_a;          /* overcurrent in discharge */
    int   ocd_delay_ms;   /* 8, 20, 40, 80, 160, 320, 640, 1280 */
    int   ov_delay_s;     /* 1, 2, 4, 8 */
    int   uv_delay_s;     /* 1, 4, 8, 16 */
} bq769x0_protect_t;

#ifdef ESP_PLATFORM
esp_err_t bq769x0_set_protect(bq769x0_handle_t dev, const bq769x0_protect_t *p);
esp_err_t bq769x0_get_protect(bq769x0_handle_t dev, bq769x0_protect_t *p);

/*
 * Cell balancing. mask bit 0 is cell 1, counted up the pack; the driver
 * moves each bit to the VC input that cell is wired to.
 *
 * Neighbouring cells within one set of five inputs must never balance
 * together -- their bleed resistors would end up in series across one cell.
 * This refuses such a mask, and one naming a cell above `cells`, rather than
 * letting the hardware do it.
 */
esp_err_t bq769x0_set_balancing(bq769x0_handle_t dev, uint16_t mask);

/*
 * The charge and discharge FETs.
 *
 * Only meaningful if you have wired the FET drivers. Turning the discharge
 * FET off under load interrupts a lithium pack's current path -- make sure
 * that is what you want.
 */
esp_err_t bq769x0_set_fets(bq769x0_handle_t dev, bool charge, bool discharge);

/* Which of the four identities was found, for logging and for anyone
 * wanting to talk to the part directly. */
uint8_t bq769x0_address(bq769x0_handle_t dev);
bool    bq769x0_crc_enabled(bq769x0_handle_t dev);

esp_err_t bq769x0_reg_read(bq769x0_handle_t dev, uint8_t reg, uint8_t *value);
esp_err_t bq769x0_reg_write(bq769x0_handle_t dev, uint8_t reg, uint8_t value);

#else
int bq769x0_set_protect(bq769x0_handle_t dev, const bq769x0_protect_t *p);
int bq769x0_get_protect(bq769x0_handle_t dev, bq769x0_protect_t *p);

/*
 * Cell balancing. mask bit 0 is cell 1, counted up the pack; the driver
 * moves each bit to the VC input that cell is wired to.
 *
 * Neighbouring cells within one set of five inputs must never balance
 * together -- their bleed resistors would end up in series across one cell.
 * This refuses such a mask, and one naming a cell above `cells`, rather than
 * letting the hardware do it.
 */
int bq769x0_set_balancing(bq769x0_handle_t dev, uint16_t mask);

/*
 * The charge and discharge FETs.
 *
 * Only meaningful if you have wired the FET drivers. Turning the discharge
 * FET off under load interrupts a lithium pack's current path -- make sure
 * that is what you want.
 */
int bq769x0_set_fets(bq769x0_handle_t dev, bool charge, bool discharge);

/* Which of the four identities was found, for logging and for anyone
 * wanting to talk to the part directly. */
uint8_t bq769x0_address(bq769x0_handle_t dev);
bool    bq769x0_crc_enabled(bq769x0_handle_t dev);

int bq769x0_reg_read(bq769x0_handle_t dev, uint8_t reg, uint8_t *value);
int bq769x0_reg_write(bq769x0_handle_t dev, uint8_t reg, uint8_t value);

#endif

/*
 * The part as a struct battery_dev, for battery_read(): every connected
 * cell, the pack, the current through the shunt and TS1; no state of charge
 * (NAN). Alerts from SYS_STAT: OV is BATTERY_ALERT_OVERVOLTAGE, UV
 * UNDERVOLTAGE, OCD OVERCURRENT, SCD SHORT_CIRCUIT, DEVICE_XREADY
 * CHIP_FAULT and OVRD_ALERT EXTERNAL. battery_clear_alerts() writes those
 * bits back to clear them. NULL for a NULL handle. Valid until
 * bq769x0_del().
 */
struct battery_dev *bq769x0_battery(bq769x0_handle_t dev);

#ifdef __cplusplus
}
#endif
