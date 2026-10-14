/*
 * Host-side check of the conversions, esp-max17048-fuelgauge's test_math.c
 * unchanged but for the include paths.
 *
 * The two reference drivers express the voltage scale differently -- Linux as
 * raw * 625 / 8 microvolts, Zephyr as raw * 78125 / 1000 -- so the first test
 * asserts both forms agree with the constant here. A future edit that changes
 * one has to fail.
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "max17048_math.h"
#include "max17048_regs.h"

#define NEAR(a, b) (fabs((double)(a) - (double)(b)) < 1e-4 * fabs(b) + 1e-9)

static void test_voltage_scale_matches_both_drivers(void)
{
    for (uint32_t raw = 0; raw <= 0xFFFF; raw += 257) {
        double linux_uv  = (double)raw * 625.0 / 8.0;       /* max17040_battery.c */
        double zephyr_uv = (double)raw * 78125.0 / 1000.0;  /* Zephyr max17048.c */

        assert(fabs(linux_uv - zephyr_uv) < 1e-6);
        assert(fabs(max17048_volts((uint16_t)raw) - linux_uv / 1e6) < 1e-6);
    }
    /* A full 16-bit count is about 5.12 V, which is why this part is 1S only. */
    assert(NEAR(max17048_volts(0xFFFF), 5.11992f));
    assert(max17048_volts(0) == 0.0f);
    /* A healthy lithium cell sits around 3.7 V. */
    assert(NEAR(max17048_volts(47360), 3.7f));
}

static void test_soc(void)
{
    /* 1% per 256 counts, and the low byte is fractional -- not noise. */
    assert(NEAR(max17048_soc_percent(256), 1.0f));
    assert(NEAR(max17048_soc_percent(25600), 100.0f));
    assert(NEAR(max17048_soc_percent(128), 0.5f));
    assert(NEAR(max17048_soc_percent(0), 0.0f));
    /* Truncating to whole percent would lose this. */
    assert(max17048_soc_percent(25700) > 100.0f);
}

static void test_crate_is_signed(void)
{
    assert(NEAR(max17048_crate_pct_per_hour(0), 0.0f));
    assert(NEAR(max17048_crate_pct_per_hour(100), 20.8f));
    /* Discharging is negative, so the register must be read as signed. */
    assert(max17048_crate_pct_per_hour(0xFFFF) < 0.0f);
    assert(NEAR(max17048_crate_pct_per_hour(0xFFFF), -0.208f));
    assert(NEAR(max17048_crate_pct_per_hour(0x8000), -32768.0f * 0.208f));
}

static void test_hours_remaining(void)
{
    /* Discharging at 10%/hr from 50% is 5 hours to empty. */
    assert(NEAR(max17048_hours_from_rate(50.0f, -10.0f), 5.0f));
    /* Charging at 10%/hr from 50% is 5 hours to full. */
    assert(NEAR(max17048_hours_from_rate(50.0f, 10.0f), 5.0f));
    assert(NEAR(max17048_hours_from_rate(0.0f, -10.0f), 0.0f));
    assert(NEAR(max17048_hours_from_rate(100.0f, 10.0f), 0.0f));
    /* At rest the rate is near zero; dividing would report centuries. */
    assert(max17048_hours_from_rate(50.0f, 0.0f) == 0.0f);
    assert(max17048_hours_from_rate(50.0f, 0.005f) == 0.0f);
    assert(max17048_hours_from_rate(50.0f, -0.005f) == 0.0f);
}

static void test_config_packing(void)
{
    uint16_t c = 0;

    /* RCOMP is the high byte; the flags in the low byte must survive. */
    c = 0x00AB;
    c = max17048_config_with_rcomp(c, 0x97);
    assert(c == 0x97AB);
    assert(max17048_get_rcomp(c) == 0x97);

    /* ATHD counts DOWN from 32: 4% is stored as 28. */
    c = max17048_config_with_athd(0x9700, 4);
    assert((c & 0x1F) == 28);
    assert(max17048_get_athd(c) == 4);
    /* and the RCOMP half is untouched */
    assert(max17048_get_rcomp(c) == 0x97);

    assert(max17048_get_athd(max17048_config_with_athd(0, 1)) == 1);
    assert(max17048_get_athd(max17048_config_with_athd(0, 32)) == 32);
    /* Out of range clamps rather than wrapping into another field. */
    assert(max17048_get_athd(max17048_config_with_athd(0, 0)) == 1);
    assert(max17048_get_athd(max17048_config_with_athd(0, 99)) == 32);
    for (int p = 1; p <= 32; p++)
        assert(max17048_get_athd(max17048_config_with_athd(0xAB00, p)) == p);
}

static void test_word_is_big_endian(void)
{
    const uint8_t p[2] = { 0x12, 0x34 };

    assert(max17048_word(p) == 0x1234);
}

static void test_register_constants(void)
{
    assert(MAX17048_ADDR == 0x36);
    assert(MAX17048_VCELL == 0x02 && MAX17048_SOC == 0x04);
    assert(MAX17048_MODE == 0x06 && MAX17048_VERSION == 0x08);
    assert(MAX17048_CONFIG == 0x0C && MAX17048_CRATE == 0x16);
    assert(MAX17048_STATUS == 0x1A && MAX17048_CMD == 0xFE);
    assert(MAX17048_POR_VALUE == 0x5400);
    assert(MAX17048_MODE_QUICK_START == 0x4000);
    assert(MAX17048_CONFIG_ALRT == 0x0020);
}

int main(void)
{
    test_voltage_scale_matches_both_drivers();
    test_soc();
    test_crate_is_signed();
    test_hours_remaining();
    test_config_packing();
    test_word_is_big_endian();
    test_register_constants();
    printf("all max17048 math checks passed\n");
    return 0;
}
