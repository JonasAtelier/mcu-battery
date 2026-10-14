/*
 * A fake MAX17048 at register level, and the driver's cases against it,
 * shared by esp/test_max17048.c and stm/test_max17048.c:
 * esp-max17048-fuelgauge's test_driver.c on both platforms, its fake and its
 * cases unchanged but for the return codes, plus the struct battery_dev, the
 * presence probe and the bus errors.
 *
 * The fake keeps what the driver relies on, from the datasheet (19-6171 Rev
 * 7; pN is its page): the power-up defaults of p10-11 Table 2, STATUS.RI set
 * among them; writes to read-only registers ignored (p17); a POR on 0x5400
 * to CMD that resets the part and is not ACKed (p14). STATUS bits are written
 * like any register: the driver clears one by writing it 0 (p14).
 * Each platform file feeds the fake from its own bus calls --
 * fake_present(), fake_write() and fake_read_block() -- and provides:
 *
 *   RET(err)        what the handle API returns for a negative errno
 *   NACK_ERRNO      the errno of a transfer the part did not ACK
 *   NACK            RET(NACK_ERRNO), what the handle API returns for it
 *   I2C_BUS         the bus handle max17048_init() takes
 *   OPEN_DEVICES()  devices added to the bus and not yet removed
 */

static uint16_t regs[256];
static uint8_t  last_addr;          /* where the last transfer went */
static int      resets;             /* PORs the part went through */
static int      cmd_writes;         /* writes to CMD, of any value */
static int      status_writes;      /* writes to STATUS */
static bool     present;            /* something ACKs at 0x36 */
static int      fail_write_reg;     /* NACK writes to this register; -1 none */
static int      fail_read_reg;      /* NACK reads of this register; -1 none */
static int      xfers;
static int      live_allocs;        /* calloc()s not yet free()d */
static bool     fail_calloc;        /* the next calloc() returns NULL */

/* What a power-up, or a POR, leaves: p10-11 Table 2. VERSION survives. */
static void power_up_defaults(void)
{
    uint16_t version = regs[MAX17048_VERSION];

    memset(regs, 0, sizeof(regs));
    regs[MAX17048_VERSION] = version;
    regs[MAX17048_HIBRT]   = 0x8030;
    regs[MAX17048_CONFIG]  = 0x971C;
    regs[MAX17048_VALRT]   = 0x00FF;
    regs[MAX17048_VRESET]  = 0x9600;      /* 0x96__, the ID byte factory set */
    regs[MAX17048_STATUS]  = 0x0100;      /* 0x01__: RI, as a literal, so a
                                           * wrong MAX17048_STATUS_RI shows */
    regs[MAX17048_CMD]     = 0xFFFF;
}

static void chip_reset(void)
{
    regs[MAX17048_VERSION] = 0x0012;
    power_up_defaults();
    resets = cmd_writes = 0;
    present = true;
    fail_write_reg = fail_read_reg = -1;
}

static bool read_only(uint8_t reg)
{
    return reg == MAX17048_VCELL || reg == MAX17048_SOC ||
           reg == MAX17048_VERSION || reg == MAX17048_CRATE;
}

void *__real_calloc(size_t n, size_t size);
void __real_free(void *p);
void *__wrap_calloc(size_t n, size_t size);
void __wrap_free(void *p);

void *__wrap_calloc(size_t n, size_t size)
{
    void *p;

    if (fail_calloc) {
        fail_calloc = false;
        return NULL;
    }
    p = __real_calloc(n, size);
    if (p)
        live_allocs++;
    return p;
}

void __wrap_free(void *p)
{
    if (p)
        live_allocs--;
    __real_free(p);
}

static bool fake_present(uint8_t addr)
{
    return present && addr == MAX17048_ADDR;
}

/* frame is the register, then the word high byte first. */
static bool fake_write(uint8_t addr, const uint8_t *frame, size_t len)
{
    uint16_t v;

    xfers++;
    last_addr = addr;
    assert(len == 3 && "a register write is address then two bytes");
    if (!fake_present(addr) || frame[0] == fail_write_reg)
        return false;
    v = (uint16_t)((frame[1] << 8) | frame[2]);

    if (frame[0] == MAX17048_STATUS)
        status_writes++;
    if (frame[0] == MAX17048_CMD) {
        cmd_writes++;
        if (v == MAX17048_POR_VALUE) {
            /* Reset "when the last bit has been clocked in", and "the IC
             * does not respond with an I2C ACK" (p14). */
            power_up_defaults();
            resets++;
            return false;
        }
    }
    if (!read_only(frame[0]))
        regs[frame[0]] = v;
    return true;
}

static bool fake_read_block(uint8_t addr, uint8_t reg, uint8_t *rx, size_t rx_len)
{
    xfers++;
    last_addr = addr;
    assert(rx_len == 2);
    if (!fake_present(addr) || reg == fail_read_reg)
        return false;
    rx[0] = (uint8_t)(regs[reg] >> 8);
    rx[1] = (uint8_t)regs[reg];
    return true;
}

static max17048_handle_t open_ok(void)
{
    max17048_config_t c = { 0 };
    max17048_handle_t d = NULL;

    chip_reset();
    assert(max17048_init(I2C_BUS, &c, &d) == RET(0));
    return d;
}

static void test_probe(void)
{
    max17048_config_t c = { 0 };
    max17048_handle_t d = NULL;

    chip_reset();
    assert(max17048_init(I2C_BUS, &c, &d) == RET(0));
    assert(last_addr == MAX17048_ADDR);
    /* No POR: the part does not ACK one (p14), and it quick-starts (p9). */
    assert(resets == 0 && cmd_writes == 0);
    max17048_del(d);

    /* A bus stuck low or high is not a gauge. */
    chip_reset();
    regs[MAX17048_VERSION] = 0x0000;
    assert(max17048_init(I2C_BUS, &c, &d) == RET(-ENODEV));
    chip_reset();
    regs[MAX17048_VERSION] = 0xFFFF;
    assert(max17048_init(I2C_BUS, &c, &d) == RET(-ENODEV));

    /* VERSION is 0x001_ (p10 Table 2): any production version is taken,
     * anything outside the family -- a clone, another chip -- is not. */
    chip_reset();
    regs[MAX17048_VERSION] = 0x0010;
    assert(max17048_init(I2C_BUS, &c, &d) == RET(0));
    max17048_del(d);
    chip_reset();
    regs[MAX17048_VERSION] = 0x001F;
    assert(max17048_init(I2C_BUS, &c, &d) == RET(0));
    max17048_del(d);
    {
        static const uint16_t others[] = { 0x0020, 0x0011 | 0x0100, 0x000F, 0x8012 };

        for (size_t i = 0; i < sizeof(others) / sizeof(others[0]); i++) {
            chip_reset();
            regs[MAX17048_VERSION] = others[i];
            live_allocs = 0;
            assert(max17048_init(I2C_BUS, &c, &d) == RET(-ENODEV));
            assert(live_allocs == 0 && OPEN_DEVICES() == 0);
        }
    }

    chip_reset();
    c.empty_alert_pct = 33;
    assert(max17048_init(I2C_BUS, &c, &d) == RET(-EINVAL));
    c.empty_alert_pct = -1;
    assert(max17048_init(I2C_BUS, &c, &d) == RET(-EINVAL));
}

static void test_config_written_as_one_word(void)
{
    max17048_config_t c = { .rcomp = 0x5A, .empty_alert_pct = 10 };
    max17048_handle_t d = NULL;

    chip_reset();
    assert(max17048_init(I2C_BUS, &c, &d) == RET(0));
    /* Both halves must land in the same register without clobbering. */
    assert(max17048_get_rcomp(regs[MAX17048_CONFIG]) == 0x5A);
    assert(max17048_get_athd(regs[MAX17048_CONFIG]) == 10);
    max17048_del(d);

    /* Defaults when the caller leaves them at zero. */
    chip_reset();
    max17048_config_t z = { 0 };
    assert(max17048_init(I2C_BUS, &z, &d) == RET(0));
    assert(max17048_get_rcomp(regs[MAX17048_CONFIG]) == MAX17048_RCOMP_DEFAULT);
    assert(max17048_get_athd(regs[MAX17048_CONFIG]) == 4);
    max17048_del(d);
}

static void test_read(void)
{
    max17048_handle_t d = open_ok();
    max17048_data_t s;
    float v, p, h;

    regs[MAX17048_VCELL] = 47360;         /* ~3.7 V */
    regs[MAX17048_SOC]   = 25600 / 2;     /* 50% */
    regs[MAX17048_CRATE] = 0xFFCE;        /* -50 counts, discharging */

    assert(max17048_read(d, &s) == RET(0));
    assert(fabs(s.volts - 3.7f) < 1e-3);
    assert(fabs(s.soc_percent - 50.0f) < 1e-3);
    assert(s.crate_pct_hr < 0.0f);
    assert(!s.alert);

    assert(max17048_voltage(d, &v) == RET(0) && fabs(v - 3.7f) < 1e-3);
    assert(max17048_soc(d, &p) == RET(0) && fabs(p - 50.0f) < 1e-3);

    /* 50% at -10.4%/hr is a bit under 5 hours. */
    assert(max17048_hours_remaining(d, &h) == RET(0));
    assert(h > 4.0f && h < 5.5f);

    /* At rest, no estimate rather than a nonsense one. */
    regs[MAX17048_CRATE] = 0;
    assert(max17048_hours_remaining(d, &h) == RET(0) && h == 0.0f);

    max17048_del(d);
}

static void test_alert_clear_preserves_rcomp(void)
{
    max17048_handle_t d = open_ok();
    max17048_data_t s;

    regs[MAX17048_CONFIG] = max17048_config_with_rcomp(0x001C, 0x97)
                            | MAX17048_CONFIG_ALRT;
    assert(max17048_read(d, &s) == RET(0) && s.alert);

    assert(max17048_clear_alert(d) == RET(0));
    assert(!(regs[MAX17048_CONFIG] & MAX17048_CONFIG_ALRT));
    /* Clearing the alert must not wipe the compensation beside it. */
    assert(max17048_get_rcomp(regs[MAX17048_CONFIG]) == 0x97);
    assert(max17048_get_athd(regs[MAX17048_CONFIG]) == 4);

    assert(max17048_set_rcomp(d, 0x40) == RET(0));
    assert(max17048_get_rcomp(regs[MAX17048_CONFIG]) == 0x40);
    assert(max17048_get_athd(regs[MAX17048_CONFIG]) == 4);

    max17048_del(d);
}

static void test_quick_start_and_sleep(void)
{
    max17048_handle_t d = open_ok();

    assert(max17048_quick_start(d) == RET(0));
    assert(regs[MAX17048_MODE] & MAX17048_MODE_QUICK_START);

    assert(max17048_sleep(d, true) == RET(0));
    /* Sleep only works if MODE enabled it first. */
    assert(regs[MAX17048_MODE] & MAX17048_MODE_EN_SLEEP);
    assert(regs[MAX17048_CONFIG] & MAX17048_CONFIG_SLEEP);

    assert(max17048_sleep(d, false) == RET(0));
    assert(!(regs[MAX17048_CONFIG] & MAX17048_CONFIG_SLEEP));

    max17048_del(d);
}

static void test_null_arguments(void)
{
    max17048_data_t s;
    max17048_config_t c = { 0 };
    max17048_handle_t d = NULL;
    uint16_t u;
    float f;

    assert(max17048_init(I2C_BUS, NULL, &d) == RET(-EINVAL));
    assert(max17048_init(I2C_BUS, &c, NULL) == RET(-EINVAL));
    assert(max17048_read(NULL, &s) == RET(-EINVAL));
    assert(max17048_voltage(NULL, &f) == RET(-EINVAL));
    assert(max17048_soc(NULL, &f) == RET(-EINVAL));
    assert(max17048_hours_remaining(NULL, &f) == RET(-EINVAL));
    assert(max17048_status(NULL, &u) == RET(-EINVAL));
    assert(max17048_clear_alert(NULL) == RET(-EINVAL));
    assert(max17048_quick_start(NULL) == RET(-EINVAL));
    assert(max17048_set_rcomp(NULL, 0) == RET(-EINVAL));
    assert(max17048_sleep(NULL, true) == RET(-EINVAL));
    assert(max17048_del(NULL) == RET(-EINVAL));
    assert(max17048_reg_read(NULL, 0, &u) == RET(-EINVAL));
    assert(max17048_reg_write(NULL, 0, 0) == RET(-EINVAL));

    d = open_ok();
    assert(max17048_read(d, NULL) == RET(-EINVAL));
    assert(max17048_voltage(d, NULL) == RET(-EINVAL));
    assert(max17048_status(d, NULL) == RET(-EINVAL));
    assert(max17048_reg_read(d, 0, NULL) == RET(-EINVAL));
    max17048_del(d);
}

/* ------------------------------------------------- mcu-battery ---- */

static void test_absent_allocates_nothing(void)
{
    max17048_config_t c = { 0 };
    max17048_handle_t d = NULL;

    /* Nothing ACKs at 0x36: decided by the address, before any allocation. */
    chip_reset();
    present = false;
    live_allocs = 0;
    xfers = 0;
    assert(max17048_init(I2C_BUS, &c, &d) == RET(-ENXIO));
    assert(d == NULL && live_allocs == 0 && xfers == 0);

    chip_reset();
    assert(max17048_init(NULL, &c, &d) == RET(-EINVAL));
    fail_calloc = true;
    assert(max17048_init(I2C_BUS, &c, &d) == RET(-ENOMEM));
    assert(live_allocs == 0);

    /* A stuck VERSION, or a gauge that fails mid-init, leaks nothing. */
    regs[MAX17048_VERSION] = 0xFFFF;
    assert(max17048_init(I2C_BUS, &c, &d) == RET(-ENODEV));
    assert(live_allocs == 0 && OPEN_DEVICES() == 0);
    chip_reset();
    fail_write_reg = MAX17048_CONFIG;
    assert(max17048_init(I2C_BUS, &c, &d) == NACK);
    assert(live_allocs == 0 && OPEN_DEVICES() == 0);
    chip_reset();
    fail_write_reg = MAX17048_STATUS;
    assert(max17048_init(I2C_BUS, &c, &d) == NACK);
    assert(live_allocs == 0 && OPEN_DEVICES() == 0);
    fail_write_reg = -1;
}

static void test_bus_errors_come_back(void)
{
    max17048_handle_t d = open_ok();
    float f = 1.5f;
    uint16_t u = 7;

    fail_read_reg = MAX17048_SOC;
    assert(max17048_soc(d, &f) == NACK);
    assert(f == 1.5f && "a failed read leaves the output alone");
    fail_read_reg = MAX17048_STATUS;
    assert(max17048_status(d, &u) == NACK && u == 7);
    fail_read_reg = -1;
    fail_write_reg = MAX17048_MODE;
    assert(max17048_quick_start(d) == NACK);
    fail_write_reg = -1;
    max17048_del(d);
}

static void test_battery_dev(void)
{
    max17048_handle_t d;
    struct battery_dev *bat;
    struct battery_sample s;
    uint16_t rcomp_athd;

    assert(max17048_battery(NULL) == NULL);

    live_allocs = 0;
    d = open_ok();
    bat = max17048_battery(d);
    assert(bat && !strcmp(bat->name, "max17048"));

    /* The part powered up with RI set (p11 Table 2); init cleared it, so the
     * first read after a power-up is not a CHIP_RESET alert. */
    assert(!(regs[MAX17048_STATUS] & MAX17048_STATUS_RI));
    assert(battery_read(bat, &s) == 0 && s.alerts == 0);

    regs[MAX17048_VCELL] = 47360;         /* ~3.7 V */
    regs[MAX17048_SOC]   = 25600 / 2;     /* 50% */
    regs[MAX17048_STATUS] = MAX17048_STATUS_VL | MAX17048_STATUS_HD |
                            MAX17048_STATUS_SC | MAX17048_STATUS_ENVR;
    assert(battery_read(bat, &s) == 0);
    assert(s.cells == 1);
    assert(s.cell_v[0] == max17048_volts(47360) && s.pack_v == s.cell_v[0]);
    assert(s.cell_v[1] == 0.0f);
    assert(s.soc_percent == 50.0f);
    assert(isnan(s.current_a) && isnan(s.temperature_c));
    /* SC (1% moved) and ENVR (a setting) are not alerts. */
    assert(s.alerts == (BATTERY_ALERT_UNDERVOLTAGE | BATTERY_ALERT_LOW_CHARGE));

    regs[MAX17048_STATUS] = MAX17048_STATUS_VH | MAX17048_STATUS_RI;
    assert(battery_read(bat, &s) == 0);
    assert(s.alerts == (BATTERY_ALERT_OVERVOLTAGE | BATTERY_ALERT_CHIP_RESET));

    /* Clearing takes the named STATUS bits and ALRT, and nothing beside. */
    regs[MAX17048_STATUS] = MAX17048_STATUS_VH | MAX17048_STATUS_RI |
                            MAX17048_STATUS_ENVR;
    regs[MAX17048_CONFIG] |= MAX17048_CONFIG_ALRT;
    rcomp_athd = (uint16_t)(regs[MAX17048_CONFIG] & ~MAX17048_CONFIG_ALRT);
    assert(battery_clear_alerts(bat, BATTERY_ALERT_OVERVOLTAGE) == 0);
    assert(regs[MAX17048_STATUS] == (MAX17048_STATUS_RI | MAX17048_STATUS_ENVR));
    assert(regs[MAX17048_CONFIG] == rcomp_athd);
    assert(battery_clear_alerts(bat, BATTERY_ALERT_CHIP_RESET |
                                     BATTERY_ALERT_LOW_CHARGE) == 0);
    assert(regs[MAX17048_STATUS] == MAX17048_STATUS_ENVR);

    /* An alert this gauge never raises is not latched: nothing to write. */
    xfers = 0;
    assert(battery_clear_alerts(bat, BATTERY_ALERT_SHORT_CIRCUIT |
                                     BATTERY_ALERT_EXTERNAL) == 0);
    assert(xfers == 0);

    /* A failed read comes back as the bus's errno, the sample untouched. */
    s.cells = 42;
    fail_read_reg = MAX17048_STATUS;
    assert(battery_read(bat, &s) == NACK_ERRNO);
    assert(s.cells == 42);
    fail_read_reg = -1;

    max17048_del(d);
    assert(live_allocs == 0 && OPEN_DEVICES() == 0);
}

/* ------------------------------------------- power-up and MCU reboot ---- */

/*
 * The part powered up with the MCU: RI set, every register at its default.
 * Init configures it and clears RI, and sends no POR.
 */
static void test_init_after_power_up(void)
{
    max17048_config_t c = { .rcomp = 0x5A, .empty_alert_pct = 10 };
    max17048_handle_t d = NULL;
    struct battery_sample s;

    chip_reset();
    assert(regs[MAX17048_STATUS] & MAX17048_STATUS_RI);
    assert(max17048_init(I2C_BUS, &c, &d) == RET(0));
    assert(cmd_writes == 0 && resets == 0);
    assert(regs[MAX17048_CONFIG] ==
           max17048_config_with_athd(max17048_config_with_rcomp(0, 0x5A), 10));
    assert(!(regs[MAX17048_STATUS] & MAX17048_STATUS_RI));
    assert(battery_read(max17048_battery(d), &s) == 0 && s.alerts == 0);
    max17048_del(d);
}

/*
 * Only the MCU restarted: RI clear, the part has been gauging all along.
 * Init writes CONFIG and leaves the learned state alone -- no POR, no
 * quick-start, MODE, HIBRT, VALRT and STATUS as they were.
 */
static void test_init_after_mcu_reboot(void)
{
    max17048_config_t c = { .empty_alert_pct = 10 };
    max17048_handle_t d = NULL;

    chip_reset();
    regs[MAX17048_STATUS] = MAX17048_STATUS_HD;   /* RI cleared long ago */
    regs[MAX17048_SOC]    = 61 * 256;
    regs[MAX17048_HIBRT]  = 0x0000;               /* the app's own settings */
    regs[MAX17048_VALRT]  = 0xA0D2;
    regs[MAX17048_CONFIG] = max17048_config_with_rcomp(0x001C, 0x40);
    status_writes = 0;
    assert(max17048_init(I2C_BUS, &c, &d) == RET(0));
    assert(status_writes == 0);
    assert(cmd_writes == 0 && resets == 0);
    assert(regs[MAX17048_SOC] == 61 * 256);
    assert(regs[MAX17048_HIBRT] == 0x0000 && regs[MAX17048_VALRT] == 0xA0D2);
    assert(regs[MAX17048_MODE] == 0);
    /* STATUS not rewritten: a latched alert survives the MCU's reboot. */
    assert(regs[MAX17048_STATUS] == MAX17048_STATUS_HD);
    assert(max17048_get_rcomp(regs[MAX17048_CONFIG]) == MAX17048_RCOMP_DEFAULT);
    assert(max17048_get_athd(regs[MAX17048_CONFIG]) == 10);
    max17048_del(d);
}

/*
 * dingo_s3's cold boot as the original saw it: a POR written to CMD is not
 * ACKed (p14), which is why the original's init could only fail on a real
 * part. The raw write reports the NACK and the part reset all the same;
 * init then succeeds and clears the RI that reset left.
 */
static void test_por_is_not_acked(void)
{
    max17048_config_t c = { 0 };
    max17048_handle_t d;
    uint16_t status = 0;

    d = open_ok();
    assert(max17048_reg_write(d, MAX17048_CMD, MAX17048_POR_VALUE) == NACK);
    assert(resets == 1);
    assert(max17048_status(d, &status) == RET(0) &&
           (status & MAX17048_STATUS_RI));
    max17048_del(d);

    assert(max17048_init(I2C_BUS, &c, &d) == RET(0));
    assert(resets == 1 && cmd_writes == 1);   /* init sent none of its own */
    assert(!(regs[MAX17048_STATUS] & MAX17048_STATUS_RI));
    max17048_del(d);
}

/* Init cannot tell the two cases apart without STATUS: no read, no init. */
static void test_init_status_read_fails(void)
{
    max17048_config_t c = { 0 };
    max17048_handle_t d = NULL;

    chip_reset();
    fail_read_reg = MAX17048_STATUS;
    live_allocs = 0;
    assert(max17048_init(I2C_BUS, &c, &d) == NACK);
    assert(d == NULL && live_allocs == 0 && OPEN_DEVICES() == 0);
    assert(regs[MAX17048_CONFIG] == 0x971C);   /* nothing written */
    fail_read_reg = -1;
}

static void run_max17048_cases(void)
{
    test_probe();
    test_config_written_as_one_word();
    test_read();
    test_alert_clear_preserves_rcomp();
    test_quick_start_and_sleep();
    test_null_arguments();
    test_absent_allocates_nothing();
    test_bus_errors_come_back();
    test_battery_dev();
    test_init_after_power_up();
    test_init_after_mcu_reboot();
    test_por_is_not_acked();
    test_init_status_read_fails();
}
