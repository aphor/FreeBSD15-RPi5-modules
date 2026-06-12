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
 * F2 FIFO write (SDPCM packet transmission path)
 *
 * F2 is a FIFO, not a memory-mapped region.  All CMD53 transfers must use
 * fixed-address mode (incaddr=false) at CYW_F2_FIFO_ADDR (0x8000).
 * Transfer length must be rounded up to CYW_F2_BLKSIZE (64 bytes) so sdiob
 * uses block-mode CMD53.  The SDPCM HW header carries the true frame length.
 * Reference: brcmfmac-freebsd sdpcm.c brcmf_sdpcm_send().
 * ------------------------------------------------------------------------- */
int
cyw_f2_write_block(struct cyw_softc *sc, const uint8_t *buf, size_t flen)
{
	device_t parent = device_get_parent(sc->dev);
	size_t txlen;

	/* Round up to block boundary for block-mode CMD53 */
	txlen = (flen + CYW_F2_BLKSIZE - 1) & ~(size_t)(CYW_F2_BLKSIZE - 1);

	return (SDIO_WRITE_EXTENDED(parent, 2 /* F2 */, CYW_F2_FIFO_ADDR,
	    txlen, __DECONST(uint8_t *, buf), false /* FIFO, fixed addr */));
}

/* -------------------------------------------------------------------------
 * ARM CR4 TCM bank scan — compute actual SRAM size
 *
 * brcmf_chip_tcm_ramsize() — chip.c:680–707.  On BCM43455 the hardcoded
 * 0xdc000 is the expected result; this confirms it rather than assuming it.
 * ------------------------------------------------------------------------- */
static uint32_t
cyw_cr4_ram_size(struct cyw_softc *sc)
{
	uint32_t corecap, nab, nbb, bxinfo, blksize, memsize;
	unsigned int idx;

	corecap = cyw_bp_read32(sc, CYW_ARM_CORE_BASE + ARMCR4_CAP);
	nab = (corecap & ARMCR4_TCBANB_MASK);
	nbb = (corecap & ARMCR4_TCBBNB_MASK) >> 4;
	memsize = 0;

	for (idx = 0; idx < nab + nbb; idx++) {
		cyw_bp_write32(sc, CYW_ARM_CORE_BASE + ARMCR4_BANKIDX, idx);
		bxinfo = cyw_bp_read32(sc, CYW_ARM_CORE_BASE + ARMCR4_BANKINFO);
		blksize = ARMCR4_BSZ_MULT;
		if (bxinfo & ARMCR4_BLK_1K_MASK)
			blksize >>= 3;
		uint32_t banksz = ((bxinfo & ARMCR4_BSZ_MASK) + 1) * blksize;
		CYW_DPRINTF(sc, CYW_DBG_SDIO,
		    "  CR4 bank %u: bxinfo=0x%08x blksize=%u banksz=%u KB\n",
		    idx, bxinfo, blksize, banksz / 1024);
		memsize += banksz;
	}
	CYW_DPRINTF(sc, CYW_DBG_SDIO,
	    "CR4 TCM: nab=%u nbb=%u ramsize=0x%x (%u KB)\n",
	    nab, nbb, memsize, memsize / 1024);
	return (memsize);
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

	for (retries = 500; retries > 0; retries--) {
		iordy = sdio_f0_read_1(sc->f1, SDIO_CCCR_IORx, &err);
		if (err == 0 && (iordy & (1 << 2)))	/* bit 2 = F2 ready */
			return (0);
		DELAY(10000);	/* 10 ms */
	}
	device_printf(sc->dev, "F2 not ready after 5 s (IORx=0x%02x err=%d)\n",
	    iordy, err);
	return (ETIMEDOUT);
}

/* -------------------------------------------------------------------------
 * ARM CR4 core control — halt and release
 * ------------------------------------------------------------------------- */
static void
cyw_arm_halt(struct cyw_softc *sc)
{
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
	 * Halt ARM CR4 for firmware download.
	 *
	 * We hold the CPU halted via CPUHALT in IOCTL but do NOT assert
	 * BCMA RESET (RESET_CTL stays 0).  BCMA reset gates the ARM core's
	 * AXI slave port, which also makes SOCSRAM inaccessible from the
	 * backplane — each SDIO F1 write stalls for tens of milliseconds
	 * waiting for an AXI response that never comes, causing the SDHCI
	 * data-transfer timeout before the firmware can be written.
	 *
	 * The cyw_arm_release() function does the full coredisable + resetcore
	 * sequence (RESET_CTL = RESET → 0) after the download, so the ARM
	 * gets a clean hard reset before executing firmware regardless.
	 *
	 * Sequence (matches brcmf intent minus the hard-reset):
	 *   1. Set FGC | CLK | CPUHALT (force clock transition, halt CPU)
	 *   2. Flush read
	 *   3. Clear RESET_CTL (ensure not in hard reset — keeps SOCSRAM live)
	 *   4. Drop FGC, keep CLK | CPUHALT
	 */
	cyw_bp_write32(sc, CYW_ARM_WRAP_BASE + BCMA_IOCTL,
	    BCMA_IOCTL_FGC | BCMA_IOCTL_CLK | BCMA_IOCTL_CPUHALT);
	(void)cyw_bp_read32(sc, CYW_ARM_WRAP_BASE + BCMA_IOCTL);	/* flush */
	cyw_bp_write32(sc, CYW_ARM_WRAP_BASE + BCMA_RESET_CTL, 0);
	DELAY(1);
	cyw_bp_write32(sc, CYW_ARM_WRAP_BASE + BCMA_IOCTL,
	    BCMA_IOCTL_CLK | BCMA_IOCTL_CPUHALT);
	DELAY(10);
}

void
cyw_arm_release(struct cyw_softc *sc, uint32_t rstvec)
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
	 * Clear all pending INTSTATUS bits before writing the reset vector.
	 * brcmf_sdio_buscore_activate() (sdio.c:4094-4096) does this first:
	 *   brcmf_sdiod_writel(sdiodev, core->base + SD_REG(intstatus), 0xFFFFFFFF)
	 * Without this, stale bits from F1 enable / clock setup confuse the
	 * interrupt-gating logic after the ARM starts.
	 */
	if (sc->sdio_core_base != 0)
		cyw_bp_write32(sc, sc->sdio_core_base + SD_REG_INTSTATUS, 0xFFFFFFFF);

	/*
	 * Write the ARM reset vector to backplane address 0x00000000 — the
	 * CR4 ITCM reset vector slot.  brcmf_chip_cr4_set_active() does
	 * brcmf_sdiod_ramrw(sdiodev, true, 0, &rstvec, 4); addr=0 is the
	 * ITCM slot, not SOCSRAM base.  The CPU fetches from address 0 on
	 * reset; writing the firmware entry branch there lets it jump to the
	 * code loaded at 0x198000.
	 */
	if (rstvec != 0)
		cyw_bp_write32(sc, 0x00000000, rstvec);

	/*
	 * Release ARM CR4, matching brcmf_chip_resetcore(arm, CPUHALT, 0, 0):
	 *
	 * Disable step (brcmf_chip_ai_coredisable, prereset=CPUHALT, reset=0):
	 *   1. IOCTL = FGC|CLK|CPUHALT  (+ flush read)
	 *   2. RESET_CTL = RESET
	 *   3. udelay(1)
	 *   4. IOCTL = CPUHALT  (drop FGC)
	 *   5. udelay(10)
	 *
	 * Enable step (back in brcmf_chip_resetcore):
	 *   6. IOCTL = FGC|CLK  (no CPUHALT; + flush read)
	 *   7. RESET_CTL = 0    (release from reset)
	 *   8. udelay(1)
	 *   9. IOCTL = CLK      (drop FGC; + flush read) — ARM begins executing
	 *
	 * CRITICAL: the final IOCTL must be BCMA_IOCTL_CLK (0x01), not 0.
	 * Without CLK asserted the ARM wrapper does not clock the CPU and
	 * the firmware never runs.
	 */
	/* Disable step — FGC|CLK|CPUHALT matches brcmf_chip_ai_coredisable(prereset=CPUHALT) */
	cyw_bp_write32(sc, CYW_ARM_WRAP_BASE + BCMA_IOCTL,
	    BCMA_IOCTL_FGC | BCMA_IOCTL_CLK | BCMA_IOCTL_CPUHALT);
	(void)cyw_bp_read32(sc, CYW_ARM_WRAP_BASE + BCMA_IOCTL);
	cyw_bp_write32(sc, CYW_ARM_WRAP_BASE + BCMA_RESET_CTL,
	    BCMA_RESET_CTL_RESET);
	DELAY(1);
	cyw_bp_write32(sc, CYW_ARM_WRAP_BASE + BCMA_IOCTL,
	    BCMA_IOCTL_CPUHALT);
	DELAY(10);

	/* Enable step */
	cyw_bp_write32(sc, CYW_ARM_WRAP_BASE + BCMA_IOCTL,
	    BCMA_IOCTL_FGC | BCMA_IOCTL_CLK);
	(void)cyw_bp_read32(sc, CYW_ARM_WRAP_BASE + BCMA_IOCTL);
	cyw_bp_write32(sc, CYW_ARM_WRAP_BASE + BCMA_RESET_CTL, 0);
	DELAY(1);
	cyw_bp_write32(sc, CYW_ARM_WRAP_BASE + BCMA_IOCTL, BCMA_IOCTL_CLK);
	(void)cyw_bp_read32(sc, CYW_ARM_WRAP_BASE + BCMA_IOCTL);

	/* Diagnostic: IOCTL should be 0x01 (CLK), RESET_CTL should be 0x00 */
	ioctl    = cyw_bp_read32(sc, CYW_ARM_WRAP_BASE + BCMA_IOCTL);
	resetctl = cyw_bp_read32(sc, CYW_ARM_WRAP_BASE + BCMA_RESET_CTL);
	CYW_DPRINTF(sc, CYW_DBG_BRINGUP,
	    "ARM post-release: IOCTL=0x%08x RESET_CTL=0x%08x%s\n",
	    ioctl, resetctl,
	    (ioctl & BCMA_IOCTL_CPUHALT) ? " [HALTED]" : " [running]");
}

/* -------------------------------------------------------------------------
 * EROM scanner — find the SDIO device core base address
 *
 * The BCMA EROM (Enumeration ROM) is a table of 32-bit descriptors at the
 * address stored in ChipCommon register CHIPC_EROMPTR (offset 0xfc).
 * We walk it to find the core with ID BHND_COREID_SDIOD (0x829) and return
 * the base address of its first DEVICE-type slave port region.
 *
 * Reference: /usr/src/sys/dev/bhnd/bcma/bcma_eromreg.h and bcma_erom.c.
 * ------------------------------------------------------------------------- */
static uint32_t
cyw_erom_find_sdio_core_base(struct cyw_softc *sc)
{
	uint32_t erom_base, erom_addr, entry;
	int in_sdiod = 0;
	int max_entries = 512;	/* safety limit */

	/* Read EROM base pointer from ChipCommon */
	erom_base = cyw_bp_read32(sc, CYW_SI_ENUM_BASE + CHIPC_EROMPTR);
	if (erom_base == 0 || erom_base == 0xffffffff) {
		device_printf(sc->dev, "EROM: bad EROMPTR 0x%08x\n", erom_base);
		return (0);
	}
	CYW_DPRINTF(sc, CYW_DBG_SDIO, "EROM: base=0x%08x\n", erom_base);

	erom_addr = erom_base;
	while (max_entries-- > 0) {
		entry = cyw_bp_read32(sc, erom_addr);
		erom_addr += 4;

		if ((entry & 0xf) == BCMA_EROM_TABLE_EOF)
			break;

		if (!(entry & BCMA_EROM_ENTRY_ISVALID))
			continue;

		switch (entry & BCMA_EROM_ENTRY_TYPE_MASK) {
		case BCMA_EROM_ENTRY_TYPE_CORE: {
			uint32_t coreb;
			uint16_t corid;
			uint8_t nmp, ndp __unused;

			corid = (entry & BCMA_EROM_COREA_ID_MASK) >>
			    BCMA_EROM_COREA_ID_SHIFT;

			/* consume COREB */
			coreb = cyw_bp_read32(sc, erom_addr);
			erom_addr += 4;

			nmp  = (coreb & BCMA_EROM_COREB_NUM_MP_MASK) >>
			    BCMA_EROM_COREB_NUM_MP_SHIFT;
			ndp  = (coreb & BCMA_EROM_COREB_NUM_DP_MASK) >>
			    BCMA_EROM_COREB_NUM_DP_SHIFT;
			(void)ndp;

			in_sdiod = (corid == BHND_COREID_SDIOD);
			CYW_DPRINTF(sc, CYW_DBG_SDIO,
			    "EROM: core 0x%03x nmp=%u%s\n",
			    corid, nmp, in_sdiod ? " *** SDIOD ***" : "");

			/* skip master port descriptors (one word each) */
			for (int i = 0; i < nmp; i++) {
				entry = cyw_bp_read32(sc, erom_addr);
				erom_addr += 4;
				/* MPORT: valid + type 0x2 */
				while (!(entry & BCMA_EROM_ENTRY_ISVALID) ||
				    (entry & BCMA_EROM_ENTRY_TYPE_MASK) !=
				    BCMA_EROM_ENTRY_TYPE_MPORT) {
					entry = cyw_bp_read32(sc, erom_addr);
					erom_addr += 4;
				}
			}
			break;
		}
		case BCMA_EROM_ENTRY_TYPE_REGION: {
			uint8_t rtype, port, rsz;
			uint32_t base;

			rtype = (entry & BCMA_EROM_REGION_TYPE_MASK) >>
			    BCMA_EROM_REGION_TYPE_SHIFT;
			port  = (entry & BCMA_EROM_REGION_PORT_MASK) >>
			    BCMA_EROM_REGION_PORT_SHIFT;
			rsz   = (entry & BCMA_EROM_REGION_SIZE_MASK) >>
			    BCMA_EROM_REGION_SIZE_SHIFT;
			base  = entry & BCMA_EROM_REGION_BASE_MASK;

			/* consume extra size word if present */
			if (rsz == BCMA_EROM_REGION_SIZE_OTHER)
				erom_addr += 4;

			if (in_sdiod && rtype == BCMA_EROM_REGION_TYPE_DEVICE &&
			    port == 0) {
				CYW_DPRINTF(sc, CYW_DBG_SDIO,
				    "EROM: SDIOD device base=0x%08x\n", base);
				return (base);
			}
			break;
		}
		default:
			break;
		}
	}

	device_printf(sc->dev, "EROM: SDIOD core not found\n");
	return (0);
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

	/*
	 * KSO (Keep SDIO On) init — brcmf_sdio_kso_init(), sdio.c:3494.
	 * For SDIO core rev >= 12: read SLEEPCSR; if KSO bit is clear, set it.
	 * This is a one-shot read-modify-write, not a retry loop — the retry
	 * loop (brcmf_sdio_kso_control) is the runtime sleep/wake path only.
	 */
	{
		int kso_err = 0;
		uint8_t sleepcsr;

		sleepcsr = sdio_read_1(sc->f1, SBSDIO_FUNC1_SLEEPCSR, &kso_err);
		if (kso_err == 0 && !(sleepcsr & SBSDIO_FUNC1_SLEEPCSR_KSO_EN)) {
			sdio_write_1(sc->f1, SBSDIO_FUNC1_SLEEPCSR,
			    sleepcsr | SBSDIO_FUNC1_SLEEPCSR_KSO_EN, &kso_err);
			DELAY(200);
			sleepcsr = sdio_read_1(sc->f1, SBSDIO_FUNC1_SLEEPCSR,
			    &kso_err);
		}
		CYW_DPRINTF(sc, CYW_DBG_SDIO, "KSO init: SLEEPCSR=0x%02x%s\n",
		    sleepcsr,
		    (sleepcsr & SBSDIO_FUNC1_SLEEPCSR_KSO_EN)
		    ? " [KSO set]" : " [KSO NOT set]");
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
	CYW_DPRINTF(sc, CYW_DBG_SDIO, "chip 0x%04x rev %u\n",
	    sc->chip_id, sc->chip_rev);

	/* Detect SR capability: PMU chipcontrol register 3 bit 2 (chip.c:1410) */
	cyw_bp_write32(sc, CYW_SI_ENUM_BASE + CC_CHIPCTL_ADDR, 3);
	{
		uint32_t cc3 = cyw_bp_read32(sc, CYW_SI_ENUM_BASE + CC_CHIPCTL_DATA);
		sc->sr_capable = (cc3 & (1u << 2)) != 0;
		CYW_DPRINTF(sc, CYW_DBG_SDIO, "SR capable: %s (PMU CC3=0x%08x)\n",
		    sc->sr_capable ? "yes" : "no", cc3);
	}

	/* Find real SDIO device core base via EROM scan */
	sc->sdio_core_base = cyw_erom_find_sdio_core_base(sc);

	/*
	 * For ARM CR4 chips, brcmf_chip_get_raminfo() uses brcmf_chip_tcm_ramsize()
	 * (the CR4 bank scan) — not the SOCSRAM size — for firmware and NVRAM
	 * placement (brcmf_chip_get_raminfo chip.c:762-766).  NVRAM is written at
	 * rambase + tcm_ramsize - nvlen so the firmware finds it at the top of its
	 * addressable TCM.  Using the SOCSRAM size (0xdc000) instead of the CR4 TCM
	 * size (0xc8000) places NVRAM 80 KB too high, outside the firmware's scan
	 * window, so it never finds valid NVRAM and never completes SDPCM init.
	 */
	sc->ram_base = CYW_RAM_BASE;
	sc->ram_size = cyw_cr4_ram_size(sc);	/* CR4 TCM bank scan → 0xc8000 */
	device_printf(sc->dev, "RAM: base=0x%08x size=0x%x (%u KB)\n",
	    sc->ram_base, sc->ram_size, sc->ram_size / 1024);

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

	/*
	 * Leave the chip in the same passive state a power-on reset would —
	 * otherwise the still-running firmware sleeps the backplane / clears
	 * KSO after we disable the SDIO functions, and the next attach finds
	 * the chip unresponsive (every F1 access returns EIO).
	 *
	 * Mirrors Linux brcmf_sdio_remove():
	 *   1. ensure backplane (ALP) clock for the reset writes
	 *   2. settle  (Linux: msleep(20))
	 *   3. cyw_arm_halt() == brcmf_chip_cr4_set_passive():
	 *        halt ARM CR4 + reset D11 core
	 *   4. release the backplane clock  (Linux: clkctl CLK_NONE)
	 *   5. disable F2 then F1
	 */

	/* 1. backplane clock available for the passive-reset writes */
	(void)cyw_sdio_enable_clock(sc);	/* requests ALP, waits ALP_AVAIL */

	/* 2. settle (Linux msleep(20)) */
	DELAY(20000);

	/* 3. halt ARM CR4 + reset D11 — chip now passive (pre-firmware) */
	cyw_arm_halt(sc);

	/* 4. release the backplane clock request */
	sdio_write_1(sc->f1, SBSDIO_FUNC1_CHIPCLKCSR, 0, &err);

	/* 5. disable functions */
	sdio_disable_func(sc->f2);
	sdio_disable_func(sc->f1);
	sc->sdio_attached = false;
}
