#include "uwb_log.h"

void uwb_log_config(const struct uwb_dev *uwb)
{
	uwb_info(uwb, "channel %u, PRF %u MHz, %u kbps, preamble %u, addr 0x%04x",
		 (unsigned)uwb->channel, (unsigned)uwb->prf_mhz,
		 (unsigned)uwb->rate_kbps, (unsigned)uwb->preamble_len,
		 (unsigned)uwb->addr);
}
