/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2025 FreeBSD Contributors
 * All rights reserved.
 *
 * rp1_pcie2_recon — Milestone 3 reconnaissance
 *
 * Maps the BCM2712 PCIe2 host controller (the one connecting BCM2712 to RP1)
 * and dumps its firmware-left state to dmesg + sysctls.  Answers the question
 * from if_gem-PLAN.md §3.1: what has the VPU firmware left us?
 *
 * Physical address from FDT: /axi/pcie@1000120000
 *   reg = <0x10 0x120000 0x0 0x9310>
 *   CPU phys base = 0x1000120000, size 0x9310
 *
 * Register layout (Broadcom STB PCIe, same family as BCM2711):
 *   0x0000 - 0x0FFF  PCI configuration space (type-0 RC header)
 *   0x4000 - 0x42FF  MISC registers
 *   0x4500 - 0x45FF  MSI interrupt controller (INTR2)
 *
 * References:
 *   drivers/pci/controller/pcie-brcmstb.c  (Linux kernel)
 *   if_gem-PLAN.md §3.1
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/sysctl.h>
#include <vm/vm.h>
#include <vm/pmap.h>
#include <machine/bus.h>

/* -----------------------------------------------------------------------
 * Physical address of PCIe2 controller register window
 * ----------------------------------------------------------------------- */
#define PCIE2_BASE_PHYS		0x1000120000UL
#define PCIE2_MAP_SIZE		0x9310

/* -----------------------------------------------------------------------
 * PCI config space registers (RC Type-0 header, offset from PCIE2_BASE)
 * ----------------------------------------------------------------------- */
#define PCIE2_CFG_VENDOR_DEVICE		0x0000	/* vendor[15:0] device[31:16] */
#define PCIE2_CFG_STATUS_CMD		0x0004
#define PCIE2_CFG_CLASS_REV		0x0008	/* class[31:8] rev[7:0] */
#define PCIE2_CFG_PCIE_CAP_OFFSET	0x00ac	/* pointer to PCIe capability */
/* PCIe capability at 0x00ac (usually offset 0x00ac for BCM) */
#define PCIE2_CFG_PCIE_CAP		0x00ac
#define PCIE2_CFG_LINK_STATUS		0x00bc	/* link status: speed[19:16] width[25:20] */

/* -----------------------------------------------------------------------
 * MISC registers (offset 0x4000 from PCIE2_BASE)
 * Source: pcie-brcmstb.c
 * ----------------------------------------------------------------------- */
#define PCIE2_MISC_MISC_CTRL		0x4008
#define PCIE2_MISC_MEM_WIN0_LO		0x400c	/* CPU→PCIe outbound window 0 lo */
#define PCIE2_MISC_MEM_WIN0_HI		0x4010	/* CPU→PCIe outbound window 0 hi */
#define PCIE2_MISC_RC_BAR1_CFG_LO	0x402c	/* inbound BAR1 (DMA) lo */
#define PCIE2_MISC_RC_BAR1_CFG_HI	0x4030	/* inbound BAR1 (DMA) hi */
#define PCIE2_MISC_RC_BAR2_CFG_LO	0x4034	/* inbound BAR2 (DMA) lo */
#define PCIE2_MISC_RC_BAR2_CFG_HI	0x4038	/* inbound BAR2 (DMA) hi */
#define PCIE2_MISC_RC_BAR3_CFG_LO	0x403c	/* inbound BAR3 (DMA) lo */
#define PCIE2_MISC_RC_BAR3_CFG_HI	0x4040	/* inbound BAR3 (DMA) hi */
#define PCIE2_MISC_MSI_BAR_LO		0x4044	/* MSI target address lo */
#define PCIE2_MISC_MSI_BAR_HI		0x4048	/* MSI target address hi */
#define PCIE2_MISC_MSI_DATA		0x404c	/* MSI data */
#define PCIE2_MISC_PCIE_STATUS		0x4068	/* link/phy/DL status */
#  define PCIE_STATUS_PHYLINKUP		(1u << 4)  /* SerDes PLL locked */
#  define PCIE_STATUS_DL_ACTIVE		(1u << 5)  /* data link layer active */
#  define PCIE_STATUS_IN_L23		(1u << 6)  /* link in L23 (power-off) */
#  define PCIE_STATUS_RC_MODE		(1u << 7)  /* RC (not EP) mode */
#define PCIE2_MISC_MEM_WIN0_BASE_LIM	0x4070	/* outbound window 0 base/limit */
#define PCIE2_MISC_MEM_WIN0_BASE_HI	0x4080	/* outbound window 0 base hi */
#define PCIE2_MISC_MEM_WIN0_LIMIT_HI	0x4088	/* outbound window 0 limit hi */
#define PCIE2_MISC_HARD_DEBUG		0x4204	/* LTSSM state + SerDes debug */
#  define PCIE_HARD_DEBUG_LTSSM_MASK	0x1fu	/* bits [4:0]: LTSSM state */

/* -----------------------------------------------------------------------
 * MSI INTR2 registers (offset 0x4500)
 * ----------------------------------------------------------------------- */
#define PCIE2_MSI_INTR2_STATUS		0x4500	/* asserted MSI vectors */
#define PCIE2_MSI_INTR2_SET		0x4504
#define PCIE2_MSI_INTR2_CLR		0x4508
#define PCIE2_MSI_INTR2_MASK_SET	0x450c	/* 1 = masked */
#define PCIE2_MSI_INTR2_MASK_CLR	0x4510

/* LTSSM state names (PCIe 2.0 spec + Broadcom extension) */
static const char * const ltssm_names[] = {
	[0x00] = "DETECT.QUIET",
	[0x01] = "DETECT.ACTIVE",
	[0x02] = "POLLING.ACTIVE",
	[0x03] = "POLLING.COMPLIANCE",
	[0x04] = "POLLING.CONFIGURATION",
	[0x05] = "CONFIG.LINKWIDTHSTART",
	[0x06] = "CONFIG.LINKWIDTHACCEPT",
	[0x07] = "CONFIG.LANENUM.WAIT",
	[0x08] = "CONFIG.LANENUM.ACCEPT",
	[0x09] = "CONFIG.COMPLETE",
	[0x0a] = "CONFIG.IDLE",
	[0x0b] = "RECOVERY.RCVR.LOCK",
	[0x0c] = "RECOVERY.SPEED",
	[0x0d] = "RECOVERY.RCVR.CFG",
	[0x0e] = "RECOVERY.IDLE",
	[0x0f] = "L0",
	[0x10] = "L0s",
	[0x11] = "L1.ENTRY",
	[0x12] = "L1.IDLE",
	[0x13] = "L2.IDLE",
	[0x14] = "L2.TRANSMIT_WAKE",
	[0x15] = "DISABLED",
	[0x16] = "LOOPBACK.ENTRY",
	[0x17] = "LOOPBACK.ACTIVE",
	[0x18] = "LOOPBACK.EXIT",
	[0x19] = "HOT_RESET",
};

static void	*pcie2_kva;
static struct sysctl_ctx_list pcie2_sysctl_ctx;

#define RD4(off) (*(volatile uint32_t *)((uintptr_t)pcie2_kva + (off)))

/* -----------------------------------------------------------------------
 * sysctl: read one 32-bit register (arg2 = offset)
 * ----------------------------------------------------------------------- */
static int
pcie2_reg_sysctl(SYSCTL_HANDLER_ARGS)
{
	uint32_t off = arg2, val;

	if (pcie2_kva == NULL)
		return (ENODEV);
	val = RD4(off);
	return (sysctl_handle_int(oidp, &val, 0, req));
}

/* -----------------------------------------------------------------------
 * sysctl: decoded link status summary string
 * ----------------------------------------------------------------------- */
static int
pcie2_status_decoded_sysctl(SYSCTL_HANDLER_ARGS)
{
	uint32_t st, hd;
	int ltssm;
	char buf[128];

	if (pcie2_kva == NULL)
		return (ENODEV);

	st   = RD4(PCIE2_MISC_PCIE_STATUS);
	hd   = RD4(PCIE2_MISC_HARD_DEBUG);
	ltssm = hd & PCIE_HARD_DEBUG_LTSSM_MASK;

	snprintf(buf, sizeof(buf),
	    "phylinkup=%u dl_active=%u in_l23=%u rc_mode=%u ltssm=0x%02x(%s)",
	    (st & PCIE_STATUS_PHYLINKUP) ? 1 : 0,
	    (st & PCIE_STATUS_DL_ACTIVE) ? 1 : 0,
	    (st & PCIE_STATUS_IN_L23)    ? 1 : 0,
	    (st & PCIE_STATUS_RC_MODE)   ? 1 : 0,
	    ltssm,
	    (ltssm < (int)nitems(ltssm_names) && ltssm_names[ltssm]) ?
	        ltssm_names[ltssm] : "?");

	return (sysctl_handle_string(oidp, buf, sizeof(buf), req));
}

/* -----------------------------------------------------------------------
 * sysctl: decoded outbound window 0 summary
 * ----------------------------------------------------------------------- */
static int
pcie2_outbound_decoded_sysctl(SYSCTL_HANDLER_ARGS)
{
	uint64_t base, limit, pcie_lo;
	uint32_t bl, bhi, lhi;
	char buf[192];

	if (pcie2_kva == NULL)
		return (ENODEV);

	/*
	 * outbound window 0 maps CPU phys → PCIe bus:
	 *   CPU base  = WIN0_BASE_HI:WIN0_BASE_LIM[31:16] << 16
	 *   CPU limit = WIN0_LIMIT_HI:WIN0_BASE_LIM[15:0] << 16
	 *   PCIe dest = WIN0_HI:WIN0_LO
	 */
	pcie_lo = RD4(PCIE2_MISC_MEM_WIN0_LO);
	bl  = RD4(PCIE2_MISC_MEM_WIN0_BASE_LIM);
	bhi = RD4(PCIE2_MISC_MEM_WIN0_BASE_HI);
	lhi = RD4(PCIE2_MISC_MEM_WIN0_LIMIT_HI);

	base  = ((uint64_t)bhi << 32) | ((uint64_t)(bl >> 16) << 16);
	limit = ((uint64_t)lhi << 32) | ((uint64_t)(bl & 0xffff) << 16) | 0xffff;

	snprintf(buf, sizeof(buf),
	    "cpu_phys 0x%llx-0x%llx → pcie 0x%llx "
	    "(lo_reg=0x%08x hi_reg=0x%08x bl=0x%08x)",
	    (unsigned long long)base, (unsigned long long)limit,
	    (unsigned long long)(((uint64_t)RD4(PCIE2_MISC_MEM_WIN0_HI) << 32) |
	    pcie_lo),
	    (unsigned)pcie_lo, (unsigned)RD4(PCIE2_MISC_MEM_WIN0_HI),
	    (unsigned)bl);

	return (sysctl_handle_string(oidp, buf, sizeof(buf), req));
}

/* -----------------------------------------------------------------------
 * sysctl: decoded inbound DMA BAR2 (host DRAM visible to RP1)
 * ----------------------------------------------------------------------- */
static int
pcie2_inbound_decoded_sysctl(SYSCTL_HANDLER_ARGS)
{
	uint64_t bar2_lo, bar2_hi, bar2_pcie;
	uint32_t size_encoded;
	char buf[192];

	if (pcie2_kva == NULL)
		return (ENODEV);

	bar2_lo  = RD4(PCIE2_MISC_RC_BAR2_CFG_LO);
	bar2_hi  = RD4(PCIE2_MISC_RC_BAR2_CFG_HI);

	/*
	 * BAR config format (pcie-brcmstb.c brcm_pcie_set_rc_bar2):
	 *   lo[3:1] = size exponent (actual_size = 1 << (size_exp + 15))
	 *   lo[31:4] | hi = PCIe address of inbound window
	 */
	size_encoded = (bar2_lo >> 1) & 0x1f;
	bar2_pcie = ((uint64_t)bar2_hi << 32) | (bar2_lo & ~0xfUL);

	snprintf(buf, sizeof(buf),
	    "pcie_addr=0x%llx size_exp=%u (~%llu MB) lo=0x%08x hi=0x%08x",
	    (unsigned long long)bar2_pcie,
	    size_encoded,
	    (unsigned long long)(1ULL << (size_encoded + 15)) >> 20,
	    (unsigned)bar2_lo, (unsigned)bar2_hi);

	return (sysctl_handle_string(oidp, buf, sizeof(buf), req));
}

/* -----------------------------------------------------------------------
 * sysctl: MSI configuration summary
 * ----------------------------------------------------------------------- */
static int
pcie2_msi_decoded_sysctl(SYSCTL_HANDLER_ARGS)
{
	uint64_t msi_addr;
	uint32_t msi_data, msi_mask;
	char buf[192];

	if (pcie2_kva == NULL)
		return (ENODEV);

	msi_addr = ((uint64_t)RD4(PCIE2_MISC_MSI_BAR_HI) << 32) |
	    RD4(PCIE2_MISC_MSI_BAR_LO);
	msi_data = RD4(PCIE2_MISC_MSI_DATA);
	msi_mask = RD4(PCIE2_MSI_INTR2_MASK_SET);

	snprintf(buf, sizeof(buf),
	    "target_addr=0x%llx data=0x%08x intr2_mask=0x%08x "
	    "intr2_status=0x%08x",
	    (unsigned long long)msi_addr, msi_data, msi_mask,
	    (unsigned)RD4(PCIE2_MSI_INTR2_STATUS));

	return (sysctl_handle_string(oidp, buf, sizeof(buf), req));
}

/* -----------------------------------------------------------------------
 * Module event handler
 * ----------------------------------------------------------------------- */
static int
rp1_pcie2_recon_modevent(module_t mod __unused, int event, void *arg __unused)
{
	struct sysctl_oid *tree, *misc_tree, *msi_tree;
	uint32_t st, hd, vid_did, bar2lo, bar2hi;
	uint64_t bar2_pcie;
	int ltssm;

	switch (event) {
	case MOD_LOAD:
		pcie2_kva = pmap_mapdev_attr(PCIE2_BASE_PHYS, PCIE2_MAP_SIZE,
		    VM_MEMATTR_DEVICE);
		if (pcie2_kva == NULL) {
			printf("rp1_pcie2_recon: cannot map 0x%lx\n",
			    PCIE2_BASE_PHYS);
			return (ENXIO);
		}
		printf("rp1_pcie2_recon: PCIe2 mapped at phys 0x%lx KVA %p\n",
		    PCIE2_BASE_PHYS, pcie2_kva);

		/* ---- Snapshot dump to dmesg ---- */
		vid_did = RD4(PCIE2_CFG_VENDOR_DEVICE);
		st      = RD4(PCIE2_MISC_PCIE_STATUS);
		hd      = RD4(PCIE2_MISC_HARD_DEBUG);
		ltssm   = hd & PCIE_HARD_DEBUG_LTSSM_MASK;
		bar2lo  = RD4(PCIE2_MISC_RC_BAR2_CFG_LO);
		bar2hi  = RD4(PCIE2_MISC_RC_BAR2_CFG_HI);
		bar2_pcie = ((uint64_t)bar2hi << 32) | (bar2lo & ~0xfUL);

		printf("rp1_pcie2_recon: cfg vendor:device = 0x%04x:0x%04x\n",
		    vid_did & 0xffff, vid_did >> 16);
		printf("rp1_pcie2_recon: PCIE_STATUS = 0x%08x "
		    "(phylinkup=%u dl_active=%u in_l23=%u rc_mode=%u)\n",
		    st,
		    (st & PCIE_STATUS_PHYLINKUP) ? 1 : 0,
		    (st & PCIE_STATUS_DL_ACTIVE) ? 1 : 0,
		    (st & PCIE_STATUS_IN_L23)    ? 1 : 0,
		    (st & PCIE_STATUS_RC_MODE)   ? 1 : 0);
		printf("rp1_pcie2_recon: HARD_DEBUG = 0x%08x "
		    "(LTSSM=0x%02x = %s)\n",
		    hd, ltssm,
		    (ltssm < (int)nitems(ltssm_names) && ltssm_names[ltssm]) ?
		        ltssm_names[ltssm] : "?");
		printf("rp1_pcie2_recon: outbound WIN0_LO=0x%08x WIN0_HI=0x%08x "
		    "BASE_LIM=0x%08x\n",
		    RD4(PCIE2_MISC_MEM_WIN0_LO),
		    RD4(PCIE2_MISC_MEM_WIN0_HI),
		    RD4(PCIE2_MISC_MEM_WIN0_BASE_LIM));
		printf("rp1_pcie2_recon: inbound BAR2 pcie_addr=0x%llx "
		    "(lo=0x%08x hi=0x%08x)\n",
		    (unsigned long long)bar2_pcie, bar2lo, bar2hi);
		printf("rp1_pcie2_recon: MSI bar=0x%llx data=0x%08x "
		    "intr2_mask=0x%08x intr2_status=0x%08x\n",
		    (unsigned long long)(((uint64_t)RD4(PCIE2_MISC_MSI_BAR_HI) << 32) |
		        RD4(PCIE2_MISC_MSI_BAR_LO)),
		    RD4(PCIE2_MISC_MSI_DATA),
		    RD4(PCIE2_MSI_INTR2_MASK_SET),
		    RD4(PCIE2_MSI_INTR2_STATUS));

		/* ---- Sysctl tree ---- */
		sysctl_ctx_init(&pcie2_sysctl_ctx);
		tree = SYSCTL_ADD_NODE(&pcie2_sysctl_ctx,
		    SYSCTL_STATIC_CHILDREN(_hw),
		    OID_AUTO, "rp1_pcie2",
		    CTLFLAG_RD | CTLFLAG_MPSAFE, 0,
		    "BCM2712 PCIe2 controller (RP1 host)");

		SYSCTL_ADD_PROC(&pcie2_sysctl_ctx,
		    SYSCTL_CHILDREN(tree), OID_AUTO, "status_decoded",
		    CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_MPSAFE,
		    NULL, 0, pcie2_status_decoded_sysctl, "A",
		    "Decoded link/LTSSM status");
		SYSCTL_ADD_PROC(&pcie2_sysctl_ctx,
		    SYSCTL_CHILDREN(tree), OID_AUTO, "outbound_win0",
		    CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_MPSAFE,
		    NULL, 0, pcie2_outbound_decoded_sysctl, "A",
		    "Decoded outbound window 0 (CPU→PCIe)");
		SYSCTL_ADD_PROC(&pcie2_sysctl_ctx,
		    SYSCTL_CHILDREN(tree), OID_AUTO, "inbound_bar2",
		    CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_MPSAFE,
		    NULL, 0, pcie2_inbound_decoded_sysctl, "A",
		    "Decoded inbound BAR2 (PCIe→host DRAM for RP1 DMA)");
		SYSCTL_ADD_PROC(&pcie2_sysctl_ctx,
		    SYSCTL_CHILDREN(tree), OID_AUTO, "msi",
		    CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_MPSAFE,
		    NULL, 0, pcie2_msi_decoded_sysctl, "A",
		    "Decoded MSI controller state");

		misc_tree = SYSCTL_ADD_NODE(&pcie2_sysctl_ctx,
		    SYSCTL_CHILDREN(tree), OID_AUTO, "regs",
		    CTLFLAG_RD | CTLFLAG_MPSAFE, 0, "Raw MISC registers");

#define ADD_REG(name_, off_, desc_)					\
	SYSCTL_ADD_PROC(&pcie2_sysctl_ctx,				\
	    SYSCTL_CHILDREN(misc_tree), OID_AUTO, (name_),		\
	    CTLTYPE_UINT | CTLFLAG_RD | CTLFLAG_MPSAFE,			\
	    NULL, (off_), pcie2_reg_sysctl, "IU", (desc_))

		ADD_REG("cfg_vid_did",    PCIE2_CFG_VENDOR_DEVICE,
		    "Config vendor:device ID");
		ADD_REG("misc_ctrl",      PCIE2_MISC_MISC_CTRL,
		    "MISC_CTRL: DMA enable, endian swap");
		ADD_REG("pcie_status",    PCIE2_MISC_PCIE_STATUS,
		    "PCIE_STATUS: phylinkup/dl_active/L23");
		ADD_REG("hard_debug",     PCIE2_MISC_HARD_DEBUG,
		    "HARD_DEBUG: LTSSM[4:0]");
		ADD_REG("win0_lo",        PCIE2_MISC_MEM_WIN0_LO,
		    "Outbound win0 PCIe dest lo");
		ADD_REG("win0_hi",        PCIE2_MISC_MEM_WIN0_HI,
		    "Outbound win0 PCIe dest hi");
		ADD_REG("win0_base_lim",  PCIE2_MISC_MEM_WIN0_BASE_LIM,
		    "Outbound win0 CPU base/limit");
		ADD_REG("win0_base_hi",   PCIE2_MISC_MEM_WIN0_BASE_HI,
		    "Outbound win0 CPU base hi");
		ADD_REG("win0_limit_hi",  PCIE2_MISC_MEM_WIN0_LIMIT_HI,
		    "Outbound win0 CPU limit hi");
		ADD_REG("rc_bar1_lo",     PCIE2_MISC_RC_BAR1_CFG_LO,
		    "Inbound BAR1 config lo");
		ADD_REG("rc_bar1_hi",     PCIE2_MISC_RC_BAR1_CFG_HI,
		    "Inbound BAR1 config hi");
		ADD_REG("rc_bar2_lo",     PCIE2_MISC_RC_BAR2_CFG_LO,
		    "Inbound BAR2 config lo (host DRAM)");
		ADD_REG("rc_bar2_hi",     PCIE2_MISC_RC_BAR2_CFG_HI,
		    "Inbound BAR2 config hi (host DRAM)");
		ADD_REG("rc_bar3_lo",     PCIE2_MISC_RC_BAR3_CFG_LO,
		    "Inbound BAR3 config lo");
		ADD_REG("rc_bar3_hi",     PCIE2_MISC_RC_BAR3_CFG_HI,
		    "Inbound BAR3 config hi");
		ADD_REG("msi_bar_lo",     PCIE2_MISC_MSI_BAR_LO,
		    "MSI target address lo");
		ADD_REG("msi_bar_hi",     PCIE2_MISC_MSI_BAR_HI,
		    "MSI target address hi");
		ADD_REG("msi_data",       PCIE2_MISC_MSI_DATA,
		    "MSI data value");

		msi_tree = SYSCTL_ADD_NODE(&pcie2_sysctl_ctx,
		    SYSCTL_CHILDREN(tree), OID_AUTO, "msi_intr2",
		    CTLFLAG_RD | CTLFLAG_MPSAFE, 0, "MSI INTR2 controller");
		ADD_REG("status",  PCIE2_MSI_INTR2_STATUS,  "Pending MSI vectors");
		ADD_REG("mask",    PCIE2_MSI_INTR2_MASK_SET,"Masked MSI vectors");
#undef ADD_REG

		(void)msi_tree;
		printf("rp1_pcie2_recon: sysctls at hw.rp1_pcie2.*\n");
		break;

	case MOD_UNLOAD:
		sysctl_ctx_free(&pcie2_sysctl_ctx);
		if (pcie2_kva != NULL) {
			pmap_unmapdev(pcie2_kva, PCIE2_MAP_SIZE);
			pcie2_kva = NULL;
		}
		printf("rp1_pcie2_recon: unloaded\n");
		break;

	default:
		return (EOPNOTSUPP);
	}
	return (0);
}

static moduledata_t rp1_pcie2_recon_mdata = {
	"rp1_pcie2_recon",
	rp1_pcie2_recon_modevent,
	NULL
};

DECLARE_MODULE(rp1_pcie2_recon, rp1_pcie2_recon_mdata,
    SI_SUB_DRIVERS, SI_ORDER_ANY);
MODULE_VERSION(rp1_pcie2_recon, 1);
