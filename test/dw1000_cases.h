/*
 * A fake DW1000 at SPI-transaction level, and this library's cases against
 * it, shared by esp/test_dw1000.c and stm/test_dw1000.c. The fake decodes
 * each transaction's 1-3 byte header (UM 2.2.1.2), keeps the register files,
 * and models what the driver relies on: DEV_ID; SYS_STATUS bits cleared by
 * writing 1; CPLOCK only once the LDE load has handed the clocks back; OTP
 * reads; TXSTRT, delayed with TXDLYS or not, stamping TX_TIME and raising
 * TXFRS -- or HPDWARN for a slot already missed; WAIT4RESP and RXENAB
 * turning the receiver on for a queued frame, stamping RX_TIME. A scripted
 * peer answers the ranging frames. Each platform file feeds it from its own
 * SPI calls -- spi_fails() first, to fail the transaction the case asks
 * for, then chip_xfer() -- and provides:
 *
 *   RET(err)               what the handle API returns for a negative errno
 *   base_cfg()             a config on that platform's bus, channel 5
 *   raise_irq()            the IRQ line rising, if the test wired one
 *
 * esp-dw1000-uwb had no driver test; all of this is new.
 */

#include "dw1000_frame.h"
#include "dw1000_regs.h"
#include "dw1000_time.h"

/* ------------------------------------------------------- the fake chip --- */

static void raise_irq(void);
extern char stub_log_last[256];

#define FILE_LEN 0x2A00                 /* past LDE_REPC at 0x2E:2804 */
#define MASK40   0xFFFFFFFFFFULL

static uint8_t file[64][FILE_LEN];
static bool absent, pll_never, cplock, lde_started, lde_loaded;
static bool rx_on, late_next_tx;
static uint32_t dev_id_value;
static uint32_t otp[0x20];
static uint64_t sys_time;
static int xfers, spi_fail_at = -1;
static bool spi_fail_once;
static int soft_resets_all, soft_resets_rx, ldo_kicks;

struct air_frame {
    uint8_t data[128];
    size_t len;
    uint64_t ts;
    bool error;
};
static struct air_frame rxq[4];
static int rxq_n;

static uint8_t last_tx[128];
static size_t last_tx_len;
static uint64_t last_tx_ts;
static int tx_count;
static void (*peer_on_tx)(const uint8_t *f, size_t len, uint64_t ts);

static uint64_t get_le(int id, int sub, int n)
{
    uint64_t v = 0;

    for (int i = n - 1; i >= 0; i--)
        v = (v << 8) | file[id][sub + i];
    return v;
}

static void put_le(int id, int sub, uint64_t v, int n)
{
    for (int i = 0; i < n; i++)
        file[id][sub + i] = (uint8_t)(v >> (8 * i));
}

static void status_set(uint64_t bits)
{
    put_le(DW_SYS_STATUS, 0, get_le(DW_SYS_STATUS, 0, 5) | bits, 5);
}

static void chip_reset(void)
{
    memset(file, 0, sizeof(file));
    absent = pll_never = cplock = lde_started = lde_loaded = false;
    rx_on = late_next_tx = false;
    dev_id_value = 0xDECA0130;
    memset(otp, 0, sizeof(otp));
    otp[OTP_XTAL_TRIM] = 0x0B;
    otp[OTP_LDOTUNE] = 0x88;
    sys_time = 0x1234;
    xfers = 0;
    spi_fail_at = -1;
    spi_fail_once = false;
    soft_resets_all = soft_resets_rx = ldo_kicks = 0;
    rxq_n = 0;
    last_tx_len = 0;
    tx_count = 0;
    peer_on_tx = NULL;
    put_le(DW_SYS_CFG, 0, 0x00001200, 4);   /* reset default, UM p.68 */
}

/* A frame on the air, for the receiver's next turn on. */
static void air_queue(const void *data, size_t len, uint64_t ts, bool error)
{
    assert(rxq_n < 4 && len <= 125);
    memcpy(rxq[rxq_n].data, data, len);
    rxq[rxq_n].len = len;
    rxq[rxq_n].ts = ts & MASK40;
    rxq[rxq_n].error = error;
    rxq_n++;
}

static void deliver(void)
{
    struct air_frame f;

    if (!rx_on || !rxq_n)
        return;
    f = rxq[0];
    memmove(rxq, rxq + 1, (size_t)--rxq_n * sizeof(rxq[0]));
    rx_on = false;
    if (f.error) {
        status_set(SYS_STATUS_RXPHE);
    } else {
        memcpy(file[DW_RX_BUFFER], f.data, f.len);
        put_le(DW_RX_FINFO, 0, f.len + 2, 4);       /* RXFLEN with the FCS */
        put_le(DW_RX_TIME, 0, f.ts, 5);
        status_set(SYS_STATUS_RXDFR | SYS_STATUS_RXFCG | SYS_STATUS_LDEDONE);
    }
    raise_irq();
}

static void do_tx(uint32_t ctrl)
{
    size_t len = (size_t)(get_le(DW_TX_FCTRL, 0, 4) & 0x7F) - 2;
    uint64_t antd = get_le(DW_TX_ANTD, 0, 2);
    uint64_t ts;

    if (ctrl & SYS_CTRL_TXDLYS) {
        if (late_next_tx) {
            late_next_tx = false;
            status_set(SYS_STATUS_HPDWARN);
            return;
        }
        /* DX_TIME's low 9 bits are ignored (UM p.77). */
        ts = ((get_le(DW_DX_TIME, 0, 5) & ~0x1FFULL) + antd) & MASK40;
    } else {
        ts = (sys_time + antd) & MASK40;
    }
    memcpy(last_tx, file[DW_TX_BUFFER], len);
    last_tx_len = len;
    last_tx_ts = ts;
    tx_count++;
    put_le(DW_TX_TIME, 0, ts, 5);
    status_set(SYS_STATUS_TXFRS);
    if (ctrl & SYS_CTRL_WAIT4RESP)
        rx_on = true;
    if (peer_on_tx)
        peer_on_tx(last_tx, len, ts);
    raise_irq();
    deliver();
}

/* One SPI transaction, full duplex: out[] goes in, in[] comes back. */
static void chip_xfer(const uint8_t *out, uint8_t *in, size_t n)
{
    bool write = out[0] & 0x80;
    int id = out[0] & 0x3F;
    size_t h = 1;
    int sub = 0;

    xfers++;
    sys_time = (sys_time + 640) & MASK40;
    memset(in, 0, n);
    if (absent) {
        memset(in, 0xFF, n);            /* MISO pulled up, nothing there */
        return;
    }
    if (out[0] & 0x40) {
        sub = out[1] & 0x7F;
        h = 2;
        if (out[1] & 0x80) {
            sub |= out[2] << 7;
            h = 3;
        }
    }
    assert(n >= h && sub + (n - h) <= FILE_LEN);

    if (!write) {
        put_le(DW_DEV_ID, 0, dev_id_value, 4);
        put_le(DW_SYS_TIME, 0, sys_time, 5);
        if (cplock)
            file[DW_SYS_STATUS][0] |= (uint8_t)SYS_STATUS_CPLOCK;
        memcpy(in + h, &file[id][sub], n - h);
        return;
    }

    const uint8_t *d = out + h;
    size_t len = n - h;

    if (id == DW_SYS_STATUS) {
        for (size_t i = 0; i < len; i++)
            file[id][sub + i] &= (uint8_t)~d[i];        /* write 1 to clear */
        return;
    }
    memcpy(&file[id][sub], d, len);
    if (id == DW_SYS_CTRL && sub == 0) {
        uint32_t v = (uint32_t)get_le(DW_SYS_CTRL, 0, 4);

        put_le(DW_SYS_CTRL, 0, 0, 4);                   /* self-clearing */
        if (v & SYS_CTRL_TRXOFF)
            rx_on = false;
        if (v & SYS_CTRL_TXSTRT)
            do_tx(v);
        if (v & SYS_CTRL_RXENAB) {
            rx_on = true;
            deliver();
        }
    } else if (id == DW_PMSC && sub == DW_PMSC_SOFTRESET) {
        if (d[0] == 0x00) {
            soft_resets_all++;
            cplock = lde_started = lde_loaded = false;
            put_le(DW_SYS_STATUS, 0, 0, 5);
        } else if (d[0] == 0xE0) {
            soft_resets_rx++;
        }
    } else if (id == DW_PMSC && sub == DW_PMSC_CTRL0 && len == 2) {
        uint16_t v = (uint16_t)get_le(DW_PMSC, 0, 2);

        if (v == 0x0301)
            lde_started = true;
        if (v == 0x0200 && lde_started && get_le(DW_OTP_IF, DW_OTP_CTRL, 2) == 0x8000) {
            lde_loaded = true;
            cplock = !pll_never;
        }
    } else if (id == DW_OTP_IF && sub == DW_OTP_CTRL && len == 1 && d[0] == 0x03) {
        put_le(DW_OTP_IF, DW_OTP_RDAT, otp[get_le(DW_OTP_IF, DW_OTP_ADDR, 2) & 0x1F], 4);
    } else if (id == DW_OTP_IF && sub == DW_OTP_SF && (d[0] & OTP_SF_LDO_KICK)) {
        ldo_kicks++;
    }
}

/* Whether the platform should fail this transaction instead: every one
 * from spi_fail_at on, or with spi_fail_once that one alone. */
static bool spi_fails(void)
{
    if (spi_fail_at < 0)
        return false;
    if (!spi_fail_once)
        return xfers >= spi_fail_at;
    if (xfers != spi_fail_at)
        return false;
    xfers++;                            /* it happened; the next one is fine */
    return true;
}

/* ------------------------------------------------------ allocations ------ */

void *__real_calloc(size_t n, size_t size);
void __real_free(void *p);
void *__wrap_calloc(size_t n, size_t size);
void __wrap_free(void *p);

static int allocs, frees;

void *__wrap_calloc(size_t n, size_t size)
{
    allocs++;
    return __real_calloc(n, size);
}

void __wrap_free(void *p)
{
    if (p)
        frees++;
    __real_free(p);
}

/* ------------------------------------------------------------ helpers ---- */

static dw1000_handle_t open_cfg(dw1000_config_t *c)
{
    dw1000_handle_t h = NULL;

    chip_reset();
    assert(dw1000_init(c, &h) == RET(0));
    assert(h);
    return h;
}

static dw1000_handle_t open_ok(void)
{
    dw1000_config_t c = base_cfg();

    return open_cfg(&c);
}

/* ------------------------------------------------------------- cases ---- */

static void test_init(void)
{
    dw1000_config_t c = base_cfg();
    dw1000_handle_t h;
    struct uwb_dev *u;

    allocs = frees = 0;
    h = open_cfg(&c);
    /* Soft reset, LDE microcode (UM Table 4), PLL lock detect on, the
     * factory LDO tuning kicked and the crystal trim applied. */
    assert(soft_resets_all == 1 && lde_loaded);
    assert(file[DW_EXT_SYNC][DW_EC_CTRL] == EC_CTRL_PLLLDT);
    assert(ldo_kicks == 1);
    assert(file[DW_FS_CTRL][DW_FS_XTALT] == (0x0B | 0x60));

    /* SYS_CFG: the reset default, plus DIS_STXP -- bit 18 -- so the manual
     * TX power values below apply. A literal, so a macro cannot agree with
     * itself. */
    assert(get_le(DW_SYS_CFG, 0, 4) & (1UL << 18));
    assert(get_le(DW_SYS_CFG, 0, 4) == 0x00041200UL);
    /* Channel 5, PRF 64: UM Tables 19, 34, 35, 37, 40, 41. */
    assert(get_le(DW_TX_POWER, 0, 4) == 0x85858585UL);
    assert(file[DW_RF_CONF][DW_RF_RXCTRLH] == 0xD8);
    assert(get_le(DW_RF_CONF, DW_RF_TXCTRL, 4) == 0x001E3FE0UL);
    assert(file[DW_TX_CAL][DW_TC_PGDELAY] == 0xC0);
    assert(get_le(DW_FS_CTRL, DW_FS_PLLCFG, 4) == 0x0800041DUL);
    assert(file[DW_FS_CTRL][DW_FS_PLLTUNE] == 0xBE);
    /* CHAN_CTRL: channel 5 both ways, RXPRF 64 MHz, code 9 both ways. */
    assert(get_le(DW_CHAN_CTRL, 0, 4) ==
           (5u | 5u << 4 | 2u << 18 | 9u << 22 | 9u << 27));
    /* The default antenna delay, both sides. */
    assert(get_le(DW_TX_ANTD, 0, 2) == 16436 &&
           get_le(DW_LDE_CTRL, DW_LDE_RXANTD, 2) == 16436);

    /* The category view. */
    u = dw1000_uwb(h);
    assert(u && !strcmp(u->name, "dw1000") && u->ops && u->priv == h);
    assert(u->channel == 5 && u->prf_mhz == 64 && u->rate_kbps == 6800 &&
           u->preamble_len == 128 && u->addr == 2);
    assert(u->ts_bits == 40 && u->tick_s == DW_TIME_UNIT);
    assert(dw1000_uwb(NULL) == NULL);

    assert(dw1000_del(h) == RET(0));
    assert(allocs == frees);
}

/* Channel 3 takes the manual's FS_PLLTUNE, 0x5E (UM Table 41), not the
 * 0x56 of DecaWave's reference code. */
static void test_channel_3(void)
{
    dw1000_config_t c = base_cfg();
    dw1000_handle_t h;

    c.channel = 3;
    c.prf = DW_PRF_16M;
    c.preamble_code = 5;                /* a 16 MHz code of channel 3 */
    h = open_cfg(&c);
    assert(file[DW_FS_CTRL][DW_FS_PLLTUNE] == 0x5E);
    assert(get_le(DW_FS_CTRL, DW_FS_PLLCFG, 4) == 0x08401009UL);
    assert(get_le(DW_TX_POWER, 0, 4) == 0x6F6F6F6FUL);
    dw1000_del(h);
}

static void test_init_failures(void)
{
    dw1000_config_t c = base_cfg();
    dw1000_handle_t h = NULL;
    int good;

    /* Nothing on the bus, or another part. */
    chip_reset();
    absent = true;
    allocs = frees = 0;
    assert(dw1000_init(&c, &h) == RET(-ENODEV));
    chip_reset();
    dev_id_value = 0xDECA0131;
    assert(dw1000_init(&c, &h) == RET(-ENODEV));
    assert(strstr(stub_log_last, "DEV_ID reads 0xDECA0131"));

    /* The PLL never locks. */
    chip_reset();
    pll_never = true;
    assert(dw1000_init(&c, &h) == RET(-EIO));
    assert(!h && allocs == frees);

    /* A config refused before anything is allocated. */
    allocs = 0;
    c.channel = 6;
    assert(dw1000_init(&c, &h) == RET(-EINVAL));
    c = base_cfg();
    c.preamble_code = 25;
    assert(dw1000_init(&c, &h) == RET(-EINVAL));
    assert(dw1000_init(NULL, &h) == RET(-EINVAL));
    c = base_cfg();
    assert(dw1000_init(&c, NULL) == RET(-EINVAL));
    assert(allocs == 0);

    /*
     * Every transaction of init failing in turn: the bus gone from there
     * on, or that one transaction alone -- which only a check of that very
     * result catches. esp-dw1000-uwb read a failed register as 0 and
     * ignored every configuration write's result; now each failure stops
     * init with its errno, and nothing leaks.
     */
    chip_reset();
    h = open_cfg(&c);
    good = xfers;
    dw1000_del(h);
    assert(good > 40);
    for (int once = 0; once < 2; once++) {
        for (int k = 0; k < good; k++) {
            chip_reset();
            spi_fail_at = k;
            spi_fail_once = once;
            allocs = frees = 0;
            h = NULL;
            assert(dw1000_init(&c, &h) == RET(-ETIMEDOUT));
            assert(!h && allocs == frees);
        }
    }
    spi_fail_once = false;
}

static void test_send_recv(void)
{
    dw1000_handle_t h = open_ok();
    struct uwb_dev *u = dw1000_uwb(h);
    static const uint8_t payload[5] = { 1, 2, 3, 4, 5 };
    uint8_t big[126] = { 0 };
    uint8_t buf[16];
    uint64_t ts;
    size_t len;

    /* Out: the buffer, TFLEN with the FCS, TXFRS waited for and cleared. */
    assert(dw1000_send(h, payload, sizeof payload) == RET(0));
    assert(last_tx_len == 5 && !memcmp(last_tx, payload, 5));
    assert((get_le(DW_TX_FCTRL, 0, 4) & 0x7F) == 7);
    assert(!(get_le(DW_SYS_STATUS, 0, 5) & SYS_STATUS_TXFRS));
    assert(uwb_tx_timestamp(u, &ts) == 0 && ts == last_tx_ts);
    assert(dw1000_tx_timestamp(h) == last_tx_ts);
    assert(dw1000_send(h, big, sizeof big) == RET(-EMSGSIZE));

    /* In: the payload without its FCS, cut to the buffer, stamped. */
    air_queue("hello", 5, 0x0102030405ULL, false);
    assert(dw1000_recv(h, buf, sizeof buf, &len, 50) == RET(0));
    assert(len == 5 && !memcmp(buf, "hello", 5));
    assert(uwb_rx_timestamp(u, &ts) == 0 && ts == 0x0102030405ULL);
    air_queue("longer frame", 12, 7, false);
    assert(uwb_recv(u, buf, 4, &len, 50) == 0 && len == 4);

    /* Nothing in time: a timeout, the receiver reset (UM p.182). */
    soft_resets_rx = 0;
    assert(dw1000_recv(h, buf, sizeof buf, &len, 20) == RET(-ETIMEDOUT));
    assert(soft_resets_rx == 1);
    /* A frame in error: -EIO, the receiver reset too. */
    air_queue("x", 1, 0, true);
    assert(dw1000_recv(h, buf, sizeof buf, &len, 50) == RET(-EIO));
    assert(soft_resets_rx == 2);

    assert(uwb_systime(u, &ts) == 0 && ts != 0);
    dw1000_del(h);
}

/*
 * A read that fails is a failure now, not a 0. Timestamps through the
 * category say so and leave the value alone; the handle's own uint64_t
 * calls give 0, as before.
 */
static void test_read_failures(void)
{
    dw1000_handle_t h = open_ok();
    struct uwb_dev *u = dw1000_uwb(h);
    uint8_t buf[8];
    uint64_t ts = 77;
    size_t len;

    spi_fail_at = xfers;
    assert(uwb_rx_timestamp(u, &ts) == -ETIMEDOUT && ts == 77);
    assert(dw1000_rx_timestamp(h) == 0);
    assert(dw1000_recv(h, buf, sizeof buf, &len, 50) == RET(-ETIMEDOUT));
    assert(dw1000_set_antenna_delay(h, 1) == RET(-ETIMEDOUT));
    spi_fail_at = -1;
    assert(dw1000_set_antenna_delay(h, 16400) == RET(0));
    assert(get_le(DW_TX_ANTD, 0, 2) == 16400);
    dw1000_del(h);
}

/* ---- ranging, against a scripted peer ---- */

static uint64_t peer_t1, peer_t4, peer_t5;
static uint8_t peer_seq;
static bool peer_stale;
static float peer_report = 12.5f;

/* The responder at address 2: RESP to a POLL, REPORT to a FINAL. */
static void responder(const uint8_t *f, size_t len, uint64_t ts)
{
    uint8_t r[TWR_HDR_LEN + 4];

    if (len >= TWR_HDR_LEN && f[0] == 0xA1) {           /* POLL */
        peer_t1 = ts;
        peer_seq = f[1];
        twr_put_hdr(r, 0xA2, (uint8_t)(peer_stale ? peer_seq + 1 : peer_seq),
                    2, twr_src(f));
        peer_t4 = ts + 100000000ULL + 2 * 3000;
        air_queue(r, TWR_HDR_LEN, peer_t4, false);
    } else if (len >= TWR_HDR_LEN + 15 && f[0] == 0xA3) {   /* FINAL */
        uint64_t t[3] = { 0 };

        for (int k = 0; k < 3; k++)
            for (int i = 4; i >= 0; i--)
                t[k] = (t[k] << 8) | f[TWR_HDR_LEN + 5 * k + i];
        /* The FINAL carries what the initiator saw, and its own send time. */
        assert(t[0] == peer_t1 && t[1] == peer_t4 && t[2] == ts);
        peer_t5 = ts;
        twr_put_hdr(r, 0xA4, peer_seq, 2, twr_src(f));
        memcpy(r + TWR_HDR_LEN, &peer_report, sizeof(float));
        air_queue(r, sizeof r, ts + 100000000ULL, false);
    }
}

static void test_twr_range(void)
{
    dw1000_config_t c = base_cfg();
    dw1000_handle_t h;
    float dist = 0.0f;
    int sent;

    /* The initiator is address 1; the REPORT's distance comes back. */
    c.addr = 1;
    h = open_cfg(&c);
    peer_on_tx = responder;
    peer_stale = false;
    assert(dw1000_twr_range(h, 2, &dist) == RET(0));
    assert(dist == 12.5f && peer_t5 != 0);
    /* The FINAL went out at a delayed slot: DX_TIME on a 512-tick grid. */
    assert((get_le(DW_DX_TIME, 0, 5) & 0x1FF) == 0);

    /* A RESP with another exchange's sequence number is dropped. */
    peer_stale = true;
    assert(dw1000_twr_range(h, 2, &dist) == RET(-EBADMSG));
    peer_stale = false;

    /* The FINAL's slot already missed: -ETIME, and nothing was sent. */
    rxq_n = 0;
    peer_on_tx = responder;
    sent = tx_count;
    late_next_tx = true;
    assert(dw1000_twr_range(h, 2, &dist) == RET(-ETIME));
    assert(tx_count == sent + 1);       /* the POLL, not the FINAL */
    assert(!(get_le(DW_SYS_STATUS, 0, 5) & SYS_STATUS_HPDWARN));

    /* No answer at all. */
    peer_on_tx = NULL;
    rxq_n = 0;
    assert(dw1000_twr_range(h, 2, &dist) == RET(-ETIMEDOUT));
    dw1000_del(h);
}

/* The initiator at address 1, for a responder under test: it answers the
 * RESP with a FINAL whose timestamps put the two 10 m apart. */
static uint64_t tof_ticks, init_t1, init_t2;
static float reported;

static void initiator(const uint8_t *f, size_t len, uint64_t ts)
{
    if (len >= TWR_HDR_LEN && f[0] == 0xA2) {           /* RESP, sent at t3 */
        uint64_t t4 = ts + tof_ticks;
        uint64_t t5 = t4 + 120000000ULL;
        uint64_t t6 = t5 + tof_ticks;
        uint8_t r[TWR_HDR_LEN + 15];
        uint64_t t[3] = { init_t1, t4, t5 };

        twr_put_hdr(r, 0xA3, f[1], 1, 2);
        for (int k = 0; k < 3; k++)
            for (int i = 0; i < 5; i++)
                r[TWR_HDR_LEN + 5 * k + i] = (uint8_t)(t[k] >> (8 * i));
        air_queue(r, sizeof r, t6, false);
    } else if (len >= TWR_HDR_LEN + 4 && f[0] == 0xA4) {    /* REPORT */
        memcpy(&reported, f + TWR_HDR_LEN, sizeof(float));
    }
}

static void test_twr_respond(void)
{
    dw1000_handle_t h = open_ok();
    uint8_t poll[TWR_HDR_LEN];
    float dist = 0.0f;

    tof_ticks = (uint64_t)(10.0 / (DW_TIME_UNIT * DW_SPEED_OF_LIGHT));
    init_t1 = 5000;
    init_t2 = init_t1 + tof_ticks;
    peer_on_tx = initiator;
    reported = 0.0f;

    /* A POLL for another responder first: ignored, and listened past. */
    twr_put_hdr(poll, 0xA1, 9, 1, 7);
    air_queue(poll, sizeof poll, 1000, false);
    twr_put_hdr(poll, 0xA1, 9, 1, 2);
    air_queue(poll, sizeof poll, init_t2, false);

    assert(dw1000_twr_respond(h, &dist, 500) == RET(0));
    assert(fabs(dist - 10.0) < 0.01);
    assert(reported == dist);
    /* The REPORT went to the initiator, with the POLL's sequence number. */
    assert(last_tx[0] == 0xA4 && last_tx[1] == 9 && twr_dst(last_tx) == 1);

    /* No POLL in time. */
    peer_on_tx = NULL;
    assert(dw1000_twr_respond(h, &dist, 30) == RET(-ETIMEDOUT));
    dw1000_del(h);
}

static void test_null_arguments(void)
{
    dw1000_handle_t h;
    uint8_t b[4] = { 0 };
    float f;

    assert(dw1000_send(NULL, b, 1) == RET(-EINVAL));
    assert(dw1000_recv(NULL, b, 1, NULL, 1) == RET(-EINVAL));
    assert(dw1000_set_antenna_delay(NULL, 1) == RET(-EINVAL));
    assert(dw1000_del(NULL) == RET(-EINVAL));
    assert(dw1000_twr_range(NULL, 1, &f) == RET(-EINVAL));
    assert(dw1000_twr_respond(NULL, &f, 1) == RET(-EINVAL));
    assert(dw1000_tx_timestamp(NULL) == 0 && dw1000_systime(NULL) == 0);
    h = open_ok();
    assert(dw1000_send(h, NULL, 1) == RET(-EINVAL));
    assert(dw1000_recv(h, NULL, 1, NULL, 1) == RET(-EINVAL));
    dw1000_del(h);
}

static void run_dw1000_cases(void)
{
    test_init();
    test_channel_3();
    test_init_failures();
    test_send_recv();
    test_read_failures();
    test_twr_range();
    test_twr_respond();
    test_null_arguments();
}
