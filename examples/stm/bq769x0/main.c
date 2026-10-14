/*
 * bq76930: monitors a 6S LiPo once a second through the battery API, after
 * setting the protection limits through the bq769x0_* calls. It never
 * touches the FETs.
 *
 * A snippet, not a project: the clock tree, the printf() retarget and the bus
 * are the app's. Pins are examples -- I2C1 on PB8 (SCL) and PB9 (SDA), AF4 --
 * any pair your chip routes to an I2C peripheral works. Written for the F4,
 * whose I2C takes Init.ClockSpeed; an F7 sets Init.Timing instead, as CubeMX
 * computes it. Needs HAL_I2C_MODULE_ENABLED and BATTERY_BQ769X0 = y under
 * PKG_battery = y; printing the floats needs -u _printf_float.
 *
 * Wiring: see docs/hardware.md.
 */
#include <stdio.h>
#include <string.h>

#include CMSIS_device_header

#include "battery.h"
#include "battery_log.h"
#include "bq769x0.h"

#define CELLS       6
#define SHUNT_MOHM  1.0f      /* the sense resistor you actually fitted */

/* Per-cell limits for lithium polymer. Adjust for your chemistry. */
#define OV_MV  4200
#define UV_MV  3000

static I2C_HandleTypeDef i2c1;

/* The app's half: pins, peripheral clock and bus speed. */
static void i2c1_init(void)
{
    GPIO_InitTypeDef pins = {
        .Pin       = GPIO_PIN_8 | GPIO_PIN_9,
        .Mode      = GPIO_MODE_AF_OD,
        .Pull      = GPIO_PULLUP, /* weak; fit real 4.7k pull-ups too */
        .Speed     = GPIO_SPEED_FREQ_HIGH,
        .Alternate = GPIO_AF4_I2C1,
    };

    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_I2C1_CLK_ENABLE();
    HAL_GPIO_Init(GPIOB, &pins);

    i2c1.Instance             = I2C1;
    i2c1.Init.ClockSpeed      = 100000;   /* the part is not a fast-mode device */
    i2c1.Init.DutyCycle       = I2C_DUTYCYCLE_2;
    i2c1.Init.AddressingMode  = I2C_ADDRESSINGMODE_7BIT;
    HAL_I2C_Init(&i2c1);
}

int main(void)
{
    HAL_Init();
    /* SystemClock_Config() and the UART behind printf() go here. */
    i2c1_init();

    const bq769x0_config_t cfg = { .cells = CELLS, .shunt_mohm = SHUNT_MOHM };
    bq769x0_handle_t bms;
    int err = bq769x0_init(&i2c1, &cfg, &bms);
    if (!err)
        err = bq769x0_set_ov_uv(bms, OV_MV, UV_MV);
    if (err) {
        printf("bq769x0: init failed: %s\n", strerror(-err));
        for (;;)
            ;
    }
    struct battery_dev *bat = bq769x0_battery(bms);

    for (;;) {
        struct battery_sample b;

        err = battery_read(bat, &b);
        if (err) {
            battery_err(bat, "read failed: %s", strerror(-err));
        } else {
            battery_log_sample(bat, &b);
            /* Faults latch. Clear only once you know why they set. */
            if (b.alerts)
                battery_warn(bat, "alerts 0x%02lx", (unsigned long)b.alerts);
        }
        HAL_Delay(1000);
    }
}
