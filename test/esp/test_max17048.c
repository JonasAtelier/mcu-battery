/*
 * Host-side check of the MAX17048 driver on ESP-IDF: the cases are
 * ../max17048_cases.h; this file feeds the fake from the i2c_master calls,
 * the way IDF 5.x reports each outcome, and checks the esp_err_t codes the
 * handle API returns -- the ones dingo_s3 sees.
 */
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#include "battery.h"
#include "battery_priv.h"
#include "max17048.h"
#include "max17048_math.h"
#include "max17048_regs.h"

#define RET(err)	battery_esp_err(err)
/* IDF 5.x returns ESP_ERR_INVALID_STATE for a transfer the device NACKs. */
#define NACK_ERRNO	(-EBUSY)
#define NACK		RET(NACK_ERRNO)

static int bus_tag;
#define I2C_BUS		((i2c_master_bus_handle_t)(void *)&bus_tag)

static int devices_open;
#define OPEN_DEVICES()	devices_open

#include "../max17048_cases.h"

/* ------------------------------------------------ the IDF side of it --- */

struct i2c_master_dev_t {
	bool used;
	uint16_t address;
	uint32_t scl_hz;
};

extern TickType_t stub_ticks;

static struct i2c_master_dev_t dev;
static esp_err_t probe_err = ESP_OK;	/* other than presence */

esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus, uint16_t address,
			   int xfer_timeout_ms)
{
	assert(bus == I2C_BUS && xfer_timeout_ms > 0);
	if (probe_err)
		return probe_err;
	return fake_present((uint8_t)address) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus,
				    const i2c_device_config_t *cfg,
				    i2c_master_dev_handle_t *out)
{
	assert(bus == I2C_BUS && !dev.used);
	dev = (struct i2c_master_dev_t){
		.used = true,
		.address = cfg->device_address,
		.scl_hz = cfg->scl_speed_hz,
	};
	devices_open++;
	*out = &dev;
	return ESP_OK;
}

esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t d)
{
	assert(d == &dev && dev.used);
	dev.used = false;
	devices_open--;
	return ESP_OK;
}

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t d, const uint8_t *data,
			      size_t length, int timeout_ms)
{
	assert(d == &dev && dev.used && timeout_ms > 0);
	return fake_write((uint8_t)dev.address, data, length) ?
	       ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t d,
				      const uint8_t *tx, size_t tx_length,
				      uint8_t *rx, size_t rx_length,
				      int timeout_ms)
{
	assert(d == &dev && dev.used && tx_length == 1 && timeout_ms > 0);
	return fake_read_block((uint8_t)dev.address, tx[0], rx, rx_length) ?
	       ESP_OK : ESP_ERR_INVALID_STATE;
}

/*
 * dingo_s3's two calls, as it makes them, and every code esp-max17048-
 * fuelgauge's callers could see, by name.
 */
static void test_dingo_calls(void)
{
	const max17048_config_t gauge_config = { .scl_hz = 400000 };
	max17048_handle_t gauge = NULL;
	float charge_percent = -1.0f;
	esp_err_t err;

	chip_reset();
	stub_ticks = 0;
	err = max17048_init(I2C_BUS, &gauge_config, &gauge);
	assert(err == ESP_OK && gauge);
	/* No POR, so no wait after one. */
	assert(stub_ticks == 0);
	assert(dev.scl_hz == 400000);
	regs[MAX17048_SOC] = (uint16_t)(42.5f * 256.0f);
	assert(max17048_soc(gauge, &charge_percent) == ESP_OK);
	assert(charge_percent == 42.5f);
	max17048_del(gauge);

	/* scl_hz 0 is 400 kHz, as before; anything else is passed on. */
	{
		max17048_config_t c = { 0 };

		assert(max17048_init(I2C_BUS, &c, &gauge) == ESP_OK);
		assert(dev.scl_hz == 400000);
		max17048_del(gauge);
		c.scl_hz = 100000;
		assert(max17048_init(I2C_BUS, &c, &gauge) == ESP_OK);
		assert(dev.scl_hz == 100000);
		max17048_del(gauge);
	}
}

static void test_esp_codes(void)
{
	max17048_config_t c = { 0 };
	max17048_handle_t d = NULL;
	float f;

	chip_reset();
	present = false;
	assert(max17048_init(I2C_BUS, &c, &d) == ESP_ERR_NOT_FOUND);
	chip_reset();
	regs[MAX17048_VERSION] = 0;
	assert(max17048_init(I2C_BUS, &c, &d) == ESP_ERR_NOT_FOUND);
	chip_reset();
	probe_err = ESP_ERR_TIMEOUT;
	assert(max17048_init(I2C_BUS, &c, &d) == ESP_ERR_TIMEOUT);
	probe_err = ESP_OK;
	fail_calloc = true;
	assert(max17048_init(I2C_BUS, &c, &d) == ESP_ERR_NO_MEM);
	c.empty_alert_pct = 40;
	assert(max17048_init(I2C_BUS, &c, &d) == ESP_ERR_INVALID_ARG);

	c.empty_alert_pct = 0;
	assert(max17048_init(I2C_BUS, &c, &d) == ESP_OK);
	fail_read_reg = MAX17048_VCELL;
	assert(max17048_voltage(d, &f) == ESP_ERR_INVALID_STATE);
	fail_read_reg = -1;
	max17048_del(d);
	assert(devices_open == 0);
}

int main(void)
{
	run_max17048_cases();
	test_dingo_calls();
	test_esp_codes();
	printf("all max17048 checks passed\n");
	return 0;
}
