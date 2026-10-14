CC ?= cc
# gnu11: the battery_* log macros use `, ##__VA_ARGS__`.
CFLAGS += -std=gnu11 -Wall -Wextra -Werror -pedantic
# Signed overflow usually wraps anyway, so a test of wrapping arithmetic
# passes on the broken code too. UBSan makes it abort instead.
CFLAGS += -fsanitize=undefined -fno-sanitize-recover=all
# src/ for the common private header, and every chip folder, because the
# tests include a chip's private headers by plain name.
CPPFLAGS += -I../include -I../src $(addprefix -I,$(wildcard ../src/*/))

# Two suites, one per platform: the same chip driver through
# src/battery_port_esp.c against idf_stubs/ (esp/), and through
# src/battery_port_stm.c against hal_stubs/ (stm/). The cases live once, in
# *_cases.h, and both platforms run them against their own fakes. The two
# math tests are the original repos' test_math.c; they touch no bus or
# platform API and run once.

# What ESP-IDF passes: ESP_PLATFORM, and every chip on.
ESP_CPPFLAGS := -Iidf_stubs -DESP_PLATFORM \
		-DCONFIG_BATTERY_MAX17048=1 -DCONFIG_BATTERY_BQ769X0=1
# What stm-forge passes: the family header the code includes, the HAL modules
# an app's hal_conf would enable, and every chip option on
# (BATTERY_<CHIP> = y).
STM_CPPFLAGS := -Ihal_stubs \
		-DCMSIS_device_header='"stm32_hal_stub.h"' \
		-DHAL_I2C_MODULE_ENABLED -DHAL_SPI_MODULE_ENABLED \
		-DHAL_UART_MODULE_ENABLED \
		-DCONFIG_BATTERY_MAX17048=1 -DCONFIG_BATTERY_BQ769X0=1 \
		-DCONFIG_BATTERY_DEBUG=1

# The core and the stubs every binary of a suite links.
CORE     := ../src/battery.c ../src/battery_log.c
ESP_CORE := $(CORE) ../src/battery_port_esp.c idf_stubs/idf_stubs.c
STM_CORE := $(CORE) ../src/battery_port_stm.c hal_stubs/hal_stubs.c

MAX17048 := ../src/max17048/max17048.c
BQ769X0  := ../src/bq769x0/bq769x0.c

MATH_TESTS := test_max17048_math test_bq769x0_math
# Plus one port test each, for the bus ops no chip here uses.
ESP_TESTS  := $(addprefix esp/,test_core test_max17048 test_bq769x0 \
		test_port_esp)
STM_TESTS  := $(addprefix stm/,test_core test_max17048 test_bq769x0 \
		test_port_stm)
TESTS      := $(MATH_TESTS) test_names $(ESP_TESTS) $(STM_TESTS)

.PHONY: all clean test

all: $(TESTS)

test_max17048_math: test_max17048_math.c
test_bq769x0_math: test_bq769x0_math.c
test_names: test_names.c

esp/test_core: esp/test_core.c $(ESP_CORE)
esp/test_max17048: esp/test_max17048.c $(MAX17048) $(ESP_CORE)
esp/test_bq769x0: esp/test_bq769x0.c $(BQ769X0) $(ESP_CORE)
esp/test_port_esp: esp/test_port_esp.c $(ESP_CORE)

stm/test_core: stm/test_core.c $(STM_CORE)
stm/test_max17048: stm/test_max17048.c $(MAX17048) $(STM_CORE)
stm/test_bq769x0: stm/test_bq769x0.c $(BQ769X0) $(STM_CORE)
stm/test_port_stm: stm/test_port_stm.c $(STM_CORE)

# test_names is an ESP-IDF translation unit: the headers as dingo_s3 sees
# them.
test_names esp/%: CPPFLAGS += $(ESP_CPPFLAGS)
$(ESP_TESTS): CPPFLAGS += $(ESP_CPPFLAGS)
$(STM_TESTS): CPPFLAGS += $(STM_CPPFLAGS)

# Lets the driver tests count calloc() and free(), to prove a refused init
# allocates nothing and a failed one leaks nothing. GNU ld; only the
# driver's own references are wrapped.
esp/test_max17048 stm/test_max17048 esp/test_bq769x0 stm/test_bq769x0: \
	LDFLAGS += -Wl,--wrap=calloc -Wl,--wrap=free
# Every printf() in a binary that links the HAL stubs also lands in
# stub_log_last, so a test can check what was logged.
$(STM_TESTS): LDFLAGS += -Wl,--wrap=printf

$(TESTS):
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $(filter %.c,$^) -o $@ -lm

test: $(TESTS)
	@set -e; for t in $(TESTS); do echo "./$$t"; ./$$t; done

clean:
	rm -f $(TESTS)

# Headers are prerequisites so that editing one rebuilds. Without this, `make
# test` reruns a stale binary and reports green for code it never compiled.
# Recipes use $(filter %.c,$^) so the headers never reach the compiler.
HDRS  := $(wildcard ../include/*.h ../src/*.h ../src/*/*.h)
STUBS := $(wildcard idf_stubs/*.h idf_stubs/*/*.h hal_stubs/*.h)
$(TESTS): $(HDRS) $(STUBS) $(wildcard *_cases.h)
