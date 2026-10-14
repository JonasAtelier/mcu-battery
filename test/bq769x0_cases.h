/*
 * A fake bq769x0 at register level, and the driver's cases against it,
 * shared by esp/test_bq769x0.c and stm/test_bq769x0.c: esp-bq769x0-bms's
 * test_driver.c on both platforms, its fake and its cases unchanged but for
 * the return codes, plus the struct battery_dev and the bus errors.
 *
 * The fake models all four factory identities -- two addresses, each with or
 * without CRC -- because which one a part uses is not readable and the driver
 * has to find it by trying. It also checks the CRC on every byte when the
 * identity says it should, so a driver that computes it over the wrong bytes
 * fails here rather than on a bench. A bad CRC is NACKed (SLUSBK2I 8.3.1.4);
 * the rest is inferred from it: a CRC part ACKs a write without CRC and drops
 * it, and a read without CRC gets data and CRC bytes interleaved. A part
 * without CRC ACKs a write with one, and the CRC byte goes on to the next
 * register, read-only past CC_CFG. Each platform file feeds it from its own
 * bus calls -- fake_present(), fake_write() and fake_read_block() -- and
 * provides:
 *
 *   RET(err)        what the handle API returns for a negative errno
 *   NACK_ERRNO      the errno of a transfer the part did not ACK
 *   NACK            RET(NACK_ERRNO), what the handle API returns for it
 *   I2C_BUS         the bus handle bq769x0_init() takes
 *   OPEN_DEVICES()  devices added to the bus and not yet removed
 *   probe_fail      set: every address probe fails as a stuck bus would,
 *                   which the handle API returns as RET(-ETIMEDOUT)
 */

static uint8_t regs[256];
static uint8_t real_addr;      /* the identity this fake part actually has */
static bool    real_crc;
static int     crc_errors;
static int     xfers;              /* I2C transactions, reads and writes */
static int     corrupt_crc_at;     /* spoil the CRC of this read byte; -1 none */
static int     fail_write_reg;     /* refuse writes to this register; -1 none */
static bool    touched[256];       /* registers read since the last clear */
static uint8_t set_on_cc_read;     /* SYS_STAT bits that arrive as CC is read */
static uint8_t last_tx[3];         /* the last write frame, as sent */
static bool    probe_fail;
static int     live_allocs;        /* calloc()s not yet free()d */
static bool    fail_calloc;        /* the next calloc() returns NULL */
static bool    ignore_writes;      /* ACK writes but store nothing: not a bq769x0 */

static void chip_reset(uint8_t addr, bool crc)
{
    memset(regs, 0, sizeof(regs));
    regs[BQ769X0_ADCGAIN1]  = 0x0C;   /* high bits set  */
    regs[BQ769X0_ADCGAIN2]  = 0xE0;   /* low bits set -> gain 396 */
    regs[BQ769X0_ADCOFFSET] = 0x00;
    real_addr = addr;
    real_crc = crc;
    crc_errors = 0;
    corrupt_crc_at = -1;
    fail_write_reg = -1;
    set_on_cc_read = 0;
    probe_fail = false;
    ignore_writes = false;
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
    return addr == real_addr;
}

/* frame is the register, the data byte and, with CRC, its CRC. */
static bool fake_write(uint8_t addr, const uint8_t *frame, size_t len)
{
    xfers++;
    memcpy(last_tx, frame, len < 3 ? len : 3);

    if (addr != real_addr)
        return false;                        /* nothing acks at that address */

    if (real_crc) {
        uint8_t crc = 0;

        /* No CRC byte: every byte is ACKed, and the write dropped at STOP
         * -- inferred from SLUSBK2I 8.3.1.4, which only says a bad CRC is
         * NACKed. Nothing on the bus says it was lost. */
        if (len == 2)
            return true;
        if (len != 3)
            return false;
        crc = bq769x0_crc8(crc, (uint8_t)(real_addr << 1));
        crc = bq769x0_crc8(crc, frame[0]);
        crc = bq769x0_crc8(crc, frame[1]);
        if (crc != frame[2]) {
            crc_errors++;
            return false;
        }
    } else if (len != 2 && len != 3) {
        return false;
    }
    if (frame[0] == fail_write_reg)
        return false;                        /* a write lost on the bus */
    if (ignore_writes)
        return true;
    if (frame[0] == BQ769X0_SYS_STAT)
        regs[frame[0]] &= (uint8_t)~frame[1];  /* write 1 to clear, Table 8-3 */
    else
        regs[frame[0]] = frame[1];
    /* A part without CRC takes a third byte as data for the next register,
     * the address auto-incrementing; past CC_CFG they are read-only. */
    if (!real_crc && len == 3 && frame[0] + 1 <= BQ769X0_CC_CFG)
        regs[frame[0] + 1] = frame[2];
    return true;
}

static uint8_t fake_read(uint8_t reg)
{
    touched[reg] = true;
    if (reg == BQ769X0_CC_HI)
        regs[BQ769X0_SYS_STAT] |= set_on_cc_read;
    return regs[reg];
}

static bool fake_read_block(uint8_t addr, uint8_t reg, uint8_t *rx, size_t rx_len)
{
    xfers++;

    if (addr != real_addr)
        return false;

    /* The register address auto-increments across a block read. With CRC
     * every data byte is followed by its own CRC: the first covers the read
     * address byte and the data, each later one the data byte alone
     * (SLUSBK2I 8.3.1.4). */
    /* The part sends data and CRC bytes for as long as the master reads:
     * a read without CRC of one byte gets the data byte, of more it gets
     * data and CRC interleaved, as they come. */
    if (real_crc) {
        if (rx_len == 0)
            return false;
        for (size_t j = 0; j < rx_len; j++) {
            size_t i = j / 2;
            uint8_t crc = 0;

            if (j % 2 == 0) {
                rx[j] = fake_read((uint8_t)(reg + i));
                continue;
            }
            if (i == 0)
                crc = bq769x0_crc8(crc, (uint8_t)((real_addr << 1) | 1));
            rx[j] = bq769x0_crc8(crc, rx[j - 1]);
            if ((int)i == corrupt_crc_at)
                rx[j] ^= 0xFF;
        }
        return true;
    }
    if (rx_len == 0)
        return false;
    for (size_t i = 0; i < rx_len; i++)
        rx[i] = fake_read((uint8_t)(reg + i));
    return true;
}

static bq769x0_config_t base_cfg(void)
{
    bq769x0_config_t c = { .cells = 6, .shunt_mohm = 1.0f };
    return c;
}

static bq769x0_handle_t open_ok(void)
{
    bq769x0_config_t c = base_cfg();
    bq769x0_handle_t d = NULL;

    chip_reset(BQ769X0_ADDR_A, false);
    assert(bq769x0_init(I2C_BUS, &c, &d) == RET(0));
    return d;
}

static void put(int reg, uint16_t raw)      /* 14-bit result, high byte first */
{
    regs[reg]     = (uint8_t)((raw >> 8) & BQ769X0_ADC_MASK_HI);
    regs[reg + 1] = (uint8_t)(raw & 0xFF);
}

/* ------------------------------------------------- identity discovery ---- */

static void test_finds_all_four_identities(void)
{
    const uint8_t addrs[2] = { BQ769X0_ADDR_A, BQ769X0_ADDR_B };

    for (int a = 0; a < 2; a++) {
        for (int crc = 0; crc < 2; crc++) {
            bq769x0_config_t c = base_cfg();
            bq769x0_handle_t d = NULL;

            chip_reset(addrs[a], crc != 0);
            assert(bq769x0_init(I2C_BUS, &c, &d) == RET(0));
            assert(bq769x0_address(d) == addrs[a]);
            assert(bq769x0_crc_enabled(d) == (crc != 0));
            /* CC_CFG has to be left at the magic value the part needs. */
            assert(regs[BQ769X0_CC_CFG] == BQ769X0_CC_CFG_VALUE);
            /* No CRC mismatches on the identity that was settled on. */
            assert(crc_errors == 0);
            bq769x0_del(d);
            /* Every probe attempt must release the device it opened. */
            assert(OPEN_DEVICES() == 0);
        }
    }
}

static void test_absent_device(void)
{
    bq769x0_config_t c = base_cfg();
    bq769x0_handle_t d = NULL;

    chip_reset(0x55, false);              /* an address the driver never tries */
    assert(bq769x0_init(I2C_BUS, &c, &d) == RET(-ENXIO));
    assert(OPEN_DEVICES() == 0 && "a failed probe must not leak handles");
}

static void test_bad_config(void)
{
    bq769x0_config_t c;
    bq769x0_handle_t d = NULL;

    chip_reset(BQ769X0_ADDR_A, false);
    c = base_cfg(); c.cells = 2;
    assert(bq769x0_init(I2C_BUS, &c, &d) == RET(-EINVAL));
    c = base_cfg(); c.cells = 16;
    assert(bq769x0_init(I2C_BUS, &c, &d) == RET(-EINVAL));
    /* No shunt means no current; refuse rather than divide by zero later. */
    c = base_cfg(); c.shunt_mohm = 0.0f;
    assert(bq769x0_init(I2C_BUS, &c, &d) == RET(-EINVAL));
    assert(bq769x0_init(I2C_BUS, NULL, &d) == RET(-EINVAL));
    assert(bq769x0_init(I2C_BUS, &c, NULL) == RET(-EINVAL));
}

static void test_calibration_is_read(void)
{
    bq769x0_config_t c = base_cfg();
    bq769x0_handle_t d = NULL;

    /* Minimum gain. */
    chip_reset(BQ769X0_ADDR_A, false);
    regs[BQ769X0_ADCGAIN1] = 0x00;
    regs[BQ769X0_ADCGAIN2] = 0x00;
    regs[BQ769X0_ADCOFFSET] = 0x00;
    assert(bq769x0_init(I2C_BUS, &c, &d) == RET(0));
    put(BQ769X0_VC1_HI, 10000);
    {
        bq769x0_data_t s;
        assert(bq769x0_read(d, &s) == RET(0));
        assert(s.cell_mv[0] == bq769x0_cell_mv(10000, 365, 0));
    }
    bq769x0_del(d);

    /* Same counts, maximum gain and a negative offset: a different answer. */
    chip_reset(BQ769X0_ADDR_A, false);
    regs[BQ769X0_ADCGAIN1] = 0x0C;
    regs[BQ769X0_ADCGAIN2] = 0xE0;
    regs[BQ769X0_ADCOFFSET] = 0xFB;      /* -5 mV */
    assert(bq769x0_init(I2C_BUS, &c, &d) == RET(0));
    put(BQ769X0_VC1_HI, 10000);
    {
        bq769x0_data_t s;
        assert(bq769x0_read(d, &s) == RET(0));
        assert(s.cell_mv[0] == bq769x0_cell_mv(10000, 396, -5));
        assert(s.cell_mv[0] != bq769x0_cell_mv(10000, 365, 0));
    }
    bq769x0_del(d);
}

static void test_read(void)
{
    bq769x0_handle_t d = open_ok();
    bq769x0_data_t s;
    int lo, hi, spread;

    /* A 6S bq76930 has its cells on VC1, 2, 5, 6, 7 and 10. */
    for (int i = 0; i < 6; i++) {
        static const int vc[6] = { 1, 2, 5, 6, 7, 10 };

        put(BQ769X0_VC1_HI + (vc[i] - 1) * 2, (uint16_t)(9300 + i * 30));
    }
    put(BQ769X0_BAT_HI, 14000);
    put(BQ769X0_TS1_HI, (uint16_t)(1650.0f * 1000.0f / 382.0f));
    regs[BQ769X0_CC_HI] = 0xFF;          /* -256 counts, discharging */
    regs[BQ769X0_CC_HI + 1] = 0x00;
    regs[BQ769X0_SYS_STAT] = BQ769X0_STAT_CC_READY;

    assert(bq769x0_read(d, &s) == RET(0));
    assert(s.cells == 6);
    /* Only the configured cells are populated, the rest stay zero. */
    for (int i = 0; i < 6; i++)
        assert(s.cell_mv[i] > 3000 && s.cell_mv[i] < 4500);
    for (int i = 6; i < BQ769X0_CELLS_MAX; i++)
        assert(s.cell_mv[i] == 0);
    assert(s.pack_mv > 0);
    assert(s.current_ma < 0.0f && "a negative coulomb count is discharge");
    assert(fabs(s.temperature_c - 25.0f) < 1.5f);
    assert(s.status & BQ769X0_STAT_CC_READY);

    /* The spread is what tells you a cell is going bad. */
    assert(bq769x0_cell_spread(d, &lo, &hi, &spread) == RET(0));
    assert(hi > lo && spread == hi - lo);
    assert(spread > 0 && spread < 200);

    bq769x0_del(d);
}

static void test_pack_is_full_16_bit(void)
{
    bq769x0_config_t c = base_cfg();      /* 6 cells */
    bq769x0_handle_t d = NULL;
    bq769x0_data_t s;

    /* Gain 380: 365 + 8 from ADCGAIN1 bit 2 + 7 from ADCGAIN2 bits 7:5. */
    chip_reset(BQ769X0_ADDR_A, false);
    regs[BQ769X0_ADCGAIN1] = 0x04;
    regs[BQ769X0_ADCGAIN2] = 0xE0;
    assert(bq769x0_init(I2C_BUS, &c, &d) == RET(0));

    /* BAT is not a 14-bit result. 6S at 4.2 V is 0x40C2; masked like a cell
     * register it drops to 194 counts, a 294 mV pack. */
    regs[BQ769X0_BAT_HI] = 0x40;
    regs[BQ769X0_BAT_HI + 1] = 0xC2;
    assert(bq769x0_read(d, &s) == RET(0));
    assert(s.pack_mv == 25198);           /* 4 * 380 * 16578 / 1000 */

    /* Full scale: every high bit is data. */
    regs[BQ769X0_BAT_HI] = 0xFF;
    regs[BQ769X0_BAT_HI + 1] = 0xFF;
    assert(bq769x0_read(d, &s) == RET(0));
    assert(s.pack_mv == 99613);           /* 4 * 380 * 65535 / 1000 */

    bq769x0_del(d);
}

static void test_read_is_one_burst(void)
{
    bq769x0_config_t c = base_cfg();
    bq769x0_handle_t d = NULL;
    bq769x0_data_t s;

    /* 15 cells over CRC is the worst case: 37 transactions a pair at a time. */
    c.cells = 15;
    chip_reset(BQ769X0_ADDR_A, true);
    assert(bq769x0_init(I2C_BUS, &c, &d) == RET(0));
    for (int i = 0; i < 15; i++)
        put(BQ769X0_VC1_HI + i * 2, (uint16_t)(9000 + i * 20));
    regs[BQ769X0_BAT_HI] = 0x40;
    regs[BQ769X0_BAT_HI + 1] = 0xC2;
    put(BQ769X0_TS1_HI, (uint16_t)(1650.0f * 1000.0f / 382.0f));
    regs[BQ769X0_CC_HI] = 0xFF;          /* -256 counts */
    regs[BQ769X0_CC_HI + 1] = 0x00;

    /* Every result in one transaction, plus one for SYS_STAT. A high and low
     * byte are only one conversion when they arrive in the same read. */
    xfers = 0;
    assert(bq769x0_read(d, &s) == RET(0));
    assert(xfers == 2);
    for (int i = 0; i < 15; i++)
        assert(s.cell_mv[i] == bq769x0_cell_mv((uint16_t)(9000 + i * 20), 396, 0));
    assert(s.pack_mv == bq769x0_pack_mv(0x40C2, 396, 0, 15));
    assert(fabs(s.temperature_c - 25.0f) < 1.5f);
    assert(s.current_ma == bq769x0_current_ma(-256, 1.0f));

    /* Each byte of a block carries its own CRC; a bad one mid-block must be
     * caught, not only the first. */
    corrupt_crc_at = 20;
    assert(bq769x0_read(d, &s) == RET(-EBADMSG));
    corrupt_crc_at = -1;

    bq769x0_del(d);
}

static void test_bq76920_skips_upper_registers(void)
{
    /* VC6..VC15 (0x16..0x29), TS2 and TS3 (0x2E..0x31) are only valid on
     * the '30 and '40 (SLUSBK2I 8.5 notes). A bq76920 read goes around them;
     * BAT and TS1 between them are its own. */
    for (int n = 3; n <= 5; n++) {
        bq769x0_config_t c = base_cfg();
        bq769x0_handle_t d = NULL;
        bq769x0_data_t s;

        c.cells = n;
        chip_reset(BQ769X0_ADDR_A, false);
        assert(bq769x0_init(I2C_BUS, &c, &d) == RET(0));
        regs[BQ769X0_BAT_HI] = 0x40;
        regs[BQ769X0_BAT_HI + 1] = 0xC2;
        put(BQ769X0_TS1_HI, (uint16_t)(1650.0f * 1000.0f / 382.0f));
        regs[BQ769X0_CC_HI] = 0xFF;      /* -256 counts */
        memset(touched, 0, sizeof(touched));
        assert(bq769x0_read(d, &s) == RET(0));
        for (int r = 0x16; r <= 0x31; r++)
            assert(!touched[r] || (r >= BQ769X0_BAT_HI && r <= BQ769X0_TS1_HI + 1));
        /* and what it did read landed where the decoding expects it */
        assert(s.pack_mv == bq769x0_pack_mv(0x40C2, 396, 0, n));
        assert(fabs(s.temperature_c - 25.0f) < 1.5f);
        assert(s.current_ma == bq769x0_current_ma(-256, 1.0f));
        bq769x0_del(d);
    }
}

/* Every channel reads differently, so a cell's value says which VC input it
 * came from. */
static void put_all_channels(void)
{
    for (int ch = 0; ch < 15; ch++)
        put(BQ769X0_VC1_HI + ch * 2, (uint16_t)(5000 + ch * 100));
}
static int channel_mv(int vc)                /* vc is 1-based, like VC1 */
{
    return bq769x0_cell_mv((uint16_t)(5000 + (vc - 1) * 100), 396, 0);
}

static void test_cell_channels_match_datasheet(void)
{
    /* SLUSBK2I 9.1.8, Tables 9-2 (3-5), 9-3 (6-10) and 9-4 (11-15), read
     * column by column: the VC input under each cell, bottom up. */
    static const uint8_t wiring[16][16] = {
        [3]  = { 1, 2, 5 },
        [4]  = { 1, 2, 3, 5 },
        [5]  = { 1, 2, 3, 4, 5 },
        [6]  = { 1, 2, 5, 6, 7, 10 },
        [7]  = { 1, 2, 3, 5, 6, 7, 10 },
        [8]  = { 1, 2, 3, 5, 6, 7, 8, 10 },
        [9]  = { 1, 2, 3, 4, 5, 6, 7, 8, 10 },
        [10] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 },
        [11] = { 1, 2, 3, 5, 6, 7, 8, 10, 11, 12, 15 },
        [12] = { 1, 2, 3, 5, 6, 7, 8, 10, 11, 12, 13, 15 },
        [13] = { 1, 2, 3, 4, 5, 6, 7, 8, 10, 11, 12, 13, 15 },
        [14] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 15 },
        [15] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 },
    };

    for (int n = 3; n <= 15; n++) {
        bq769x0_config_t c = base_cfg();
        bq769x0_handle_t d = NULL;
        bq769x0_data_t s;

        /* The transcription itself: exactly n inputs listed. */
        assert(wiring[n][n - 1] != 0 && wiring[n][n] == 0);

        c.cells = n;
        chip_reset(BQ769X0_ADDR_A, false);
        assert(bq769x0_init(I2C_BUS, &c, &d) == RET(0));
        put_all_channels();
        assert(bq769x0_read(d, &s) == RET(0));
        for (int i = 0; i < n; i++)
            assert(s.cell_mv[i] == channel_mv(wiring[n][i]));
        bq769x0_del(d);
    }
}

static void test_cell_channel_map(void)
{
    bq769x0_config_t c = base_cfg();
    bq769x0_handle_t d = NULL;
    bq769x0_data_t s;

    /* 3S: VC3 and VC4 are shorted, so cell 3 is VC5 -- reading VC3 would
     * report a 0 V cell and miss the real one. */
    c.cells = 3;
    chip_reset(BQ769X0_ADDR_A, false);
    assert(bq769x0_init(I2C_BUS, &c, &d) == RET(0));
    put_all_channels();
    assert(bq769x0_read(d, &s) == RET(0));
    assert(s.cell_mv[2] == channel_mv(5));
    assert(s.cell_mv[3] == 0);

    /* Balancing is by cell and lands on the cell's own input. */
    assert(bq769x0_set_balancing(d, 0x0004) == RET(0));    /* cell 3 */
    assert(regs[BQ769X0_CELLBAL1] == 0x10);                 /* VC5 */
    /* Cells 2 and 3 sit on VC2 and VC5, but the shorted VC2..VC4 are one
     * node: they are neighbours all the same (SLUSBK2I 8.3.1.3.3). */
    assert(bq769x0_set_balancing(d, 0x0006) == RET(-EINVAL));
    assert(regs[BQ769X0_CELLBAL1] == 0x10);   /* the refused mask is not written */
    /* There is no cell 4. */
    assert(bq769x0_set_balancing(d, 0x0008) == RET(-EINVAL));
    bq769x0_del(d);

    /* A bq76940 with 9 cells is wired differently from a bq76930 with 9,
     * and the part cannot say which it is: cell_channels does. */
    c.cells = 9;
    c.cell_channels = 0x4E73;                               /* Table 9-4 */
    chip_reset(BQ769X0_ADDR_A, false);
    assert(bq769x0_init(I2C_BUS, &c, &d) == RET(0));
    put_all_channels();
    assert(bq769x0_read(d, &s) == RET(0));
    assert(s.cell_mv[6] == channel_mv(11));
    assert(s.cell_mv[8] == channel_mv(15));
    bq769x0_del(d);

    /* A mask naming a different number of inputs than cells is refused. */
    c.cell_channels = 0x4E77;                               /* ten inputs */
    assert(bq769x0_init(I2C_BUS, &c, &d) == RET(-EINVAL));
    c.cell_channels = 0x81FF;                               /* no VC16 */
    assert(bq769x0_init(I2C_BUS, &c, &d) == RET(-EINVAL));
}

static void test_ts2_ts3(void)
{
    const uint16_t raw25 = (uint16_t)(1650.0f * 1000.0f / 382.0f);
    const int cells[3] = { 15, 6, 3 };     /* a '40, a '30, a '20 */
    bq769x0_config_t c = base_cfg();
    bq769x0_handle_t d = NULL;
    bq769x0_data_t s;

    for (int k = 0; k < 3; k++) {
        c.cells = cells[k];
        chip_reset(BQ769X0_ADDR_A, false);
        assert(bq769x0_init(I2C_BUS, &c, &d) == RET(0));
        put(BQ769X0_TS2_HI, raw25);
        put(BQ769X0_TS3_HI, raw25);
        assert(bq769x0_read(d, &s) == RET(0));

        /* A part without the input has no temperature there, whatever its
         * register holds. */
        if (cells[k] > 5)
            assert(fabs(s.temperature2_c - 25.0f) < 1.5f);
        else
            assert(isnan(s.temperature2_c));
        if (cells[k] > 10)
            assert(fabs(s.temperature3_c - 25.0f) < 1.5f);
        else
            assert(isnan(s.temperature3_c));
        bq769x0_del(d);
    }
}

static void test_read_clears_cc_ready(void)
{
    bq769x0_handle_t d = open_ok();
    bq769x0_data_t s;

    /* ALERT is the OR of SYS_STAT (8.3.1.3.4). Left set, CC_READY holds it
     * high for good and it is useless as an interrupt. */
    regs[BQ769X0_SYS_STAT] = BQ769X0_STAT_CC_READY | BQ769X0_STAT_OV;
    assert(bq769x0_read(d, &s) == RET(0));
    assert(s.status == (BQ769X0_STAT_CC_READY | BQ769X0_STAT_OV));
    /* CC_READY is consumed; the fault stays latched for the caller. */
    assert(regs[BQ769X0_SYS_STAT] == BQ769X0_STAT_OV);

    /* One that arrives while CC is being read was never seen, so it must
     * survive for the next read rather than be cleared unread. */
    regs[BQ769X0_SYS_STAT] = 0;
    set_on_cc_read = BQ769X0_STAT_CC_READY;
    assert(bq769x0_read(d, &s) == RET(0));
    assert(!(s.status & BQ769X0_STAT_CC_READY));
    assert(regs[BQ769X0_SYS_STAT] == BQ769X0_STAT_CC_READY);

    bq769x0_del(d);
}

static void test_ov_uv(void)
{
    bq769x0_handle_t d = open_ok();
    int ov = 0, uv = 0;

    assert(bq769x0_set_ov_uv(d, 4200, 2800) == RET(0));
    assert(bq769x0_get_ov_uv(d, &ov, &uv) == RET(0));
    assert(abs(ov - 4200) < 40);
    assert(abs(uv - 2800) < 40);
    /* OV must come back above UV, which only holds if the implicit top bits
     * are right -- getting them equal is the bug this guards. */
    assert(ov > uv);

    assert(bq769x0_set_ov_uv(d, 2800, 4200) == RET(-EINVAL));
    /* Outside the reachable window, the register would wrap and the cutoff
     * would never trip. */
    assert(bq769x0_set_ov_uv(d, 9000, 2800) == RET(-EINVAL));
    assert(bq769x0_set_ov_uv(d, 4200, 500) == RET(-EINVAL));

    bq769x0_del(d);
}

static void test_protect(void)
{
    bq769x0_config_t c = base_cfg();
    bq769x0_handle_t d = NULL;
    bq769x0_protect_t p = { .scd_a = 120, .scd_delay_us = 150, .ocd_a = 40,
                            .ocd_delay_ms = 100, .ov_delay_s = 3, .uv_delay_s = 5 };
    bq769x0_protect_t back;

    /* 0.5 mOhm, so amps and millivolts differ and the shunt has to be used. */
    c.shunt_mohm = 0.5f;
    chip_reset(BQ769X0_ADDR_A, false);
    assert(bq769x0_init(I2C_BUS, &c, &d) == RET(0));

    /* Each goes down to the step at or below it: 60 mV -> 56, 150 us -> 100,
     * 20 mV -> 19, 100 ms -> 80, 3 s -> 2, 5 s -> 4. */
    assert(bq769x0_set_protect(d, &p) == RET(0));
    assert(regs[BQ769X0_PROTECT1] == 0x0B);
    assert(regs[BQ769X0_PROTECT2] == 0x34);
    assert(regs[BQ769X0_PROTECT3] == 0x50);
    assert(bq769x0_get_protect(d, &back) == RET(0));
    assert(back.scd_a == 112.0f && back.scd_delay_us == 100);
    assert(back.ocd_a == 38.0f && back.ocd_delay_ms == 80);
    assert(back.ov_delay_s == 2 && back.uv_delay_s == 4);

    /* RSNS moves SCD and OCD together. The range whose steps land closest to
     * both requests wins: 150 + 20 mV is 133 + 17 upper, not 100 + 19 lower. */
    p.scd_a = 300;
    assert(bq769x0_set_protect(d, &p) == RET(0));
    assert(regs[BQ769X0_PROTECT1] & BQ769X0_RSNS);
    assert(bq769x0_get_protect(d, &back) == RET(0));
    assert(back.scd_a == 266.0f && back.ocd_a == 34.0f);
    /* 105 + 20 mV is 100 + 19 lower, not 89 + 17 upper. */
    p.scd_a = 210;
    assert(bq769x0_set_protect(d, &p) == RET(0));
    assert(bq769x0_get_protect(d, &back) == RET(0));
    assert(back.scd_a == 200.0f && back.ocd_a == 38.0f);
    /* 40 + 60 mV only fits the lower range, where SCD has a step below 44. */
    p.scd_a = 80;
    p.ocd_a = 120;
    assert(bq769x0_set_protect(d, &p) == RET(0));
    assert(bq769x0_get_protect(d, &back) == RET(0));
    assert(back.scd_a == 66.0f && back.ocd_a == 100.0f);

    /* RSNS in PROTECT1 doubles the OCD code in PROTECT2. A write lost
     * halfway must never pair an OCD code with a larger range than the one
     * it was chosen for, in either direction. */
    p.scd_a = 120;
    p.ocd_a = 40;                                   /* lower: OCD 19 mV */
    assert(bq769x0_set_protect(d, &p) == RET(0));
    p.scd_a = 300;                                  /* going up */
    fail_write_reg = BQ769X0_PROTECT2;
    assert(bq769x0_set_protect(d, &p) != RET(0));
    fail_write_reg = -1;
    assert(bq769x0_get_protect(d, &back) == RET(0));
    assert(back.ocd_a <= 40.0f);
    assert(bq769x0_set_protect(d, &p) == RET(0));   /* upper: OCD 17 mV */
    p.scd_a = 120;                                  /* going down */
    fail_write_reg = BQ769X0_PROTECT1;
    assert(bq769x0_set_protect(d, &p) != RET(0));
    fail_write_reg = -1;
    assert(bq769x0_get_protect(d, &back) == RET(0));
    assert(back.ocd_a <= 40.0f);

    /* Below the smallest step there is nothing safe to round to: refuse and
     * write nothing. */
    p.scd_a = 120;
    p.ocd_a = 10;                                   /* 5 mV; the lowest is 8 */
    assert(bq769x0_set_protect(d, &p) == RET(-EINVAL));
    assert(regs[BQ769X0_PROTECT1] & BQ769X0_RSNS);  /* still the last set */
    p.ocd_a = 40;
    p.scd_delay_us = 50;
    assert(bq769x0_set_protect(d, &p) == RET(-EINVAL));

    assert(bq769x0_set_protect(NULL, &p) == RET(-EINVAL));
    assert(bq769x0_set_protect(d, NULL) == RET(-EINVAL));
    assert(bq769x0_get_protect(d, NULL) == RET(-EINVAL));
    bq769x0_del(d);
}

static void test_balancing_refuses_adjacent(void)
{
    bq769x0_handle_t d = open_ok();

    /* 6S: cells 1, 3, 5 are VC1, VC5 and VC7. */
    assert(bq769x0_set_balancing(d, 0x0015) == RET(0));
    assert(regs[BQ769X0_CELLBAL1] == 0x11);
    assert(regs[BQ769X0_CELLBAL2] == 0x02);

    assert(bq769x0_set_balancing(d, 0x0000) == RET(0));
    assert(regs[BQ769X0_CELLBAL1] == 0x00);
    assert(regs[BQ769X0_CELLBAL2] == 0x00);

    /* Adjacent cells would put two bleed resistors across one cell. */
    assert(bq769x0_set_balancing(d, 0x0003) == RET(-EINVAL));
    assert(bq769x0_set_balancing(d, 0x001F) == RET(-EINVAL));
    /* and the refused mask must not have been written */
    assert(regs[BQ769X0_CELLBAL1] == 0x00);

    /* Cells 3 and 4 are VC5 and VC6: adjacent, but in different sets of
     * five, which the datasheet does not restrict (SLUSBK2I 8.3.1.3.3). */
    assert(bq769x0_set_balancing(d, 0x000C) == RET(0));
    assert(regs[BQ769X0_CELLBAL1] == 0x10);
    assert(regs[BQ769X0_CELLBAL2] == 0x01);

    bq769x0_del(d);
}

static void test_fets_and_faults(void)
{
    bq769x0_handle_t d = open_ok();
    uint8_t st = 0;

    regs[BQ769X0_SYS_CTRL2] = BQ769X0_CC_EN;
    assert(bq769x0_set_fets(d, true, true) == RET(0));
    assert(regs[BQ769X0_SYS_CTRL2] & BQ769X0_CHG_ON);
    assert(regs[BQ769X0_SYS_CTRL2] & BQ769X0_DSG_ON);
    /* Coulomb counting must survive a FET change. */
    assert(regs[BQ769X0_SYS_CTRL2] & BQ769X0_CC_EN);

    assert(bq769x0_set_fets(d, false, true) == RET(0));
    assert(!(regs[BQ769X0_SYS_CTRL2] & BQ769X0_CHG_ON));
    assert(regs[BQ769X0_SYS_CTRL2] & BQ769X0_DSG_ON);

    regs[BQ769X0_SYS_STAT] = BQ769X0_STAT_OV | BQ769X0_STAT_OCD;
    assert(bq769x0_status(d, &st) == RET(0));
    assert(st == (BQ769X0_STAT_OV | BQ769X0_STAT_OCD));
    assert(bq769x0_clear_faults(d, BQ769X0_STAT_FAULTS) == RET(0));
    assert(regs[BQ769X0_SYS_STAT] == 0);

    /* Teardown must not open the pack. */
    regs[BQ769X0_SYS_CTRL2] = BQ769X0_CHG_ON | BQ769X0_DSG_ON;
    bq769x0_del(d);
    assert(regs[BQ769X0_SYS_CTRL2] & BQ769X0_DSG_ON);
}

static void test_init_keeps_fets(void)
{
    bq769x0_config_t c = base_cfg();
    bq769x0_handle_t d = NULL;

    /* The MCU rebooted, the part did not: SYS_CTRL2 still holds the FETs
     * on. An init that clears DSG_ON browns out an MCU fed from the pack. */
    chip_reset(BQ769X0_ADDR_A, false);
    regs[BQ769X0_SYS_CTRL2] = BQ769X0_CHG_ON | BQ769X0_DSG_ON | BQ769X0_DELAY_DIS;
    assert(bq769x0_init(I2C_BUS, &c, &d) == RET(0));
    assert(regs[BQ769X0_SYS_CTRL2] == (BQ769X0_CHG_ON | BQ769X0_DSG_ON |
                                       BQ769X0_DELAY_DIS | BQ769X0_CC_EN));
    bq769x0_del(d);
}

static void test_del_keeps_protection(void)
{
    bq769x0_handle_t d = open_ok();

    /* OV and UV protection run on the ADC; teardown must not switch it off
     * and leave the FETs on with no cutoff. */
    assert(regs[BQ769X0_SYS_CTRL1] & BQ769X0_ADC_EN);
    bq769x0_del(d);
    assert(regs[BQ769X0_SYS_CTRL1] & BQ769X0_ADC_EN);
}

static void test_crc_frames_by_hand(void)
{
    bq769x0_config_t c = base_cfg();
    bq769x0_handle_t d = NULL;
    uint8_t reg = BQ769X0_CC_CFG, rx[4];

    chip_reset(BQ769X0_ADDR_A, true);
    assert(bq769x0_init(I2C_BUS, &c, &d) == RET(0));

    /* Worked out by hand -- CRC-8, x^8 + x^2 + x + 1, from 0 -- rather than
     * with bq769x0_crc8, so the fake and the driver cannot share a mistake.
     * CC_CFG = 0x19 at 0x08 goes out as 0x0B 0x19 and a CRC over the write
     * address 0x10, 0x0B and 0x19; after each byte it is 0x70, 0x66, 0x7A. */
    assert(bq769x0_reg_write(d, BQ769X0_CC_CFG, 0x19) == RET(0));
    assert(last_tx[0] == 0x0B && last_tx[1] == 0x19 && last_tx[2] == 0x7A);

    /* Two bytes back from there, both 0x19. The first CRC covers the read
     * address 0x11 and the data: 0x77, then 0x0D. The second covers the data
     * alone: 0x4F. */
    regs[BQ769X0_CC_CFG + 1] = 0x19;
    assert(fake_read_block(BQ769X0_ADDR_A, reg, rx, 4));
    assert(rx[0] == 0x19 && rx[1] == 0x0D && rx[2] == 0x19 && rx[3] == 0x4F);

    bq769x0_del(d);
}

static void test_null_arguments(void)
{
    bq769x0_data_t s;
    bq769x0_handle_t d = NULL;
    uint8_t v;
    int a, b, c;

    assert(bq769x0_read(NULL, &s) == RET(-EINVAL));
    assert(bq769x0_cell_spread(NULL, &a, &b, &c) == RET(-EINVAL));
    assert(bq769x0_status(NULL, &v) == RET(-EINVAL));
    assert(bq769x0_clear_faults(NULL, 0) == RET(-EINVAL));
    assert(bq769x0_set_ov_uv(NULL, 4200, 2800) == RET(-EINVAL));
    assert(bq769x0_get_ov_uv(NULL, &a, &b) == RET(-EINVAL));
    assert(bq769x0_set_balancing(NULL, 0) == RET(-EINVAL));
    assert(bq769x0_set_fets(NULL, true, true) == RET(-EINVAL));
    assert(bq769x0_del(NULL) == RET(-EINVAL));
    assert(bq769x0_reg_read(NULL, 0, &v) == RET(-EINVAL));
    assert(bq769x0_reg_write(NULL, 0, 0) == RET(-EINVAL));
    assert(bq769x0_address(NULL) == 0);
    assert(!bq769x0_crc_enabled(NULL));

    d = open_ok();
    assert(bq769x0_read(d, NULL) == RET(-EINVAL));
    assert(bq769x0_status(d, NULL) == RET(-EINVAL));
    assert(bq769x0_reg_read(d, 0, NULL) == RET(-EINVAL));
    bq769x0_del(d);
}

/* ------------------------------------------------- mcu-battery ---- */

static void test_absent_allocates_nothing(void)
{
    bq769x0_config_t c = base_cfg();
    bq769x0_handle_t d = NULL;

    chip_reset(0x55, false);
    live_allocs = 0;
    assert(bq769x0_init(I2C_BUS, &c, &d) == RET(-ENXIO));
    assert(live_allocs == 0 && d == NULL);

    /* A stuck bus is not an absent part, and says so. */
    chip_reset(BQ769X0_ADDR_A, false);
    probe_fail = true;
    assert(bq769x0_init(I2C_BUS, &c, &d) == RET(-ETIMEDOUT));
    assert(live_allocs == 0 && d == NULL);
    probe_fail = false;

    assert(bq769x0_init(NULL, &c, &d) == RET(-EINVAL));
    fail_calloc = true;
    assert(bq769x0_init(I2C_BUS, &c, &d) == RET(-ENOMEM));

    /* Something ACKs at 0x08 but keeps no CC_CFG, with or without CRC: not
     * this part. Both tries are let go of again, and nothing is left. */
    chip_reset(BQ769X0_ADDR_A, false);
    ignore_writes = true;
    assert(bq769x0_init(I2C_BUS, &c, &d) == RET(-ENODEV));
    assert(live_allocs == 0 && OPEN_DEVICES() == 0);
    ignore_writes = false;

    /* A part that answers, then fails mid-init, leaks nothing either. */
    fail_write_reg = BQ769X0_SYS_CTRL1;
    assert(bq769x0_init(I2C_BUS, &c, &d) == NACK);
    assert(live_allocs == 0 && OPEN_DEVICES() == 0);
    fail_write_reg = -1;
}

static void test_bus_errors_come_back(void)
{
    bq769x0_handle_t d = open_ok();
    bq769x0_data_t s;

    /* A NACKed transfer is the platform's own code, not a generic failure. */
    fail_write_reg = BQ769X0_SYS_CTRL2;
    assert(bq769x0_set_fets(d, true, true) == NACK);
    fail_write_reg = -1;
    bq769x0_del(d);

    /* A CRC mismatch is RET(-EBADMSG): ESP_ERR_INVALID_CRC on ESP-IDF. */
    {
        bq769x0_config_t c = base_cfg();

        chip_reset(BQ769X0_ADDR_B, true);
        assert(bq769x0_init(I2C_BUS, &c, &d) == RET(0));
        corrupt_crc_at = 0;
        assert(bq769x0_read(d, &s) == RET(-EBADMSG));
        corrupt_crc_at = -1;
        bq769x0_del(d);
    }
}

static void test_battery_dev(void)
{
    bq769x0_config_t c = base_cfg();
    bq769x0_handle_t d = NULL;
    struct battery_dev *bat;
    struct battery_sample s;

    assert(bq769x0_battery(NULL) == NULL);

    chip_reset(BQ769X0_ADDR_A, false);
    live_allocs = 0;
    assert(bq769x0_init(I2C_BUS, &c, &d) == RET(0));
    bat = bq769x0_battery(d);
    assert(bat && !strcmp(bat->name, "bq769x0"));

    /* A 6S bq76930 has its cells on VC1, 2, 5, 6, 7 and 10. */
    for (int i = 0; i < 6; i++) {
        static const int vc[6] = { 1, 2, 5, 6, 7, 10 };

        put(BQ769X0_VC1_HI + (vc[i] - 1) * 2, (uint16_t)(10000 + i * 10));
    }
    regs[BQ769X0_BAT_HI] = 0x40;
    regs[BQ769X0_BAT_HI + 1] = 0xC2;
    put(BQ769X0_TS1_HI, (uint16_t)(1650.0f * 1000.0f / 382.0f));
    regs[BQ769X0_CC_HI] = 0xFF;          /* -256 counts, discharging */
    regs[BQ769X0_CC_HI + 1] = 0x00;
    regs[BQ769X0_SYS_STAT] = BQ769X0_STAT_CC_READY | BQ769X0_STAT_UV |
                             BQ769X0_STAT_SCD | BQ769X0_STAT_OVRD_ALERT;

    assert(battery_read(bat, &s) == 0);
    assert(s.cells == 6);
    for (int i = 0; i < 6; i++)
        assert(s.cell_v[i] ==
               (float)bq769x0_cell_mv((uint16_t)(10000 + i * 10), 396, 0) / 1000.0f);
    assert(s.cell_v[6] == 0.0f);
    assert(s.pack_v == (float)bq769x0_pack_mv(0x40C2, 396, 0, 6) / 1000.0f);
    assert(s.current_a == bq769x0_current_ma(-256, 1.0f) / 1000.0f);
    assert(s.current_a < 0.0f && "discharge is negative");
    assert(fabsf(s.temperature_c - 25.0f) < 1.5f);
    assert(isnan(s.soc_percent));
    /* CC_READY is not an alert; the faults are, each its own bit. */
    assert(s.alerts == (BATTERY_ALERT_UNDERVOLTAGE | BATTERY_ALERT_SHORT_CIRCUIT |
                        BATTERY_ALERT_EXTERNAL));

    regs[BQ769X0_SYS_STAT] = BQ769X0_STAT_OV | BQ769X0_STAT_OCD |
                             BQ769X0_STAT_DEVICE_XREADY;
    assert(battery_read(bat, &s) == 0);
    assert(s.alerts == (BATTERY_ALERT_OVERVOLTAGE | BATTERY_ALERT_OVERCURRENT |
                        BATTERY_ALERT_CHIP_FAULT));
    regs[BQ769X0_SYS_STAT] = BQ769X0_STAT_CC_READY;
    assert(battery_read(bat, &s) == 0 && s.alerts == 0);
    regs[BQ769X0_SYS_STAT] = BQ769X0_STAT_OVRD_ALERT;
    assert(battery_read(bat, &s) == 0 && s.alerts == BATTERY_ALERT_EXTERNAL);
    regs[BQ769X0_SYS_STAT] = BQ769X0_STAT_OV | BQ769X0_STAT_OCD |
                             BQ769X0_STAT_DEVICE_XREADY;

    /* Clearing writes exactly those SYS_STAT bits back, write 1 to clear. */
    assert(battery_clear_alerts(bat, BATTERY_ALERT_OVERVOLTAGE |
                                     BATTERY_ALERT_CHIP_FAULT) == 0);
    assert(last_tx[0] == BQ769X0_SYS_STAT &&
           last_tx[1] == (BQ769X0_STAT_OV | BQ769X0_STAT_DEVICE_XREADY));
    assert(regs[BQ769X0_SYS_STAT] == BQ769X0_STAT_OCD);
    /* An alert this part never raises is not latched: nothing to write. */
    xfers = 0;
    assert(battery_clear_alerts(bat, BATTERY_ALERT_LOW_CHARGE |
                                     BATTERY_ALERT_CHIP_RESET) == 0);
    assert(xfers == 0);

    /* A failed read -- here the CC_READY write at its end -- comes back as
     * the bus's errno and leaves the sample alone. */
    s.cells = 42;
    fail_write_reg = BQ769X0_SYS_STAT;
    regs[BQ769X0_SYS_STAT] = BQ769X0_STAT_CC_READY;
    assert(battery_read(bat, &s) == NACK_ERRNO);
    assert(s.cells == 42);
    fail_write_reg = -1;

    bq769x0_del(d);
    assert(live_allocs == 0);
}

/*
 * The MCU reset, the part did not: CC_CFG still holds 0x19. A CRC part
 * ACKs a write without CRC and drops it, and answers a one-byte read without
 * CRC with the data byte, so writing 0x19 and reading 0x19 back would pass on
 * the no-CRC identity, and every later write would be dropped.
 */
static void test_crc_part_after_mcu_reset(void)
{
    const uint8_t addrs[2] = { BQ769X0_ADDR_A, BQ769X0_ADDR_B };

    for (int a = 0; a < 2; a++) {
        for (int crc = 0; crc < 2; crc++) {
            bq769x0_config_t c = base_cfg();
            bq769x0_handle_t d = NULL;

            chip_reset(addrs[a], crc != 0);
            regs[BQ769X0_CC_CFG] = BQ769X0_CC_CFG_VALUE;
            assert(bq769x0_init(I2C_BUS, &c, &d) == RET(0));
            assert(bq769x0_address(d) == addrs[a]);
            assert(bq769x0_crc_enabled(d) == (crc != 0));
            assert(regs[BQ769X0_CC_CFG] == BQ769X0_CC_CFG_VALUE);
            /* init's own writes landed, and a later one does too */
            assert(regs[BQ769X0_SYS_CTRL1] == (BQ769X0_ADC_EN | BQ769X0_TEMP_SEL));
            assert(regs[BQ769X0_SYS_CTRL2] & BQ769X0_CC_EN);
            assert(bq769x0_set_fets(d, true, true) == RET(0));
            assert((regs[BQ769X0_SYS_CTRL2] & (BQ769X0_CHG_ON | BQ769X0_DSG_ON)) ==
                   (BQ769X0_CHG_ON | BQ769X0_DSG_ON));
            bq769x0_del(d);
            assert(OPEN_DEVICES() == 0);
        }
    }
}

/* Far outside the windows, and far enough that (mv - offset) * 1000 would
 * overflow an int: refused, not undefined (UBSan aborts on the latter). */
static void test_ov_uv_extremes(void)
{
    bq769x0_handle_t d = open_ok();

    assert(bq769x0_set_ov_uv(d, INT_MAX, 2800) == RET(-EINVAL));
    assert(bq769x0_set_ov_uv(d, 4200, INT_MIN) == RET(-EINVAL));
    assert(bq769x0_set_ov_uv(d, INT_MAX, INT_MAX - 1) == RET(-EINVAL));
    assert(bq769x0_set_ov_uv(d, INT_MIN + 1, INT_MIN) == RET(-EINVAL));
    assert(regs[BQ769X0_OV_TRIP] == 0 && regs[BQ769X0_UV_TRIP] == 0);
    bq769x0_del(d);
}

static void run_bq769x0_cases(void)
{
    test_finds_all_four_identities();
    test_absent_device();
    test_bad_config();
    test_calibration_is_read();
    test_read();
    test_pack_is_full_16_bit();
    test_read_is_one_burst();
    test_bq76920_skips_upper_registers();
    test_cell_channels_match_datasheet();
    test_cell_channel_map();
    test_read_clears_cc_ready();
    test_ts2_ts3();
    test_ov_uv();
    test_protect();
    test_balancing_refuses_adjacent();
    test_fets_and_faults();
    test_init_keeps_fets();
    test_del_keeps_protection();
    test_crc_frames_by_hand();
    test_null_arguments();
    test_absent_allocates_nothing();
    test_bus_errors_come_back();
    test_battery_dev();
    test_crc_part_after_mcu_reset();
    test_ov_uv_extremes();
}
