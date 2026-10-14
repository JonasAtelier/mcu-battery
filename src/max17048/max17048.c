/*
 * MAX17048 fuel gauge, from esp-max17048-fuelgauge 81df2e3: the same calls
 * on the battery bus layer, so on both platforms, plus the struct
 * battery_dev the handle hands out. The calls and their checks are the
 * original's; what changed is listed in docs/chips.md.
 */
#include "battery_priv.h"

#if CONFIG_BATTERY_MAX17048

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "battery_log.h"
#include "max17048.h"
#include "max17048_math.h"
#include "max17048_regs.h"

#define I2C_TIMEOUT_MS   100
#define DEFAULT_ATHD_PCT 4     /* CONFIG.ATHD's POR value, 0x1C (p12) */

#define TRY(expr) do { int _e = (expr); if (_e) return _e; } while (0)

struct max17048_t {
    struct battery_bus bus;
    struct battery_dev dev;
};

/* For the lines logged before there is a handle. */
static const struct battery_dev max17048_log_dev = { .name = "max17048" };

static int max17048_read_sample(struct battery_dev *bat, struct battery_sample *s);
static int max17048_clear(struct battery_dev *bat, uint32_t alerts);

static const struct battery_ops max17048_ops = {
    .read         = max17048_read_sample,
    .clear_alerts = max17048_clear,
};

/* STATUS bits and the battery alert each one raises. */
static const struct {
    uint16_t status;
    uint32_t alert;
} status_alerts[] = {
    { MAX17048_STATUS_VH, BATTERY_ALERT_OVERVOLTAGE },
    { MAX17048_STATUS_VL, BATTERY_ALERT_UNDERVOLTAGE },
    { MAX17048_STATUS_HD, BATTERY_ALERT_LOW_CHARGE },
    { MAX17048_STATUS_RI, BATTERY_ALERT_CHIP_RESET },
};

#define STATUS_ALERT_COUNT (sizeof(status_alerts) / sizeof(status_alerts[0]))

/* ---------------------------------------------------------------- I2C ---- */

static int reg_read(struct max17048_t *d, uint8_t reg, uint16_t *value)
{
    uint8_t b[2];

    TRY(d->bus.ops->transfer(&d->bus, reg, NULL, 0, b, 2));
    *value = max17048_word(b);      /* every register is 16 bit, big endian */
    return 0;
}

static int reg_write(struct max17048_t *d, uint8_t reg, uint16_t value)
{
    uint8_t b[2] = { (uint8_t)(value >> 8), (uint8_t)value };

    return d->bus.ops->transfer(&d->bus, reg, b, 2, NULL, 0);
}

battery_handle_ret max17048_reg_read(max17048_handle_t d, uint8_t reg, uint16_t *value)
{
    if (!d || !value)
        return BATTERY_HANDLE_RET(-EINVAL);

    return BATTERY_HANDLE_RET(reg_read(d, reg, value));
}

battery_handle_ret max17048_reg_write(max17048_handle_t d, uint8_t reg, uint16_t value)
{
    if (!d)
        return BATTERY_HANDLE_RET(-EINVAL);

    return BATTERY_HANDLE_RET(reg_write(d, reg, value));
}

/* Write 0 to the STATUS bits in mask, keeping the rest (p14). */
static int clear_status(struct max17048_t *d, uint16_t mask)
{
    uint16_t status;

    TRY(reg_read(d, MAX17048_STATUS, &status));
    return reg_write(d, MAX17048_STATUS, (uint16_t)(status & ~mask));
}

/* -------------------------------------------------------------- init ----- */

static int init(battery_i2c_bus bus, const max17048_config_t *cfg,
                max17048_handle_t *out)
{
    max17048_handle_t d;
    uint16_t version = 0, status = 0, config;
    int athd, err;

    if (!bus || !cfg || !out)
        return -EINVAL;

    athd = cfg->empty_alert_pct ? cfg->empty_alert_pct : DEFAULT_ATHD_PCT;
    if (athd < 1 || athd > 32) {
        battery_err(&max17048_log_dev,
                    "empty alert threshold must be 1..32%%, not %d", athd);
        return -EINVAL;
    }

    /* Presence by address, before anything is allocated: a NACK in the
     * middle of a transfer is not "nothing there" on either platform. */
    err = battery_i2c_probe(bus, MAX17048_ADDR, I2C_TIMEOUT_MS);
    if (err) {
        battery_err(&max17048_log_dev, "nothing answers at 0x%02x",
                    MAX17048_ADDR);
        return err;
    }

    d = calloc(1, sizeof(*d));
    if (!d)
        return -ENOMEM;

    /* scl_hz 0 is 400 kHz, the attach's default, as it was the original's. */
    err = battery_i2c_attach(&d->bus, bus, MAX17048_ADDR, cfg->scl_hz,
                             I2C_TIMEOUT_MS);
    if (err) {
        free(d);
        return err;
    }

    /* VERSION is the only identity this part offers: 0x001_, the low nibble
     * a production version (p10 Table 2). A stuck bus, another chip at 0x36
     * -- or a clone or a later revision outside 0x001_ -- is refused. */
    err = reg_read(d, MAX17048_VERSION, &version);
    if (err)
        goto fail;
    if ((version & MAX17048_VERSION_MASK) != MAX17048_VERSION_FAMILY) {
        battery_err(&max17048_log_dev,
                    "no MAX17048 at 0x%02x: VERSION reads 0x%04x, not 0x001_",
                    MAX17048_ADDR, version);
        err = -ENODEV;
        goto fail;
    }

    /*
     * No power-on reset, unlike esp-max17048-fuelgauge, which sent one on
     * every init. The datasheet speaks against it twice. "The IC does not
     * respond with an I2C ACK after this command sequence" (p14, CMD
     * Register), so the write reads as a failure. And "POR includes a
     * quick-start, so only use it when the battery is fully relaxed" (p9,
     * Power-On Reset): on every MCU boot, under load or not, it threw the
     * learned state away.
     *
     * STATUS.RI says which case this is: "RI (reset indicator) is set when
     * the device powers up. Any time this bit is set, the IC is not
     * configured, so the model should be loaded and the bit should be
     * cleared" (p14). Set, the part is in its power-up defaults, which is
     * what a POR would have made it; clear, only the MCU restarted and the
     * part kept gauging. Either way CONFIG is written below, and RI cleared.
     */
    err = reg_read(d, MAX17048_STATUS, &status);
    if (err)
        goto fail;

    /* RCOMP and the alert threshold share CONFIG, so build the word rather
     * than writing either half on its own. */
    config = 0;
    config = max17048_config_with_rcomp(config,
                                cfg->rcomp ? cfg->rcomp : MAX17048_RCOMP_DEFAULT);
    config = max17048_config_with_athd(config, athd);
    err = reg_write(d, MAX17048_CONFIG, config);
    if (err)
        goto fail;

    /* The part is configured now, so RI is not news for the caller: clear
     * it, or the first read after every power-up reports
     * BATTERY_ALERT_CHIP_RESET. Left alone when already clear: rewriting
     * STATUS could clear an alert that arrived in between. */
    if (status & MAX17048_STATUS_RI) {
        err = clear_status(d, MAX17048_STATUS_RI);
        if (err)
            goto fail;
    }

    d->dev.name = "max17048";
    d->dev.ops  = &max17048_ops;
    d->dev.priv = d;

    battery_info(&d->dev, "version 0x%04x, %s, RCOMP 0x%02x, empty alert at %d%%",
                 version, (status & MAX17048_STATUS_RI) ? "powered up" : "kept its state",
                 max17048_get_rcomp(config), max17048_get_athd(config));

    *out = d;
    return 0;

fail:
    battery_bus_detach(&d->bus);
    free(d);
    return err;
}

battery_handle_ret max17048_init(battery_i2c_bus bus, const max17048_config_t *cfg,
                                 max17048_handle_t *out)
{
    return BATTERY_HANDLE_RET(init(bus, cfg, out));
}

battery_handle_ret max17048_del(max17048_handle_t d)
{
    if (!d)
        return BATTERY_HANDLE_RET(-EINVAL);

    battery_bus_detach(&d->bus);
    free(d);
    return BATTERY_HANDLE_RET(0);
}

struct battery_dev *max17048_battery(max17048_handle_t d)
{
    return d ? &d->dev : NULL;
}

/* -------------------------------------------------------------- read ----- */

battery_handle_ret max17048_voltage(max17048_handle_t d, float *volts)
{
    uint16_t raw;
    int err;

    if (!d || !volts)
        return BATTERY_HANDLE_RET(-EINVAL);

    err = reg_read(d, MAX17048_VCELL, &raw);
    if (!err)
        *volts = max17048_volts(raw);
    return BATTERY_HANDLE_RET(err);
}

battery_handle_ret max17048_soc(max17048_handle_t d, float *percent)
{
    uint16_t raw;
    int err;

    if (!d || !percent)
        return BATTERY_HANDLE_RET(-EINVAL);

    err = reg_read(d, MAX17048_SOC, &raw);
    if (!err)
        *percent = max17048_soc_percent(raw);
    return BATTERY_HANDLE_RET(err);
}

static int read_data(struct max17048_t *d, max17048_data_t *out)
{
    uint16_t v, s, c, cfg;

    TRY(reg_read(d, MAX17048_VCELL, &v));
    TRY(reg_read(d, MAX17048_SOC, &s));
    TRY(reg_read(d, MAX17048_CRATE, &c));
    TRY(reg_read(d, MAX17048_CONFIG, &cfg));

    out->volts        = max17048_volts(v);
    out->soc_percent  = max17048_soc_percent(s);
    out->crate_pct_hr = max17048_crate_pct_per_hour(c);
    out->alert        = (cfg & MAX17048_CONFIG_ALRT) != 0;
    return 0;
}

battery_handle_ret max17048_read(max17048_handle_t d, max17048_data_t *out)
{
    if (!d || !out)
        return BATTERY_HANDLE_RET(-EINVAL);

    return BATTERY_HANDLE_RET(read_data(d, out));
}

battery_handle_ret max17048_hours_remaining(max17048_handle_t d, float *hours)
{
    max17048_data_t s;
    int err;

    if (!d || !hours)
        return BATTERY_HANDLE_RET(-EINVAL);

    err = read_data(d, &s);
    if (!err)
        *hours = max17048_hours_from_rate(s.soc_percent, s.crate_pct_hr);
    return BATTERY_HANDLE_RET(err);
}

battery_handle_ret max17048_status(max17048_handle_t d, uint16_t *status)
{
    if (!d || !status)
        return BATTERY_HANDLE_RET(-EINVAL);

    return BATTERY_HANDLE_RET(reg_read(d, MAX17048_STATUS, status));
}

/* ALRT is write-0-to-clear and lives beside RCOMP, so read first rather
 * than writing a whole word and losing the compensation. */
static int clear_alrt(struct max17048_t *d)
{
    uint16_t cfg;

    TRY(reg_read(d, MAX17048_CONFIG, &cfg));
    return reg_write(d, MAX17048_CONFIG, (uint16_t)(cfg & ~MAX17048_CONFIG_ALRT));
}

battery_handle_ret max17048_clear_alert(max17048_handle_t d)
{
    if (!d)
        return BATTERY_HANDLE_RET(-EINVAL);

    return BATTERY_HANDLE_RET(clear_alrt(d));
}

battery_handle_ret max17048_quick_start(max17048_handle_t d)
{
    if (!d)
        return BATTERY_HANDLE_RET(-EINVAL);

    battery_warn(&d->dev, "quick start: only meaningful on a resting pack");
    return BATTERY_HANDLE_RET(reg_write(d, MAX17048_MODE, MAX17048_MODE_QUICK_START));
}

battery_handle_ret max17048_set_rcomp(max17048_handle_t d, uint8_t rcomp)
{
    uint16_t cfg;
    int err;

    if (!d)
        return BATTERY_HANDLE_RET(-EINVAL);

    err = reg_read(d, MAX17048_CONFIG, &cfg);
    if (!err)
        err = reg_write(d, MAX17048_CONFIG, max17048_config_with_rcomp(cfg, rcomp));
    return BATTERY_HANDLE_RET(err);
}

static int set_sleep(struct max17048_t *d, bool sleep)
{
    uint16_t cfg;

    /* Sleep has to be enabled in MODE before the CONFIG bit does anything. */
    if (sleep)
        TRY(reg_write(d, MAX17048_MODE, MAX17048_MODE_EN_SLEEP));

    TRY(reg_read(d, MAX17048_CONFIG, &cfg));
    cfg = sleep ? (uint16_t)(cfg | MAX17048_CONFIG_SLEEP)
                : (uint16_t)(cfg & ~MAX17048_CONFIG_SLEEP);
    return reg_write(d, MAX17048_CONFIG, cfg);
}

battery_handle_ret max17048_sleep(max17048_handle_t d, bool sleep)
{
    if (!d)
        return BATTERY_HANDLE_RET(-EINVAL);

    return BATTERY_HANDLE_RET(set_sleep(d, sleep));
}

/* ------------------------------------------------------- battery_dev ---- */

static int max17048_read_sample(struct battery_dev *bat, struct battery_sample *s)
{
    struct max17048_t *d = bat->priv;
    uint16_t v, soc, status;

    TRY(reg_read(d, MAX17048_VCELL, &v));
    TRY(reg_read(d, MAX17048_SOC, &soc));
    TRY(reg_read(d, MAX17048_STATUS, &status));

    s->cells         = 1;
    s->cell_v[0]     = max17048_volts(v);
    s->pack_v        = s->cell_v[0];
    s->soc_percent   = max17048_soc_percent(soc);
    s->current_a     = NAN;    /* no sense resistor: the gauge models the cell */
    s->temperature_c = NAN;
    for (size_t i = 0; i < STATUS_ALERT_COUNT; i++)
        if (status & status_alerts[i].status)
            s->alerts |= status_alerts[i].alert;
    return 0;
}

/*
 * The STATUS flags named, then CONFIG.ALRT, which any of them may have set.
 * Each is cleared by writing it 0, "Clear the corresponding bit after
 * servicing the alert" (p14), as ALRT is (p12).
 */
static int max17048_clear(struct battery_dev *bat, uint32_t alerts)
{
    struct max17048_t *d = bat->priv;
    uint16_t mask = 0;

    for (size_t i = 0; i < STATUS_ALERT_COUNT; i++)
        if (alerts & status_alerts[i].alert)
            mask |= status_alerts[i].status;
    if (!mask)
        return 0;               /* none this gauge raises, so none latched */

    TRY(clear_status(d, mask));
    return clear_alrt(d);
}

#endif /* CONFIG_BATTERY_MAX17048 */
