#include <errno.h>

#include "uwb.h"
#include "uwb_math.h"

static bool uwb_bound(const struct uwb_dev *uwb)
{
	return uwb && uwb->ops && uwb->ops->send && uwb->ops->recv &&
	       uwb->ops->tx_timestamp && uwb->ops->rx_timestamp &&
	       uwb->ops->systime;
}

int uwb_send(struct uwb_dev *uwb, const void *frame, size_t len)
{
	if (!uwb_bound(uwb) || !frame)
		return -EINVAL;

	return uwb->ops->send(uwb, frame, len);
}

int uwb_recv(struct uwb_dev *uwb, void *buf, size_t cap, size_t *len,
	     uint32_t timeout_ms)
{
	if (!uwb_bound(uwb) || !buf)
		return -EINVAL;

	return uwb->ops->recv(uwb, buf, cap, len, timeout_ms);
}

/* Into scratch first, so a failed read leaves *ticks alone. */
static int uwb_stamp(struct uwb_dev *uwb, uint64_t *ticks,
		     int (*op)(struct uwb_dev *, uint64_t *))
{
	uint64_t t;
	int ret;

	ret = op(uwb, &t);
	if (ret)
		return ret;
	*ticks = t;
	return 0;
}

int uwb_tx_timestamp(struct uwb_dev *uwb, uint64_t *ticks)
{
	if (!uwb_bound(uwb) || !ticks)
		return -EINVAL;

	return uwb_stamp(uwb, ticks, uwb->ops->tx_timestamp);
}

int uwb_rx_timestamp(struct uwb_dev *uwb, uint64_t *ticks)
{
	if (!uwb_bound(uwb) || !ticks)
		return -EINVAL;

	return uwb_stamp(uwb, ticks, uwb->ops->rx_timestamp);
}

int uwb_systime(struct uwb_dev *uwb, uint64_t *ticks)
{
	if (!uwb_bound(uwb) || !ticks)
		return -EINVAL;

	return uwb_stamp(uwb, ticks, uwb->ops->systime);
}

uint64_t uwb_ticks_between(const struct uwb_dev *uwb, uint64_t start,
			   uint64_t end)
{
	if (!uwb)
		return 0;
	return uwb_ts_diff(end, start, uwb->ts_bits);
}
