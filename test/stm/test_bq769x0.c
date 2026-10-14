/*
 * Host-side check of the BQ769x0 driver on the STM32 HAL: the cases are
 * ../bq769x0_cases.h; this file feeds the fake from the HAL's I2C calls.
 * esp-bq769x0-bms had no HAL side; on it the handle API returns 0 or a
 * negative errno.
 */
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stm32_hal_stub.h"

#include "battery.h"
#include "bq769x0.h"
#include "bq769x0_math.h"
#include "bq769x0_regs.h"

#define RET(err)	(err)
/* A NACK is HAL_ERROR, like any other fault. */
#define NACK_ERRNO	(-EIO)
#define NACK		RET(NACK_ERRNO)

static I2C_HandleTypeDef hi2c;
#define I2C_BUS		(&hi2c)

/* The HAL port adds nothing to the handle: nothing to leave behind. */
#define OPEN_DEVICES()	0

#include "../bq769x0_cases.h"

/* ------------------------------------------------ the HAL side of it --- */

HAL_StatusTypeDef HAL_I2C_IsDeviceReady(I2C_HandleTypeDef *h, uint16_t addr,
					uint32_t trials, uint32_t timeout)
{
	assert(h == &hi2c && trials >= 1 && timeout > 0);
	if (probe_fail)
		return HAL_TIMEOUT;
	return fake_present((uint8_t)(addr >> 1)) ? HAL_OK : HAL_ERROR;
}

HAL_StatusTypeDef HAL_I2C_Mem_Write(I2C_HandleTypeDef *h, uint16_t addr,
				    uint16_t mem, uint16_t mem_size,
				    uint8_t *data, uint16_t size,
				    uint32_t timeout)
{
	uint8_t frame[1 + 32];

	assert(h == &hi2c && mem_size == I2C_MEMADD_SIZE_8BIT &&
	       size < sizeof(frame) && timeout > 0);
	frame[0] = (uint8_t)mem;
	memcpy(frame + 1, data, size);
	return fake_write((uint8_t)(addr >> 1), frame, 1 + (size_t)size) ?
	       HAL_OK : HAL_ERROR;
}

HAL_StatusTypeDef HAL_I2C_Mem_Read(I2C_HandleTypeDef *h, uint16_t addr,
				   uint16_t mem, uint16_t mem_size,
				   uint8_t *data, uint16_t size,
				   uint32_t timeout)
{
	assert(h == &hi2c && mem_size == I2C_MEMADD_SIZE_8BIT && timeout > 0);
	return fake_read_block((uint8_t)(addr >> 1), (uint8_t)mem, data, size) ?
	       HAL_OK : HAL_ERROR;
}

/* The errno codes the HAL side returns, by name, and its log line. */
static void test_hal_codes(void)
{
	bq769x0_config_t c = base_cfg();
	bq769x0_handle_t d = NULL;

	chip_reset(0x55, false);
	assert(bq769x0_init(I2C_BUS, &c, &d) == -ENXIO);
	assert(!strcmp(stub_log_last, "battery: bq769x0: no bq769x0 at 0x08 or "
		       "0x18, with or without CRC"));
	chip_reset(BQ769X0_ADDR_B, true);
	assert(bq769x0_init(I2C_BUS, &c, &d) == 0);
	assert(!strcmp(stub_log_last, "battery: bq769x0: at 0x18 with CRC: 6 "
		       "cells, gain 396 uV/LSB, offset 0 mV"));
	bq769x0_del(d);
}

int main(void)
{
	run_bq769x0_cases();
	test_hal_codes();
	printf("all bq769x0 checks passed\n");
	return 0;
}
