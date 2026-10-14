/*
 * MAX17048 register addresses and bit fields.
 *
 * Checked against Maxim's datasheet, 19-6171 Rev 7 (11/16), committed as
 * docs/max17048.pdf (gitignored); "pN" is its printed page number. Addresses
 * against the register summary, p10-11 Table 2, bits against each register's
 * figure. Every value here also appears in the two drivers this one was
 * first cross-checked against, which agree: the Linux kernel's
 * drivers/power/supply/max17040_battery.c and Zephyr's
 * drivers/fuel_gauge/max17048/.
 *
 * All registers are 16 bit, big endian, read and written as a pair: "8-bit
 * writes cause no effect" (p10), and the first data byte lands at the
 * register address, the second at the next (p17, Write Data Protocol).
 */
#pragma once

/* 7-bit; "fixed to 0x6C (write)/0x6D (read)", 0110110 (p16, Slave Address) */
#define MAX17048_ADDR        0x36

/* p10-11 Table 2 */
#define MAX17048_VCELL       0x02   /* cell voltage */
#define MAX17048_SOC         0x04   /* state of charge */
#define MAX17048_MODE        0x06   /* quick-start, enable sleep */
#define MAX17048_VERSION     0x08   /* production version, 0x001_ */
#define MAX17048_HIBRT       0x0A   /* hibernate thresholds */
#define MAX17048_CONFIG      0x0C   /* RCOMP, sleep, alert threshold */
#define MAX17048_VALRT       0x14   /* voltage alert window */
#define MAX17048_CRATE       0x16   /* charge rate */
#define MAX17048_VRESET      0x18   /* reset voltage */
#define MAX17048_CHIP_ID     0x19   /* the ID byte, p13 Figure 12 */
#define MAX17048_STATUS      0x1A
#define MAX17048_TABLE       0x40   /* custom model, 0x40 to 0x7F */
#define MAX17048_CMD         0xFE   /* write the POR value to reset */

/*
 * VERSION reads 0x001_ (p10 Table 2): the low nibble is the production
 * version, the rest fixed. Anything else is not a MAX17048/MAX17049.
 */
#define MAX17048_VERSION_MASK      0xFFF0
#define MAX17048_VERSION_FAMILY    0x0010

/*
 * Writing this to CMD resets the part "as if power had been removed", and
 * "the IC does not respond with an I2C ACK after this command sequence"
 * (p14, CMD Register). The driver never sends it; see max17048.c init().
 */
#define MAX17048_POR_VALUE   0x5400

/* ---- MODE (0x06), p11 Figure 8: high byte bits 6, 5, 4 ---- */
#define MAX17048_MODE_QUICK_START  0x4000
#define MAX17048_MODE_EN_SLEEP     0x2000
#define MAX17048_MODE_HIB_STAT     0x1000   /* read only */

/* ---- CONFIG (0x0C), p12 Figure 10 ----
 * High byte is RCOMP, the battery compensation. Low byte holds the flags. */
#define MAX17048_CONFIG_RCOMP_SHIFT 8
#define MAX17048_CONFIG_SLEEP       0x0080
#define MAX17048_CONFIG_ALSC        0x0040   /* alert on SoC change */
#define MAX17048_CONFIG_ALRT        0x0020   /* alert asserted, write 0 to clear (p12) */
#define MAX17048_CONFIG_ATHD_MASK   0x001F   /* empty alert threshold */

/* "The POR value of RCOMP is 0x97" (p12); CONFIG resets to 0x971C (p11
 * Table 2). Correct for a typical lithium polymer cell at room temperature. */
#define MAX17048_RCOMP_DEFAULT      0x97

/* ---- STATUS (0x1A), p14 Figure 13: high byte bits 0 to 6 ----
 * "Clear the corresponding bit after servicing the alert" (p14): written 0,
 * the other bits written back as read. */
#define MAX17048_STATUS_RI   0x0100   /* reset indicator; set at power-up */
#define MAX17048_STATUS_VH   0x0200   /* voltage high */
#define MAX17048_STATUS_VL   0x0400   /* voltage low */
#define MAX17048_STATUS_VR   0x0800   /* voltage reset */
#define MAX17048_STATUS_HD   0x1000   /* SoC low */
#define MAX17048_STATUS_SC   0x2000   /* SoC change of 1% */
#define MAX17048_STATUS_ENVR 0x4000   /* enable voltage reset alert */
