/*
 * The core's cases, shared by esp/test_core.c and stm/test_core.c, against
 * a fake chip whose ops hand back whatever the test put in them. Each
 * platform file provides LOG_PREFIX(level), what its log puts before
 * "battery: ".
 */

extern char stub_log_last[256];

static struct battery_sample fake_sample;
static int fake_ret, fake_calls, clear_calls;
static uint32_t cleared;
static bool arrived_zeroed;

static int fake_read(struct battery_dev *bat, struct battery_sample *s)
{
	static const struct battery_sample zero;

	(void)bat;
	fake_calls++;
	arrived_zeroed = !memcmp(s, &zero, sizeof(zero));
	if (fake_ret)
		return fake_ret;
	*s = fake_sample;
	return 0;
}

static int fake_clear(struct battery_dev *bat, uint32_t alerts)
{
	(void)bat;
	clear_calls++;
	cleared = alerts;
	return fake_ret;
}

static const struct battery_ops fake_ops = {
	.read = fake_read,
	.clear_alerts = fake_clear,
};
static const struct battery_ops read_only_ops = { .read = fake_read };

static struct battery_dev fake_dev(void)
{
	struct battery_dev bat = { .name = "fake", .ops = &fake_ops };

	fake_sample = (struct battery_sample){
		.pack_v = 8.25f,
		.soc_percent = NAN,
		.current_a = -1.5f,
		.temperature_c = 25.5f,
		.cells = 2,
		.cell_v = { 4.125f, 4.125f },
		.alerts = BATTERY_ALERT_OVERVOLTAGE,
	};
	fake_ret = fake_calls = clear_calls = 0;
	cleared = 0;
	return bat;
}

static void test_read(void)
{
	struct battery_dev bat = fake_dev();
	struct battery_sample s;

	memset(&s, 0xA5, sizeof(s));
	assert(battery_read(&bat, &s) == 0);
	assert(fake_calls == 1);
	/* The op starts from zero, so a field it skips cannot be stale. */
	assert(arrived_zeroed);
	assert(s.pack_v == 8.25f && isnan(s.soc_percent) &&
	       s.current_a == -1.5f && s.temperature_c == 25.5f &&
	       s.cells == 2 && s.cell_v[1] == 4.125f && s.cell_v[2] == 0.0f &&
	       s.alerts == BATTERY_ALERT_OVERVOLTAGE);
}

static void test_read_errors(void)
{
	struct battery_dev bat = fake_dev();
	struct battery_sample s = { .pack_v = 1.0f, .cells = 7 };

	/* Whatever the chip says comes back unchanged, and s is left alone. */
	fake_ret = -ETIMEDOUT;
	assert(battery_read(&bat, &s) == -ETIMEDOUT);
	fake_ret = -EBADMSG;
	assert(battery_read(&bat, &s) == -EBADMSG);
	fake_ret = -EIO;
	assert(battery_read(&bat, &s) == -EIO);
	assert(s.pack_v == 1.0f && s.cells == 7);

	/* Nothing to call is the caller's mistake, not a chip error. */
	fake_calls = 0;
	assert(battery_read(NULL, &s) == -EINVAL);
	assert(battery_read(&bat, NULL) == -EINVAL);
	bat.ops = NULL;
	assert(battery_read(&bat, &s) == -EINVAL);
	assert(fake_calls == 0);
}

static void test_clear_alerts(void)
{
	struct battery_dev bat = fake_dev();

	assert(battery_clear_alerts(&bat, BATTERY_ALERT_OVERVOLTAGE |
					  BATTERY_ALERT_EXTERNAL) == 0);
	assert(clear_calls == 1 &&
	       cleared == (BATTERY_ALERT_OVERVOLTAGE | BATTERY_ALERT_EXTERNAL));

	/* Nothing named: nothing written. */
	assert(battery_clear_alerts(&bat, 0) == 0);
	assert(clear_calls == 1);

	/* A bit that is no alert is refused before the chip sees it. */
	assert(battery_clear_alerts(&bat, 1u << 8) == -EINVAL);
	assert(battery_clear_alerts(&bat, BATTERY_ALERT_CHIP_RESET |
					  0x80000000u) == -EINVAL);
	assert(clear_calls == 1);

	/* The chip's own error comes back. */
	fake_ret = -EIO;
	assert(battery_clear_alerts(&bat, BATTERY_ALERT_UNDERVOLTAGE) == -EIO);

	/* A chip with nothing to clear says so, even for no alerts. */
	bat.ops = &read_only_ops;
	assert(battery_clear_alerts(&bat, BATTERY_ALERT_LOW_CHARGE) ==
	       -EOPNOTSUPP);
	assert(battery_clear_alerts(&bat, 0) == -EOPNOTSUPP);

	assert(battery_clear_alerts(NULL, 0) == -EINVAL);
	bat.ops = NULL;
	assert(battery_clear_alerts(&bat, 0) == -EINVAL);
}

static void test_log(void)
{
	struct battery_dev bat = fake_dev();
	struct battery_sample s = fake_sample;

	battery_err(&bat, "no answer (%d)", -6);
	assert(!strcmp(stub_log_last,
		       LOG_PREFIX("E") "battery: fake: no answer (-6)"));
	/* No arguments after the format must still compile and work. */
	battery_warn(&bat, "no arguments");
	assert(!strcmp(stub_log_last,
		       LOG_PREFIX("W") "battery: fake: no arguments"));

	battery_log_sample(&bat, &s);
	assert(!strcmp(stub_log_last, LOG_PREFIX("I")
		       "battery: fake: 8.250 V  nan %  -1.500 A  25.5 C  "
		       "alerts 0x01  cells 4.125 4.125"));

	/* Fifteen cells fit; a wild value cuts the line short, not past it. */
	s.cells = BATTERY_CELLS_MAX;
	for (int i = 0; i < BATTERY_CELLS_MAX; i++)
		s.cell_v[i] = -1000.125f;
	battery_log_sample(&bat, &s);
	assert(strstr(stub_log_last, "cells -1000.125"));
	s.cells = 99;
	battery_log_sample(&bat, &s);
}

static void run_core_cases(void)
{
	test_read();
	test_read_errors();
	test_clear_alerts();
	test_log();
}
