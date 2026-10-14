/*
 * max17048 -- ESP-IDF driver for the Maxim MAX17048 lithium fuel gauge.
 *
 * Reports state of charge for ONE lithium cell without a sense resistor and
 * without coulomb counting. It models the cell instead: voltage in, percentage
 * out, and it learns as it goes.
 *
 * SINGLE CELL ONLY. The MAX17049 is the 2-cell sibling and is a different
 * part number; for anything larger you want a multi-cell monitor, not this.
 *
 * Register provenance is in src/max17048/max17048_regs.h. Nothing here has
 * been run against a physical chip.
 */
/*
 * In mcu-battery: the same calls on the STM32 HAL, where they return 0 or a
 * negative errno and the gauge hangs off an I2C handle the app has already
 * HAL_I2C_Init()ed, whose clock wins over scl_hz; and max17048_battery(),
 * the handle as a struct battery_dev. Presence is now decided by an
 * address probe, so a gauge that does not ACK is ESP_ERR_NOT_FOUND, as
 * promised below. See docs/chips.md.
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

typedef struct max17048_t *max17048_handle_t;

typedef struct {
    uint32_t scl_hz;      /* 0 selects 400 kHz */
    uint8_t  rcomp;       /* 0 selects the reset default, 0x97 */
    int      empty_alert_pct;  /* 1..32; 0 selects 4% */
} max17048_config_t;

typedef struct {
    float volts;          /* cell voltage */
    float soc_percent;    /* 0..100, with fractional resolution */
    float crate_pct_hr;   /* signed: positive charging */
    bool  alert;          /* the ALRT flag is set; see max17048_status() */
} max17048_data_t;

#ifdef ESP_PLATFORM
/*
 * Configure the gauge: RCOMP and the empty-alert threshold, then clear
 * STATUS.RI if the part has just powered up. No power-on reset is sent: the
 * part does not ACK one and it forces a quick-start (datasheet p14, p9).
 *
 * ESP_ERR_NOT_FOUND if nothing answers at 0x36.
 * ESP_ERR_INVALID_ARG for an empty-alert threshold outside 1..32.
 */
esp_err_t max17048_init(i2c_master_bus_handle_t bus, const max17048_config_t *cfg,
                        max17048_handle_t *out);
esp_err_t max17048_del(max17048_handle_t dev);

/* Voltage, state of charge and charge rate in one go. */
esp_err_t max17048_read(max17048_handle_t dev, max17048_data_t *out);

esp_err_t max17048_voltage(max17048_handle_t dev, float *volts);
esp_err_t max17048_soc(max17048_handle_t dev, float *percent);

/*
 * Hours until empty, or until full if charging. Derived from the charge rate,
 * so it needs a few minutes of steady load to mean anything, and returns 0
 * while the pack is at rest.
 */
esp_err_t max17048_hours_remaining(max17048_handle_t dev, float *hours);

/* The raw STATUS register. See MAX17048_STATUS_* in
 * src/max17048/max17048_regs.h. */
esp_err_t max17048_status(max17048_handle_t dev, uint16_t *status);
esp_err_t max17048_clear_alert(max17048_handle_t dev);

/*
 * Force the model to re-estimate from the present voltage.
 *
 * Only correct on a pack that is genuinely at rest. Under load the terminal
 * voltage is depressed by the cell's internal resistance and a quick start
 * will read that as a flatter battery than it is. Left alone, the gauge
 * converges on its own within a few percent -- which is usually better than
 * a badly timed quick start.
 */
esp_err_t max17048_quick_start(max17048_handle_t dev);

/*
 * RCOMP tunes the model to the cell chemistry and its temperature. The reset
 * default suits a typical lithium polymer cell around 20 C. Maxim's
 * application note gives a temperature compensation curve; without it,
 * accuracy drifts at the ends of the temperature range.
 */
esp_err_t max17048_set_rcomp(max17048_handle_t dev, uint8_t rcomp);

/* Under 1 uA. The gauge stops modelling while asleep, so it loses track of
 * anything that happens to the cell in the meantime. */
esp_err_t max17048_sleep(max17048_handle_t dev, bool sleep);

esp_err_t max17048_reg_read(max17048_handle_t dev, uint8_t reg, uint16_t *value);
esp_err_t max17048_reg_write(max17048_handle_t dev, uint8_t reg, uint16_t value);

#else
/*
 * Configure the gauge: RCOMP and the empty-alert threshold, then clear
 * STATUS.RI if the part has just powered up. No power-on reset is sent: the
 * part does not ACK one and it forces a quick-start (datasheet p14, p9).
 *
 * -ENXIO if nothing answers at 0x36.
 * -EINVAL for an empty-alert threshold outside 1..32.
 */
int max17048_init(I2C_HandleTypeDef *bus, const max17048_config_t *cfg,
                  max17048_handle_t *out);
int max17048_del(max17048_handle_t dev);

/* Voltage, state of charge and charge rate in one go. */
int max17048_read(max17048_handle_t dev, max17048_data_t *out);

int max17048_voltage(max17048_handle_t dev, float *volts);
int max17048_soc(max17048_handle_t dev, float *percent);

/*
 * Hours until empty, or until full if charging. Derived from the charge rate,
 * so it needs a few minutes of steady load to mean anything, and returns 0
 * while the pack is at rest.
 */
int max17048_hours_remaining(max17048_handle_t dev, float *hours);

/* The raw STATUS register. See MAX17048_STATUS_* in
 * src/max17048/max17048_regs.h. */
int max17048_status(max17048_handle_t dev, uint16_t *status);
int max17048_clear_alert(max17048_handle_t dev);

/*
 * Force the model to re-estimate from the present voltage.
 *
 * Only correct on a pack that is genuinely at rest. Under load the terminal
 * voltage is depressed by the cell's internal resistance and a quick start
 * will read that as a flatter battery than it is. Left alone, the gauge
 * converges on its own within a few percent -- which is usually better than
 * a badly timed quick start.
 */
int max17048_quick_start(max17048_handle_t dev);

/*
 * RCOMP tunes the model to the cell chemistry and its temperature. The reset
 * default suits a typical lithium polymer cell around 20 C. Maxim's
 * application note gives a temperature compensation curve; without it,
 * accuracy drifts at the ends of the temperature range.
 */
int max17048_set_rcomp(max17048_handle_t dev, uint8_t rcomp);

/* Under 1 uA. The gauge stops modelling while asleep, so it loses track of
 * anything that happens to the cell in the meantime. */
int max17048_sleep(max17048_handle_t dev, bool sleep);

int max17048_reg_read(max17048_handle_t dev, uint8_t reg, uint16_t *value);
int max17048_reg_write(max17048_handle_t dev, uint8_t reg, uint16_t value);

#endif

/*
 * The gauge as a struct battery_dev, for battery_read(): one cell, its
 * voltage and the state of charge; no current or temperature (NAN). Alerts
 * from STATUS: VH is BATTERY_ALERT_OVERVOLTAGE, VL UNDERVOLTAGE, HD
 * LOW_CHARGE and RI CHIP_RESET. battery_clear_alerts() clears those bits
 * and CONFIG.ALRT. NULL for a NULL handle. Valid until max17048_del().
 */
struct battery_dev *max17048_battery(max17048_handle_t dev);

#ifdef __cplusplus
}
#endif
