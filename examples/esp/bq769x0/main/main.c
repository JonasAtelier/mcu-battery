/*
 * bq76930: monitor a 6S LiPo, one cell at a time.
 *
 * This example NEVER touches the FETs. It reads and reports; the protection
 * thresholds are set in hardware and the part enforces them itself. If you
 * want software to switch the pack, that is a decision to make deliberately.
 *
 * esp-bq769x0-bms's example, unchanged on the bq769x0_* calls, plus the
 * alerts read through the battery API, as code that treats every battery
 * chip alike would. sdkconfig.defaults turns the driver on.
 */
#include <stdio.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "battery.h"
#include "battery_log.h"
#include "bq769x0.h"

#define PIN_SDA 21
#define PIN_SCL 22

#define CELLS       6
#define SHUNT_MOHM  1.0f      /* the sense resistor you actually fitted */

/* Per-cell limits for lithium polymer. Adjust for your chemistry. */
#define OV_MV  4200
#define UV_MV  3000

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

    bq769x0_config_t cfg = {
        .cells = CELLS,
        .shunt_mohm = SHUNT_MOHM,
    };

    bq769x0_handle_t bms;
    ESP_ERROR_CHECK(bq769x0_init(bus, &cfg, &bms));
    ESP_LOGI(TAG, "found at 0x%02x%s", bq769x0_address(bms),
             bq769x0_crc_enabled(bms) ? " with CRC" : "");

    ESP_ERROR_CHECK(bq769x0_set_ov_uv(bms, OV_MV, UV_MV));
    {
        int ov, uv;
        bq769x0_get_ov_uv(bms, &ov, &uv);
        /* The registers are coarse, so report what actually got set. */
        ESP_LOGI(TAG, "protection set to %d / %d mV per cell", ov, uv);
    }

    /* Discharge current limits: size them for your FETs and wiring. */
    {
        bq769x0_protect_t p = {
            .scd_a = 80, .scd_delay_us = 200,
            .ocd_a = 30, .ocd_delay_ms = 320,
            .ov_delay_s = 2, .uv_delay_s = 4,
        };
        ESP_ERROR_CHECK(bq769x0_set_protect(bms, &p));
        bq769x0_get_protect(bms, &p);
        ESP_LOGI(TAG, "short circuit %.0f A after %d us, overcurrent %.0f A "
                      "after %d ms", p.scd_a, p.scd_delay_us, p.ocd_a,
                 p.ocd_delay_ms);
    }

    for (;;) {
        bq769x0_data_t s;
        int lo, hi, spread;

        if (bq769x0_read(bms, &s) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        printf("pack %5d mV  %+7.0f mA  %4.1f C  |", s.pack_mv, s.current_ma,
               s.temperature_c);
        for (int i = 0; i < s.cells; i++)
            printf(" %d", s.cell_mv[i]);
        printf("\n");

        bq769x0_cell_spread(bms, &lo, &hi, &spread);
        if (spread > 100)
            ESP_LOGW(TAG, "cell spread %d mV (%d..%d) -- a cell is drifting",
                     spread, lo, hi);

        if (s.status & BQ769X0_STAT_FAULTS) {
            ESP_LOGE(TAG, "fault flags 0x%02x", s.status & BQ769X0_STAT_FAULTS);
            /* Faults latch. Clear only once you know why they set. */
        }

        /* The same part as a struct battery_dev: one sample, alerts by name. */
        {
            struct battery_dev *bat = bq769x0_battery(bms);
            struct battery_sample b;

            if (battery_read(bat, &b) == 0 && b.alerts)
                battery_log_sample(bat, &b);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
