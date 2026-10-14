#include <errno.h>
#include <string.h>

#include "battery.h"

#define BATTERY_ALERT_ALL	0xFFu

int battery_read(struct battery_dev *bat, struct battery_sample *s)
{
	struct battery_sample tmp;
	int ret;

	if (!bat || !bat->ops || !bat->ops->read || !s)
		return -EINVAL;

	memset(&tmp, 0, sizeof(tmp));
	ret = bat->ops->read(bat, &tmp);
	if (ret)
		return ret;

	*s = tmp;
	return 0;
}

int battery_clear_alerts(struct battery_dev *bat, uint32_t alerts)
{
	if (!bat || !bat->ops)
		return -EINVAL;
	if (alerts & ~BATTERY_ALERT_ALL)
		return -EINVAL;
	if (!bat->ops->clear_alerts)
		return -EOPNOTSUPP;
	if (!alerts)
		return 0;
	return bat->ops->clear_alerts(bat, alerts);
}
