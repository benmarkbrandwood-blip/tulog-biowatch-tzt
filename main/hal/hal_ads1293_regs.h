#pragma once
#include <stdint.h>

/* ADS1293 SPI command byte encoding */
#define ADS1293_CMD_WRITE(addr)  ((uint8_t)((addr) & 0x7F))
#define ADS1293_CMD_READ(addr)   ((uint8_t)(0x80u | ((addr) & 0x7F)))

/* ── Control registers ────────────────────────────────────────────────────── */
#define ADS1293_CONFIG          0x00  /* bit0 = START_CON */
#define ADS1293_FLEX_CH1_CN     0x01  /* CH1 flex input routing */
#define ADS1293_FLEX_CH2_CN     0x02  /* CH2 flex input routing */
#define ADS1293_FLEX_CH3_CN     0x03  /* CH3 flex input routing */
#define ADS1293_LOD_CN          0x04  /* Lead-off detection */
#define ADS1293_LOD_EN          0x05  /* Lead-off enable per input */
#define ADS1293_LOD_CURRENT     0x06  /* Lead-off current level */
#define ADS1293_LOD_AC_CN       0x07  /* Lead-off AC config */
#define ADS1293_CMDET_EN        0x0A  /* Common-mode detect enable */
#define ADS1293_CMDET_CN        0x0B  /* CM detect routing */
#define ADS1293_RLD_CN          0x0C  /* RLD amplifier connection */
#define ADS1293_WILSON_EN1      0x0D  /* Wilson reference buffer 1 */
#define ADS1293_WILSON_EN2      0x0E  /* Wilson reference buffer 2 */
#define ADS1293_WILSON_EN3      0x0F  /* Wilson reference buffer 3 */
#define ADS1293_WILSON_CN       0x10  /* Wilson reference output */
#define ADS1293_REF_CN          0x11  /* Reference configuration */
#define ADS1293_OSC_CN          0x12  /* Oscillator control */
#define ADS1293_AFE_RES         0x13  /* AFE resolution: bit[n-1] = hi-res for CHn */
#define ADS1293_AFE_SHDN_CN     0x14  /* AFE shutdown per channel */
#define ADS1293_ERROR_LOD       0x18  /* Lead-off error status */
#define ADS1293_ERROR_STATUS    0x19  /* General error status */
#define ADS1293_ERROR_RANGE1    0x1A  /* Out-of-range CH1 */
#define ADS1293_ERROR_RANGE2    0x1B  /* Out-of-range CH2 */
#define ADS1293_ERROR_RANGE3    0x1C  /* Out-of-range CH3 */
#define ADS1293_ERROR_SYNC      0x1D  /* Sync error status */

/* ── Decimation rate registers ───────────────────────────────────────────── */
#define ADS1293_R2_RATE         0x21  /* R2 for all channels */
#define ADS1293_R3_RATE_CH1     0x22  /* R3 channel 1 */
#define ADS1293_R3_RATE_CH2     0x23  /* R3 channel 2 */
#define ADS1293_R3_RATE_CH3     0x24  /* R3 channel 3 */
#define ADS1293_R1_RATE         0x25  /* R1 pace decimation */
#define ADS1293_DIS_EFILTER     0x26  /* ECG filter disable */
#define ADS1293_DRDYB_SRC       0x27  /* Data-ready pin source */
#define ADS1293_SYNCB_CN        0x28  /* SYNCB pin control */
#define ADS1293_MASK_DRDYB      0x29  /* DRDYB optional mask */
#define ADS1293_MASK_ERR        0x2A  /* ALARMB mask */
#define ADS1293_ALARM_FILTER    0x2E  /* Alarm digital filter */
#define ADS1293_CH_CNFG         0x2F  /* Loop readback channel enable */

/* ── Data / status registers ─────────────────────────────────────────────── */
#define ADS1293_DATA_STATUS     0x30  /* Pace/ECG data-ready status */
#define ADS1293_DATA_CH1_PACE_H 0x31  /* CH1 pace data, upper byte */
#define ADS1293_DATA_CH1_PACE_L 0x32  /* CH1 pace data, lower byte */
#define ADS1293_DATA_CH2_PACE_H 0x33
#define ADS1293_DATA_CH2_PACE_L 0x34
#define ADS1293_DATA_CH3_PACE_H 0x35
#define ADS1293_DATA_CH3_PACE_L 0x36
#define ADS1293_DATA_CH1_ECG_H  0x37  /* CH1 ECG upper byte (24-bit, MSB first) */
#define ADS1293_DATA_CH1_ECG_M  0x38
#define ADS1293_DATA_CH1_ECG_L  0x39
#define ADS1293_DATA_CH2_ECG_H  0x3A
#define ADS1293_DATA_CH2_ECG_M  0x3B
#define ADS1293_DATA_CH2_ECG_L  0x3C
#define ADS1293_DATA_CH3_ECG_H  0x3D
#define ADS1293_DATA_CH3_ECG_M  0x3E
#define ADS1293_DATA_CH3_ECG_L  0x3F
#define ADS1293_REVID           0x40  /* Revision ID — always 0x01 */
#define ADS1293_DATA_LOOP       0x50  /* Loop read-back start address */

/* ── Key register values ─────────────────────────────────────────────────── */

/* CONFIG (0x00) */
#define ADS1293_CONFIG_STOP     0x00
#define ADS1293_CONFIG_START    0x01

/* AFE_RES (0x11) */
#define ADS1293_AFE_RES_CH1_HI  (1u << 0)
#define ADS1293_AFE_RES_CH2_HI  (1u << 1)
#define ADS1293_AFE_RES_CH3_HI  (1u << 2)

/* OSC_CN (0x12)
 * bit 2 = CLKEXT: 1 = external crystal on XTAL pins; 0 = internal RC oscillator.
 * bit 1 = CLK input from CLK pin (slave multi-device mode).
 * bit 0 = CLK output enable on CLK pin (master multi-device mode). */
#define ADS1293_OSC_INTERNAL    0x04  /* bit2=CLKFED: feed oscillator to digital; RC runs without crystal */
#define ADS1293_OSC_EXTERNAL    0x04  /* same register value; crystal vs RC is determined by hardware only */

/* DRDYB_SRC (0x27) — which channel drives the DRDYB pin */
#define ADS1293_DRDYB_CH1_PACE  0x01
#define ADS1293_DRDYB_CH2_PACE  0x02
#define ADS1293_DRDYB_CH3_PACE  0x04
#define ADS1293_DRDYB_CH1_ECG   0x08
#define ADS1293_DRDYB_CH2_ECG   0x10
#define ADS1293_DRDYB_CH3_ECG   0x20

/* CH_CNFG (0x2F) — which channels are included in DATA_LOOP readback */
#define ADS1293_CHCNFG_STS_EN   (1u << 0)  /* DATA_STATUS */
#define ADS1293_CHCNFG_P1_EN    (1u << 1)  /* CH1 pace */
#define ADS1293_CHCNFG_P2_EN    (1u << 2)
#define ADS1293_CHCNFG_P3_EN    (1u << 3)
#define ADS1293_CHCNFG_E1_EN    (1u << 4)  /* CH1 ECG */
#define ADS1293_CHCNFG_E2_EN    (1u << 5)  /* CH2 ECG */
#define ADS1293_CHCNFG_E3_EN    (1u << 6)  /* CH3 ECG */

/* DATA_STATUS (0x30) DRDY bits */
#define ADS1293_STATUS_ALARMB   (1u << 1)
#define ADS1293_STATUS_P1_DRDY  (1u << 2)
#define ADS1293_STATUS_P2_DRDY  (1u << 3)
#define ADS1293_STATUS_P3_DRDY  (1u << 4)
#define ADS1293_STATUS_E1_DRDY  (1u << 5)
#define ADS1293_STATUS_E2_DRDY  (1u << 6)
#define ADS1293_STATUS_E3_DRDY  (1u << 7)

/* Expected REVID value */
#define ADS1293_REVID_EXPECTED  0x01

/* Byte count for a CH1+CH2 ECG loop read (cmd byte excluded) */
#define ADS1293_LOOP_E1E2_BYTES 6

/* R2_RATE / R3_RATE encoding — bits [1:0] only
 * ODR = 102400 / (R1 × R2 × R3), R1 = 4 (fixed for ECG mode)
 *
 * R2_RATE [1:0]:   00=R2×2  01=R2×4  10=R2×5  11=R2×8
 * R3_RATE [1:0]:   00=R3×2  01=R3×4  10=R3×6  11=R3×8
 *
 * Useful operating points:
 *   ECG record (~853 SPS): R2=5 (0x02), R3=6 (0x02) → 102400/120 = 853 SPS
 *   BP / PTT  (~1067 SPS): R2=4 (0x01), R3=6 (0x02) → 102400/96  = 1067 SPS
 *   High res  (~1280 SPS): R2=5 (0x02), R3=4 (0x01) → 102400/80  = 1280 SPS
 */
#define ADS1293_R2_VAL_2   0x00   /* R2 = 2 */
#define ADS1293_R2_VAL_4   0x01   /* R2 = 4 */
#define ADS1293_R2_VAL_5   0x02   /* R2 = 5 (default) */
#define ADS1293_R2_VAL_8   0x03   /* R2 = 8 */

#define ADS1293_R3_VAL_2   0x00   /* R3 = 2 */
#define ADS1293_R3_VAL_4   0x01   /* R3 = 4 */
#define ADS1293_R3_VAL_6   0x02   /* R3 = 6 (default) */
#define ADS1293_R3_VAL_8   0x03   /* R3 = 8 */
