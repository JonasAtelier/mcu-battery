/*
 * battery -- a small Linux-IIO-style subsystem for battery monitoring chips
 * on ESP-IDF and the STM32 HAL: fuel gauges and cell monitors, parts that
 * know they sit on a battery.
 *
 * One struct battery_dev per chip. It holds what every driver here shares:
 * one read that fills a struct battery_sample, and alerts that latch until
 * cleared. What only one chip has -- cell balancing, protection FETs and
 * trip points on the BQ769x0, the charge-rate model on the MAX17048 -- stays
 * in that chip's own header.
 *
 * Each chip keeps its own API (max17048.h, bq769x0.h). Its handle holds a
 * struct battery_dev and hands it out, for code that treats every battery
 * the same way.
 *
 * A plain shunt and bus-voltage monitor (INA219, INA226, ...) is not here
 * but in mcu-powermon, even when it is wired across a pack.
 *
 * Every int-returning function returns 0 or a negative errno. An op a chip
 * does not have is -EOPNOTSUPP.
 */
#ifndef BATTERY_H
#define BATTERY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The most cells one chip here measures: the bq76940's fifteen. */
#define BATTERY_CELLS_MAX		15

/*
 * Alerts in struct battery_sample.alerts. Each chip's header says which it
 * raises and what in the chip sets it. They latch in the chip until
 * battery_clear_alerts().
 */
#define BATTERY_ALERT_OVERVOLTAGE	(1u << 0)	/* a cell above its limit */
#define BATTERY_ALERT_UNDERVOLTAGE	(1u << 1)	/* a cell below its limit */
#define BATTERY_ALERT_OVERCURRENT	(1u << 2)	/* discharge, sustained */
#define BATTERY_ALERT_SHORT_CIRCUIT	(1u << 3)	/* discharge, fast */
#define BATTERY_ALERT_LOW_CHARGE	(1u << 4)	/* state of charge low */
#define BATTERY_ALERT_CHIP_FAULT	(1u << 5)	/* the chip's own fault */
#define BATTERY_ALERT_CHIP_RESET	(1u << 6)	/* settings lost */
#define BATTERY_ALERT_EXTERNAL		(1u << 7)	/* alert pin driven from outside */

/*
 * SI units throughout. A quantity the chip does not measure is NAN, so a
 * caller can tell "not measured" from zero with isnan(). Current is
 * positive while the pack charges.
 */
struct battery_sample {
	float pack_v;			/* V */
	float soc_percent;		/* 0..100; NAN where the chip has no gauge */
	float current_a;		/* A; NAN where there is no current sense */
	float temperature_c;		/* degrees C; NAN where there is no sensor */
	int cells;			/* cell_v entries filled, bottom cell first */
	float cell_v[BATTERY_CELLS_MAX];	/* V; entries past cells are 0 */
	uint32_t alerts;		/* BATTERY_ALERT_* latched in the chip */
};

struct battery_dev;

struct battery_ops {
	/* Every field; *s arrives zeroed */
	int (*read)(struct battery_dev *bat, struct battery_sample *s);
	/* optional; alerts is non-zero and has only BATTERY_ALERT_* bits */
	int (*clear_alerts)(struct battery_dev *bat, uint32_t alerts);
};

struct battery_dev {
	const char *name;		/* "max17048"; log prefix */
	const struct battery_ops *ops;
	void *priv;			/* the chip's state */
};

/* Read the chip. *s is written only on success. */
int battery_read(struct battery_dev *bat, struct battery_sample *s);

/*
 * Clear the latched alerts named, where the chip lets them be cleared.
 * With 0 alerts nothing is written. -EINVAL for a bit that is not a
 * BATTERY_ALERT_*, -EOPNOTSUPP for a chip with no alerts to clear.
 */
int battery_clear_alerts(struct battery_dev *bat, uint32_t alerts);

#ifdef __cplusplus
}
#endif

#endif /* BATTERY_H */
