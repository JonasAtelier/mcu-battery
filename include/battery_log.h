/*
 * Kernel dev_err()-style logging for battery monitor drivers and their users.
 *
 * ESP-IDF: everything goes out under the one ESP-IDF tag "battery",
 * prefixed with the device's name, so one esp_log_level_set("battery", ...)
 * controls every chip and a line still says which chip it came from. There
 * is deliberately no level machinery of our own; battery_dbg() is also
 * subject to CONFIG_LOG_MAXIMUM_LEVEL, and CONFIG_BATTERY_DEBUG is not used.
 *
 * STM32 HAL: every line goes out through printf() as "battery: <name>:
 * ...", so a line still says which chip it came from. Where printf() ends up
 * is the app's business: with newlib, retarget _write() to a UART. There is
 * deliberately no level machinery; battery_dbg() alone is compiled out
 * unless CONFIG_BATTERY_DEBUG is 1 (stm-forge: BATTERY_DEBUG = y).
 */
#ifndef BATTERY_LOG_H
#define BATTERY_LOG_H

#ifdef ESP_PLATFORM
#include "esp_log.h"
#else
#include <stdio.h>
#endif

#include "battery.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef ESP_PLATFORM
#define BATTERY_LOG_TAG	"battery"

#define battery_err(bat, fmt, ...) \
	ESP_LOGE(BATTERY_LOG_TAG, "%s: " fmt, (bat)->name, ##__VA_ARGS__)
#define battery_warn(bat, fmt, ...) \
	ESP_LOGW(BATTERY_LOG_TAG, "%s: " fmt, (bat)->name, ##__VA_ARGS__)
#define battery_info(bat, fmt, ...) \
	ESP_LOGI(BATTERY_LOG_TAG, "%s: " fmt, (bat)->name, ##__VA_ARGS__)
#define battery_dbg(bat, fmt, ...) \
	ESP_LOGD(BATTERY_LOG_TAG, "%s: " fmt, (bat)->name, ##__VA_ARGS__)

#else /* STM32 HAL */
#define battery_err(bat, fmt, ...) \
	printf("battery: %s: " fmt "\n", (bat)->name, ##__VA_ARGS__)
#define battery_warn(bat, fmt, ...) \
	printf("battery: %s: " fmt "\n", (bat)->name, ##__VA_ARGS__)
#define battery_info(bat, fmt, ...) \
	printf("battery: %s: " fmt "\n", (bat)->name, ##__VA_ARGS__)

#if CONFIG_BATTERY_DEBUG
#define battery_dbg(bat, fmt, ...) \
	printf("battery: %s: " fmt "\n", (bat)->name, ##__VA_ARGS__)
#else
/* Still type-checked, so a debug line cannot rot while it is off. */
#define battery_dbg(bat, fmt, ...) do { \
	if (0) \
		printf("battery: %s: " fmt "\n", (bat)->name, ##__VA_ARGS__); \
} while (0)
#endif
#endif /* ESP_PLATFORM */

/*
 * One info line: pack voltage, state of charge, current, temperature
 * ("nan" where the chip has none), the alert bits, then each cell.
 */
void battery_log_sample(const struct battery_dev *bat,
			const struct battery_sample *s);

#ifdef __cplusplus
}
#endif

#endif /* BATTERY_LOG_H */
