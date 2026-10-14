/*
 * MAX17048: state of charge for a single lithium cell, with no sense resistor.
 *
 * The first few minutes after power-up are the gauge learning the cell. Do not
 * judge it on its first reading.
 *
 * esp-max17048-fuelgauge's example, unchanged on the max17048_* calls, plus
 * the same gauge read once through the battery API, as code that treats
 * every battery chip alike would. sdkconfig.defaults turns the driver on.
 */
#include <stdio.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "battery.h"
#include "battery_log.h"
#include "max17048.h"

#define PIN_SDA 21
#define PIN_SCL 22

static const char *TAG = "example";

void app_main(void)
{
    i2c_master_bus_config_t buscfg = {
        .i2c_port = -1,
        .sda_io_num = PIN_SDA,
        .scl_io_num = PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&buscfg, &bus));

    max17048_config_t cfg = {
        .empty_alert_pct = 10,   /* pull ALRT low at 10% left */
    };

    max17048_handle_t gauge;
    ESP_ERROR_CHECK(max17048_init(bus, &cfg, &gauge));
    {
        struct battery_dev *bat = max17048_battery(gauge);
        struct battery_sample b;

        if (battery_read(bat, &b) == 0)
            battery_log_sample(bat, &b);
    }

    for (;;) {
        max17048_data_t s;
        float hours;

        if (max17048_read(gauge, &s) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        max17048_hours_remaining(gauge, &hours);

        printf("%.3f V  %5.1f %%  %+6.2f %%/hr", s.volts, s.soc_percent,
               s.crate_pct_hr);
        if (hours > 0.0f)
            printf("  %s in %.1f h", s.crate_pct_hr > 0 ? "full" : "empty", hours);
        printf("%s\n", s.alert ? "   ALERT" : "");

        if (s.alert) {
            ESP_LOGW(TAG, "battery low");
            max17048_clear_alert(gauge);
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
