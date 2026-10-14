#include <stdio.h>

#include "battery_log.h"

void battery_log_sample(const struct battery_dev *bat,
			const struct battery_sample *s)
{
	/* " 4.123" per cell, fifteen of them, and the NUL; a wilder value
	 * only cuts the line short. */
	char cells[7 * BATTERY_CELLS_MAX + 1];
	size_t used = 0;
	int n = s->cells < BATTERY_CELLS_MAX ? s->cells : BATTERY_CELLS_MAX;

	cells[0] = '\0';
	for (int i = 0; i < n && used < sizeof(cells); i++)
		used += (size_t)snprintf(cells + used, sizeof(cells) - used,
					 " %.3f", (double)s->cell_v[i]);

	battery_info(bat, "%.3f V  %.1f %%  %.3f A  %.1f C  alerts 0x%02lx  "
		     "cells%s", (double)s->pack_v, (double)s->soc_percent,
		     (double)s->current_a, (double)s->temperature_c,
		     (unsigned long)s->alerts, cells);
}
