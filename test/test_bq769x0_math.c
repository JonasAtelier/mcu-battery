/*
 * Host-side check of the conversions and the CRC, esp-bq769x0-bms's
 * test_math.c unchanged but for the include paths.
 *
 * The ADC gain is the dangerous one: its five bits are split across two
 * registers at different offsets, and getting the assembly wrong shifts every
 * cell voltage by a few percent. On a lithium pack that is the difference
 * between a full cell and an overcharged one, and nothing reports it.
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "bq769x0_math.h"
#include "bq769x0_regs.h"

#define NEAR(a, b) (fabs((double)(a) - (double)(b)) < 1e-3 * fabs(b) + 1e-6)

static void test_adc_gain_assembly(void)
{
    /* Both fields zero is the minimum, 365 uV/LSB. */
    assert(bq769x0_adc_gain_uv(0x00, 0x00) == 365);
    /* All five bits set is 365 + 31 = 396, the datasheet maximum. */
    assert(bq769x0_adc_gain_uv(0x0C, 0xE0) == 396);

    /* ADCGAIN1 bits 3:2 are the HIGH bits, shifted left by one. */
    assert(bq769x0_adc_gain_uv(0x04, 0x00) == 365 + 8);
    assert(bq769x0_adc_gain_uv(0x08, 0x00) == 365 + 16);
    assert(bq769x0_adc_gain_uv(0x0C, 0x00) == 365 + 24);
    /* ADCGAIN2 bits 7:5 are the LOW three. */
    assert(bq769x0_adc_gain_uv(0x00, 0x20) == 365 + 1);
    assert(bq769x0_adc_gain_uv(0x00, 0xE0) == 365 + 7);

    /* Bits outside the two fields must be ignored, not leak in. */
    assert(bq769x0_adc_gain_uv(0xF3, 0x1F) == 365);
    assert(bq769x0_adc_gain_uv(0xFF, 0xFF) == 396);

    /* The result must always stay inside the datasheet range. */
    for (int a = 0; a < 256; a++)
        for (int b = 0; b < 256; b += 17) {
            int g = bq769x0_adc_gain_uv((uint8_t)a, (uint8_t)b);
            assert(g >= 365 && g <= 396);
        }
}

static void test_adc_offset_is_signed(void)
{
    assert(bq769x0_adc_offset_mv(0x00) == 0);
    assert(bq769x0_adc_offset_mv(0x7F) == 127);
    assert(bq769x0_adc_offset_mv(0xFF) == -1);
    assert(bq769x0_adc_offset_mv(0x80) == -128);
}

static void test_adc_raw_masks_the_top_bits(void)
{
    /* Results are 14 bit; the top two bits of the high byte are not data. */
    assert(bq769x0_adc_raw(0x3F, 0xFF) == 0x3FFF);
    assert(bq769x0_adc_raw(0xFF, 0xFF) == 0x3FFF);
    assert(bq769x0_adc_raw(0xC0, 0x00) == 0);
    assert(bq769x0_adc_raw(0x00, 0x12) == 0x0012);
}

static void test_cell_and_pack_voltage(void)
{
    /* A 3.7 V cell at 380 uV/LSB is about 9737 counts. */
    int mv = bq769x0_cell_mv(9737, 380, 0);
    assert(mv > 3690 && mv < 3710);

    /* The offset is added once per cell reading. */
    assert(bq769x0_cell_mv(1000, 380, 10) == bq769x0_cell_mv(1000, 380, 0) + 10);
    assert(bq769x0_cell_mv(0, 380, -5) == -5);

    /* Pack is scaled by four, and the offset applies once per cell. */
    assert(bq769x0_pack_mv(1000, 380, 0, 6) == 4 * 380 * 1000 / 1000);
    assert(bq769x0_pack_mv(1000, 380, 10, 6) ==
           bq769x0_pack_mv(1000, 380, 0, 6) + 60);
    /* BAT is 16 bit: full scale at the top gain and cell count must not wrap. */
    assert(bq769x0_pack_mv(0xFFFF, 396, 0, 15) == 103807);  /* 4 * 396 * 65535 / 1000 */

    /* A 6S pack of 3.7 V cells is about 22.2 V; the pack register should land
     * near the sum of the cells, which is the sanity check that matters. */
    {
        int cell_raw = 9737, gain = 380, off = 0, cells = 6;
        int sum = 6 * bq769x0_cell_mv(cell_raw, gain, off);
        int pack = bq769x0_pack_mv(cell_raw * 6 / 4, gain, off, cells);
        assert(abs(pack - sum) < sum / 20);   /* within 5% */
    }
}

static void test_current(void)
{
    /* 8.44 uV per LSB across the shunt. 1 mOhm and 1000 counts is 8440 mA. */
    assert(NEAR(bq769x0_current_ma(1000, 1.0f), 8440.0f));
    /* Halving the shunt doubles the current for the same counts. */
    assert(NEAR(bq769x0_current_ma(1000, 0.5f), 16880.0f));
    /* Signed: discharge must come out negative. */
    assert(bq769x0_current_ma(-1000, 1.0f) < 0.0f);
    assert(NEAR(bq769x0_current_ma(-1000, 1.0f), -8440.0f));
    assert(bq769x0_current_ma(0, 1.0f) == 0.0f);
    /* No shunt value means no current, not a division by zero. */
    assert(bq769x0_current_ma(1000, 0.0f) == 0.0f);
    assert(bq769x0_current_ma(1000, -1.0f) == 0.0f);
}

static void test_temperature(void)
{
    /* At 25 C the thermistor is 10k, which is half the divider: V = 1650 mV,
     * so the raw count is 1650000 / 382. */
    uint16_t raw25 = (uint16_t)(1650.0f * 1000.0f / 382.0f);
    float t = bq769x0_temperature_c(raw25, 3435.0f);

    assert(fabs(t - 25.0f) < 1.0f);

    /* Hotter thermistor means lower resistance means lower V_TS. */
    assert(bq769x0_temperature_c((uint16_t)(raw25 * 0.7f), 3435.0f) > t);
    assert(bq769x0_temperature_c((uint16_t)(raw25 * 1.3f), 3435.0f) < t);

    /* An open or shorted thermistor lands at the rails and must not produce
     * a plausible-looking temperature. */
    assert(isnan(bq769x0_temperature_c(0, 3435.0f)));
    assert(isnan(bq769x0_temperature_c(0x3FFF, 3435.0f)));
    assert(isnan(bq769x0_temperature_c(raw25, 0.0f)));
}

static void test_ov_uv_trip_round_trip(void)
{
    const int gain = 380, off = 0;

    /* The registers hold only the middle 8 bits, so a set/read round trip
     * lands near what was asked but not on it. Assert the error is bounded
     * rather than pretending it is exact. */
    /* OV and UV reach different windows because their implicit top bits
     * differ; outside them the value wraps and must be screened. */
    assert(bq769x0_ov_trip_fits(4200, gain, off));
    assert(!bq769x0_ov_trip_fits(2500, gain, off));
    assert(bq769x0_uv_trip_fits(2800, gain, off));
    assert(!bq769x0_uv_trip_fits(4200, gain, off));

    for (int mv = 3200; mv <= 4600; mv += 25) {
        uint8_t r = bq769x0_ov_trip_reg(mv, gain, off);
        int back = bq769x0_ov_trip_mv(r, gain, off);

        assert(abs(back - mv) < 30);     /* one LSB is 16 counts * 380 uV ~ 6 mV */
    }
    for (int mv = 1600; mv <= 3050; mv += 25) {
        uint8_t r = bq769x0_uv_trip_reg(mv, gain, off);
        int back = bq769x0_uv_trip_mv(r, gain, off);

        assert(abs(back - mv) < 30);
    }
    /* OV has bit 13 implicitly set and UV does not, so for the same register
     * value OV must read considerably higher. */
    assert(bq769x0_ov_trip_mv(0x80, gain, off) > bq769x0_uv_trip_mv(0x80, gain, off));
}

static void test_ov_uv_trip_matches_datasheet(void)
{
    /* SLUSBK2I Tables 8-12 and 8-13: OV is 10-OV_T<7:0>-1000, UV is
     * 01-UV_T<7:0>-0000. With the datasheet's example calibration (GAIN 380,
     * OFFSET 30, 8.3.1.1.2) the reset values 0xAC and 0x97 are 0x2AC8 and
     * 0x1970: 380 * 10952 / 1000 + 30 and 380 * 6512 / 1000 + 30. */
    assert(bq769x0_ov_trip_mv(0xAC, 380, 30) == 4191);
    assert(bq769x0_uv_trip_mv(0x97, 380, 30) == 2504);

    /* The window is OVRANGE 0x2008..0x2FF8 (SLUSBK2I 7.5), not the raw
     * span of the top two bits. */
    assert(!bq769x0_ov_trip_fits(3113, 380, 0));    /* 0x2000 */
    assert(bq769x0_ov_trip_fits(4666, 380, 0));     /* 0x2FF6 */
}

static void test_ov_uv_trip_never_looser(void)
{
    const int gain = 380, off = 0;

    /* A request lands between two steps. OV must take the one below and UV
     * the one above: never a cutoff looser than asked. */
    for (int mv = 3100; mv <= 4700; mv++)
        if (bq769x0_ov_trip_fits(mv, gain, off))
            assert(bq769x0_ov_trip_mv(bq769x0_ov_trip_reg(mv, gain, off),
                                      gain, off) <= mv);
    for (int mv = 1540; mv <= 3120; mv++)
        if (bq769x0_uv_trip_fits(mv, gain, off))
            assert(bq769x0_uv_trip_mv(bq769x0_uv_trip_reg(mv, gain, off),
                                      gain, off) >= mv);

    /* By hand. 4196 mV is 11042 counts; the OV steps either side are 0x2B18
     * (11032) and 0x2B28 (11048), so OV takes 0x2B18: 4192 mV. 2800 mV is
     * 7368.4 counts; the UV steps are 0x1CC0 (7360) and 0x1CD0 (7376), so UV
     * takes 0x1CD0: 2802 mV. */
    assert(bq769x0_ov_trip_mv(bq769x0_ov_trip_reg(4196, gain, off), gain, off) == 4192);
    assert(bq769x0_uv_trip_mv(bq769x0_uv_trip_reg(2800, gain, off), gain, off) == 2802);
}

static void test_crc8(void)
{
    /* CRC-8-CCITT, polynomial 0x07, seeded at 0. */
    assert(bq769x0_crc8(0, 0x00) == 0x00);
    /* Known vector: the string "123456789" gives 0xF4. */
    {
        const char *s = "123456789";
        uint8_t c = 0;

        for (const char *p = s; *p; p++)
            c = bq769x0_crc8(c, (uint8_t)*p);
        assert(c == 0xF4);
    }
    /* A single bit flip must change the result. */
    assert(bq769x0_crc8(0, 0x01) != bq769x0_crc8(0, 0x02));
}

static void test_register_constants(void)
{
    assert(BQ769X0_ADDR_A == 0x08 && BQ769X0_ADDR_B == 0x18);
    assert(BQ769X0_SYS_STAT == 0x00 && BQ769X0_SYS_CTRL1 == 0x04);
    assert(BQ769X0_SYS_CTRL2 == 0x05 && BQ769X0_CC_CFG == 0x0B);
    assert(BQ769X0_CC_CFG_VALUE == 0x19);
    assert(BQ769X0_VC1_HI == 0x0C && BQ769X0_BAT_HI == 0x2A);
    assert(BQ769X0_TS1_HI == 0x2C && BQ769X0_CC_HI == 0x32);
    assert(BQ769X0_ADCGAIN1 == 0x50 && BQ769X0_ADCOFFSET == 0x51);
    assert(BQ769X0_ADCGAIN2 == 0x59);
    /* The 15 cell registers must reach exactly up to BAT. */
    assert(BQ769X0_VC1_HI + BQ769X0_MAX_CELLS * 2 == BQ769X0_BAT_HI);
    assert(BQ769X0_CHG_ON == 0x01 && BQ769X0_DSG_ON == 0x02);
    assert(BQ769X0_CC_EN == 0x40 && BQ769X0_ADC_EN == 0x10);
}

int main(void)
{
    test_adc_gain_assembly();
    test_adc_offset_is_signed();
    test_adc_raw_masks_the_top_bits();
    test_cell_and_pack_voltage();
    test_current();
    test_temperature();
    test_ov_uv_trip_round_trip();
    test_ov_uv_trip_matches_datasheet();
    test_ov_uv_trip_never_looser();
    test_crc8();
    test_register_constants();
    printf("all bq769x0 math checks passed\n");
    return 0;
}
