/*
 * The core's cases -- uwb_send(), uwb_recv(), the timestamps, the wrap-safe
 * tick difference, and the log -- shared by esp/test_core.c and
 * stm/test_core.c. Each includes this after providing:
 *
 *   LOG_PREFIX             what the platform's log puts before "uwb: "
 *   stub_log_last          the last log line
 *
 * What the chip's ops do lives in its driver, so test_dw1000.c checks that
 * against the real driver.
 */

static int fake_calls;
static int fake_ret;                    /* fail the next op with this */
static uint64_t fake_ticks;
static size_t fake_len;

static int fake_send(struct uwb_dev *uwb, const void *frame, size_t len)
{
	(void)uwb;
	(void)frame;
	fake_calls++;
	fake_len = len;
	return fake_ret;
}

static int fake_recv(struct uwb_dev *uwb, void *buf, size_t cap, size_t *len,
		     uint32_t timeout_ms)
{
	(void)uwb;
	(void)timeout_ms;
	fake_calls++;
	if (fake_ret)
		return fake_ret;
	memset(buf, 0xAB, cap);
	*len = cap;
	return 0;
}

static int fake_stamp(struct uwb_dev *uwb, uint64_t *ticks)
{
	(void)uwb;
	fake_calls++;
	if (fake_ret) {
		*ticks = 0xDEAD;        /* the op scribbles; the caller must not see it */
		return fake_ret;
	}
	*ticks = fake_ticks;
	return 0;
}

static const struct uwb_ops fake_ops = {
	.send = fake_send,
	.recv = fake_recv,
	.tx_timestamp = fake_stamp,
	.rx_timestamp = fake_stamp,
	.systime = fake_stamp,
};

static struct uwb_dev fake_dev(void)
{
	struct uwb_dev uwb = {
		.name = "fake",
		.ops = &fake_ops,
		.channel = 5,
		.prf_mhz = 64,
		.rate_kbps = 6800,
		.preamble_len = 128,
		.addr = 0x1234,
		.ts_bits = 40,
		.tick_s = 15.65e-12,
	};

	fake_calls = 0;
	fake_ret = 0;
	return uwb;
}

static void test_traffic(void)
{
	struct uwb_dev u = fake_dev();
	uint8_t buf[4];
	size_t len = 0;

	assert(uwb_send(&u, "abc", 3) == 0 && fake_len == 3);
	assert(uwb_recv(&u, buf, sizeof buf, &len, 10) == 0 && len == 4);
	fake_ret = -EIO;
	assert(uwb_send(&u, "abc", 3) == -EIO);
	assert(uwb_recv(&u, buf, sizeof buf, &len, 10) == -EIO);
}

static void test_timestamps(void)
{
	struct uwb_dev u = fake_dev();
	uint64_t t = 7;

	fake_ticks = 0x123456789AULL;
	assert(uwb_tx_timestamp(&u, &t) == 0 && t == 0x123456789AULL);
	assert(uwb_rx_timestamp(&u, &t) == 0 && t == 0x123456789AULL);
	assert(uwb_systime(&u, &t) == 0 && t == 0x123456789AULL);

	/* A failed read leaves *ticks alone, whatever the op wrote. */
	t = 7;
	fake_ret = -ETIMEDOUT;
	assert(uwb_tx_timestamp(&u, &t) == -ETIMEDOUT && t == 7);
	assert(uwb_rx_timestamp(&u, &t) == -ETIMEDOUT && t == 7);
	assert(uwb_systime(&u, &t) == -ETIMEDOUT && t == 7);
}

static void test_ticks_between(void)
{
	struct uwb_dev u = fake_dev();

	assert(uwb_ticks_between(&u, 100, 350) == 250);
	/* Across the 40-bit wrap. */
	assert(uwb_ticks_between(&u, 0xFFFFFFFFF0ULL, 0x10) == 0x20);
	u.ts_bits = 32;
	assert(uwb_ticks_between(&u, 0xFFFFFFF0ULL, 0x10) == 0x20);
	assert(uwb_ticks_between(NULL, 1, 2) == 0);
}

static void test_unbound(void)
{
	struct uwb_dev u = fake_dev();
	struct uwb_ops partial = fake_ops;
	uint8_t buf[4];
	uint64_t t;
	size_t len;

	/* Nothing to call is the caller's mistake, and nothing is called. */
	assert(uwb_send(NULL, "a", 1) == -EINVAL);
	assert(uwb_send(&u, NULL, 1) == -EINVAL);
	assert(uwb_recv(&u, NULL, 1, &len, 1) == -EINVAL);
	assert(uwb_tx_timestamp(&u, NULL) == -EINVAL);
	assert(uwb_rx_timestamp(NULL, &t) == -EINVAL);
	assert(uwb_systime(&u, NULL) == -EINVAL);
	u.ops = NULL;
	assert(uwb_send(&u, "a", 1) == -EINVAL);
	assert(uwb_recv(&u, buf, 1, &len, 1) == -EINVAL);
	/* An ops table with a hole is refused whole. */
	partial.systime = NULL;
	u.ops = &partial;
	assert(uwb_send(&u, "a", 1) == -EINVAL);
	assert(uwb_tx_timestamp(&u, &t) == -EINVAL);
	assert(fake_calls == 0);
}

static void test_log(void)
{
	struct uwb_dev u = fake_dev();

	uwb_err(&u, "no answer (%d)", -19);
	assert(!strcmp(stub_log_last, LOG_PREFIX("E") "uwb: fake: no answer (-19)"));

	uwb_log_config(&u);
	assert(!strcmp(stub_log_last, LOG_PREFIX("I")
		       "uwb: fake: channel 5, PRF 64 MHz, 6800 kbps, "
		       "preamble 128, addr 0x1234"));
}

static void run_core_cases(void)
{
	test_traffic();
	test_timestamps();
	test_ticks_between();
	test_unbound();
	test_log();
}
