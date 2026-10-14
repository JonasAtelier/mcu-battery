/*
 * Logs the gauge every five seconds: once through the max17048_* calls, as
 * esp-max17048-fuelgauge's example did, and once through the battery API.
 *
 * A snippet, not a project: the clock tree, the printf() retarget and the bus
 * are the app's. Pins are examples -- I2C1 on PB8 (SCL) and PB9 (SDA), AF4 --
 * any pair your chip routes to an I2C peripheral works. Written for the F4,
 * whose I2C takes Init.ClockSpeed; an F7 sets Init.Timing instead, as CubeMX
 * computes it. Needs HAL_I2C_MODULE_ENABLED and BATTERY_MAX17048 = y under
 * PKG_battery = y; printing the floats needs -u _printf_float.
 *
 * Wiring: see docs/hardware.md.
 */
#include <stdio.h>
#include <string.h>

#include CMSIS_device_header

#include "battery.h"
#include "battery_log.h"
#include "max17048.h"

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
    i2c1.Init.ClockSpeed      = 400000;
    i2c1.Init.DutyCycle       = I2C_DUTYCYCLE_2;
    i2c1.Init.AddressingMode  = I2C_ADDRESSINGMODE_7BIT;
    HAL_I2C_Init(&i2c1);
}

int main(void)
{
    HAL_Init();
    /* SystemClock_Config() and the UART behind printf() go here. */
    i2c1_init();

    const max17048_config_t cfg = { .empty_alert_pct = 10 };
    max17048_handle_t gauge;
    int err = max17048_init(&i2c1, &cfg, &gauge);
    if (err) {
        printf("max17048: init failed: %s\n", strerror(-err));
        for (;;)
            ;
    }
    struct battery_dev *bat = max17048_battery(gauge);

    for (;;) {
        max17048_data_t s;
        struct battery_sample b;
        float hours;

        if (max17048_read(gauge, &s) == 0 &&
            max17048_hours_remaining(gauge, &hours) == 0)
            printf("%.3f V  %5.1f %%  %+6.2f %%/hr  %.1f h\n", (double)s.volts,
                   (double)s.soc_percent, (double)s.crate_pct_hr,
                   (double)hours);

        err = battery_read(bat, &b);
        if (err)
            battery_err(bat, "read failed: %s", strerror(-err));
        else
            battery_log_sample(bat, &b);
        HAL_Delay(5000);
    }
}
