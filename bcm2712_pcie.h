/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * bcm2712_pcie.h — KPI for rp1_eth to register its interrupt filter.
 *
 * Include this in rp1_eth_cfg.c (M1 module init) or rp1_eth.c (M2/M3)
 * to call bcm2712_pcie_register_rp1_intr() during attach and
 * bcm2712_pcie_deregister_rp1_intr() during detach.
 */

#ifndef _BCM2712_PCIE_H_
#define _BCM2712_PCIE_H_

#include <sys/bus.h>

void	bcm2712_pcie_register_rp1_intr(driver_filter_t *filter, void *arg);
void	bcm2712_pcie_deregister_rp1_intr(void);

#endif /* _BCM2712_PCIE_H_ */
