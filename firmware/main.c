/* RP2350 (RISC-V Hazard3) dual-chamber pacemaker: DDD, with the
 * rate-responsive DDDR and VVIR, and DOO, VVI, VOO and OOO.
 *
 * Raw MMIO only, no pico-sdk -- clocked straight off the 12 MHz crystal 
 * (see clocks_init).  The closed loop is built from four real datasheet 
 * parts and the simulated heart they share, plus a magnet sensor.
 *
 *   - TI ADS1292R on SPI0 (SCK GP18, MOSI GP19, MISO GP16, CS GP17, DRDY GP20):
 *     the sensing front end, 500 SPS at gain 6. CH1 is the ventricular lead
 *     and CH2 the atrial one, with DC lead-off detection on both.
 *   - Microchip MCP4822 on SPI1 (SCK GP10, MOSI GP11, CS GP13): VOUTA charges
 *     the ventricular output capacitor to the programmed amplitude, VOUTB the
 *     atrial one.
 *   - Two TI TS5A3166 analog switches, IN on GP14 (ventricle) and GP15
 *     (atrium): held high for the pulse width, each dumps its capacitor into
 *     its lead.
 *   - TI DRV5032FA Hall switch, OUT on GP21: low while a magnet is over the
 *     device, which forces asynchronous pacing (see the magnet response).
 *   - A lithium/carbon-monofluoride primary cell, VBAT on GP26 (ADC0): the battery
 *     everything runs from. Pacing draws on it pulse by pulse, so its loaded
 *     voltage is what firmware watches for the elective replacement
 *     indicator (see the battery monitor).
 *   - Analog Devices ADXL362 accelerometer, sharing SPI0 with the ADS1292R
 *     on its own CS (GP22), INT1 on GP27: the activity sensor. In DDDR and
 *     VVIR the lower rate follows a sensor-indicated rate derived from how
 *     much the patient moves, and in every mode that senses the atrium the
 *     same sensor feeds a chronotropic-incompetence diagnostic (see rate
 *     response).
 *   - TI RF430CL330H dynamic NFC tag on SPI1 beside the DAC (MISO GP12, CS
 *     GP2, INTO GP3): the telemetry head. A clinic programmer's wand, held
 *     over the implant, reads and writes it (see the telemetry session). Its
 *     files move over SPI1 on DMA, a whole file per transaction (see the
 *     link), and the DAC's writes wait for those to finish.
 *   - Microsemi ZL70102 implant radio, the third part on SPI1 (CS GP5, IRQ
 *     GP6, WU_EN GP7), powered through a TI TPS22860 load switch on GP25: the
 *     same telemetry session without the wand, to a bedside communicator or a
 *     programmer over the MICS band, started by its 2.45 GHz wake-up or by the
 *     implant itself on a schedule (see the radio link). The highest-power
 *     thing the device does, so it is off at ERI. Its register map is the
 *     emulator model's, not the silicon's (see the radio section).
 *
 * DDD timing, the textbook dual-chamber cycle, ventricular-based:
 *
 *   - A ventricular event (VS or VP) starts the VA interval, LRI - PAV. If it
 *     expires with no atrial sense, pace the atrium (AP).
 *   - An atrial event starts the AV delay: PAV after an AP, the shorter SAV
 *     after an AS, since a sensed P wave is only seen partway into it. A VS
 *     inside it inhibits the ventricular pace; otherwise pace (VP) when it
 *     expires. That is what restores AV synchrony in heart block.
 *   - A tracked VP never comes sooner than the upper tracking rate (UTR)
 *     allows after the last ventricular event: a fast atrium is followed with
 *     a growing AV delay until a P wave falls in PVARP and is dropped
 *     (pacemaker Wenckebach).
 *   - The atrium is refractory for the whole AV delay and for the
 *     post-ventricular atrial refractory period (PVARP) after every
 *     ventricular event: a sense there (AR) is reported but not tracked. The
 *     first 100 ms after any ventricular event are blanked on the atrial
 *     channel outright (PVAB) -- a ventricular pace couples 5% of its
 *     amplitude into the atrial lead, and the far-field R wave lives there
 *     too. Each channel is also blanked after its own pace, and the
 *     ventricular one for 30 ms after an atrial pace (crosstalk: an atrial
 *     pulse sensed as a ventricular beat would inhibit the ventricular pace a
 *     heart-block patient depends on).
 *
 * DDDR is DDD with the lower-rate interval taken from the sensor-indicated
 * rate instead of the programmed lower rate; VVIR is VVI with the same.
 *
 * DOO paces both chambers at the lower rate and PAV and ignores sensing, what
 * a magnet does to a dual-chamber device.
 *
 * VVI timing, the single-chamber cycle:
 *
 *   - A ventricular event -- a sensed R wave (VS) or a pace (VP) -- starts the
 *     lower-rate interval (LRI = 60 / rate). If it expires with no sensed
 *     beat, pace.
 *   - It also starts the ventricular refractory period (VRP, 250 ms). The
 *     front end keeps sampling through it, and a sense there is reported as VR
 *     but restarts nothing: the T wave and the pace's own afterpotential live
 *     there. The first 50 ms after a pace are blanked outright. The pulse
 *     rails the sense channel, and nothing in that window is cardiac.
 *
 * VOO paces the ventricle at the lower rate and ignores sensing, and OOO
 * turns pacing off and only senses, in both chambers.
 *
 * Sensing is a 4 ms first difference of each electrogram against that
 * chamber's programmed sensitivity. That is the simplest filter that passes
 * an R wave's (or P wave's) steep upstroke and rejects the slow T wave and
 * the far-field signal of the other chamber: on the atrial lead a 1 mV
 * sensitivity takes the 3 mV P wave's ~3.1 mV slope and leaves the 1 mV
 * far-field R wave's ~0.65 mV.
 *
 * Two serial ports, both 115200 8N1. UART1 (GP4 TX, tty1) is telemetry: the
 * marker channel a pacemaker programmer shows, one line per cardiac event,
 * the lead-off alarm, and a status line every 5 s. It is output only. UART0
 * (GP0/GP1, tty0) is the console: text programming commands, with typed input
 * echoed back. Type `help` there for the command set. Keeping the two apart is what
 * real programmers do too -- a beat-by-beat stream interleaved with a command
 * line makes both unreadable. There is no
 * invented status register anywhere: lead-off comes from the ADS1292R's own
 * LOFF_STAT bits in its data stream, and loss of capture shows up as the
 * patient's own escape beats being sensed between paces, as on a real device.
 *
 * Diagnostics, what a clinic reads back at a follow-up: counters (events,
 * AV sequences, PVCs), rate histograms, an episode log (lead-off, magnet,
 * atrial and ventricular high-rate runs, ERI/EOL, mode changes, the
 * chronotropic diagnostic, power-ons), long-term trends and stored
 * electrograms around the episodes that matter. They live in RAM and are
 * checkpointed to a Microchip AT24C256C EEPROM on I2C0 (SDA GP8, SCL GP9), so
 * a reset keeps them. A clinic programmer reads them out through its wand: a
 * binary session (COBS frames with a CRC-16) over the RF430CL330H, open from
 * OPEN until END or until the wand is taken away -- or, wandless, over the
 * ZL70102 radio, the same frames and the same session; see the diagnostic
 * store and the telemetry session below, and
 * `programmer/pacemaker_programmer.py` for the host side.
 *
 * Integer only, as the other demos in this repo are.
 */
#include <stdint.h>

#define REG(a) (*(volatile uint32_t *)(a))

#define SET_ALIAS 0x2000u
#define CLR_ALIAS 0x3000u

/* --- RESETS (0x40020000, ch 7.5) --- */
#define RESETS_BASE 0x40020000u
#define RESETS_RESET (RESETS_BASE + 0x0)
#define RESETS_RESET_DONE (RESETS_BASE + 0x8)
#define RESET_ADC (1u << 0)
#define RESET_DMA (1u << 2)
#define RESET_I2C0 (1u << 4)
#define RESET_IO_BANK0 (1u << 6)
#define RESET_PADS_BANK0 (1u << 9)
#define RESET_SPI0 (1u << 18)
#define RESET_SPI1 (1u << 19)
#define RESET_TIMER0 (1u << 23)
#define RESET_UART0 (1u << 26)
#define RESET_UART1 (1u << 27)
#define RESET_BITS                                                                                   \
    (RESET_ADC | RESET_DMA | RESET_I2C0 | RESET_IO_BANK0 | RESET_PADS_BANK0 | RESET_SPI0 | RESET_SPI1 | RESET_TIMER0 |      \
     RESET_UART0 | RESET_UART1)

/* --- CLOCKS (0x40010000, ch 8.1) --- */
#define CLOCKS_BASE 0x40010000u
#define CLK_REF_CTRL (CLOCKS_BASE + 0x30)
#define CLK_REF_DIV (CLOCKS_BASE + 0x34)
#define CLK_REF_SELECTED (CLOCKS_BASE + 0x38)
#define CLK_SYS_CTRL (CLOCKS_BASE + 0x3c)
#define CLK_SYS_DIV (CLOCKS_BASE + 0x40)
#define CLK_SYS_SELECTED (CLOCKS_BASE + 0x44)
#define CLK_PERI_CTRL (CLOCKS_BASE + 0x48)
#define CLK_REF_SRC_XOSC 2u
#define CLK_SYS_SRC_AUX 1u
#define CLK_CTRL_ENABLE (1u << 11)

/* --- XOSC (ch 8.2): the 12 MHz crystal, used directly ---
 *
 * No PLL. A pacemaker is a battery device that needs a few MIPS at most, and
 * real ones run far slower still: clk_ref = clk_sys = clk_peri = XOSC. It is
 * also the cheapest clock to emulate -- guest time runs about twelve times
 * closer to wall time than at the usual 150 MHz. */
#define XOSC_BASE 0x40048000u
#define XOSC_CTRL (XOSC_BASE + 0x00)
#define XOSC_STATUS (XOSC_BASE + 0x04)
#define XOSC_STARTUP (XOSC_BASE + 0x0c)
#define XOSC_ENABLE_MAGIC (0xfabu << 12)
#define XOSC_RANGE_1_15MHZ 0xaa0u
#define XOSC_STATUS_STABLE (1u << 31)

/* --- TICKS (0x40108000, ch 8.5) --- */
#define TICKS_BASE 0x40108000u
#define TICKS_TIMER0_CTRL (TICKS_BASE + 0x18)
#define TICKS_TIMER0_CYCLES (TICKS_BASE + 0x1c)
#define TICKS_RISCV_CTRL (TICKS_BASE + 0x3c)
#define TICKS_RISCV_CYCLES (TICKS_BASE + 0x40)
#define TICK_CYCLES_1US 12u

/* --- IO_BANK0 / PADS_BANK0 (ch 9) --- */
#define IO_BANK0_BASE 0x40028000u
#define GPIO_CTRL(n) (IO_BANK0_BASE + 8u * (n) + 4u)
#define FUNCSEL_SPI 1u
#define FUNCSEL_UART 2u
#define FUNCSEL_I2C 3u
#define FUNCSEL_SIO 5u
#define FUNCSEL_NULL 0x1fu

#define PADS_BANK0_BASE 0x40038000u
#define PAD_GPIO(n) (PADS_BANK0_BASE + 4u + 4u * (n))
#define PAD_PDE (1u << 2)
#define PAD_PUE (1u << 3)
#define PAD_IE (1u << 6)
#define PAD_OD (1u << 7)
#define PAD_ISO (1u << 8)

/* --- SIO --- */
#define SIO_BASE 0xd0000000u
#define SIO_GPIO_IN (SIO_BASE + 0x004)
#define SIO_GPIO_OUT_SET (SIO_BASE + 0x018)
#define SIO_GPIO_OUT_CLR (SIO_BASE + 0x020)
#define SIO_GPIO_OE_SET (SIO_BASE + 0x038)

/* --- Board wiring (ROADMAP.md) --- */
#define ADS_MISO_PIN 16u
#define ADS_CS_PIN 17u
#define ADS_SCK_PIN 18u
#define ADS_MOSI_PIN 19u
#define ADS_DRDY_PIN 20u
#define DAC_SCK_PIN 10u
#define DAC_MOSI_PIN 11u
#define DAC_CS_PIN 13u
#define TAG_MISO_PIN 12u   /* SPI1 RX: only the RF430CL330H answers */
#define TAG_CS_PIN 2u      /* RF430CL330H on SPI1, beside the MCP4822 */
#define TAG_INTO_PIN 3u    /* RF430CL330H INTO, driven active low */
#define VPACE_PIN 14u
#define APACE_PIN 15u
#define MAGNET_PIN 21u /* DRV5032FA OUT, push-pull, low with a magnet */
#define VBAT_PIN 26u   /* the cell's terminal, straight to ADC0 */
#define ACCEL_CS_PIN 22u   /* ADXL362 on SPI0, beside the ADS1292R */
#define ACCEL_INT1_PIN 27u /* ADXL362 INT1, active high: FIFO watermark */
#define EEPROM_SDA_PIN 8u  /* I2C0 SDA, AT24C256C */
#define EEPROM_SCL_PIN 9u  /* I2C0 SCL */
#define VBAT_ADC_CHANNEL 0u

/* --- ADC (ch 12.4): 12 bits against the board's regulated 3.3 V --- */
#define ADC_BASE 0x400a0000u
#define ADC_CS (ADC_BASE + 0x00)
#define ADC_RESULT (ADC_BASE + 0x04)
#define ADC_CS_EN (1u << 0)
#define ADC_CS_START_ONCE (1u << 2)
#define ADC_CS_READY (1u << 8)
#define ADC_CS_AINSEL_SHIFT 12u
#define ADC_FULL_SCALE_MV 3300u

/* --- PSM (ch 7.4) and WATCHDOG (ch 12.9): a programmer-commanded reset --- */
#define PSM_WDSEL 0x40018008u
#define PSM_WDSEL_ALL 0x01ffffffu
#define PSM_WDSEL_OSCS 0x0000000cu /* ROSC and XOSC keep running, as in pico-sdk's watchdog_reboot */
#define WATCHDOG_CTRL 0x400d8000u
#define WATCHDOG_CTRL_TRIGGER (1u << 31)

/* --- Why did we reset? (RP2350 datasheet) ---
 *
 * POWMAN.CHIP_RESET (6.4, Table 521) logs the last chip-level reset: bit 18
 * HAD_RUN_LOW is the RUN pin held low, which on this board is the TPS3839
 * supervisor. WATCHDOG.REASON (12.9) is set when the watchdog fired, by timeout
 * (bit 0) or by the programmer's `reboot` (TRIGGER, bit 1), and survives the
 * watchdog's own reset. REASON is read first: CHIP_RESET's HAD_* bits are a log
 * that a later watchdog reset only adds to, so HAD_RUN_LOW can still be set after
 * a commanded reboot. A RUN reset (7.3.1, Table 560) clears REASON, so a
 * supervisor reset that follows a reboot is not mistaken for one. */
#define POWMAN_CHIP_RESET 0x4010002cu
#define POWMAN_HAD_RUN_LOW (1u << 18)
#define WATCHDOG_REASON 0x400d8008u

#define TIMER0_BASE 0x400b0000u
#define TIMER0_TIMERAWL (TIMER0_BASE + 0x28)

/* UART0 = console (GP0 TX, GP1 RX), UART1 = telemetry (GP4 TX). */
#define CONSOLE 0x40070000u
#define TELEMETRY 0x40078000u
#define UART_DR 0x00u
#define UART_FR 0x18u
#define UART_IBRD 0x24u
#define UART_FBRD 0x28u
#define UART_LCR_H 0x2cu
#define UART_CR 0x30u
#define UART_FR_RXFE (1u << 4)
#define UART_FR_TXFF (1u << 5)

/* --- PL022 SPI (ch 12.3) --- */
#define SPI0_BASE 0x40080000u
#define SPI1_BASE 0x40088000u
#define SPI_CR0 0x00u
#define SPI_CR1 0x04u
#define SPI_DR 0x08u
#define SPI_SR 0x0cu
#define SPI_CPSR 0x10u
#define SPI_DMACR 0x24u /* SSPDMACR: lets the DMA see the FIFOs' requests (ch 12.3) */
#define SPI_DMACR_RXDMAE (1u << 0)
#define SPI_DMACR_TXDMAE (1u << 1)
#define SPI_SR_TNF (1u << 1)
#define SPI_SR_RNE (1u << 2)
#define SPI_SR_BSY (1u << 4)
#define SPI_CR1_SSE (1u << 1)
#define SPI_CR0_DSS_8BIT 7u
#define SPI_CR0_SPH (1u << 7)
#define SPI_CR0_SCR_SHIFT 8u

/* --- ADS1292R (SBAS502C) --- */
#define ADS_WAKEUP 0x02u
#define ADS_START 0x08u
#define ADS_STOP 0x0au
#define ADS_RDATAC 0x10u
#define ADS_SDATAC 0x11u
#define ADS_RREG 0x20u
#define ADS_WREG 0x40u
#define ADS_REG_ID 0x00u
#define ADS_REG_CONFIG1 0x01u
#define ADS_ID_ADS1292R 0x73u
/* Status word bits 19:15 are LOFF_STAT[4:0]: RLD, IN2N, IN2P, IN1N, IN1P. */
#define ADS_STATUS_LOFF_SHIFT 15u
#define ADS_LOFF_CH1 0x03u /* ventricular lead */
#define ADS_LOFF_CH2 0x0cu /* atrial lead */

/* MCP4822 (DS20002249B) write word: A/B, -, GA, SHDN, D11..D0. GA = 0 is the
 * 2x range, where one LSB is exactly 1 mV, which is why amplitudes below are
 * programmed in mV. */
#define DAC_ACTIVE (1u << 12)
#define DAC_CHANNEL_B (1u << 15)

/* --- ADXL362 (Rev. G) --- */
#define ACCEL_WRITE_REG 0x0au
#define ACCEL_READ_REG 0x0bu
#define ACCEL_READ_FIFO 0x0du
#define ACCEL_REG_DEVID_AD 0x00u
#define ACCEL_REG_FIFO_ENTRIES_L 0x0cu
#define ACCEL_REG_SOFT_RESET 0x1fu
#define ACCEL_REG_FIFO_CONTROL 0x28u
#define ACCEL_REG_FIFO_SAMPLES 0x29u
#define ACCEL_REG_INTMAP1 0x2au
#define ACCEL_REG_FILTER_CTL 0x2cu
#define ACCEL_REG_POWER_CTL 0x2du
#define ACCEL_SOFT_RESET_KEY 0x52u /* "R" */
#define ACCEL_FILTER_2G_25HZ 0x11u /* +-2 g (1 mg/LSB), HALF_BW, ODR 25 Hz */
#define ACCEL_FIFO_STREAM 0x02u
#define ACCEL_POWER_MEASURE 0x02u /* measurement, normal noise */
#define ACCEL_INT_FIFO_WATERMARK 0x04u
/* 25 samples of X, Y and Z: INT1 rises once a second at 25 Hz. */
#define ACCEL_FIFO_WATERMARK 75u
/* At most this many entries per drain, so one burst stays well inside the
 * 2 ms between ADS1292R results (see accel_drain). */
#define ACCEL_DRAIN_MAX 120u

/* --- I2C0 (DesignWare APB I2C, ch 12.2) --- */
#define I2C0_BASE 0x40090000u
#define I2C_CON (I2C0_BASE + 0x00)
#define I2C_TAR (I2C0_BASE + 0x04)
#define I2C_DATA_CMD (I2C0_BASE + 0x10)
#define I2C_FS_SCL_HCNT (I2C0_BASE + 0x1c)
#define I2C_FS_SCL_LCNT (I2C0_BASE + 0x20)
#define I2C_RAW_INTR_STAT (I2C0_BASE + 0x34)
#define I2C_CLR_TX_ABRT (I2C0_BASE + 0x54)
#define I2C_CLR_STOP_DET (I2C0_BASE + 0x60)
#define I2C_ENABLE (I2C0_BASE + 0x6c)
#define I2C_STATUS (I2C0_BASE + 0x70)
#define I2C_RXFLR (I2C0_BASE + 0x78)
#define I2C_SDA_HOLD (I2C0_BASE + 0x7c)
#define I2C_TX_ABRT_SOURCE (I2C0_BASE + 0x80)
#define I2C_FS_SPKLEN (I2C0_BASE + 0xa0)
/* IC_CON: master, fast mode, restart enabled, slave disabled, TX_EMPTY_CTRL
 * -- what pico-sdk's i2c_init writes. */
#define I2C_CON_MASTER_FAST 0x165u
#define I2C_STATUS_TFNF (1u << 1)
#define INTR_TX_ABRT (1u << 6)
#define INTR_STOP_DET (1u << 9)
#define CMD_READ (1u << 8)
#define CMD_STOP (1u << 9)
#define CMD_RESTART (1u << 10)
/* No healthy transfer takes this long; a missing EEPROM must not wedge the
 * pacing loop. */
#define I2C_TIMEOUT_US 2000u

/* --- AT24C256C (Microchip DS20006270) --- */
#define EEPROM_ADDR 0x50u /* 1010 A2 A1 A0, all three strapped low */
/* tWR, the self-timed write cycle: the part ignores the bus for up to 5 ms
 * after a write's STOP (DS20006270, AC characteristics). */
#define EEPROM_TWR_US 5000u

/* --- Pacing parameters and limits --- */
#define MODE_DDD 0
#define MODE_DOO 1
#define MODE_VVI 2
#define MODE_VOO 3
#define MODE_OOO 4
#define MODE_DDDR 5
#define MODE_VVIR 6
#define N_MODES 7
static const char *const MODE_NAMES[N_MODES] = {"DDD", "DOO", "VVI", "VOO", "OOO", "DDDR", "VVIR"};

/* Both chambers paced: the timing cycle is the dual-chamber one. */
static int dual_chamber(uint32_t mode) {
    return mode == MODE_DDD || mode == MODE_DDDR || mode == MODE_DOO;
}

/* Inhibited and triggered dual-chamber: senses the atrium and tracks it. */
static int atrial_tracking(uint32_t mode) {
    return mode == MODE_DDD || mode == MODE_DDDR;
}

/* Single-chamber inhibited: a ventricular sense restarts timing. */
static int vvi_like(uint32_t mode) {
    return mode == MODE_VVI || mode == MODE_VVIR;
}

/* The atrial channel is not used at all. */
static int ventricle_only(uint32_t mode) {
    return vvi_like(mode) || mode == MODE_VOO;
}

static int rate_responsive(uint32_t mode) {
    return mode == MODE_DDDR || mode == MODE_VVIR;
}

#define VRP_US 250000u
#define BLANKING_US 50000u
/* A refractory sense is only reported this long after any event on its
 * channel: inside it is the event's own tail -- the second lobe of a sensed
 * R or P wave, or a pace's evoked response -- which is the beat already
 * counted. */
#define VR_QUIET_US 120000u
#define AR_QUIET_US 120000u
/* Atrial channel. ABLANK covers the atrial pace's own pulse and the worst of
 * its afterpotential (2% of the pulse, 15 ms time constant, in the simulated
 * heart). PVAB follows every ventricular event: the pace's 5% crosstalk, and
 * the far-field R wave ~30 ms after the depolarization. Outside the
 * dual-chamber cycle, an atrial sense re-arms only after the atrium's own
 * refractory period, so one P wave is one AS. */
#define ABLANK_US 50000u
#define PVAB_US 100000u
#define ARP_US 250000u
/* Ventricular blanking after an atrial pace, against crosstalk. */
#define VBLANK_AFTER_AP_US 30000u
#define STATUS_PERIOD_US 5000000u

static uint32_t g_mode = MODE_DDD;
static uint32_t g_rate_ppm = 60;
static uint32_t g_amp_mv = 2500;
static uint32_t g_pw_us = 400;
static uint32_t g_sens_uv = 2500;
static uint32_t g_aamp_mv = 2500;
static uint32_t g_asens_uv = 1000;
/* Upper tracking rate, AV delays and PVARP. The AV delays are set long enough
 * that a patient with normal conduction (PR 160 ms) conducts on their own and
 * inhibits the ventricular pace: a sensed P wave is detected ~9 ms into the
 * atrial depolarization and the R wave ~9 ms into the ventricular one, so
 * AS-VS is ~160 ms against a 190 ms SAV; an AP captures 4 ms after the pulse,
 * so AP-VS is ~175 ms against a 220 ms PAV. */
static uint32_t g_utr_ppm = 120;
static uint32_t g_pav_ms = 220;
static uint32_t g_sav_ms = 190;
static uint32_t g_pvarp_ms = 250;
/* Set by the programmer when the mode changes, so the timing cycle restarts
 * cleanly rather than finishing an AV delay the new mode has no notion of. */
static int g_mode_changed;

/* Magnet response. A clinic magnet over the device closes the DRV5032 and
 * forces asynchronous pacing -- DDD/DOO become DOO, VVI/VOO become VOO, OOO
 * stays off -- so the device paces at a known rate whatever the patient is
 * doing: the classic check of battery and capture, and the way to stop
 * inhibition by interference during surgery. The first TMT_BEATS beats run at
 * TMT_RATE_PPM with the last one's pulse width cut to 80%: the threshold
 * margin test, which shows at a glance whether capture has a safety margin.
 * Then pacing continues at MAGNET_RATE_PPM until the magnet is removed. */
#define MAGNET_RATE_PPM 85u
#define TMT_RATE_PPM 100u
#define TMT_BEATS 3u
/* Battery monitor. The ACCOLADE's cell is lithium/carbon-monofluoride
 * (Li/CFx; BSC spec sheet: "lithium-carbon monofluoride cell"). It sits on a
 * flat plateau -- 2.6 V new, a peak of 2.72 V after about a third of its
 * life, under 150 mV of droop to 94% discharged (US 6,804,557) -- and then
 * falls off a cliff, while its internal resistance climbs from ~10 ohm to
 * ~1 kohm (crates/emerson-collections/implant_cell, LI_CFX_OCV), so its
 * voltage *under the device's own load* is the measure of what is left. It is
 * read once a second, an average of 8 conversions, and a state change needs
 * BATT_CONFIRM readings in a row past the threshold, so one bad sample cannot
 * declare the battery spent. The states latch: a cell does not recover, and
 * ERI is cleared only by replacing the device (here, a reset).
 *
 * ERI, the elective replacement indicator, is the clinic's cue to schedule a
 * replacement while the device still has months of reserve. At ERI the
 * device sheds load the way real ones do: dual-chamber modes fall back to
 * VVI, the lower rate to 65 ppm, and the magnet rate drops to 65 ppm too --
 * the change a clinician sees by putting a magnet over the device. EOL is
 * the end of that reserve: replace now.
 *
 * Thresholds: ERI 2.55 V and EOL 2.30 V, the quiescent-loaded voltage. They
 * are the model's own, not published BSC values: ERI where the cliff starts
 * (95.8% of 1.6 Ah, ~0.63 y of default-load service left; BSC ACCOLADE EL
 * "up to 1.6 Amp-hour usable battery capacity"), EOL at 98.5% (~0.2 y). The
 * new cell's 2.6 V is just above ERI, so firmware needs no BOL blanking (the
 * patent's other embodiment starts at 2.4 V, which would trip it). A voltage
 * test at quiescent load does not see an internal-resistance defect (BSC
 * ACCOLADE physician letter, Sept 2025, Appendix A: the high-impedance cell
 * looks normal at quiescent load); only a mA-class burst on the rail does. */
#define BATT_OK 0u
#define BATT_ERI 1u
#define BATT_EOL 2u
static const char *const BATT_NAMES[3] = {"OK", "ERI", "EOL"};
#define BATT_PERIOD_US 1000000u
#define BATT_SAMPLES 8u
#define BATT_CONFIRM 3u
#define ERI_MV 2550u
#define EOL_MV 2300u
#define ERI_RATE_PPM 65u
#define ERI_MAGNET_RATE_PPM 65u
static uint32_t g_batt_mv;
static uint32_t g_batt_state = BATT_OK;
static uint32_t g_batt_confirm; /* consecutive readings past the next threshold */

/* Rate response. A healthy sinus node raises the heart rate with metabolic
 * demand; one that cannot (chronotropic incompetence, common in sick sinus
 * syndrome) leaves the patient breathless on the stairs at 60 bpm. The
 * accelerometer stands in for demand, as it does in most pacemakers: body
 * motion is a decent proxy for work, it costs microamps, and it needs no
 * extra lead.
 *
 * Every 2 s the firmware turns the accelerometer's samples into an activity
 * level: each axis high-passed to remove gravity (so the device's orientation
 * in the pocket does not matter), then the mean of |x| + |y| + |z| in mg.
 * Above the activity threshold, each 100 mg adds `rrslope` ppm to the lower
 * rate, up to the maximum sensor rate (MSR); below it, the target is the
 * lower rate. The sensor-indicated rate (SIR) approaches that target along
 * an exponential that covers 90% of a change in the reaction time on the
 * way up and the recovery time on the way down -- quick to respond to
 * exertion, slow to drop the rate after it, as the body does.
 *
 * The sensor has the classic blind spots, which the patient model exercises:
 * a car ride's vibration reads as activity, and cycling, which works the
 * legs while the chest stays still, barely does.
 *
 * Chronotropic-incompetence diagnostic. The SIR runs in every mode, not only
 * DDDR/VVIR, so in DDD it can be compared with what the sinus node does
 * about the same exertion. During sustained activity (SIR at least
 * DIAG_MIN_RISE_PPM above the lower rate) the intrinsic atrial rate -- from
 * the intervals between consecutive AS/AR events, never paces -- is measured
 * over a DIAG_WINDOW_MS window, and its rise over the patient's resting
 * atrial rate (every atrial interval, paced or not, while the sensor reads
 * rest) is divided by the SIR's rise over the lower rate. That ratio is the sensor's stand-in for the clinical
 * chronotropic index (heart-rate reserve used over metabolic reserve used),
 * and below CI_INDEX_PCT it reports CI suspected: the cue to program DDDR. It
 * is a screening heuristic in the spirit of the device diagnostics that
 * compare sensor and intrinsic rate histograms, not any vendor's algorithm,
 * and it inherits the sensor's blind spots (cycling never gates it on). */
#define ACT_PERIOD_US 2000000u
#define DIAG_WINDOW_MS 60000u
#define DIAG_MIN_RISE_PPM 15u
#define CI_INDEX_PCT 80u
static uint32_t g_msr_ppm = 120;
static uint32_t g_athresh_mg = 50;
static uint32_t g_rrslope = 15; /* ppm per 100 mg above the threshold */
static uint32_t g_rrup_s = 30;
static uint32_t g_rrdown_s = 150;
static uint32_t g_sir_mppm = 60000; /* sensor-indicated rate, milli-ppm */
static uint32_t g_act_mg;           /* last period's activity */
static int g_accel_ok;
/* Accumulated between periods: sum of |x| + |y| + |z| in LSB/16, samples. */
static uint32_t g_act_sum, g_act_n;
static int32_t g_hp[3]; /* high-pass baselines, LSB * 16 */
static uint32_t g_hp_init; /* bit per axis */
/* The diagnostic. Atrial intervals this period: all of them, and the ones
 * between two intrinsic events (ms summed, and counted). Rates come from
 * intervals rather than beats per period, which a 2 s period quantizes to
 * 30 bpm steps. The resting rate is a running total that halves past
 * REST_SPAN_MS, so it forgets slowly; then the exertion window in progress. */
#define REST_SPAN_MS 60000u
static uint32_t g_last_a_us;
static int g_have_last_a, g_last_a_intrinsic;
static uint32_t g_p_all_ms, g_p_all_n, g_p_intr_ms, g_p_intr_n;
static uint32_t g_rest_ms, g_rest_n;
static uint32_t g_ex_ms, g_ex_intr_ms, g_ex_intr_n, g_ex_sir_sum, g_ex_windows;

/* --- Implant time scale and Safety Mode (BSC ACCOLADE physician letter, Sept 2025,
 * Appendix A / Table 3) ---
 *
 * The implant clock runs `g_scale` implant seconds per guest second. The default,
 * 1e6, is one implant second per guest microsecond: the cell's aging_factor in
 * peripherals.yaml (`/board/battery set_aging_factor` must be set to the same
 * number whenever the scale is changed, or the diagnostics' dates and the cell's
 * wear disagree). It is persisted in the EEPROM (the safety block) and
 * programmable over the console: `timescale <implant s per guest s>`. Every
 * implant-time quantity goes through it: the trend period, the clock checkpoint,
 * the radio wakeup interval and retry, and the Safety Mode window.
 *
 * Safety Mode: three system resets within 48 hours put the device in a
 * permanent, non-programmable VVI mode (Table 3: VVI, 72.5 ppm, AGC 0.25 mV,
 * 5.0 V at 1.0 ms RV, RV unipolar sensing and pacing, RVRP 250 ms, noise
 * response VOO, magnet response disabled). The latch lives in the EEPROM and no
 * command clears it. */
#define SCALE_DEFAULT 1000000u
#define SCALE_MIN 100u
#define SCALE_MAX 1000000u
#define SAFETY_RESETS 3u            /* Table 3: three resets ... */
#define SAFETY_WINDOW_S (48u * 3600u) /* ... within a 48-hour period (implant time) */
#define SAFETY_LRI_US 827586u       /* 72.5 ppm: 60 s / 72.5 */
#define SAFETY_RATE_PPM 72u         /* the integer shown; the timing uses SAFETY_LRI_US */
#define SAFETY_AMP_MV 5000u         /* 5.0 V ... */
#define SAFETY_PW_US 1000u          /* ... at 1.0 ms */
#define SAFETY_SENS_UV 250u         /* AGC 0.25 mV: the floor the automatic gain control decays to (agc_threshold) */
#define POLARITY_PIN 28u            /* TS5A3159 IN: low = bipolar (ring), high = unipolar (can) */
static uint32_t g_scale = SCALE_DEFAULT;
static int g_safety;

static int g_magnet_response = 1; /* `magnet async|off` */
static int g_magnet;              /* the response is in force */
static uint32_t g_tmt_left;       /* threshold-margin-test beats still to pace */

/* --- UART ---
 *
 * Every print helper writes to `g_port`, so a block of output picks its
 * destination once with `to(CONSOLE)` or `to(TELEMETRY)`. */

static uint32_t g_port = CONSOLE;

static void to(uint32_t port) {
    g_port = port;
}

static void uart_putc(char c) {
    while (REG(g_port + UART_FR) & UART_FR_TXFF) {
    }
    REG(g_port + UART_DR) = (uint8_t)c;
}

static void uart_puts(const char *s) {
    while (*s) {
        if (*s == '\n')
            uart_putc('\r');
        uart_putc(*s++);
    }
}

static void uart_put_udec(uint32_t v) {
    char buf[11];
    int i = 10;
    buf[i] = '\0';
    do {
        buf[--i] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v && i > 0);
    uart_puts(&buf[i]);
}

/* `raw / scale` with `digits` zero-padded decimals: 2500/1000/3 -> "2.500". */
static void uart_put_fixed(uint32_t raw, uint32_t scale, uint32_t digits) {
    uart_put_udec(raw / scale);
    uart_putc('.');
    uint32_t frac = raw % scale;
    for (uint32_t i = 0; i < digits; i++) {
        frac *= 10u;
        uart_putc((char)('0' + frac / scale));
        frac %= scale;
    }
}

static void uart_put_hex8(uint32_t v) {
    static const char HEX[] = "0123456789abcdef";
    uart_putc(HEX[(v >> 4) & 0xfu]);
    uart_putc(HEX[v & 0xfu]);
}

/* --- Time --- */

static uint32_t time_us(void) {
    return REG(TIMER0_TIMERAWL);
}

static void delay_us(uint32_t us) {
    uint32_t start = time_us();
    while ((uint32_t)(time_us() - start) < us) {
    }
}

/* a * b / c without a 64-bit divide (no libgcc here): the product is exact in 64
 * bits and divided by shift-subtract. Saturates at 0xffffffff. */
static uint32_t muldiv(uint32_t a, uint32_t b, uint32_t c) {
    uint64_t n = (uint64_t)a * b;
    uint64_t q = 0, r = 0;
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1u);
        if (r >= c) {
            r -= c;
            q |= (uint64_t)1 << i;
        }
    }
    return q > 0xffffffffu ? 0xffffffffu : (uint32_t)q;
}

/* An implant-time span in guest microseconds, at the current scale. */
static uint32_t implant_s_to_us(uint32_t s) {
    return muldiv(s, 1000000u, g_scale);
}

static void clocks_init(void) {
    REG(XOSC_STARTUP) = 0xc4;
    REG(XOSC_CTRL) = XOSC_ENABLE_MAGIC | XOSC_RANGE_1_15MHZ;
    while (!(REG(XOSC_STATUS) & XOSC_STATUS_STABLE)) {
    }

    REG(CLK_REF_DIV) = 1u << 16;
    REG(CLK_REF_CTRL) = CLK_REF_SRC_XOSC;
    while (REG(CLK_REF_SELECTED) != (1u << CLK_REF_SRC_XOSC)) {
    }

    /* clk_sys resets onto its AUX mux (ROSC); its glitchless source 0 is
     * clk_ref, now the crystal. */
    REG(CLK_SYS_DIV) = 1u << 16;
    REG(CLK_SYS_CTRL + CLR_ALIAS) = CLK_SYS_SRC_AUX;
    while (REG(CLK_SYS_SELECTED) != 1u) {
    }

    REG(CLK_PERI_CTRL) = CLK_CTRL_ENABLE; /* AUXSRC 0: clk_sys */

    REG(TICKS_TIMER0_CYCLES) = TICK_CYCLES_1US;
    REG(TICKS_TIMER0_CTRL) = 1;
    REG(TICKS_RISCV_CYCLES) = TICK_CYCLES_1US;
    REG(TICKS_RISCV_CTRL) = 1;
}

/* --- GPIO --- */

static void pin_output(uint32_t pin, int high) {
    REG(PAD_GPIO(pin) + CLR_ALIAS) = PAD_ISO;
    REG(GPIO_CTRL(pin)) = FUNCSEL_SIO;
    REG(high ? SIO_GPIO_OUT_SET : SIO_GPIO_OUT_CLR) = 1u << pin;
    REG(SIO_GPIO_OE_SET) = 1u << pin;
}

static void pin_input(uint32_t pin) {
    REG(PAD_GPIO(pin) + CLR_ALIAS) = PAD_ISO;
    REG(PAD_GPIO(pin) + SET_ALIAS) = PAD_IE;
    REG(GPIO_CTRL(pin)) = FUNCSEL_SIO;
}

static void pin_spi(uint32_t pin) {
    REG(PAD_GPIO(pin) + CLR_ALIAS) = PAD_ISO;
    REG(PAD_GPIO(pin) + SET_ALIAS) = PAD_IE;
    REG(GPIO_CTRL(pin)) = FUNCSEL_SPI;
}

/* An ADC input, set up as pico-sdk's adc_gpio_init does: digital input and
 * output both off and no pulls, so the pad loads the cell with nothing. */
static void pin_analog(uint32_t pin) {
    REG(PAD_GPIO(pin) + CLR_ALIAS) = PAD_IE | PAD_PUE | PAD_PDE;
    REG(PAD_GPIO(pin) + SET_ALIAS) = PAD_OD;
    REG(PAD_GPIO(pin) + CLR_ALIAS) = PAD_ISO;
    REG(GPIO_CTRL(pin)) = FUNCSEL_NULL;
}

static void pin_set(uint32_t pin, int high) {
    REG(high ? SIO_GPIO_OUT_SET : SIO_GPIO_OUT_CLR) = 1u << pin;
}

/* --- SPI: one byte out, one byte back, full duplex --- */

static uint8_t spi_xfer(uint32_t base, uint8_t out) {
    while (!(REG(base + SPI_SR) & SPI_SR_TNF)) {
    }
    REG(base + SPI_DR) = out;
    while (!(REG(base + SPI_SR) & SPI_SR_RNE)) {
    }
    return (uint8_t)REG(base + SPI_DR);
}

/* Every queued frame has gone out: required before moving CS, since the
 * controller only sees CS through the pad. */
static void spi_drain(uint32_t base) {
    while (REG(base + SPI_SR) & SPI_SR_BSY) {
    }
}

/* SPI0 is shared, and its two parts disagree on the frame: the ADS1292R
 * wants mode 1 (CPHA 1) at 1 MHz, the ADXL362 mode 0 at up to 8 MHz (6 MHz
 * here). The format is reprogrammed with the port disabled before whichever
 * part is selected next, and only when it changes. */
#define SPI0_ADS_CR0 (SPI_CR0_DSS_8BIT | SPI_CR0_SPH)
#define SPI0_ADS_CPSR 12u
#define SPI0_ACCEL_CR0 SPI_CR0_DSS_8BIT
#define SPI0_ACCEL_CPSR 2u
static uint32_t g_spi0_cr0 = ~0u;

static void spi0_format(uint32_t cr0, uint32_t cpsr) {
    if (g_spi0_cr0 == cr0)
        return;
    spi_drain(SPI0_BASE);
    REG(SPI0_BASE + SPI_CR1) = 0;
    REG(SPI0_BASE + SPI_CPSR) = cpsr;
    REG(SPI0_BASE + SPI_CR0) = cr0;
    REG(SPI0_BASE + SPI_CR1) = SPI_CR1_SSE;
    g_spi0_cr0 = cr0;
}

/* --- ADS1292R --- */

static void ads_select(void) {
    spi0_format(SPI0_ADS_CR0, SPI0_ADS_CPSR);
    pin_set(ADS_CS_PIN, 0);
}

/* "After the serial communication is finished, always wait four or more tCLK
 * cycles before taking CS high" (§8.5.1.1): 4 / 512 kHz = 7.8 us. */
static void ads_deselect(void) {
    spi_drain(SPI0_BASE);
    delay_us(8);
    pin_set(ADS_CS_PIN, 1);
}

static void ads_command(uint8_t opcode) {
    ads_select();
    spi_xfer(SPI0_BASE, opcode);
    ads_deselect();
}

/* At SCLK = 1 MHz a byte takes 8 us, which already covers the 4-tCLK decode
 * time a multi-byte command needs between bytes (§8.5.2.10). */
static uint8_t ads_rreg(uint8_t addr) {
    ads_select();
    spi_xfer(SPI0_BASE, (uint8_t)(ADS_RREG | addr));
    spi_xfer(SPI0_BASE, 0);
    uint8_t value = spi_xfer(SPI0_BASE, 0);
    ads_deselect();
    return value;
}

static void ads_wreg(uint8_t addr, uint8_t value) {
    ads_select();
    spi_xfer(SPI0_BASE, (uint8_t)(ADS_WREG | addr));
    spi_xfer(SPI0_BASE, 0);
    spi_xfer(SPI0_BASE, value);
    ads_deselect();
}

static int ads_drdy(void) {
    return (REG(SIO_GPIO_IN) & (1u << ADS_DRDY_PIN)) == 0u; /* active low */
}

/* One RDATAC frame: 24 status bits, then CH1 and CH2, MSB first. */
static void ads_read_frame(uint32_t *status, int32_t ch[2]) {
    uint8_t b[9];
    ads_select();
    for (int i = 0; i < 9; i++)
        b[i] = spi_xfer(SPI0_BASE, 0);
    ads_deselect();
    *status = ((uint32_t)b[0] << 16) | ((uint32_t)b[1] << 8) | b[2];
    for (int c = 0; c < 2; c++) {
        const uint8_t *d = &b[3 + 3 * c];
        uint32_t raw = ((uint32_t)d[0] << 16) | ((uint32_t)d[1] << 8) | d[2];
        ch[c] = (int32_t)(raw << 8) >> 8;
    }
}

/* Code -> microvolts at gain 6, VREF 2.42 V: LSB = 2.42 V / 6 / (2^23 - 1)
 * = 0.048081 uV. 121/2517 is within 0.02% of that and keeps code * 121 inside
 * 32 bits for any code. */
static int32_t ads_code_to_uv(int32_t code) {
    return code * 121 / 2517;
}

static int ads_init(void) {
    /* The part wakes up in RDATAC mode: leave it before touching registers. */
    delay_us(1000);
    ads_command(ADS_SDATAC);
    uint8_t id = (uint8_t)ads_rreg(ADS_REG_ID);
    uart_puts("[ads1292r] ID=0x");
    uart_put_hex8(id);
    uart_puts(id == ADS_ID_ADS1292R ? " (ADS1292R)\n" : " -- NOT an ADS1292R\n");
    if (id != ADS_ID_ADS1292R)
        return 0;

    ads_wreg(ADS_REG_CONFIG1, 0x02); /* continuous, 500 SPS */
    ads_wreg(0x02, 0xe0);            /* CONFIG2: lead-off comparators + internal 2.42 V reference */
    ads_wreg(0x03, 0x10);            /* LOFF: DC lead-off, 95%/5% thresholds, 6 nA */
    ads_wreg(0x04, 0x00);            /* CH1SET: gain 6, electrode input (ventricular lead) */
    ads_wreg(0x05, 0x00);            /* CH2SET: gain 6, electrode input (atrial lead) */
    ads_wreg(0x06, 0x00);            /* RLD_SENS: no right-leg drive; an implant has none */
    ads_wreg(0x07, 0x0f);            /* LOFF_SENS: lead-off on IN1P/IN1N and IN2P/IN2N */
    ads_wreg(0x09, 0x02);            /* RESP1: respiration off (bit 1 must be 1) */
    ads_wreg(0x0a, 0x03);            /* RESP2: (bit 0 must be 1) */
    ads_command(ADS_START);
    ads_command(ADS_RDATAC);
    return 1;
}

/* SPI1 is shared too: the MCP4822 takes 6 MHz, the RF430CL330H no more
 * than 100 kHz (SLAS916C §4.11). Both are mode 0; only the rate changes,
 * reprogrammed as SPI0's format is. */
#define SPI1_DAC_CR0 SPI_CR0_DSS_8BIT
#define SPI1_DAC_CPSR 2u
#define SPI1_TAG_CR0 (SPI_CR0_DSS_8BIT | (59u << SPI_CR0_SCR_SHIFT)) /* 12 MHz / (2 x 60) */
#define SPI1_TAG_CPSR 2u
static uint32_t g_spi1_cr0 = ~0u;

static void spi1_format(uint32_t cr0, uint32_t cpsr) {
    if (g_spi1_cr0 == cr0)
        return;
    spi_drain(SPI1_BASE);
    REG(SPI1_BASE + SPI_CR1) = 0;
    REG(SPI1_BASE + SPI_CPSR) = cpsr;
    REG(SPI1_BASE + SPI_CR0) = cr0;
    REG(SPI1_BASE + SPI_CR1) = SPI_CR1_SSE;
    g_spi1_cr0 = cr0;
}

/* --- DMA (ch 12.6): the wand link's SPI1 transfers ---
 *
 * Two channels move a whole RF430CL330H transaction while the CPU paces: TX
 * feeds SPI1's DR from memory, paced by DREQ_SPI1_TX (the PL022 asks while
 * its transmit FIFO has room), and RX empties DR as each frame comes back,
 * paced by DREQ_SPI1_RX (receive FIFO not empty) -- into a buffer for a read,
 * or into one dummy byte for a write, which still has to drain what full
 * duplex clocks in or the receive FIFO would overrun. Byte transfers, no
 * chaining, no interrupts: the link polls BUSY from its slot. */
#define DMA_BASE 0x50000000u
#define DMA_CH(n) (DMA_BASE + 0x40u * (n)) /* each channel's four registers */
#define DMA_READ_ADDR 0x00u
#define DMA_WRITE_ADDR 0x04u
#define DMA_TRANS_COUNT 0x08u
#define DMA_CTRL_TRIG 0x0cu      /* writing it starts the channel (§12.6.2.1) */
#define DMA_CHAN_ABORT 0x464u    /* reads non-zero until the abort is done (§12.6.2.2) */
#define DMA_CTRL_EN (1u << 0)    /* CHx_CTRL_TRIG fields (§12.6.2.1); DATA_SIZE 0 = byte */
#define DMA_CTRL_INCR_READ (1u << 4)
#define DMA_CTRL_INCR_WRITE (1u << 6)
#define DMA_CTRL_CHAIN_TO_SHIFT 13u /* chaining to itself is no chain */
#define DMA_CTRL_TREQ_SHIFT 17u
#define DMA_CTRL_BUSY (1u << 26)
#define DMA_CTRL_ERRORS (3u << 29) /* WRITE_ERROR, READ_ERROR: write 1 to clear */
#define DREQ_SPI1_TX 26u           /* §12.6.4's DREQ table */
#define DREQ_SPI1_RX 27u
#define DMA_TAG_TX 0u
#define DMA_TAG_RX 1u

/* Whatever the CPU stored before this is in memory before the DMA reads it,
 * and whatever the DMA wrote is read fresh after: the channels are another
 * bus master the compiler cannot see. */
static void dma_barrier(void) {
    __asm__ volatile("" ::: "memory");
}

static void dma_start(uint32_t ch, uint32_t from, uint32_t to_addr, uint32_t count, uint32_t ctrl, uint32_t treq) {
    dma_barrier();
    REG(DMA_CH(ch) + DMA_READ_ADDR) = from;
    REG(DMA_CH(ch) + DMA_WRITE_ADDR) = to_addr;
    REG(DMA_CH(ch) + DMA_TRANS_COUNT) = count;
    /* Last, since it triggers; clears any error from an earlier run too. */
    REG(DMA_CH(ch) + DMA_CTRL_TRIG) = ctrl | DMA_CTRL_EN | DMA_CTRL_ERRORS | ch << DMA_CTRL_CHAIN_TO_SHIFT |
                                      treq << DMA_CTRL_TREQ_SHIFT;
}

static int dma_busy(uint32_t ch) {
    return (REG(DMA_CH(ch) + DMA_CTRL_TRIG) & DMA_CTRL_BUSY) != 0u;
}

/* --- MCP4822 ---
 *
 * The DAC shares SPI1 with the telemetry tag, whose transfers run on DMA
 * across many passes with the tag's CS low. A DAC write must never land
 * inside one, so while one is in flight the word waits (the latest per
 * channel) and goes out the moment the tag's CS rises: on hardware a whole
 * answer file is ~0.2 s of bus time at 100 kHz, and the new amplitude is in
 * place for the next pace after it. */
static int g_tag_xfer; /* a DMA transaction holds SPI1, the tag's CS low */
static uint16_t g_dac_pending[2];
static uint32_t g_dac_pending_mask;

static void dac_write(uint16_t word) {
    if (g_tag_xfer) {
        uint32_t ch = word >> 15; /* the channel select bit */
        g_dac_pending[ch] = word;
        g_dac_pending_mask |= 1u << ch;
        return;
    }
    spi1_format(SPI1_DAC_CR0, SPI1_DAC_CPSR);
    pin_set(DAC_CS_PIN, 0);
    spi_xfer(SPI1_BASE, (uint8_t)(word >> 8));
    spi_xfer(SPI1_BASE, (uint8_t)word);
    spi_drain(SPI1_BASE);
    /* LDAC is tied low: VOUT updates on this rising edge. */
    pin_set(DAC_CS_PIN, 1);
}

/* 2x gain, active: 1 mV per code at the DAC. Channel A is the ventricular
 * output, channel B the atrial one. */
/* The output stage multiplies the DAC's voltage by 1.25 (`output_gain: 1.25` on
 * the mcp4822 entry in peripherals.yaml), so the capacitor reaches 1.25 mV per
 * code and Safety Mode's 5.0 V is code 4000 (code 4095 is 5.119 V). Amplitudes
 * stay programmed in mV at the capacitor: code = mV / 1.25, rounded. */
static uint32_t dac_code_for_mv(uint32_t mv) {
    uint32_t code = (mv * 4u + 2u) / 5u;
    return code > 0x0fffu ? 0x0fffu : code;
}

static void dac_set_vpace_mv(uint32_t mv) {
    dac_write((uint16_t)(DAC_ACTIVE | dac_code_for_mv(mv)));
}

static void dac_set_apace_mv(uint32_t mv) {
    dac_write((uint16_t)(DAC_CHANNEL_B | DAC_ACTIVE | dac_code_for_mv(mv)));
}

/* The words that waited for a tag transaction, now that SPI1 is free. */
static void dac_flush(void) {
    for (uint32_t ch = 0; ch < 2u && !g_tag_xfer; ch++) {
        if (g_dac_pending_mask & (1u << ch)) {
            g_dac_pending_mask &= ~(1u << ch);
            dac_write(g_dac_pending[ch]);
        }
    }
}

/* --- RF430CL330H (SLAS916C): the telemetry head ---
 *
 * An NFC Type 4 tag whose 3 KB of SRAM the wand's reader and firmware share.
 * It holds the NDEF application (Table 5-32; the bytes as TI's example
 * firmware lays them out): the capability container, then the NDEF file
 * E104h, which carries the device's answers up to the wand, then a
 * proprietary file E105h, which carries the programmer's requests down.
 * Each file starts with its length, big-endian (NFC Forum Type 4). The
 * registers are 16-bit little-endian (§5.7). */
#define TAG_CMD_WRITE 0x02u
#define TAG_CMD_READ 0x03u
#define TAG_CONTROL 0xfffeu
#define TAG_STATUS 0xfffcu
#define TAG_INT_ENABLE 0xfffau
#define TAG_INT_FLAGS 0xfff8u
#define TAG_VERSION 0xffeeu
#define TAG_CTRL_SW_RESET (1u << 0)
#define TAG_CTRL_RF_ENABLE (1u << 1)
#define TAG_CTRL_INT_ENABLE (1u << 2)
#define TAG_CTRL_INTO_DRIVE (1u << 4)
#define TAG_STATUS_RF_BUSY (1u << 2)
#define TAG_INT_END_OF_READ (1u << 1)
#define TAG_INT_END_OF_WRITE (1u << 2)
#define TAG_INT_NDEF_ERROR (1u << 5)
/* INTO enabled, driven, active low (Table 5-10); RF on top when the reader
 * may come in. */
#define TAG_CTRL_BASE (TAG_CTRL_INT_ENABLE | TAG_CTRL_INTO_DRIVE)
#define TAG_T_READY_US 20000u /* §4.9: after power-up or reset */
#define TAG_T_SU_CS_US 25u    /* §4.11 */
#define TAG_T_HIGH_CS_US 50u
#define TAG_CCLEN 0x17u /* 7 bytes, then two 8-byte file control TLVs */
#define UP_FILE_ID 0xe104u
#define UP_FILE_SIZE 0x0900u /* with its length field */
#define DOWN_FILE_ID 0xe105u
#define DOWN_FILE_SIZE 0x0100u
/* The application name (7) and the CC's identifier (2), the CC, the file's
 * identifier (2): its length field. */
#define UP_LEN_ADDR (9u + TAG_CCLEN + 2u)
#define UP_DATA_ADDR (UP_LEN_ADDR + 2u)
#define UP_DATA_MAX (UP_FILE_SIZE - 2u)
#define DOWN_LEN_ADDR (UP_LEN_ADDR + UP_FILE_SIZE + 2u)
#define DOWN_DATA_ADDR (DOWN_LEN_ADDR + 2u)
#define DOWN_DATA_MAX (DOWN_FILE_SIZE - 2u)

static int g_tag_ok;

static void tag_begin(uint32_t cmd, uint32_t addr) {
    spi1_format(SPI1_TAG_CR0, SPI1_TAG_CPSR);
    pin_set(TAG_CS_PIN, 0);
    delay_us(TAG_T_SU_CS_US);
    spi_xfer(SPI1_BASE, (uint8_t)cmd);
    spi_xfer(SPI1_BASE, (uint8_t)(addr >> 8));
    spi_xfer(SPI1_BASE, (uint8_t)addr);
}

static void tag_end(void) {
    spi_drain(SPI1_BASE);
    pin_set(TAG_CS_PIN, 1);
    delay_us(TAG_T_HIGH_CS_US);
}

static void tag_write(uint32_t addr, const uint8_t *b, uint32_t n) {
    tag_begin(TAG_CMD_WRITE, addr);
    for (uint32_t i = 0; i < n; i++)
        spi_xfer(SPI1_BASE, b[i]);
    tag_end();
}

/* Reads take a dummy byte after the address (§5.6). */
static void tag_read(uint32_t addr, uint8_t *b, uint32_t n) {
    tag_begin(TAG_CMD_READ, addr);
    spi_xfer(SPI1_BASE, 0);
    for (uint32_t i = 0; i < n; i++)
        b[i] = spi_xfer(SPI1_BASE, 0);
    tag_end();
}

static void tag_wreg(uint32_t addr, uint32_t v) {
    uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
    tag_write(addr, b, 2);
}

static uint32_t tag_rreg(uint32_t addr) {
    uint8_t b[2];
    tag_read(addr, b, 2);
    return b[0] | (uint32_t)b[1] << 8;
}

static void tag_wlen(uint32_t addr, uint32_t n) {
    uint8_t b[2] = {(uint8_t)(n >> 8), (uint8_t)n};
    tag_write(addr, b, 2);
}

static uint32_t tag_rlen(uint32_t addr) {
    uint8_t b[2];
    tag_read(addr, b, 2);
    return (uint32_t)b[0] << 8 | b[1];
}

/* A whole transaction on DMA: CS low and tSU,CS, then `n` bytes from `tx`,
 * with what comes back into `rx` (or dropped, for a write). The tag takes
 * the command and address from the first bytes and auto-increments for as
 * long as CS stays low (§5.6), so a whole file is one transaction. */
static uint8_t g_tag_sink;

static void tag_xfer_start(const uint8_t *tx, uint8_t *rx, uint32_t n) {
    spi1_format(SPI1_TAG_CR0, SPI1_TAG_CPSR);
    pin_set(TAG_CS_PIN, 0);
    delay_us(TAG_T_SU_CS_US);
    g_tag_xfer = 1;
    /* RX first, so it is waiting when the first frame comes back. */
    dma_start(DMA_TAG_RX, SPI1_BASE + SPI_DR, (uint32_t)(rx ? rx : &g_tag_sink), n, rx ? DMA_CTRL_INCR_WRITE : 0u,
              DREQ_SPI1_RX);
    dma_start(DMA_TAG_TX, (uint32_t)tx, SPI1_BASE + SPI_DR, n, DMA_CTRL_INCR_READ, DREQ_SPI1_TX);
}

/* Every frame has gone out and come back: the RX channel finishes last. */
static int tag_xfer_done(void) {
    int done = !dma_busy(DMA_TAG_TX) && !dma_busy(DMA_TAG_RX);
    dma_barrier();
    return done;
}

/* SPI idle, CS high, tHIGH,CS (§4.11); then any DAC write that waited. */
static void tag_xfer_end(void) {
    tag_end();
    g_tag_xfer = 0;
    dac_flush();
}

/* Stop a transaction part way (the session closed under it): the tag ignores
 * what it was given once CS rises, and the file it was writing is emptied
 * right after (link_reset). */
static void tag_xfer_abort(void) {
    if (!g_tag_xfer)
        return;
    REG(DMA_BASE + DMA_CHAN_ABORT) = 1u << DMA_TAG_TX | 1u << DMA_TAG_RX;
    while (REG(DMA_BASE + DMA_CHAN_ABORT)) {
    }
    spi_drain(SPI1_BASE);
    while (REG(SPI1_BASE + SPI_SR) & SPI_SR_RNE)
        (void)REG(SPI1_BASE + SPI_DR);
    tag_xfer_end();
}

static int tag_into(void) {
    return !(REG(SIO_GPIO_IN) & (1u << TAG_INTO_PIN));
}

static void tag_rf(int on) {
    tag_wreg(TAG_CONTROL, TAG_CTRL_BASE | (on ? TAG_CTRL_RF_ENABLE : 0u));
}

/* §5.10 steps 1-3, after a software reset: a watchdog reboot resets the MCU,
 * not the tag, which would still hold the last session's files. */
static int tag_init(void) {
    tag_wreg(TAG_CONTROL, TAG_CTRL_SW_RESET);
    delay_us(TAG_T_READY_US);
    /* §5.8.3: offer 847 kbps both ways (Table 5-30, C4h) by the sequence
     * for this ROM version -- the tag only reports 106 kbps by default. */
    uint32_t version = tag_rreg(TAG_VERSION);
    uint32_t rate_addr;
    if (version == 0x0101u)
        rate_addr = 0x2aa4u; /* Table 5-28 */
    else if (version == 0x0201u)
        rate_addr = 0x2a7cu; /* Table 5-29 */
    else
        return 0;
    tag_wreg(0xffe0u, 0x004eu);
    tag_wreg(TAG_CONTROL, 0x0080u);
    tag_wreg(rate_addr, 0x00c4u);
    tag_wreg(0x2814u, 0x0000u);
    tag_wreg(0xffe0u, 0x0000u);
    static const uint8_t head[] = {
        0xd2, 0x76, 0x00, 0x00, 0x85, 0x01, 0x01, /* NDEF application name */
        0xe1, 0x03,                               /* the capability container */
        0x00, TAG_CCLEN, 0x20,                    /* CCLEN, mapping version 2.0 */
        0x00, 0xf9, 0x00, 0xf6,                   /* MLe, MLc, as §5.9.1 advises */
        0x04, 0x06, UP_FILE_ID >> 8, UP_FILE_ID & 0xff, UP_FILE_SIZE >> 8, UP_FILE_SIZE & 0xff, 0x00, 0x00,
        0x05, 0x06, DOWN_FILE_ID >> 8, DOWN_FILE_ID & 0xff, DOWN_FILE_SIZE >> 8, DOWN_FILE_SIZE & 0xff, 0x00, 0x00,
        UP_FILE_ID >> 8, UP_FILE_ID & 0xff, 0x00, 0x00, /* the answer file, empty */
    };
    static const uint8_t down[] = {DOWN_FILE_ID >> 8, DOWN_FILE_ID & 0xff, 0x00, 0x00};
    tag_write(0, head, sizeof head);
    tag_write(DOWN_LEN_ADDR - 2u, down, sizeof down);
    tag_wreg(TAG_INT_ENABLE, TAG_INT_END_OF_READ | TAG_INT_END_OF_WRITE | TAG_INT_NDEF_ERROR);
    tag_rf(1);
    return !(tag_rreg(TAG_INT_FLAGS) & TAG_INT_NDEF_ERROR);
}

/* The end of a tty1 radio line: when it happened, as the marker lines stamp theirs. */
static void radio_stamp(void) {
    uart_puts(" t=");
    uart_put_fixed(time_us() / 1000u, 1000, 3);
    uart_puts("\n");
}

/* --- ZL70102 implant radio (wandless telemetry): the chip on SPI1 ---
 *
 * Microsemi's ZL70102 medical implantable transceiver (MICS band, 402-405 MHz,
 * with a 2.45 GHz wake-up receiver): the radio ACCOLADE-generation implants use
 * for wandless telemetry, to a bedside communicator (a home monitor) or a
 * clinic programmer without a wand. On this board it is the third part on
 * SPI1 -- CS GP5, IRQ GP6 (active high), WU_EN GP7 -- and its VSUP comes from
 * the cell through a TI TPS22860 load switch whose ON pin is GP25 (pulled down
 * on the board): the radio is unpowered until GP25 goes high, and again in any
 * reset, when the pad falls back to its pulled-down reset state.
 *
 * REGISTER MAP: the ZL70102 data sheet (Rev 2, Microsemi 2012, "DS") names the
 * registers and says what they do, but the addresses and bit positions live in
 * the Design Manual, which is not public. The addresses below are the
 * emulator model's own (crates/emerson-collections/zl70102, module `reg`), which
 * follows the data sheet's names and behaviour. THIS DRIVER TARGETS THE MODEL:
 * it is not a driver for the silicon. Everything else it relies on -- the SPI
 * protocol (DS section 5, Figures 5-2/5-3: A7 is the read bit, 7-bit address,
 * auto-increment except the TX/RX FIFO address), the start-up methods (DS
 * "Startup Method Using IMD Pin Control"), the strobe-based 2.45 GHz sniff
 * (Figure 3-6), 15-byte blocks (2 to 15, DS "Media Access Controller"), the 5 s
 * main watchdog (Table 4-1) -- is the data sheet's.
 *
 * Mode 0, at most 4 MHz (DS section 5): 12 MHz / (2 x 2) = 3 MHz here. SPI1 is
 * shared, so the format is reprogrammed before each transaction (spi1_format)
 * and no radio transaction starts while a tag DMA transaction holds the bus. */
#define RADIO_CS_PIN 5u
#define RADIO_IRQ_PIN 6u
#define RADIO_WU_EN_PIN 7u
#define RADIO_PWR_PIN 25u /* TPS22860 ON (SLVSD04): high connects VIN to VOUT, the radio's VSUP */
#define SPI1_RADIO_CR0 (SPI_CR0_DSS_8BIT | (1u << SPI_CR0_SCR_SHIFT)) /* distinct from the DAC's, so spi1_format acts */
#define SPI1_RADIO_CPSR 2u

/* The model's register map (zl70102::reg); names are the data sheet's. */
#define RADIO_REG_MAC_CTRL 0x01u
#define RADIO_REG_MAC_IDLE 0x03u /* the Idle flag: 1 Idle, 0 Send_Response (DS, start-up method steps 2-3) */
#define RADIO_REG_MAC_MODULATION 0x0cu
#define RADIO_REG_MAC_CHANNEL 0x0du
#define RADIO_REG_MAC_TRAINING 0x0eu /* training word length in bytes, 1 to 63 (DS Figure 3-9) */
#define RADIO_REG_TXBUFF_MAXPACKSIZE 0x10u
#define RADIO_REG_TXBUFF_BSIZE 0x11u
#define RADIO_REG_RXBUFF_BSIZE 0x12u
#define RADIO_REG_TXBUFF_USED 0x13u
#define RADIO_REG_RXBUFF_USED 0x14u
#define RADIO_REG_TXRXBUFF 0x15u /* the FIFO: the address does not auto-increment (DS section 5) */
#define RADIO_REG_IRQ_STATUS 0x17u
#define RADIO_REG_IRQ_ENABLE 0x18u
#define RADIO_REG_IRQ_CLEAR 0x1au
#define RADIO_REG_WAKEUP_CRCCTRL 0x20u
#define RADIO_REG_WAKEUP_IMD_ID 0x24u /* three bytes, LSB first, then the company ID (DS Figure 3-7) */
#define RADIO_REG_WAKEUP_COMPANY_ID 0x27u
#define RADIO_REG_WAKEUP_STROSC_CTRL 0x28u
#define RADIO_REG_WAKEUP_STROSC_PERIOD 0x29u
#define RADIO_REG_POWERSAVE 0x2bu
#define RADIO_REG_STATUS 0x3fu
#define RADIO_MAC_COPY_REGS (1u << 0)
#define RADIO_MAC_SLEEP (1u << 2)
#define RADIO_CRC_CHECK (1u << 0)
#define RADIO_CRC_CALC (1u << 1)
#define RADIO_IRQ_RX_NOT_EMPTY (1u << 0)
#define RADIO_IRQ_RADIO_READY (1u << 3)
#define RADIO_IRQ_LINK_ESTABLISHED (1u << 4)
#define RADIO_IRQ_WATCHDOG_WARNING (1u << 6)
#define RADIO_STATUS_LINK_UP (1u << 0)
#define RADIO_STATUS_WOKE_BY_245 (1u << 3)
#define RADIO_STATUS_TX_ACTIVE (1u << 4) /* a packet is on air */

/* Radio configuration. */
#define RADIO_BLOCK 15u          /* bytes per block: the maximum (DS "Media Access Controller": 2 to 15) */
#define RADIO_COMPANY_ID 0x5au   /* the company ID of the wake-up packet (DS Figure 3-7) */
#define RADIO_IMD_ID 0x0acc01u   /* this implant's 24-bit ID; the communicator's default master ID matches it */
#define RADIO_CHANNEL 3u         /* MICS channel number (DS Table 3-6) */
#define RADIO_MODULATION 0x05u   /* 2FSK both ways: bits 1:0 TX, 3:2 RX (model encoding, DS Table 3-5) */
#define RADIO_SNIFF_100MS 10u    /* internal strobe period, 100 ms units: 1 s (DS Figure 3-6's example) */
#define RADIO_T_WU_PULSE_US 2000u /* WU_EN high for over 1.5 ms starts the core (DS start-up method) */
#define RADIO_T_START_US 12000u   /* the core is ready ~11 ms after (model: 1 ms XO + <10 ms calibrations, DS section 6) */

static int tele_off(void);
static int g_radio_ok;    /* the chip answered at boot */
static int g_radio_fault; /* it did not */

static void radio_begin(uint32_t addr, int read) {
    spi1_format(SPI1_RADIO_CR0, SPI1_RADIO_CPSR);
    pin_set(RADIO_CS_PIN, 0);
    spi_xfer(SPI1_BASE, (uint8_t)((read ? 0x80u : 0u) | addr));
}

static void radio_end(void) {
    spi_drain(SPI1_BASE);
    pin_set(RADIO_CS_PIN, 1);
}

static void radio_wr(uint32_t addr, uint32_t v) {
    radio_begin(addr, 0);
    spi_xfer(SPI1_BASE, (uint8_t)v);
    radio_end();
}

static uint32_t radio_rd(uint32_t addr) {
    radio_begin(addr, 1);
    uint32_t v = spi_xfer(SPI1_BASE, 0);
    radio_end();
    return v;
}

static int radio_irq(void) {
    return (REG(SIO_GPIO_IN) >> RADIO_IRQ_PIN) & 1u;
}

/* The wake-up block and the core's registers, with the core up: who this implant
 * is (DS Figure 3-7), the internal strobe oscillator that sniffs for the 2.45 GHz
 * wake-up with no MCU involvement (DS Figure 3-6), the CRC of that block (without
 * a calculated one every wake-up flags a CRC error, DS section 4), the channel and
 * modulation a pin-started session uses, 15-byte blocks, and the interrupts the
 * driver waits on. COPY_REGS backs the core's registers up so every later wake-up
 * restores them (DS section 4). */
static void radio_program(void) {
    radio_begin(RADIO_REG_WAKEUP_IMD_ID, 0);
    spi_xfer(SPI1_BASE, (uint8_t)RADIO_IMD_ID);
    spi_xfer(SPI1_BASE, (uint8_t)(RADIO_IMD_ID >> 8));
    spi_xfer(SPI1_BASE, (uint8_t)(RADIO_IMD_ID >> 16));
    spi_xfer(SPI1_BASE, RADIO_COMPANY_ID); /* the next address */
    radio_end();
    radio_wr(RADIO_REG_WAKEUP_STROSC_PERIOD, RADIO_SNIFF_100MS);
    /* Wandless telemetry disabled by the battery test (v2.04): no sniffing, so no communicator can wake it. */
    radio_wr(RADIO_REG_WAKEUP_STROSC_CTRL, tele_off() ? 0u : 1u);
    radio_wr(RADIO_REG_WAKEUP_CRCCTRL, RADIO_CRC_CALC);
    radio_wr(RADIO_REG_WAKEUP_CRCCTRL, RADIO_CRC_CHECK);
    radio_wr(RADIO_REG_MAC_CHANNEL, RADIO_CHANNEL);
    radio_wr(RADIO_REG_MAC_MODULATION, RADIO_MODULATION);
    radio_wr(RADIO_REG_TXBUFF_BSIZE, RADIO_BLOCK);
    radio_wr(RADIO_REG_RXBUFF_BSIZE, RADIO_BLOCK);
    radio_wr(RADIO_REG_TXBUFF_MAXPACKSIZE, 31);
    radio_wr(RADIO_REG_POWERSAVE, 0);
    radio_wr(RADIO_REG_IRQ_ENABLE, RADIO_IRQ_RX_NOT_EMPTY | RADIO_IRQ_RADIO_READY | RADIO_IRQ_LINK_ESTABLISHED |
                                       RADIO_IRQ_WATCHDOG_WARNING);
    radio_wr(RADIO_REG_MAC_CTRL, RADIO_MAC_COPY_REGS);
}

/* Power the radio, wake its core with a WU_EN pulse, check it answers and
 * program it, then put it back to sleep sniffing. Blocks ~16 ms: boot only, before
 * the first pace. 1 on success. */
static int radio_cold_init(void) {
    pin_output(RADIO_CS_PIN, 1);
    pin_output(RADIO_WU_EN_PIN, 0);
    pin_input(RADIO_IRQ_PIN);
    pin_output(RADIO_PWR_PIN, 1); /* the load switch: tON is nanoseconds (SLVSD04 Table 7.6) */
    delay_us(100);
    pin_set(RADIO_WU_EN_PIN, 1);
    delay_us(RADIO_T_WU_PULSE_US);
    pin_set(RADIO_WU_EN_PIN, 0);
    delay_us(RADIO_T_START_US);
    /* No ID register: a register that reads back what was written says a chip is there. */
    radio_wr(RADIO_REG_MAC_CHANNEL, 0x05);
    int ok = radio_rd(RADIO_REG_MAC_CHANNEL) == 0x05u;
    if (ok) {
        radio_program();
        radio_wr(RADIO_REG_MAC_CTRL, RADIO_MAC_SLEEP);
    } else {
        pin_set(RADIO_PWR_PIN, 0); /* nothing there: no point powering it */
    }
    return ok;
}

/* --- ADXL362 --- */

static void accel_select(void) {
    spi0_format(SPI0_ACCEL_CR0, SPI0_ACCEL_CPSR);
    pin_set(ACCEL_CS_PIN, 0);
}

static void accel_deselect(void) {
    spi_drain(SPI0_BASE);
    pin_set(ACCEL_CS_PIN, 1);
}

static void accel_write(uint8_t addr, uint8_t value) {
    accel_select();
    spi_xfer(SPI0_BASE, ACCEL_WRITE_REG);
    spi_xfer(SPI0_BASE, addr);
    spi_xfer(SPI0_BASE, value);
    accel_deselect();
}

static uint8_t accel_read(uint8_t addr) {
    accel_select();
    spi_xfer(SPI0_BASE, ACCEL_READ_REG);
    spi_xfer(SPI0_BASE, addr);
    uint8_t value = spi_xfer(SPI0_BASE, 0);
    accel_deselect();
    return value;
}

static int accel_int1(void) {
    return (REG(SIO_GPIO_IN) & (1u << ACCEL_INT1_PIN)) != 0u;
}

static int accel_init(void) {
    accel_write(ACCEL_REG_SOFT_RESET, ACCEL_SOFT_RESET_KEY);
    delay_us(1000); /* 0.5 ms reset latency */
    uint8_t ad = accel_read(ACCEL_REG_DEVID_AD);
    uint8_t mst = accel_read(ACCEL_REG_DEVID_AD + 1u);
    uint8_t part = accel_read(ACCEL_REG_DEVID_AD + 2u);
    uart_puts("[adxl362] DEVID_AD=0x");
    uart_put_hex8(ad);
    uart_puts(" DEVID_MST=0x");
    uart_put_hex8(mst);
    uart_puts(" PARTID=0x");
    uart_put_hex8(part);
    int ok = ad == 0xadu && mst == 0x1du && part == 0xf2u;
    uart_puts(ok ? " (ADXL362)\n" : " -- NOT an ADXL362\n");
    if (!ok)
        return 0;
    accel_write(ACCEL_REG_FILTER_CTL, ACCEL_FILTER_2G_25HZ);
    accel_write(ACCEL_REG_FIFO_SAMPLES, ACCEL_FIFO_WATERMARK);
    accel_write(ACCEL_REG_FIFO_CONTROL, ACCEL_FIFO_STREAM);
    accel_write(ACCEL_REG_INTMAP1, ACCEL_INT_FIFO_WATERMARK);
    accel_write(ACCEL_REG_POWER_CTL, ACCEL_POWER_MEASURE);
    return 1;
}

/* One FIFO entry: bits 15:14 the axis, 13:0 the sign-extended value in LSB
 * (1 mg at +-2 g). Each axis is high-passed against its own running mean (a
 * 16-sample, 0.64 s time constant), and the magnitudes summed; Z closes a
 * sample. */
static void accel_entry(uint32_t entry) {
    uint32_t axis = entry >> 14;
    if (axis > 2u)
        return;
    int32_t v16 = ((int32_t)(entry << 18) >> 18) * 16;
    if (!(g_hp_init & (1u << axis))) {
        g_hp[axis] = v16;
        g_hp_init |= 1u << axis;
    }
    g_hp[axis] += (v16 - g_hp[axis]) / 16;
    int32_t ac = v16 - g_hp[axis];
    g_act_sum += (uint32_t)(ac < 0 ? -ac : ac);
    if (axis == 2u)
        g_act_n++;
}

/* Drain the FIFO. SCLK reaches the ADS1292R whatever its CS says, and its
 * DRDY returns high on any SCLK edge (SBAS502C 8.5.1.6): a burst that ran
 * across a conversion would hide that result. So this runs right after an
 * ADS1292R frame has been read, with ~1.7 ms to the next one, and reads at
 * most ACCEL_DRAIN_MAX entries -- a few hundred microseconds. */
static void accel_drain(void) {
    accel_select();
    spi_xfer(SPI0_BASE, ACCEL_READ_REG);
    spi_xfer(SPI0_BASE, ACCEL_REG_FIFO_ENTRIES_L);
    uint32_t n = spi_xfer(SPI0_BASE, 0);
    n |= (uint32_t)(spi_xfer(SPI0_BASE, 0) & 0x03u) << 8;
    accel_deselect();
    if (n > ACCEL_DRAIN_MAX)
        n = ACCEL_DRAIN_MAX;
    accel_select();
    spi_xfer(SPI0_BASE, ACCEL_READ_FIFO);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t lo = spi_xfer(SPI0_BASE, 0);
        uint32_t hi = spi_xfer(SPI0_BASE, 0);
        accel_entry(lo | (hi << 8));
    }
    accel_deselect();
}

/* --- I2C0 --- */

static void i2c_init(void) {
    REG(PAD_GPIO(EEPROM_SDA_PIN) + CLR_ALIAS) = PAD_ISO;
    REG(PAD_GPIO(EEPROM_SCL_PIN) + CLR_ALIAS) = PAD_ISO;
    REG(PAD_GPIO(EEPROM_SDA_PIN) + SET_ALIAS) = PAD_IE | PAD_PUE;
    REG(PAD_GPIO(EEPROM_SCL_PIN) + SET_ALIAS) = PAD_IE | PAD_PUE;
    REG(GPIO_CTRL(EEPROM_SDA_PIN)) = FUNCSEL_I2C;
    REG(GPIO_CTRL(EEPROM_SCL_PIN)) = FUNCSEL_I2C;

    /* 400 kHz from the 12 MHz clk_sys, as pico-sdk's i2c_set_baudrate works
     * it out: a 30-cycle period, 3/5 of it low, a 1-cycle spike filter and
     * a 4-cycle SDA hold. Fast-mode plus (1 MHz) would need a faster clk_sys:
     * the controller wants at least 8 cycles per SCL phase. */
    REG(I2C_ENABLE) = 0;
    REG(I2C_CON) = I2C_CON_MASTER_FAST;
    REG(I2C_FS_SCL_HCNT) = 12;
    REG(I2C_FS_SCL_LCNT) = 18;
    REG(I2C_FS_SPKLEN) = 1;
    REG(I2C_SDA_HOLD) = 4;
    REG(I2C_TAR) = EEPROM_ADDR;
    REG(I2C_ENABLE) = 1;
}

/* Consume a pending transfer abort, if any. A NACK raises TX_ABRT, which is
 * sticky: left set, every later transfer would look aborted too. */
static int i2c_take_abort(void) {
    if ((REG(I2C_RAW_INTR_STAT) & INTR_TX_ABRT) == 0u)
        return 0;
    (void)REG(I2C_TX_ABRT_SOURCE);
    (void)REG(I2C_CLR_TX_ABRT);
    (void)REG(I2C_CLR_STOP_DET);
    return 1;
}

static void i2c_put(uint32_t cmd) {
    while (!(REG(I2C_STATUS) & I2C_STATUS_TFNF)) {
    }
    REG(I2C_DATA_CMD) = cmd;
}

/* The STOP that ends a transfer: 1 once it is on the bus, 0 if the transfer
 * aborted (a NACK) or never finished. */
static int i2c_wait_stop(void) {
    uint32_t start = time_us();
    for (;;) {
        if (i2c_take_abort())
            return 0;
        if (REG(I2C_RAW_INTR_STAT) & INTR_STOP_DET) {
            (void)REG(I2C_CLR_STOP_DET);
            return 1;
        }
        if ((uint32_t)(time_us() - start) > I2C_TIMEOUT_US)
            return 0;
    }
}

/* --- AT24C256C --- */

/* A sequential read of `n` bytes from `addr`: a dummy write sets the address
 * pointer, then a repeated START reads (DS20006270 §6.3, random read). */
static int eeprom_read(uint32_t addr, uint8_t *dst, uint32_t n) {
    (void)i2c_take_abort();
    i2c_put(CMD_RESTART | ((addr >> 8) & 0x7fu));
    if (i2c_take_abort())
        return 0;
    i2c_put(addr & 0xffu);
    for (uint32_t i = 0; i < n; i++) {
        i2c_put(CMD_READ | (i == 0u ? CMD_RESTART : 0u) | (i == n - 1u ? CMD_STOP : 0u));
        uint32_t start = time_us();
        while (REG(I2C_RXFLR) == 0u) {
            if (i2c_take_abort() || (uint32_t)(time_us() - start) > I2C_TIMEOUT_US)
                return 0;
        }
        dst[i] = (uint8_t)REG(I2C_DATA_CMD);
    }
    return i2c_wait_stop();
}

/* A page write of `n` bytes, which must not cross a 64-byte page: the part
 * wraps within the page (§5.2). The write cycle starts at the STOP. */
static int eeprom_write(uint32_t addr, const uint8_t *src, uint32_t n) {
    (void)i2c_take_abort();
    i2c_put(CMD_RESTART | ((addr >> 8) & 0x7fu));
    if (i2c_take_abort())
        return 0;
    i2c_put(addr & 0xffu);
    for (uint32_t i = 0; i < n; i++)
        i2c_put((i == n - 1u ? CMD_STOP : 0u) | src[i]);
    return i2c_wait_stop();
}

/* --- Diagnostic store ---
 *
 * Everything the clinic reads back. One structure in RAM is the working copy,
 * laid out byte for byte as the AT24C256C's 32 KB, so checkpointing is a copy
 * of whichever 32-byte chunks changed (`store_dirty`), one chunk per write
 * cycle, in the quiet moment after an ADS1292R frame. At power-on the whole
 * EEPROM is read back into RAM (~0.7 s at 400 kHz, before pacing starts), so
 * a reset loses nothing but what changed since the last checkpoint. A blank
 * part (all 0xff) or one from another layout is a new device: formatted.
 *
 *   0x0000  header     magic, the implant clock, lifetime totals, ring counts
 *   0x0040  counters   since the last clear
 *   0x00c0  histograms 5 x 16 rate bins, since the last clear
 *   0x0200  episodes   ring of 64
 *   0x0600  trends     ring of 256
 *   0x1200  safety     the Safety Mode latch, the time scale, the uncommanded-reset ring
 *   0x2000  EGMs       4 strips of 6 s, 6 KB each
 *
 * The programmer reads the raw structures, little-endian, so their layout is
 * the wire format too (programmer/pacemaker_programmer.py mirrors it).
 *
 * The implant clock. The battery ages 1e6 times faster than real time
 * (peripherals.yaml `aging_factor`), and so do the diagnostics' dates, so the
 * battery trend and the ERI date land where a decade-long cell puts them: one
 * guest microsecond is one implant second, and a guest second is 11.6 days.
 * Rates, intervals, durations and EGMs stay in heart time -- the heart beats
 * at its real rate -- so an episode is stamped with its implant date but
 * lasts as many seconds as it did on the marker channel. */
#define STORE_MAGIC 0x53444d50u   /* "PMDS" */
#define STORE_LAYOUT 1u
#define STORE_CHUNK 32u
#define STORE_BYTES 0x8000u
#define STORE_CHUNKS (STORE_BYTES / STORE_CHUNK)

#define EP_SLOTS 64u
#define TREND_SLOTS 256u
#define EGM_SLOTS 4u
#define EGM_SPS 250u        /* 500 SPS front end, pairs averaged */
#define EGM_SAMPLES 1500u   /* 6 s per channel */
#define EGM_PRE 500u        /* 2 s of it before the trigger */
#define EGM_UV_PER_LSB 16u  /* +-524 mV: the ADS1292R's whole +-403 mV range */
#define EGM_MARKERS 32u
#define HIST_BINS 16u       /* <40, 40-49, ..., 170-179, >=180 bpm */

/* Trends: one point per TREND_PERIOD_S implant seconds, 92.6 implant days --
 * quarterly (8 s of guest time at the default scale). The clock is checkpointed to
 * the EEPROM every CKPT_PERIOD_S implant seconds but never more often than
 * CKPT_FLOOR_US of guest time: at the scenario's 3600x that is 250 ms, 15
 * implant minutes, so a reset loses under 0.5% of the 48 h window. */
#define TREND_PERIOD_S 8000000u
#define CKPT_PERIOD_S 900u
#define CKPT_FLOOR_US 250000u

/* High-rate episodes: ONSET consecutive intervals faster than the rate open
 * one, TERM consecutive slower ones close it. Atrial: sensed atrial events
 * (AS or AR); ventricular: sensed ventricular ones. */
#define AHR_BPM 170u
#define VHR_BPM 170u
#define HR_ONSET_BEATS 8u
#define HR_TERM_BEATS 8u

/* Episode kinds; also the EGM's trigger. */
#define EP_POWER_ON 1u /* a = 1 for a newly formatted store (implant), b = reset cause (RC_*), c = 1 in Safety Mode */
#define EP_LEAD_OFF 2u /* a = LOFF_STAT chamber bits */
#define EP_ERI 3u      /* a = loaded battery mV */
#define EP_EOL 4u      /* a = loaded battery mV */
#define EP_MAGNET 5u   /* a = the asynchronous mode */
#define EP_AHR 6u      /* a = fastest bpm, b = beats */
#define EP_VHR 7u      /* a = fastest bpm, b = beats */
#define EP_CI 8u       /* a = chronotropic index %, b/c = exertion/rest atrial bpm x10 */
#define EP_MODE 9u     /* a = old mode, b = new mode, c = 1 if ERI forced it */
#define EP_RADIO_OFF 10u /* wandless (radio) telemetry disabled at ERI; a = loaded battery mV */
#define EP_SAFETY 11u    /* Safety Mode latched; a = uncommanded resets in the window */
#define EP_NOISE 12u     /* noise reversion to VOO; a = noise-window senses, b = V paces in it; with an EGM */
#define EP_PAUSE 13u     /* no ventricular event for over PAUSE_US; dur_ms = the whole gap; with an EGM */
#define EP_CODE1003 14u  /* a = burst mV, b = expected mV, c = projected longevity x100 (years) */
#define EP_TELE_OFF 15u  /* wandless telemetry disabled; a = burst mV, b = expected mV, c = DIS_* */
#define EP_FW_LEVEL 16u  /* firmware level changed; a = old, b = new, c = 1 if telemetry was re-enabled */
#define EP_ONGOING 0xffffffffu
#define EGM_NONE 0xffu

/* Marker codes, for the EGM's marker annotations. */
#define MK_AS 1u
#define MK_AP 2u
#define MK_AR 3u
#define MK_VS 4u
#define MK_VP 5u
#define MK_VR 6u

/* What started the AV interval a ventricular event ends. */
#define SEQ_NONE 0u
#define SEQ_AS 1u
#define SEQ_AP 2u

#define H_AS 0u /* atrial sensed, AS and AR */
#define H_AP 1u
#define H_VS 2u
#define H_VP 3u
#define H_SIR 4u /* one count per 2 s activity period */
#define N_HIST 5u

struct store_header {
    uint32_t magic;
    uint16_t layout;
    uint16_t reserved0;
    uint32_t implant_s;        /* the implant clock */
    uint32_t epoch_at_implant; /* Unix time of implant second 0; 0 until a programmer sets the time */
    uint32_t last_clear_s;
    uint32_t last_session_s;
    uint32_t power_ons;
    uint32_t lifetime_ap, lifetime_vp;
    uint32_t episodes; /* logged since the last clear; slot = n % EP_SLOTS */
    uint32_t trends;   /* logged since implant; slot = n % TREND_SLOTS */
    uint32_t egms;     /* stored since the last clear; slot = n % EGM_SLOTS */
    uint32_t reserved[4];
};

struct counters {
    uint32_t as, ap, ar, vs, vp, vr, pvc;
    uint32_t as_vs, as_vp, ap_vs, ap_vp;
    uint32_t magnet, mode_changes, ahr, vhr, lead_off;
    uint32_t noise;  /* noise-reversion episodes (Safety Mode) */
    uint32_t pauses; /* ventricular pauses (g_pause_us, default 2 s) */
    uint32_t reserved[14];
};

struct episode {
    uint32_t start_s; /* implant clock */
    uint32_t dur_ms;  /* heart time; EP_ONGOING while open; 0 for an instant */
    uint8_t kind;
    uint8_t egm; /* the EGM slot, or EGM_NONE */
    uint16_t a, b, c;
};

struct trend {
    uint16_t day; /* implant day */
    uint16_t batt_mv;
    uint16_t act_mg;
    uint8_t pct_ap, pct_vp; /* of all atrial / ventricular events */
    uint8_t a_rate, v_rate; /* mean bpm, from the intervals */
    uint8_t mode;
    uint8_t flags; /* 1:0 battery state, 2 magnet, 3 lead off, 4 AHR, 5 VHR */
};

struct egm_marker {
    uint16_t sample;
    uint8_t code;
    uint8_t reserved;
};

struct egm {
    uint32_t start_s;  /* implant clock at the trigger */
    uint16_t episode;  /* the episode's number, low 16 bits */
    uint8_t kind;      /* the trigger; 0 = empty slot */
    uint8_t n_markers;
    uint16_t n_samples; /* EGM_SAMPLES once complete, 0 while recording */
    uint16_t pre_samples;
    uint16_t sps;
    uint16_t uv_per_lsb;
    struct egm_marker markers[EGM_MARKERS];
    int16_t samples[EGM_SAMPLES][2]; /* [0] ventricular (CH1), [1] atrial (CH2) */
};

/* The uncommanded-reset ring and the Safety Mode latch: the EEPROM survives a
 * reset, POWMAN's AON timer and scratch registers do not (RP2350 datasheet 7.3.1,
 * Table 560, EXTERNAL RESET row), so this is the only record there is. */
#define RESET_SLOTS 16u
#define SAFETY_LATCHED 0x45464153u /* "SAFE" */
#define RC_COLD 0u       /* neither WATCHDOG.REASON nor CHIP_RESET.HAD_RUN_LOW: power-on */
#define RC_COMMANDED 1u  /* the watchdog: a programmer's `reboot` (or a firmware hang) */
#define RC_SUPERVISOR 2u /* RUN held low by the supply supervisor: an uncommanded system reset */
struct reset_rec {
    uint32_t implant_s; /* when, on the implant clock (checkpoint + the supervisor's hold) */
    uint8_t cause;      /* RC_* */
    uint8_t reserved[3];
};

struct safety_block {
    uint32_t latched;   /* SAFETY_LATCHED, forever */
    uint32_t latched_s; /* implant clock at the latch */
    uint32_t total;     /* uncommanded resets since implant; slot = n % RESET_SLOTS */
    uint32_t scale;     /* implant s per guest s; 0 = SCALE_DEFAULT */
    uint32_t window_s;  /* SAFETY_WINDOW_S, so a programmer need not know it */
    uint32_t resets_needed;
    uint32_t reserved[2];
    struct reset_rec rl[RESET_SLOTS];
};

/* Firmware level and the daily battery test (BSC ACCOLADE physician letter, Sept 2025, Appendix A
 * "Description of Software Enhancement"). All zeros -- a blank or older store -- is the base level. */
#define BT_RECS 16u
#define FW_BASE 0u  /* the firmware as shipped: the recall's behaviour */
#define FW_V204 1u  /* "Model 3869 v2.04": the battery test and the telemetry shut-off, with both shipped bugs */
#define FW_FIXED 2u /* the forthcoming fix the letter describes */
#define FW_F_CODE1003 (1u << 0) /* red alert: voltage too low for projected remaining longevity */
#define FW_F_TELE_OFF (1u << 1) /* wandless telemetry disabled by the test */
#define FW_F_EXPLANT (1u << 2)  /* explant indicator reached (ERI, or the false positive) */
#define FW_F_CLEARED (1u << 3)  /* the fixed level re-enabled telemetry after a false positive */
#define DIS_NONE 0u
#define DIS_IMPEDANCE 1u  /* the test saw a high-impedance cell */
#define DIS_BAD_SAMPLE 2u /* the test evaluated a burst sample that was never taken (v2.04 bug 2) */
struct bt_rec {
    uint32_t implant_s;
    uint16_t v_pre_mv;   /* the rail with the radio core up and idle, before the burst */
    uint16_t v_burst_mv; /* the rail during the burst; 0 = no sample was taken */
    uint16_t v_exp_mv;   /* what a healthy cell shows for the projected longevity */
    uint16_t l_x100;     /* projected remaining longevity, years x 100 */
    uint8_t flags;       /* BT_REC_* */
    uint8_t verdict;     /* BT_V_* */
    uint16_t reserved;
};
struct fw_block {
    uint32_t level;       /* FW_* */
    uint32_t upgraded_s;  /* implant clock at the last upgrade */
    uint32_t flags;       /* FW_F_* */
    uint32_t alert_s;     /* implant clock at Code-1003 */
    uint32_t disabled_s;  /* implant clock when wandless telemetry was disabled */
    uint32_t explant_cal; /* explant indicator date: seconds since 2000-01-01 on the device calendar; 0 = never recorded */
    uint32_t tests;       /* tests recorded since implant; slot = n % BT_RECS */
    uint32_t next_test_s; /* implant clock of the next daily test */
    uint32_t run_1003;    /* consecutive tests past the Code-1003 margin */
    uint32_t run_dis;     /* consecutive tests past the disable margin */
    uint32_t disable_why; /* DIS_* */
    uint32_t reserved[5];
    struct bt_rec rec[BT_RECS];
};

struct store {
    struct store_header hdr;
    struct counters cnt;
    uint32_t hist[N_HIST][HIST_BINS];
    struct episode ep[EP_SLOTS];
    struct trend tr[TREND_SLOTS];
    struct safety_block sf;
    uint8_t reserved_a[0x1300 - 0x1200 - sizeof(struct safety_block)];
    struct fw_block fw;
    uint8_t reserved_b[0x2000 - 0x1300 - sizeof(struct fw_block)];
    struct egm egm[EGM_SLOTS];
};

_Static_assert(sizeof(struct store_header) == 0x40, "header layout");
_Static_assert(__builtin_offsetof(struct store, cnt) == 0x0040, "counters offset");
_Static_assert(__builtin_offsetof(struct store, hist) == 0x00c0, "histograms offset");
_Static_assert(__builtin_offsetof(struct store, ep) == 0x0200, "episodes offset");
_Static_assert(__builtin_offsetof(struct store, tr) == 0x0600, "trends offset");
_Static_assert(__builtin_offsetof(struct store, sf) == 0x1200, "safety block offset");
_Static_assert(sizeof(struct safety_block) == 32u + 8u * RESET_SLOTS, "safety block size");
_Static_assert(__builtin_offsetof(struct store, fw) == 0x1300, "firmware block offset");
_Static_assert(sizeof(struct bt_rec) == 16u, "battery test record size");
_Static_assert(sizeof(struct fw_block) == 64u + 16u * BT_RECS, "firmware block size");
_Static_assert(__builtin_offsetof(struct store, egm) == 0x2000, "EGM offset");
_Static_assert(sizeof(struct egm) == 0x1800, "EGM slot size");
_Static_assert(sizeof(struct store) == STORE_BYTES, "the whole AT24C256C");

static struct store g_store;

/* Wandless telemetry has been disabled by the battery test (v2.04 and later). */
static int tele_off(void) {
    return (g_store.fw.flags & FW_F_TELE_OFF) != 0u;
}
static uint32_t g_dirty[STORE_CHUNKS / 32u]; /* a bit per 32-byte chunk */
static uint32_t g_dirty_cursor;
static int g_eeprom_ok;
static uint32_t g_eeprom_ready_us; /* tWR: the part is busy until then */
static uint32_t g_implant_last_us;

static void zero(void *p, uint32_t n) {
    volatile uint8_t *b = p;
    while (n--)
        *b++ = 0;
}

static void store_dirty(const void *p, uint32_t n) {
    uint32_t off = (uint32_t)((const uint8_t *)p - (const uint8_t *)&g_store);
    for (uint32_t c = off / STORE_CHUNK; c <= (off + n - 1u) / STORE_CHUNK; c++)
        g_dirty[c / 32u] |= 1u << (c % 32u);
}

/* One dirty chunk to the EEPROM, if the part is past its last write cycle.
 * The chunk is copied as it is now; a change mid-write dirties it again. */
static void eeprom_service(uint32_t now) {
    if (!g_eeprom_ok || (int32_t)(now - g_eeprom_ready_us) < 0)
        return;
    for (uint32_t i = 0; i < STORE_CHUNKS / 32u; i++) {
        uint32_t w = (g_dirty_cursor / 32u + i) % (STORE_CHUNKS / 32u);
        if (!g_dirty[w])
            continue;
        uint32_t bit = 0;
        while (!(g_dirty[w] & (1u << bit)))
            bit++;
        g_dirty[w] &= ~(1u << bit);
        uint32_t chunk = w * 32u + bit;
        g_dirty_cursor = chunk + 1u;
        if (!eeprom_write(chunk * STORE_CHUNK, (const uint8_t *)&g_store + chunk * STORE_CHUNK, STORE_CHUNK)) {
            g_eeprom_ok = 0;
            to(TELEMETRY);
            uart_puts("ALARM: DIAG_STORE -- AT24C256C write failed: diagnostics now kept in RAM only\n");
        }
        g_eeprom_ready_us = time_us() + EEPROM_TWR_US;
        return;
    }
}

/* Counters, histograms and the header (the implant clock, ring counts):
 * what changes on every beat is only written out at a checkpoint. */
static void diag_checkpoint(void) {
    store_dirty(&g_store.hdr, sizeof g_store.hdr);
    store_dirty(&g_store.cnt, sizeof g_store.cnt);
    store_dirty(g_store.hist, sizeof g_store.hist);
}

/* The implant clock advances by elapsed guest time x g_scale / 1e6, with the
 * remainder carried; the span is taken in chunks whose product cannot overflow. */
static uint32_t g_implant_frac;

static void implant_update(uint32_t now) {
    uint32_t left = now - g_implant_last_us;
    g_implant_last_us = now;
    while (left) {
        uint32_t step = left > 4000u ? 4000u : left; /* 4000 x 1e6 + a remainder < 2^32 */
        left -= step;
        g_implant_frac += step * g_scale;
        g_store.hdr.implant_s += g_implant_frac / 1000000u;
        g_implant_frac %= 1000000u;
    }
}

/* --- Episodes --- */

static struct episode *ep_get(uint32_t num) {
    if (num >= g_store.hdr.episodes || g_store.hdr.episodes - num > EP_SLOTS)
        return 0; /* never logged, or since overwritten */
    return &g_store.ep[num % EP_SLOTS];
}

/* Log one; returns its number. `dur_ms` is EP_ONGOING for one that stays
 * open until ep_close. */
static uint32_t ep_log(uint32_t kind, uint32_t dur_ms, uint32_t a, uint32_t b, uint32_t c) {
    uint32_t num = g_store.hdr.episodes++;
    struct episode *e = &g_store.ep[num % EP_SLOTS];
    e->start_s = g_store.hdr.implant_s;
    e->dur_ms = dur_ms;
    e->kind = (uint8_t)kind;
    e->egm = EGM_NONE;
    e->a = (uint16_t)a;
    e->b = (uint16_t)b;
    e->c = (uint16_t)c;
    store_dirty(e, sizeof *e);
    store_dirty(&g_store.hdr, sizeof g_store.hdr);
    return num;
}

static void ep_close(uint32_t num, uint32_t dur_us, uint32_t a, uint32_t b) {
    struct episode *e = ep_get(num);
    if (!e)
        return;
    e->dur_ms = dur_us / 1000u;
    if (a)
        e->a = (uint16_t)a;
    if (b)
        e->b = (uint16_t)b;
    store_dirty(e, sizeof *e);
}

/* --- Stored EGMs ---
 *
 * The front end's two channels, pairs of samples averaged to 250 SPS, run
 * through a 2 s pre-trigger ring all the time. A trigger copies the ring into
 * the next slot (the oldest is overwritten) and keeps recording to 6 s, with
 * the marker channel's events annotated on the strip. One strip at a time: a
 * trigger while one is recording gets none. */
static int16_t g_pre[EGM_PRE][2];
static uint32_t g_egm_clock; /* 250 SPS samples since boot */
static int32_t g_pair[2];
static uint32_t g_pair_n;
static int g_rec_slot = -1;
static uint32_t g_rec_start; /* g_egm_clock at the strip's first sample */
#define MK_RING 16u
static uint32_t g_mk_stamp[MK_RING];
static uint8_t g_mk_code[MK_RING];
static uint32_t g_mk_n;

static int16_t egm_lsb(int32_t uv2) {
    int32_t v = uv2 / (2 * (int32_t)EGM_UV_PER_LSB);
    return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

static void egm_add_marker(struct egm *e, uint32_t stamp, uint32_t code) {
    if (e->n_markers >= EGM_MARKERS || stamp < g_rec_start)
        return;
    e->markers[e->n_markers].sample = (uint16_t)(stamp - g_rec_start);
    e->markers[e->n_markers].code = (uint8_t)code;
    e->n_markers++;
}

static void egm_mark(uint32_t code) {
    g_mk_stamp[g_mk_n % MK_RING] = g_egm_clock;
    g_mk_code[g_mk_n % MK_RING] = (uint8_t)code;
    g_mk_n++;
    if (g_rec_slot >= 0)
        egm_add_marker(&g_store.egm[g_rec_slot], g_egm_clock, code);
}

/* One front-end frame, in uV. */
static void egm_sample(int32_t v_uv, int32_t a_uv) {
    g_pair[0] += v_uv;
    g_pair[1] += a_uv;
    if (++g_pair_n < 2u)
        return;
    int16_t v = egm_lsb(g_pair[0]), a = egm_lsb(g_pair[1]);
    g_pair[0] = g_pair[1] = 0;
    g_pair_n = 0;
    if (g_rec_slot >= 0) {
        struct egm *e = &g_store.egm[g_rec_slot];
        uint32_t n = g_egm_clock - g_rec_start;
        e->samples[n][0] = v;
        e->samples[n][1] = a;
        if (n + 1u == EGM_SAMPLES) {
            e->n_samples = EGM_SAMPLES;
            store_dirty(e, sizeof *e);
            g_rec_slot = -1;
        }
    }
    g_pre[g_egm_clock % EGM_PRE][0] = v;
    g_pre[g_egm_clock % EGM_PRE][1] = a;
    g_egm_clock++;
}

/* Start a strip for episode `num`; returns its slot, or EGM_NONE. */
static uint32_t egm_trigger(uint32_t kind, uint32_t num) {
    if (g_rec_slot >= 0)
        return EGM_NONE;
    uint32_t slot = g_store.hdr.egms++ % EGM_SLOTS;
    struct egm *e = &g_store.egm[slot];
    uint32_t pre = g_egm_clock < EGM_PRE ? g_egm_clock : EGM_PRE;
    g_rec_start = g_egm_clock - pre;
    for (uint32_t i = 0; i < pre; i++) {
        e->samples[i][0] = g_pre[(g_rec_start + i) % EGM_PRE][0];
        e->samples[i][1] = g_pre[(g_rec_start + i) % EGM_PRE][1];
    }
    e->start_s = g_store.hdr.implant_s;
    e->episode = (uint16_t)num;
    e->kind = (uint8_t)kind;
    e->n_markers = 0;
    e->n_samples = 0;
    e->pre_samples = (uint16_t)pre;
    e->sps = EGM_SPS;
    e->uv_per_lsb = EGM_UV_PER_LSB;
    /* The markers the pre-trigger part already holds, oldest first. */
    uint32_t first = g_mk_n > MK_RING ? g_mk_n - MK_RING : 0u;
    for (uint32_t i = first; i < g_mk_n; i++)
        egm_add_marker(e, g_mk_stamp[i % MK_RING], g_mk_code[i % MK_RING]);
    g_rec_slot = (int)slot;
    struct episode *ep = ep_get(num);
    if (ep) {
        ep->egm = (uint8_t)slot;
        store_dirty(ep, sizeof *ep);
    }
    return slot;
}

/* An episode that gets a strip. */
static uint32_t ep_log_egm(uint32_t kind, uint32_t a) {
    uint32_t num = ep_log(kind, EP_ONGOING, a, 0, 0);
    egm_trigger(kind, num);
    return num;
}

/* --- Counters, histograms, high-rate detection and trends --- */

static uint32_t hist_bin(uint32_t bpm) {
    if (bpm < 40u)
        return 0;
    if (bpm >= 180u)
        return HIST_BINS - 1u;
    return (bpm - 40u) / 10u + 1u;
}

struct rate_detector {
    uint32_t fast, slow, beats, max_bpm, run_start_us, last_fast_us, ep;
    int open;
};

static struct rate_detector g_ahr, g_vhr;

/* One interval, `iv` ending at `t`, into a high-rate detector. The episode
 * records the run itself -- from the start of its first fast interval to the
 * end of its last, and how many there were -- not the windows that confirmed
 * its onset and its end. */
static void rate_detect(struct rate_detector *d, uint32_t kind, int fast, uint32_t bpm, uint32_t t, uint32_t iv) {
    if (!d->open) {
        if (!fast) {
            d->fast = 0;
            return;
        }
        if (d->fast++ == 0) {
            d->run_start_us = t - iv;
            d->max_bpm = 0;
        }
        if (bpm > d->max_bpm)
            d->max_bpm = bpm;
        d->last_fast_us = t;
        if (d->fast < HR_ONSET_BEATS)
            return;
        d->open = 1;
        d->slow = 0;
        d->beats = d->fast;
        d->ep = ep_log_egm(kind, d->max_bpm);
        if (kind == EP_AHR)
            g_store.cnt.ahr++;
        else
            g_store.cnt.vhr++;
        to(TELEMETRY);
        uart_puts(kind == EP_AHR ? "EPISODE: atrial high rate, " : "EPISODE: ventricular high rate, ");
        uart_put_udec(bpm);
        uart_puts(" bpm\n");
        return;
    }
    if (fast) {
        d->beats++;
        d->slow = 0;
        d->last_fast_us = t;
        if (bpm > d->max_bpm)
            d->max_bpm = bpm;
        return;
    }
    if (++d->slow < HR_TERM_BEATS)
        return;
    d->open = 0;
    d->fast = 0;
    ep_close(d->ep, d->last_fast_us - d->run_start_us, d->max_bpm, d->beats);
}

/* The current trend period's running totals. */
static struct {
    uint32_t a_all, ap, v_all, vp;
    uint32_t a_ms, a_n, v_ms, v_n;
    uint32_t act_sum, act_n;
    uint32_t flags;
} g_tp;

static uint32_t g_last_a_ev, g_last_v_ev; /* for the rate histograms */
static int g_have_a_ev, g_have_v_ev;
static int g_a_since_v = 1; /* an atrial event since the last ventricular one */
static uint32_t g_diag_mode;

/* An atrial event: AS, AP or AR. */
static void diag_a(uint32_t t, uint32_t mk) {
    struct counters *c = &g_store.cnt;
    egm_mark(mk);
    g_a_since_v = 1;
    g_tp.a_all++;
    if (mk == MK_AP) {
        c->ap++;
        g_store.hdr.lifetime_ap++;
        g_tp.ap++;
    } else if (mk == MK_AS) {
        c->as++;
    } else {
        c->ar++;
    }
    if (g_have_a_ev && t != g_last_a_ev) {
        uint32_t iv = t - g_last_a_ev;
        uint32_t bpm = 60000000u / iv;
        g_store.hist[mk == MK_AP ? H_AP : H_AS][hist_bin(bpm)]++;
        g_tp.a_ms += iv / 1000u;
        g_tp.a_n++;
        rate_detect(&g_ahr, EP_AHR, mk != MK_AP && iv < 60000000u / AHR_BPM, bpm, t, iv);
    }
    g_last_a_ev = t;
    g_have_a_ev = 1;
}

/* --- Automatic gain control, noise reversion and ventricular pauses (Safety Mode) ---
 *
 * AGC. BSC ACCOLADE physician letter, Sept 2025, Appendix A / Table 3: Safety Mode senses
 * "AGC 0.25 mV". BSC's own AGC algorithm is not public; this is a labelled model of the
 * published conventional design, "threshold templating" in digital AGC pacemaker
 * sense amplifiers (US 5,658,317 and US 5,620,466, Cardiac Pacemakers Inc., the Guidant lineage
 * that became Boston Scientific): after a ventricular event the sensing threshold is set to a
 * fraction of that event's measured peak amplitude and decays in steps to the minimum, so a large
 * R or T wave cannot be re-sensed but the amplifier is at full gain again before the next beat.
 * Model choices: the peak is the largest 4 ms first difference (the same quantity the sense test
 * uses) seen in the VR_QUIET_US after the event (after blanking for a pace); the template is
 * AGC_PCT of it, applies once the refractory period ends (VRP_US), and falls linearly to the
 * programmed 0.25 mV over AGC_DECAY_US, i.e. back at maximum sensitivity at VRP + decay = 750 ms,
 * before the 827.6 ms escape interval ends. A sensed noise burst measures its own, small, peak,
 * so interference makes the template shrink, as in a real amplifier. */
#define AGC_PCT 75u
#define AGC_DECAY_US 500000u
static uint32_t g_agc_ev;   /* the ventricular event the template follows */
static uint32_t g_agc_peak; /* its measured peak, uV of first difference */
static uint32_t g_agc_max;  /* the running peak while its measuring window is open */
static int g_agc_track;

static void agc_event(uint32_t t) {
    g_agc_ev = t;
    g_agc_max = 0;
    g_agc_track = 1;
}

/* Every front-end frame of the ventricular channel: `slope` is |x[n] - x[n-2]| in uV. */
static void agc_sample(uint32_t t, uint32_t slope, int blanked) {
    if (!g_agc_track)
        return;
    if ((uint32_t)(t - g_agc_ev) >= VR_QUIET_US) {
        g_agc_track = 0;
        g_agc_peak = g_agc_max;
    } else if (!blanked && slope > g_agc_max) {
        g_agc_max = slope;
    }
}

/* The sensing threshold at `t`, in uV. Outside Safety Mode it is the programmed one, untouched. */
static uint32_t agc_threshold(uint32_t t) {
    if (!g_safety)
        return g_sens_uv;
    uint32_t base = g_sens_uv;
    uint32_t high = g_agc_peak / 100u * AGC_PCT;
    uint32_t since = t - g_agc_ev;
    if (high <= base)
        return base;
    if (since < VRP_US)
        return high;
    since -= VRP_US;
    if (since >= AGC_DECAY_US)
        return base;
    return high - (high - base) * (since / 1000u) / (AGC_DECAY_US / 1000u);
}

/* Noise. The conventional design: after the absolute refractory period (here the first
 * NOISE_START_US of a cycle, the event's own tail) there is a relative refractory, or
 * noise-sampling, period (100 ms: up to VRP_US here) in which a detected signal is classed as
 * noise and restarts the relative refractory for another NOISE_RRP_US; continuous detection at
 * 10 Hz or faster reverts the generator to asynchronous operation at its programmed rate for as
 * long as the noise lasts (US 5,522,857, "Pacemaker with improved detection of and response
 * to noise"; the same mechanism is described in the pacing literature as the noise-reversion
 * mode, and myopotential sensing is reported to alternate between inhibiting the pacer and
 * reverting it). Here: a crossing in the window opens a chain; each crossing NOISE_DEAD_US
 * after the last counted one that falls inside the restarted RRP extends it and counts; NOISE_EVENTS
 * in a chain declare reversion (Table 3 "noise response VOO": VVI becomes VOO). While the RRP is
 * open or reversion is on, no crossing is a beat, so interference never inhibits the pace from
 * inside a chain. A burst that starts outside the window and outside a chain is sensed as a
 * beat (VS) and inhibits the pace: intermittent interference does that, which is the clinical
 * subtlety behind the letter's "pacing inhibition due to myopotential oversensing". Reversion
 * ends at the next VP or VS after the RRP has lapsed with no further signal. The detection
 * threshold is the programmed 0.25 mV, not the AGC template. */
#define NOISE_START_US 150000u
#define NOISE_RRP_US 100000u
#define NOISE_DEAD_US 20000u
#define NOISE_EVENTS 5u
static int g_noise;
static int g_chain_on;
static uint32_t g_chain, g_rrp_end, g_nz_dead;
static uint32_t g_noise_start, g_noise_events, g_noise_paces, g_noise_ep;
static uint32_t g_pause_ref; /* the last VP or VS: the cycle's start, and the pause clock */

static int noise_enabled(void) {
    return g_safety;
}

/* Every ventricular threshold crossing at the programmed sensitivity (not blanked, lead on).
 * Returns 1 if it is classed as noise rather than a candidate beat. */
static int noise_cross(uint32_t t) {
    uint32_t into = t - g_pause_ref;
    int in_rrp = g_chain_on && (int32_t)(g_rrp_end - t) > 0;
    if (!in_rrp)
        g_chain_on = 0;
    int window = into >= NOISE_START_US && into < VRP_US;
    if (!window && !in_rrp)
        return g_noise;
    if ((int32_t)(t - g_nz_dead) >= 0) {
        g_nz_dead = t + NOISE_DEAD_US;
        g_rrp_end = t + NOISE_RRP_US;
        g_chain = in_rrp ? g_chain + 1u : 1u;
        g_chain_on = 1;
        g_noise_events++;
        if (!g_noise && noise_enabled() && g_chain >= NOISE_EVENTS) {
            g_noise = 1;
            g_noise_start = t;
            g_noise_events = g_chain;
            g_noise_paces = 0;
            g_store.cnt.noise++;
            g_noise_ep = ep_log_egm(EP_NOISE, 0);
            to(TELEMETRY);
            uart_puts("NOISE reversion on (asynchronous VOO while the ventricular channel is noisy) t=");
            uart_put_fixed(t / 1000u, 1000, 3);
            uart_puts("\n");
        }
    }
    return 1;
}

/* A ventricular event that ends a cycle: VP or VS. */
static void noise_cycle(uint32_t t, uint32_t mk) {
    if (g_noise)
        g_noise_paces += mk == MK_VP;
    if (g_noise && (int32_t)(t - g_rrp_end) > 0) {
        g_noise = 0;
        g_chain_on = 0;
        ep_close(g_noise_ep, t - g_noise_start, g_noise_events, g_noise_paces);
        to(TELEMETRY);
        uart_puts("NOISE reversion off, ");
        uart_put_udec(g_noise_events);
        uart_puts(" noise senses, ");
        uart_put_udec(g_noise_paces);
        uart_puts(" asynchronous paces t=");
        uart_put_fixed(t / 1000u, 1000, 3);
        uart_puts("\n");
    }
}

/* Pauses: a device records the harm it can see itself. The ventricle's own events are VP
 * and VS (a VR is never a beat). The EGM covers the 2 s before the 2 s mark and 4 s after. */
#define PAUSE_US 2000000u
/* Programmable like a loop recorder's pause duration (tty0 `pause <ms>`, 1000..10000): a diagnostic
 * setting, not a pacing parameter, so Safety Mode leaves it adjustable. Not persisted. */
static uint32_t g_pause_us = PAUSE_US;
static int g_pause_open;
static uint32_t g_pause_ep;

static void pause_check(uint32_t now, uint32_t mode) {
    if (g_pause_open || mode == MODE_OOO || (uint32_t)(now - g_pause_ref) <= g_pause_us)
        return;
    g_pause_open = 1;
    g_store.cnt.pauses++;
    g_pause_ep = ep_log_egm(EP_PAUSE, 0);
    to(TELEMETRY);
    uart_puts("EPISODE: ventricular pause, no VP or VS for over ");
    uart_put_udec(g_pause_us / 1000u);
    uart_puts(" ms, t=");
    uart_put_fixed(now / 1000u, 1000, 3);
    uart_puts("\n");
}

static void pause_event(uint32_t t) {
    if (g_pause_open) {
        g_pause_open = 0;
        ep_close(g_pause_ep, t - g_pause_ref, 0, 0);
        to(TELEMETRY);
        uart_puts("EPISODE: ventricular pause ended after ");
        uart_put_udec((t - g_pause_ref) / 1000u);
        uart_puts(" ms\n");
    }
    g_pause_ref = t;
}

/* A ventricular event: VS, VP or VR; `seq` is what started its AV
 * interval. */
static void diag_v(uint32_t t, uint32_t mk, uint32_t seq) {
    struct counters *c = &g_store.cnt;
    egm_mark(mk);
    if (mk == MK_VR) {
        c->vr++;
        return;
    }
    noise_cycle(t, mk);
    pause_event(t);
    if (g_safety)
        agc_event(t);
    if (mk == MK_VP) {
        c->vp++;
        g_store.hdr.lifetime_vp++;
        g_tp.vp++;
        if (seq == SEQ_AS)
            c->as_vp++;
        else if (seq == SEQ_AP)
            c->ap_vp++;
    } else {
        c->vs++;
        if (seq == SEQ_AS)
            c->as_vs++;
        else if (seq == SEQ_AP)
            c->ap_vs++;
        /* A PVC: a sensed ventricular beat with no atrial event since the
         * last ventricular one. Only countable while the atrium is sensed. */
        if (!g_a_since_v && !ventricle_only(g_diag_mode))
            c->pvc++;
    }
    g_a_since_v = 0;
    g_tp.v_all++;
    if (g_have_v_ev && t != g_last_v_ev) {
        uint32_t iv = t - g_last_v_ev;
        uint32_t bpm = 60000000u / iv;
        g_store.hist[mk == MK_VP ? H_VP : H_VS][hist_bin(bpm)]++;
        g_tp.v_ms += iv / 1000u;
        g_tp.v_n++;
        rate_detect(&g_vhr, EP_VHR, mk == MK_VS && iv < 60000000u / VHR_BPM, bpm, t, iv);
    }
    g_last_v_ev = t;
    g_have_v_ev = 1;
}

/* A mode change restarts the timing cycle: its first interval in each
 * chamber is not a rate. */
static void diag_restart_intervals(void) {
    g_have_a_ev = g_have_v_ev = 0;
}

static uint32_t g_ep_loff[2], g_loff_start[2];
static uint32_t g_ep_magnet, g_magnet_start;

/* Lead-off on one lead: `bits` is its LOFF_STAT pair, `on` whether it is off
 * now. */
static void diag_lead(uint32_t lead, uint32_t bits, int on, uint32_t t) {
    if (on) {
        g_store.cnt.lead_off++;
        g_tp.flags |= 1u << 3;
        g_loff_start[lead] = t;
        g_ep_loff[lead] = ep_log_egm(EP_LEAD_OFF, bits);
    } else {
        ep_close(g_ep_loff[lead], t - g_loff_start[lead], 0, 0);
    }
}

static void diag_magnet(int on, uint32_t mode, uint32_t t) {
    if (on) {
        g_store.cnt.magnet++;
        g_tp.flags |= 1u << 2;
        g_magnet_start = t;
        g_ep_magnet = ep_log_egm(EP_MAGNET, mode);
    } else {
        ep_close(g_ep_magnet, t - g_magnet_start, 0, 0);
    }
}

static void diag_mode(uint32_t from, uint32_t to_mode, int eri) {
    if (from == to_mode)
        return;
    g_store.cnt.mode_changes++;
    ep_log(EP_MODE, 0, from, to_mode, eri ? 1u : 0u);
    diag_restart_intervals();
}

/* One activity period: the sensor-rate histogram and the trend's mean. */
static void diag_activity(uint32_t sir, uint32_t act_mg) {
    g_store.hist[H_SIR][hist_bin(sir)]++;
    g_tp.act_sum += act_mg;
    g_tp.act_n++;
}

static uint8_t clamp8(uint32_t v) {
    return (uint8_t)(v > 255u ? 255u : v);
}

/* Close the trend period: one point, then a checkpoint. */
static void diag_trend(void) {
    struct trend *tr = &g_store.tr[g_store.hdr.trends++ % TREND_SLOTS];
    tr->day = (uint16_t)(g_store.hdr.implant_s / 86400u);
    tr->batt_mv = (uint16_t)g_batt_mv;
    tr->act_mg = (uint16_t)(g_tp.act_n ? g_tp.act_sum / g_tp.act_n : 0u);
    tr->pct_ap = (uint8_t)(g_tp.a_all ? g_tp.ap * 100u / g_tp.a_all : 0u);
    tr->pct_vp = (uint8_t)(g_tp.v_all ? g_tp.vp * 100u / g_tp.v_all : 0u);
    tr->a_rate = clamp8(g_tp.a_n ? g_tp.a_n * 60000u / g_tp.a_ms : 0u);
    tr->v_rate = clamp8(g_tp.v_n ? g_tp.v_n * 60000u / g_tp.v_ms : 0u);
    tr->mode = (uint8_t)g_mode;
    tr->flags = (uint8_t)(g_batt_state | g_tp.flags | (g_ahr.open ? 1u << 4 : 0u) | (g_vhr.open ? 1u << 5 : 0u));
    store_dirty(tr, sizeof *tr);
    zero(&g_tp, sizeof g_tp);
    diag_checkpoint();
}

/* What a clinician's "clear diagnostics" resets after a follow-up: counters,
 * histograms, episodes and EGMs. Trends and lifetime totals stay; they are
 * the device's long-term record. Anything open now is forgotten. */
static void diag_clear(void) {
    zero(&g_store.cnt, sizeof g_store.cnt);
    zero(g_store.hist, sizeof g_store.hist);
    zero(g_store.ep, sizeof g_store.ep);
    for (uint32_t i = 0; i < EGM_SLOTS; i++) {
        g_store.egm[i].kind = 0;
        store_dirty(&g_store.egm[i], 16u);
    }
    g_store.hdr.episodes = 0;
    g_store.hdr.egms = 0;
    g_store.hdr.last_clear_s = g_store.hdr.implant_s;
    g_rec_slot = -1;
    g_ahr.open = g_vhr.open = 0;
    g_ahr.fast = g_vhr.fast = 0;
    store_dirty(g_store.ep, sizeof g_store.ep);
    diag_checkpoint();
}

/* Everything dirty to the EEPROM now, waiting out each write cycle: before
 * a commanded reset, which would lose whatever only RAM holds. */
static int store_any_dirty(void) {
    for (uint32_t w = 0; w < STORE_CHUNKS / 32u; w++)
        if (g_dirty[w])
            return 1;
    return 0;
}

static void diag_flush(void) {
    diag_checkpoint();
    while (g_eeprom_ok && store_any_dirty()) {
        delay_us(EEPROM_TWR_US);
        eeprom_service(time_us());
    }
}

/* --- Time scale caches and the reset log --- */

static uint32_t g_trend_period_us = 8000000u;
static uint32_t g_ckpt_us = 1000000u;
static uint32_t g_boot_cause = RC_COLD;
static void radio_wakeup_refresh(void);

/* Every implant-time period, in guest microseconds, at the current scale. A span
 * longer than u32 microseconds (71 minutes; the loop compares in u32) saturates
 * and so never comes due: only a scale below ~1000 can do that. */
static void scale_apply(void) {
    g_trend_period_us = implant_s_to_us(TREND_PERIOD_S);
    uint32_t c = implant_s_to_us(CKPT_PERIOD_S);
    g_ckpt_us = c < CKPT_FLOOR_US ? CKPT_FLOOR_US : c;
    radio_wakeup_refresh();
}

/* Straight to the EEPROM, a page at a time, waiting out each write cycle: for
 * what must survive a reset an instant from now (the Safety Mode latch). */
static void eeprom_write_sync(const void *p, uint32_t n) {
    uint32_t off = (uint32_t)((const uint8_t *)p - (const uint8_t *)&g_store);
    const uint8_t *src = p;
    while (n && g_eeprom_ok) {
        uint32_t room = 64u - (off % 64u); /* the part wraps within its 64-byte page (5.2) */
        uint32_t len = n < room ? n : room;
        if (!eeprom_write(off, src, len)) {
            g_eeprom_ok = 0;
            break;
        }
        delay_us(EEPROM_TWR_US);
        off += len;
        src += len;
        n -= len;
    }
    g_eeprom_ready_us = time_us();
}

static uint32_t boot_cause(void) {
    if (REG(WATCHDOG_REASON))
        return RC_COMMANDED;
    if (REG(POWMAN_CHIP_RESET) & POWMAN_HAD_RUN_LOW)
        return RC_SUPERVISOR;
    return RC_COLD;
}

/* The TPS3839's reset delay td (SBVS193D 7.6, 200 ms typical) is time the chip was
 * held in reset and the implant clock was not running: the board has no always-on
 * RTC, only this checkpointed counter, so it is added back. */
#define SUPERVISOR_HOLD_US 200000u

/* Uncommanded resets inside SAFETY_WINDOW_S of the newest, the newest included. */
static uint32_t resets_in_window(void) {
    struct safety_block *sf = &g_store.sf;
    uint32_t n = 0;
    uint32_t have = sf->total < RESET_SLOTS ? sf->total : RESET_SLOTS;
    for (uint32_t i = 0; i < have; i++) {
        const struct reset_rec *r = &sf->rl[(sf->total - 1u - i) % RESET_SLOTS];
        if (g_store.hdr.implant_s - r->implant_s > SAFETY_WINDOW_S)
            break;
        n++;
    }
    return n;
}

/* Load the store from the EEPROM, or format a new one; then classify this boot
 * and, before anything else is done with it, log an uncommanded reset and latch
 * Safety Mode if it is the third in 48 implant hours. */
static void diag_init(uint32_t now) {
    i2c_init();
    g_eeprom_ok = eeprom_read(0, (uint8_t *)&g_store, STORE_BYTES);
    int fresh = !g_eeprom_ok || g_store.hdr.magic != STORE_MAGIC || g_store.hdr.layout != STORE_LAYOUT;
    if (fresh) {
        zero(&g_store, STORE_BYTES);
        g_store.hdr.magic = STORE_MAGIC;
        g_store.hdr.layout = STORE_LAYOUT;
        store_dirty(&g_store, STORE_BYTES);
    }
    g_implant_last_us = now;
    struct safety_block *sf = &g_store.sf;
    sf->window_s = SAFETY_WINDOW_S;
    sf->resets_needed = SAFETY_RESETS;
    g_scale = sf->scale >= SCALE_MIN && sf->scale <= SCALE_MAX ? sf->scale : SCALE_DEFAULT;
    scale_apply();

    g_boot_cause = boot_cause();
    uint32_t in_window = 0;
    int newly_latched = 0;
    if (g_boot_cause == RC_SUPERVISOR) {
        g_store.hdr.implant_s += muldiv(SUPERVISOR_HOLD_US, g_scale, 1000000u);
        struct reset_rec *r = &sf->rl[sf->total++ % RESET_SLOTS];
        r->implant_s = g_store.hdr.implant_s;
        r->cause = (uint8_t)RC_SUPERVISOR;
        in_window = resets_in_window();
        if (sf->latched != SAFETY_LATCHED && in_window >= SAFETY_RESETS) {
            sf->latched = SAFETY_LATCHED;
            sf->latched_s = g_store.hdr.implant_s;
            newly_latched = 1;
        }
        /* Before anything else this boot does: a reset a moment later must find the
         * log and the latch already there. */
        store_dirty(sf, sizeof *sf);
        eeprom_write_sync(sf, sizeof *sf);
    }
    g_safety = sf->latched == SAFETY_LATCHED;

    g_store.hdr.power_ons++;
    ep_log(EP_POWER_ON, 0, fresh ? 1u : 0u, g_boot_cause, g_safety ? 1u : 0u);
    if (newly_latched)
        ep_log(EP_SAFETY, 0, in_window, 0, 0);
    diag_checkpoint();

    uart_puts("[diag] ");
    if (!g_eeprom_ok)
        uart_puts("AT24C256C not responding -- diagnostics kept in RAM only, lost on reset\n");
    else if (fresh)
        uart_puts("AT24C256C blank: new device, diagnostic store formatted\n");
    else {
        static const char *const CAUSES[] = {"power-on", "commanded reboot (watchdog)", "supervisor reset (RUN low)"};
        uart_puts("reset cause: ");
        uart_puts(CAUSES[g_boot_cause]);
        uart_puts("; time scale ");
        uart_put_udec(g_scale);
        uart_puts(" implant s per guest s; ");
        uart_puts("AT24C256C: implant day ");
        uart_put_udec(g_store.hdr.implant_s / 86400u);
        uart_puts(", power-on ");
        uart_put_udec(g_store.hdr.power_ons);
        uart_puts(", ");
        uart_put_udec(g_store.hdr.episodes);
        uart_puts(" episodes since the last clear\n");
    }
}

/* --- Pacing --- */

static void pace(uint32_t pin) {
    /* The threshold margin test's last beat runs at 80% pulse width. */
    uint32_t pw = g_magnet && g_tmt_left == 1 ? g_pw_us * 4u / 5u : g_pw_us;
    pin_set(pin, 1);
    delay_us(pw);
    pin_set(pin, 0);
}

/* The mode the timing cycle runs in: the programmed one, or its
 * asynchronous counterpart while a magnet is over the device. */
static uint32_t eff_mode(void) {
    if (!g_magnet || g_mode == MODE_OOO)
        return g_mode;
    return dual_chamber(g_mode) ? MODE_DOO : MODE_VOO;
}

/* The sensor-indicated rate in ppm, within [lower rate, MSR]. */
static uint32_t sir_ppm(void) {
    uint32_t sir = (g_sir_mppm + 500u) / 1000u;
    if (sir < g_rate_ppm)
        return g_rate_ppm;
    return sir > g_msr_ppm ? g_msr_ppm : sir;
}

static uint32_t lri_us(void) {
    if (g_safety)
        return SAFETY_LRI_US; /* Table 3: 72.5 ppm, no magnet rate, no sensor rate */
    if (g_magnet)
        return 60000000u /
               (g_tmt_left ? TMT_RATE_PPM : g_batt_state == BATT_OK ? MAGNET_RATE_PPM : ERI_MAGNET_RATE_PPM);
    if (rate_responsive(g_mode))
        return 60000000u / sir_ppm();
    return 60000000u / g_rate_ppm;
}

static uint32_t uri_us(void) {
    return 60000000u / g_utr_ppm;
}

/* VA interval: lower-rate interval less the paced AV delay, so an AP-VP
 * cycle runs at exactly the lower rate. The PAV range (max 300 ms) is under
 * the shortest LRI (333 ms at 180 ppm), so this never underflows. */
static uint32_t va_us(void) {
    return lri_us() - g_pav_ms * 1000u;
}

/* --- Battery monitor --- */

/* The cell's loaded voltage, in mV: the mean of BATT_SAMPLES one-shot
 * conversions of ADC0, at 3300 mV full scale. */
/* The cell's rail on ADC0, an average of `n` back-to-back conversions (2 us each). */
static uint32_t adc_mv(uint32_t n) {
    uint32_t sum = 0;
    for (uint32_t i = 0; i < n; i++) {
        REG(ADC_CS) = ADC_CS_EN | ADC_CS_START_ONCE | (VBAT_ADC_CHANNEL << ADC_CS_AINSEL_SHIFT);
        while (!(REG(ADC_CS) & ADC_CS_READY)) {
        }
        sum += REG(ADC_RESULT) & 0xfffu;
    }
    return (sum * ADC_FULL_SCALE_MV / n + 2048u) / 4096u;
}

static uint32_t battery_read_mv(void) {
    return adc_mv(BATT_SAMPLES);
}

static void print_battery(void) {
    uart_puts("batt=");
    uart_put_fixed(g_batt_mv, 1000, 3);
    uart_puts(" V ");
    uart_puts(BATT_NAMES[g_batt_state]);
}

static void radio_disable(void);
static int radio_awake(void);
static void fw_explant_reached(int record_date);
/* When the daily battery test last returned: the rail is still recovering from its burst for a few ms (the
 * cell's R x the 47 uF), so a reading taken then is not the quiescent voltage ERI and EOL are defined for. */
static int g_bt_ran;
static uint32_t g_bt_end_us;
#define BT_SETTLE_US 50000u

/* ERI: shed load. A dual-chamber or rate-responsive mode becomes VVI, and
 * the lower rate drops to 65 ppm (the magnet rate follows in lri_us). OOO
 * and VOO keep their mode; VOO still gets the lower rate. */
static void enter_eri(void) {
    to(TELEMETRY);
    uart_puts("ALARM: ERI -- battery ");
    uart_put_fixed(g_batt_mv, 1000, 3);
    uart_puts(" V loaded (below 2.550 V): elective replacement indicated");
    ep_log(EP_ERI, 0, g_batt_mv, 0, 0);
    fw_explant_reached(1);
    if (g_mode != MODE_OOO && !g_safety) { /* Safety Mode's settings are fixed */
        /* Rate response goes too: it only ever paces faster. */
        uint32_t from = g_mode;
        if (dual_chamber(g_mode) || g_mode == MODE_VVIR)
            g_mode = MODE_VVI;
        diag_mode(from, g_mode, 1);
        g_rate_ppm = ERI_RATE_PPM;
        g_mode_changed = 1;
        uart_puts("; now ");
        uart_puts(MODE_NAMES[g_mode]);
        uart_puts(" 65 ppm, magnet rate 65 ppm");
    }
    uart_puts("\n");
    /* Near replacement the radio, the highest-power thing the device does, is off. */
    radio_disable();
}

/* One reading, and whatever state change BATT_CONFIRM of them in a row call
 * for. A cell that collapses straight past EOL still goes through ERI's
 * load shedding first. */
static void battery_check(void) {
    /* Not while the radio loads the cell with mA: ERI and EOL are quiescent-load voltages. */
    if (radio_awake() || (g_bt_ran && (uint32_t)(time_us() - g_bt_end_us) < BT_SETTLE_US))
        return;
    g_batt_mv = battery_read_mv();
    uint32_t reading = g_batt_mv < EOL_MV ? BATT_EOL : g_batt_mv < ERI_MV ? BATT_ERI : BATT_OK;
    if (reading <= g_batt_state) {
        g_batt_confirm = 0;
        return;
    }
    if (++g_batt_confirm < BATT_CONFIRM)
        return;
    g_batt_confirm = 0;
    if (g_batt_state == BATT_OK) {
        g_batt_state = BATT_ERI;
        enter_eri();
    }
    if (reading == BATT_EOL) {
        g_batt_state = BATT_EOL;
        ep_log(EP_EOL, 0, g_batt_mv, 0, 0);
        to(TELEMETRY);
        uart_puts("ALARM: EOL -- battery ");
        uart_put_fixed(g_batt_mv, 1000, 3);
        uart_puts(" V loaded (below 2.300 V): end of service, replace the device now\n");
    }
}

/* --- Rate response --- */

/* An atrial event for the diagnostic: AS or AR (`intrinsic`), or AP. */
static void atrial_event(uint32_t t, int intrinsic) {
    if (g_have_last_a) {
        uint32_t ms = (t - g_last_a_us) / 1000u;
        g_p_all_ms += ms;
        g_p_all_n++;
        if (intrinsic && g_last_a_intrinsic) {
            g_p_intr_ms += ms;
            g_p_intr_n++;
        }
    }
    g_last_a_us = t;
    g_have_last_a = 1;
    g_last_a_intrinsic = intrinsic;
}

/* One activity period of `elapsed_ms`: the activity level, the SIR's step
 * toward its target, and the diagnostic. `mode` is the mode in force. */
static void rate_response_update(uint32_t elapsed_ms, uint32_t mode) {
    g_act_mg = g_act_n ? g_act_sum / (16u * g_act_n) : 0u;
    g_act_sum = g_act_n = 0;

    uint32_t lower = g_rate_ppm * 1000u, msr = g_msr_ppm * 1000u;
    uint32_t target = lower;
    if (g_act_mg > g_athresh_mg)
        target += (g_act_mg - g_athresh_mg) * g_rrslope * 10u;
    if (target > msr)
        target = msr;
    if (g_sir_mppm < lower)
        g_sir_mppm = lower;
    if (g_sir_mppm > msr)
        g_sir_mppm = msr;
    /* The fraction of the remaining distance covered this period, in
     * thousandths: 90% in rrup/rrdown seconds is a time constant of that
     * over ln 10 = 2.303. */
    uint32_t span_s = target > g_sir_mppm ? g_rrup_s : g_rrdown_s;
    uint32_t k = elapsed_ms * 2303u / (span_s * 1000u);
    if (k > 1000u)
        k = 1000u;
    uint32_t diff = target > g_sir_mppm ? target - g_sir_mppm : g_sir_mppm - target;
    uint32_t step = diff * k / 1000u;
    if (step == 0u)
        step = diff;
    g_sir_mppm = target > g_sir_mppm ? g_sir_mppm + step : g_sir_mppm - step;
    diag_activity(sir_ppm(), g_act_mg);

    uint32_t all_ms = g_p_all_ms, all_n = g_p_all_n, intr_ms = g_p_intr_ms, intr_n = g_p_intr_n;
    g_p_all_ms = g_p_all_n = g_p_intr_ms = g_p_intr_n = 0;
    /* The diagnostic needs the atrium sensed and not overdriven by a magnet
     * (which makes the mode DOO). */
    if (!(atrial_tracking(mode) || mode == MODE_OOO)) {
        g_ex_ms = g_ex_intr_ms = g_ex_intr_n = g_ex_sir_sum = g_ex_windows = 0;
        g_have_last_a = 0;
        return;
    }
    uint32_t sir = sir_ppm();
    if (g_act_mg <= g_athresh_mg && sir <= g_rate_ppm + 1u) {
        /* At rest: every atrial interval, paced or not, is the resting
         * rhythm. */
        g_rest_ms += all_ms;
        g_rest_n += all_n;
        if (g_rest_ms > REST_SPAN_MS) {
            g_rest_ms /= 2u;
            g_rest_n /= 2u;
        }
    }
    if (sir < g_rate_ppm + DIAG_MIN_RISE_PPM || !g_rest_n) {
        g_ex_ms = g_ex_intr_ms = g_ex_intr_n = g_ex_sir_sum = g_ex_windows = 0;
        return;
    }
    g_ex_ms += elapsed_ms;
    g_ex_intr_ms += intr_ms;
    g_ex_intr_n += intr_n;
    g_ex_sir_sum += sir;
    g_ex_windows++;
    if (g_ex_ms < DIAG_WINDOW_MS)
        return;

    /* No two intrinsic beats in a row: the atrium was paced throughout, so
     * the sinus node never once outran the pacemaker. */
    uint32_t intrinsic_x10 = g_ex_intr_n ? g_ex_intr_n * 600000u / g_ex_intr_ms : 0u;
    uint32_t rest_x10 = g_rest_n * 600000u / g_rest_ms;
    uint32_t mean_sir = g_ex_sir_sum / g_ex_windows;
    uint32_t expected = mean_sir - g_rate_ppm;
    uint32_t rise_x10 = intrinsic_x10 > rest_x10 ? intrinsic_x10 - rest_x10 : 0u;
    uint32_t index_pct = rise_x10 * 10u / expected;
    to(TELEMETRY);
    uart_puts(index_pct < CI_INDEX_PCT ? "DIAG: chronotropic incompetence suspected -- "
                                       : "DIAG: chronotropic response normal -- ");
    uart_put_udec(g_ex_ms / 1000u);
    uart_puts(" s of exertion at sensor rate ");
    uart_put_udec(mean_sir);
    uart_puts(" ppm (+");
    uart_put_udec(expected);
    uart_puts("); intrinsic atrial rate ");
    uart_put_fixed(intrinsic_x10, 10, 1);
    uart_puts(" bpm vs ");
    uart_put_fixed(rest_x10, 10, 1);
    uart_puts(" at rest: chronotropic index ");
    uart_put_udec(index_pct);
    uart_puts(index_pct < CI_INDEX_PCT ? "% (below 80%)\n" : "%\n");
    ep_log(EP_CI, 0, index_pct, intrinsic_x10, rest_x10);
    g_ex_ms = g_ex_intr_ms = g_ex_intr_n = g_ex_sir_sum = g_ex_windows = 0;
}

static void print_activity(void) {
    uart_puts("act=");
    uart_put_udec(g_act_mg);
    uart_puts(" mg sir=");
    uart_put_udec(sir_ppm());
    uart_puts(" ppm");
}

/* --- Telemetry session ---
 *
 * A clinic programmer talks to the device through its wand, held over the
 * implant: the wand's reader writes requests into the RF430CL330H's request
 * file and reads answers out of its answer file (see the tag, above, and the
 * link, below). The bytes either way are a stream of COBS-encoded frames,
 * each ended by a 0x00 byte, and a frame is
 *
 *     type, seq, payload..., CRC-16 (CCITT-FALSE, low byte first)
 *
 * with the CRC over type, seq and payload. Each request gets one response with
 * the same seq and the type's top bit set, or an error (RSP_ERROR: request
 * type, code). The requests:
 *
 *   0x01 OPEN      u32 Unix time now (0: leave the clock) -> device info,
 *                                                            and the session
 *   0x02 COUNTERS                                         -> struct counters
 *   0x03 HISTOGRAMS                                       -> the 5 x 16 bins
 *   0x04 EPISODES  u8 first slot, u8 count               -> first, count, records
 *   0x05 TRENDS    u16 first slot, u16 count             -> first, count, points
 *   0x06 EGM       u8 slot                                -> slot, struct egm
 *   0x07 CLEAR                                            -> u32 implant clock
 *   0x08 STREAM    u8 on (1) / off (0)                    -> on, u16 sps, u16 uV/LSB,
 *                                                            u32 sample clock
 *   0x7f END                                              -> (empty), then text
 *
 * While STREAM is on, the device also sends unsolicited 0x48 frames, the
 * live electrogram that a programmer draws during a follow-up: seq counts
 * them (mod 256), and the payload is
 *
 *     u32 first sample, u8 n samples, u8 n markers, u8 mode in effect,
 *     u8 flags (bit 0 magnet, bits 1-2 battery OK/ERI/EOL),
 *     n x (s16 V, s16 A), n markers x (u32 sample, u8 code)
 *
 * in the stored EGMs' units (250 SPS, EGM_UV_PER_LSB). Sample numbers count
 * from boot, so consecutive frames continue each other; a jump means the
 * stream fell more than the 2 s pre-trigger ring behind (a long response
 * held the UART) and skipped ahead. A marker's sample is the first one after
 * its event. Sending STREAM on again just answers, which keeps a session
 * that only streams from timing out.
 *
 * Until OPEN, every other request is refused (ERR_NO_SESSION). OPEN also
 * suspends the magnet response until the wand is lifted: the wand carries
 * the magnet that tells the device it is there, and without this the whole
 * follow-up would run in DOO at the magnet rate -- and the moments between
 * END and lifting the wand would log a magnet episode of their own. Real
 * devices hold off magnet mode during programmer telemetry for the same
 * reason.
 *
 * Requests are answered one at a time, in order, as many into one answer
 * file as fit. A response is streamed straight out of the store:
 * COBS-encoded a few hundred bytes per pass at most, the CRC computed as it
 * goes, into the answer file's image in RAM, which then goes to the tag in
 * one DMA transaction (see the link). So a 6 KB EGM costs the pacing loop no
 * more per pass than a short answer does, and the store needs no second
 * copy. The bytes are the store as it is while they are encoded -- a counter
 * can tick mid-response, as it can between two requests.
 *
 * The session ends once END's answer has been read, when the wand is taken
 * away (the Hall switch releases), or after SESSION_TIMEOUT_US of silence,
 * as a programmer losing the patient's device does.
 *
 * The bytes need not come through the wand. The session layer here (the
 * receive ring, session_service, the response staging) is independent of
 * how they travel: the tag's link feeds and drains it for the wand, the
 * radio's link for a communicator (see the radio link, below). One session
 * is open at a time, over whichever carried its OPEN; a radio session needs
 * no magnet and suspends nothing. */
#define REQ_OPEN 0x01u
#define REQ_COUNTERS 0x02u
#define REQ_HISTOGRAMS 0x03u
#define REQ_EPISODES 0x04u
#define REQ_TRENDS 0x05u
#define REQ_EGM 0x06u
#define REQ_CLEAR 0x07u
#define REQ_STREAM 0x08u
#define REQ_SAFETY 0x09u /* protocol 4: the Safety Mode latch, the time scale, the reset ring */
#define REQ_FWINFO 0x0au  /* protocol 5: the firmware level, the alerts and the battery test history */
#define REQ_UPGRADE 0x0bu /* protocol 5: move the firmware level forward (arg: the level) */
#define REQ_END 0x7fu
#define MSG_STREAM 0x48u
#define RSP_ERROR 0xeeu
#define ERR_CRC 1u
#define ERR_UNKNOWN 2u
#define ERR_ARGS 3u
#define ERR_NO_SESSION 4u
#define PROTOCOL_VERSION 5u /* 2: STREAM; 3: through the wand, OPEN opens the session; 4: SAFETY; 5: FWINFO, UPGRADE */
#define FW_MAJOR 1u
#define FW_MINOR 4u
#define SESSION_TIMEOUT_US 30000000u
/* No ADS1292R frame for this long (2 ms is normal): service the session and
 * the EEPROM from the main loop instead of the post-frame slot. */
#define QUIET_FALLBACK_US 10000u
#define RX_RING 512u /* a power of two, with room for a whole request file */
#define REQ_MAX 32u  /* a decoded request */

static int g_session;
/* Where the session's bytes travel. The tag (NFC, under the wand) and the radio each
 * feed the same receive ring and take responses from the same staging, one session at
 * a time: `g_rx_src` is who last fed the ring, `g_xport` the open session's
 * transport, `g_dst` where the response being staged goes. */
enum { XP_NONE, XP_NFC, XP_RADIO };
static int g_xport = XP_NONE;
static int g_rx_src = XP_NFC;
static int g_dst = XP_NFC;
static uint32_t g_session_last_us;
static int g_session_end; /* END answered: the session closes once it is read */
static int g_hall;        /* the DRV5032's last reading: a magnet or the wand */
static int g_wand;        /* a session was opened under this magnet: it is the wand */

static uint8_t g_rx_ring[RX_RING];
static uint32_t g_rx_head, g_rx_tail;
static uint32_t g_rx_frames; /* complete frames waiting in the ring */
static uint16_t g_crc_table[256];

/* The response going out: a header of its own (type, seq and any small
 * fields) and then one payload span, usually straight from the store. */
static struct {
    int active;
    uint8_t head[8];
    uint32_t head_len;
    const uint8_t *body;
    uint32_t body_len;
    uint32_t pos; /* raw bytes taken so far, CRC included */
    uint32_t crc;
    int final_sent; /* the last COBS block is staged; the delimiter is next */
    int done;       /* the delimiter is staged */
    uint8_t blk[255];
    uint32_t blk_len, blk_pos;
} g_out;
static uint8_t g_info[96];

static void crc_init(void) {
    for (uint32_t i = 0; i < 256u; i++) {
        uint32_t crc = i << 8;
        for (int b = 0; b < 8; b++)
            crc = crc & 0x8000u ? (crc << 1) ^ 0x1021u : crc << 1;
        g_crc_table[i] = (uint16_t)crc;
    }
}

static uint32_t crc16_step(uint32_t crc, uint32_t b) {
    return ((crc << 8) ^ g_crc_table[((crc >> 8) ^ b) & 0xffu]) & 0xffffu;
}

/* The next raw byte of the response: header, body, then the CRC. */
static int out_next(uint8_t *b) {
    uint32_t n = g_out.head_len + g_out.body_len;
    uint32_t p = g_out.pos;
    if (p < g_out.head_len)
        *b = g_out.head[p];
    else if (p < n)
        *b = g_out.body[p - g_out.head_len];
    else if (p == n)
        *b = (uint8_t)g_out.crc; /* the CRC covers everything before it */
    else if (p == n + 1u)
        *b = (uint8_t)(g_out.crc >> 8);
    else
        return 0;
    if (p < n)
        g_out.crc = crc16_step(g_out.crc, *b);
    g_out.pos++;
    return 1;
}

/* Stage the next COBS block: up to 254 non-zero bytes behind a code byte one
 * more than their count, which stands in for the zero that ended them (0xff:
 * a full block, no zero). Then the 0x00 delimiter. */
static int out_stage(void) {
    if (!g_out.active || g_out.done)
        return 0;
    g_out.blk_pos = 0;
    if (g_out.final_sent) {
        g_out.blk[0] = 0;
        g_out.blk_len = 1;
        g_out.done = 1;
        return 1;
    }
    uint32_t n = 0;
    uint8_t b;
    for (;;) {
        if (!out_next(&b)) {
            g_out.final_sent = 1;
            break;
        }
        if (b == 0)
            break;
        g_out.blk[1 + n++] = b;
        if (n == 254u)
            break;
    }
    g_out.blk[0] = (uint8_t)(n + 1u);
    g_out.blk_len = n + 1u;
    return 1;
}

/* --- The link: the session over the RF430CL330H ---
 *
 * One exchange per enabling of the tag's RF interface, as §5.10 has it. While
 * RF is on (LINK_LISTEN) the wand may read the answer file and write the
 * request file, and INTO falls once it has. Then firmware turns RF off (the
 * reader is done, or idle when only an answer is waiting: Figure 5-11),
 * clears the flags, reads the request file into the receive ring in one DMA
 * transaction (LINK_INTAKE), COBS-encodes as much of the answer as fits into
 * the file's image in RAM (LINK_STAGE), writes that image -- the length field
 * and the whole file, one more DMA transaction (LINK_FILL) -- and turns RF
 * back on. Each state is one step per pass of the post-frame slot: a DMA
 * transaction is started in one pass and found finished in a later one, so
 * the CPU never waits on the tag's SPI, which runs at 100 kHz at most
 * (§4.11): a full answer file is ~0.2 s of bus time on hardware. Staging
 * encodes a bounded share of the answer per pass (see up_stage), and answers
 * every waiting request that fits before the file goes back. */
enum { LINK_LISTEN, LINK_INTAKE, LINK_STAGE, LINK_FILL };
static int g_link;
static uint32_t g_up_len;   /* bytes in the answer file (or its image, while staging), unread */
static uint32_t g_down_len; /* the request file, being taken in */
/* The answer file's image, as the write transaction sends it: the command,
 * the length field's address, the length (big-endian), then the data. */
#define UP_HEAD 5u
static uint8_t g_up_buf[UP_HEAD + UP_DATA_MAX];
/* The request file's read: command, address and dummy, then zeros clocking
 * the data in; the data lands at offset 4 of what comes back. */
#define DOWN_HEAD 4u
static uint8_t g_down_tx[DOWN_HEAD + DOWN_DATA_MAX];
static uint8_t g_down_rx[DOWN_HEAD + DOWN_DATA_MAX];

/* Move the response into the answer file's image. Encoding costs ~30
 * cycles a byte (the CRC and COBS), so a pass starts no new COBS block once
 * STAGE_RAW bytes of the response have been encoded in it: at most
 * STAGE_RAW + 253 bytes, under 1 ms at 12 MHz, however the zeros fall (a
 * run of them is a run of one-byte blocks). */
#define STAGE_RAW 128u
static void up_stage(void) {
    uint32_t pos0 = g_out.pos;
    while (g_up_len < UP_DATA_MAX && g_out.active && g_dst == XP_NFC) {
        if (g_out.blk_pos == g_out.blk_len) {
            if (g_out.pos - pos0 >= STAGE_RAW)
                break; /* the next block waits for the next pass */
            if (!out_stage()) {
                g_out.active = 0;
                break;
            }
        }
        g_up_buf[UP_HEAD + g_up_len++] = g_out.blk[g_out.blk_pos++];
    }
}

/* Both files empty and RF back on, whatever the link was doing: a session
 * that ends leaves nothing for the next wand to read. A transaction on the
 * bus is stopped; an exchange in flight is let finish first (Figure 5-11),
 * for at most 100 ms. */
static void link_reset(void) {
    if (!g_tag_ok)
        return;
    tag_xfer_abort();
    uint32_t t0 = time_us();
    while ((tag_rreg(TAG_STATUS) & TAG_STATUS_RF_BUSY) && (uint32_t)(time_us() - t0) < 100000u) {
    }
    tag_rf(0);
    tag_wreg(TAG_INT_FLAGS, 0xffffu);
    tag_wlen(UP_LEN_ADDR, 0);
    tag_wlen(DOWN_LEN_ADDR, 0);
    tag_rf(1);
    g_up_len = g_down_len = 0;
    g_link = LINK_LISTEN;
}

static void out_begin(uint32_t type, uint32_t seq, const void *body, uint32_t body_len) {
    g_out.active = 1;
    g_out.head[0] = (uint8_t)type;
    g_out.head[1] = (uint8_t)seq;
    g_out.head_len = 2;
    g_out.body = body;
    g_out.body_len = body_len;
    g_out.pos = 0;
    g_out.crc = 0xffffu;
    g_out.final_sent = 0;
    g_out.done = 0;
    g_out.blk_len = g_out.blk_pos = 0;
}

static void out_head(uint32_t v) {
    g_out.head[g_out.head_len++] = (uint8_t)v;
}

static void out_error(uint32_t seq, uint32_t type, uint32_t code) {
    out_begin(RSP_ERROR, seq, 0, 0);
    out_head(type);
    out_head(code);
}

static uint32_t g_info_len;

static void info8(uint32_t v) {
    g_info[g_info_len++] = (uint8_t)v;
}

static void info16(uint32_t v) {
    info8(v);
    info8(v >> 8);
}

static void info32(uint32_t v) {
    info16(v);
    info16(v >> 16);
}

/* OPEN's answer: who the device is, how it is programmed, and how much it
 * holds. The programmer's INFO_FIELDS unpacks it in this order. */
static void build_info(void) {
    struct store_header *h = &g_store.hdr;
    g_info_len = 0;
    info8(PROTOCOL_VERSION);
    info8(FW_MAJOR);
    info8(FW_MINOR);
    info8(g_eeprom_ok);
    info32(h->implant_s);
    info32(h->epoch_at_implant);
    info32(h->last_clear_s);
    info32(h->last_session_s);
    info32(h->power_ons);
    info32(h->lifetime_ap);
    info32(h->lifetime_vp);
    info32(h->episodes);
    info32(h->trends);
    info32(h->egms);
    info16(g_batt_mv);
    info8(g_batt_state);
    info8(g_mode);
    info8(g_magnet && !g_wand); /* the session suspends it from the next pass */
    info8(g_magnet_response);
    info8(g_rate_ppm);
    info8(g_utr_ppm);
    info8(g_msr_ppm);
    info16(g_pav_ms);
    info16(g_sav_ms);
    info16(g_pvarp_ms);
    info16(g_pw_us);
    info16(g_amp_mv);
    info16(g_aamp_mv);
    info16(g_sens_uv);
    info16(g_asens_uv);
    info8(EP_SLOTS);
    info8(EGM_SLOTS);
    info16(TREND_SLOTS);
    info16(EGM_SAMPLES);
    info16(EGM_PRE);
    info16(EGM_SPS);
    info16(EGM_UV_PER_LSB);
    info32(TREND_PERIOD_S);
    info16(g_act_mg);
    info16(sir_ppm());
    info32(g_scale);
}

/* The next whole frame from the receive ring, COBS-decoded into `req`;
 * returns its length, or 0 for a malformed one. */
static uint32_t rx_take(uint8_t *req) {
    uint8_t raw[REQ_MAX + 8u];
    uint32_t len = 0;
    int overlong = 0;
    for (;;) {
        uint8_t c = g_rx_ring[g_rx_tail++ % RX_RING];
        if (!c)
            break;
        if (len < sizeof raw)
            raw[len++] = c;
        else
            overlong = 1;
    }
    g_rx_frames--;
    if (overlong)
        return 0;
    uint32_t in = 0, out = 0;
    while (in < len) {
        uint32_t code = raw[in++];
        if (in + code - 1u > len)
            return 0;
        for (uint32_t i = 1; i < code; i++) {
            if (out == REQ_MAX)
                return 0;
            req[out++] = raw[in++];
        }
        if (code < 0xffu && in < len) {
            if (out == REQ_MAX)
                return 0;
            req[out++] = 0;
        }
    }
    return out;
}

/* The live stream (STREAM): cursors into the pre-trigger sample ring and
 * the marker ring, both of which keep filling whether or not anyone reads
 * them. */
#define STREAM_MIN 25u /* samples pending before a frame goes: 100 ms */
#define STREAM_MAX 50u /* samples in one frame at most */
static int g_stream;
static uint32_t g_stream_next; /* g_egm_clock of the next sample to send */
static uint32_t g_stream_mk;   /* g_mk_n of the next marker to send */
static uint32_t g_stream_seq;
static uint8_t g_stream_buf[8u + STREAM_MAX * 4u + MK_RING * 5u];
static uint32_t g_stream_len;

static int stream_due(void) {
    return g_stream && g_egm_clock - g_stream_next >= STREAM_MIN;
}

static void stream_put(uint32_t v, uint32_t bytes) {
    for (uint32_t i = 0; i < bytes; i++)
        g_stream_buf[g_stream_len++] = (uint8_t)(v >> (8u * i));
}

/* One 0x48 frame: the samples from the cursor and the markers stamped
 * among them, copied out of the rings so they hold still while the frame
 * goes out. */
static void stream_send(void) {
    if (g_egm_clock - g_stream_next > EGM_PRE)
        g_stream_next = g_egm_clock - EGM_PRE; /* the ring has moved on: skip */
    if (g_mk_n - g_stream_mk > MK_RING)
        g_stream_mk = g_mk_n - MK_RING;
    uint32_t n = g_egm_clock - g_stream_next;
    if (n > STREAM_MAX)
        n = STREAM_MAX;
    uint32_t end = g_stream_next + n;
    g_stream_len = 0;
    stream_put(g_stream_next, 4);
    stream_put(n, 1);
    stream_put(0, 1); /* the marker count, filled in below */
    stream_put(eff_mode(), 1); /* what the timing cycle runs: DOO/VOO under a magnet */
    stream_put((g_magnet ? 1u : 0u) | (uint32_t)g_batt_state << 1, 1);
    for (uint32_t i = g_stream_next; i != end; i++) {
        stream_put((uint16_t)g_pre[i % EGM_PRE][0], 2);
        stream_put((uint16_t)g_pre[i % EGM_PRE][1], 2);
    }
    uint32_t n_mk = 0;
    while (g_stream_mk != g_mk_n && (int32_t)(g_mk_stamp[g_stream_mk % MK_RING] - end) < 0) {
        stream_put(g_mk_stamp[g_stream_mk % MK_RING], 4);
        stream_put(g_mk_code[g_stream_mk % MK_RING], 1);
        g_stream_mk++;
        n_mk++;
    }
    g_stream_buf[5] = (uint8_t)n_mk;
    g_stream_next = end;
    out_begin(MSG_STREAM, g_stream_seq++, g_stream_buf, g_stream_len);
}

static void radio_session_end(void);

/* The session is over: whatever was in flight is dropped, and the magnet
 * response, if a magnet is still there, comes back on the next pass. */
static void session_close(const char *why) {
    int radio = g_xport == XP_RADIO;
    g_session = 0;
    g_xport = XP_NONE;
    g_session_end = 0;
    g_stream = 0;
    g_out.active = 0;
    g_rx_head = g_rx_tail = g_rx_frames = 0;
    if (!g_hall)
        g_wand = 0; /* no magnet left to hold off */
    if (radio)
        radio_session_end(); /* the core back to sleep */
    else
        link_reset();
    to(TELEMETRY);
    uart_puts(radio ? "RADIO session closed: " : "WAND session closed: ");
    uart_puts(why);
    if (radio)
        radio_stamp();
    else
        uart_puts("\n");
}

/* Answer the next waiting request, if the last response has gone out;
 * otherwise send the live stream's next frame, if one is due. */
static int radio_drained(void);
static int fw_upgrade(uint32_t level);

static void session_service(void) {
    if (g_out.active)
        return;
    /* The response goes back the way the request came; once a session is open, the way it opened. */
    g_dst = g_session ? g_xport : g_rx_src;
    if (g_session_end) {
        /* END's answer has been read (the tag) or has gone out over the air (the radio). */
        if (g_xport == XP_RADIO ? radio_drained() : !g_up_len)
            session_close("END");
        return;
    }
    if (!g_rx_frames) {
        if (stream_due())
            stream_send();
        return;
    }
    uint8_t req[REQ_MAX];
    uint32_t len = rx_take(req);
    if (len < 4u)
        return; /* malformed: no answer, as for line noise */
    uint32_t type = req[0], seq = req[1];
    uint32_t n = len - 4u; /* payload bytes */
    const uint8_t *arg = &req[2];
    uint32_t crc = 0xffffu;
    for (uint32_t i = 0; i < len - 2u; i++)
        crc = crc16_step(crc, req[i]);
    if (crc != (uint32_t)(req[len - 2u] | req[len - 1u] << 8)) {
        out_error(seq, type, ERR_CRC);
        return;
    }
    if (!g_session && type != REQ_OPEN) {
        out_error(seq, type, ERR_NO_SESSION);
        return;
    }
    g_session_last_us = time_us();
    switch (type) {
    case REQ_OPEN: {
        if (!g_session) {
            g_session = 1;
            g_xport = g_rx_src;
            to(TELEMETRY);
            if (g_xport == XP_NFC) {
                g_wand = 1;
                uart_puts("WAND session open: magnet response suspended\n");
            } else {
                /* Over the air there is no wand, and no magnet to hold off. */
                uart_puts("RADIO session open: wandless telemetry");
                radio_stamp();
            }
        }
        uint32_t unix_now = n >= 4u ? arg[0] | arg[1] << 8 | arg[2] << 16 | (uint32_t)arg[3] << 24 : 0u;
        build_info();
        g_store.hdr.last_session_s = g_store.hdr.implant_s;
        if (unix_now)
            g_store.hdr.epoch_at_implant = unix_now - g_store.hdr.implant_s;
        store_dirty(&g_store.hdr, sizeof g_store.hdr);
        out_begin(type | 0x80u, seq, g_info, g_info_len);
        break;
    }
    case REQ_COUNTERS:
        out_begin(type | 0x80u, seq, &g_store.cnt, sizeof g_store.cnt);
        break;
    case REQ_HISTOGRAMS:
        out_begin(type | 0x80u, seq, g_store.hist, sizeof g_store.hist);
        break;
    case REQ_EPISODES: {
        uint32_t first = n >= 2u ? arg[0] : EP_SLOTS, count = n >= 2u ? arg[1] : 0u;
        if (first + count > EP_SLOTS) {
            out_error(seq, type, ERR_ARGS);
            break;
        }
        out_begin(type | 0x80u, seq, &g_store.ep[first], count * sizeof(struct episode));
        out_head(first);
        out_head(count);
        break;
    }
    case REQ_TRENDS: {
        uint32_t first = n >= 4u ? arg[0] | arg[1] << 8 : TREND_SLOTS, count = n >= 4u ? arg[2] | arg[3] << 8 : 0u;
        if (first + count > TREND_SLOTS) {
            out_error(seq, type, ERR_ARGS);
            break;
        }
        out_begin(type | 0x80u, seq, &g_store.tr[first], count * sizeof(struct trend));
        out_head(first);
        out_head(first >> 8);
        out_head(count);
        out_head(count >> 8);
        break;
    }
    case REQ_EGM: {
        uint32_t slot = n >= 1u ? arg[0] : EGM_SLOTS;
        if (slot >= EGM_SLOTS) {
            out_error(seq, type, ERR_ARGS);
            break;
        }
        out_begin(type | 0x80u, seq, &g_store.egm[slot], sizeof(struct egm));
        out_head(slot);
        break;
    }
    case REQ_CLEAR:
        diag_clear();
        out_begin(type | 0x80u, seq, &g_store.hdr.implant_s, 4u);
        break;
    case REQ_STREAM:
        if (n >= 1u && arg[0] && !g_stream) {
            g_stream_next = g_egm_clock;
            g_stream_mk = g_mk_n;
        }
        g_stream = n >= 1u && arg[0];
        g_info_len = 0;
        info8(g_stream);
        info16(EGM_SPS);
        info16(EGM_UV_PER_LSB);
        info32(g_egm_clock);
        out_begin(type | 0x80u, seq, g_info, g_info_len);
        break;
    case REQ_SAFETY:
        out_begin(type | 0x80u, seq, &g_store.sf, sizeof g_store.sf);
        break;
    case REQ_FWINFO:
        out_begin(type | 0x80u, seq, &g_store.fw, sizeof g_store.fw);
        break;
    case REQ_UPGRADE:
        /* A modelling simplification: the real upgrade downloads firmware; here every level is in the image. */
        if (n < 1u || !fw_upgrade(arg[0])) {
            out_error(seq, type, ERR_ARGS);
            break;
        }
        out_begin(type | 0x80u, seq, &g_store.fw, sizeof g_store.fw);
        break;
    case REQ_END:
        g_session_end = 1;
        out_begin(type | 0x80u, seq, 0, 0);
        break;
    default:
        out_error(seq, type, ERR_UNKNOWN);
        break;
    }
}

/* One received byte into the ring; a full ring drops the byte, and the frame
 * it was part of fails its CRC or decode. */
static void session_byte(uint8_t c) {
    if (g_rx_head - g_rx_tail >= RX_RING - 1u)
        return;
    g_rx_ring[g_rx_head++ % RX_RING] = c;
    if (!c)
        g_rx_frames++;
}

/* Whether the link has anything to do this pass. */
static int link_due(void) {
    if (!g_tag_ok)
        return 0;
    if (g_link == LINK_LISTEN && g_session && g_xport == XP_RADIO)
        return 0; /* a radio session has the session layer: the wand waits */
    if (g_link != LINK_LISTEN || tag_into())
        return 1;
    if (g_out.active)
        return g_dst == XP_NFC && !g_up_len; /* an answer, and the file is free for it */
    return g_rx_frames || (g_session_end && !g_up_len) || stream_due();
}

/* One step of the link: start or finish a DMA transaction, or stage a
 * block of the answer. */
static void link_service(void) {
    session_service();
    switch (g_link) {
    case LINK_LISTEN: {
        if (g_session && g_xport == XP_RADIO)
            return;
        if (tag_into()) {
            if (RX_RING - 1u - (g_rx_head - g_rx_tail) < DOWN_DATA_MAX)
                return; /* no room for a whole request file yet */
        } else if (g_up_len || !g_out.active || g_dst != XP_NFC || (tag_rreg(TAG_STATUS) & TAG_STATUS_RF_BUSY)) {
            return;
        }
        tag_rf(0);
        uint32_t flags = tag_rreg(TAG_INT_FLAGS);
        tag_wreg(TAG_INT_FLAGS, flags);
        if (flags & TAG_INT_END_OF_READ)
            g_up_len = 0;
        g_down_len = flags & TAG_INT_END_OF_WRITE ? tag_rlen(DOWN_LEN_ADDR) : 0u;
        if (g_down_len > DOWN_DATA_MAX)
            g_down_len = DOWN_DATA_MAX;
        if (!g_down_len) {
            g_link = LINK_STAGE;
            return;
        }
        g_down_tx[0] = TAG_CMD_READ;
        g_down_tx[1] = (uint8_t)(DOWN_DATA_ADDR >> 8);
        g_down_tx[2] = (uint8_t)DOWN_DATA_ADDR;
        g_down_tx[3] = 0; /* the dummy byte (§5.6); the rest stays zero */
        tag_xfer_start(g_down_tx, g_down_rx, DOWN_HEAD + g_down_len);
        g_link = LINK_INTAKE;
        return;
    }
    case LINK_INTAKE:
        if (!tag_xfer_done())
            return;
        tag_xfer_end();
        for (uint32_t i = 0; i < g_down_len; i++)
            session_byte(g_down_rx[DOWN_HEAD + i]);
        g_rx_src = XP_NFC;
        tag_wlen(DOWN_LEN_ADDR, 0); /* taken: the wand may write the next */
        g_link = LINK_STAGE;
        return;
    case LINK_STAGE:
        up_stage();
        if (!g_out.active)
            session_service(); /* the next answer, if a request is waiting */
        if (g_out.active && g_dst == XP_NFC && g_up_len < UP_DATA_MAX)
            return; /* more of it next pass */
        g_up_buf[0] = TAG_CMD_WRITE;
        g_up_buf[1] = (uint8_t)(UP_LEN_ADDR >> 8);
        g_up_buf[2] = (uint8_t)UP_LEN_ADDR;
        g_up_buf[3] = (uint8_t)(g_up_len >> 8);
        g_up_buf[4] = (uint8_t)g_up_len;
        tag_xfer_start(g_up_buf, 0, UP_HEAD + g_up_len);
        g_link = LINK_FILL;
        return;
    default: /* LINK_FILL */
        if (!tag_xfer_done())
            return;
        tag_xfer_end();
        tag_rf(1);
        g_link = LINK_LISTEN;
        return;
    }
}

/* --- The radio link: the session over the ZL70102 ---
 *
 * The same session as the wand's (the COBS frames, OPEN ... END, STREAM: the layer
 * above, session_service) carried over the radio. The wire protocol is
 * unchanged; only how bytes reach the receive ring and leave the response differs.
 *
 * The chip sleeps sniffing for the 2.45 GHz wake-up (RS_SLEEP, ~0.6 uA with its
 * internal strobe oscillator, DS Figure 3-6), and a link starts one of two ways:
 *
 *   - base-initiated: a communicator or programmer sends the wake-up packet, the
 *     chip wakes itself ~11 ms later and starts answering with wake-up responses
 *     (DS Figure 3-1). Firmware sees IRQ rise on RADIO_READY.
 *   - implant-initiated: the firmware pulses WU_EN for over 1.5 ms (RS_PULSE, DS
 *     "Startup Method Using IMD Pin Control"), the core starts Idle, and writing
 *     the Idle flag to 0 makes it send the responses (RS_STARTING, RS_RESPOND).
 *     The automatic remote-monitoring wakeups work this way, on a programmable
 *     interval: a home monitor that is listening answers.
 *
 * Once the link is established (RS_LINKED) bytes move in whole 15-byte blocks
 * through the chip's 64-block TX and RX buffers, in the post-frame slot like the
 * wand's link and never waiting on the chip: a pass reads at most a few blocks
 * each way. A response is COBS-encoded into a byte ring (out_stage, as the
 * wand's image is) and cut into blocks, the last one zero-padded; a zero is a
 * COBS frame delimiter, so the padding reads as empty frames, which the receive
 * side drops. The session ends on END (once its answer has gone out), when the
 * radio link is lost (a communicator that stops, the chip's own main watchdog
 * warning 0.6 s before 5 s of silence, DS Table 4-1), or after RADIO_IDLE_US
 * with no request and no session. Then MAC_CTRL.SLEEP puts the core back to
 * sleep. The link draws ~4.8 mA while it is up (DS Table 7-14: RX 4.3 mA, TX
 * 5.3 mA, no power-save), the highest-power thing this device does.
 *
 * At ERI the radio is switched off at the load switch (GP25 low) and its automatic
 * wakeups with it: wandless telemetry is disabled near replacement, as real devices
 * do, and the wand (NFC) path stays. A healthy Li/CFx cell under radio load only
 * reaches ~2.31 V at ERI, under the supervisor's 2.325 V threshold. */
#define RADIO_POLL_US 3000u              /* while awake: every other post-frame slot */
#define RADIO_STARTING_TIMEOUT_US 100000u /* RADIO_READY is ~11 ms after the wake */
#define RADIO_RESPOND_IMPLANT_US 1000000u /* no communicator answered an automatic wakeup */
#define RADIO_RESPOND_BASE_US 3000000u    /* a base-initiated link that never forms */
#define RADIO_IDLE_US 3000000u            /* a link with no request: hang up before the 5 s watchdog */
#define RADIO_RX_MAX_BLOCKS 8u
#define RADIO_TX_MAX_BLOCKS 8u
#define RTX_RING 512u /* a power of two: staged response bytes */
/* Automatic remote-monitoring wakeups. The interval is in implant seconds, the unit
 * the implant clock counts, and converted to guest time with the runtime scale
 * (implant_s_to_us): daily here. A radio
 * session is real-time, so it only fits where an implant day is longer than a
 * session: at the current x1e6 clock a day is 86 ms of guest time and every
 * session would also be charged x1e6 to the cell. Below RADIO_WAKEUP_MIN_US the
 * wakeups are not scheduled (and tty1 says so once); the `radio wakeup <implant s>`
 * console command programs it, 0 for off. */
#define RADIO_WAKEUP_INTERVAL_S 86400u
#define RADIO_WAKEUP_MIN_US 1000000u
/* Retry policy (a model choice: the letter does not describe the monitor's
 * schedule). An implant-initiated wakeup that does not end cleanly -- the
 * implant reset under it, the core never came up, nobody answered, the link
 * dropped -- is tried again after RADIO_RETRY_S implant seconds rather than a
 * whole interval later. That is what makes successive resets in the field: each
 * attempt loads the cell with the radio's mA again. After an uncommanded reset
 * the radio is not initialised at boot at all (radio_boot_deferred): the chip
 * lost its supply with the reset, and powering and programming it (0.95 mA for
 * ~16 ms) at the moment the rail has just recovered is what drove the stage-3b
 * boot-time reset loop at deep defect. It comes back at the retry time, or
 * RADIO_SETTLE_US after boot in Safety Mode (the clinic must see the alert). */
#define RADIO_RETRY_S (4u * 3600u)
#define RADIO_SETTLE_US 2000000u

enum { RS_OFF, RS_SLEEP, RS_PULSE, RS_STARTING, RS_RESPOND, RS_LINKED };
static int g_rs = RS_OFF;
static int g_radio_on;          /* wandless telemetry enabled (cleared for good at ERI) */
static int g_radio_base;        /* this link was started by the communicator's wake-up */
static int g_radio_reprogram;   /* the chip lost its wake-up block: program it at the next wake */
static int g_radio_program;     /* this wake is only to program it */
static int g_radio_sleep_pending; /* MAC_CTRL.SLEEP is owed (the bus was busy) */
static uint32_t g_rs_t;         /* entry time of the current state */
static uint32_t g_radio_poll_us;
static uint32_t g_radio_act_us; /* the last request bytes */
static uint32_t g_radio_next_us;
static uint32_t g_radio_wakeup_s = RADIO_WAKEUP_INTERVAL_S;
static int g_radio_sched_note;  /* the "not scheduled" note has been printed */
static int g_radio_drained;     /* the response has reached the chip's TX buffer and gone out */
static uint8_t g_rtx[RTX_RING];
static uint32_t g_rtx_head, g_rtx_tail;

/* Cached: the conversion divides, and radio_due asks every pass. Refreshed when the
 * interval or the time scale changes (scale_apply, `radio wakeup`). */
static uint32_t g_radio_wakeup_us = RADIO_WAKEUP_INTERVAL_S;
static uint32_t g_radio_retry_us = RADIO_RETRY_S;

static void radio_wakeup_refresh(void) {
    g_radio_wakeup_us = implant_s_to_us(g_radio_wakeup_s);
    g_radio_retry_us = implant_s_to_us(RADIO_RETRY_S);
}

static uint32_t radio_wakeup_interval_us(void) {
    return g_radio_wakeup_us;
}

static int radio_wakeups_scheduled(void) {
    return g_radio_wakeup_s && radio_wakeup_interval_us() >= RADIO_WAKEUP_MIN_US;
}

/* An implant-initiated wakeup did not end cleanly: try again soon. */
static void radio_retry_soon(uint32_t now) {
    /* Not in Safety Mode: the daily schedule only, or a device that can no longer
     * complete a session would reset every few seconds for good. */
    if (!g_safety && g_radio_wakeup_s && g_radio_retry_us < g_radio_wakeup_us && radio_wakeups_scheduled())
        g_radio_next_us = now + g_radio_retry_us;
}

static void radio_arm_wakeup(uint32_t now) {
    g_radio_next_us = now + radio_wakeup_interval_us();
    if (g_radio_wakeup_s && !radio_wakeups_scheduled() && !g_radio_sched_note) {
        g_radio_sched_note = 1;
        uint32_t port = g_port;
        to(TELEMETRY);
        uart_puts("RADIO automatic wakeups not scheduled: ");
        uart_put_udec(g_radio_wakeup_s);
        uart_puts(" implant s is ");
        uart_put_udec(radio_wakeup_interval_us() / 1000u);
        uart_puts(" ms of guest time at this implant clock scale (a session cannot fit); `radio wakeup <implant s>` programs it\n");
        to(port);
    }
}

/* The chip is awake and loading the cell with mA: the battery's ERI and EOL
 * thresholds are set for the quiescent load, not for this. */
static int radio_awake(void) {
    return g_rs >= RS_PULSE;
}

static int radio_blocks_nfc(void) {
    return g_session && g_xport == XP_NFC;
}

static void radio_sleep_now(uint32_t now) {
    g_rtx_head = g_rtx_tail = 0;
    g_rs = RS_SLEEP;
    g_rs_t = now;
    /* The write waits for the bus if a tag transaction (an exchange begun before this
     * link opened) holds it: radio_service does it. */
    if (g_tag_xfer)
        g_radio_sleep_pending = 1;
    else
        radio_wr(RADIO_REG_MAC_CTRL, RADIO_MAC_SLEEP);
}

/* A link that carries no session is over. */
static void radio_link_close(const char *why) {
    if (g_session && g_xport == XP_RADIO) {
        session_close(why); /* says so, and ends the radio too */
        return;
    }
    to(TELEMETRY);
    uart_puts("RADIO link closed: ");
    uart_puts(why);
    radio_stamp();
    radio_sleep_now(time_us());
}

/* session_close's radio half: the core back to sleep. */
static void radio_session_end(void) {
    if (g_rs >= RS_STARTING)
        radio_sleep_now(time_us());
    g_rtx_head = g_rtx_tail = 0;
}

/* ERI: wandless telemetry off, for good (a reset after replacing the device
 * brings it back). The load switch drops the radio's supply outright. */
static void radio_disable(void) {
    if (!g_radio_ok || !g_radio_on)
        return;
    g_radio_on = 0;
    if (g_session && g_xport == XP_RADIO)
        session_close("radio telemetry disabled at ERI");
    pin_set(RADIO_PWR_PIN, 0);
    pin_set(RADIO_WU_EN_PIN, 0);
    g_rtx_head = g_rtx_tail = 0;
    g_rs = RS_OFF;
    to(TELEMETRY);
    uart_puts("ALARM: RADIO telemetry disabled at ERI (wandless sessions and automatic wakeups off, radio unpowered; the wand still works)\n");
    ep_log(EP_RADIO_OFF, 0, g_batt_mv, 0, 0);
}

static int bt_ready(void);
static void battery_test_run(void);

static int radio_due(void) {
    if (!g_radio_ok || g_rs == RS_OFF)
        return 0;
    uint32_t now = time_us();
    if (g_rs == RS_PULSE)
        return (uint32_t)(now - g_rs_t) >= RADIO_T_WU_PULSE_US;
    if (g_tag_xfer)
        return 0; /* the tag's DMA holds SPI1 */
    if (g_radio_sleep_pending)
        return 1;
    if (g_rs == RS_SLEEP)
        return radio_irq() || bt_ready() ||
               (g_radio_on && !g_session &&
                (g_radio_reprogram || (radio_wakeups_scheduled() && (int32_t)(now - g_radio_next_us) >= 0)));
    return (uint32_t)(now - g_radio_poll_us) >= RADIO_POLL_US;
}

/* One request byte from the air into the receive ring. A zero is a frame
 * delimiter; the zero padding of a block is not a frame, so a zero straight
 * after another (or after nothing) is dropped. */
static void radio_feed(uint8_t c) {
    if (!c && (g_rx_head == g_rx_tail || g_rx_ring[(g_rx_head - 1u) % RX_RING] == 0))
        return;
    session_byte(c);
    g_rx_src = XP_RADIO;
}

static void radio_rx(void) {
    uint32_t n = radio_rd(RADIO_REG_RXBUFF_USED); /* complete blocks */
    uint32_t room = (RX_RING - 1u - (g_rx_head - g_rx_tail)) / RADIO_BLOCK;
    if (n > room)
        n = room;
    if (n > RADIO_RX_MAX_BLOCKS)
        n = RADIO_RX_MAX_BLOCKS;
    if (!n)
        return;
    g_radio_act_us = time_us();
    radio_begin(RADIO_REG_TXRXBUFF, 1);
    for (uint32_t i = 0; i < n * RADIO_BLOCK; i++)
        radio_feed(spi_xfer(SPI1_BASE, 0));
    radio_end();
}

/* Encode the response into the ring (at most STAGE_RAW raw bytes a pass, as the
 * wand's staging does), then hand the chip as many whole blocks as its buffer
 * has room for; the last, partial block of a response is zero-padded. */
static void radio_tx(uint32_t used) {
    uint32_t pos0 = g_out.pos;
    while (g_out.active && g_dst == XP_RADIO && RTX_RING - (g_rtx_head - g_rtx_tail) > 260u) {
        if (g_out.blk_pos == g_out.blk_len) {
            if (g_out.pos - pos0 >= STAGE_RAW)
                break;
            if (!out_stage()) {
                g_out.active = 0;
                break;
            }
        }
        g_rtx[g_rtx_head++ % RTX_RING] = g_out.blk[g_out.blk_pos++];
    }
    uint32_t more = g_out.active && g_dst == XP_RADIO;
    uint32_t avail = g_rtx_head - g_rtx_tail;
    uint32_t free_blocks = 64u - used; /* DS "Media Access Controller": 64 blocks of buffer */
    uint32_t blocks = avail / RADIO_BLOCK;
    if (avail % RADIO_BLOCK && !more)
        blocks++; /* the tail of the response, padded */
    if (blocks > free_blocks)
        blocks = free_blocks;
    if (blocks > RADIO_TX_MAX_BLOCKS)
        blocks = RADIO_TX_MAX_BLOCKS;
    if (!blocks)
        return;
    radio_begin(RADIO_REG_TXRXBUFF, 0);
    for (uint32_t b = 0; b < blocks; b++) {
        for (uint32_t i = 0; i < RADIO_BLOCK; i++) {
            uint8_t c = 0;
            if (g_rtx_tail != g_rtx_head)
                c = g_rtx[g_rtx_tail++ % RTX_RING];
            spi_xfer(SPI1_BASE, c);
        }
    }
    radio_end();
}

static void radio_linked(uint32_t now) {
    uint32_t st = radio_rd(RADIO_REG_STATUS);
    if (!(st & RADIO_STATUS_LINK_UP)) {
        /* The chip is not linked, or is not there: it lost its supply or went to sleep. A
         * radio that browned out below its minimum has lost its wake-up block too. */
        g_radio_reprogram = 1;
        if (!g_radio_base)
            radio_retry_soon(now);
        radio_link_close("link dropped (the radio fell asleep or lost its supply)");
        return;
    }
    uint32_t irqs = radio_rd(RADIO_REG_IRQ_STATUS);
    if (irqs & RADIO_IRQ_WATCHDOG_WARNING) {
        if (!g_radio_base)
            radio_retry_soon(now);
        radio_link_close("the communicator stopped (main watchdog warning)");
        return;
    }
    uint32_t used = radio_rd(RADIO_REG_TXBUFF_USED);
    g_radio_drained = g_rtx_head == g_rtx_tail && !used;
    session_service(); /* answers the next request; END closes the session once drained */
    if (g_rs != RS_LINKED)
        return;
    radio_rx();
    radio_tx(used);
    /* Signed: radio_rx stamped g_radio_act_us a few microseconds after `now` was taken. */
    if (!g_session && (int32_t)(now - g_radio_act_us) >= (int32_t)RADIO_IDLE_US)
        radio_link_close("idle: no request from the communicator");
}

static void radio_service(void) {
    uint32_t now = time_us();
    if (g_radio_sleep_pending) {
        g_radio_sleep_pending = 0;
        radio_wr(RADIO_REG_MAC_CTRL, RADIO_MAC_SLEEP);
        return;
    }
    switch (g_rs) {
    case RS_SLEEP: {
        if (radio_irq()) {
            /* The chip woke itself: a communicator's wake-up. Wait for its RADIO_READY. */
            if (radio_blocks_nfc()) {
                radio_sleep_now(now);
                return;
            }
            g_rs = RS_STARTING;
            g_rs_t = now;
            g_radio_base = 1;
            g_radio_program = 0;
            g_radio_poll_us = now;
            return;
        }
        if (bt_ready()) {
            battery_test_run();
            return;
        }
        g_radio_program = g_radio_reprogram;
        g_radio_base = 0;
        radio_arm_wakeup(now);
        if (!g_radio_program) {
            to(TELEMETRY);
            uart_puts("RADIO wakeup (automatic remote monitoring): WU_EN pulse, then responses");
            radio_stamp();
        }
        pin_set(RADIO_WU_EN_PIN, 1);
        g_rs = RS_PULSE;
        g_rs_t = now;
        return;
    }
    case RS_PULSE:
        pin_set(RADIO_WU_EN_PIN, 0);
        g_rs = RS_STARTING;
        g_rs_t = now;
        g_radio_poll_us = now;
        return;
    case RS_STARTING: {
        g_radio_poll_us = now;
        if (!radio_irq()) {
            if ((uint32_t)(now - g_rs_t) >= RADIO_STARTING_TIMEOUT_US) {
                g_radio_reprogram = 1;
                to(TELEMETRY);
                uart_puts("RADIO wakeup failed: the core never became ready");
                radio_stamp();
                if (!g_radio_base)
                    radio_retry_soon(now);
                radio_sleep_now(now);
            }
            return;
        }
        uint32_t irqs = radio_rd(RADIO_REG_IRQ_STATUS);
        radio_wr(RADIO_REG_IRQ_CLEAR, irqs);
        if (!(irqs & RADIO_IRQ_RADIO_READY))
            return;
        if (g_radio_program) {
            /* Only here to put the wake-up block back (the chip lost it). */
            radio_program();
            g_radio_reprogram = 0;
            g_radio_program = 0;
            radio_sleep_now(now);
            return;
        }
        uint32_t st = radio_rd(RADIO_REG_STATUS);
        if (st & RADIO_STATUS_WOKE_BY_245)
            g_radio_base = 1;
        if (!g_radio_base)
            radio_wr(RADIO_REG_MAC_IDLE, 0); /* Send_Response: start the wake-up responses */
        g_radio_act_us = now;
        g_rs = RS_RESPOND;
        g_rs_t = now;
        return;
    }
    case RS_RESPOND: {
        g_radio_poll_us = now;
        uint32_t irqs = radio_rd(RADIO_REG_IRQ_STATUS);
        if (irqs & RADIO_IRQ_LINK_ESTABLISHED) {
            radio_wr(RADIO_REG_IRQ_CLEAR, RADIO_IRQ_LINK_ESTABLISHED);
            if (tele_off()) {
                /* Wandless telemetry is disabled: the wakeup that reached a base is hung up on. In v2.04 the
                 * wakeup (the WU_EN pulse, the core's start-up, the wake-up responses) still happens: that is
                 * its shipped bug 1; the fixed level never gets here (it cuts the radio's supply). */
                to(TELEMETRY);
                uart_puts("RADIO link refused: wandless telemetry is disabled");
                radio_stamp();
                radio_sleep_now(now);
                return;
            }
            g_rs = RS_LINKED;
            g_rs_t = now;
            g_radio_act_us = now;
            g_radio_drained = 1;
            to(TELEMETRY);
            uart_puts(g_radio_base ? "RADIO link up (base-initiated wake-up)" : "RADIO link up (implant-initiated)");
            radio_stamp();
            return;
        }
        uint32_t limit = g_radio_base ? RADIO_RESPOND_BASE_US : RADIO_RESPOND_IMPLANT_US;
        if ((uint32_t)(now - g_rs_t) >= limit || radio_blocks_nfc()) {
            to(TELEMETRY);
            uart_puts(radio_blocks_nfc() ? "RADIO link refused: a wand session is open"
                                         : "RADIO wakeup: no communicator answered; back to sleep");
            radio_stamp();
            if (!g_radio_base)
                radio_retry_soon(now);
            radio_sleep_now(now);
        }
        return;
    }
    case RS_LINKED:
        g_radio_poll_us = now;
        radio_linked(now);
        return;
    default:
        return;
    }
}

/* The radio, at boot: the cold init, and its state. Skipped when the cell is already
 * at ERI (a reset leaves a depleted cell depleted): wandless telemetry stays off. */
static void radio_boot(uint32_t now) {
    g_radio_ok = radio_cold_init();
    if (!g_radio_ok) {
        g_radio_fault = 1;
        uart_puts("[fault] implant radio missing -- no wandless telemetry\n");
        return;
    }
    g_radio_on = 1;
    g_rs = RS_SLEEP;
    g_rs_t = now;
    radio_arm_wakeup(now);
    uart_puts("[radio] ZL70102 up: asleep, sniffing for the 2.45 GHz wake-up every ");
    uart_put_udec(RADIO_SNIFF_100MS / 10u);
    uart_puts(" s; automatic wakeups ");
    if (radio_wakeups_scheduled()) {
        uart_puts("every ");
        uart_put_udec(g_radio_wakeup_s);
        uart_puts(" implant s\n");
    } else {
        uart_puts("not scheduled at this implant clock scale\n");
    }
}

/* The radio, after an uncommanded reset: not at boot (see RADIO_RETRY_S). When it
 * comes up it also starts the retry wakeup at once, except in Safety Mode (where
 * the daily schedule resumes). */
static int g_radio_init_pending;
static uint32_t g_radio_init_us;

static int radio_barred(void);

static void radio_boot_deferred(uint32_t now) {
    g_radio_init_pending = 0;
    to(TELEMETRY);
    if (radio_barred()) {
        uart_puts("[radio] not powered: wandless telemetry was disabled by the battery test\n");
        return;
    }
    if (g_batt_state != BATT_OK) {
        uart_puts("[radio] not powered: the battery is below ERI, wandless telemetry stays off\n");
        return;
    }
    radio_boot(now);
    if (g_radio_ok && !g_safety && radio_wakeups_scheduled())
        g_radio_next_us = now;
}

static int radio_drained(void) {
    return g_radio_drained;
}

/* --- Firmware levels and the daily battery test ---
 *
 * BSC ACCOLADE physician letter, Sept 2025, Appendix A "Description of Software Enhancement":
 * the Model 3869 v2.04 software adds "a new daily wandless ZIP telemetry activated battery test that
 * assesses the battery's response to short bursts (e.g., milliseconds) of telemetry". Stage 1: an
 * elevated-impedance reading raises the alert "Voltage too low for projected remaining longevity"
 * (Code-1003), designed to come "at least three (3) months before pacemakers without this software would
 * have initiated Safety Mode"; "the earliest a Code-1003 alert will be initiated is four (4) days after
 * the in-office programmer interrogation when the pacemaker's software is upgraded". Stage 2: the test
 * "continues assessing the battery impedance and disables wandless ZIP telemetry operation before a high
 * impedance condition results in successive device resets"; "in certain cases in which a device has a
 * sufficiently high battery impedance at the time of receiving the software, the algorithm will disable ZIP
 * wandless telemetry without first issuing a Code-1003". Wanded telemetry keeps working.
 *
 * ONE image holds all three levels (FW_*), kept in the EEPROM and reported to the programmer:
 *   base   today's behaviour: no test, nothing disabled.
 *   v2.04  the mitigation, with the two bugs the letter reports: (1) "automatic low power telemetry wakeups
 *          are not disabled as intended" after a high battery impedance is detected, (2) the test "can
 *          misinterpret measurements in the presence of a magnet", a false positive that disables wandless
 *          telemetry and raises "an erroneous explant indicator (e.g., explant indicator was reached
 *          January 1, 2000)".
 *   fixed  the forthcoming software: no magnet false positive, the remaining wakeups fully disabled, and
 *          wandless telemetry re-enabled after a previous false positive.
 * MODELLING SIMPLIFICATION: the real upgrade downloads firmware through the Model 3300 LATITUDE programmer.
 * Here "upgrading" is one telemetry request (REQ_UPGRADE, over the wand or the communicator) that moves the
 * level forward; the code of every level is already in the image.
 *
 * THE TEST (a model of an algorithm the letter describes only in outline): once per implant day, in the quiet
 * slot just after a ventricular event (so the ~40 ms it holds the main loop falls inside the ventricular
 * refractory period), the firmware wakes the ZL70102's core with a WU_EN pulse, reads the rail at idle,
 * switches the transmitter to a spare MICS channel and the 200 kbit/s 2FSK_FB modulation, sets the training
 * word to 63 bytes (DS Figure 3-9) and clears the Idle flag: the radio then sends header-only wake-up
 * responses, each a (63 x 8 + 40 + 130) bits / 200 kbit/s = 3.4 ms transmission at 5.1 mA (DS Table 7-14
 * ID 9.3). No base is on that channel (imd_response: channel must match), so none answers and nothing links.
 * The ADC conversion is SYNCHRONISED to the burst: the driver polls STATUS.TX_ACTIVE, and 3.0 ms after it
 * rises converts the rail (four back-to-back conversions, 2 us each) and checks TX_ACTIVE is still set. A
 * short burst's sag is the load's charge over the rail capacitance (I t / C) until t approaches R C, so a
 * 1 ms burst cannot tell 250 ohm from 840 ohm; 3 ms can (stage 1 / stage 6 notes).
 * The reading is compared with the voltage a healthy cell shows in that burst for the PROJECTED REMAINING
 * LONGEVITY. The projection is the quiescent voltage on the Li/CFx curve (US 6,804,557): a high-impedance
 * cell reads normal at quiescent load (letter, Appendix A), so the quiescent voltage still says how much
 * charge is left while the burst says how much current it can deliver. (A coulomb count would be the other
 * source; here depletion is injected by the emulator, so only the voltage is trustworthy.)
 * Both tables below are the healthy cell's, measured in this model with this very sequence.
 *   margin = expected - burst:  >= BT_M1_MV for BT_CONFIRM_1003 daily tests in a row: Code-1003
 *                               >= BT_M2_MV for BT_CONFIRM_DISABLE tests in a row: wandless telemetry off
 * Four daily tests make the earliest Code-1003 four days after the upgrade (the first test is a day after
 * it). A rail that the idle core alone has already pulled BT_IDLE_SAG_MAX_MV below its quiescent voltage
 * (0.95 mA x R: about 85 ohm) would reset on the burst (5.1 mA x R over 3 ms): the burst is not sent and that
 * counts as the disable-class reading, so a device already past the point is disabled in two days with no
 * Code-1003 first. Thresholds are justified in notes/stage-6.md against the stage 1 defect table.
 */
#define BT_PERIOD_S 86400u
#define BT_RETRY_S 3600u
#define BT_CONFIRM_1003 4u
#define BT_CONFIRM_DISABLE 2u
#define BT_M1_MV 60u
#define BT_M2_MV 140u
#define BT_IDLE_SAG_MAX_MV 80u
#define BT_CHANNEL 8u
#define BT_MODULATION 0x00u /* 2FSK_FB both ways: 200 kbit/s raw (DS Table 3-5) */
#define BT_TRAINING 63u
#define BT_SAMPLE_US 3000u
#define BT_EDGE_WAIT_US 25000u /* the first response follows the wake by a 5-15 ms gap (assume::RESPONSE_GAP) */
#define BT_GATE_US 50000u
#define BT_GATE_GIVE_UP_US 5000000u
#define BT_REC_VALID 1u  /* the sample was taken inside the burst */
#define BT_REC_MAGNET 2u /* the magnet response was in force */
#define BT_V_OK 0u
#define BT_V_ELEVATED 1u
#define BT_V_HIGH 2u
#define BT_V_GUARD 3u    /* the rail was too low to send the burst: counted as HIGH */
#define BT_V_NOSAMPLE 4u /* the burst window never produced a sample */
#define BT_V_DISCARDED 5u /* an invalid sample the fixed level threw away */
#define CAL_EPOCH_UNIX 946684800u /* 2000-01-01 00:00 UTC: the device calendar's zero */

struct pt {
    uint16_t x, y;
};

/* Linear interpolation in an ascending table, clamped at both ends. */
static uint32_t interp(const struct pt *t, uint32_t n, uint32_t x) {
    if (x <= t[0].x)
        return t[0].y;
    for (uint32_t i = 1; i < n; i++) {
        if (x <= t[i].x) {
            uint32_t dx = t[i].x - t[i - 1].x;
            int32_t dy = (int32_t)t[i].y - (int32_t)t[i - 1].y;
            return (uint32_t)((int32_t)t[i - 1].y + dy * (int32_t)(x - t[i - 1].x) / (int32_t)dx);
        }
    }
    return t[n - 1].y;
}

/* Healthy cell: quiescent rail (mV, ADC) -> years of default-load longevity left, x100. Li/CFx curve,
 * US 6,804,557; measured on this model's `implant_cell` through this firmware (`battest now` at 4 to 0.6
 * years of longevity left, notes/stage-6.md). */
static const struct pt LONGEVITY_BY_VQ[] = {{2541, 60},  {2568, 70},  {2595, 80},  {2620, 90},  {2623, 100}, {2630, 120},
                                                  {2640, 150}, {2651, 200}, {2660, 250}, {2670, 300}, {2676, 350}, {2680, 400}};
/* Healthy cell: projected longevity (years x100) -> the rail 3.0 ms into the burst (mV), same measurements. */
static const struct pt EXPECTED_BURST[] = {{60, 2392},  {70, 2445},  {80, 2498},  {90, 2533},  {100, 2543}, {120, 2554},
                                                {150, 2566}, {200, 2577}, {250, 2588}, {300, 2598}, {350, 2605}, {400, 2610}};

static int g_bt_waiting;
static uint32_t g_bt_due_since;

static uint32_t cal_now(void) {
    uint32_t epoch = g_store.hdr.epoch_at_implant;
    return epoch > CAL_EPOCH_UNIX ? epoch - CAL_EPOCH_UNIX + g_store.hdr.implant_s : g_store.hdr.implant_s;
}

/* The explant indicator: ERI (and, in v2.04, the false positive) sets the flag. Only the ERI path records
 * the date -- `record_date` -- so a flag raised any other way carries the calendar's zero, 2000-01-01:
 * the letter's "explant indicator was reached January 1, 2000". */
static void fw_explant_reached(int record_date) {
    struct fw_block *fw = &g_store.fw;
    fw->flags |= FW_F_EXPLANT;
    if (record_date)
        fw->explant_cal = cal_now();
    store_dirty(fw, sizeof *fw);
}

static int bt_ready(void) {
    struct fw_block *fw = &g_store.fw;
    if (fw->level < FW_V204 || tele_off() || g_safety || !g_radio_ok || !g_radio_on || g_rs != RS_SLEEP || g_session ||
        g_tag_xfer || g_batt_state != BATT_OK || g_radio_init_pending || g_radio_reprogram || g_radio_sleep_pending ||
        g_store.hdr.implant_s < fw->next_test_s) {
        g_bt_waiting = 0;
        return 0;
    }
    /* FIXED: no test while a magnet response is in force (v2.04 bug 2: see battery_test_run). */
    if (fw->level == FW_FIXED && g_magnet)
        return 0;
    uint32_t now = time_us();
    if (!g_bt_waiting) {
        g_bt_waiting = 1;
        g_bt_due_since = now;
    }
    /* The test holds the main loop for up to ~40 ms: start it just after a ventricular event, when the
     * next pace is at least half a second away and the front end is in its refractory period. It does not
     * wait for one longer than BT_GATE_GIVE_UP_US, nor longer than a quarter of an implant day: at a fast
     * implant clock (the scenarios' fast-forward, where a day is shorter than a heartbeat) waiting for a beat
     * would make it one test per beat instead of one per day. */
    uint32_t give_up = implant_s_to_us(BT_PERIOD_S) / 4u;
    if (give_up > BT_GATE_GIVE_UP_US)
        give_up = BT_GATE_GIVE_UP_US;
    return (g_have_v_ev && (uint32_t)(now - g_last_v_ev) < BT_GATE_US) || (uint32_t)(now - g_bt_due_since) > give_up;
}

/* The fixed level keeps the radio unpowered once the test has disabled wandless telemetry. */
static int radio_barred(void) {
    return tele_off() && g_store.fw.level == FW_FIXED;
}

/* Wandless telemetry off, the way the fixed level does it: the radio's supply drops at the load switch,
 * so there is no sniffing, no wakeup of any kind and no session (the wand still works). */
static void radio_cut_power(void) {
    g_radio_on = 0;
    pin_set(RADIO_PWR_PIN, 0);
    pin_set(RADIO_WU_EN_PIN, 0);
    g_rtx_head = g_rtx_tail = 0;
    g_rs = RS_OFF;
}

/* The test says stop: wandless telemetry disabled. */
static void tele_disable(uint32_t why, uint32_t v_burst, uint32_t v_exp) {
    struct fw_block *fw = &g_store.fw;
    fw->flags |= FW_F_TELE_OFF;
    fw->disabled_s = g_store.hdr.implant_s;
    fw->disable_why = why;
    ep_log(EP_TELE_OFF, 0, v_burst, v_exp, why);
    store_dirty(fw, sizeof *fw);
    to(TELEMETRY);
    uart_puts("ALARM: WANDLESS TELEMETRY DISABLED (");
    uart_puts(why == DIS_BAD_SAMPLE ? "the battery test evaluated a burst reading that was never taken" : "high battery impedance");
    uart_puts(fw->level == FW_FIXED ? "): radio unpowered, no wakeups of any kind; the wand still works\n"
                                    : "): no sessions; automatic wakeups continue; the wand still works\n");
}

/* One test, start to finish, blocking (~40 ms at most, started by bt_ready). */
static void battery_test_run(void) {
    struct fw_block *fw = &g_store.fw;
    uint32_t level = fw->level;
    uint32_t vq = adc_mv(BATT_SAMPLES * 8u); /* the quiescent rail, averaged over 64 conversions */
    uint32_t l100 = interp(LONGEVITY_BY_VQ, sizeof LONGEVITY_BY_VQ / sizeof *LONGEVITY_BY_VQ, vq);
    uint32_t v_exp = interp(EXPECTED_BURST, sizeof EXPECTED_BURST / sizeof *EXPECTED_BURST, l100);
    g_bt_waiting = 0;

    /* The radio's core, woken by a WU_EN pulse as at boot (DS "Startup Method Using IMD Pin Control"). */
    pin_set(RADIO_WU_EN_PIN, 1);
    delay_us(RADIO_T_WU_PULSE_US);
    pin_set(RADIO_WU_EN_PIN, 0);
    delay_us(RADIO_T_START_US);
    if (!radio_irq()) {
        to(TELEMETRY);
        uart_puts("BATTEST: the radio core did not start; tried again in an hour\n");
        g_radio_reprogram = 1;
        fw->next_test_s = g_store.hdr.implant_s + BT_RETRY_S;
        store_dirty(fw, sizeof *fw);
        return;
    }
    uint32_t irqs = radio_rd(RADIO_REG_IRQ_STATUS);
    radio_wr(RADIO_REG_IRQ_CLEAR, irqs);
    radio_wr(RADIO_REG_MAC_CHANNEL, BT_CHANNEL);
    radio_wr(RADIO_REG_MAC_MODULATION, BT_MODULATION);
    radio_wr(RADIO_REG_MAC_TRAINING, BT_TRAINING);
    uint32_t v_pre = adc_mv(4);

    uint32_t v_burst = 0; /* 0 = no sample taken */
    uint32_t flags = g_magnet ? BT_REC_MAGNET : 0u;
    uint32_t verdict;
    int sent = 0;
    if (vq > v_pre && vq - v_pre >= BT_IDLE_SAG_MAX_MV) {
        verdict = BT_V_GUARD; /* the burst would reset the device: not sent */
    } else if (g_magnet) {
        /* HYPOTHESIS (the letter gives no mechanism): v2.04 shipped bug 2. While the magnet response is in
         * force the device paces asynchronously on a hard timer and a clinic magnet is also what marks a
         * programmer head, so the same RF interlock that refuses a radio link next to a wand (radio_blocks_nfc)
         * keeps the test's transmitter quiet: the burst window is skipped, and the sample the evaluation
         * reads below keeps its cleared value, 0 mV. v2.04's evaluation does not check that a sample was
         * taken, so a missing reading is "a battery far below any expectation": both stages at once.
         * The magnet RESPONSE has to be in force: with `magnet off` g_magnet never rises (the letter: "this
         * false positive behavior is not possible if the device's magnet response is turned off"). The
         * fixed level does not run the test under a magnet at all (bt_ready) and discards any invalid sample. */
        verdict = BT_V_NOSAMPLE;
    } else {
        radio_wr(RADIO_REG_MAC_IDLE, 0); /* Send_Response: the transmissions start after a 5-15 ms gap */
        uint32_t t0 = time_us();
        int seen = 0;
        while ((uint32_t)(time_us() - t0) < BT_EDGE_WAIT_US) {
            if (radio_rd(RADIO_REG_STATUS) & RADIO_STATUS_TX_ACTIVE) {
                seen = 1;
                break;
            }
        }
        verdict = BT_V_NOSAMPLE;
        if (seen) {
            delay_us(BT_SAMPLE_US);
            v_burst = adc_mv(4);
            if (radio_rd(RADIO_REG_STATUS) & RADIO_STATUS_TX_ACTIVE) {
                flags |= BT_REC_VALID;
                sent = 1;
            } else {
                v_burst = 0; /* the burst had already ended: not a measurement of it */
            }
        }
        radio_wr(RADIO_REG_MAC_IDLE, 1);
    }
    if (sent) {
        uint32_t margin = v_exp > v_burst ? v_exp - v_burst : 0u;
        verdict = margin >= BT_M2_MV ? BT_V_HIGH : margin >= BT_M1_MV ? BT_V_ELEVATED : BT_V_OK;
    }

    /* v2.04 evaluates whatever the burst window left, a 0 mV "reading" included; the fixed level throws an
     * invalid sample away and tries again in an hour. */
    int discard = level == FW_FIXED && verdict == BT_V_NOSAMPLE;
    if (discard)
        verdict = BT_V_DISCARDED;

    uint32_t margin = v_exp > v_burst ? v_exp - v_burst : 0u;
    struct bt_rec *r = &fw->rec[fw->tests++ % BT_RECS];
    r->implant_s = g_store.hdr.implant_s;
    r->v_pre_mv = (uint16_t)v_pre;
    r->v_burst_mv = (uint16_t)v_burst;
    r->v_exp_mv = (uint16_t)v_exp;
    r->l_x100 = (uint16_t)l100;
    r->flags = (uint8_t)flags;
    r->verdict = (uint8_t)verdict;
    r->reserved = 0;

    uint32_t why = 0;
    int raised = 0;
    if (discard) {
        fw->next_test_s = g_store.hdr.implant_s + BT_RETRY_S;
    } else {
        fw->next_test_s += BT_PERIOD_S;
        while (fw->next_test_s <= g_store.hdr.implant_s)
            fw->next_test_s += BT_PERIOD_S;
        /* A test holds the main loop for ~40 ms: never closer together than 100 ms of guest time, so that
         * at the default x1e6 clock (a day is 86 ms) it cannot starve pacing. At any scale up to
         * 8.6e5 implant s per guest s the schedule is a day. */
        uint32_t earliest = g_store.hdr.implant_s + g_scale / 10u;
        if (fw->next_test_s < earliest)
            fw->next_test_s = earliest;
        if (verdict == BT_V_NOSAMPLE) {
            /* v2.04: the 0 mV reading is below even the end-of-service voltage, so it is not read as an
             * impedance at all but as a depleted battery: telemetry off at once and the explant indicator
             * raised -- without the date, which only the ERI path writes. */
            fw->run_1003 = fw->run_dis = 0;
            why = DIS_BAD_SAMPLE;
        } else {
            fw->run_1003 = margin >= BT_M1_MV || verdict == BT_V_GUARD ? fw->run_1003 + 1u : 0u;
            fw->run_dis = margin >= BT_M2_MV || verdict == BT_V_GUARD ? fw->run_dis + 1u : 0u;
            if (verdict == BT_V_GUARD)
                fw->run_dis = BT_CONFIRM_DISABLE; /* no second look at a rail that cannot carry a burst */
            if (fw->run_dis >= BT_CONFIRM_DISABLE) {
                why = DIS_IMPEDANCE;
            } else if (fw->run_1003 >= BT_CONFIRM_1003 && !(fw->flags & FW_F_CODE1003)) {
                fw->flags |= FW_F_CODE1003;
                raised = 1;
                fw->alert_s = g_store.hdr.implant_s;
                ep_log(EP_CODE1003, 0, v_burst, v_exp, l100);
            }
        }
    }
    store_dirty(fw, sizeof *fw);

    /* The line a clinic's remote-monitoring service would also see. */
    to(TELEMETRY);
    uart_puts("BATTEST #");
    uart_put_udec(fw->tests);
    uart_puts(" day ");
    uart_put_udec(g_store.hdr.implant_s / 86400u);
    uart_puts(": quiescent ");
    uart_put_fixed(vq, 1000, 3);
    uart_puts(" V -> projected ");
    uart_put_fixed(l100, 100, 2);
    uart_puts(" y; idle ");
    uart_put_fixed(v_pre, 1000, 3);
    uart_puts(" V, burst ");
    if (v_burst)
        uart_put_fixed(v_burst, 1000, 3);
    else
        uart_puts("(no sample)");
    uart_puts(" V vs expected ");
    uart_put_fixed(v_exp, 1000, 3);
    uart_puts(" V, margin ");
    uart_put_udec(margin);
    uart_puts(" mV: ");
    static const char *const VERDICTS[] = {"OK", "ELEVATED", "HIGH", "NOT SENT (rail too low)", "NO SAMPLE", "DISCARDED (invalid sample)"};
    uart_puts(VERDICTS[verdict]);
    if (flags & BT_REC_MAGNET)
        uart_puts(" [magnet response in force]");
    uart_puts(" (runs ");
    uart_put_udec(fw->run_1003);
    uart_puts("/");
    uart_put_udec(BT_CONFIRM_1003);
    uart_puts(" alert, ");
    uart_put_udec(fw->run_dis);
    uart_puts("/");
    uart_put_udec(BT_CONFIRM_DISABLE);
    uart_puts(" disable)\n");
    if (raised) {
        uart_puts("ALARM: Code-1003 Voltage too low for projected remaining longevity: elevated battery impedance (red alert on the programmer and remote monitoring)\n");
    }

    if (why == DIS_IMPEDANCE && !tele_off()) {
        tele_disable(DIS_IMPEDANCE, v_burst, v_exp);
        if (level == FW_V204) {
            /* v2.04: sessions are refused, but the wakeups go on (bug 1). The chip's strobe is switched off so a
             * communicator cannot wake it, and its wake-up block's CRC recomputed. */
            radio_wr(RADIO_REG_WAKEUP_STROSC_CTRL, 0);
            radio_wr(RADIO_REG_WAKEUP_CRCCTRL, RADIO_CRC_CALC);
            radio_wr(RADIO_REG_WAKEUP_CRCCTRL, RADIO_CRC_CHECK);
        }
    } else if (why == DIS_BAD_SAMPLE && !tele_off()) {
        tele_disable(DIS_BAD_SAMPLE, v_burst, v_exp);
        fw_explant_reached(0);
        to(TELEMETRY);
        uart_puts("ALARM: EXPLANT INDICATOR reached (the battery is not at ERI: the date field was never written)\n");
        radio_wr(RADIO_REG_WAKEUP_STROSC_CTRL, 0);
        radio_wr(RADIO_REG_WAKEUP_CRCCTRL, RADIO_CRC_CALC);
        radio_wr(RADIO_REG_WAKEUP_CRCCTRL, RADIO_CRC_CHECK);
    }
    if (tele_off() && level == FW_FIXED)
        radio_cut_power();
    else
        radio_wr(RADIO_REG_MAC_CTRL, RADIO_MAC_SLEEP);
    g_bt_ran = 1;
    g_bt_end_us = time_us();
}

/* The programmer's upgrade request: move the firmware level forward (a downgrade, or an upgrade in Safety Mode,
 * is refused). Returns 1 if the level changed. The FIXED level also clears a v2.04 false positive: the
 * disabling reading was a missing sample (0 mV, below any real rail), so wandless telemetry is re-enabled and
 * the explant indicator raised without a date is withdrawn; if the cell really is high-impedance the next daily
 * test disables it again, properly. A device v2.04 had disabled for a real impedance reading has its remaining
 * wakeups switched off. */
static int fw_upgrade(uint32_t level) {
    struct fw_block *fw = &g_store.fw;
    if (g_safety || level > FW_FIXED || level <= fw->level)
        return 0;
    uint32_t old = fw->level;
    uint32_t cleared = 0;
    fw->level = level;
    fw->upgraded_s = g_store.hdr.implant_s;
    fw->next_test_s = g_store.hdr.implant_s + BT_PERIOD_S;
    fw->run_1003 = fw->run_dis = 0;
    if (level == FW_FIXED && tele_off() && fw->disable_why == DIS_BAD_SAMPLE) {
        fw->flags &= ~(FW_F_TELE_OFF | FW_F_EXPLANT);
        fw->flags |= FW_F_CLEARED;
        fw->disable_why = DIS_NONE;
        fw->disabled_s = 0;
        g_radio_reprogram = 1; /* the strobe oscillator comes back at the next wake */
        cleared = 1;
    }
    ep_log(EP_FW_LEVEL, 0, old, level, cleared);
    store_dirty(fw, sizeof *fw);
    if (level == FW_FIXED && tele_off() && g_radio_on)
        radio_cut_power();
    to(TELEMETRY);
    uart_puts("FIRMWARE level ");
    uart_puts(old == FW_BASE ? "base" : "v2.04");
    uart_puts(" -> ");
    uart_puts(level == FW_V204 ? "v2.04" : "fixed");
    uart_puts(cleared ? "; wandless telemetry re-enabled after a false positive\n" : "\n");
    return 1;
}

/* --- Programmer link: one command per line --- */

static char g_line[48];
static uint32_t g_line_len;

static int parse_uint(const char *s, uint32_t *out) {
    uint32_t v = 0;
    int any = 0;
    while (*s == ' ')
        s++;
    while (*s >= '0' && *s <= '9') {
        v = v * 10u + (uint32_t)(*s++ - '0');
        any = 1;
    }
    *out = v;
    return any && (*s == '\0' || *s == ' ');
}

static int starts_with(const char *s, const char *prefix) {
    while (*prefix) {
        if (*s++ != *prefix++)
            return 0;
    }
    return *s == '\0' || *s == ' ';
}

static void print_chamber(const char *label, uint32_t amp_mv, uint32_t sens_uv) {
    uart_puts(label);
    uart_puts(" amp=");
    uart_put_fixed(amp_mv, 1000, 3);
    uart_puts(" V sens=");
    uart_put_fixed(sens_uv, 1000, sens_uv % 100u ? 2 : 1); /* Safety Mode's 0.25 mV */
    uart_puts(" mV");
}

/* One line. The dual-chamber timers and the atrial channel only appear in
 * the modes that use them. */
static void print_settings(void) {
    uart_puts("mode=");
    uart_puts(MODE_NAMES[g_mode]);
    uart_puts(" rate=");
    if (g_safety)
        uart_puts("72.5");
    else
        uart_put_udec(g_rate_ppm);
    uart_puts(" ppm");
    if (rate_responsive(g_mode)) {
        uart_puts(" msr=");
        uart_put_udec(g_msr_ppm);
        uart_puts(" ppm");
    }
    if (dual_chamber(g_mode)) {
        uart_puts(" utr=");
        uart_put_udec(g_utr_ppm);
        uart_puts(" ppm pav=");
        uart_put_udec(g_pav_ms);
        uart_puts(" sav=");
        uart_put_udec(g_sav_ms);
        uart_puts(" pvarp=");
        uart_put_udec(g_pvarp_ms);
        uart_puts(" ms");
    }
    uart_puts(" pw=");
    uart_put_udec(g_pw_us);
    uart_puts(" us |");
    if (!ventricle_only(g_mode)) {
        print_chamber(" A", g_aamp_mv, g_asens_uv);
        uart_puts(" |");
    }
    print_chamber(" V", g_amp_mv, g_sens_uv);
    uart_puts(g_magnet_response ? " | magnet=async" : " | magnet=off");
    if (g_safety)
        uart_puts(" | SAFETY MODE (VVI, unipolar, fixed settings)");
}

/* Set a parameter if `value` is within [lo, hi]. */
static int set_bounded(uint32_t *param, uint32_t value, uint32_t lo, uint32_t hi) {
    if (value < lo || value > hi) {
        uart_puts("[prog] out of range: ");
        uart_put_udec(lo);
        uart_puts("..");
        uart_put_udec(hi);
        uart_puts("\n");
        return 0;
    }
    *param = value;
    return 1;
}

/* The total atrial refractory period, SAV + PVARP, is the shortest P-P
 * interval DDD can track 1:1. If it is longer than the upper-rate interval
 * the patient reaches 2:1 block before the UTR -- programmable, as on real
 * devices, but worth a warning. */
static void check_tarp(void) {
    uint32_t tarp_ms = g_sav_ms + g_pvarp_ms;
    if (tarp_ms * 1000u > uri_us()) {
        uart_puts("[prog] note: TARP (SAV + PVARP) = ");
        uart_put_udec(tarp_ms);
        uart_puts(" ms is longer than the upper-rate interval: 2:1 block above ");
        uart_put_udec(60000u / tarp_ms);
        uart_puts(" ppm\n");
    }
}

static void run_command(const char *line) {
    uint32_t value;
    const char *arg = line;
    while (*arg && *arg != ' ')
        arg++;
    int has_value = parse_uint(arg, &value);

    if (starts_with(line, "help")) {
        uart_puts("[prog] mode DDD|DDDR|DOO|VVI|VVIR|VOO|OOO, rate <ppm 30-180, below msr and utr>,\n"
                  "[prog] utr <ppm 80-180>, pav <ms 50-300>, sav <ms 50-300>, pvarp <ms 150-500>,\n"
                  "[prog] pw <us 100-1500>, amp / aamp <mV 100-4000> (V / A), sens <uV 500-10000>,\n"
                  "[prog] asens <uV 200-5000>, msr <ppm, above rate, up to utr>, athresh <mg 10-500>,\n"
                  "[prog] rrslope <ppm per 100 mg 1-100>, rrup <s 10-120>, rrdown <s 30-900>,\n"
                  "[prog] magnet async|off, activity, battery, battest [now], radio [wakeup <implant s>], pause [<ms>], timescale [<implant s per guest s>],\n"
                  "[prog] status, reboot (Safety Mode, once latched, fixes the pacing parameters for good)\n"
                  "[prog] (diagnostics are read through the wand: see programmer/pacemaker_programmer.py)\n");
        return;
    }
    if (starts_with(line, "reboot")) {
        /* A programmer-commanded reset: checkpoint, then the watchdog's chip
         * reset (pico-sdk watchdog_reboot). Diagnostics come back from the
         * EEPROM, and the reset is logged as a power-on. */
        uart_puts("[prog] saving diagnostics, then reset\n");
        diag_flush();
        REG(PSM_WDSEL) = PSM_WDSEL_ALL & ~PSM_WDSEL_OSCS;
        REG(WATCHDOG_CTRL + SET_ALIAS) = WATCHDOG_CTRL_TRIGGER;
        for (;;) {
        }
    }
    if (starts_with(line, "status")) {
        uart_puts("[prog] ");
        print_settings();
        uart_puts(" | ");
        print_battery();
        uart_puts("\n");
        return;
    }
    if (starts_with(line, "activity")) {
        uart_puts("[prog] ");
        print_activity();
        uart_puts(g_accel_ok ? "" : " (NO SENSOR)");
        uart_puts(" | rate response: lower ");
        uart_put_udec(g_rate_ppm);
        uart_puts(" ppm, msr ");
        uart_put_udec(g_msr_ppm);
        uart_puts(" ppm, threshold ");
        uart_put_udec(g_athresh_mg);
        uart_puts(" mg, slope ");
        uart_put_udec(g_rrslope);
        uart_puts(" ppm/100 mg, reaction ");
        uart_put_udec(g_rrup_s);
        uart_puts(" s, recovery ");
        uart_put_udec(g_rrdown_s);
        uart_puts(" s | resting atrial rate ");
        if (g_rest_n)
            uart_put_fixed(g_rest_n * 600000u / g_rest_ms, 10, 1);
        else
            uart_puts("not yet measured");
        uart_puts(g_rest_n ? " bpm\n" : "\n");
        return;
    }
    if (starts_with(line, "battery")) {
        uart_puts("[prog] ");
        print_battery();
        uart_puts(" (loaded; ERI below 2.550 V, EOL below 2.300 V)\n");
        return;
    }
    if (starts_with(line, "magnet")) {
        if (g_safety) {
            uart_puts("[prog] SAFETY MODE: magnet response disabled, not programmable\n");
            return;
        }
        while (*arg == ' ')
            arg++;
        if (starts_with(arg, "async") || starts_with(arg, "off")) {
            g_magnet_response = starts_with(arg, "async");
            uart_puts("[prog] ");
            print_settings();
            uart_puts("\n");
        } else {
            uart_puts("[prog] magnet must be async or off\n");
        }
        return;
    }
    if (starts_with(line, "mode")) {
        if (g_safety) {
            uart_puts("[prog] SAFETY MODE: VVI 72.5 ppm, fixed and not programmable\n");
            return;
        }
        while (*arg == ' ')
            arg++;
        for (uint32_t m = 0; m < N_MODES; m++) {
            if (starts_with(arg, MODE_NAMES[m])) {
                if (rate_responsive(m) && !g_accel_ok) {
                    uart_puts("[prog] no activity sensor: rate-responsive modes unavailable\n");
                    return;
                }
                diag_mode(g_mode, m, 0);
                g_mode = m;
                g_mode_changed = 1;
                uart_puts("[prog] mode=");
                uart_puts(MODE_NAMES[m]);
                uart_puts("\n");
                return;
            }
        }
        uart_puts("[prog] mode must be DDD, DDDR, DOO, VVI, VVIR, VOO or OOO\n");
        return;
    }
    if (starts_with(line, "radio")) {
        while (*arg == ' ')
            arg++;
        if (starts_with(arg, "wakeup")) {
            uint32_t secs;
            if (!parse_uint(arg + 6, &secs)) {
                uart_puts("[prog] radio wakeup <implant seconds between automatic wakeups, 0 = off>\n");
                return;
            }
            g_radio_wakeup_s = secs;
            radio_wakeup_refresh();
            g_radio_sched_note = 0;
            radio_arm_wakeup(time_us());
        }
        static const char *const RS_NAMES[] = {"off", "asleep", "pulsing WU_EN", "starting", "responding", "linked"};
        uart_puts("[prog] radio: ");
        uart_puts(g_radio_ok ? RS_NAMES[g_rs] : g_radio_fault ? "missing" : "off");
        uart_puts(g_radio_on ? "" : ", wandless telemetry disabled");
        uart_puts(", automatic wakeups ");
        if (radio_wakeups_scheduled()) {
            uart_puts("every ");
            uart_put_udec(g_radio_wakeup_s);
            uart_puts(" implant s (");
            uart_put_udec(radio_wakeup_interval_us() / 1000u);
            uart_puts(" ms of guest time)\n");
        } else {
            uart_puts(g_radio_wakeup_s ? "not scheduled at this implant clock scale\n" : "off\n");
        }
        return;
    }
    if (starts_with(line, "battest") || starts_with(line, "fw")) {
        static const char *const LEVELS[] = {"base", "v2.04", "fixed"};
        struct fw_block *fw = &g_store.fw;
        while (*arg == ' ')
            arg++;
        uart_puts("[prog] firmware level ");
        uart_puts(LEVELS[fw->level <= FW_FIXED ? fw->level : 0]);
        uart_puts(" (upgraded only by a programmer's telemetry request)");
        if (fw->level == FW_BASE) {
            uart_puts("; no battery test at this level\n");
            return;
        }
        if (starts_with(arg, "now"))
            fw->next_test_s = 0; /* due at once; it still waits for the quiet slot after a ventricular event */
        uart_puts("; battery test #");
        uart_put_udec(fw->tests);
        uart_puts(", next ");
        if (fw->next_test_s <= g_store.hdr.implant_s)
            uart_puts("due");
        else {
            uart_puts("in ");
            uart_put_udec((fw->next_test_s - g_store.hdr.implant_s) / 3600u);
            uart_puts(" implant h");
        }
        uart_puts(fw->flags & FW_F_CODE1003 ? "; Code-1003 RAISED" : "; no Code-1003");
        uart_puts(tele_off() ? "; wandless telemetry DISABLED\n" : "; wandless telemetry enabled\n");
        return;
    }
    if (starts_with(line, "pause")) {
        if (has_value) {
            if (value < 1000u || value > 10000u) {
                uart_puts("[prog] pause <detection duration in ms, 1000..10000>\n");
                return;
            }
            g_pause_us = value * 1000u;
        }
        uart_puts("[prog] ventricular pause detection after ");
        uart_put_udec(g_pause_us / 1000u);
        uart_puts(" ms with no VP or VS\n");
        return;
    }
    if (starts_with(line, "timescale")) {
        if (has_value) {
            if (value < SCALE_MIN || value > SCALE_MAX) {
                uart_puts("[prog] timescale <implant s per guest s, 100..1000000>\n");
                return;
            }
            g_scale = value;
            g_store.sf.scale = value;
            store_dirty(&g_store.sf, sizeof g_store.sf);
            scale_apply();
            g_radio_sched_note = 0;
            radio_arm_wakeup(time_us());
        }
        uart_puts("[prog] time scale ");
        uart_put_udec(g_scale);
        uart_puts(" implant s per guest s (set /board/battery set_aging_factor to match)\n");
        return;
    }
    if (g_safety && has_value) {
        uart_puts("[prog] SAFETY MODE: pacing parameters are fixed and not programmable\n");
        return;
    }
    int ok = 0;
    if (!has_value) {
        uart_puts("[prog] unknown command (try help): ");
        uart_puts(line);
        uart_puts("\n");
        return;
    } else if (starts_with(line, "rate")) {
        ok = set_bounded(&g_rate_ppm, value, 30, (g_msr_ppm < g_utr_ppm ? g_msr_ppm : g_utr_ppm) - 1);
    } else if (starts_with(line, "utr")) {
        ok = set_bounded(&g_utr_ppm, value, g_rate_ppm < 80 ? 80 : g_rate_ppm + 1, 180);
        if (ok && g_msr_ppm > g_utr_ppm) {
            /* The sensor may not drive the atrium past what DDD can track. */
            g_msr_ppm = g_utr_ppm;
            uart_puts("[prog] msr lowered to the new utr\n");
        }
    } else if (starts_with(line, "msr")) {
        ok = set_bounded(&g_msr_ppm, value, g_rate_ppm + 1, g_utr_ppm);
    } else if (starts_with(line, "athresh")) {
        ok = set_bounded(&g_athresh_mg, value, 10, 500);
    } else if (starts_with(line, "rrslope")) {
        ok = set_bounded(&g_rrslope, value, 1, 100);
    } else if (starts_with(line, "rrup")) {
        ok = set_bounded(&g_rrup_s, value, 10, 120);
    } else if (starts_with(line, "rrdown")) {
        ok = set_bounded(&g_rrdown_s, value, 30, 900);
    } else if (starts_with(line, "pav")) {
        ok = set_bounded(&g_pav_ms, value, 50, 300);
    } else if (starts_with(line, "sav")) {
        ok = set_bounded(&g_sav_ms, value, 50, 300);
    } else if (starts_with(line, "pvarp")) {
        ok = set_bounded(&g_pvarp_ms, value, 150, 500);
    } else if (starts_with(line, "amp")) {
        ok = set_bounded(&g_amp_mv, value, 100, 4000);
        if (ok)
            dac_set_vpace_mv(g_amp_mv);
    } else if (starts_with(line, "aamp")) {
        ok = set_bounded(&g_aamp_mv, value, 100, 4000);
        if (ok)
            dac_set_apace_mv(g_aamp_mv);
    } else if (starts_with(line, "pw")) {
        ok = set_bounded(&g_pw_us, value, 100, 1500);
    } else if (starts_with(line, "sens")) {
        ok = set_bounded(&g_sens_uv, value, 500, 10000);
    } else if (starts_with(line, "asens")) {
        ok = set_bounded(&g_asens_uv, value, 200, 5000);
    } else {
        uart_puts("[prog] unknown command (try help): ");
        uart_puts(line);
        uart_puts("\n");
        return;
    }
    if (ok) {
        uart_puts("[prog] ");
        print_settings();
        uart_puts("\n");
        check_tarp();
    }
}

static void prompt(void) {
    to(CONSOLE);
    uart_puts("> ");
}

/* Console input, echoed as typed so the user sees their command line: Enter
 * runs it, backspace/DEL erases a character. Nothing else is printed to the
 * console between prompts, so the line is never broken up. */
static void poll_programmer(void) {
    while (!(REG(CONSOLE + UART_FR) & UART_FR_RXFE)) {
        char c = (char)(REG(CONSOLE + UART_DR) & 0xffu);
        to(CONSOLE);
        if (c == '\r' || c == '\n') {
            uart_puts("\n");
            if (g_line_len) {
                g_line[g_line_len] = '\0';
                run_command(g_line);
                g_line_len = 0;
            }
            prompt();
        } else if (c == '\b' || c == 0x7f) {
            if (g_line_len) {
                g_line_len--;
                uart_puts("\b \b");
            }
        } else if (c >= ' ' && g_line_len < sizeof(g_line) - 1) {
            g_line[g_line_len++] = c;
            uart_putc(c);
        }
    }
}

/* --- Marker channel --- */

/* `av_us`, when non-zero, is the AV interval that ended in this ventricular
 * event; `interval_us` is the time since the previous event in the same
 * chamber. */
static void marker(const char *code, uint32_t now, uint32_t av_us, uint32_t interval_us) {
    to(TELEMETRY);
    uart_puts(code);
    uart_puts(" t=");
    uart_put_fixed(now / 1000u, 1000, 3);
    if (av_us) {
        uart_puts(" AV=");
        uart_put_udec(av_us / 1000u);
        uart_puts(" ms");
    }
    if (interval_us) {
        uart_puts(" interval=");
        uart_put_udec(interval_us / 1000u);
        uart_puts(" ms (");
        uart_put_fixed(600000000u / interval_us, 10, 1);
        uart_puts(" bpm)");
    }
    uart_puts("\n");
}

/* Safety Mode (Table 3): the fixed settings, and the ventricular lead unipolar
 * (GP28 high drives the TS5A3159 IN to the can: sensing and pacing return both).
 * Hooks for a later stage: automatic gain control on the 0.25 mV sensitivity, and
 * the noise response (VOO while the sense channel sees interference). The atrial
 * channel is simply unused: VVI never senses or paces it. */
static void safety_apply(void) {
    g_mode = MODE_VVI;
    g_rate_ppm = SAFETY_RATE_PPM;
    g_amp_mv = SAFETY_AMP_MV;
    g_pw_us = SAFETY_PW_US;
    g_sens_uv = SAFETY_SENS_UV;
    g_magnet_response = 0;
    pin_output(POLARITY_PIN, 1);
}

int main(void) {
    REG(RESETS_RESET + CLR_ALIAS) = RESET_BITS;
    while ((REG(RESETS_RESET_DONE) & RESET_BITS) != RESET_BITS) {
    }

    clocks_init();

    REG(PAD_GPIO(0) + CLR_ALIAS) = PAD_ISO;
    REG(PAD_GPIO(1) + CLR_ALIAS) = PAD_ISO;
    REG(PAD_GPIO(1) + SET_ALIAS) = PAD_IE;
    REG(PAD_GPIO(4) + CLR_ALIAS) = PAD_ISO;
    REG(GPIO_CTRL(0)) = FUNCSEL_UART;
    REG(GPIO_CTRL(1)) = FUNCSEL_UART;
    REG(GPIO_CTRL(4)) = FUNCSEL_UART; /* UART1 TX */
    /* Both at 115200 from the 12 MHz clk_peri: 12e6/(16*115200) = 6.51,
     * FBRD = round(0.51 * 64) = 33. The console receives; telemetry only
     * transmits. */
    REG(CONSOLE + UART_IBRD) = 6;
    REG(CONSOLE + UART_FBRD) = 33;
    REG(CONSOLE + UART_LCR_H) = 0x70;
    REG(CONSOLE + UART_CR) = 0x301; /* UARTEN | TXE | RXE */
    REG(TELEMETRY + UART_IBRD) = 6;
    REG(TELEMETRY + UART_FBRD) = 33;
    REG(TELEMETRY + UART_LCR_H) = 0x70;
    REG(TELEMETRY + UART_CR) = 0x101; /* UARTEN | TXE */

    /* SPI0 -> ADS1292R: mode 1 (CPOL 0, CPHA 1: DIN latched on SCLK falling
     * edge), 8-bit frames, 12 MHz / 12 = 1 MHz -- within the "twice fCLK"
     * limit for register access (§8.5.1.2). */
    pin_spi(ADS_MISO_PIN);
    pin_spi(ADS_SCK_PIN);
    pin_spi(ADS_MOSI_PIN);
    pin_output(ADS_CS_PIN, 1);
    pin_input(ADS_DRDY_PIN);
    pin_output(ACCEL_CS_PIN, 1);
    pin_input(ACCEL_INT1_PIN);
    spi0_format(SPI0_ADS_CR0, SPI0_ADS_CPSR);

    /* SPI1 -> MCP4822: mode 0, 12 MHz / 2 = 6 MHz (max 20 MHz). */
    pin_spi(DAC_SCK_PIN);
    pin_spi(DAC_MOSI_PIN);
    pin_output(DAC_CS_PIN, 1);
    spi1_format(SPI1_DAC_CR0, SPI1_DAC_CPSR);
    /* ...and the RF430CL330H beside it, mode 0 at 100 kHz, the only part on
     * SPI1 that answers. Its transactions run on DMA (see the link): the
     * PL022's DMA requests go to the DMA's DREQ_SPI1_TX/RX. */
    REG(SPI1_BASE + SPI_DMACR) = SPI_DMACR_TXDMAE | SPI_DMACR_RXDMAE;
    pin_spi(TAG_MISO_PIN);
    pin_output(TAG_CS_PIN, 1);
    pin_input(TAG_INTO_PIN);

    /* Output switches open before anything else can happen. */
    pin_output(VPACE_PIN, 0);
    pin_output(APACE_PIN, 0);
    pin_output(POLARITY_PIN, 0); /* TS5A3159 IN low: the ring, bipolar, until Safety Mode */
    pin_input(MAGNET_PIN);
    pin_analog(VBAT_PIN);

    uart_puts("\nRP2350 pacemaker demo: DDD(R) dual-chamber (ADS1292R + ADXL362 + MCP4822 + 2x TS5A3166 + RF430CL330H)\n");
    int ads_ok = ads_init();
    if (!ads_ok) {
        uart_puts("[fault] sensing front end missing -- pacing DOO only\n");
        g_mode = MODE_DOO;
    }
    g_accel_ok = accel_init();
    if (!g_accel_ok)
        uart_puts("[fault] activity sensor missing -- rate response unavailable\n");
    g_tag_ok = tag_init();
    if (!g_tag_ok)
        uart_puts("[fault] telemetry head missing -- a programmer wand cannot interrogate\n");
    dac_set_vpace_mv(g_amp_mv);
    dac_set_apace_mv(g_aamp_mv);
    g_batt_mv = battery_read_mv();
    crc_init();
    diag_init(time_us());
    if (g_safety)
        safety_apply();
    dac_set_vpace_mv(g_amp_mv); /* again: Safety Mode's amplitude */
    if (g_batt_mv >= ERI_MV && g_boot_cause == RC_SUPERVISOR && !radio_barred()) {
        g_radio_init_pending = 1;
        g_radio_init_us = time_us() + (g_safety || g_radio_retry_us < RADIO_SETTLE_US ? RADIO_SETTLE_US : g_radio_retry_us);
        uart_puts("[radio] supervisor reset: not initialised at boot, comes up later\n");
    } else if (radio_barred())
        uart_puts("[radio] not powered: wandless telemetry was disabled by the battery test (fixed level: the radio stays off)\n");
    else if (g_batt_mv >= ERI_MV)
        radio_boot(time_us());
    else
        uart_puts("[radio] not powered: the battery is below ERI, wandless telemetry stays off\n");
    uart_puts("[prog] ");
    print_settings();
    uart_puts(" | ");
    print_battery();
    uart_puts("\n[prog] type `help` for programmer commands; markers and alarms are on telemetry (UART1);\n"
              "[prog] diagnostics are read through a programmer wand (RF430CL330H)\n");
    to(TELEMETRY);
    uart_puts("\n[telemetry] pacemaker marker channel: AP/VP paced, AS/VS sensed, AR/VR sensed in refractory\n");
    if (g_safety) {
        uart_puts("ALARM: SAFETY MODE -- latched at implant s ");
        uart_put_udec(g_store.sf.latched_s);
        uart_puts(" after ");
        uart_put_udec(SAFETY_RESETS);
        uart_puts(" system resets within 48 h: VVI 72.5 ppm, RV unipolar 5.0 V @ 1.0 ms, AGC 0.25 mV, noise response VOO, magnet response off. Permanent: replace the device\n");
    }
    if (g_store.fw.flags & FW_F_CODE1003)
        uart_puts("ALARM: Code-1003 Voltage too low for projected remaining longevity\n");
    if (tele_off())
        uart_puts("ALARM: WANDLESS TELEMETRY DISABLED by the battery test; the wand still works\n");
    prompt();

    uint32_t now = time_us();
    /* Ventricular channel. */
    uint32_t last_v = now;    /* last ventricular event that restarts timing */
    uint32_t last_vp = now;   /* for blanking */
    int last_v_was_pace = 0;
    /* Last sensed beat. In VVI and DDD it is also `last_v`; in VOO, DOO and
     * OOO a sense restarts no timing, but still opens a refractory window of
     * its own, or one R wave would be sensed on every sample it spans. */
    uint32_t last_v_sense = now;
    uint32_t last_v_any = 0;  /* any VP, VS or VR: the VR quiet window and PVAB */
    /* Atrial channel, the same set. */
    uint32_t last_a = now;
    uint32_t last_ap = now;
    int last_a_was_pace = 0;
    uint32_t last_a_sense = now;
    uint32_t last_a_any = 0;
    /* An atrial event is waiting for its ventricular one: the AV delay, of
     * `av_us`, is running from `last_a`. */
    int in_av = 0;
    uint32_t av_us = 0;

    uint32_t last_status = now;
    uint32_t last_batt = now;
    uint32_t last_act = now;
    uint32_t last_trend = now;
    uint32_t last_ckpt = now;
    uint32_t last_frame = now;
    uint32_t loff = 0;
    int32_t vhist[2] = {0, 0}; /* the two previous samples, in uV */
    int32_t ahist[2] = {0, 0};
    uint32_t n_ap = 0, n_as = 0, n_ar = 0, n_vp = 0, n_vs = 0, n_vr = 0;
    g_pause_ref = now;

    for (;;) {
        poll_programmer();
        now = time_us();
        implant_update(now);

        /* The DRV5032 latches OUT between its 20 Hz samples, so one read is
         * already a debounced one. The wand carries a magnet: one going away
         * mid-session is the clinician lifting the wand, which ends it. */
        int hall = !(REG(SIO_GPIO_IN) & (1u << MAGNET_PIN));
        if (g_hall && !hall) {
            g_hall = 0;
            if (g_session && g_xport == XP_NFC)
                session_close("wand removed");
            g_wand = 0;
        }
        g_hall = hall;
        int magnet_now = g_magnet_response && hall && !g_wand;
        if (magnet_now != g_magnet) {
            g_magnet = magnet_now;
            g_tmt_left = magnet_now ? TMT_BEATS : 0;
            g_mode_changed = 1;
            diag_magnet(magnet_now, eff_mode(), now);
            to(TELEMETRY);
            if (magnet_now) {
                uart_puts("MAGNET on (DRV5032 OUT low): asynchronous ");
                uart_puts(MODE_NAMES[eff_mode()]);
                if (eff_mode() != MODE_OOO)
                    uart_puts(g_batt_state == BATT_OK
                                  ? ", threshold margin test 3 beats @ 100 ppm (3rd at 80% pw), then 85 ppm"
                                  : ", threshold margin test 3 beats @ 100 ppm (3rd at 80% pw), then 65 ppm (ERI)");
                uart_puts("\n");
            } else {
                uart_puts(!hall      ? "MAGNET off: back to programmed "
                          : g_wand ? "MAGNET response suspended by the programmer session: back to programmed "
                                      : "MAGNET response programmed off: back to programmed ");
                uart_puts(MODE_NAMES[g_mode]);
                uart_puts("\n");
            }
        }
        uint32_t mode = eff_mode();
        g_diag_mode = mode;

        if (g_mode_changed) {
            g_mode_changed = 0;
            in_av = 0;
            last_v = now;
            g_pause_ref = now;
            diag_restart_intervals();
        }

        if (dual_chamber(mode)) {
            if (in_av) {
                /* AV delay running: pace the ventricle when it expires, but
                 * in DDD never sooner than the upper tracking rate allows. */
                int due = (uint32_t)(now - last_a) >= av_us;
                if (atrial_tracking(mode) && (uint32_t)(now - last_v) < uri_us())
                    due = 0;
                if (due) {
                    uint32_t av = now - last_a;
                    uint32_t interval = now - last_v;
                    pace(VPACE_PIN);
                    n_vp++;
                    last_vp = now;
                    last_v = now;
                    last_v_any = now;
                    last_v_was_pace = 1;
                    in_av = 0;
                    marker("VP", now, av, interval);
                    diag_v(now, MK_VP, last_a_was_pace ? SEQ_AP : SEQ_AS);
                    if (g_magnet && g_tmt_left)
                        g_tmt_left--;
                }
            } else if ((uint32_t)(now - last_v) >= va_us()) {
                /* VA interval expired with no atrial sense. */
                uint32_t interval = now - last_a;
                pace(APACE_PIN);
                n_ap++;
                atrial_event(now, 0);
                last_ap = now;
                last_a = now;
                last_a_any = now;
                last_a_was_pace = 1;
                in_av = 1;
                av_us = g_pav_ms * 1000u;
                marker("AP", now, 0, interval);
                diag_a(now, MK_AP);
            }
        } else if (mode != MODE_OOO && (uint32_t)(now - last_v) >= lri_us()) {
            /* Lower-rate timer: VVI paces when it expires, VOO always. */
            uint32_t interval = now - last_v;
            pace(VPACE_PIN);
            n_vp++;
            last_vp = now;
            last_v = now;
            last_v_any = now;
            last_v_was_pace = 1;
            marker("VP", now, 0, interval);
            diag_v(now, MK_VP, SEQ_NONE);
            if (g_magnet && g_tmt_left)
                g_tmt_left--;
        }

        pause_check(now, mode);

        if (ads_drdy()) {
            uint32_t status;
            int32_t code[2];
            ads_read_frame(&status, code);
            uint32_t t = time_us();
            last_frame = t;
            int32_t uv = ads_code_to_uv(code[0]);
            egm_sample(uv, ads_code_to_uv(code[1]));
            int32_t vslope = uv - vhist[0]; /* x[n] - x[n-2]: a 4 ms first difference */
            if (vslope < 0)
                vslope = -vslope;
            vhist[0] = vhist[1];
            vhist[1] = uv;
            uv = ads_code_to_uv(code[1]);
            int32_t aslope = uv - ahist[0];
            if (aslope < 0)
                aslope = -aslope;
            ahist[0] = ahist[1];
            ahist[1] = uv;

            uint32_t now_loff = (status >> ADS_STATUS_LOFF_SHIFT) & (ADS_LOFF_CH1 | ADS_LOFF_CH2);
            if (now_loff != loff) {
                uint32_t changed = now_loff ^ loff;
                loff = now_loff;
                if (changed & ADS_LOFF_CH1)
                    diag_lead(0, ADS_LOFF_CH1, (loff & ADS_LOFF_CH1) != 0u, t);
                if (changed & ADS_LOFF_CH2)
                    diag_lead(1, ADS_LOFF_CH2, (loff & ADS_LOFF_CH2) != 0u, t);
                to(TELEMETRY);
                if (changed & ADS_LOFF_CH1)
                    uart_puts(loff & ADS_LOFF_CH1 ? "ALARM: LEAD_OFF (ventricular lead, LOFF_STAT=0x"
                                                  : "lead-off cleared (ventricular lead, LOFF_STAT=0x");
                else
                    uart_puts(loff & ADS_LOFF_CH2 ? "ALARM: LEAD_OFF (atrial lead, LOFF_STAT=0x"
                                                  : "lead-off cleared (atrial lead, LOFF_STAT=0x");
                uart_put_hex8(loff);
                uart_puts(")\n");
            }

            /* Ventricular channel. */
            uint32_t since_v = t - last_v;
            uint32_t since_sense = t - last_v_sense;
            int blanked = (last_v_was_pace && (uint32_t)(t - last_vp) < BLANKING_US) ||
                          (last_a_was_pace && (uint32_t)(t - last_ap) < VBLANK_AFTER_AP_US);
            if (g_safety)
                agc_sample(t, (uint32_t)vslope, blanked);
            if (!blanked && !(loff & ADS_LOFF_CH1) && vslope >= (int32_t)g_sens_uv) {
                int noise = noise_cross(t);
                int vquiet = (uint32_t)(t - last_v_any) >= VR_QUIET_US;
                /* A wave whose onset was sensed as VR is still that wave when
                 * VRP ends partway through it: not a new beat. */
                int outside = since_v >= VRP_US && since_sense >= VRP_US && vquiet;
                if (outside && noise) {
                    /* Inside a noise chain (or reverted): not a beat; pacing carries on at the
                     * lower rate. */
                    n_vr++;
                    last_v_any = t;
                    marker("VR", t, 0, 0);
                    diag_v(t, MK_VR, SEQ_NONE);
                } else if (outside && (uint32_t)vslope < agc_threshold(t)) {
                    /* Above the programmed sensitivity, below the AGC template: not sensed. */
                } else if (outside) {
                    /* VS: a real ventricular beat. VVI and DDD inhibit and
                     * restart timing; VOO and DOO just report it, and so does
                     * OOO, which has no timing to restart. */
                    n_vs++;
                    int tracking = atrial_tracking(mode) && in_av;
                    marker("VS", t, tracking ? t - last_a : 0, since_v < since_sense ? since_v : since_sense);
                    diag_v(t, MK_VS, tracking ? (last_a_was_pace ? SEQ_AP : SEQ_AS) : SEQ_NONE);
                    if (g_safety)
                        g_agc_max = (uint32_t)vslope; /* the wave's rise is the start of its peak */
                    last_v_any = t;
                    last_v_sense = t;
                    if (atrial_tracking(mode) || vvi_like(mode) || mode == MODE_OOO) {
                        last_v = t;
                        last_v_was_pace = 0;
                        in_av = 0;
                    }
                } else if (vquiet) {
                    /* A sense in refractory that is not the last event's own
                     * tail: a PVC right after a beat, say. Reported, never
                     * acted on. */
                    n_vr++;
                    last_v_any = t;
                    marker("VR", t, 0, 0);
                    diag_v(t, MK_VR, SEQ_NONE);
                }
            }

            /* Atrial channel: not sensed at all in the single-chamber
             * ventricular modes, as on a real VVI device. */
            int ablanked = (last_a_was_pace && (uint32_t)(t - last_ap) < ABLANK_US) ||
                           (uint32_t)(t - last_v_any) < PVAB_US;
            if (!ventricle_only(mode) && !ablanked && !(loff & ADS_LOFF_CH2) &&
                aslope >= (int32_t)g_asens_uv) {
                uint32_t since_a = t - last_a;
                uint32_t since_asense = t - last_a_sense;
                int quiet = (uint32_t)(t - last_a_any) >= AR_QUIET_US;
                if (atrial_tracking(mode) && in_av) {
                    /* Atrial refractory for the whole AV delay: almost always
                     * the triggering P wave's own tail. */
                    if (quiet) {
                        n_ar++;
                        atrial_event(t, 1);
                        last_a_any = t;
                        marker("AR", t, 0, 0);
                        diag_a(t, MK_AR);
                    }
                } else if (atrial_tracking(mode) && (uint32_t)(t - last_v) < g_pvarp_ms * 1000u) {
                    /* PVARP: reported, never tracked. */
                    if (quiet) {
                        n_ar++;
                        atrial_event(t, 1);
                        last_a_any = t;
                        marker("AR", t, 0, 0);
                        diag_a(t, MK_AR);
                    }
                } else if (since_a >= ARP_US && since_asense >= ARP_US && quiet) {
                    /* AS: DDD tracks it and starts the sensed AV delay. Not
                     * within `quiet` of an AR: a P wave whose onset fell in
                     * PVARP is still that P wave when PVARP ends partway
                     * through it, and tracking it would pace off a beat
                     * already declared refractory. */
                    n_as++;
                    atrial_event(t, 1);
                    marker("AS", t, 0, since_a < since_asense ? since_a : since_asense);
                    diag_a(t, MK_AS);
                    last_a_sense = t;
                    last_a_any = t;
                    if (atrial_tracking(mode)) {
                        last_a = t;
                        last_a_was_pace = 0;
                        in_av = 1;
                        av_us = g_sav_ms * 1000u;
                    }
                } else if (quiet) {
                    n_ar++;
                    atrial_event(t, 1);
                    last_a_any = t;
                    marker("AR", t, 0, 0);
                    diag_a(t, MK_AR);
                }
            }

            /* Right after a front-end frame: the one moment an SPI0 burst
             * cannot hide the next DRDY (see accel_drain). The same quiet
             * slot, when the accelerometer does not need it, takes a step of
             * the wand's link or writes an EEPROM chunk. */
            if (g_accel_ok && accel_int1())
                accel_drain();
            else if (link_due())
                link_service();
            else if (radio_due())
                radio_service();
            else
                eeprom_service(time_us());
        } else if (!ads_ok || (uint32_t)(now - last_frame) > QUIET_FALLBACK_US) {
            /* No front end, or it has gone silent: interrogation and
             * checkpoints must not depend on it -- a sensing failure is just
             * when a clinic wants the device's history. */
            if (link_due())
                link_service();
            else if (radio_due())
                radio_service();
            else
                eeprom_service(now);
        }

        /* A fresh reading: a request answered later in this pass stamped
         * g_session_last_us after `now`. */
        if (g_session && (uint32_t)(time_us() - g_session_last_us) >= SESSION_TIMEOUT_US)
            session_close("timed out");

        if (g_radio_init_pending && (int32_t)(now - g_radio_init_us) >= 0 && !g_tag_xfer && !g_session)
            radio_boot_deferred(now);

        if ((uint32_t)(now - last_trend) >= g_trend_period_us) {
            last_trend = now;
            diag_trend();
        }

        if ((uint32_t)(now - last_act) >= ACT_PERIOD_US) {
            uint32_t elapsed_ms = (now - last_act) / 1000u;
            last_act = now;
            rate_response_update(elapsed_ms, mode);
        }

        if ((uint32_t)(now - last_batt) >= BATT_PERIOD_US) {
            last_batt = now;
            battery_check();
        }

        /* The implant clock's checkpoint: what a reset will lose of it, and so of the
         * 48 h window, is at most this period (see CKPT_PERIOD_S). */
        if ((uint32_t)(now - last_ckpt) >= g_ckpt_us) {
            last_ckpt = now;
            store_dirty(&g_store.hdr, sizeof g_store.hdr);
        }

        if ((uint32_t)(now - last_status) >= STATUS_PERIOD_US) {
            last_status = now;
            to(TELEMETRY);
            uart_puts("[status] ");
            print_settings();
            if (g_magnet) {
                uart_puts(" | MAGNET: ");
                uart_puts(MODE_NAMES[mode]);
            }
            uart_puts(" | last 5 s:");
            if (!ventricle_only(mode)) {
                uart_puts(" AP=");
                uart_put_udec(n_ap);
                uart_puts(" AS=");
                uart_put_udec(n_as);
                uart_puts(" AR=");
                uart_put_udec(n_ar);
            }
            uart_puts(" VP=");
            uart_put_udec(n_vp);
            uart_puts(" VS=");
            uart_put_udec(n_vs);
            uart_puts(" VR=");
            uart_put_udec(n_vr);
            if (loff & ADS_LOFF_CH2)
                uart_puts(" A_LEAD_OFF");
            if (loff & ADS_LOFF_CH1)
                uart_puts(" V_LEAD_OFF");
            uart_puts(" | ");
            print_activity();
            uart_puts(" | ");
            print_battery();
            uart_puts("\n");
            n_ap = n_as = n_ar = n_vp = n_vs = n_vr = 0;
        }
    }
}
