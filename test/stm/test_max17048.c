/*
 * Host-side check of the MAX17048 driver on the STM32 HAL: the cases are
 * ../max17048_cases.h; this file feeds the fake from the HAL's I2C calls.
 * esp-max17048-fuelgauge had no HAL side; on it the handle API returns 0 or
 * a negative errno.
 */
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stm32_hal_stub.h"

#include "battery.h"
#include "max17048.h"
#include "max17048_math.h"
#include "max17048_regs.h"

#define RET(err)	(err)
/* A NACK is HAL_ERROR, like any other fault. */
#define NACK_ERRNO	(-EIO)
#define NACK		RET(NACK_ERRNO)

static I2C_HandleTypeDef hi2c;
#define I2C_BUS		(&hi2c)

/* The HAL port adds nothing to the handle: nothing to leave behind. */
#define OPEN_DEVICES()	0

#include "../max17048_cases.h"

/* ------------------------------------------------ the HAL side of it --- */

HAL_StatusTypeDef HAL_I2C_IsDeviceReady(I2C_HandleTypeDef *h, uint16_t addr,
					uint32_t trials, uint32_t timeout)
{
	assert(h == &hi2c && trials >= 1 && timeout > 0);
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

/* No POR, so no wait; the log line says which start this was. */
static void test_hal_wait_and_log(void)
{
	max17048_config_t c = { 0 };
	max17048_handle_t d = NULL;

	chip_reset();
	stub_tick = 0;
	assert(max17048_init(I2C_BUS, &c, &d) == 0);
	assert(stub_tick == 0);
	assert(!strcmp(stub_log_last, "battery: max17048: version 0x0012, "
		       "powered up, RCOMP 0x97, empty alert at 4%"));
	max17048_del(d);
	assert(max17048_init(I2C_BUS, &c, &d) == 0);
	assert(!strcmp(stub_log_last, "battery: max17048: version 0x0012, "
		       "kept its state, RCOMP 0x97, empty alert at 4%"));
	max17048_del(d);
}

int main(void)
{
	run_max17048_cases();
	test_hal_wait_and_log();
	printf("all max17048 checks passed\n");
	return 0;
}
