/*
 * Conversions and the I2C CRC, host testable.
 *
 * The part reports raw ADC counts. Turning them into volts needs a per-device
 * gain and offset burned in at the factory, read from three registers whose
 * bits are scattered -- get the assembly wrong and every cell voltage is out
 * by a few percent, which on a lithium pack is the difference between a full
 * cell and an overcharged one.
 */
#pragma once

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "bq769x0_regs.h"

/*
 * ADC gain in microvolts per LSB, 365 to 396: 365 plus ADCGAIN<4:0>
 * (SLUSBK2I Tables 8-21, 8-22).
 *
 * The five bits live in two registers and neither is aligned:
 *   ADCGAIN1 bits 3:2 are the high bits, ADCGAIN2 bits 7:5 the low ones.
 */
static inline int bq769x0_adc_gain_uv(uint8_t adcgain1, uint8_t adcgain2)
{
    return 365 + (((adcgain1 & 0x0C) << 1) | ((adcgain2 & 0xE0) >> 5));
}

/* ADCOFFSET is a two's complement millivolt offset (SLUSBK2I Table 8-23). */
static inline int bq769x0_adc_offset_mv(uint8_t adcoffset)
{
    return (int8_t)adcoffset;
}

/* A result register is 14 bit: the top two bits of the high byte are junk. */
static inline uint16_t bq769x0_adc_raw(uint8_t hi, uint8_t lo)
{
    return (uint16_t)(((uint16_t)(hi & BQ769X0_ADC_MASK_HI) << 8) | lo);
}

/* One cell, in millivolts: GAIN x ADC + OFFSET (SLUSBK2I 8.3.1.1.2, eq. 1). */
static inline int bq769x0_cell_mv(uint16_t raw, int gain_uv, int offset_mv)
{
    return (int)((int32_t)raw * gain_uv / 1000) + offset_mv;
}

/*
 * Pack voltage, in millivolts. The BAT register is scaled by four relative to
 * a cell register, and the offset applies once per connected cell -- which is
 * why this needs to be told how many cells there are: 4 x GAIN x ADC +
 * cells x OFFSET (SLUSBK2I 8.3.1.1.6, eq. 9).
 */
static inline int bq769x0_pack_mv(uint16_t raw, int gain_uv, int offset_mv, int cells)
{
    return (int)(4LL * gain_uv * raw / 1000) + cells * offset_mv;
}

/*
 * Pack current in milliamps, from the coulomb counter and the sense resistor.
 * 8.44 uV per LSB across the shunt, signed: positive charging (SLUSBK2I
 * 8.3.1.1.3, eq. 3; CCLSB in 7.5).
 */
#define BQ769X0_CC_UV_PER_LSB  8.44f

static inline float bq769x0_current_ma(int16_t cc_raw, float shunt_mohm)
{
    if (!(shunt_mohm > 0.0f))
        return 0.0f;
    return (float)cc_raw * BQ769X0_CC_UV_PER_LSB / shunt_mohm;
}

/*
 * Thermistor temperature in degrees C, from a TS input.
 *
 * The datasheet allows 10k NTC thermistors only. V_TSX is 382 uV per LSB
 * against a 3.3 V reference through a 10k pull-up (SLUSBK2I 8.3.1.1.4,
 * eq. 4 and 5), and the Beta equation converts resistance to temperature
 * about a 25 C reference.
 */
#define BQ769X0_TS_UV_PER_LSB  382.0f
#define BQ769X0_TS_PULLUP_OHM  10000.0f
#define BQ769X0_TS_VREF_MV     3300.0f
#define BQ769X0_NTC_R25_OHM    10000.0f

static inline float bq769x0_temperature_c(uint16_t ts_raw, float beta)
{
    float vtsx_mv = (float)ts_raw * BQ769X0_TS_UV_PER_LSB / 1000.0f;
    float rts, kelvin;

    /* At the reference rail the divider maths blows up, and a shorted or open
     * thermistor lands exactly there. */
    if (vtsx_mv <= 0.0f || vtsx_mv >= BQ769X0_TS_VREF_MV || !(beta > 0.0f))
        return NAN;

    rts = BQ769X0_TS_PULLUP_OHM * vtsx_mv / (BQ769X0_TS_VREF_MV - vtsx_mv);
    kelvin = 1.0f / (1.0f / 298.15f + logf(rts / BQ769X0_NTC_R25_OHM) / beta);
    return kelvin - 273.15f;
}

/*
 * Overvoltage and undervoltage trip registers hold bits 11:4 of a 14-bit
 * threshold. The top two bits are implicit and DIFFERENT for the two:
 * 0b10 for OV, 0b01 for UV. Use the wrong base and the threshold is out by a
 * factor of about two -- an overvoltage cutoff that never trips.
 *
 * The bottom four bits are implicit too, and also differ: 1000 for OV, 0000
 * for UV (SLUSBK2I Tables 8-12, 8-13). So a request lands between two steps
 * about 6 mV apart. OV takes the step below and UV the step above, so neither
 * cutoff is looser than asked; TI's procedure (8.3.1.2.1) only shifts right,
 * which with OV's 1000 can land above the request. Read back what got set.
 *
 * That also fixes the reachable window, OVRANGE and UVRANGE in SLUSBK2I 7.5;
 * at a nominal 380 uV/LSB:
 *   OV   0x2008..0x2FF8, about 3115 to 4665 mV
 *   UV   0x1000..0x1FF0, about 1555 to 3110 mV
 * Anything outside wraps, so bq769x0_ov_trip_fits() screens it first.
 */
#define BQ769X0_OV_BASE  ((1 << 13) | 0x8)
#define BQ769X0_UV_BASE  (1 << 12)

static inline bool bq769x0_ov_trip_fits(int mv, int gain_uv, int offset_mv)
{
    int32_t v = (int32_t)(mv - offset_mv) * 1000 / gain_uv;

    return v >= BQ769X0_OV_BASE && v <= BQ769X0_OV_BASE + 0xFF0;
}
static inline bool bq769x0_uv_trip_fits(int mv, int gain_uv, int offset_mv)
{
    int32_t v = ((int32_t)(mv - offset_mv) * 1000 + gain_uv - 1) / gain_uv;  /* up */

    return v >= BQ769X0_UV_BASE && v <= BQ769X0_UV_BASE + 0xFF0;
}
static inline uint8_t bq769x0_ov_trip_reg(int mv, int gain_uv, int offset_mv)
{
    int32_t v = ((int32_t)(mv - offset_mv) * 1000 / gain_uv);

    return (uint8_t)(((v - BQ769X0_OV_BASE) >> 4) & 0xFF);       /* step below */
}
static inline int bq769x0_ov_trip_mv(uint8_t reg, int gain_uv, int offset_mv)
{
    int32_t v = BQ769X0_OV_BASE | ((int32_t)reg << 4);

    return (int)(v * gain_uv / 1000) + offset_mv;
}
static inline uint8_t bq769x0_uv_trip_reg(int mv, int gain_uv, int offset_mv)
{
    int32_t v = ((int32_t)(mv - offset_mv) * 1000 + gain_uv - 1) / gain_uv;  /* up */

    return (uint8_t)(((v - BQ769X0_UV_BASE + 15) >> 4) & 0xFF);  /* step above */
}
static inline int bq769x0_uv_trip_mv(uint8_t reg, int gain_uv, int offset_mv)
{
    int32_t v = BQ769X0_UV_BASE | ((int32_t)reg << 4);

    return (int)(v * gain_uv / 1000) + offset_mv;
}

/*
 * CRC-8-CCITT, polynomial 0x07 (x^8 + x^2 + x + 1, initial value 0,
 * SLUSBK2I 8.3.1.4), used by the CRC variants on every I2C byte.
 * The first byte's CRC covers the address byte as well as the data, which is
 * the part that catches people out.
 */
static inline uint8_t bq769x0_crc8(uint8_t crc, uint8_t data)
{
    uint8_t v = crc ^ data;

    for (int i = 0; i < 8; i++)
        v = (v & 0x80) ? (uint8_t)((v << 1) ^ 0x07) : (uint8_t)(v << 1);
    return v;
}
