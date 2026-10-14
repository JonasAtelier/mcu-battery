/*
 * Register <-> physical-unit conversions, host testable.
 *
 * The LSBs are the datasheet's, 19-6171 Rev 7 p10-11 Table 2: VCELL
 * 78.125 uV/cell, SOC 1%/256, CRATE 0.208 %/hr. The reference drivers express
 * VCELL differently: Linux as raw * 625 / 8 microvolts, Zephyr as
 * raw * 78125 / 1000. The tests assert both agree with the constant here, so
 * a future edit cannot quietly change one.
 */
#pragma once

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

/* p10-11 Table 2. CRATE's sign the datasheet does not state; both
 * reference drivers read it two's complement, positive charging. */
#define MAX17048_VCELL_UV_PER_LSB   78.125f   /* Linux 625/8, Zephyr 78125/1000 */
#define MAX17048_SOC_PER_LSB        (1.0f / 256.0f)   /* 1% per 256 counts */
#define MAX17048_CRATE_PCT_HR       0.208f    /* per LSB, signed */

static inline float max17048_volts(uint16_t raw)
{
    return (float)raw * MAX17048_VCELL_UV_PER_LSB / 1e6f;
}

/* State of charge in percent. The register is 1%/256, so the low byte is
 * fractional -- do not truncate it away and then wonder why it jumps. */
static inline float max17048_soc_percent(uint16_t raw)
{
    return (float)raw * MAX17048_SOC_PER_LSB;
}

/* Charge rate in percent per hour, signed: positive charging, negative
 * discharging. This is what makes a time estimate possible without any
 * current sense at all. */
static inline float max17048_crate_pct_per_hour(uint16_t raw)
{
    return (float)(int16_t)raw * MAX17048_CRATE_PCT_HR;
}

/*
 * Hours until empty (discharging) or full (charging), from the SoC and its
 * rate of change.
 *
 * Returns a negative number when the pack is not moving in that direction,
 * and 0 when the rate is too small to divide by -- a gauge sitting at rest
 * has a crate near zero and would otherwise report centuries.
 */
static inline float max17048_hours_from_rate(float soc_pct, float crate_pct_hr)
{
    if (crate_pct_hr > 0.01f)
        return (100.0f - soc_pct) / crate_pct_hr;      /* to full */
    if (crate_pct_hr < -0.01f)
        return soc_pct / -crate_pct_hr;                /* to empty */
    return 0.0f;
}

/* CONFIG high byte is RCOMP; the low byte holds flags that must survive. */
static inline uint16_t max17048_config_with_rcomp(uint16_t config, uint8_t rcomp)
{
    return (uint16_t)((config & 0x00FF) | ((uint16_t)rcomp << 8));
}
static inline uint8_t max17048_get_rcomp(uint16_t config)
{
    return (uint8_t)(config >> 8);
}

/*
 * Empty-alert threshold, ATHD in CONFIG bits 4:0. The register counts
 * *down* from 32%: the threshold is (32 - ATHD)%, "00000b -> 32%, 00001b ->
 * 31% ... 11111b -> 1%" (p12, CONFIG Register), which is backwards from
 * every instinct. The POR value 0x1C is 4%.
 */
static inline uint16_t max17048_config_with_athd(uint16_t config, int percent)
{
    int v;

    if (percent < 1)
        percent = 1;
    if (percent > 32)
        percent = 32;
    v = 32 - percent;
    return (uint16_t)((config & ~0x001F) | (uint16_t)v);
}
static inline int max17048_get_athd(uint16_t config)
{
    return 32 - (int)(config & 0x001F);
}

/* Big endian, high byte first. */
static inline uint16_t max17048_word(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}
