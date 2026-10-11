/*
 * DW1000 register map and per-configuration tuning constants.
 *
 * Every value here is quoted from DecaWave's DW1000 User Manual v2.05
 * (docs/DW1000_User_Manual_v2.05.pdf, "UM") or the DW1000 data sheet
 * (docs/DW1000_Data_Sheet.pdf, "DS"), with its section, table and page.
 * The register file ids are UM Table 15 (p.62-63); each sub-register and
 * bit names its 7.2.x entry. esp-dw1000-uwb wrote them without the manual
 * and cross-checked them against Zephyr's driver; against the manual, two
 * values differ from that driver, and this file follows the manual:
 *
 *   - FS_PLLTUNE for channel 3 is 0x5E (UM Table 41), not the 0x56 of
 *     DecaWave's reference code. To be confirmed on a bench, on channel 3.
 *   - TX_POWER holds the manual-mode values of UM Table 19, which apply
 *     with smart TX power off -- so dw_sys_cfg() now sets DIS_STXP, which
 *     the original never did.
 *
 * A wrong tuning word degrades ranging accuracy silently rather than failing
 * loudly; UM 2.5.5, "Default configurations that should be modified", lists
 * the ones that matter.
 */
#pragma once

#include <stdint.h>

/* ---- Register file IDs (6-bit, carried in the SPI transaction header):
 * UM Table 15, p.62-63; lengths in octets as there ---- */
#define DW_DEV_ID      0x00 /* 4 bytes, reads 0xDECA0130; UM 7.2.2, p.65 */
#define DW_EUI         0x01 /* 8 */
#define DW_PANADR      0x03 /* 4 */
#define DW_SYS_CFG     0x04 /* 4 */
#define DW_SYS_TIME    0x06 /* 5 */
#define DW_TX_FCTRL    0x08 /* 5 */
#define DW_TX_BUFFER   0x09 /* 1024 */
#define DW_DX_TIME     0x0A /* 5 */
#define DW_RX_FWTO     0x0C /* 2 */
#define DW_SYS_CTRL    0x0D /* 4 */
#define DW_SYS_MASK    0x0E /* 4 */
#define DW_SYS_STATUS  0x0F /* 5 */
#define DW_RX_FINFO    0x10 /* 4 */
#define DW_RX_BUFFER   0x11 /* 1024 */
#define DW_RX_TIME     0x15 /* 14, RX_STAMP is the first 5 (UM 7.2.23, p.99) */
#define DW_TX_TIME     0x17 /* 10, TX_STAMP is the first 5 (UM 7.2.25, p.100) */
#define DW_TX_ANTD     0x18 /* 2 */
#define DW_ACK_RESP_T  0x1A /* 4 */
#define DW_TX_POWER    0x1E /* 4 */
#define DW_CHAN_CTRL   0x1F /* 4 */
#define DW_AGC_CTRL    0x23
#define DW_EXT_SYNC    0x24
#define DW_DRX_CONF    0x27
#define DW_RF_CONF     0x28
#define DW_TX_CAL      0x2A
#define DW_FS_CTRL     0x2B
#define DW_AON         0x2C
#define DW_OTP_IF      0x2D
#define DW_LDE_CTRL    0x2E
#define DW_PMSC        0x36

/* ---- Sub-addresses within the register files above: UM 7.2.x, page ---- */
#define DW_AGC_TUNE1   0x04   /* 7.2.36.3, p.115 */
#define DW_AGC_TUNE2   0x0C   /* 7.2.36.5, p.116 */
#define DW_AGC_TUNE3   0x12   /* 7.2.36.7, p.116 */
#define DW_EC_CTRL     0x00   /* 7.2.37.1, p.118 */
#define DW_DRX_TUNE0b  0x02   /* 7.2.40.2, p.135 */
#define DW_DRX_TUNE1a  0x04   /* 7.2.40.3, p.136 */
#define DW_DRX_TUNE1b  0x06   /* 7.2.40.4, p.136 */
#define DW_DRX_TUNE2   0x08   /* 7.2.40.5, p.137 */
#define DW_DRX_SFDTOC  0x20   /* 7.2.40.7, p.137 */
#define DW_DRX_TUNE4H  0x26   /* 7.2.40.10, p.139 */
#define DW_RF_RXCTRLH  0x0B   /* 7.2.41.3, p.141 */
#define DW_RF_TXCTRL   0x0C   /* 7.2.41.4, p.141 */
#define DW_TC_PGDELAY  0x0B   /* 7.2.43.4, p.147 */
#define DW_FS_PLLCFG   0x07   /* 7.2.44.2, p.149 */
#define DW_FS_PLLTUNE  0x0B   /* 7.2.44.3, p.149 */
#define DW_FS_XTALT    0x0E   /* 7.2.44.5, p.150 */
#define DW_OTP_ADDR    0x04   /* 7.2.46.2, p.161 */
#define DW_OTP_CTRL    0x06   /* 7.2.46.3, p.162 */
#define DW_OTP_RDAT    0x0A   /* 7.2.46.5, p.164 */
#define DW_OTP_SF      0x12   /* 7.2.46.7, p.164 */
#define DW_LDE_CFG1    0x0806 /* 7.2.47.2, p.167 */
#define DW_LDE_RXANTD  0x1804 /* 7.2.47.5, p.168 */
#define DW_LDE_CFG2    0x1806 /* 7.2.47.6, p.169 */
#define DW_LDE_REPC    0x2804 /* 7.2.47.7, p.169 */
#define DW_PMSC_CTRL0  0x00   /* 7.2.50.1, p.180 */
#define DW_PMSC_SOFTRESET 0x03 /* byte 3 of PMSC_CTRL0: SOFTRESET is bits 31:28, p.182 */
#define DW_PMSC_CTRL1  0x04   /* 7.2.50.2, p.182 */

/* ---- SYS_CTRL bits: UM 7.2.15, p.80-81 ---- */
#define SYS_CTRL_SFCST      (1UL << 0)
#define SYS_CTRL_TXSTRT     (1UL << 1)
#define SYS_CTRL_TXDLYS     (1UL << 2)
#define SYS_CTRL_TRXOFF     (1UL << 6)
#define SYS_CTRL_WAIT4RESP  (1UL << 7)
#define SYS_CTRL_RXENAB     (1UL << 8)
#define SYS_CTRL_RXDLYE     (1UL << 9)

/* ---- SYS_STATUS bits: UM 7.2.17, p.85-92; TXPUTE is octet 4 bit 2 ---- */
#define SYS_STATUS_CPLOCK   (1UL << 1)  /* clock PLL locked */
#define SYS_STATUS_TXFRS    (1UL << 7)  /* transmit frame sent */
#define SYS_STATUS_LDEDONE  (1UL << 10)
#define SYS_STATUS_RXPHE    (1UL << 12) /* PHY header error */
#define SYS_STATUS_RXDFR    (1UL << 13) /* data frame ready */
#define SYS_STATUS_RXFCG    (1UL << 14) /* FCS good */
#define SYS_STATUS_RXFCE    (1UL << 15) /* FCS error */
#define SYS_STATUS_RXRFSL   (1UL << 16) /* reed-solomon sync loss */
#define SYS_STATUS_RXRFTO   (1UL << 17) /* frame wait timeout */
#define SYS_STATUS_LDEERR   (1UL << 18)
#define SYS_STATUS_RXOVRR   (1UL << 20)
#define SYS_STATUS_RXPTO    (1UL << 21) /* preamble detect timeout */
#define SYS_STATUS_RXSFDTO  (1UL << 26)
#define SYS_STATUS_HPDWARN  (1UL << 27) /* delayed TX/RX time already missed */
#define SYS_STATUS_AFFREJ   (1UL << 29)
#define SYS_STATUS_TXPUTE   (1ULL << 34) /* no time to power up for a delayed send */

#define SYS_STATUS_ALL_RX_GOOD (SYS_STATUS_RXDFR | SYS_STATUS_RXFCG | SYS_STATUS_LDEDONE)
#define SYS_STATUS_ALL_RX_ERR                                                          \
    (SYS_STATUS_RXPHE | SYS_STATUS_RXFCE | SYS_STATUS_RXRFSL | SYS_STATUS_RXSFDTO |    \
     SYS_STATUS_AFFREJ | SYS_STATUS_LDEERR)
#define SYS_STATUS_ALL_RX_TO (SYS_STATUS_RXRFTO | SYS_STATUS_RXPTO)

/* HPDWARN (bit 27) and TXPUTE (bit 34, no time left to power up the
 * transmitter), as seen by a 16-bit access at SYS_STATUS offset 3. This is
 * Decawave's SYS_STATUS_TXERR, the test its dwt_starttx makes after TXDLYS. */
#define SYS_STATUS_TXERR_AT3 0x0408U

/* ---- SYS_CFG bits: UM 7.2.6, p.68-72 ---- */
#define SYS_CFG_RXAUTR      (1UL << 29) /* receiver auto re-enable */
#define SYS_CFG_RXWTOE      (1UL << 28) /* receive wait timeout enable */
#define SYS_CFG_RXM110K     (1UL << 22) /* 110 kbps receiver mode */
#define SYS_CFG_DIS_STXP    (1UL << 18) /* smart TX power off, p.71 */
#define SYS_CFG_DIS_DRXB    (1UL << 12) /* single RX buffer; reset default */
#define SYS_CFG_HIRQ_POL    (1UL << 9)  /* IRQ active high; reset default */

/* ---- EC_CTRL bits: UM 7.2.37.1, p.118; the PLLLDT note, p.85 ---- */
#define EC_CTRL_PLLLDT      (1UL << 2)  /* PLL lock detect; makes CPLOCK reliable */

/* ---- OTP_SF bits: UM 7.2.46.7, p.165 ---- */
#define OTP_SF_LDO_KICK     (1UL << 1)  /* load LDOTUNE_CAL from OTP */

/* ---- OTP addresses holding factory calibration: UM Table 10, p.58 ---- */
#define OTP_XTAL_TRIM  0x01E
#define OTP_LDOTUNE    0x004

/* ==========================================================================
 * Configuration-dependent tuning tables, each from its UM table.
 * ========================================================================== */

/* AGC_TUNE1 -- indexed by PRF (0 = 16 MHz, 1 = 64 MHz). UM Table 22, p.115 */
static const uint16_t dw_agc_tune1[2] = { 0x8870, 0x889B };
#define DW_AGC_TUNE2_VAL 0x2502A907UL /* constant; UM Table 23, p.116 */
#define DW_AGC_TUNE3_VAL 0x0035U      /* constant; UM Table 24, p.116 */

/* DRX_TUNE0b -- indexed by data rate (0 = 110k, 1 = 850k, 2 = 6.8M).
 * These are the STANDARD-SFD values. Decawave's proprietary SFD needs a
 * different set (110k 0x0016, 850k 0x0006, 6.8M 0x0002); that choice and
 * CHAN_CTRL bit 17 must always be changed together. UM Table 28, p.135 */
static const uint16_t dw_drx_tune0b[3] = { 0x000A, 0x0001, 0x0001 };

/* DRX_TUNE1a -- indexed by PRF. UM Table 29, p.136 */
static const uint16_t dw_drx_tune1a[2] = { 0x0087, 0x008D };

/* DRX_TUNE2 -- [PAC size index][PRF]. PAC index 0..3 = PAC 8/16/32/64.
 * UM Table 31, p.137; the PAC for each preamble length is UM Table 6, p.32. */
static const uint32_t dw_drx_tune2[4][2] = {
    { 0x311A002DUL, 0x313B006BUL },
    { 0x331A0052UL, 0x333B00BEUL },
    { 0x351A009AUL, 0x353B015EUL },
    { 0x371A011DUL, 0x373B0296UL },
};

/* RF_RXCTRLH -- indexed by channel 1,2,3,4,5,7 (see dw_chan_index).
 * UM Table 34, p.141 */
static const uint8_t dw_rf_rxctrlh[6] = { 0xD8, 0xD8, 0xD8, 0xBC, 0xD8, 0xBC };

/* RF_TXCTRL -- per channel. UM Table 35, p.142 */
static const uint32_t dw_rf_txctrl[6] = {
    0x00005C40UL, 0x00045CA0UL, 0x00086CC0UL, 0x00045C80UL, 0x001E3FE0UL, 0x001E7DE0UL,
};

/* TC_PGDELAY -- per channel, sets the transmit pulse bandwidth. UM Table 37, p.147 */
static const uint8_t dw_tc_pgdelay[6] = { 0xC9, 0xC2, 0xC5, 0x95, 0xC0, 0x93 };

/* FS_PLLCFG and FS_PLLTUNE -- per channel. UM Tables 40 and 41, p.149.
 * Channel 3's PLLTUNE is the manual's 0x5E; esp-dw1000-uwb had 0x56, the
 * value in DecaWave's reference code. Confirm on a bench, on channel 3. */
static const uint32_t dw_fs_pllcfg[6] = {
    0x09000407UL, 0x08400508UL, 0x08401009UL, 0x08400508UL, 0x0800041DUL, 0x0800041DUL,
};
static const uint8_t dw_fs_plltune[6] = { 0x1E, 0x26, 0x5E, 0x26, 0xBE, 0xBE };

/* LDE_CFG1 is constant: PMULT 3 and NTM 13, the NTM "used in our device
 * driver software" (UM 7.2.47.2, p.167). LDE_CFG2 is indexed by PRF, UM
 * Table 47, p.169. */
#define DW_LDE_CFG1_VAL 0x6DU
static const uint16_t dw_lde_cfg2[2] = { 0x1607, 0x0607 };

/* TX_POWER for manual transmit power -- smart TX power disabled, which
 * dw_sys_cfg() does with DIS_STXP -- [channel][PRF]. UM Table 19, p.107. */
static const uint32_t dw_tx_power[6][2] = {
    { 0x75757575UL, 0x67676767UL }, /* ch 1 */
    { 0x75757575UL, 0x67676767UL }, /* ch 2 */
    { 0x6F6F6F6FUL, 0x8B8B8B8BUL }, /* ch 3 */
    { 0x5F5F5F5FUL, 0x9A9A9A9AUL }, /* ch 4 */
    { 0x48484848UL, 0x85858585UL }, /* ch 5 */
    { 0x92929292UL, 0xD1D1D1D1UL }, /* ch 7 */
};

/* LDE_REPC, indexed by preamble code 1..24. Entry 0 is unused padding.
 * Read through dw_lde_repc_val, which scales for 110 kbps. UM Table 48, p.169-170 */
static const uint16_t dw_lde_repc[25] = {
    0x0000, 0x5998, 0x5998, 0x51EA, 0x428E, 0x451E, 0x2E14, 0x8000, 0x51EA,
    0x28F4, 0x3332, 0x3AE0, 0x3D70, 0x3AE0, 0x35C2, 0x2B84, 0x35C2, 0x3332,
    0x35C2, 0x35C2, 0x47AE, 0x3AE0, 0x3850, 0x30A2, 0x3850,
};

/* LDE_REPC for a preamble code and data rate index (0 = 110k). At 110 kbps
 * the value is divided by 8 (UM Table 48 note, p.170). */
static inline uint16_t dw_lde_repc_val(int code, int rate)
{
    return rate == 0 ? dw_lde_repc[code] >> 3 : dw_lde_repc[code];
}

/* Maps a user-facing channel number to an index into the tables above. */
static inline int dw_chan_index(int ch)
{
    switch (ch) {
    case 1: return 0;
    case 2: return 1;
    case 3: return 2;
    case 4: return 3;
    case 5: return 4;
    case 7: return 5;
    default: return -1;
    }
}

/* TX_FCTRL encodes the preamble length as a 4-bit symbol-count code: PE in
 * bits 3:2, TXPSR in bits 1:0. UM Table 16, p.76. */
static inline uint32_t dw_preamble_field(uint16_t len)
{
    switch (len) {
    case 64:   return 0x1;
    case 128:  return 0x5;
    case 256:  return 0x9;
    case 512:  return 0xD;
    case 1024: return 0x2;
    case 1536: return 0x6;
    case 2048: return 0xA;
    default:   return 0x3; /* 4096 */
    }
}

/* SYS_CFG for a data rate index (0 = 110k, 1 = 850k, 2 = 6.8M): the reset
 * value 0x00001200, DIS_STXP, and the 110 kbps receiver mode at that rate,
 * written as a whole word so the result never depends on what the register
 * held before. DIS_DRXB matters: the driver does not manage double-buffered
 * RX. DIS_STXP turns smart TX power off, so the TX_POWER values above --
 * the manual-mode ones -- mean what they say (UM 7.2.31.2); esp-dw1000-uwb
 * left it on. RXWTOE stays off; the software wait already bounds
 * dw1000_recv, and RX_FWTO is never programmed. */
static inline uint32_t dw_sys_cfg(int rate)
{
    return SYS_CFG_DIS_STXP | SYS_CFG_DIS_DRXB | SYS_CFG_HIRQ_POL |
           (rate == 0 ? SYS_CFG_RXM110K : 0);
}

/* SPI clock once the PLL is locked: 0 selects 8 MHz. The DW1000 accepts at
 * most 20 MHz then, 3 MHz before (DS Table 18, p.26); anything faster
 * returns 0, "refuse". */
static inline uint32_t dw_fast_spi_hz(uint32_t hz)
{
    if (hz == 0) return 8000000UL;
    return hz <= 20000000UL ? hz : 0;
}

/* TX_FCTRL without the frame length, which dw_prepare_tx adds per frame.
 * `rate` is a data rate index as above, `prf` 0 = 16 MHz, 1 = 64 MHz. */
static inline uint32_t dw_tx_fctrl_base(int rate, int prf, uint16_t preamble_len)
{
    return (dw_preamble_field(preamble_len) << 18) /* TXPSR 19:18, PE 21:20 (UM p.75-76) */
         | ((uint32_t)rate << 13)                 /* TXBR 14:13: 0 110k, 1 850k, 2 6.8M */
         | ((uint32_t)(prf ? 2 : 1) << 16);       /* TXPRF 17:16 */
}
