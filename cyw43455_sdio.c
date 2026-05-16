/*
 * cyw43455_sdio.c — F1 backplane access, F2 data channel, clock management
 *
 * The F1 function provides a 32-bit address window into the chip's SoC
 * backplane.  A 3-byte window register (SBADDRLOW/MID/HIGH) selects the
 * 32 KB page; bit 15 of the F1 data address selects 4-byte vs 1-byte mode.
 *
 * F2 carries SDPCM-framed WLAN packets and is accessed via extended writes.
 *
 * Locking: Milestone 1 has no concurrent access paths (no interrupts, no
 * ioctl, single callout).  All locking is deferred to Milestone 2.  The
 * SDIO bus methods internally acquire their own CAM SIM lock; holding our
 * private mutex while calling them would violate lock order (bus framework
 * holds Giant during device_attach which establishes Giant > sc->mtx, but
 * the CAM SIM lock is ordered below sc->mtx in the callout path).
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/systm.h>

#include <dev/sdio/sdio_subr.h>
#include <dev/sdio/sdiob.h>
#include "sdio_if.h"		/* SDIO_READ_EXTENDED / SDIO_WRITE_EXTENDED */

#include "cyw43455_var.h"

/* -------------------------------------------------------------------------
 * Backplane window management
 * ------------------------------------------------------------------------- */

/* Update the three SBADDR registers if the window has changed. */
static int
cyw_set_backplane_window(struct cyw_softc *sc, uint32_t addr)
{
	uint32_t new_win = addr & ~SBSDIO_SB_OFT_ADDR_MASK;
	int err = 0;

	if (new_win == sc->bp_window)
		return (0);

	sdio_write_1(sc->f1, SBSDIO_FUNC1_SBADDRLOW,
	    (new_win >>  8) & 0xff, &err);
	if (err) return (err);
	sdio_write_1(sc->f1, SBSDIO_FUNC1_SBADDRMID,
	    (new_win >> 16) & 0xff, &err);
	if (err) return (err);
	sdio_write_1(sc->f1, SBSDIO_FUNC1_SBADDRHIGH,
	    (new_win >> 24) & 0xff, &err);
	if (err) return (err);

	sc->bp_window = new_win;
	return (0);
}

/* -------------------------------------------------------------------------
 * 32-bit backplane read/write (single register)
 * ------------------------------------------------------------------------- */

uint32_t
cyw_bp_read32(struct cyw_softc *sc, uint32_t addr)
{
	uint32_t f1_off;
	int err = 0;

	if (cyw_set_backplane_window(sc, addr) != 0)
		return (0xffffffff);

	f1_off = (addr & SBSDIO_SB_OFT_ADDR_MASK) | SBSDIO_SB_ACCESS_2_4B_FLAG;
	return (sdio_read_4(sc->f1, f1_off, &err));
}

void
cyw_bp_write32(struct cyw_softc *sc, uint32_t addr, uint32_t val)
{
	uint32_t f1_off;
	int err = 0;

	if (cyw_set_backplane_window(sc, addr) != 0)
		return;

	f1_off = (addr & SBSDIO_SB_OFT_ADDR_MASK) | SBSDIO_SB_ACCESS_2_4B_FLAG;
	sdio_write_4(sc->f1, f1_off, val, &err);
}

/* -------------------------------------------------------------------------
 * Block write to backplane (for firmware download)
 *
 * Splits the transfer at 32 KB window boundaries and uses extended writes
 * to keep SDIO bus transactions large.
 * ------------------------------------------------------------------------- */
int
cyw_bp_write_block(struct cyw_softc *sc, uint32_t addr,
    const uint8_t *buf, size_t total)
{
	device_t parent = device_get_parent(sc->dev);
	size_t done = 0;
	int err;

	while (done < total) {
		uint32_t f1_off = (addr + done) & SBSDIO_SB_OFT_ADDR_MASK;
		size_t chunk = MIN(total - done,
		    (size_t)(SBSDIO_SB_OFT_ADDR_MASK + 1 - f1_off));

		err = cyw_set_backplane_window(sc, addr + done);
		if (err)
			return (err);

		err = SDIO_WRITE_EXTENDED(parent, 1 /* F1 */, f1_off,
		    chunk, __DECONST(uint8_t *, buf + done), true);
		if (err)
			return (err);

		done += chunk;
	}
	return (0);
}

/* -------------------------------------------------------------------------
 * F2 block write (SDPCM packet transmission path)
 * ------------------------------------------------------------------------- */
int
cyw_f2_write_block(struct cyw_softc *sc, uint32_t addr,
    const uint8_t *buf, size_t len)
{
	device_t parent = device_get_parent(sc->dev);

	return (SDIO_WRITE_EXTENDED(parent, 2 /* F2 */, addr, len,
	    __DECONST(uint8_t *, buf), true));
}

/* -------------------------------------------------------------------------
 * Clock management — request ALP, then optionally HT
 * ------------------------------------------------------------------------- */
int
cyw_sdio_enable_clock(struct cyw_softc *sc)
{
	int err = 0, retries;
	uint8_t csr;

	/* Step 1: request ALP (always-low-power) clock first */
	sdio_write_1(sc->f1, SBSDIO_FUNC1_CHIPCLKCSR, SBSDIO_ALP_AVAIL_REQ, &err);
	if (err)
		return (err);

	for (retries = 50; retries > 0; retries--) {
		csr = sdio_read_1(sc->f1, SBSDIO_FUNC1_CHIPCLKCSR, &err);
		if (err)
			return (err);
		if (csr & SBSDIO_ALP_AVAIL)
			break;
		DELAY(1000);
	}
	if (retries == 0) {
		device_printf(sc->dev, "ALP clock timeout (CSR=0x%02x)\n", csr);
		return (ETIMEDOUT);
	}

	/*
	 * Step 2: request HT (high-throughput) clock with FORCE_HW_CLKREQ_OFF.
	 * brcmf_sdio_htclk() does this before firmware download; without HT
	 * clock the chip's SDIO core may not initialize reliably after ARM boot.
	 * Keep the request asserted — cyw_sdio_detach() will clear it.
	 */
	sdio_write_1(sc->f1, SBSDIO_FUNC1_CHIPCLKCSR,
	    SBSDIO_HT_AVAIL_REQ | SBSDIO_FORCE_HW_CLKREQ_OFF, &err);
	if (err)
		return (err);

	for (retries = 50; retries > 0; retries--) {
		csr = sdio_read_1(sc->f1, SBSDIO_FUNC1_CHIPCLKCSR, &err);
		if (err)
			return (err);
		if (csr & SBSDIO_HT_AVAIL)
			break;
		DELAY(5000);
	}
	if (retries == 0)
		device_printf(sc->dev, "HT clock not available (CSR=0x%02x), "
		    "continuing with ALP\n", csr);

	return (0);
}

/* -------------------------------------------------------------------------
 * Poll F2 ready bit (firmware sets it when boot succeeds)
 * ------------------------------------------------------------------------- */
int
cyw_sdio_f2_ready(struct cyw_softc *sc)
{
	int err = 0, retries;
	uint8_t iordy;

	for (retries = 300; retries > 0; retries--) {
		iordy = sdio_f0_read_1(sc->f1, SDIO_CCCR_IORx, &err);
		if (err == 0 && (iordy & (1 << 2)))	/* bit 2 = F2 ready */
			return (0);
		DELAY(10000);	/* 10 ms */
	}
	device_printf(sc->dev, "F2 not ready after 3 s (IORx=0x%02x err=%d)\n",
	    iordy, err);
	return (ETIMEDOUT);
}

/* -------------------------------------------------------------------------
 * ARM CR4 core control — halt and release
 * ------------------------------------------------------------------------- */
static void
cyw_arm_halt(struct cyw_softc *sc)
{
	uint32_t val;

	/*
	 * Reset D11 (802.11 MAC) core using D11-specific IOCTL bits.
	 * Matches brcmf_chip_resetcore(d11, PHYRESET|PHYCLOCKEN, ..., PHYCLOCKEN).
	 */
	cyw_bp_write32(sc, CYW_D11_WRAP_BASE + BCMA_IOCTL,
	    BCMA_D11_IOCTL_PHYRESET | BCMA_D11_IOCTL_PHYCLOCKEN | BCMA_IOCTL_FGC);
	cyw_bp_write32(sc, CYW_D11_WRAP_BASE + BCMA_RESET_CTL,
	    BCMA_RESET_CTL_RESET);
	DELAY(1);
	cyw_bp_write32(sc, CYW_D11_WRAP_BASE + BCMA_RESET_CTL, 0);
	DELAY(1);
	cyw_bp_write32(sc, CYW_D11_WRAP_BASE + BCMA_IOCTL,
	    BCMA_D11_IOCTL_PHYCLOCKEN);

	/*
	 * Halt ARM CR4, matching brcmf_chip_disable_arm(BCMA_CORE_ARM_CR4):
	 *   1. Briefly clear CPUHALT (bring out of any prior halt state)
	 *   2. Set FGC | CPUHALT (halt with forced clock transition)
	 *   3. Clear RESET_CTL (ensure not in reset — core must be out of reset)
	 *   4. Clear FGC, keep CPUHALT — CRITICAL: FGC must be cleared or AXI
	 *      bus transactions to SOCSRAM will be gated.
	 */
	val = cyw_bp_read32(sc, CYW_ARM_WRAP_BASE + BCMA_IOCTL);
	val &= ~BCMA_IOCTL_CPUHALT;
	cyw_bp_write32(sc, CYW_ARM_WRAP_BASE + BCMA_IOCTL, val);
	val |= BCMA_IOCTL_FGC | BCMA_IOCTL_CPUHALT;
	cyw_bp_write32(sc, CYW_ARM_WRAP_BASE + BCMA_IOCTL, val);
	cyw_bp_write32(sc, CYW_ARM_WRAP_BASE + BCMA_RESET_CTL, 0);
	val &= ~BCMA_IOCTL_FGC;	/* clear FGC, keep CPUHALT */
	cyw_bp_write32(sc, CYW_ARM_WRAP_BASE + BCMA_IOCTL, val);
	DELAY(1);
}

void
cyw_arm_release(struct cyw_softc *sc)
{
	uint32_t ioctl, resetctl;

	/*
	 * ChipCommon chipcontrol write, matching brcmf_chip_cr4_set_active():
	 *   Write chipcontrol_addr=4, chipcontrol_data=2 (bit 1 of CC register 4).
	 *   Exact purpose is chip-specific; brcmfmac does this before ARM release
	 *   on all CR4 chips and it appears necessary for reliable firmware boot.
	 */
	cyw_bp_write32(sc, CYW_SI_ENUM_BASE + CC_CHIPCTL_ADDR, 4);
	cyw_bp_write32(sc, CYW_SI_ENUM_BASE + CC_CHIPCTL_DATA, 2);

	/*
	 * Release ARM CR4, matching brcmf_chip_resetcore(arm, CPUHALT, CPUHALT, 0):
	 *   Disable step: FGC|CPUHALT + RESET, then Enable step: FGC|CPUHALT + no
	 *   reset, then clear FGC|CPUHALT — ARM begins executing from reset vector.
	 */
	cyw_bp_write32(sc, CYW_ARM_WRAP_BASE + BCMA_IOCTL,
	    BCMA_IOCTL_FGC | BCMA_IOCTL_CPUHALT);
	cyw_bp_write32(sc, CYW_ARM_WRAP_BASE + BCMA_RESET_CTL,
	    BCMA_RESET_CTL_RESET);
	DELAY(10);
	cyw_bp_write32(sc, CYW_ARM_WRAP_BASE + BCMA_IOCTL,
	    BCMA_IOCTL_FGC | BCMA_IOCTL_CPUHALT);
	cyw_bp_write32(sc, CYW_ARM_WRAP_BASE + BCMA_RESET_CTL, 0);
	DELAY(1);
	cyw_bp_write32(sc, CYW_ARM_WRAP_BASE + BCMA_IOCTL, 0);
	DELAY(1);

	/* Diagnostic: confirm ARM is out of reset and running */
	ioctl    = cyw_bp_read32(sc, CYW_ARM_WRAP_BASE + BCMA_IOCTL);
	resetctl = cyw_bp_read32(sc, CYW_ARM_WRAP_BASE + BCMA_RESET_CTL);
	device_printf(sc->dev, "ARM post-release: IOCTL=0x%08x RESET_CTL=0x%08x%s\n",
	    ioctl, resetctl,
	    (ioctl & BCMA_IOCTL_CPUHALT) ? " [HALTED]" : " [running]");
}

/* -------------------------------------------------------------------------
 * cyw_sdio_attach — enable F1, read chip ID, set up F2 block size
 * ------------------------------------------------------------------------- */
int
cyw_sdio_attach(struct cyw_softc *sc)
{
	uint32_t chipid_reg;
	int err;

	/* Set F1 block size before enabling */
	err = sdio_set_block_size(sc->f1, CYW_F1_BLKSIZE);
	if (err) {
		device_printf(sc->dev, "F1 set_block_size failed: %d\n", err);
		return (err);
	}

	/* Enable F1 */
	err = sdio_enable_func(sc->f1);
	if (err) {
		device_printf(sc->dev, "F1 enable failed: %d\n", err);
		return (err);
	}

	/* Request ALP clock so we can access the backplane */
	err = cyw_sdio_enable_clock(sc);
	if (err)
		return (err);

	/* Read chip ID from ChipCommon */
	chipid_reg = cyw_bp_read32(sc, CYW_SI_ENUM_BASE + CYW_CHIPCOMMON_ID_OFF);
	sc->chip_id  = chipid_reg & 0xffff;
	sc->chip_rev = (chipid_reg >> 16) & 0xf;

	if (sc->chip_id != CYW_CHIP_ID_43455) {
		device_printf(sc->dev,
		    "unexpected chip ID 0x%04x (expected 0x%04x)\n",
		    sc->chip_id, CYW_CHIP_ID_43455);
		return (ENXIO);
	}

	sdio_f0_write_1(sc->f1,
	    SDIO_FBR_BASE(2) + SDIO_FBR_BLKSIZE_LO,
	    CYW_F2_BLKSIZE & 0xff, &err);
	if (err)
		return (err);
	sdio_f0_write_1(sc->f1,
	    SDIO_FBR_BASE(2) + SDIO_FBR_BLKSIZE_HI,
	    (CYW_F2_BLKSIZE >> 8) & 0xff, &err);
	if (err)
		return (err);

	/* Watermarks for F2 RX flow control */
	sdio_write_1(sc->f1, SBSDIO_WATERMARK, CYW_F2_WATERMARK, &err);
	if (err)
		return (err);
	sdio_write_1(sc->f1, SBSDIO_FUNC1_MESBUSYCTRL,
	    CYW_MES_WATERMARK | 0x80 /* enable */, &err);
	if (err)
		return (err);

	/* Halt ARM so we can download firmware */
	cyw_arm_halt(sc);

	sc->sdio_attached = true;
	return (0);
}

/* -------------------------------------------------------------------------
 * cyw_sdio_detach — deassert chip (minimal cleanup for Milestone 1)
 * ------------------------------------------------------------------------- */
void
cyw_sdio_detach(struct cyw_softc *sc)
{
	int err = 0;

	if (!sc->sdio_attached)
		return;
	/* Release HT clock request set during enable_clock */
	sdio_write_1(sc->f1, SBSDIO_FUNC1_CHIPCLKCSR, 0, &err);
	sdio_disable_func(sc->f2);
	sdio_disable_func(sc->f1);
	sc->sdio_attached = false;
}
