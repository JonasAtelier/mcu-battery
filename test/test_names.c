/*
 * Every public header in one ESP-IDF translation unit, under -Werror: a
 * type, tag, enumerator or prototype two of them define fails the build, and
 * so does a macro defined twice differently. The link-time half -- no global
 * symbol outside battery_*, max17048_* and bq769x0_* -- is checked with nm
 * on a real firmware build; see docs/development.md.
 */
#include <stdio.h>

#include "battery.h"
#include "battery_log.h"
#include "bq769x0.h"
#include "max17048.h"

int main(void)
{
	/* One of each type, so each header is used, not skipped. */
	struct battery_sample s = { .cells = 1 };
	max17048_config_t gauge = { .scl_hz = 0 };
	bq769x0_config_t monitor = { .cells = 6 };
	bq769x0_protect_t protect = { .ocd_delay_ms = 8 };

	(void)max17048_battery;
	(void)bq769x0_battery;
	printf("test_names: %d %d %d %d, headers coexist\n", s.cells,
	       (int)gauge.scl_hz, monitor.cells, protect.ocd_delay_ms);
	return 0;
}
