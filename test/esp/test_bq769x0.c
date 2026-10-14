/*
 * Host-side check of the BQ769x0 driver on ESP-IDF: the cases are
 * ../bq769x0_cases.h; this file feeds the fake from the i2c_master calls,
 * the way IDF 5.x reports each outcome, and checks the esp_err_t codes the
 * handle API returns.
 */
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#include "battery.h"
#include "battery_priv.h"
#include "bq769x0.h"
#include "bq769x0_math.h"
#include "bq769x0_regs.h"

#define RET(err)	battery_esp_err(err)
/* IDF 5.x returns ESP_ERR_INVALID_STATE for a transfer the device NACKs. */
#define NACK_ERRNO	(-EBUSY)
#define NACK		RET(NACK_ERRNO)

static int bus_tag;
#define I2C_BUS		((i2c_master_bus_handle_t)(void *)&bus_tag)

static int devices_open;
#define OPEN_DEVICES()	devices_open

#include "../bq769x0_cases.h"

/* ------------------------------------------------ the IDF side of it --- */

struct i2c_master_dev_t {
	bool used;
	uint16_t address;
	uint32_t scl_hz;
};

static struct i2c_master_dev_t devs[4];

esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus, uint16_t address,
			   int xfer_timeout_ms)
{
	assert(bus == I2C_BUS && xfer_timeout_ms > 0);
	if (probe_fail)
		return ESP_ERR_TIMEOUT;
	return fake_present((uint8_t)address) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus,
				    const i2c_device_config_t *cfg,
				    i2c_master_dev_handle_t *out)
{
	assert(bus == I2C_BUS);
	for (size_t i = 0; i < sizeof(devs) / sizeof(devs[0]); i++) {
		if (!devs[i].used) {
			devs[i] = (struct i2c_master_dev_t){
				.used = true,
				.address = cfg->device_address,
				.scl_hz = cfg->scl_speed_hz,
			};
			devices_open++;
			*out = &devs[i];
			return ESP_OK;
		}
	}
	return ESP_ERR_NO_MEM;
}

esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t dev)
{
	assert(dev->used);
	dev->used = false;
	devices_open--;
	return ESP_OK;
}

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev,
			      const uint8_t *data, size_t length,
			      int timeout_ms)
{
	assert(dev->used && timeout_ms > 0);
	return fake_write((uint8_t)dev->address, data, length) ?
	       ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev,
				      const uint8_t *tx, size_t tx_length,
				      uint8_t *rx, size_t rx_length,
				      int timeout_ms)
{
	assert(dev->used && tx_length == 1 && timeout_ms > 0);
	return fake_read_block((uint8_t)dev->address, tx[0], rx, rx_length) ?
	       ESP_OK : ESP_ERR_INVALID_STATE;
}

/* The codes esp-bq769x0-bms's callers saw, by name, and the bus speed. */
static void test_esp_codes(void)
{
	bq769x0_config_t c = base_cfg();
	bq769x0_handle_t d = NULL;
	bq769x0_data_t s;

	chip_reset(0x55, false);
	assert(bq769x0_init(I2C_BUS, &c, &d) == ESP_ERR_NOT_FOUND);
	chip_reset(BQ769X0_ADDR_A, false);
	probe_fail = true;
	assert(bq769x0_init(I2C_BUS, &c, &d) == ESP_ERR_TIMEOUT);
	probe_fail = false;
	fail_calloc = true;
	assert(bq769x0_init(I2C_BUS, &c, &d) == ESP_ERR_NO_MEM);
	c.cells = 2;
	assert(bq769x0_init(I2C_BUS, &c, &d) == ESP_ERR_INVALID_ARG);

	/* 100 kHz unless asked: the part is not a fast-mode device. */
	c = base_cfg();
	assert(bq769x0_init(I2C_BUS, &c, &d) == ESP_OK);
	assert(devs[0].scl_hz == 100000);
	fail_write_reg = BQ769X0_SYS_CTRL2;
	assert(bq769x0_set_fets(d, false, false) == ESP_ERR_INVALID_STATE);
	fail_write_reg = -1;
	bq769x0_del(d);

	c.scl_hz = 50000;
	chip_reset(BQ769X0_ADDR_B, true);
	assert(bq769x0_init(I2C_BUS, &c, &d) == ESP_OK);
	assert(devs[0].scl_hz == 50000);
	corrupt_crc_at = 1;
	assert(bq769x0_read(d, &s) == ESP_ERR_INVALID_CRC);
	corrupt_crc_at = -1;
	bq769x0_del(d);
	assert(devices_open == 0);
}

int main(void)
{
	run_bq769x0_cases();
	test_esp_codes();
	printf("all bq769x0 checks passed\n");
	return 0;
}
