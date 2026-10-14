/*
 * BQ76920 / BQ76930 / BQ76940 monitor, from esp-bq769x0-bms 04dde64: the
 * same calls on the battery bus layer, so on both platforms, plus the
 * struct battery_dev the handle hands out. The calls and their checks are
 * the original's; what changed is listed in docs/chips.md.
 */
#include "battery_priv.h"

#if CONFIG_BATTERY_BQ769X0

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "battery_log.h"
#include "bq769x0.h"
#include "bq769x0_math.h"
#include "bq769x0_regs.h"

#define DEFAULT_SCL_HZ  100000     /* the part is not a fast-mode device */
#define I2C_TIMEOUT_MS  100
#define DEFAULT_BETA    3435.0f

#define TRY(expr) do { int _e = (expr); if (_e) return _e; } while (0)

/* A handle belongs to one task: its calls are not locked against each other. */
struct bq769x0_t {
    struct battery_bus bus;
    struct battery_dev dev;
    uint8_t  address;
    bool     crc;
    int      cells;
    uint16_t channels;     /* VC inputs carrying a cell, bit 0 = VC1 */
    int      gain_uv;      /* factory ADC gain, microvolts per LSB */
    int      offset_mv;    /* factory ADC offset */
    float    shunt_mohm;
    float    beta;
};

/* For the lines logged before there is a handle. */
static const struct battery_dev bq769x0_log_dev = { .name = "bq769x0" };

static int bq769x0_read_sample(struct battery_dev *bat, struct battery_sample *s);
static int bq769x0_clear(struct battery_dev *bat, uint32_t alerts);

static const struct battery_ops bq769x0_ops = {
    .read         = bq769x0_read_sample,
    .clear_alerts = bq769x0_clear,
};

/* SYS_STAT bits and the battery alert each one raises (SLUSBK2I Table 8-3). */
static const struct {
    uint8_t  status;
    uint32_t alert;
} status_alerts[] = {
    { BQ769X0_STAT_OV,            BATTERY_ALERT_OVERVOLTAGE },
    { BQ769X0_STAT_UV,            BATTERY_ALERT_UNDERVOLTAGE },
    { BQ769X0_STAT_OCD,           BATTERY_ALERT_OVERCURRENT },
    { BQ769X0_STAT_SCD,           BATTERY_ALERT_SHORT_CIRCUIT },
    { BQ769X0_STAT_DEVICE_XREADY, BATTERY_ALERT_CHIP_FAULT },
    { BQ769X0_STAT_OVRD_ALERT,    BATTERY_ALERT_EXTERNAL },
};

#define STATUS_ALERT_COUNT (sizeof(status_alerts) / sizeof(status_alerts[0]))

/* ---------------------------------------------------------------- I2C ---- */

/*
 * On the CRC variants every byte carries a CRC-8. For a write the first CRC
 * covers the I2C address byte *and* the register, which is the detail that
 * catches people; for a read the first CRC covers the read address byte and
 * the data byte.
 */
static int reg_write(struct bq769x0_t *d, uint8_t reg, uint8_t value)
{
    uint8_t b[2];
    size_t n;

    b[0] = value;
    n = 1;

    if (d->crc) {
        uint8_t crc = 0;

        crc = bq769x0_crc8(crc, (uint8_t)(d->address << 1));  /* write address */
        crc = bq769x0_crc8(crc, reg);
        crc = bq769x0_crc8(crc, value);
        b[1] = crc;
        n = 2;
    }
    return d->bus.ops->transfer(&d->bus, reg, b, n, NULL, 0);
}

/*
 * Consecutive registers in one transaction; the address auto-increments. A
 * high and low byte are only guaranteed to be the same conversion when both
 * come in one such read (SLUSBK2I 8.3.1.1). With CRC every data byte is
 * followed by its own: the first covers the read address byte and the data,
 * each later one the data byte alone (SLUSBK2I 8.3.1.4).
 */
#define BLOCK_MAX  (BQ769X0_CC_HI + 2 - BQ769X0_VC1_HI)   /* 0x0C..0x33 */

static int read_block(struct bq769x0_t *d, uint8_t reg, uint8_t *buf, size_t len)
{
    uint8_t rx[2 * BLOCK_MAX];

    if (len > BLOCK_MAX)
        return -EMSGSIZE;

    if (!d->crc)
        return d->bus.ops->transfer(&d->bus, reg, NULL, 0, buf, len);

    TRY(d->bus.ops->transfer(&d->bus, reg, NULL, 0, rx, 2 * len));
    for (size_t i = 0; i < len; i++) {
        uint8_t crc = 0;

        if (i == 0)
            crc = bq769x0_crc8(crc, (uint8_t)((d->address << 1) | 1));  /* read address */
        if (bq769x0_crc8(crc, rx[2 * i]) != rx[2 * i + 1])
            return -EBADMSG;
        buf[i] = rx[2 * i];
    }
    return 0;
}

static int reg_read(struct bq769x0_t *d, uint8_t reg, uint8_t *value)
{
    return read_block(d, reg, value, 1);
}

battery_handle_ret bq769x0_reg_write(bq769x0_handle_t d, uint8_t reg, uint8_t value)
{
    if (!d)
        return BATTERY_HANDLE_RET(-EINVAL);

    return BATTERY_HANDLE_RET(reg_write(d, reg, value));
}

battery_handle_ret bq769x0_reg_read(bq769x0_handle_t d, uint8_t reg, uint8_t *value)
{
    if (!d || !value)
        return BATTERY_HANDLE_RET(-EINVAL);

    return BATTERY_HANDLE_RET(reg_read(d, reg, value));
}

/* -------------------------------------------------------------- init ----- */

/*
 * The VC inputs that carry a cell, bit 0 = VC1, for each cell count on the
 * smallest part that takes it. Fewer cells than inputs means shorting some,
 * and SLUSBK2I 9.1.8 Tables 9-2, 9-3 and 9-4 say which: each set of five
 * keeps its top input and at least three cells, and the lower sets fill first.
 * A shorted input reads about 0 V; the part ignores it for UV (8.3.1.2.1).
 */
static const uint16_t default_channels[BQ769X0_CELLS_MAX + 1] = {
    [3]  = 0x0013, [4]  = 0x0017, [5]  = 0x001F,                  /* bq76920 */
    [6]  = 0x0273, [7]  = 0x0277, [8]  = 0x02F7, [9]  = 0x02FF,
    [10] = 0x03FF,                                                /* bq76930 */
    [11] = 0x4EF7, [12] = 0x5EF7, [13] = 0x5EFF, [14] = 0x5FFF,
    [15] = 0x7FFF,                                                /* bq76940 */
};

/*
 * The four identities are not readable, so try each in turn. An address
 * that does not ACK is skipped; at one that does, CC_CFG is written and read
 * back, first without CRC and then with.
 *
 * One value is not enough. A CRC part ACKs every byte of a write without a
 * CRC and then drops it at STOP, and a read without CRC still hands back the
 * data byte -- inferred from SLUSBK2I 8.3.1.4, which only says a bad CRC is
 * NACKed, not what a missing one does. After an MCU reset that did not
 * reset the part, CC_CFG still holds 0x19, so writing 0x19 and reading 0x19
 * back passes on the wrong identity, and every later write is dropped. So each mode must
 * store two values: 0x00, CC_CFG's reset value (Table 8-14), and then the
 * 0x19 the part needs.
 *
 * -ENXIO when nothing ACKs; -ENODEV when something ACKs but stores neither
 * value, so is not a bq769x0; a probe that failed some other way -- a stuck
 * or busy bus -- comes back as its own errno.
 */
static bool stores(struct bq769x0_t *d, uint8_t value)
{
    uint8_t back;

    return reg_write(d, BQ769X0_CC_CFG, value) == 0 &&
           reg_read(d, BQ769X0_CC_CFG, &back) == 0 && back == value;
}

static int probe_identity(struct bq769x0_t *d, battery_i2c_bus bus, uint32_t scl_hz)
{
    static const uint8_t addrs[2] = { BQ769X0_ADDR_A, BQ769X0_ADDR_B };
    int err = -ENXIO;

    for (int a = 0; a < 2; a++) {
        int ret = battery_i2c_probe(bus, addrs[a], I2C_TIMEOUT_MS);

        if (ret) {
            if (ret != -ENXIO)
                err = ret;
            continue;
        }
        ret = battery_i2c_attach(&d->bus, bus, addrs[a], scl_hz, I2C_TIMEOUT_MS);
        if (ret) {
            err = ret;
            continue;
        }

        d->address = addrs[a];
        for (int crc = 0; crc < 2; crc++) {
            d->crc = crc != 0;
            if (stores(d, 0x00) && stores(d, BQ769X0_CC_CFG_VALUE))
                return 0;
        }
        battery_bus_detach(&d->bus);
        d->bus.ops = NULL;
        if (err == -ENXIO)
            err = -ENODEV;
    }
    return err;
}

static int init(battery_i2c_bus bus, const bq769x0_config_t *cfg,
                bq769x0_handle_t *out)
{
    bq769x0_handle_t d;
    uint8_t g1 = 0, g2 = 0, off = 0, ctrl2 = 0, stat = 0;
    int err;

    if (!bus || !cfg || !out)
        return -EINVAL;
    if (cfg->cells < 3 || cfg->cells > BQ769X0_CELLS_MAX) {
        battery_err(&bq769x0_log_dev, "cells must be 3..15, not %d", cfg->cells);
        return -EINVAL;
    }
    if (!(cfg->shunt_mohm > 0.0f)) {
        battery_err(&bq769x0_log_dev,
                    "shunt_mohm is required; without it there is no current");
        return -EINVAL;
    }
    if (cfg->cell_channels && ((cfg->cell_channels & ~0x7FFF) ||
                               __builtin_popcount(cfg->cell_channels) != cfg->cells)) {
        battery_err(&bq769x0_log_dev, "cell_channels 0x%04x does not name %d of VC1..VC15",
                    cfg->cell_channels, cfg->cells);
        return -EINVAL;
    }

    d = calloc(1, sizeof(*d));
    if (!d)
        return -ENOMEM;

    d->dev.name   = "bq769x0";
    d->cells      = cfg->cells;
    d->channels   = cfg->cell_channels ? cfg->cell_channels
                                       : default_channels[cfg->cells];
    d->shunt_mohm = cfg->shunt_mohm;
    d->beta       = cfg->thermistor_beta > 0.0f ? cfg->thermistor_beta : DEFAULT_BETA;

    err = probe_identity(d, bus, cfg->scl_hz ? cfg->scl_hz : DEFAULT_SCL_HZ);
    if (err) {
        battery_err(&d->dev, "no bq769x0 at 0x08 or 0x18, with or without CRC");
        free(d);
        return err;
    }

    /* The ADC gain and offset are burned in per device. Without them the cell
     * voltages are out by a few percent, which on lithium is the difference
     * between full and overcharged. */
    err = reg_read(d, BQ769X0_ADCGAIN1, &g1);
    if (!err)
        err = reg_read(d, BQ769X0_ADCGAIN2, &g2);
    if (!err)
        err = reg_read(d, BQ769X0_ADCOFFSET, &off);
    if (err)
        goto fail;

    d->gain_uv   = bq769x0_adc_gain_uv(g1, g2);
    d->offset_mv = bq769x0_adc_offset_mv(off);

    /* ADC on, thermistor inputs selected, continuous coulomb counting.
     * Only the events in SLUSBK2I Table 8-1 clear the FET bits, and an MCU
     * reboot is not one of them, so SYS_CTRL2 may still hold them on. Only
     * CC_EN is added: an MCU fed from the pack that cleared DSG_ON here
     * would brown out and reboot into doing it again. */
    err = reg_write(d, BQ769X0_SYS_CTRL1, BQ769X0_ADC_EN | BQ769X0_TEMP_SEL);
    if (!err)
        err = reg_read(d, BQ769X0_SYS_CTRL2, &ctrl2);
    if (!err)
        err = reg_write(d, BQ769X0_SYS_CTRL2, (uint8_t)(ctrl2 | BQ769X0_CC_EN));
    /* Faults latched before this boot may be why the MCU restarted. Say
     * which before clearing them, or the reason is gone. */
    if (!err)
        err = reg_read(d, BQ769X0_SYS_STAT, &stat);
    if (!err && (stat & BQ769X0_STAT_FAULTS)) {
        battery_warn(&d->dev, "clearing faults latched before init: 0x%02x",
                     stat & BQ769X0_STAT_FAULTS);
        err = reg_write(d, BQ769X0_SYS_STAT, (uint8_t)(stat & BQ769X0_STAT_FAULTS));
    }
    if (err)
        goto fail;

    d->dev.ops  = &bq769x0_ops;
    d->dev.priv = d;

    battery_info(&d->dev, "at 0x%02x%s: %d cells, gain %d uV/LSB, offset %d mV",
                 d->address, d->crc ? " with CRC" : "", d->cells,
                 d->gain_uv, d->offset_mv);

    *out = d;
    return 0;

fail:
    battery_bus_detach(&d->bus);
    free(d);
    return err;
}

battery_handle_ret bq769x0_init(battery_i2c_bus bus, const bq769x0_config_t *cfg,
                                bq769x0_handle_t *out)
{
    return BATTERY_HANDLE_RET(init(bus, cfg, out));
}

battery_handle_ret bq769x0_del(bq769x0_handle_t d)
{
    if (!d)
        return BATTERY_HANDLE_RET(-EINVAL);

    /* Leave the part as it is: a BMS that opens the pack on teardown is
     * worse than one that does nothing. That includes the ADC -- OV and UV
     * protection only run while it is on (SLUSBK2I 8.3.1.2.1), so stopping it
     * would leave the FETs on with no cutoff. */
    battery_bus_detach(&d->bus);
    free(d);
    return BATTERY_HANDLE_RET(0);
}

struct battery_dev *bq769x0_battery(bq769x0_handle_t d)
{
    return d ? &d->dev : NULL;
}

/* -------------------------------------------------------------- read ----- */

static int read_data(struct bq769x0_t *d, bq769x0_data_t *out)
{
    uint8_t b[BLOCK_MAX];      /* b[0] is VC1_HI */
    const uint8_t *p;
    uint8_t st = 0;

    memset(out, 0, sizeof(*out));
    out->cells = d->cells;

    /* SYS_STAT first: a CC_READY that arrives after this is left for the
     * next read, not cleared below unread. */
    TRY(reg_read(d, BQ769X0_SYS_STAT, &st));

    /* Cells, BAT, the TS inputs and CC in one transaction: a pair at a time
     * was 37 of them at 15 cells, and a pair split across two could straddle
     * a conversion. A bq76920 has no VC6..VC15, TS2 or TS3, and those
     * registers are only valid on the '30 and '40 (SLUSBK2I 8.5 notes), so
     * it reads around them: VC1..VC5, BAT and TS1, CC. */
    if (d->channels <= 0x001F) {
        TRY(read_block(d, BQ769X0_VC1_HI, b, 10));
        TRY(read_block(d, BQ769X0_BAT_HI, &b[BQ769X0_BAT_HI - BQ769X0_VC1_HI], 4));
        TRY(read_block(d, BQ769X0_CC_HI, &b[BQ769X0_CC_HI - BQ769X0_VC1_HI], 2));
    } else {
        TRY(read_block(d, BQ769X0_VC1_HI, b, sizeof(b)));
    }

    for (int ch = 0, i = 0; ch < BQ769X0_MAX_CELLS; ch++) {
        if (!(d->channels & (1u << ch)))
            continue;                        /* a shorted input */
        p = &b[ch * 2];
        out->cell_mv[i++] = bq769x0_cell_mv(bq769x0_adc_raw(p[0], p[1]),
                                            d->gain_uv, d->offset_mv);
    }

    /* BAT is full 16 bit too; masked like a cell result it wraps above ~25 V. */
    p = &b[BQ769X0_BAT_HI - BQ769X0_VC1_HI];
    out->pack_mv = bq769x0_pack_mv((uint16_t)((p[0] << 8) | p[1]),
                                   d->gain_uv, d->offset_mv, d->cells);

    p = &b[BQ769X0_TS1_HI - BQ769X0_VC1_HI];
    out->temperature_c = bq769x0_temperature_c(bq769x0_adc_raw(p[0], p[1]), d->beta);

    /* TS2 exists on the bq76930 and up, TS3 on the bq76940 (SLUSBK2I 8.5).
     * Each set of five keeps its top input wired, so a cell above VC5 or
     * VC10 says which part this is. */
    p = &b[BQ769X0_TS2_HI - BQ769X0_VC1_HI];
    out->temperature2_c = d->channels > 0x001F
        ? bq769x0_temperature_c(bq769x0_adc_raw(p[0], p[1]), d->beta) : NAN;
    p = &b[BQ769X0_TS3_HI - BQ769X0_VC1_HI];
    out->temperature3_c = d->channels > 0x03FF
        ? bq769x0_temperature_c(bq769x0_adc_raw(p[0], p[1]), d->beta) : NAN;

    /* The coulomb counter is signed and full 16 bit, not a masked ADC result. */
    p = &b[BQ769X0_CC_HI - BQ769X0_VC1_HI];
    out->current_ma = bq769x0_current_ma((int16_t)((p[0] << 8) | p[1]),
                                         d->shunt_mohm);

    out->status = st;

    /* CC has just been read, so consume CC_READY. ALERT is the OR of every
     * SYS_STAT bit (SLUSBK2I 8.3.1.3.4); left set, it holds ALERT high and
     * the pin is no use as an interrupt. Faults stay latched for the caller. */
    if (st & BQ769X0_STAT_CC_READY)
        TRY(reg_write(d, BQ769X0_SYS_STAT, BQ769X0_STAT_CC_READY));
    return 0;
}

battery_handle_ret bq769x0_read(bq769x0_handle_t d, bq769x0_data_t *out)
{
    if (!d || !out)
        return BATTERY_HANDLE_RET(-EINVAL);

    return BATTERY_HANDLE_RET(read_data(d, out));
}

battery_handle_ret bq769x0_cell_spread(bq769x0_handle_t d, int *min_mv, int *max_mv,
                                       int *spread_mv)
{
    bq769x0_data_t s;
    int lo, hi, err;

    if (!d)
        return BATTERY_HANDLE_RET(-EINVAL);

    err = read_data(d, &s);
    if (err)
        return BATTERY_HANDLE_RET(err);
    lo = hi = s.cell_mv[0];
    for (int i = 1; i < s.cells; i++) {
        if (s.cell_mv[i] < lo)
            lo = s.cell_mv[i];
        if (s.cell_mv[i] > hi)
            hi = s.cell_mv[i];
    }
    if (min_mv)
        *min_mv = lo;
    if (max_mv)
        *max_mv = hi;
    if (spread_mv)
        *spread_mv = hi - lo;
    return BATTERY_HANDLE_RET(0);
}

battery_handle_ret bq769x0_status(bq769x0_handle_t d, uint8_t *status)
{
    if (!d || !status)
        return BATTERY_HANDLE_RET(-EINVAL);

    return BATTERY_HANDLE_RET(reg_read(d, BQ769X0_SYS_STAT, status));
}

battery_handle_ret bq769x0_clear_faults(bq769x0_handle_t d, uint8_t mask)
{
    if (!d)
        return BATTERY_HANDLE_RET(-EINVAL);

    /* SYS_STAT is write-1-to-clear (SLUSBK2I Table 8-3). */
    return BATTERY_HANDLE_RET(reg_write(d, BQ769X0_SYS_STAT,
                                        (uint8_t)(mask & BQ769X0_STAT_FAULTS)));
}

/* ------------------------------------------------------- protection ------ */

/* Far past either window, but small enough that (mv - offset) * 1000 fits. */
static int clamp_mv(int mv)
{
    return mv < 0 ? 0 : mv > 10000 ? 10000 : mv;
}

static int set_ov_uv(struct bq769x0_t *d, int ov_mv, int uv_mv)
{
    if (uv_mv >= ov_mv) {
        battery_err(&d->dev, "undervoltage %d mV must be below overvoltage %d mV",
                    uv_mv, ov_mv);
        return -EINVAL;
    }
    ov_mv = clamp_mv(ov_mv);
    uv_mv = clamp_mv(uv_mv);
    /* The two thresholds reach different windows, and outside them the
     * register wraps -- an overvoltage cutoff that silently never trips. */
    if (!bq769x0_ov_trip_fits(ov_mv, d->gain_uv, d->offset_mv)) {
        battery_err(&d->dev, "overvoltage %d mV is outside the reachable window "
                             "(%d..%d mV at this gain)", ov_mv,
                    bq769x0_ov_trip_mv(0x00, d->gain_uv, d->offset_mv),
                    bq769x0_ov_trip_mv(0xFF, d->gain_uv, d->offset_mv));
        return -EINVAL;
    }
    if (!bq769x0_uv_trip_fits(uv_mv, d->gain_uv, d->offset_mv)) {
        battery_err(&d->dev, "undervoltage %d mV is outside the reachable window "
                             "(%d..%d mV at this gain)", uv_mv,
                    bq769x0_uv_trip_mv(0x00, d->gain_uv, d->offset_mv),
                    bq769x0_uv_trip_mv(0xFF, d->gain_uv, d->offset_mv));
        return -EINVAL;
    }

    TRY(reg_write(d, BQ769X0_OV_TRIP,
                  bq769x0_ov_trip_reg(ov_mv, d->gain_uv, d->offset_mv)));
    return reg_write(d, BQ769X0_UV_TRIP,
                     bq769x0_uv_trip_reg(uv_mv, d->gain_uv, d->offset_mv));
}

battery_handle_ret bq769x0_set_ov_uv(bq769x0_handle_t d, int ov_mv, int uv_mv)
{
    if (!d)
        return BATTERY_HANDLE_RET(-EINVAL);

    return BATTERY_HANDLE_RET(set_ov_uv(d, ov_mv, uv_mv));
}

static int get_ov_uv(struct bq769x0_t *d, int *ov_mv, int *uv_mv)
{
    uint8_t ov = 0, uv = 0;

    TRY(reg_read(d, BQ769X0_OV_TRIP, &ov));
    TRY(reg_read(d, BQ769X0_UV_TRIP, &uv));
    if (ov_mv)
        *ov_mv = bq769x0_ov_trip_mv(ov, d->gain_uv, d->offset_mv);
    if (uv_mv)
        *uv_mv = bq769x0_uv_trip_mv(uv, d->gain_uv, d->offset_mv);
    return 0;
}

battery_handle_ret bq769x0_get_ov_uv(bq769x0_handle_t d, int *ov_mv, int *uv_mv)
{
    if (!d)
        return BATTERY_HANDLE_RET(-EINVAL);

    return BATTERY_HANDLE_RET(get_ov_uv(d, ov_mv, uv_mv));
}

/*
 * PROTECT1..3 steps, SLUSBK2I Tables 8-9, 8-10 and 8-11. Thresholds are mV
 * across the shunt; RSNS selects the upper row for SCD and OCD together.
 */
static const int16_t scd_mv[2][8] = {
    { 22, 33, 44, 56, 67, 78, 89, 100 },                           /* RSNS 0 */
    { 44, 67, 89, 111, 133, 155, 178, 200 },                       /* RSNS 1 */
};
static const int16_t ocd_mv[2][16] = {
    { 8, 11, 14, 17, 19, 22, 25, 28, 31, 33, 36, 39, 42, 44, 47, 50 },
    { 17, 22, 28, 33, 39, 44, 50, 56, 61, 67, 72, 78, 83, 89, 94, 100 },
};
static const int16_t scd_us[4] = { 70, 100, 200, 400 };
static const int16_t ocd_ms[8] = { 8, 20, 40, 80, 160, 320, 640, 1280 };
static const int16_t ov_s[4]   = { 1, 2, 4, 8 };
static const int16_t uv_s[4]   = { 1, 4, 8, 16 };

/* The largest step not above want, or -1 if want is below them all. The
 * small allowance stops float rounding from pushing an exact step down one. */
static int step_at_or_below(const int16_t *steps, int n, float want)
{
    int i = -1;

    while (i + 1 < n && steps[i + 1] <= want + 0.01f)
        i++;
    return i;
}

static int set_protect(struct bq769x0_t *d, const bq769x0_protect_t *p)
{
    float scd, ocd, gap = 0.0f;
    int rsns = -1, st = -1, sd, ot = -1, od, ovd, uvd;
    uint8_t p1, p2;

    scd = p->scd_a * d->shunt_mohm;          /* amps times milliohms is mV */
    ocd = p->ocd_a * d->shunt_mohm;
    /* One RSNS bit serves both. Of the ranges where each request has a step
     * at or below it, take the one whose steps land closest in total; the
     * lower range on a tie. */
    for (int r = 0; r < 2; r++) {
        int s = step_at_or_below(scd_mv[r], 8, scd);
        int o = step_at_or_below(ocd_mv[r], 16, ocd);

        if (s < 0 || o < 0)
            continue;
        if (rsns < 0 || (scd - scd_mv[r][s]) + (ocd - ocd_mv[r][o]) < gap) {
            gap = (scd - scd_mv[r][s]) + (ocd - ocd_mv[r][o]);
            rsns = r;
            st = s;
            ot = o;
        }
    }
    sd  = step_at_or_below(scd_us, 4, (float)p->scd_delay_us);
    od  = step_at_or_below(ocd_ms, 8, (float)p->ocd_delay_ms);
    ovd = step_at_or_below(ov_s, 4, (float)p->ov_delay_s);
    uvd = step_at_or_below(uv_s, 4, (float)p->uv_delay_s);
    if (rsns < 0 || sd < 0 || od < 0 || ovd < 0 || uvd < 0) {
        battery_err(&d->dev, "protection below the smallest step: SCD %.1f mV %d us, "
                             "OCD %.1f mV %d ms, OV %d s, UV %d s",
                    (double)scd, p->scd_delay_us, (double)ocd, p->ocd_delay_ms,
                    p->ov_delay_s, p->uv_delay_s);
        return -EINVAL;
    }

    p1 = (uint8_t)((rsns ? BQ769X0_RSNS : 0) | sd << BQ769X0_SCD_D_SHIFT | st);
    p2 = (uint8_t)(od << BQ769X0_OCD_D_SHIFT | ot);
    /* RSNS in PROTECT1 doubles the OCD code in PROTECT2. Order the writes so
     * one lost halfway never pairs a code with a larger range than it was
     * chosen for: the code before a move to the upper range, the range
     * before a move to the lower. */
    if (rsns) {
        TRY(reg_write(d, BQ769X0_PROTECT2, p2));
        TRY(reg_write(d, BQ769X0_PROTECT1, p1));
    } else {
        TRY(reg_write(d, BQ769X0_PROTECT1, p1));
        TRY(reg_write(d, BQ769X0_PROTECT2, p2));
    }
    return reg_write(d, BQ769X0_PROTECT3, (uint8_t)(uvd << BQ769X0_UV_D_SHIFT |
                     ovd << BQ769X0_OV_D_SHIFT));
}

battery_handle_ret bq769x0_set_protect(bq769x0_handle_t d, const bq769x0_protect_t *p)
{
    if (!d || !p)
        return BATTERY_HANDLE_RET(-EINVAL);

    return BATTERY_HANDLE_RET(set_protect(d, p));
}

battery_handle_ret bq769x0_get_protect(bq769x0_handle_t d, bq769x0_protect_t *p)
{
    uint8_t r[3];     /* PROTECT1..3 */
    int rsns, err;

    if (!d || !p)
        return BATTERY_HANDLE_RET(-EINVAL);

    err = read_block(d, BQ769X0_PROTECT1, r, sizeof(r));
    if (err)
        return BATTERY_HANDLE_RET(err);
    rsns = (r[0] & BQ769X0_RSNS) != 0;
    p->scd_a        = scd_mv[rsns][r[0] & 0x07] / d->shunt_mohm;
    p->scd_delay_us = scd_us[(r[0] >> BQ769X0_SCD_D_SHIFT) & 0x03];
    p->ocd_a        = ocd_mv[rsns][r[1] & 0x0F] / d->shunt_mohm;
    p->ocd_delay_ms = ocd_ms[(r[1] >> BQ769X0_OCD_D_SHIFT) & 0x07];
    p->ov_delay_s   = ov_s[(r[2] >> BQ769X0_OV_D_SHIFT) & 0x03];
    p->uv_delay_s   = uv_s[(r[2] >> BQ769X0_UV_D_SHIFT) & 0x03];
    return BATTERY_HANDLE_RET(0);
}

static int set_balancing(struct bq769x0_t *d, uint16_t mask)
{
    uint16_t vc = 0;
    uint8_t regs[3];

    if (mask >> d->cells) {
        battery_err(&d->dev, "balancing mask 0x%04x names a cell above %d",
                    mask, d->cells);
        return -EINVAL;
    }

    /* mask counts cells, CELLBAL counts VC inputs, and with shorted inputs
     * the two differ. Neighbouring cells share a bleed path; balancing both
     * puts the resistors in series across one cell and can drive the shared
     * pin past its rating. The hardware will happily do it. The rule is per
     * set of five inputs, and it is about cells, not inputs: shorted inputs
     * are one node, so 3S cells 2 and 3 on VC2 and VC5 are neighbours
     * (SLUSBK2I 8.3.1.3.3). */
    for (int ch = 0, i = 0, prev = -1; ch < BQ769X0_MAX_CELLS; ch++) {
        if (!(d->channels & (1u << ch)))
            continue;
        if (i > 0 && ((mask >> (i - 1)) & 3) == 3 && prev / 5 == ch / 5) {
            battery_err(&d->dev, "balancing mask 0x%04x turns on neighbouring "
                                 "cells %d and %d", mask, i, i + 1);
            return -EINVAL;
        }
        if (mask & (1u << i))
            vc |= (uint16_t)(1u << ch);
        prev = ch;
        i++;
    }

    regs[0] = (uint8_t)(vc & 0x1F);
    regs[1] = (uint8_t)((vc >> 5) & 0x1F);
    regs[2] = (uint8_t)((vc >> 10) & 0x1F);
    for (int i = 0; i < 3; i++)
        TRY(reg_write(d, (uint8_t)(BQ769X0_CELLBAL1 + i), regs[i]));
    return 0;
}

battery_handle_ret bq769x0_set_balancing(bq769x0_handle_t d, uint16_t mask)
{
    if (!d)
        return BATTERY_HANDLE_RET(-EINVAL);

    return BATTERY_HANDLE_RET(set_balancing(d, mask));
}

static int set_fets(struct bq769x0_t *d, bool charge, bool discharge)
{
    uint8_t v = 0;

    TRY(reg_read(d, BQ769X0_SYS_CTRL2, &v));
    v = (uint8_t)(v & ~(BQ769X0_CHG_ON | BQ769X0_DSG_ON));
    if (charge)
        v |= BQ769X0_CHG_ON;
    if (discharge)
        v |= BQ769X0_DSG_ON;
    return reg_write(d, BQ769X0_SYS_CTRL2, v);
}

battery_handle_ret bq769x0_set_fets(bq769x0_handle_t d, bool charge, bool discharge)
{
    if (!d)
        return BATTERY_HANDLE_RET(-EINVAL);

    return BATTERY_HANDLE_RET(set_fets(d, charge, discharge));
}

uint8_t bq769x0_address(bq769x0_handle_t d) { return d ? d->address : 0; }
bool bq769x0_crc_enabled(bq769x0_handle_t d) { return d ? d->crc : false; }

/* ------------------------------------------------------- battery_dev ---- */

static int bq769x0_read_sample(struct battery_dev *bat, struct battery_sample *s)
{
    struct bq769x0_t *d = bat->priv;
    bq769x0_data_t data;

    TRY(read_data(d, &data));

    s->cells = data.cells;
    for (int i = 0; i < data.cells; i++)
        s->cell_v[i] = (float)data.cell_mv[i] / 1000.0f;
    s->pack_v        = (float)data.pack_mv / 1000.0f;
    s->soc_percent   = NAN;    /* a monitor, not a gauge */
    s->current_a     = data.current_ma / 1000.0f;
    s->temperature_c = data.temperature_c;
    for (size_t i = 0; i < STATUS_ALERT_COUNT; i++)
        if (data.status & status_alerts[i].status)
            s->alerts |= status_alerts[i].alert;
    return 0;
}

/* SYS_STAT is write-1-to-clear (SLUSBK2I Table 8-3). */
static int bq769x0_clear(struct battery_dev *bat, uint32_t alerts)
{
    struct bq769x0_t *d = bat->priv;
    uint8_t mask = 0;

    for (size_t i = 0; i < STATUS_ALERT_COUNT; i++)
        if (alerts & status_alerts[i].alert)
            mask |= status_alerts[i].status;
    if (!mask)
        return 0;               /* none this part raises, so none latched */
    return reg_write(d, BQ769X0_SYS_STAT, mask);
}

#endif /* CONFIG_BATTERY_BQ769X0 */
