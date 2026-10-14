/*
 * bq769x0 register addresses and bit fields.
 *
 * Checked against TI's datasheet, SLUSBK2I rev I (March 2022), committed as
 * docs/bq769x0.pdf: addresses and bits against the register map (8.5, Tables
 * 8-3 to 8-23), the I2C identities against the device comparison table (5).
 * Section and table numbers sit beside the constants that need more.
 *
 * The register map also agrees with the original cross-check, two LibreSolar
 * implementations: bq769x0-arduino-library and the bq769x0 driver in
 * bms-firmware. One organisation, and the part is in neither the Linux nor
 * the Zephyr mainline tree, so where they and the datasheet differ, the
 * datasheet wins.
 */
#pragma once

/*
 * Four possible identities: two addresses, each with or without CRC. Which
 * one a part uses is set at the factory and not readable, so the driver tries
 * all four. The CRC column of SLUSBK2I 5 says which order codes use CRC.
 */
#define BQ769X0_ADDR_A         0x08   /* SLUSBK2I 5, Device Comparison Table */
#define BQ769X0_ADDR_B         0x18

/* Addresses: the register map, SLUSBK2I 8.5, and each register's own table. */
#define BQ769X0_SYS_STAT       0x00   /* Table 8-3 */
#define BQ769X0_CELLBAL1       0x01   /* cells 1-5, Table 8-4 */
#define BQ769X0_CELLBAL2       0x02   /* cells 6-10, Table 8-5 */
#define BQ769X0_CELLBAL3       0x03   /* cells 11-15, Table 8-6 */
#define BQ769X0_SYS_CTRL1      0x04   /* Table 8-7 */
#define BQ769X0_SYS_CTRL2      0x05   /* Table 8-8 */
#define BQ769X0_PROTECT1       0x06   /* short circuit in discharge, Table 8-9 */
#define BQ769X0_PROTECT2       0x07   /* overcurrent in discharge, Table 8-10 */
#define BQ769X0_PROTECT3       0x08   /* OV and UV delays, Table 8-11 */
#define BQ769X0_OV_TRIP        0x09   /* Table 8-12 */
#define BQ769X0_UV_TRIP        0x0A   /* Table 8-13 */
#define BQ769X0_CC_CFG         0x0B   /* Table 8-14 */
#define BQ769X0_VC1_HI         0x0C   /* 15 cell registers, 2 bytes each, Table 8-15 */
#define BQ769X0_BAT_HI         0x2A   /* pack voltage, Table 8-16 */
#define BQ769X0_TS1_HI         0x2C   /* three thermistor inputs, Table 8-17 */
#define BQ769X0_TS2_HI         0x2E   /* bq76930/40 only, SLUSBK2I Table 8-18 */
#define BQ769X0_TS3_HI         0x30   /* bq76940 only, Table 8-19 */
#define BQ769X0_CC_HI          0x32   /* coulomb counter, signed, Table 8-20 */
#define BQ769X0_ADCGAIN1       0x50   /* Table 8-21 */
#define BQ769X0_ADCOFFSET      0x51   /* Table 8-23 */
#define BQ769X0_ADCGAIN2       0x59   /* Table 8-22 */

#define BQ769X0_MAX_CELLS      15     /* bq76940, SLUSBK2I 5 */

/*
 * CC_CFG should be programmed to 0x19 at startup "for optimal performance"
 * (SLUSBK2I Table 8-14), which gives no reason and no alternative. Its reset
 * value is 0x00. Storing and reading back both values is how the driver tells
 * which of the four address/CRC combinations is live; see probe_identity().
 */
#define BQ769X0_CC_CFG_VALUE   0x19

/* ---- SYS_STAT (0x00), write 1 to clear: the BQ769X0_STAT_* bits are public,
 * in include/bq769x0.h, because callers test bq769x0_data_t.status ---- */

/* ---- SYS_CTRL1 (0x04), SLUSBK2I Table 8-7; bits 6:5 and 2 are reserved ---- */
#define BQ769X0_SHUT_B         0x01
#define BQ769X0_SHUT_A         0x02
#define BQ769X0_TEMP_SEL       0x08   /* 0 = die temperature, 1 = TS thermistors */
#define BQ769X0_ADC_EN         0x10
#define BQ769X0_LOAD_PRESENT   0x80

/* ---- SYS_CTRL2 (0x05), SLUSBK2I Table 8-8; bits 4:2 are reserved ---- */
#define BQ769X0_CHG_ON         0x01   /* charge FET */
#define BQ769X0_DSG_ON         0x02   /* discharge FET */
#define BQ769X0_CC_ONESHOT     0x20
#define BQ769X0_CC_EN          0x40   /* continuous coulomb counting, 250 ms */
#define BQ769X0_DELAY_DIS      0x80

/* ---- PROTECT1..3 (0x06..0x08), SLUSBK2I Tables 8-9, 8-10, 8-11 ---- */
#define BQ769X0_RSNS           0x80   /* PROTECT1: upper SCD and OCD range */
#define BQ769X0_SCD_D_SHIFT    3      /* PROTECT1 bits 4:3; 2:0 threshold */
#define BQ769X0_OCD_D_SHIFT    4      /* PROTECT2 bits 6:4; 3:0 threshold */
#define BQ769X0_OV_D_SHIFT     4      /* PROTECT3 bits 5:4 */
#define BQ769X0_UV_D_SHIFT     6      /* PROTECT3 bits 7:6 */

/* Result registers are 14 bit; the top two bits of the high byte are not data
 * (SLUSBK2I Tables 8-15, 8-17 to 8-19: bits 7:6 of each _HI read 0). */
#define BQ769X0_ADC_MASK_HI    0x3F
