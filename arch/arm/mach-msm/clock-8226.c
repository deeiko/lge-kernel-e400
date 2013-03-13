/* Copyright (c) 2013, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/err.h>
#include <linux/ctype.h>
#include <linux/io.h>
#include <linux/spinlock.h>
#include <linux/delay.h>
#include <linux/clk.h>
#include <linux/iopoll.h>
#include <linux/regulator/consumer.h>

#include <mach/rpm-regulator-smd.h>
#include <mach/socinfo.h>
#include <mach/rpm-smd.h>

#include "clock-local2.h"
#include "clock-pll.h"
#include "clock-rpm.h"
#include "clock-voter.h"
#include "clock-mdss-8974.h"
#include "clock.h"

enum {
	GCC_BASE,
	MMSS_BASE,
	LPASS_BASE,
	APCS_BASE,
	APCS_PLL_BASE,
	N_BASES,
};

static void __iomem *virt_bases[N_BASES];

#define GCC_REG_BASE(x) (void __iomem *)(virt_bases[GCC_BASE] + (x))
#define MMSS_REG_BASE(x) (void __iomem *)(virt_bases[MMSS_BASE] + (x))
#define LPASS_REG_BASE(x) (void __iomem *)(virt_bases[LPASS_BASE] + (x))
#define APCS_REG_BASE(x) (void __iomem *)(virt_bases[APCS_BASE] + (x))

/* Mux source select values */
#define xo_source_val  0
#define gpll0_source_val 1
#define gpll1_source_val 2

#define xo_mm_source_val 0
#define mmpll0_pll_mm_source_val 1
#define mmpll1_pll_mm_source_val 2
#define mmpll2_pll_mm_source_val 3
#define gpll0_mm_source_val 5
#define dsipll_750_mm_source_val 1
#define dsipll_667_mm_source_val 1

#define gpll1_hsic_source_val 4

#define xo_lpass_source_val 0
#define lpaaudio_pll_lpass_source_val 1
#define gpll0_lpass_source_val 5

/* Prevent a divider of -1 */
#define FIXDIV(div) (div ? (2 * (div) - 1) : (0))

#define F_GCC(f, s, div, m, n) \
	{ \
		.freq_hz = (f), \
		.src_clk = &s.c, \
		.m_val = (m), \
		.n_val = ~((n)-(m)) * !!(n), \
		.d_val = ~(n),\
		.div_src_val = BVAL(4, 0, (int)(FIXDIV(div))) \
			| BVAL(10, 8, s##_source_val), \
	}

#define F_MMSS(f, s, div, m, n) \
	{ \
		.freq_hz = (f), \
		.src_clk = &s.c, \
		.m_val = (m), \
		.n_val = ~((n)-(m)) * !!(n), \
		.d_val = ~(n),\
		.div_src_val = BVAL(4, 0, (int)(FIXDIV(div))) \
			| BVAL(10, 8, s##_mm_source_val), \
	}

#define F_MDSS(f, s, div, m, n) \
	{ \
		.freq_hz = (f), \
		.m_val = (m), \
		.n_val = ~((n)-(m)) * !!(n), \
		.d_val = ~(n),\
		.div_src_val = BVAL(4, 0, (int)(FIXDIV(div))) \
			| BVAL(10, 8, s##_mm_source_val), \
	}

#define F_HSIC(f, s, div, m, n) \
	{ \
		.freq_hz = (f), \
		.src_clk = &s.c, \
		.m_val = (m), \
		.n_val = ~((n)-(m)) * !!(n), \
		.d_val = ~(n),\
		.div_src_val = BVAL(4, 0, (int)(FIXDIV(div))) \
			| BVAL(10, 8, s##_hsic_source_val), \
	}

#define F_LPASS(f, s, div, m, n) \
	{ \
		.freq_hz = (f), \
		.src_clk = &s.c, \
		.m_val = (m), \
		.n_val = ~((n)-(m)) * !!(n), \
		.d_val = ~(n),\
		.div_src_val = BVAL(4, 0, (int)(FIXDIV(div))) \
			| BVAL(10, 8, s##_lpass_source_val), \
	}

#define F_APCS_PLL(f, l, m, n, pre_div, post_div, vco) \
	{ \
		.freq_hz = (f), \
		.l_val = (l), \
		.m_val = (m), \
		.n_val = (n), \
		.pre_div_val = BVAL(12, 12, (pre_div)), \
		.post_div_val = BVAL(9, 8, (post_div)), \
		.vco_val = BVAL(29, 28, (vco)), \
	}

#define VDD_DIG_FMAX_MAP1(l1, f1) \
	.vdd_class = &vdd_dig, \
	.fmax = (unsigned long[VDD_DIG_NUM]) {  \
		[VDD_DIG_##l1] = (f1),          \
	},                                      \
	.num_fmax = VDD_DIG_NUM

#define VDD_DIG_FMAX_MAP2(l1, f1, l2, f2) \
	.vdd_class = &vdd_dig, \
	.fmax = (unsigned long[VDD_DIG_NUM]) {  \
		[VDD_DIG_##l1] = (f1),          \
		[VDD_DIG_##l2] = (f2),          \
	},                                      \
	.num_fmax = VDD_DIG_NUM

#define VDD_DIG_FMAX_MAP3(l1, f1, l2, f2, l3, f3) \
	.vdd_class = &vdd_dig, \
	.fmax = (unsigned long[VDD_DIG_NUM]) {  \
		[VDD_DIG_##l1] = (f1),          \
		[VDD_DIG_##l2] = (f2),          \
		[VDD_DIG_##l3] = (f3),          \
	},                                      \
	.num_fmax = VDD_DIG_NUM

enum vdd_dig_levels {
	VDD_DIG_NONE,
	VDD_DIG_LOW,
	VDD_DIG_NOMINAL,
	VDD_DIG_HIGH,
	VDD_DIG_NUM
};

static const int vdd_corner[] = {
	[VDD_DIG_NONE]	  = RPM_REGULATOR_CORNER_NONE,
	[VDD_DIG_LOW]	  = RPM_REGULATOR_CORNER_SVS_SOC,
	[VDD_DIG_NOMINAL] = RPM_REGULATOR_CORNER_NORMAL,
	[VDD_DIG_HIGH]	  = RPM_REGULATOR_CORNER_SUPER_TURBO,
};

static struct regulator *vdd_dig_reg;

static int set_vdd_dig(struct clk_vdd_class *vdd_class, int level)
{
	return regulator_set_voltage(vdd_dig_reg, vdd_corner[level],
					RPM_REGULATOR_CORNER_SUPER_TURBO);
}

static DEFINE_VDD_CLASS(vdd_dig, set_vdd_dig, VDD_DIG_NUM);

#define RPM_MISC_CLK_TYPE	0x306b6c63
#define RPM_BUS_CLK_TYPE	0x316b6c63
#define RPM_MEM_CLK_TYPE	0x326b6c63

#define RPM_SMD_KEY_ENABLE	0x62616E45

#define CXO_ID			0x0
#define QDSS_ID			0x1

#define PNOC_ID		0x0
#define SNOC_ID		0x1
#define CNOC_ID		0x2
#define MMSSNOC_AHB_ID  0x3

#define BIMC_ID		0x0
#define OXILI_ID	0x1
#define OCMEM_ID	0x2

#define D0_ID		 1
#define D1_ID		 2
#define A0_ID		 4
#define A1_ID		 5
#define A2_ID		 6
#define DIFF_CLK_ID	 7
#define DIV_CLK1_ID	11
#define DIV_CLK2_ID	12

DEFINE_CLK_RPM_SMD(pnoc_clk, pnoc_a_clk, RPM_BUS_CLK_TYPE, PNOC_ID, NULL);
DEFINE_CLK_RPM_SMD(snoc_clk, snoc_a_clk, RPM_BUS_CLK_TYPE, SNOC_ID, NULL);
DEFINE_CLK_RPM_SMD(cnoc_clk, cnoc_a_clk, RPM_BUS_CLK_TYPE, CNOC_ID, NULL);
DEFINE_CLK_RPM_SMD(mmssnoc_ahb_clk, mmssnoc_ahb_a_clk, RPM_BUS_CLK_TYPE,
			MMSSNOC_AHB_ID, NULL);

DEFINE_CLK_RPM_SMD(bimc_clk, bimc_a_clk, RPM_MEM_CLK_TYPE, BIMC_ID, NULL);
DEFINE_CLK_RPM_SMD(ocmemgx_clk, ocmemgx_a_clk, RPM_MEM_CLK_TYPE, OCMEM_ID,
			NULL);
DEFINE_CLK_RPM_SMD(gfx3d_clk_src, gfx3d_a_clk_src, RPM_MEM_CLK_TYPE, OXILI_ID,
			NULL);

DEFINE_CLK_RPM_SMD_BRANCH(xo, xo_a_clk,
				RPM_MISC_CLK_TYPE, CXO_ID, 19200000);
DEFINE_CLK_RPM_SMD_QDSS(qdss_clk, qdss_a_clk, RPM_MISC_CLK_TYPE, QDSS_ID);

DEFINE_CLK_RPM_SMD_XO_BUFFER(cxo_d0, cxo_d0_a, D0_ID);
DEFINE_CLK_RPM_SMD_XO_BUFFER(cxo_d1, cxo_d1_a, D1_ID);
DEFINE_CLK_RPM_SMD_XO_BUFFER(cxo_a0, cxo_a0_a, A0_ID);
DEFINE_CLK_RPM_SMD_XO_BUFFER(cxo_a1, cxo_a1_a, A1_ID);
DEFINE_CLK_RPM_SMD_XO_BUFFER(cxo_a2, cxo_a2_a, A2_ID);
DEFINE_CLK_RPM_SMD_XO_BUFFER(div_clk1, div_a_clk1, DIV_CLK1_ID);
DEFINE_CLK_RPM_SMD_XO_BUFFER(div_clk2, div_a_clk2, DIV_CLK2_ID);
DEFINE_CLK_RPM_SMD_XO_BUFFER(diff_clk, diff_a_clk, DIFF_CLK_ID);

DEFINE_CLK_RPM_SMD_XO_BUFFER_PINCTRL(cxo_d0_pin, cxo_d0_a_pin, D0_ID);
DEFINE_CLK_RPM_SMD_XO_BUFFER_PINCTRL(cxo_d1_pin, cxo_d1_a_pin, D1_ID);
DEFINE_CLK_RPM_SMD_XO_BUFFER_PINCTRL(cxo_a0_pin, cxo_a0_a_pin, A0_ID);
DEFINE_CLK_RPM_SMD_XO_BUFFER_PINCTRL(cxo_a1_pin, cxo_a1_a_pin, A1_ID);
DEFINE_CLK_RPM_SMD_XO_BUFFER_PINCTRL(cxo_a2_pin, cxo_a2_a_pin, A2_ID);

struct measure_mux_entry {
	struct clk *c;
	int base;
	u32 debug_mux;
};

static struct branch_clk oxilicx_axi_clk;

#define MSS_DEBUG_CLOCK_CTL  0x0078
#define LPASS_DEBUG_CLK_CTL  0x29000
#define GLB_CLK_DIAG    0x01C
#define GLB_TEST_BUS_SEL    0x020

#define MMPLL0_PLL_MODE                                    (0x0000)
#define MMPLL0_PLL_L_VAL                                   (0x0004)
#define MMPLL0_PLL_M_VAL                                   (0x0008)
#define MMPLL0_PLL_N_VAL                                   (0x000C)
#define MMPLL0_PLL_USER_CTL                                (0x0010)
#define MMPLL0_PLL_STATUS                                  (0x001C)
#define MMPLL1_PLL_MODE                                    (0x0040)
#define MMPLL1_PLL_L_VAL                                   (0x0044)
#define MMPLL1_PLL_M_VAL                                   (0x0048)
#define MMPLL1_PLL_N_VAL                                   (0x004C)
#define MMPLL1_PLL_USER_CTL                                (0x0050)
#define MMPLL1_PLL_STATUS                                  (0x005C)
#define MMSS_PLL_VOTE_APCS                                 (0x0100)
#define VCODEC0_CMD_RCGR                                   (0x1000)
#define VENUS0_VCODEC0_CBCR                                (0x1028)
#define VENUS0_AHB_CBCR                                    (0x1030)
#define VENUS0_AXI_CBCR                                    (0x1034)
#define PCLK0_CMD_RCGR                                     (0x2000)
#define MDP_CMD_RCGR                                       (0x2040)
#define VSYNC_CMD_RCGR                                     (0x2080)
#define BYTE0_CMD_RCGR                                     (0x2120)
#define ESC0_CMD_RCGR                                      (0x2160)
#define MDSS_AHB_CBCR                                      (0x2308)
#define MDSS_AXI_CBCR                                      (0x2310)
#define MDSS_PCLK0_CBCR                                    (0x2314)
#define MDSS_MDP_CBCR                                      (0x231C)
#define MDSS_MDP_LUT_CBCR                                  (0x2320)
#define MDSS_VSYNC_CBCR                                    (0x2328)
#define MDSS_BYTE0_CBCR                                    (0x233C)
#define MDSS_ESC0_CBCR                                     (0x2344)
#define CSI0PHYTIMER_CMD_RCGR                              (0x3000)
#define CAMSS_PHY0_CSI0PHYTIMER_CBCR                       (0x3024)
#define CSI1PHYTIMER_CMD_RCGR                              (0x3030)
#define CAMSS_PHY1_CSI1PHYTIMER_CBCR                       (0x3054)
#define CSI0_CMD_RCGR                                      (0x3090)
#define CAMSS_CSI0_CBCR                                    (0x30B4)
#define CAMSS_CSI0_AHB_CBCR                                (0x30BC)
#define CAMSS_CSI0PHY_CBCR                                 (0x30C4)
#define CAMSS_CSI0RDI_CBCR                                 (0x30D4)
#define CAMSS_CSI0PIX_CBCR                                 (0x30E4)
#define CSI1_CMD_RCGR                                      (0x3100)
#define CAMSS_CSI1_CBCR                                    (0x3124)
#define CAMSS_CSI1_AHB_CBCR                                (0x3128)
#define CAMSS_CSI1PHY_CBCR                                 (0x3134)
#define CAMSS_CSI1RDI_CBCR                                 (0x3144)
#define CAMSS_CSI1PIX_CBCR                                 (0x3154)
#define CAMSS_ISPIF_AHB_CBCR                               (0x3224)
#define CCI_CMD_RCGR                                       (0x3300)
#define CAMSS_CCI_CCI_CBCR                                 (0x3344)
#define CAMSS_CCI_CCI_AHB_CBCR                             (0x3348)
#define MCLK0_CMD_RCGR                                     (0x3360)
#define CAMSS_MCLK0_CBCR                                   (0x3384)
#define MCLK1_CMD_RCGR                                     (0x3390)
#define CAMSS_MCLK1_CBCR                                   (0x33B4)
#define MMSS_GP0_CMD_RCGR                                  (0x3420)
#define CAMSS_GP0_CBCR                                     (0x3444)
#define MMSS_GP1_CMD_RCGR                                  (0x3450)
#define CAMSS_GP1_CBCR                                     (0x3474)
#define CAMSS_TOP_AHB_CBCR                                 (0x3484)
#define CAMSS_MICRO_AHB_CBCR                               (0x3494)
#define JPEG0_CMD_RCGR                                     (0x3500)
#define CAMSS_JPEG_JPEG0_CBCR                              (0x35A8)
#define CAMSS_JPEG_JPEG_AHB_CBCR                           (0x35B4)
#define CAMSS_JPEG_JPEG_AXI_CBCR                           (0x35B8)
#define VFE0_CMD_RCGR                                      (0x3600)
#define CPP_CMD_RCGR                                       (0x3640)
#define CAMSS_VFE_VFE0_CBCR                                (0x36A8)
#define CAMSS_VFE_CPP_CBCR                                 (0x36B0)
#define CAMSS_VFE_CPP_AHB_CBCR                             (0x36B4)
#define CAMSS_VFE_VFE_AHB_CBCR                             (0x36B8)
#define CAMSS_VFE_VFE_AXI_CBCR                             (0x36BC)
#define CAMSS_CSI_VFE0_CBCR                                (0x3704)
#define OXILI_GFX3D_CBCR                                   (0x4028)
#define OXILICX_AXI_CBCR                                   (0x4038)
#define OXILICX_AHB_CBCR                                   (0x403C)
#define OCMEMCX_AHB_CBCR                                   (0x405C)
#define MMPLL2_PLL_MODE                                    (0x4100)
#define MMPLL2_PLL_STATUS                                  (0x411C)
#define MMSS_MMSSNOC_AHB_CBCR                              (0x5024)
#define MMSS_MMSSNOC_BTO_AHB_CBCR                          (0x5028)
#define MMSS_MISC_AHB_CBCR                                 (0x502C)
#define AXI_CMD_RCGR                                       (0x5040)
#define MMSS_S0_AXI_CBCR                                   (0x5064)
#define MMSS_MMSSNOC_AXI_CBCR                              (0x506C)
#define MMSS_DEBUG_CLK_CTL                                 (0x0900)
#define GPLL0_MODE                                         (0x0000)
#define GPLL0_L_VAL                                        (0x0004)
#define GPLL0_M_VAL                                        (0x0008)
#define GPLL0_N_VAL                                        (0x000C)
#define GPLL0_USER_CTL                                     (0x0010)
#define GPLL0_STATUS                                       (0x001C)
#define GPLL1_MODE                                         (0x0040)
#define GPLL1_L_VAL                                        (0x0044)
#define GPLL1_M_VAL                                        (0x0048)
#define GPLL1_N_VAL                                        (0x004C)
#define GPLL1_USER_CTL                                     (0x0050)
#define GPLL1_STATUS                                       (0x005C)
#define PERIPH_NOC_AHB_CBCR                                (0x0184)
#define NOC_CONF_XPU_AHB_CBCR                              (0x01C0)
#define MMSS_NOC_CFG_AHB_CBCR                              (0x024C)
#define MSS_CFG_AHB_CBCR                                   (0x0280)
#define MSS_Q6_BIMC_AXI_CBCR                               (0x0284)
#define USB_HS_HSIC_BCR                                    (0x0400)
#define USB_HSIC_AHB_CBCR                                  (0x0408)
#define USB_HSIC_SYSTEM_CMD_RCGR                           (0x041C)
#define USB_HSIC_SYSTEM_CBCR                               (0x040C)
#define USB_HSIC_CMD_RCGR                                  (0x0440)
#define USB_HSIC_CBCR                                      (0x0410)
#define USB_HSIC_IO_CAL_CMD_RCGR                           (0x0458)
#define USB_HSIC_IO_CAL_CBCR                               (0x0414)
#define USB_HS_BCR                                         (0x0480)
#define USB_HS_SYSTEM_CBCR                                 (0x0484)
#define USB_HS_AHB_CBCR                                    (0x0488)
#define USB_HS_SYSTEM_CMD_RCGR                             (0x0490)
#define USB2A_PHY_SLEEP_CBCR                               (0x04AC)
#define SDCC1_APPS_CMD_RCGR                                (0x04D0)
#define SDCC1_APPS_CBCR                                    (0x04C4)
#define SDCC1_AHB_CBCR                                     (0x04C8)
#define SDCC2_APPS_CMD_RCGR                                (0x0510)
#define SDCC2_APPS_CBCR                                    (0x0504)
#define SDCC2_AHB_CBCR                                     (0x0508)
#define SDCC3_APPS_CMD_RCGR                                (0x0550)
#define SDCC3_APPS_CBCR                                    (0x0544)
#define SDCC3_AHB_CBCR                                     (0x0548)
#define BLSP1_AHB_CBCR                                     (0x05C4)
#define BLSP1_QUP1_SPI_APPS_CBCR                           (0x0644)
#define BLSP1_QUP1_I2C_APPS_CBCR                           (0x0648)
#define BLSP1_QUP1_I2C_APPS_CMD_RCGR                       (0x0660)
#define BLSP1_QUP2_I2C_APPS_CMD_RCGR                       (0x06E0)
#define BLSP1_QUP3_I2C_APPS_CMD_RCGR                       (0x0760)
#define BLSP1_QUP4_I2C_APPS_CMD_RCGR                       (0x07E0)
#define BLSP1_QUP5_I2C_APPS_CMD_RCGR                       (0x0860)
#define BLSP1_QUP6_I2C_APPS_CMD_RCGR                       (0x08E0)
#define BLSP1_QUP1_SPI_APPS_CMD_RCGR                       (0x064C)
#define BLSP1_UART1_APPS_CBCR                              (0x0684)
#define BLSP1_UART1_APPS_CMD_RCGR                          (0x068C)
#define BLSP1_QUP2_SPI_APPS_CBCR                           (0x06C4)
#define BLSP1_QUP2_I2C_APPS_CBCR                           (0x06C8)
#define BLSP1_QUP2_SPI_APPS_CMD_RCGR                       (0x06CC)
#define BLSP1_UART2_APPS_CBCR                              (0x0704)
#define BLSP1_UART2_APPS_CMD_RCGR                          (0x070C)
#define BLSP1_QUP3_SPI_APPS_CBCR                           (0x0744)
#define BLSP1_QUP3_I2C_APPS_CBCR                           (0x0748)
#define BLSP1_QUP3_SPI_APPS_CMD_RCGR                       (0x074C)
#define BLSP1_UART3_APPS_CBCR                              (0x0784)
#define BLSP1_UART3_APPS_CMD_RCGR                          (0x078C)
#define BLSP1_QUP4_SPI_APPS_CBCR                           (0x07C4)
#define BLSP1_QUP4_I2C_APPS_CBCR                           (0x07C8)
#define BLSP1_QUP4_SPI_APPS_CMD_RCGR                       (0x07CC)
#define BLSP1_UART4_APPS_CBCR                              (0x0804)
#define BLSP1_UART4_APPS_CMD_RCGR                          (0x080C)
#define BLSP1_QUP5_SPI_APPS_CBCR                           (0x0844)
#define BLSP1_QUP5_I2C_APPS_CBCR                           (0x0848)
#define BLSP1_QUP5_SPI_APPS_CMD_RCGR                       (0x084C)
#define BLSP1_UART5_APPS_CBCR                              (0x0884)
#define BLSP1_UART5_APPS_CMD_RCGR                          (0x088C)
#define BLSP1_QUP6_SPI_APPS_CBCR                           (0x08C4)
#define BLSP1_QUP6_I2C_APPS_CBCR                           (0x08C8)
#define BLSP1_QUP6_SPI_APPS_CMD_RCGR                       (0x08CC)
#define BLSP1_UART6_APPS_CBCR                              (0x0904)
#define BLSP1_UART6_APPS_CMD_RCGR                          (0x090C)
#define PDM_AHB_CBCR                                       (0x0CC4)
#define PDM_XO4_CBCR                                       (0x0CC8)
#define PDM2_CBCR                                          (0x0CCC)
#define PDM2_CMD_RCGR                                      (0x0CD0)
#define PRNG_AHB_CBCR                                      (0x0D04)
#define BAM_DMA_AHB_CBCR                                   (0x0D44)
#define BOOT_ROM_AHB_CBCR                                  (0x0E04)
#define CE1_CMD_RCGR                                       (0x1050)
#define CE1_CBCR                                           (0x1044)
#define CE1_AXI_CBCR                                       (0x1048)
#define CE1_AHB_CBCR                                       (0x104C)
#define GCC_XO_DIV4_CBCR                                   (0x10C8)
#define LPASS_Q6_AXI_CBCR                                  (0x11C0)
#define APCS_GPLL_ENA_VOTE                                 (0x1480)
#define APCS_CLOCK_BRANCH_ENA_VOTE                         (0x1484)
#define APCS_CLOCK_SLEEP_ENA_VOTE                          (0x1488)
#define GCC_DEBUG_CLK_CTL                                  (0x1880)
#define CLOCK_FRQ_MEASURE_CTL                              (0x1884)
#define CLOCK_FRQ_MEASURE_STATUS                           (0x1888)
#define PLLTEST_PAD_CFG                                    (0x188C)
#define GP1_CBCR                                           (0x1900)
#define GP1_CMD_RCGR                                       (0x1904)
#define GP2_CBCR                                           (0x1940)
#define GP2_CMD_RCGR                                       (0x1944)
#define GP3_CBCR                                           (0x1980)
#define GP3_CMD_RCGR                                       (0x1984)
#define LPAAUDIO_PLL_MODE                                  (0x0000)
#define LPAAUDIO_PLL_L                                     (0x0004)
#define LPAAUDIO_PLL_M                                     (0x0008)
#define LPAAUDIO_PLL_N                                     (0x000C)
#define LPAAUDIO_PLL_USER_CTL                              (0x0010)
#define LPAAUDIO_PLL_STATUS                                (0x001C)
#define LPA_PLL_VOTE_APPS                                  (0x2000)
#define Q6SS_BCR                                           (0x6000)
#define AUDIO_CORE_GDSCR                                   (0x7000)
#define LPAIF_SPKR_CMD_RCGR                                (0xA000)
#define AUDIO_CORE_LPAIF_CODEC_SPKR_OSR_CBCR               (0xA014)
#define AUDIO_CORE_LPAIF_CODEC_SPKR_IBIT_CBCR              (0xA018)
#define AUDIO_CORE_LPAIF_CODEC_SPKR_EBIT_CBCR              (0xA01C)
#define LPAIF_PRI_CMD_RCGR                                 (0xB000)
#define AUDIO_CORE_LPAIF_PRI_OSR_CBCR                      (0xB014)
#define AUDIO_CORE_LPAIF_PRI_IBIT_CBCR                     (0xB018)
#define AUDIO_CORE_LPAIF_PRI_EBIT_CBCR                     (0xB01C)
#define LPAIF_SEC_CMD_RCGR                                 (0xC000)
#define AUDIO_CORE_LPAIF_SEC_OSR_CBCR                      (0xC014)
#define AUDIO_CORE_LPAIF_SEC_IBIT_CBCR                     (0xC018)
#define AUDIO_CORE_LPAIF_SEC_EBIT_CBCR                     (0xC01C)
#define LPAIF_TER_CMD_RCGR                                 (0xD000)
#define AUDIO_CORE_LPAIF_TER_OSR_CBCR                      (0xD014)
#define AUDIO_CORE_LPAIF_TER_IBIT_CBCR                     (0xD018)
#define AUDIO_CORE_LPAIF_TER_EBIT_CBCR                     (0xD01C)
#define LPAIF_QUAD_CMD_RCGR                                (0xE000)
#define AUDIO_CORE_LPAIF_QUAD_OSR_CBCR                     (0xE014)
#define AUDIO_CORE_LPAIF_QUAD_IBIT_CBCR                    (0xE018)
#define AUDIO_CORE_LPAIF_QUAD_EBIT_CBCR                    (0xE01C)
#define LPAIF_PCM0_CMD_RCGR                                (0xF000)
#define AUDIO_CORE_LPAIF_PCM0_IBIT_CBCR                    (0xF014)
#define AUDIO_CORE_LPAIF_PCM0_EBIT_CBCR                    (0xF018)
#define LPAIF_PCM1_CMD_RCGR                                (0x10000)
#define AUDIO_CORE_LPAIF_PCM1_IBIT_CBCR                    (0x10014)
#define AUDIO_CORE_LPAIF_PCM1_EBIT_CBCR                    (0x10018)
#define SLIMBUS_CMD_RCGR                                   (0x12000)
#define AUDIO_CORE_SLIMBUS_CORE_CBCR                       (0x12014)
#define AUDIO_CORE_SLIMBUS_LFABIF_CBCR                     (0x12018)
#define LPAIF_PCMOE_CMD_RCGR                               (0x13000)
#define AUDIO_CORE_LPAIF_PCM_DATA_OE_CBCR                  (0x13014)
#define AUDIO_CORE_IXFABRIC_CBCR                           (0x1B000)
#define Q6SS_AHB_LFABIF_CBCR                               (0x22000)
#define Q6SS_AHBM_CBCR                                     (0x22004)
#define AUDIO_WRAPPER_BR_CBCR                              (0x24000)
#define Q6SS_XO_CBCR                                       (0x26000)

static unsigned int soft_vote_gpll0;

static struct pll_vote_clk gpll0 = {
	.en_reg = (void __iomem *)APCS_GPLL_ENA_VOTE,
	.en_mask = BIT(0),
	.status_reg = (void __iomem *)GPLL0_STATUS,
	.status_mask = BIT(17),
	.soft_vote = &soft_vote_gpll0,
	.soft_vote_mask = PLL_SOFT_VOTE_PRIMARY,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.rate = 600000000,
		.parent = &xo.c,
		.dbg_name = "gpll0",
		.ops = &clk_ops_pll_acpu_vote,
		CLK_INIT(gpll0.c),
	},
};

/*Don't vote for xo if using this clock to allow xo shutdown*/
static struct pll_vote_clk gpll0_ao = {
	.en_reg = (void __iomem *)APCS_GPLL_ENA_VOTE,
	.en_mask = BIT(0),
	.status_reg = (void __iomem *)GPLL0_STATUS,
	.status_mask = BIT(17),
	.soft_vote = &soft_vote_gpll0,
	.soft_vote_mask = PLL_SOFT_VOTE_ACPU,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.rate = 600000000,
		.dbg_name = "gpll0_ao",
		.ops = &clk_ops_pll_acpu_vote,
		CLK_INIT(gpll0_ao.c),
	},
};

static struct pll_vote_clk gpll1 = {
	.en_reg = (void __iomem *)APCS_GPLL_ENA_VOTE,
	.en_mask = BIT(1),
	.status_reg = (void __iomem *)GPLL1_STATUS,
	.status_mask = BIT(17),
	.base = &virt_bases[GCC_BASE],
	.c = {
		.rate = 480000000,
		.parent = &xo.c,
		.dbg_name = "gpll1",
		.ops = &clk_ops_pll_vote,
		CLK_INIT(gpll1.c),
	},
};

static struct clk_freq_tbl ftbl_gcc_blsp1_qup1_6_i2c_apps_clk[] = {
	F_GCC(  19200000,         xo,   0,    0,    0),
	F_END
};

static struct rcg_clk blsp1_qup1_i2c_apps_clk_src = {
	.cmd_rcgr_reg = BLSP1_QUP1_I2C_APPS_CMD_RCGR,
	.set_rate = set_rate_hid,
	.freq_tbl = ftbl_gcc_blsp1_qup1_6_i2c_apps_clk,
	.current_freq = &rcg_dummy_freq,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "blsp1_qup1_i2c_apps_clk_src",
		.ops = &clk_ops_rcg,
		VDD_DIG_FMAX_MAP1(LOW, 50000000),
		CLK_INIT(blsp1_qup1_i2c_apps_clk_src.c),
	},
};

static struct clk_freq_tbl ftbl_gcc_blsp1_qup1_6_spi_apps_clk[] = {
	F_GCC(    960000,         xo,  10,    1,    2),
	F_GCC(   4800000,         xo,   4,    0,    0),
	F_GCC(   9600000,         xo,   2,    0,    0),
	F_GCC(  15000000,      gpll0,  10,    1,    4),
	F_GCC(  19200000,         xo,   1,    0,    0),
	F_GCC(  25000000,      gpll0,  12,    1,    2),
	F_GCC(  50000000,      gpll0,  12,    0,    0),
	F_END
};

static struct rcg_clk blsp1_qup1_spi_apps_clk_src = {
	.cmd_rcgr_reg = BLSP1_QUP1_SPI_APPS_CMD_RCGR,
	.set_rate = set_rate_mnd,
	.freq_tbl = ftbl_gcc_blsp1_qup1_6_spi_apps_clk,
	.current_freq = &rcg_dummy_freq,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "blsp1_qup1_spi_apps_clk_src",
		.ops = &clk_ops_rcg_mnd,
		VDD_DIG_FMAX_MAP2(LOW, 25000000, NOMINAL, 50000000),
		CLK_INIT(blsp1_qup1_spi_apps_clk_src.c),
	},
};

static struct rcg_clk blsp1_qup2_i2c_apps_clk_src = {
	.cmd_rcgr_reg = BLSP1_QUP2_I2C_APPS_CMD_RCGR,
	.set_rate = set_rate_hid,
	.freq_tbl = ftbl_gcc_blsp1_qup1_6_i2c_apps_clk,
	.current_freq = &rcg_dummy_freq,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "blsp1_qup2_i2c_apps_clk_src",
		.ops = &clk_ops_rcg,
		VDD_DIG_FMAX_MAP1(LOW, 50000000),
		CLK_INIT(blsp1_qup2_i2c_apps_clk_src.c),
	},
};

static struct rcg_clk blsp1_qup2_spi_apps_clk_src = {
	.cmd_rcgr_reg = BLSP1_QUP2_SPI_APPS_CMD_RCGR,
	.set_rate = set_rate_mnd,
	.freq_tbl = ftbl_gcc_blsp1_qup1_6_spi_apps_clk,
	.current_freq = &rcg_dummy_freq,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "blsp1_qup2_spi_apps_clk_src",
		.ops = &clk_ops_rcg_mnd,
		VDD_DIG_FMAX_MAP2(LOW, 25000000, NOMINAL, 50000000),
		CLK_INIT(blsp1_qup2_spi_apps_clk_src.c),
	},
};

static struct rcg_clk blsp1_qup3_i2c_apps_clk_src = {
	.cmd_rcgr_reg = BLSP1_QUP3_I2C_APPS_CMD_RCGR,
	.set_rate = set_rate_hid,
	.freq_tbl = ftbl_gcc_blsp1_qup1_6_i2c_apps_clk,
	.current_freq = &rcg_dummy_freq,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "blsp1_qup3_i2c_apps_clk_src",
		.ops = &clk_ops_rcg,
		VDD_DIG_FMAX_MAP1(LOW, 50000000),
		CLK_INIT(blsp1_qup3_i2c_apps_clk_src.c),
	},
};

static struct rcg_clk blsp1_qup3_spi_apps_clk_src = {
	.cmd_rcgr_reg = BLSP1_QUP3_SPI_APPS_CMD_RCGR,
	.set_rate = set_rate_mnd,
	.freq_tbl = ftbl_gcc_blsp1_qup1_6_spi_apps_clk,
	.current_freq = &rcg_dummy_freq,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "blsp1_qup3_spi_apps_clk_src",
		.ops = &clk_ops_rcg_mnd,
		VDD_DIG_FMAX_MAP2(LOW, 25000000, NOMINAL, 50000000),
		CLK_INIT(blsp1_qup3_spi_apps_clk_src.c),
	},
};

static struct rcg_clk blsp1_qup4_i2c_apps_clk_src = {
	.cmd_rcgr_reg = BLSP1_QUP4_I2C_APPS_CMD_RCGR,
	.set_rate = set_rate_hid,
	.freq_tbl = ftbl_gcc_blsp1_qup1_6_i2c_apps_clk,
	.current_freq = &rcg_dummy_freq,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "blsp1_qup4_i2c_apps_clk_src",
		.ops = &clk_ops_rcg,
		VDD_DIG_FMAX_MAP1(LOW, 50000000),
		CLK_INIT(blsp1_qup4_i2c_apps_clk_src.c),
	},
};

static struct rcg_clk blsp1_qup4_spi_apps_clk_src = {
	.cmd_rcgr_reg = BLSP1_QUP4_SPI_APPS_CMD_RCGR,
	.set_rate = set_rate_mnd,
	.freq_tbl = ftbl_gcc_blsp1_qup1_6_spi_apps_clk,
	.current_freq = &rcg_dummy_freq,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "blsp1_qup4_spi_apps_clk_src",
		.ops = &clk_ops_rcg_mnd,
		VDD_DIG_FMAX_MAP2(LOW, 25000000, NOMINAL, 50000000),
		CLK_INIT(blsp1_qup4_spi_apps_clk_src.c),
	},
};

static struct rcg_clk blsp1_qup5_i2c_apps_clk_src = {
	.cmd_rcgr_reg = BLSP1_QUP5_I2C_APPS_CMD_RCGR,
	.set_rate = set_rate_hid,
	.freq_tbl = ftbl_gcc_blsp1_qup1_6_i2c_apps_clk,
	.current_freq = &rcg_dummy_freq,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "blsp1_qup5_i2c_apps_clk_src",
		.ops = &clk_ops_rcg,
		VDD_DIG_FMAX_MAP1(LOW, 50000000),
		CLK_INIT(blsp1_qup5_i2c_apps_clk_src.c),
	},
};

static struct rcg_clk blsp1_qup5_spi_apps_clk_src = {
	.cmd_rcgr_reg = BLSP1_QUP5_SPI_APPS_CMD_RCGR,
	.set_rate = set_rate_mnd,
	.freq_tbl = ftbl_gcc_blsp1_qup1_6_spi_apps_clk,
	.current_freq = &rcg_dummy_freq,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "blsp1_qup5_spi_apps_clk_src",
		.ops = &clk_ops_rcg_mnd,
		VDD_DIG_FMAX_MAP2(LOW, 25000000, NOMINAL, 50000000),
		CLK_INIT(blsp1_qup5_spi_apps_clk_src.c),
	},
};

static struct rcg_clk blsp1_qup6_i2c_apps_clk_src = {
	.cmd_rcgr_reg = BLSP1_QUP6_I2C_APPS_CMD_RCGR,
	.set_rate = set_rate_hid,
	.freq_tbl = ftbl_gcc_blsp1_qup1_6_i2c_apps_clk,
	.current_freq = &rcg_dummy_freq,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "blsp1_qup6_i2c_apps_clk_src",
		.ops = &clk_ops_rcg,
		VDD_DIG_FMAX_MAP1(LOW, 50000000),
		CLK_INIT(blsp1_qup6_i2c_apps_clk_src.c),
	},
};

static struct rcg_clk blsp1_qup6_spi_apps_clk_src = {
	.cmd_rcgr_reg = BLSP1_QUP6_SPI_APPS_CMD_RCGR,
	.set_rate = set_rate_mnd,
	.freq_tbl = ftbl_gcc_blsp1_qup1_6_spi_apps_clk,
	.current_freq = &rcg_dummy_freq,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "blsp1_qup6_spi_apps_clk_src",
		.ops = &clk_ops_rcg_mnd,
		VDD_DIG_FMAX_MAP2(LOW, 25000000, NOMINAL, 50000000),
		CLK_INIT(blsp1_qup6_spi_apps_clk_src.c),
	},
};

static struct clk_freq_tbl ftbl_gcc_blsp1_uart1_6_apps_clk[] = {
	F_GCC(   3686400,      gpll0,   1,   96, 15625),
	F_GCC(   7372800,      gpll0,   1,  192, 15625),
	F_GCC(  14745600,      gpll0,   1,  384, 15625),
	F_GCC(  16000000,      gpll0,   5,    2,   15),
	F_GCC(  19200000,         xo,   1,    0,    0),
	F_GCC(  24000000,      gpll0,   5,    1,    5),
	F_GCC(  32000000,      gpll0,   1,    4,   75),
	F_GCC(  40000000,      gpll0,  15,    0,    0),
	F_GCC(  46400000,      gpll0,   1,   29,  375),
	F_GCC(  48000000,      gpll0, 12.5,    0,    0),
	F_GCC(  51200000,      gpll0,   1,   32,  375),
	F_GCC(  56000000,      gpll0,   1,    7,   75),
	F_GCC(  58982400,      gpll0,   1, 1536, 15625),
	F_GCC(  60000000,      gpll0,  10,    0,    0),
	F_END
};

static struct rcg_clk blsp1_uart1_apps_clk_src = {
	.cmd_rcgr_reg = BLSP1_UART1_APPS_CMD_RCGR,
	.set_rate = set_rate_mnd,
	.freq_tbl = ftbl_gcc_blsp1_uart1_6_apps_clk,
	.current_freq = &rcg_dummy_freq,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "blsp1_uart1_apps_clk_src",
		.ops = &clk_ops_rcg_mnd,
		VDD_DIG_FMAX_MAP2(LOW, 31580000, NOMINAL, 63160000),
		CLK_INIT(blsp1_uart1_apps_clk_src.c),
	},
};

static struct rcg_clk blsp1_uart2_apps_clk_src = {
	.cmd_rcgr_reg = BLSP1_UART2_APPS_CMD_RCGR,
	.set_rate = set_rate_mnd,
	.freq_tbl = ftbl_gcc_blsp1_uart1_6_apps_clk,
	.current_freq = &rcg_dummy_freq,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "blsp1_uart2_apps_clk_src",
		.ops = &clk_ops_rcg_mnd,
		VDD_DIG_FMAX_MAP2(LOW, 31580000, NOMINAL, 63160000),
		CLK_INIT(blsp1_uart2_apps_clk_src.c),
	},
};

static struct rcg_clk blsp1_uart3_apps_clk_src = {
	.cmd_rcgr_reg = BLSP1_UART3_APPS_CMD_RCGR,
	.set_rate = set_rate_mnd,
	.freq_tbl = ftbl_gcc_blsp1_uart1_6_apps_clk,
	.current_freq = &rcg_dummy_freq,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "blsp1_uart3_apps_clk_src",
		.ops = &clk_ops_rcg_mnd,
		VDD_DIG_FMAX_MAP2(LOW, 31580000, NOMINAL, 63160000),
		CLK_INIT(blsp1_uart3_apps_clk_src.c),
	},
};

static struct rcg_clk blsp1_uart4_apps_clk_src = {
	.cmd_rcgr_reg = BLSP1_UART4_APPS_CMD_RCGR,
	.set_rate = set_rate_mnd,
	.freq_tbl = ftbl_gcc_blsp1_uart1_6_apps_clk,
	.current_freq = &rcg_dummy_freq,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "blsp1_uart4_apps_clk_src",
		.ops = &clk_ops_rcg_mnd,
		VDD_DIG_FMAX_MAP2(LOW, 31580000, NOMINAL, 63160000),
		CLK_INIT(blsp1_uart4_apps_clk_src.c),
	},
};

static struct rcg_clk blsp1_uart5_apps_clk_src = {
	.cmd_rcgr_reg = BLSP1_UART5_APPS_CMD_RCGR,
	.set_rate = set_rate_mnd,
	.freq_tbl = ftbl_gcc_blsp1_uart1_6_apps_clk,
	.current_freq = &rcg_dummy_freq,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "blsp1_uart5_apps_clk_src",
		.ops = &clk_ops_rcg_mnd,
		VDD_DIG_FMAX_MAP2(LOW, 31580000, NOMINAL, 63160000),
		CLK_INIT(blsp1_uart5_apps_clk_src.c),
	},
};

static struct rcg_clk blsp1_uart6_apps_clk_src = {
	.cmd_rcgr_reg = BLSP1_UART6_APPS_CMD_RCGR,
	.set_rate = set_rate_mnd,
	.freq_tbl = ftbl_gcc_blsp1_uart1_6_apps_clk,
	.current_freq = &rcg_dummy_freq,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "blsp1_uart6_apps_clk_src",
		.ops = &clk_ops_rcg_mnd,
		VDD_DIG_FMAX_MAP2(LOW, 31580000, NOMINAL, 63160000),
		CLK_INIT(blsp1_uart6_apps_clk_src.c),
	},
};

static struct clk_freq_tbl ftbl_gcc_ce1_clk[] = {
	F_GCC(  50000000,      gpll0,  12,    0,    0),
	F_GCC( 100000000,      gpll0,   6,    0,    0),
	F_END
};

static struct rcg_clk ce1_clk_src = {
	.cmd_rcgr_reg = CE1_CMD_RCGR,
	.set_rate = set_rate_hid,
	.freq_tbl = ftbl_gcc_ce1_clk,
	.current_freq = &rcg_dummy_freq,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "ce1_clk_src",
		.ops = &clk_ops_rcg,
		VDD_DIG_FMAX_MAP2(LOW, 50000000, NOMINAL, 100000000),
		CLK_INIT(ce1_clk_src.c),
	},
};

static struct clk_freq_tbl ftbl_gcc_gp1_3_clk[] = {
	F_GCC(  19200000,         xo,   1,    0,    0),
	F_END
};

static struct rcg_clk gp1_clk_src = {
	.cmd_rcgr_reg = GP1_CMD_RCGR,
	.set_rate = set_rate_mnd,
	.freq_tbl = ftbl_gcc_gp1_3_clk,
	.current_freq = &rcg_dummy_freq,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "gp1_clk_src",
		.ops = &clk_ops_rcg_mnd,
		VDD_DIG_FMAX_MAP2(LOW, 100000000, NOMINAL, 200000000),
		CLK_INIT(gp1_clk_src.c),
	},
};

static struct rcg_clk gp2_clk_src = {
	.cmd_rcgr_reg = GP2_CMD_RCGR,
	.set_rate = set_rate_mnd,
	.freq_tbl = ftbl_gcc_gp1_3_clk,
	.current_freq = &rcg_dummy_freq,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "gp2_clk_src",
		.ops = &clk_ops_rcg_mnd,
		VDD_DIG_FMAX_MAP2(LOW, 100000000, NOMINAL, 200000000),
		CLK_INIT(gp2_clk_src.c),
	},
};

static struct rcg_clk gp3_clk_src = {
	.cmd_rcgr_reg = GP3_CMD_RCGR,
	.set_rate = set_rate_mnd,
	.freq_tbl = ftbl_gcc_gp1_3_clk,
	.current_freq = &rcg_dummy_freq,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "gp3_clk_src",
		.ops = &clk_ops_rcg_mnd,
		VDD_DIG_FMAX_MAP2(LOW, 100000000, NOMINAL, 200000000),
		CLK_INIT(gp3_clk_src.c),
	},
};

static struct clk_freq_tbl ftbl_gcc_pdm2_clk[] = {
	F_GCC(  60000000,      gpll0,  10,    0,    0),
	F_END
};

static struct rcg_clk pdm2_clk_src = {
	.cmd_rcgr_reg = PDM2_CMD_RCGR,
	.set_rate = set_rate_hid,
	.freq_tbl = ftbl_gcc_pdm2_clk,
	.current_freq = &rcg_dummy_freq,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "pdm2_clk_src",
		.ops = &clk_ops_rcg,
		VDD_DIG_FMAX_MAP1(LOW, 60000000),
		CLK_INIT(pdm2_clk_src.c),
	},
};

static struct clk_freq_tbl ftbl_gcc_sdcc1_3_apps_clk[] = {
	F_GCC(    144000,         xo,  16,    3,   25),
	F_GCC(    400000,         xo,  12,    1,    4),
	F_GCC(  20000000,      gpll0,  15,    1,    2),
	F_GCC(  25000000,      gpll0,  12,    1,    2),
	F_GCC(  50000000,      gpll0,  12,    0,    0),
	F_GCC( 100000000,      gpll0,   6,    0,    0),
	F_GCC( 200000000,      gpll0,   3,    0,    0),
	F_END
};

static struct rcg_clk sdcc1_apps_clk_src = {
	.cmd_rcgr_reg = SDCC1_APPS_CMD_RCGR,
	.set_rate = set_rate_mnd,
	.freq_tbl = ftbl_gcc_sdcc1_3_apps_clk,
	.current_freq = &rcg_dummy_freq,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "sdcc1_apps_clk_src",
		.ops = &clk_ops_rcg_mnd,
		VDD_DIG_FMAX_MAP2(LOW, 100000000, NOMINAL, 200000000),
		CLK_INIT(sdcc1_apps_clk_src.c),
	},
};

static struct rcg_clk sdcc2_apps_clk_src = {
	.cmd_rcgr_reg = SDCC2_APPS_CMD_RCGR,
	.set_rate = set_rate_mnd,
	.freq_tbl = ftbl_gcc_sdcc1_3_apps_clk,
	.current_freq = &rcg_dummy_freq,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "sdcc2_apps_clk_src",
		.ops = &clk_ops_rcg_mnd,
		VDD_DIG_FMAX_MAP2(LOW, 100000000, NOMINAL, 200000000),
		CLK_INIT(sdcc2_apps_clk_src.c),
	},
};

static struct rcg_clk sdcc3_apps_clk_src = {
	.cmd_rcgr_reg = SDCC3_APPS_CMD_RCGR,
	.set_rate = set_rate_mnd,
	.freq_tbl = ftbl_gcc_sdcc1_3_apps_clk,
	.current_freq = &rcg_dummy_freq,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "sdcc3_apps_clk_src",
		.ops = &clk_ops_rcg_mnd,
		VDD_DIG_FMAX_MAP2(LOW, 50000000, NOMINAL, 100000000),
		CLK_INIT(sdcc3_apps_clk_src.c),
	},
};

static struct clk_freq_tbl ftbl_gcc_usb_hs_system_clk[] = {
	F_GCC(  75000000,      gpll0,   8,    0,    0),
	F_END
};

static struct rcg_clk usb_hs_system_clk_src = {
	.cmd_rcgr_reg = USB_HS_SYSTEM_CMD_RCGR,
	.set_rate = set_rate_hid,
	.freq_tbl = ftbl_gcc_usb_hs_system_clk,
	.current_freq = &rcg_dummy_freq,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "usb_hs_system_clk_src",
		.ops = &clk_ops_rcg,
		VDD_DIG_FMAX_MAP2(LOW, 37500000, NOMINAL, 75000000),
		CLK_INIT(usb_hs_system_clk_src.c),
	},
};

static struct clk_freq_tbl ftbl_gcc_usb_hsic_clk[] = {
	F_HSIC( 480000000,      gpll1,   0,    0,    0),
	F_END
};

static struct rcg_clk usb_hsic_clk_src = {
	.cmd_rcgr_reg = USB_HSIC_CMD_RCGR,
	.set_rate = set_rate_hid,
	.freq_tbl = ftbl_gcc_usb_hsic_clk,
	.current_freq = &rcg_dummy_freq,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "usb_hsic_clk_src",
		.ops = &clk_ops_rcg,
		VDD_DIG_FMAX_MAP1(LOW, 480000000),
		CLK_INIT(usb_hsic_clk_src.c),
	},
};

static struct clk_freq_tbl ftbl_gcc_usb_hsic_io_cal_clk[] = {
	F_GCC(   9600000,         xo,   2,    0,    0),
	F_END
};

static struct rcg_clk usb_hsic_io_cal_clk_src = {
	.cmd_rcgr_reg = USB_HSIC_IO_CAL_CMD_RCGR,
	.set_rate = set_rate_hid,
	.freq_tbl = ftbl_gcc_usb_hsic_io_cal_clk,
	.current_freq = &rcg_dummy_freq,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "usb_hsic_io_cal_clk_src",
		.ops = &clk_ops_rcg,
		VDD_DIG_FMAX_MAP1(LOW, 9600000),
		CLK_INIT(usb_hsic_io_cal_clk_src.c),
	},
};

static struct clk_freq_tbl ftbl_gcc_usb_hsic_system_clk[] = {
	F_GCC(  75000000,      gpll0,   8,    0,    0),
	F_END
};

static struct rcg_clk usb_hsic_system_clk_src = {
	.cmd_rcgr_reg = USB_HSIC_SYSTEM_CMD_RCGR,
	.set_rate = set_rate_hid,
	.freq_tbl = ftbl_gcc_usb_hsic_system_clk,
	.current_freq = &rcg_dummy_freq,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "usb_hsic_system_clk_src",
		.ops = &clk_ops_rcg,
		VDD_DIG_FMAX_MAP2(LOW, 37500000, NOMINAL, 75000000),
		CLK_INIT(usb_hsic_system_clk_src.c),
	},
};

static struct local_vote_clk gcc_bam_dma_ahb_clk = {
	.cbcr_reg = BAM_DMA_AHB_CBCR,
	.vote_reg = APCS_CLOCK_BRANCH_ENA_VOTE,
	.en_mask = BIT(12),
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "gcc_bam_dma_ahb_clk",
		.ops = &clk_ops_vote,
		CLK_INIT(gcc_bam_dma_ahb_clk.c),
	},
};

static struct local_vote_clk gcc_blsp1_ahb_clk = {
	.cbcr_reg = BLSP1_AHB_CBCR,
	.vote_reg = APCS_CLOCK_BRANCH_ENA_VOTE,
	.en_mask = BIT(17),
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "gcc_blsp1_ahb_clk",
		.ops = &clk_ops_vote,
		CLK_INIT(gcc_blsp1_ahb_clk.c),
	},
};

static struct branch_clk gcc_blsp1_qup1_i2c_apps_clk = {
	.cbcr_reg = BLSP1_QUP1_I2C_APPS_CBCR,
	.has_sibling = 0,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "gcc_blsp1_qup1_i2c_apps_clk",
		.parent = &blsp1_qup1_i2c_apps_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(gcc_blsp1_qup1_i2c_apps_clk.c),
	},
};

static struct branch_clk gcc_blsp1_qup1_spi_apps_clk = {
	.cbcr_reg = BLSP1_QUP1_SPI_APPS_CBCR,
	.has_sibling = 0,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "gcc_blsp1_qup1_spi_apps_clk",
		.parent = &blsp1_qup1_spi_apps_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(gcc_blsp1_qup1_spi_apps_clk.c),
	},
};

static struct branch_clk gcc_blsp1_qup2_i2c_apps_clk = {
	.cbcr_reg = BLSP1_QUP2_I2C_APPS_CBCR,
	.has_sibling = 0,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "gcc_blsp1_qup2_i2c_apps_clk",
		.parent = &blsp1_qup2_i2c_apps_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(gcc_blsp1_qup2_i2c_apps_clk.c),
	},
};

static struct branch_clk gcc_blsp1_qup2_spi_apps_clk = {
	.cbcr_reg = BLSP1_QUP2_SPI_APPS_CBCR,
	.has_sibling = 0,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "gcc_blsp1_qup2_spi_apps_clk",
		.parent = &blsp1_qup2_spi_apps_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(gcc_blsp1_qup2_spi_apps_clk.c),
	},
};

static struct branch_clk gcc_blsp1_qup3_i2c_apps_clk = {
	.cbcr_reg = BLSP1_QUP3_I2C_APPS_CBCR,
	.has_sibling = 0,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "gcc_blsp1_qup3_i2c_apps_clk",
		.parent = &blsp1_qup3_i2c_apps_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(gcc_blsp1_qup3_i2c_apps_clk.c),
	},
};

static struct branch_clk gcc_blsp1_qup3_spi_apps_clk = {
	.cbcr_reg = BLSP1_QUP3_SPI_APPS_CBCR,
	.has_sibling = 0,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "gcc_blsp1_qup3_spi_apps_clk",
		.parent = &blsp1_qup3_spi_apps_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(gcc_blsp1_qup3_spi_apps_clk.c),
	},
};

static struct branch_clk gcc_blsp1_qup4_i2c_apps_clk = {
	.cbcr_reg = BLSP1_QUP4_I2C_APPS_CBCR,
	.has_sibling = 0,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "gcc_blsp1_qup4_i2c_apps_clk",
		.parent = &blsp1_qup4_i2c_apps_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(gcc_blsp1_qup4_i2c_apps_clk.c),
	},
};

static struct branch_clk gcc_blsp1_qup4_spi_apps_clk = {
	.cbcr_reg = BLSP1_QUP4_SPI_APPS_CBCR,
	.has_sibling = 0,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "gcc_blsp1_qup4_spi_apps_clk",
		.parent = &blsp1_qup4_spi_apps_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(gcc_blsp1_qup4_spi_apps_clk.c),
	},
};

static struct branch_clk gcc_blsp1_qup5_i2c_apps_clk = {
	.cbcr_reg = BLSP1_QUP5_I2C_APPS_CBCR,
	.has_sibling = 0,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "gcc_blsp1_qup5_i2c_apps_clk",
		.parent = &blsp1_qup5_i2c_apps_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(gcc_blsp1_qup5_i2c_apps_clk.c),
	},
};

static struct branch_clk gcc_blsp1_qup5_spi_apps_clk = {
	.cbcr_reg = BLSP1_QUP5_SPI_APPS_CBCR,
	.has_sibling = 0,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "gcc_blsp1_qup5_spi_apps_clk",
		.parent = &blsp1_qup5_spi_apps_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(gcc_blsp1_qup5_spi_apps_clk.c),
	},
};

static struct branch_clk gcc_blsp1_qup6_i2c_apps_clk = {
	.cbcr_reg = BLSP1_QUP6_I2C_APPS_CBCR,
	.has_sibling = 0,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "gcc_blsp1_qup6_i2c_apps_clk",
		.parent = &blsp1_qup6_i2c_apps_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(gcc_blsp1_qup6_i2c_apps_clk.c),
	},
};

static struct branch_clk gcc_blsp1_qup6_spi_apps_clk = {
	.cbcr_reg = BLSP1_QUP6_SPI_APPS_CBCR,
	.has_sibling = 0,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "gcc_blsp1_qup6_spi_apps_clk",
		.parent = &blsp1_qup6_spi_apps_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(gcc_blsp1_qup6_spi_apps_clk.c),
	},
};

static struct branch_clk gcc_blsp1_uart1_apps_clk = {
	.cbcr_reg = BLSP1_UART1_APPS_CBCR,
	.has_sibling = 0,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "gcc_blsp1_uart1_apps_clk",
		.parent = &blsp1_uart1_apps_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(gcc_blsp1_uart1_apps_clk.c),
	},
};

static struct branch_clk gcc_blsp1_uart2_apps_clk = {
	.cbcr_reg = BLSP1_UART2_APPS_CBCR,
	.has_sibling = 0,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "gcc_blsp1_uart2_apps_clk",
		.parent = &blsp1_uart2_apps_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(gcc_blsp1_uart2_apps_clk.c),
	},
};

static struct branch_clk gcc_blsp1_uart3_apps_clk = {
	.cbcr_reg = BLSP1_UART3_APPS_CBCR,
	.has_sibling = 0,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "gcc_blsp1_uart3_apps_clk",
		.parent = &blsp1_uart3_apps_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(gcc_blsp1_uart3_apps_clk.c),
	},
};

static struct branch_clk gcc_blsp1_uart4_apps_clk = {
	.cbcr_reg = BLSP1_UART4_APPS_CBCR,
	.has_sibling = 0,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "gcc_blsp1_uart4_apps_clk",
		.parent = &blsp1_uart4_apps_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(gcc_blsp1_uart4_apps_clk.c),
	},
};

static struct branch_clk gcc_blsp1_uart5_apps_clk = {
	.cbcr_reg = BLSP1_UART5_APPS_CBCR,
	.has_sibling = 0,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "gcc_blsp1_uart5_apps_clk",
		.parent = &blsp1_uart5_apps_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(gcc_blsp1_uart5_apps_clk.c),
	},
};

static struct branch_clk gcc_blsp1_uart6_apps_clk = {
	.cbcr_reg = BLSP1_UART6_APPS_CBCR,
	.has_sibling = 0,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "gcc_blsp1_uart6_apps_clk",
		.parent = &blsp1_uart6_apps_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(gcc_blsp1_uart6_apps_clk.c),
	},
};

static struct local_vote_clk gcc_boot_rom_ahb_clk = {
	.cbcr_reg = BOOT_ROM_AHB_CBCR,
	.vote_reg = APCS_CLOCK_BRANCH_ENA_VOTE,
	.en_mask = BIT(10),
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "gcc_boot_rom_ahb_clk",
		.ops = &clk_ops_vote,
		CLK_INIT(gcc_boot_rom_ahb_clk.c),
	},
};

static struct local_vote_clk gcc_ce1_ahb_clk = {
	.cbcr_reg = CE1_AHB_CBCR,
	.vote_reg = APCS_CLOCK_BRANCH_ENA_VOTE,
	.en_mask = BIT(3),
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "gcc_ce1_ahb_clk",
		.ops = &clk_ops_vote,
		CLK_INIT(gcc_ce1_ahb_clk.c),
	},
};

static struct local_vote_clk gcc_ce1_axi_clk = {
	.cbcr_reg = CE1_AXI_CBCR,
	.vote_reg = APCS_CLOCK_BRANCH_ENA_VOTE,
	.en_mask = BIT(4),
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "gcc_ce1_axi_clk",
		.ops = &clk_ops_vote,
		CLK_INIT(gcc_ce1_axi_clk.c),
	},
};

static struct local_vote_clk gcc_ce1_clk = {
	.cbcr_reg = CE1_CBCR,
	.vote_reg = APCS_CLOCK_BRANCH_ENA_VOTE,
	.en_mask = BIT(5),
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "gcc_ce1_clk",
		.ops = &clk_ops_vote,
		CLK_INIT(gcc_ce1_clk.c),
	},
};

static struct branch_clk gcc_gp1_clk = {
	.cbcr_reg = GP1_CBCR,
	.has_sibling = 0,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "gcc_gp1_clk",
		.parent = &gp1_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(gcc_gp1_clk.c),
	},
};

static struct branch_clk gcc_gp2_clk = {
	.cbcr_reg = GP2_CBCR,
	.has_sibling = 0,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "gcc_gp2_clk",
		.parent = &gp2_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(gcc_gp2_clk.c),
	},
};

static struct branch_clk gcc_gp3_clk = {
	.cbcr_reg = GP3_CBCR,
	.has_sibling = 0,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "gcc_gp3_clk",
		.parent = &gp3_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(gcc_gp3_clk.c),
	},
};

static struct branch_clk gcc_lpass_q6_axi_clk = {
	.cbcr_reg = LPASS_Q6_AXI_CBCR,
	.has_sibling = 1,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "gcc_lpass_q6_axi_clk",
		.ops = &clk_ops_branch,
		CLK_INIT(gcc_lpass_q6_axi_clk.c),
	},
};

static struct branch_clk gcc_mmss_noc_cfg_ahb_clk = {
	.cbcr_reg = MMSS_NOC_CFG_AHB_CBCR,
	.has_sibling = 1,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "gcc_mmss_noc_cfg_ahb_clk",
		.ops = &clk_ops_branch,
		CLK_INIT(gcc_mmss_noc_cfg_ahb_clk.c),
	},
};

static struct branch_clk gcc_mss_cfg_ahb_clk = {
	.cbcr_reg = MSS_CFG_AHB_CBCR,
	.has_sibling = 1,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "gcc_mss_cfg_ahb_clk",
		.ops = &clk_ops_branch,
		CLK_INIT(gcc_mss_cfg_ahb_clk.c),
	},
};

static struct branch_clk gcc_mss_q6_bimc_axi_clk = {
	.cbcr_reg = MSS_Q6_BIMC_AXI_CBCR,
	.has_sibling = 1,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "gcc_mss_q6_bimc_axi_clk",
		.ops = &clk_ops_branch,
		CLK_INIT(gcc_mss_q6_bimc_axi_clk.c),
	},
};

static struct branch_clk gcc_noc_conf_xpu_ahb_clk = {
	.cbcr_reg = NOC_CONF_XPU_AHB_CBCR,
	.has_sibling = 1,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "gcc_noc_conf_xpu_ahb_clk",
		.ops = &clk_ops_branch,
		CLK_INIT(gcc_noc_conf_xpu_ahb_clk.c),
	},
};

static struct branch_clk gcc_pdm2_clk = {
	.cbcr_reg = PDM2_CBCR,
	.has_sibling = 0,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "gcc_pdm2_clk",
		.parent = &pdm2_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(gcc_pdm2_clk.c),
	},
};

static struct branch_clk gcc_pdm_ahb_clk = {
	.cbcr_reg = PDM_AHB_CBCR,
	.has_sibling = 1,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "gcc_pdm_ahb_clk",
		.ops = &clk_ops_branch,
		CLK_INIT(gcc_pdm_ahb_clk.c),
	},
};

static struct branch_clk gcc_pdm_xo4_clk = {
	.cbcr_reg = PDM_XO4_CBCR,
	.has_sibling = 1,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "gcc_pdm_xo4_clk",
		.parent = &xo.c,
		.ops = &clk_ops_branch,
		CLK_INIT(gcc_pdm_xo4_clk.c),
	},
};

static struct branch_clk gcc_periph_noc_ahb_clk = {
	.cbcr_reg = PERIPH_NOC_AHB_CBCR,
	.has_sibling = 1,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "gcc_periph_noc_ahb_clk",
		.ops = &clk_ops_branch,
		CLK_INIT(gcc_periph_noc_ahb_clk.c),
	},
};

static struct local_vote_clk gcc_prng_ahb_clk = {
	.cbcr_reg = PRNG_AHB_CBCR,
	.vote_reg = APCS_CLOCK_BRANCH_ENA_VOTE,
	.en_mask = BIT(13),
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "gcc_prng_ahb_clk",
		.ops = &clk_ops_vote,
		CLK_INIT(gcc_prng_ahb_clk.c),
	},
};

static struct branch_clk gcc_sdcc1_ahb_clk = {
	.cbcr_reg = SDCC1_AHB_CBCR,
	.has_sibling = 1,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "gcc_sdcc1_ahb_clk",
		.ops = &clk_ops_branch,
		CLK_INIT(gcc_sdcc1_ahb_clk.c),
	},
};

static struct branch_clk gcc_sdcc1_apps_clk = {
	.cbcr_reg = SDCC1_APPS_CBCR,
	.has_sibling = 0,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "gcc_sdcc1_apps_clk",
		.parent = &sdcc1_apps_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(gcc_sdcc1_apps_clk.c),
	},
};

static struct branch_clk gcc_sdcc2_ahb_clk = {
	.cbcr_reg = SDCC2_AHB_CBCR,
	.has_sibling = 1,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "gcc_sdcc2_ahb_clk",
		.ops = &clk_ops_branch,
		CLK_INIT(gcc_sdcc2_ahb_clk.c),
	},
};

static struct branch_clk gcc_sdcc2_apps_clk = {
	.cbcr_reg = SDCC2_APPS_CBCR,
	.has_sibling = 0,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "gcc_sdcc2_apps_clk",
		.parent = &sdcc2_apps_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(gcc_sdcc2_apps_clk.c),
	},
};

static struct branch_clk gcc_sdcc3_ahb_clk = {
	.cbcr_reg = SDCC3_AHB_CBCR,
	.has_sibling = 1,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "gcc_sdcc3_ahb_clk",
		.ops = &clk_ops_branch,
		CLK_INIT(gcc_sdcc3_ahb_clk.c),
	},
};

static struct branch_clk gcc_sdcc3_apps_clk = {
	.cbcr_reg = SDCC3_APPS_CBCR,
	.has_sibling = 0,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "gcc_sdcc3_apps_clk",
		.parent = &sdcc3_apps_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(gcc_sdcc3_apps_clk.c),
	},
};

static struct branch_clk gcc_usb2a_phy_sleep_clk = {
	.cbcr_reg = USB2A_PHY_SLEEP_CBCR,
	.has_sibling = 1,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "gcc_usb2a_phy_sleep_clk",
		.ops = &clk_ops_branch,
		CLK_INIT(gcc_usb2a_phy_sleep_clk.c),
	},
};

static struct branch_clk gcc_usb_hs_ahb_clk = {
	.cbcr_reg = USB_HS_AHB_CBCR,
	.has_sibling = 1,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "gcc_usb_hs_ahb_clk",
		.ops = &clk_ops_branch,
		CLK_INIT(gcc_usb_hs_ahb_clk.c),
	},
};

static struct branch_clk gcc_usb_hs_system_clk = {
	.cbcr_reg = USB_HS_SYSTEM_CBCR,
	.has_sibling = 0,
	.bcr_reg = USB_HS_BCR,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "gcc_usb_hs_system_clk",
		.parent = &usb_hs_system_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(gcc_usb_hs_system_clk.c),
	},
};

static struct branch_clk gcc_usb_hsic_ahb_clk = {
	.cbcr_reg = USB_HSIC_AHB_CBCR,
	.has_sibling = 1,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "gcc_usb_hsic_ahb_clk",
		.ops = &clk_ops_branch,
		CLK_INIT(gcc_usb_hsic_ahb_clk.c),
	},
};

static struct branch_clk gcc_usb_hsic_clk = {
	.cbcr_reg = USB_HSIC_CBCR,
	.has_sibling = 0,
	.bcr_reg = USB_HS_HSIC_BCR,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "gcc_usb_hsic_clk",
		.parent = &usb_hsic_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(gcc_usb_hsic_clk.c),
	},
};

static struct branch_clk gcc_usb_hsic_io_cal_clk = {
	.cbcr_reg = USB_HSIC_IO_CAL_CBCR,
	.has_sibling = 0,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "gcc_usb_hsic_io_cal_clk",
		.parent = &usb_hsic_io_cal_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(gcc_usb_hsic_io_cal_clk.c),
	},
};

static struct branch_clk gcc_usb_hsic_system_clk = {
	.cbcr_reg = USB_HSIC_SYSTEM_CBCR,
	.has_sibling = 0,
	.bcr_reg = USB_HS_HSIC_BCR,
	.base = &virt_bases[GCC_BASE],
	.c = {
		.dbg_name = "gcc_usb_hsic_system_clk",
		.parent = &usb_hsic_system_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(gcc_usb_hsic_system_clk.c),
	},
};

static struct measure_mux_entry measure_mux_GCC[] = {
	{ &gcc_periph_noc_ahb_clk.c,  GCC_BASE, 0x0010 },
	{ &gcc_noc_conf_xpu_ahb_clk.c,  GCC_BASE, 0x0018 },
	{ &gcc_mmss_noc_cfg_ahb_clk.c,  GCC_BASE, 0x002a },
	{ &gcc_mss_cfg_ahb_clk.c,  GCC_BASE, 0x0030 },
	{ &gcc_mss_q6_bimc_axi_clk.c,  GCC_BASE, 0x0031 },
	{ &gcc_usb_hsic_ahb_clk.c,  GCC_BASE, 0x0058 },
	{ &gcc_usb_hsic_system_clk.c,  GCC_BASE, 0x0059 },
	{ &gcc_usb_hsic_clk.c,  GCC_BASE, 0x005a },
	{ &gcc_usb_hsic_io_cal_clk.c,  GCC_BASE, 0x005b },
	{ &gcc_usb_hs_system_clk.c,  GCC_BASE, 0x0060 },
	{ &gcc_usb_hs_ahb_clk.c,  GCC_BASE, 0x0061 },
	{ &gcc_usb2a_phy_sleep_clk.c,  GCC_BASE, 0x0063 },
	{ &gcc_sdcc1_apps_clk.c,  GCC_BASE, 0x0068 },
	{ &gcc_sdcc1_ahb_clk.c,  GCC_BASE, 0x0069 },
	{ &gcc_sdcc2_apps_clk.c,  GCC_BASE, 0x0070 },
	{ &gcc_sdcc2_ahb_clk.c,  GCC_BASE, 0x0071 },
	{ &gcc_sdcc3_apps_clk.c,  GCC_BASE, 0x0078 },
	{ &gcc_sdcc3_ahb_clk.c,  GCC_BASE, 0x0079 },
	{ &gcc_blsp1_ahb_clk.c,  GCC_BASE, 0x0088 },
	{ &gcc_blsp1_qup1_spi_apps_clk.c,  GCC_BASE, 0x008a },
	{ &gcc_blsp1_qup1_i2c_apps_clk.c,  GCC_BASE, 0x008b },
	{ &gcc_blsp1_uart1_apps_clk.c,  GCC_BASE, 0x008c },
	{ &gcc_blsp1_qup2_spi_apps_clk.c,  GCC_BASE, 0x008e },
	{ &gcc_blsp1_qup2_i2c_apps_clk.c,  GCC_BASE, 0x0090 },
	{ &gcc_blsp1_uart2_apps_clk.c,  GCC_BASE, 0x0091 },
	{ &gcc_blsp1_qup3_spi_apps_clk.c,  GCC_BASE, 0x0093 },
	{ &gcc_blsp1_qup3_i2c_apps_clk.c,  GCC_BASE, 0x0094 },
	{ &gcc_blsp1_uart3_apps_clk.c,  GCC_BASE, 0x0095 },
	{ &gcc_blsp1_qup4_spi_apps_clk.c,  GCC_BASE, 0x0098 },
	{ &gcc_blsp1_qup4_i2c_apps_clk.c,  GCC_BASE, 0x0099 },
	{ &gcc_blsp1_uart4_apps_clk.c,  GCC_BASE, 0x009a },
	{ &gcc_blsp1_qup5_spi_apps_clk.c,  GCC_BASE, 0x009c },
	{ &gcc_blsp1_qup5_i2c_apps_clk.c,  GCC_BASE, 0x009d },
	{ &gcc_blsp1_uart5_apps_clk.c,  GCC_BASE, 0x009e },
	{ &gcc_blsp1_qup6_spi_apps_clk.c,  GCC_BASE, 0x00a1 },
	{ &gcc_blsp1_qup6_i2c_apps_clk.c,  GCC_BASE, 0x00a2 },
	{ &gcc_blsp1_uart6_apps_clk.c,  GCC_BASE, 0x00a3 },
	{ &gcc_pdm_ahb_clk.c,  GCC_BASE, 0x00d0 },
	{ &gcc_pdm_xo4_clk.c,  GCC_BASE, 0x00d1 },
	{ &gcc_pdm2_clk.c,  GCC_BASE, 0x00d2 },
	{ &gcc_prng_ahb_clk.c,  GCC_BASE, 0x00d8 },
	{ &gcc_bam_dma_ahb_clk.c,  GCC_BASE, 0x00e0 },
	{ &gcc_boot_rom_ahb_clk.c,  GCC_BASE, 0x00f8 },
	{ &gcc_ce1_clk.c,  GCC_BASE, 0x0138 },
	{ &gcc_ce1_axi_clk.c,  GCC_BASE, 0x0139 },
	{ &gcc_ce1_ahb_clk.c,  GCC_BASE, 0x013a },
	{ &gcc_lpass_q6_axi_clk.c,  GCC_BASE, 0x0160 },
	{&dummy_clk, N_BASES, 0x0000},
};

static struct pll_vote_clk mmpll0_pll = {
	.en_reg = (void __iomem *)MMSS_PLL_VOTE_APCS,
	.en_mask = BIT(0),
	.status_reg = (void __iomem *)MMPLL0_PLL_STATUS,
	.status_mask = BIT(17),
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.rate = 800000000,
		.parent = &xo.c,
		.dbg_name = "mmpll0_pll",
		.ops = &clk_ops_pll_vote,
		CLK_INIT(mmpll0_pll.c),
	},
};

static struct pll_vote_clk mmpll1_pll = {
	.en_reg = (void __iomem *)MMSS_PLL_VOTE_APCS,
	.en_mask = BIT(1),
	.status_reg = (void __iomem *)MMPLL1_PLL_STATUS,
	.status_mask = BIT(17),
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.rate = 1000000000,
		.parent = &xo.c,
		.dbg_name = "mmpll1_pll",
		.ops = &clk_ops_pll_vote,
		CLK_INIT(mmpll1_pll.c),
	},
};

static struct clk_freq_tbl ftbl_mmss_mmssnoc_axi_clk[] = {
	F_MMSS(  19200000,         xo,   1,    0,    0),
	F_MMSS(  37500000,      gpll0,  16,    0,    0),
	F_MMSS(  50000000,      gpll0,  12,    0,    0),
	F_MMSS(  75000000,      gpll0,   8,    0,    0),
	F_MMSS( 100000000,      gpll0,   6,    0,    0),
	F_MMSS( 150000000,      gpll0,   4,    0,    0),
	F_MMSS( 200000000, mmpll0_pll,   4,    0,    0),
	F_MMSS( 266000000, mmpll0_pll,   3,    0,    0),
	F_END
};

static struct rcg_clk axi_clk_src = {
	.cmd_rcgr_reg = AXI_CMD_RCGR,
	.set_rate = set_rate_hid,
	.freq_tbl = ftbl_mmss_mmssnoc_axi_clk,
	.current_freq = &rcg_dummy_freq,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "axi_clk_src",
		.ops = &clk_ops_rcg,
		VDD_DIG_FMAX_MAP3(LOW, 100000000, NOMINAL, 200000000, HIGH,
		266670000),
		CLK_INIT(axi_clk_src.c),
	},
};

static struct pll_clk mmpll2_pll = {
	.mode_reg = (void __iomem *)MMPLL2_PLL_MODE,
	.status_reg = (void __iomem *)MMPLL2_PLL_STATUS,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "mmpll2_pll",
		.parent = &xo.c,
		.rate = 900000000,
		.ops = &clk_ops_local_pll,
		CLK_INIT(mmpll2_pll.c),
	},
};

static struct clk_freq_tbl ftbl_camss_csi0_1_clk[] = {
	F_MMSS( 100000000,      gpll0,   6,    0,    0),
	F_MMSS( 200000000, mmpll0_pll,   4,    0,    0),
	F_END
};

static struct rcg_clk csi0_clk_src = {
	.cmd_rcgr_reg = CSI0_CMD_RCGR,
	.set_rate = set_rate_hid,
	.freq_tbl = ftbl_camss_csi0_1_clk,
	.current_freq = &rcg_dummy_freq,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "csi0_clk_src",
		.ops = &clk_ops_rcg,
		VDD_DIG_FMAX_MAP2(LOW, 100000000, NOMINAL, 200000000),
		CLK_INIT(csi0_clk_src.c),
	},
};

static struct rcg_clk csi1_clk_src = {
	.cmd_rcgr_reg = CSI1_CMD_RCGR,
	.set_rate = set_rate_hid,
	.freq_tbl = ftbl_camss_csi0_1_clk,
	.current_freq = &rcg_dummy_freq,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "csi1_clk_src",
		.ops = &clk_ops_rcg,
		VDD_DIG_FMAX_MAP2(LOW, 100000000, NOMINAL, 200000000),
		CLK_INIT(csi1_clk_src.c),
	},
};

static struct clk_freq_tbl ftbl_camss_vfe_vfe0_clk[] = {
	F_MMSS(  37500000,      gpll0,  16,    0,    0),
	F_MMSS(  50000000,      gpll0,  12,    0,    0),
	F_MMSS(  60000000,      gpll0,  10,    0,    0),
	F_MMSS(  80000000,      gpll0, 7.5,    0,    0),
	F_MMSS( 100000000,      gpll0,   6,    0,    0),
	F_MMSS( 109090000,      gpll0, 5.5,    0,    0),
	F_MMSS( 133330000,      gpll0, 4.5,    0,    0),
	F_MMSS( 200000000,      gpll0,   3,    0,    0),
	F_MMSS( 228570000, mmpll0_pll, 3.5,    0,    0),
	F_MMSS( 266670000, mmpll0_pll,   3,    0,    0),
	F_MMSS( 320000000, mmpll0_pll, 2.5,    0,    0),
	F_END
};

static struct rcg_clk vfe0_clk_src = {
	.cmd_rcgr_reg = VFE0_CMD_RCGR,
	.set_rate = set_rate_hid,
	.freq_tbl = ftbl_camss_vfe_vfe0_clk,
	.current_freq = &rcg_dummy_freq,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "vfe0_clk_src",
		.ops = &clk_ops_rcg,
		VDD_DIG_FMAX_MAP3(LOW, 133330000, NOMINAL, 266670000, HIGH,
		320000000),
		CLK_INIT(vfe0_clk_src.c),
	},
};

static struct clk_freq_tbl ftbl_mdss_mdp_clk[] = {
	F_MMSS(  37500000,      gpll0,  16,    0,    0),
	F_MMSS(  60000000,      gpll0,  10,    0,    0),
	F_MMSS(  75000000,      gpll0,   8,    0,    0),
	F_MMSS(  92310000,      gpll0, 6.5,    0,    0),
	F_MMSS( 100000000,      gpll0,   6,    0,    0),
	F_MMSS( 133330000, mmpll0_pll,   6,    0,    0),
	F_MMSS( 177780000, mmpll0_pll, 4.5,    0,    0),
	F_MMSS( 200000000, mmpll0_pll,   4,    0,    0),
	F_END
};

static struct rcg_clk mdp_clk_src = {
	.cmd_rcgr_reg = MDP_CMD_RCGR,
	.set_rate = set_rate_hid,
	.freq_tbl = ftbl_mdss_mdp_clk,
	.current_freq = &rcg_dummy_freq,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "mdp_clk_src",
		.ops = &clk_ops_rcg,
		VDD_DIG_FMAX_MAP3(LOW, 92310000, NOMINAL, 177780000, HIGH,
		200000000),
		CLK_INIT(mdp_clk_src.c),
	},
};

static struct clk_freq_tbl ftbl_camss_jpeg_jpeg0_clk[] = {
	F_MMSS(  75000000,      gpll0,   8,    0,    0),
	F_MMSS( 133330000,      gpll0, 4.5,    0,    0),
	F_MMSS( 200000000,      gpll0,   3,    0,    0),
	F_MMSS( 228570000, mmpll0_pll, 3.5,    0,    0),
	F_MMSS( 266670000, mmpll0_pll,   3,    0,    0),
	F_MMSS( 320000000, mmpll0_pll, 2.5,    0,    0),
	F_END
};

static struct rcg_clk jpeg0_clk_src = {
	.cmd_rcgr_reg = JPEG0_CMD_RCGR,
	.set_rate = set_rate_hid,
	.freq_tbl = ftbl_camss_jpeg_jpeg0_clk,
	.current_freq = &rcg_dummy_freq,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "jpeg0_clk_src",
		.ops = &clk_ops_rcg,
		VDD_DIG_FMAX_MAP3(LOW, 133330000, NOMINAL, 266670000, HIGH,
		320000000),
		CLK_INIT(jpeg0_clk_src.c),
	},
};

static struct clk_freq_tbl ftbl_mdss_pclk0_clk[] = {
	F_MDSS(  83000000, dsipll_667,   8,    0,    0),
	F_MDSS( 166000000, dsipll_667,   4,    0,    0),
	F_END
};

static struct rcg_clk pclk0_clk_src = {
	.cmd_rcgr_reg = PCLK0_CMD_RCGR,
	.set_rate = set_rate_mnd,
	.freq_tbl = ftbl_mdss_pclk0_clk,
	.current_freq = &rcg_dummy_freq,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "pclk0_clk_src",
		.ops = &clk_ops_rcg_mnd,
		VDD_DIG_FMAX_MAP2(LOW, 83330000, NOMINAL, 166670000),
		CLK_INIT(pclk0_clk_src.c),
	},
};

static struct clk_freq_tbl ftbl_venus0_vcodec0_clk[] = {
	F_MMSS(  66700000,      gpll0,   9,    0,    0),
	F_MMSS( 100000000,      gpll0,   6,    0,    0),
	F_MMSS( 133330000, mmpll0_pll,   6,    0,    0),
	F_END
};

static struct rcg_clk vcodec0_clk_src = {
	.cmd_rcgr_reg = VCODEC0_CMD_RCGR,
	.set_rate = set_rate_mnd,
	.freq_tbl = ftbl_venus0_vcodec0_clk,
	.current_freq = &rcg_dummy_freq,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "vcodec0_clk_src",
		.ops = &clk_ops_rcg_mnd,
		VDD_DIG_FMAX_MAP3(LOW, 66670000, NOMINAL, 133330000, HIGH,
		160000000),
		CLK_INIT(vcodec0_clk_src.c),
	},
};

static struct clk_freq_tbl ftbl_camss_cci_cci_clk[] = {
	F_MMSS(  19200000,         xo,   1,    0,    0),
	F_END
};

static struct rcg_clk cci_clk_src = {
	.cmd_rcgr_reg = CCI_CMD_RCGR,
	.set_rate = set_rate_mnd,
	.freq_tbl = ftbl_camss_cci_cci_clk,
	.current_freq = &rcg_dummy_freq,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "cci_clk_src",
		.ops = &clk_ops_rcg_mnd,
		VDD_DIG_FMAX_MAP2(LOW, 20000000, NOMINAL, 40000000),
		CLK_INIT(cci_clk_src.c),
	},
};

static struct clk_freq_tbl ftbl_camss_gp0_1_clk[] = {
	F_MMSS(     10000,         xo,  16,    1,  120),
	F_MMSS(     24000,         xo,  16,    1,   50),
	F_MMSS(   6000000,      gpll0,  10,    1,   10),
	F_MMSS(  12000000,      gpll0,  10,    1,    5),
	F_MMSS(  13000000,      gpll0,   4,   13,  150),
	F_MMSS(  24000000,      gpll0,   5,    1,    5),
	F_END
};

static struct rcg_clk mmss_gp0_clk_src = {
	.cmd_rcgr_reg = MMSS_GP0_CMD_RCGR,
	.set_rate = set_rate_mnd,
	.freq_tbl = ftbl_camss_gp0_1_clk,
	.current_freq = &rcg_dummy_freq,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "mmss_gp0_clk_src",
		.ops = &clk_ops_rcg_mnd,
		VDD_DIG_FMAX_MAP2(LOW, 100000000, NOMINAL, 200000000),
		CLK_INIT(mmss_gp0_clk_src.c),
	},
};

static struct rcg_clk mmss_gp1_clk_src = {
	.cmd_rcgr_reg = MMSS_GP1_CMD_RCGR,
	.set_rate = set_rate_mnd,
	.freq_tbl = ftbl_camss_gp0_1_clk,
	.current_freq = &rcg_dummy_freq,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "mmss_gp1_clk_src",
		.ops = &clk_ops_rcg_mnd,
		VDD_DIG_FMAX_MAP2(LOW, 100000000, NOMINAL, 200000000),
		CLK_INIT(mmss_gp1_clk_src.c),
	},
};

static struct clk_freq_tbl ftbl_camss_mclk0_1_clk[] = {
	F_MMSS(  66670000,      gpll0,   9,    0,    0),
	F_END
};

static struct rcg_clk mclk0_clk_src = {
	.cmd_rcgr_reg = MCLK0_CMD_RCGR,
	.set_rate = set_rate_mnd,
	.freq_tbl = ftbl_camss_mclk0_1_clk,
	.current_freq = &rcg_dummy_freq,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "mclk0_clk_src",
		.ops = &clk_ops_rcg_mnd,
		VDD_DIG_FMAX_MAP1(LOW, 66670000),
		CLK_INIT(mclk0_clk_src.c),
	},
};

static struct rcg_clk mclk1_clk_src = {
	.cmd_rcgr_reg = MCLK1_CMD_RCGR,
	.set_rate = set_rate_mnd,
	.freq_tbl = ftbl_camss_mclk0_1_clk,
	.current_freq = &rcg_dummy_freq,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "mclk1_clk_src",
		.ops = &clk_ops_rcg_mnd,
		VDD_DIG_FMAX_MAP1(LOW, 66670000),
		CLK_INIT(mclk1_clk_src.c),
	},
};

static struct clk_freq_tbl ftbl_camss_phy0_1_csi0_1phytimer_clk[] = {
	F_MMSS( 100000000,      gpll0,   6,    0,    0),
	F_MMSS( 200000000, mmpll0_pll,   4,    0,    0),
	F_END
};

static struct rcg_clk csi0phytimer_clk_src = {
	.cmd_rcgr_reg = CSI0PHYTIMER_CMD_RCGR,
	.set_rate = set_rate_hid,
	.freq_tbl = ftbl_camss_phy0_1_csi0_1phytimer_clk,
	.current_freq = &rcg_dummy_freq,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "csi0phytimer_clk_src",
		.ops = &clk_ops_rcg,
		VDD_DIG_FMAX_MAP2(LOW, 100000000, NOMINAL, 200000000),
		CLK_INIT(csi0phytimer_clk_src.c),
	},
};

static struct rcg_clk csi1phytimer_clk_src = {
	.cmd_rcgr_reg = CSI1PHYTIMER_CMD_RCGR,
	.set_rate = set_rate_hid,
	.freq_tbl = ftbl_camss_phy0_1_csi0_1phytimer_clk,
	.current_freq = &rcg_dummy_freq,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "csi1phytimer_clk_src",
		.ops = &clk_ops_rcg,
		VDD_DIG_FMAX_MAP2(LOW, 100000000, NOMINAL, 200000000),
		CLK_INIT(csi1phytimer_clk_src.c),
	},
};

static struct clk_freq_tbl ftbl_camss_vfe_cpp_clk[] = {
	F_MMSS( 133330000,      gpll0, 4.5,    0,    0),
	F_MMSS( 266670000, mmpll0_pll,   3,    0,    0),
	F_MMSS( 320000000, mmpll0_pll, 2.5,    0,    0),
	F_END
};

static struct rcg_clk cpp_clk_src = {
	.cmd_rcgr_reg = CPP_CMD_RCGR,
	.set_rate = set_rate_hid,
	.freq_tbl = ftbl_camss_vfe_cpp_clk,
	.current_freq = &rcg_dummy_freq,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "cpp_clk_src",
		.ops = &clk_ops_rcg,
		VDD_DIG_FMAX_MAP3(LOW, 133330000, NOMINAL, 266670000, HIGH,
		320000000),
		CLK_INIT(cpp_clk_src.c),
	},
};

static struct clk_freq_tbl ftbl_mdss_byte0_clk[] = {
	F_MDSS(  62500000, dsipll_750,  12,    0,    0),
	F_MDSS( 125000000, dsipll_750,   6,    0,    0),
	F_END
};

static struct rcg_clk byte0_clk_src = {
	.cmd_rcgr_reg = BYTE0_CMD_RCGR,
	.set_rate = set_rate_hid,
	.freq_tbl = ftbl_mdss_byte0_clk,
	.current_freq = &rcg_dummy_freq,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "byte0_clk_src",
		.ops = &clk_ops_rcg,
		VDD_DIG_FMAX_MAP2(LOW, 62500000, NOMINAL, 125000000),
		CLK_INIT(byte0_clk_src.c),
	},
};

static struct clk_freq_tbl ftbl_mdss_esc0_clk[] = {
	F_MDSS(  19200000,         xo,   1,    0,    0),
	F_END
};

static struct rcg_clk esc0_clk_src = {
	.cmd_rcgr_reg = ESC0_CMD_RCGR,
	.set_rate = set_rate_hid,
	.freq_tbl = ftbl_mdss_esc0_clk,
	.current_freq = &rcg_dummy_freq,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "esc0_clk_src",
		.ops = &clk_ops_rcg,
		VDD_DIG_FMAX_MAP2(LOW, 20000000, NOMINAL, 40000000),
		CLK_INIT(esc0_clk_src.c),
	},
};

static struct clk_freq_tbl ftbl_mdss_vsync_clk[] = {
	F_MDSS(  19200000,         xo,   1,    0,    0),
	F_END
};

static struct rcg_clk vsync_clk_src = {
	.cmd_rcgr_reg = VSYNC_CMD_RCGR,
	.set_rate = set_rate_hid,
	.freq_tbl = ftbl_mdss_vsync_clk,
	.current_freq = &rcg_dummy_freq,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "vsync_clk_src",
		.ops = &clk_ops_rcg,
		VDD_DIG_FMAX_MAP2(LOW, 20000000, NOMINAL, 40000000),
		CLK_INIT(vsync_clk_src.c),
	},
};

static struct branch_clk camss_cci_cci_ahb_clk = {
	.cbcr_reg = CAMSS_CCI_CCI_AHB_CBCR,
	.has_sibling = 1,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "camss_cci_cci_ahb_clk",
		.ops = &clk_ops_branch,
		CLK_INIT(camss_cci_cci_ahb_clk.c),
	},
};

static struct branch_clk camss_cci_cci_clk = {
	.cbcr_reg = CAMSS_CCI_CCI_CBCR,
	.has_sibling = 0,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "camss_cci_cci_clk",
		.parent = &cci_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(camss_cci_cci_clk.c),
	},
};

static struct branch_clk camss_csi0_ahb_clk = {
	.cbcr_reg = CAMSS_CSI0_AHB_CBCR,
	.has_sibling = 1,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "camss_csi0_ahb_clk",
		.ops = &clk_ops_branch,
		CLK_INIT(camss_csi0_ahb_clk.c),
	},
};

static struct branch_clk camss_csi0_clk = {
	.cbcr_reg = CAMSS_CSI0_CBCR,
	.has_sibling = 1,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "camss_csi0_clk",
		.parent = &csi0_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(camss_csi0_clk.c),
	},
};

static struct branch_clk camss_csi0phy_clk = {
	.cbcr_reg = CAMSS_CSI0PHY_CBCR,
	.has_sibling = 1,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "camss_csi0phy_clk",
		.parent = &csi0_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(camss_csi0phy_clk.c),
	},
};

static struct branch_clk camss_csi0pix_clk = {
	.cbcr_reg = CAMSS_CSI0PIX_CBCR,
	.has_sibling = 1,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "camss_csi0pix_clk",
		.parent = &csi0_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(camss_csi0pix_clk.c),
	},
};

static struct branch_clk camss_csi0rdi_clk = {
	.cbcr_reg = CAMSS_CSI0RDI_CBCR,
	.has_sibling = 1,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "camss_csi0rdi_clk",
		.parent = &csi0_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(camss_csi0rdi_clk.c),
	},
};

static struct branch_clk camss_csi1_ahb_clk = {
	.cbcr_reg = CAMSS_CSI1_AHB_CBCR,
	.has_sibling = 1,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "camss_csi1_ahb_clk",
		.ops = &clk_ops_branch,
		CLK_INIT(camss_csi1_ahb_clk.c),
	},
};

static struct branch_clk camss_csi1_clk = {
	.cbcr_reg = CAMSS_CSI1_CBCR,
	.has_sibling = 1,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "camss_csi1_clk",
		.parent = &csi1_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(camss_csi1_clk.c),
	},
};

static struct branch_clk camss_csi1phy_clk = {
	.cbcr_reg = CAMSS_CSI1PHY_CBCR,
	.has_sibling = 1,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "camss_csi1phy_clk",
		.parent = &csi1_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(camss_csi1phy_clk.c),
	},
};

static struct branch_clk camss_csi1pix_clk = {
	.cbcr_reg = CAMSS_CSI1PIX_CBCR,
	.has_sibling = 1,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "camss_csi1pix_clk",
		.parent = &csi1_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(camss_csi1pix_clk.c),
	},
};

static struct branch_clk camss_csi1rdi_clk = {
	.cbcr_reg = CAMSS_CSI1RDI_CBCR,
	.has_sibling = 1,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "camss_csi1rdi_clk",
		.parent = &csi1_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(camss_csi1rdi_clk.c),
	},
};

static struct branch_clk camss_csi_vfe0_clk = {
	.cbcr_reg = CAMSS_CSI_VFE0_CBCR,
	.has_sibling = 1,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "camss_csi_vfe0_clk",
		.parent = &vfe0_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(camss_csi_vfe0_clk.c),
	},
};

static struct branch_clk camss_gp0_clk = {
	.cbcr_reg = CAMSS_GP0_CBCR,
	.has_sibling = 0,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "camss_gp0_clk",
		.parent = &mmss_gp0_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(camss_gp0_clk.c),
	},
};

static struct branch_clk camss_gp1_clk = {
	.cbcr_reg = CAMSS_GP1_CBCR,
	.has_sibling = 0,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "camss_gp1_clk",
		.parent = &mmss_gp1_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(camss_gp1_clk.c),
	},
};

static struct branch_clk camss_ispif_ahb_clk = {
	.cbcr_reg = CAMSS_ISPIF_AHB_CBCR,
	.has_sibling = 1,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "camss_ispif_ahb_clk",
		.ops = &clk_ops_branch,
		CLK_INIT(camss_ispif_ahb_clk.c),
	},
};

static struct branch_clk camss_jpeg_jpeg0_clk = {
	.cbcr_reg = CAMSS_JPEG_JPEG0_CBCR,
	.has_sibling = 0,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "camss_jpeg_jpeg0_clk",
		.parent = &jpeg0_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(camss_jpeg_jpeg0_clk.c),
	},
};

static struct branch_clk camss_jpeg_jpeg_ahb_clk = {
	.cbcr_reg = CAMSS_JPEG_JPEG_AHB_CBCR,
	.has_sibling = 1,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "camss_jpeg_jpeg_ahb_clk",
		.ops = &clk_ops_branch,
		CLK_INIT(camss_jpeg_jpeg_ahb_clk.c),
	},
};

static struct branch_clk camss_jpeg_jpeg_axi_clk = {
	.cbcr_reg = CAMSS_JPEG_JPEG_AXI_CBCR,
	.has_sibling = 1,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "camss_jpeg_jpeg_axi_clk",
		.parent = &axi_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(camss_jpeg_jpeg_axi_clk.c),
	},
};

static struct branch_clk camss_mclk0_clk = {
	.cbcr_reg = CAMSS_MCLK0_CBCR,
	.has_sibling = 0,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "camss_mclk0_clk",
		.parent = &mclk0_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(camss_mclk0_clk.c),
	},
};

static struct branch_clk camss_mclk1_clk = {
	.cbcr_reg = CAMSS_MCLK1_CBCR,
	.has_sibling = 0,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "camss_mclk1_clk",
		.parent = &mclk1_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(camss_mclk1_clk.c),
	},
};

static struct branch_clk camss_micro_ahb_clk = {
	.cbcr_reg = CAMSS_MICRO_AHB_CBCR,
	.has_sibling = 1,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "camss_micro_ahb_clk",
		.ops = &clk_ops_branch,
		CLK_INIT(camss_micro_ahb_clk.c),
	},
};

static struct branch_clk camss_phy0_csi0phytimer_clk = {
	.cbcr_reg = CAMSS_PHY0_CSI0PHYTIMER_CBCR,
	.has_sibling = 0,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "camss_phy0_csi0phytimer_clk",
		.parent = &csi0phytimer_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(camss_phy0_csi0phytimer_clk.c),
	},
};

static struct branch_clk camss_phy1_csi1phytimer_clk = {
	.cbcr_reg = CAMSS_PHY1_CSI1PHYTIMER_CBCR,
	.has_sibling = 0,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "camss_phy1_csi1phytimer_clk",
		.parent = &csi1phytimer_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(camss_phy1_csi1phytimer_clk.c),
	},
};

static struct branch_clk camss_top_ahb_clk = {
	.cbcr_reg = CAMSS_TOP_AHB_CBCR,
	.has_sibling = 1,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "camss_top_ahb_clk",
		.ops = &clk_ops_branch,
		CLK_INIT(camss_top_ahb_clk.c),
	},
};

static struct branch_clk camss_vfe_cpp_ahb_clk = {
	.cbcr_reg = CAMSS_VFE_CPP_AHB_CBCR,
	.has_sibling = 1,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "camss_vfe_cpp_ahb_clk",
		.ops = &clk_ops_branch,
		CLK_INIT(camss_vfe_cpp_ahb_clk.c),
	},
};

static struct branch_clk camss_vfe_cpp_clk = {
	.cbcr_reg = CAMSS_VFE_CPP_CBCR,
	.has_sibling = 0,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "camss_vfe_cpp_clk",
		.parent = &cpp_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(camss_vfe_cpp_clk.c),
	},
};

static struct branch_clk camss_vfe_vfe0_clk = {
	.cbcr_reg = CAMSS_VFE_VFE0_CBCR,
	.has_sibling = 1,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "camss_vfe_vfe0_clk",
		.parent = &vfe0_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(camss_vfe_vfe0_clk.c),
	},
};

static struct branch_clk camss_vfe_vfe_ahb_clk = {
	.cbcr_reg = CAMSS_VFE_VFE_AHB_CBCR,
	.has_sibling = 1,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "camss_vfe_vfe_ahb_clk",
		.ops = &clk_ops_branch,
		CLK_INIT(camss_vfe_vfe_ahb_clk.c),
	},
};

static struct branch_clk camss_vfe_vfe_axi_clk = {
	.cbcr_reg = CAMSS_VFE_VFE_AXI_CBCR,
	.has_sibling = 1,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "camss_vfe_vfe_axi_clk",
		.parent = &axi_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(camss_vfe_vfe_axi_clk.c),
	},
};

static struct branch_clk mdss_ahb_clk = {
	.cbcr_reg = MDSS_AHB_CBCR,
	.has_sibling = 1,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "mdss_ahb_clk",
		.ops = &clk_ops_branch,
		CLK_INIT(mdss_ahb_clk.c),
	},
};

static struct branch_clk mdss_axi_clk = {
	.cbcr_reg = MDSS_AXI_CBCR,
	.has_sibling = 1,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "mdss_axi_clk",
		.parent = &axi_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(mdss_axi_clk.c),
	},
};

static struct branch_clk mdss_byte0_clk = {
	.cbcr_reg = MDSS_BYTE0_CBCR,
	.has_sibling = 0,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "mdss_byte0_clk",
		.parent = &byte0_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(mdss_byte0_clk.c),
	},
};

static struct branch_clk mdss_esc0_clk = {
	.cbcr_reg = MDSS_ESC0_CBCR,
	.has_sibling = 0,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "mdss_esc0_clk",
		.parent = &esc0_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(mdss_esc0_clk.c),
	},
};

static struct branch_clk mdss_mdp_clk = {
	.cbcr_reg = MDSS_MDP_CBCR,
	.has_sibling = 1,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "mdss_mdp_clk",
		.parent = &mdp_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(mdss_mdp_clk.c),
	},
};

static struct branch_clk mdss_mdp_lut_clk = {
	.cbcr_reg = MDSS_MDP_LUT_CBCR,
	.has_sibling = 1,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "mdss_mdp_lut_clk",
		.parent = &mdp_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(mdss_mdp_lut_clk.c),
	},
};

static struct branch_clk mdss_pclk0_clk = {
	.cbcr_reg = MDSS_PCLK0_CBCR,
	.has_sibling = 0,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "mdss_pclk0_clk",
		.parent = &pclk0_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(mdss_pclk0_clk.c),
	},
};

static struct branch_clk mdss_vsync_clk = {
	.cbcr_reg = MDSS_VSYNC_CBCR,
	.has_sibling = 0,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "mdss_vsync_clk",
		.parent = &vsync_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(mdss_vsync_clk.c),
	},
};

static struct branch_clk mmss_misc_ahb_clk = {
	.cbcr_reg = MMSS_MISC_AHB_CBCR,
	.has_sibling = 1,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "mmss_misc_ahb_clk",
		.ops = &clk_ops_branch,
		CLK_INIT(mmss_misc_ahb_clk.c),
	},
};

static struct branch_clk mmss_mmssnoc_ahb_clk = {
	.cbcr_reg = MMSS_MMSSNOC_AHB_CBCR,
	.has_sibling = 1,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "mmss_mmssnoc_ahb_clk",
		.ops = &clk_ops_branch,
		CLK_INIT(mmss_mmssnoc_ahb_clk.c),
	},
};

static struct branch_clk mmss_mmssnoc_bto_ahb_clk = {
	.cbcr_reg = MMSS_MMSSNOC_BTO_AHB_CBCR,
	.has_sibling = 1,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "mmss_mmssnoc_bto_ahb_clk",
		.ops = &clk_ops_branch,
		CLK_INIT(mmss_mmssnoc_bto_ahb_clk.c),
	},
};

static struct branch_clk mmss_mmssnoc_axi_clk = {
	.cbcr_reg = MMSS_MMSSNOC_AXI_CBCR,
	.has_sibling = 1,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "mmss_mmssnoc_axi_clk",
		.parent = &axi_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(mmss_mmssnoc_axi_clk.c),
	},
};

static struct branch_clk mmss_s0_axi_clk = {
	.cbcr_reg = MMSS_S0_AXI_CBCR,
	.has_sibling = 0,
	.max_div = 0,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "mmss_s0_axi_clk",
		.parent = &axi_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(mmss_s0_axi_clk.c),
		.depends = &mmss_mmssnoc_axi_clk.c,
	},
};

static struct branch_clk ocmemcx_ahb_clk = {
	.cbcr_reg = OCMEMCX_AHB_CBCR,
	.has_sibling = 1,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "ocmemcx_ahb_clk",
		.ops = &clk_ops_branch,
		CLK_INIT(ocmemcx_ahb_clk.c),
	},
};

static struct branch_clk oxili_gfx3d_clk = {
	.cbcr_reg = OXILI_GFX3D_CBCR,
	.has_sibling = 1,
	.max_div = 0,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "oxili_gfx3d_clk",
		.parent = &gfx3d_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(oxili_gfx3d_clk.c),
		.depends = &oxilicx_axi_clk.c,
	},
};

static struct branch_clk oxilicx_ahb_clk = {
	.cbcr_reg = OXILICX_AHB_CBCR,
	.has_sibling = 1,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "oxilicx_ahb_clk",
		.ops = &clk_ops_branch,
		CLK_INIT(oxilicx_ahb_clk.c),
	},
};

static struct branch_clk oxilicx_axi_clk = {
	.cbcr_reg = OXILICX_AXI_CBCR,
	.has_sibling = 1,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "oxilicx_axi_clk",
		.parent = &axi_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(oxilicx_axi_clk.c),
	},
};

static struct branch_clk venus0_ahb_clk = {
	.cbcr_reg = VENUS0_AHB_CBCR,
	.has_sibling = 1,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "venus0_ahb_clk",
		.ops = &clk_ops_branch,
		CLK_INIT(venus0_ahb_clk.c),
	},
};

static struct branch_clk venus0_axi_clk = {
	.cbcr_reg = VENUS0_AXI_CBCR,
	.has_sibling = 1,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "venus0_axi_clk",
		.parent = &axi_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(venus0_axi_clk.c),
	},
};

static struct branch_clk venus0_vcodec0_clk = {
	.cbcr_reg = VENUS0_VCODEC0_CBCR,
	.has_sibling = 0,
	.base = &virt_bases[MMSS_BASE],
	.c = {
		.dbg_name = "venus0_vcodec0_clk",
		.parent = &vcodec0_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(venus0_vcodec0_clk.c),
	},
};

static struct measure_mux_entry measure_mux_MMSS[] = {
	{ &mmss_mmssnoc_ahb_clk.c,  MMSS_BASE, 0x0001 },
	{ &mmss_mmssnoc_bto_ahb_clk.c,  MMSS_BASE, 0x0002 },
	{ &mmss_misc_ahb_clk.c,  MMSS_BASE, 0x0003 },
	{ &mmss_mmssnoc_axi_clk.c,  MMSS_BASE, 0x0004 },
	{ &mmss_s0_axi_clk.c,  MMSS_BASE, 0x0005 },
	{ &ocmemcx_ahb_clk.c,  MMSS_BASE, 0x000a },
	{ &oxilicx_axi_clk.c,  MMSS_BASE, 0x000b },
	{ &oxilicx_ahb_clk.c,  MMSS_BASE, 0x000c },
	{ &oxili_gfx3d_clk.c,  MMSS_BASE, 0x000d },
	{ &venus0_vcodec0_clk.c,  MMSS_BASE, 0x000e },
	{ &venus0_axi_clk.c,  MMSS_BASE, 0x000f },
	{ &venus0_ahb_clk.c,  MMSS_BASE, 0x0011 },
	{ &mdss_mdp_clk.c,  MMSS_BASE, 0x0014 },
	{ &mdss_mdp_lut_clk.c,  MMSS_BASE, 0x0015 },
	{ &mdss_pclk0_clk.c,  MMSS_BASE, 0x0016 },
	{ &mdss_vsync_clk.c,  MMSS_BASE, 0x001c },
	{ &mdss_byte0_clk.c,  MMSS_BASE, 0x001e },
	{ &mdss_esc0_clk.c,  MMSS_BASE, 0x0020 },
	{ &mdss_ahb_clk.c,  MMSS_BASE, 0x0022 },
	{ &mdss_axi_clk.c,  MMSS_BASE, 0x0024 },
	{ &camss_top_ahb_clk.c,  MMSS_BASE, 0x0025 },
	{ &camss_micro_ahb_clk.c,  MMSS_BASE, 0x0026 },
	{ &camss_gp0_clk.c,  MMSS_BASE, 0x0027 },
	{ &camss_gp1_clk.c,  MMSS_BASE, 0x0028 },
	{ &camss_mclk0_clk.c,  MMSS_BASE, 0x0029 },
	{ &camss_mclk1_clk.c,  MMSS_BASE, 0x002a },
	{ &camss_cci_cci_clk.c,  MMSS_BASE, 0x002d },
	{ &camss_cci_cci_ahb_clk.c,  MMSS_BASE, 0x002e },
	{ &camss_phy0_csi0phytimer_clk.c,  MMSS_BASE, 0x002f },
	{ &camss_phy1_csi1phytimer_clk.c,  MMSS_BASE, 0x0030 },
	{ &camss_jpeg_jpeg0_clk.c,  MMSS_BASE, 0x0032 },
	{ &camss_jpeg_jpeg_ahb_clk.c,  MMSS_BASE, 0x0035 },
	{ &camss_jpeg_jpeg_axi_clk.c,  MMSS_BASE, 0x0036 },
	{ &camss_vfe_vfe0_clk.c,  MMSS_BASE, 0x0038 },
	{ &camss_vfe_cpp_clk.c,  MMSS_BASE, 0x003a },
	{ &camss_vfe_cpp_ahb_clk.c,  MMSS_BASE, 0x003b },
	{ &camss_vfe_vfe_ahb_clk.c,  MMSS_BASE, 0x003c },
	{ &camss_vfe_vfe_axi_clk.c,  MMSS_BASE, 0x003d },
	{ &camss_csi_vfe0_clk.c,  MMSS_BASE, 0x003f },
	{ &camss_csi0_clk.c,  MMSS_BASE, 0x0041 },
	{ &camss_csi0_ahb_clk.c,  MMSS_BASE, 0x0042 },
	{ &camss_csi0phy_clk.c,  MMSS_BASE, 0x0043 },
	{ &camss_csi0rdi_clk.c,  MMSS_BASE, 0x0044 },
	{ &camss_csi0pix_clk.c,  MMSS_BASE, 0x0045 },
	{ &camss_csi1_clk.c,  MMSS_BASE, 0x0046 },
	{ &camss_csi1_ahb_clk.c,  MMSS_BASE, 0x0047 },
	{ &camss_csi1phy_clk.c,  MMSS_BASE, 0x0048 },
	{ &camss_csi1rdi_clk.c,  MMSS_BASE, 0x0049 },
	{ &camss_csi1pix_clk.c,  MMSS_BASE, 0x004a },
	{ &camss_ispif_ahb_clk.c,  MMSS_BASE, 0x0055 },
	{&dummy_clk, N_BASES, 0x0000},
};

static struct pll_vote_clk lpaaudio_pll = {
	.en_reg = (void __iomem *)LPA_PLL_VOTE_APPS,
	.en_mask = BIT(0),
	.status_reg = (void __iomem *)LPAAUDIO_PLL_STATUS,
	.status_mask = BIT(17),
	.base = &virt_bases[LPASS_BASE],
	.c = {
		.rate = 491520000,
		.parent = &xo.c,
		.dbg_name = "lpaaudio_pll",
		.ops = &clk_ops_pll_vote,
		CLK_INIT(lpaaudio_pll.c),
	},
};

static struct clk_freq_tbl ftbl_audio_core_lpaif__osr_clk[] = {
	F_LPASS(    512000, lpaaudio_pll,  16,    1,   60),
	F_LPASS(    768000, lpaaudio_pll,  16,    1,   40),
	F_LPASS(   1024000, lpaaudio_pll,  16,    1,   30),
	F_LPASS(   1536000, lpaaudio_pll,  16,    1,   20),
	F_LPASS(   2048000, lpaaudio_pll,  16,    1,   15),
	F_LPASS(   3072000, lpaaudio_pll,  16,    1,   10),
	F_LPASS(   4096000, lpaaudio_pll,  15,    1,    8),
	F_LPASS(   6144000, lpaaudio_pll,  10,    1,    8),
	F_LPASS(   8192000, lpaaudio_pll,  15,    1,    4),
	F_LPASS(  12288000, lpaaudio_pll,  10,    1,    4),
	F_END
};

static struct rcg_clk lpaif_pri_clk_src = {
	.cmd_rcgr_reg = LPAIF_PRI_CMD_RCGR,
	.set_rate = set_rate_mnd,
	.freq_tbl = ftbl_audio_core_lpaif__osr_clk,
	.current_freq = &rcg_dummy_freq,
	.base = &virt_bases[LPASS_BASE],
	.c = {
		.dbg_name = "lpaif_pri_clk_src",
		.ops = &clk_ops_rcg_mnd,
		VDD_DIG_FMAX_MAP2(LOW, 12290000, NOMINAL, 24580000),
		CLK_INIT(lpaif_pri_clk_src.c),
	},
};

static struct rcg_clk lpaif_quad_clk_src = {
	.cmd_rcgr_reg = LPAIF_QUAD_CMD_RCGR,
	.set_rate = set_rate_mnd,
	.freq_tbl = ftbl_audio_core_lpaif__osr_clk,
	.current_freq = &rcg_dummy_freq,
	.base = &virt_bases[LPASS_BASE],
	.c = {
		.dbg_name = "lpaif_quad_clk_src",
		.ops = &clk_ops_rcg_mnd,
		VDD_DIG_FMAX_MAP2(LOW, 12290000, NOMINAL, 24580000),
		CLK_INIT(lpaif_quad_clk_src.c),
	},
};

static struct rcg_clk lpaif_sec_clk_src = {
	.cmd_rcgr_reg = LPAIF_SEC_CMD_RCGR,
	.set_rate = set_rate_mnd,
	.freq_tbl = ftbl_audio_core_lpaif__osr_clk,
	.current_freq = &rcg_dummy_freq,
	.base = &virt_bases[LPASS_BASE],
	.c = {
		.dbg_name = "lpaif_sec_clk_src",
		.ops = &clk_ops_rcg_mnd,
		VDD_DIG_FMAX_MAP2(LOW, 12290000, NOMINAL, 24580000),
		CLK_INIT(lpaif_sec_clk_src.c),
	},
};

static struct rcg_clk lpaif_spkr_clk_src = {
	.cmd_rcgr_reg = LPAIF_SPKR_CMD_RCGR,
	.set_rate = set_rate_mnd,
	.freq_tbl = ftbl_audio_core_lpaif__osr_clk,
	.current_freq = &rcg_dummy_freq,
	.base = &virt_bases[LPASS_BASE],
	.c = {
		.dbg_name = "lpaif_spkr_clk_src",
		.ops = &clk_ops_rcg_mnd,
		VDD_DIG_FMAX_MAP2(LOW, 12290000, NOMINAL, 24580000),
		CLK_INIT(lpaif_spkr_clk_src.c),
	},
};

static struct rcg_clk lpaif_ter_clk_src = {
	.cmd_rcgr_reg = LPAIF_TER_CMD_RCGR,
	.set_rate = set_rate_mnd,
	.freq_tbl = ftbl_audio_core_lpaif__osr_clk,
	.current_freq = &rcg_dummy_freq,
	.base = &virt_bases[LPASS_BASE],
	.c = {
		.dbg_name = "lpaif_ter_clk_src",
		.ops = &clk_ops_rcg_mnd,
		VDD_DIG_FMAX_MAP2(LOW, 12290000, NOMINAL, 24580000),
		CLK_INIT(lpaif_ter_clk_src.c),
	},
};

static struct clk_freq_tbl ftbl_audio_core_lpaif_pcmoe_clk[] = {
	F_LPASS(    512000, lpaaudio_pll,  16,    1,   60),
	F_LPASS(    768000, lpaaudio_pll,  16,    1,   40),
	F_LPASS(   1024000, lpaaudio_pll,  16,    1,   30),
	F_LPASS(   1536000, lpaaudio_pll,  16,    1,   20),
	F_LPASS(   2048000, lpaaudio_pll,  16,    1,   15),
	F_LPASS(   3072000, lpaaudio_pll,  16,    1,   10),
	F_LPASS(   4096000, lpaaudio_pll,  15,    1,    8),
	F_LPASS(   6144000, lpaaudio_pll,  10,    1,    8),
	F_LPASS(   8192000, lpaaudio_pll,  15,    1,    4),
	F_LPASS(  12288000, lpaaudio_pll,  10,    1,    4),
	F_END
};

static struct rcg_clk lpaif_pcmoe_clk_src = {
	.cmd_rcgr_reg = LPAIF_PCMOE_CMD_RCGR,
	.set_rate = set_rate_mnd,
	.freq_tbl = ftbl_audio_core_lpaif_pcmoe_clk,
	.current_freq = &rcg_dummy_freq,
	.base = &virt_bases[LPASS_BASE],
	.c = {
		.dbg_name = "lpaif_pcmoe_clk_src",
		.ops = &clk_ops_rcg_mnd,
		VDD_DIG_FMAX_MAP2(LOW, 12290000, NOMINAL, 24580000),
		CLK_INIT(lpaif_pcmoe_clk_src.c),
	},
};

static struct clk_freq_tbl ftbl_audio_core_lpaif_pcm0_1_ibit_clk[] = {
	F_LPASS(    512000, lpaaudio_pll,  16,    1,   60),
	F_LPASS(    768000, lpaaudio_pll,  16,    1,   40),
	F_LPASS(   1024000, lpaaudio_pll,  16,    1,   30),
	F_LPASS(   1536000, lpaaudio_pll,  16,    1,   20),
	F_LPASS(   2048000, lpaaudio_pll,  16,    1,   15),
	F_LPASS(   3072000, lpaaudio_pll,  16,    1,   10),
	F_LPASS(   4096000, lpaaudio_pll,  15,    1,    8),
	F_LPASS(   6144000, lpaaudio_pll,  10,    1,    8),
	F_LPASS(   8192000, lpaaudio_pll,  15,    1,    4),
	F_END
};

static struct rcg_clk lpaif_pcm0_clk_src = {
	.cmd_rcgr_reg = LPAIF_PCM0_CMD_RCGR,
	.set_rate = set_rate_mnd,
	.freq_tbl = ftbl_audio_core_lpaif_pcm0_1_ibit_clk,
	.current_freq = &rcg_dummy_freq,
	.base = &virt_bases[LPASS_BASE],
	.c = {
		.dbg_name = "lpaif_pcm0_clk_src",
		.ops = &clk_ops_rcg_mnd,
		VDD_DIG_FMAX_MAP2(LOW, 4100000, NOMINAL, 8190000),
		CLK_INIT(lpaif_pcm0_clk_src.c),
	},
};

static struct rcg_clk lpaif_pcm1_clk_src = {
	.cmd_rcgr_reg = LPAIF_PCM1_CMD_RCGR,
	.set_rate = set_rate_mnd,
	.freq_tbl = ftbl_audio_core_lpaif_pcm0_1_ibit_clk,
	.current_freq = &rcg_dummy_freq,
	.base = &virt_bases[LPASS_BASE],
	.c = {
		.dbg_name = "lpaif_pcm1_clk_src",
		.ops = &clk_ops_rcg_mnd,
		VDD_DIG_FMAX_MAP2(LOW, 4100000, NOMINAL, 8190000),
		CLK_INIT(lpaif_pcm1_clk_src.c),
	},
};

static struct clk_freq_tbl ftbl_audio_core_slimbus_core_clk[] = {
	F_LPASS(  24576000, lpaaudio_pll,  10,    1,    2),
	F_END
};

static struct rcg_clk slimbus_clk_src = {
	.cmd_rcgr_reg = SLIMBUS_CMD_RCGR,
	.set_rate = set_rate_mnd,
	.freq_tbl = ftbl_audio_core_slimbus_core_clk,
	.current_freq = &rcg_dummy_freq,
	.base = &virt_bases[LPASS_BASE],
	.c = {
		.dbg_name = "slimbus_clk_src",
		.ops = &clk_ops_rcg_mnd,
		VDD_DIG_FMAX_MAP2(LOW, 13000000, NOMINAL, 26010000),
		CLK_INIT(slimbus_clk_src.c),
	},
};

static struct branch_clk audio_core_ixfabric_clk = {
	.cbcr_reg = AUDIO_CORE_IXFABRIC_CBCR,
	.has_sibling = 1,
	.base = &virt_bases[LPASS_BASE],
	.c = {
		.dbg_name = "audio_core_ixfabric_clk",
		.ops = &clk_ops_branch,
		CLK_INIT(audio_core_ixfabric_clk.c),
	},
};

static struct branch_clk audio_core_slimbus_lfabif_clk = {
	.cbcr_reg = AUDIO_CORE_SLIMBUS_LFABIF_CBCR,
	.has_sibling = 1,
	.base = &virt_bases[LPASS_BASE],
	.c = {
		.dbg_name = "audio_core_slimbus_lfabif_clk",
		.ops = &clk_ops_branch,
		CLK_INIT(audio_core_slimbus_lfabif_clk.c),
	},
};

static struct branch_clk audio_wrapper_br_clk = {
	.cbcr_reg = AUDIO_WRAPPER_BR_CBCR,
	.has_sibling = 1,
	.base = &virt_bases[LPASS_BASE],
	.c = {
		.dbg_name = "audio_wrapper_br_clk",
		.ops = &clk_ops_branch,
		CLK_INIT(audio_wrapper_br_clk.c),
	},
};

static struct branch_clk q6ss_ahb_lfabif_clk = {
	.cbcr_reg = Q6SS_AHB_LFABIF_CBCR,
	.has_sibling = 1,
	.base = &virt_bases[LPASS_BASE],
	.c = {
		.dbg_name = "q6ss_ahb_lfabif_clk",
		.ops = &clk_ops_branch,
		CLK_INIT(q6ss_ahb_lfabif_clk.c),
	},
};

static struct branch_clk q6ss_ahbm_clk = {
	.cbcr_reg = Q6SS_AHBM_CBCR,
	.has_sibling = 1,
	.base = &virt_bases[LPASS_BASE],
	.c = {
		.dbg_name = "q6ss_ahbm_clk",
		.ops = &clk_ops_branch,
		CLK_INIT(q6ss_ahbm_clk.c),
	},
};

static struct branch_clk audio_core_lpaif_pcmoe_clk = {
	.cbcr_reg = AUDIO_CORE_LPAIF_PCM_DATA_OE_CBCR,
	.has_sibling = 0,
	.base = &virt_bases[LPASS_BASE],
	.c = {
		.dbg_name = "audio_core_lpaif_pcmoe_clk",
		.parent = &lpaif_pcmoe_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(audio_core_lpaif_pcmoe_clk.c),
	},
};

static struct branch_clk audio_core_lpaif_pri_ebit_clk = {
	.cbcr_reg = AUDIO_CORE_LPAIF_PRI_EBIT_CBCR,
	.has_sibling = 0,
	.base = &virt_bases[LPASS_BASE],
	.c = {
		.dbg_name = "audio_core_lpaif_pri_ebit_clk",
		.ops = &clk_ops_branch,
		CLK_INIT(audio_core_lpaif_pri_ebit_clk.c),
	},
};

static struct branch_clk audio_core_lpaif_pri_ibit_clk = {
	.cbcr_reg = AUDIO_CORE_LPAIF_PRI_IBIT_CBCR,
	.has_sibling = 1,
	.max_div = 15,
	.base = &virt_bases[LPASS_BASE],
	.c = {
		.dbg_name = "audio_core_lpaif_pri_ibit_clk",
		.parent = &lpaif_pri_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(audio_core_lpaif_pri_ibit_clk.c),
	},
};

static struct branch_clk audio_core_lpaif_pri_osr_clk = {
	.cbcr_reg = AUDIO_CORE_LPAIF_PRI_OSR_CBCR,
	.has_sibling = 1,
	.base = &virt_bases[LPASS_BASE],
	.c = {
		.dbg_name = "audio_core_lpaif_pri_osr_clk",
		.parent = &lpaif_pri_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(audio_core_lpaif_pri_osr_clk.c),
	},
};

static struct branch_clk audio_core_lpaif_pcm0_ebit_clk = {
	.cbcr_reg = AUDIO_CORE_LPAIF_PCM0_EBIT_CBCR,
	.has_sibling = 0,
	.base = &virt_bases[LPASS_BASE],
	.c = {
		.dbg_name = "audio_core_lpaif_pcm0_ebit_clk",
		.ops = &clk_ops_branch,
		CLK_INIT(audio_core_lpaif_pcm0_ebit_clk.c),
	},
};

static struct branch_clk audio_core_lpaif_pcm0_ibit_clk = {
	.cbcr_reg = AUDIO_CORE_LPAIF_PCM0_IBIT_CBCR,
	.has_sibling = 0,
	.base = &virt_bases[LPASS_BASE],
	.c = {
		.dbg_name = "audio_core_lpaif_pcm0_ibit_clk",
		.parent = &lpaif_pcm0_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(audio_core_lpaif_pcm0_ibit_clk.c),
	},
};

static struct branch_clk q6ss_xo_clk = {
	.cbcr_reg = Q6SS_XO_CBCR,
	.has_sibling = 1,
	.bcr_reg = Q6SS_BCR,
	.base = &virt_bases[LPASS_BASE],
	.c = {
		.dbg_name = "q6ss_xo_clk",
		.parent = &xo.c,
		.ops = &clk_ops_branch,
		CLK_INIT(q6ss_xo_clk.c),
	},
};

static struct branch_clk audio_core_lpaif_quad_ebit_clk = {
	.cbcr_reg = AUDIO_CORE_LPAIF_QUAD_EBIT_CBCR,
	.has_sibling = 0,
	.base = &virt_bases[LPASS_BASE],
	.c = {
		.dbg_name = "audio_core_lpaif_quad_ebit_clk",
		.ops = &clk_ops_branch,
		CLK_INIT(audio_core_lpaif_quad_ebit_clk.c),
	},
};

static struct branch_clk audio_core_lpaif_quad_ibit_clk = {
	.cbcr_reg = AUDIO_CORE_LPAIF_QUAD_IBIT_CBCR,
	.has_sibling = 1,
	.max_div = 15,
	.base = &virt_bases[LPASS_BASE],
	.c = {
		.dbg_name = "audio_core_lpaif_quad_ibit_clk",
		.parent = &lpaif_quad_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(audio_core_lpaif_quad_ibit_clk.c),
	},
};

static struct branch_clk audio_core_lpaif_quad_osr_clk = {
	.cbcr_reg = AUDIO_CORE_LPAIF_QUAD_OSR_CBCR,
	.has_sibling = 1,
	.base = &virt_bases[LPASS_BASE],
	.c = {
		.dbg_name = "audio_core_lpaif_quad_osr_clk",
		.parent = &lpaif_quad_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(audio_core_lpaif_quad_osr_clk.c),
	},
};

static struct branch_clk audio_core_lpaif_sec_ebit_clk = {
	.cbcr_reg = AUDIO_CORE_LPAIF_SEC_EBIT_CBCR,
	.has_sibling = 0,
	.base = &virt_bases[LPASS_BASE],
	.c = {
		.dbg_name = "audio_core_lpaif_sec_ebit_clk",
		.ops = &clk_ops_branch,
		CLK_INIT(audio_core_lpaif_sec_ebit_clk.c),
	},
};

static struct branch_clk audio_core_lpaif_sec_ibit_clk = {
	.cbcr_reg = AUDIO_CORE_LPAIF_SEC_IBIT_CBCR,
	.has_sibling = 1,
	.max_div = 15,
	.base = &virt_bases[LPASS_BASE],
	.c = {
		.dbg_name = "audio_core_lpaif_sec_ibit_clk",
		.parent = &lpaif_sec_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(audio_core_lpaif_sec_ibit_clk.c),
	},
};

static struct branch_clk audio_core_lpaif_sec_osr_clk = {
	.cbcr_reg = AUDIO_CORE_LPAIF_SEC_OSR_CBCR,
	.has_sibling = 1,
	.base = &virt_bases[LPASS_BASE],
	.c = {
		.dbg_name = "audio_core_lpaif_sec_osr_clk",
		.parent = &lpaif_sec_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(audio_core_lpaif_sec_osr_clk.c),
	},
};

static struct branch_clk audio_core_lpaif_pcm1_ebit_clk = {
	.cbcr_reg = AUDIO_CORE_LPAIF_PCM1_EBIT_CBCR,
	.has_sibling = 0,
	.base = &virt_bases[LPASS_BASE],
	.c = {
		.dbg_name = "audio_core_lpaif_pcm1_ebit_clk",
		.ops = &clk_ops_branch,
		CLK_INIT(audio_core_lpaif_pcm1_ebit_clk.c),
	},
};

static struct branch_clk audio_core_lpaif_pcm1_ibit_clk = {
	.cbcr_reg = AUDIO_CORE_LPAIF_PCM1_IBIT_CBCR,
	.has_sibling = 0,
	.base = &virt_bases[LPASS_BASE],
	.c = {
		.dbg_name = "audio_core_lpaif_pcm1_ibit_clk",
		.parent = &lpaif_pcm1_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(audio_core_lpaif_pcm1_ibit_clk.c),
	},
};

static struct branch_clk audio_core_slimbus_core_clk = {
	.cbcr_reg = AUDIO_CORE_SLIMBUS_CORE_CBCR,
	.has_sibling = 0,
	.base = &virt_bases[LPASS_BASE],
	.c = {
		.dbg_name = "audio_core_slimbus_core_clk",
		.parent = &slimbus_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(audio_core_slimbus_core_clk.c),
	},
};

static struct branch_clk audio_core_lpaif_codec_spkr_ebit_clk = {
	.cbcr_reg = AUDIO_CORE_LPAIF_CODEC_SPKR_EBIT_CBCR,
	.has_sibling = 0,
	.base = &virt_bases[LPASS_BASE],
	.c = {
		.dbg_name = "audio_core_lpaif_codec_spkr_ebit_clk",
		.ops = &clk_ops_branch,
		CLK_INIT(audio_core_lpaif_codec_spkr_ebit_clk.c),
	},
};

static struct branch_clk audio_core_lpaif_codec_spkr_ibit_clk = {
	.cbcr_reg = AUDIO_CORE_LPAIF_CODEC_SPKR_IBIT_CBCR,
	.has_sibling = 1,
	.max_div = 15,
	.base = &virt_bases[LPASS_BASE],
	.c = {
		.dbg_name = "audio_core_lpaif_codec_spkr_ibit_clk",
		.parent = &lpaif_spkr_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(audio_core_lpaif_codec_spkr_ibit_clk.c),
	},
};

static struct branch_clk audio_core_lpaif_codec_spkr_osr_clk = {
	.cbcr_reg = AUDIO_CORE_LPAIF_CODEC_SPKR_OSR_CBCR,
	.has_sibling = 1,
	.base = &virt_bases[LPASS_BASE],
	.c = {
		.dbg_name = "audio_core_lpaif_codec_spkr_osr_clk",
		.parent = &lpaif_spkr_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(audio_core_lpaif_codec_spkr_osr_clk.c),
	},
};

static struct branch_clk audio_core_lpaif_ter_ebit_clk = {
	.cbcr_reg = AUDIO_CORE_LPAIF_TER_EBIT_CBCR,
	.has_sibling = 0,
	.base = &virt_bases[LPASS_BASE],
	.c = {
		.dbg_name = "audio_core_lpaif_ter_ebit_clk",
		.ops = &clk_ops_branch,
		CLK_INIT(audio_core_lpaif_ter_ebit_clk.c),
	},
};

static struct branch_clk audio_core_lpaif_ter_ibit_clk = {
	.cbcr_reg = AUDIO_CORE_LPAIF_TER_IBIT_CBCR,
	.has_sibling = 1,
	.max_div = 15,
	.base = &virt_bases[LPASS_BASE],
	.c = {
		.dbg_name = "audio_core_lpaif_ter_ibit_clk",
		.parent = &lpaif_ter_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(audio_core_lpaif_ter_ibit_clk.c),
	},
};

static struct branch_clk audio_core_lpaif_ter_osr_clk = {
	.cbcr_reg = AUDIO_CORE_LPAIF_TER_OSR_CBCR,
	.has_sibling = 1,
	.base = &virt_bases[LPASS_BASE],
	.c = {
		.dbg_name = "audio_core_lpaif_ter_osr_clk",
		.parent = &lpaif_ter_clk_src.c,
		.ops = &clk_ops_branch,
		CLK_INIT(audio_core_lpaif_ter_osr_clk.c),
	},
};

static struct measure_mux_entry measure_mux_LPASS[] = {
	{ &lpaif_pcmoe_clk_src.c,  LPASS_BASE, 0x000f },
	{ &slimbus_clk_src.c,  LPASS_BASE, 0x0011 },
	{ &lpaif_pcm1_clk_src.c,  LPASS_BASE, 0x0012 },
	{ &lpaif_pcm0_clk_src.c,  LPASS_BASE, 0x0013 },
	{ &lpaif_quad_clk_src.c,  LPASS_BASE, 0x0014 },
	{ &lpaif_ter_clk_src.c,  LPASS_BASE, 0x0015 },
	{ &lpaif_sec_clk_src.c,  LPASS_BASE, 0x0016 },
	{ &lpaif_pri_clk_src.c,  LPASS_BASE, 0x0017 },
	{ &lpaif_spkr_clk_src.c,  LPASS_BASE, 0x0018 },
	{ &q6ss_ahbm_clk.c,  LPASS_BASE, 0x001d },
	{ &q6ss_ahb_lfabif_clk.c,  LPASS_BASE, 0x001e },
	{ &audio_wrapper_br_clk.c,  LPASS_BASE, 0x0022 },
	{ &q6ss_xo_clk.c,  LPASS_BASE, 0x002b },
	{ &audio_core_lpaif_pcmoe_clk.c,  LPASS_BASE, 0x0030 },
	{ &audio_core_slimbus_core_clk.c,  LPASS_BASE, 0x003d },
	{ &audio_core_slimbus_lfabif_clk.c,  LPASS_BASE, 0x003e },
	{ &audio_core_ixfabric_clk.c,  LPASS_BASE, 0x0059 },
	{&dummy_clk, N_BASES, 0x0000},
};


static DEFINE_CLK_MEASURE(apc0_m_clk);
static DEFINE_CLK_MEASURE(apc1_m_clk);
static DEFINE_CLK_MEASURE(apc2_m_clk);
static DEFINE_CLK_MEASURE(apc3_m_clk);
static DEFINE_CLK_MEASURE(l2_m_clk);

static struct  measure_mux_entry measure_mux_APSS[] = {
	{&apc0_m_clk,                    APCS_BASE, 0x00010},
	{&apc1_m_clk,                    APCS_BASE, 0x00114},
	{&apc2_m_clk,                    APCS_BASE, 0x00220},
	{&apc3_m_clk,                    APCS_BASE, 0x00324},
	{&l2_m_clk,                      APCS_BASE, 0x01000},
	{&dummy_clk, N_BASES, 0x0000}
};

#define APCS_SH_PLL_MODE        (0x000)
#define APCS_SH_PLL_L_VAL       (0x004)
#define APCS_SH_PLL_M_VAL       (0x008)
#define APCS_SH_PLL_N_VAL       (0x00C)
#define APCS_SH_PLL_USER_CTL    (0x010)
#define APCS_SH_PLL_CONFIG_CTL  (0x014)
#define APCS_SH_PLL_STATUS      (0x01C)

enum vdd_sr2_pll_levels {
	VDD_SR2_PLL_OFF,
	VDD_SR2_PLL_ON,
	VDD_SR2_PLL_NUM
};

static struct regulator *vdd_sr2_reg;
static int set_vdd_sr2_pll(struct clk_vdd_class *vdd_class, int level)
{
	if (level == VDD_SR2_PLL_ON) {
		return regulator_set_voltage(vdd_sr2_reg, 1800000,
		1800000);
	} else {
		return regulator_set_voltage(vdd_sr2_reg, 0, 1800000);
	}
}

static DEFINE_VDD_CLASS(vdd_sr2_pll, set_vdd_sr2_pll,
			VDD_SR2_PLL_NUM);

static struct pll_freq_tbl apcs_pll_freq[] = {
	F_APCS_PLL( 384000000, 20, 0x0, 0x1, 0x0, 0x0, 0x0),
	F_APCS_PLL( 787200000, 41, 0x0, 0x1, 0x0, 0x0, 0x0),
	F_APCS_PLL( 998400000, 52, 0x0, 0x1, 0x0, 0x0, 0x0),
	F_APCS_PLL(1190400000, 62, 0x0, 0x1, 0x0, 0x0, 0x0),
	PLL_F_END
};

static struct pll_clk a7sspll = {
	.mode_reg = (void __iomem *)APCS_SH_PLL_MODE,
	.l_reg = (void __iomem *)APCS_SH_PLL_L_VAL,
	.m_reg = (void __iomem *)APCS_SH_PLL_M_VAL,
	.n_reg = (void __iomem *)APCS_SH_PLL_N_VAL,
	.config_reg = (void __iomem *)APCS_SH_PLL_USER_CTL,
	.status_reg = (void __iomem *)APCS_SH_PLL_STATUS,
	.freq_tbl = apcs_pll_freq,
	.masks = {
		.vco_mask = BM(29, 28),
		.pre_div_mask = BIT(12),
		.post_div_mask = BM(9, 8),
		.mn_en_mask = BIT(24),
		.main_output_mask = BIT(0),
	},
	.base = &virt_bases[APCS_PLL_BASE],
	.c = {
		.dbg_name = "a7sspll",
		.ops = &clk_ops_sr2_pll,
		.vdd_class = &vdd_sr2_pll,
		.fmax = (unsigned long [VDD_SR2_PLL_NUM]) {
			[VDD_SR2_PLL_ON] = ULONG_MAX,
		},
		.num_fmax = VDD_SR2_PLL_NUM,
		CLK_INIT(a7sspll.c),
		/*
		 * Need to skip handoff of the acpu pll to avoid
		 * turning off the pll when the cpu is using it
		 */
		.flags = CLKFLAG_SKIP_HANDOFF,
	},
};

static DEFINE_CLK_VOTER(pnoc_msmbus_clk, &pnoc_clk.c, LONG_MAX);
static DEFINE_CLK_VOTER(snoc_msmbus_clk, &snoc_clk.c, LONG_MAX);
static DEFINE_CLK_VOTER(cnoc_msmbus_clk, &cnoc_clk.c, LONG_MAX);
static DEFINE_CLK_VOTER(pnoc_msmbus_a_clk, &pnoc_a_clk.c, LONG_MAX);
static DEFINE_CLK_VOTER(snoc_msmbus_a_clk, &snoc_a_clk.c, LONG_MAX);
static DEFINE_CLK_VOTER(cnoc_msmbus_a_clk, &cnoc_a_clk.c, LONG_MAX);

static DEFINE_CLK_VOTER(bimc_msmbus_clk, &bimc_clk.c, LONG_MAX);
static DEFINE_CLK_VOTER(bimc_msmbus_a_clk, &bimc_a_clk.c, LONG_MAX);
static DEFINE_CLK_VOTER(bimc_acpu_a_clk, &bimc_a_clk.c, LONG_MAX);
static DEFINE_CLK_VOTER(oxili_gfx3d_clk_src, &gfx3d_clk_src.c, LONG_MAX);
static DEFINE_CLK_VOTER(ocmemgx_msmbus_clk, &ocmemgx_clk.c, LONG_MAX);
static DEFINE_CLK_VOTER(ocmemgx_msmbus_a_clk, &ocmemgx_a_clk.c, LONG_MAX);
static DEFINE_CLK_VOTER(ocmemgx_core_clk, &ocmemgx_clk.c, LONG_MAX);

static DEFINE_CLK_VOTER(pnoc_sps_clk, &pnoc_clk.c, LONG_MAX);

#ifdef CONFIG_DEBUG_FS
static int measure_clk_set_parent(struct clk *c, struct clk *parent)
{
	struct measure_clk *clk = to_measure_clk(c);
	unsigned long flags;
	u32 regval, clk_sel;
	int i;
	struct measure_mux_entry *array[] = {
		measure_mux_GCC,
		measure_mux_MMSS,
		measure_mux_LPASS,
		measure_mux_APSS,
		NULL
	};
	struct measure_mux_entry *mux = array[0];

	if (!parent)
		return -EINVAL;

	for (i = 0; array[i]; i++) {
		for (mux = array[i]; mux->c != &dummy_clk; mux++)
			if (mux->c == parent)
				break;
	}

	if (mux->c == &dummy_clk)
		return -EINVAL;

	spin_lock_irqsave(&local_clock_reg_lock, flags);
	/*
	 * Program the test vector, measurement period (sample_ticks)
	 * and scaling multiplier.
	 */
	clk->sample_ticks = 0x10000;
	clk->multiplier = 1;

	switch (mux->base) {

	case GCC_BASE:
		writel_relaxed(0, GCC_REG_BASE(GCC_DEBUG_CLK_CTL));
		clk_sel = mux->debug_mux;
		break;

	case MMSS_BASE:
		writel_relaxed(0, MMSS_REG_BASE(MMSS_DEBUG_CLK_CTL));
		clk_sel = 0x02C;
		regval = BVAL(11, 0, mux->debug_mux);
		writel_relaxed(regval, MMSS_REG_BASE(MMSS_DEBUG_CLK_CTL));

		/* Activate debug clock output */
		regval |= BIT(16);
		writel_relaxed(regval, MMSS_REG_BASE(MMSS_DEBUG_CLK_CTL));
		break;

	case LPASS_BASE:
		writel_relaxed(0, LPASS_REG_BASE(LPASS_DEBUG_CLK_CTL));
		clk_sel = 0x161;
		regval = BVAL(11, 0, mux->debug_mux);
		writel_relaxed(regval, LPASS_REG_BASE(LPASS_DEBUG_CLK_CTL));

		/* Activate debug clock output */
		regval |= BIT(20);
		writel_relaxed(regval, LPASS_REG_BASE(LPASS_DEBUG_CLK_CTL));
		break;

	case APCS_BASE:
		clk->multiplier = 4;
		clk_sel = 362;
		regval = readl_relaxed(APCS_REG_BASE(GLB_CLK_DIAG));
		regval &= ~0xC0037335;
		/* configure a divider of 4 */
		regval = BVAL(31, 30, 0x3) | mux->debug_mux;
		writel_relaxed(regval, APCS_REG_BASE(GLB_CLK_DIAG));
		break;

	default:
		return -EINVAL;
	}

	/* Set debug mux clock index */
	regval = BVAL(8, 0, clk_sel);
	writel_relaxed(regval, GCC_REG_BASE(GCC_DEBUG_CLK_CTL));

	/* Activate debug clock output */
	regval |= BIT(16);
	writel_relaxed(regval, GCC_REG_BASE(GCC_DEBUG_CLK_CTL));

	/* Make sure test vector is set before starting measurements. */
	mb();
	spin_unlock_irqrestore(&local_clock_reg_lock, flags);

	return 0;
}

/* Sample clock for 'ticks' reference clock ticks. */
static u32 run_measurement(unsigned ticks)
{
	/* Stop counters and set the XO4 counter start value. */
	writel_relaxed(ticks, GCC_REG_BASE(CLOCK_FRQ_MEASURE_CTL));

	/* Wait for timer to become ready. */
	while ((readl_relaxed(GCC_REG_BASE(CLOCK_FRQ_MEASURE_STATUS)) &
			BIT(25)) != 0)
		cpu_relax();

	/* Run measurement and wait for completion. */
	writel_relaxed(BIT(20)|ticks, GCC_REG_BASE(CLOCK_FRQ_MEASURE_CTL));
	while ((readl_relaxed(GCC_REG_BASE(CLOCK_FRQ_MEASURE_STATUS)) &
			BIT(25)) == 0)
		cpu_relax();

	/* Return measured ticks. */
	return readl_relaxed(GCC_REG_BASE(CLOCK_FRQ_MEASURE_STATUS)) &
				BM(24, 0);
}

/*
 * Perform a hardware rate measurement for a given clock.
 * FOR DEBUG USE ONLY: Measurements take ~15 ms!
 */
static unsigned long measure_clk_get_rate(struct clk *c)
{
	unsigned long flags;
	u32 gcc_xo4_reg_backup;
	u64 raw_count_short, raw_count_full;
	struct measure_clk *clk = to_measure_clk(c);
	unsigned ret;

	ret = clk_prepare_enable(&xo.c);
	if (ret) {
		pr_warn("CXO clock failed to enable. Can't measure\n");
		return 0;
	}

	spin_lock_irqsave(&local_clock_reg_lock, flags);

	/* Enable CXO/4 and RINGOSC branch. */
	gcc_xo4_reg_backup = readl_relaxed(GCC_REG_BASE(GCC_XO_DIV4_CBCR));
	writel_relaxed(0x1, GCC_REG_BASE(GCC_XO_DIV4_CBCR));

	/*
	 * The ring oscillator counter will not reset if the measured clock
	 * is not running.  To detect this, run a short measurement before
	 * the full measurement.  If the raw results of the two are the same
	 * then the clock must be off.
	 */

	/* Run a short measurement. (~1 ms) */
	raw_count_short = run_measurement(0x1000);
	/* Run a full measurement. (~14 ms) */
	raw_count_full = run_measurement(clk->sample_ticks);

	writel_relaxed(gcc_xo4_reg_backup, GCC_REG_BASE(GCC_XO_DIV4_CBCR));

	/* Return 0 if the clock is off. */
	if (raw_count_full == raw_count_short) {
		ret = 0;
	} else {
		/* Compute rate in Hz. */
		raw_count_full = ((raw_count_full * 10) + 15) * 4800000;
		do_div(raw_count_full, ((clk->sample_ticks * 10) + 35));
		ret = (raw_count_full * clk->multiplier);
	}

	writel_relaxed(0x51A00, GCC_REG_BASE(PLLTEST_PAD_CFG));
	spin_unlock_irqrestore(&local_clock_reg_lock, flags);

	clk_disable_unprepare(&xo.c);

	return ret;
}

#else /* !CONFIG_DEBUG_FS */
static int measure_clk_set_parent(struct clk *clk, struct clk *parent)
{
	return -EINVAL;
}

static unsigned long measure_clk_get_rate(struct clk *clk)
{
	return 0;
}
#endif /* CONFIG_DEBUG_FS */

static struct clk_ops clk_ops_measure = {
	.set_parent = measure_clk_set_parent,
	.get_rate = measure_clk_get_rate,
};

static struct measure_clk measure_clk = {
	.c = {
		.dbg_name = "measure_clk",
		.ops = &clk_ops_measure,
		CLK_INIT(measure_clk.c),
	},
	.multiplier = 1,
};

static struct clk_lookup msm_clocks_8226[] = {
	/* Debug Clocks */
	CLK_LOOKUP("measure", measure_clk.c, "debug"),
	CLK_LOOKUP("apc0_m_clk", apc0_m_clk, ""),
	CLK_LOOKUP("apc1_m_clk", apc1_m_clk, ""),
	CLK_LOOKUP("apc2_m_clk", apc2_m_clk, ""),
	CLK_LOOKUP("apc3_m_clk", apc3_m_clk, ""),
	CLK_LOOKUP("l2_m_clk", l2_m_clk, ""),

	/* PIL-LPASS */
	CLK_LOOKUP("xo",                         xo.c, "fe200000.qcom,lpass"),
	CLK_LOOKUP("core_clk",          q6ss_xo_clk.c, "fe200000.qcom,lpass"),
	CLK_LOOKUP("bus_clk",  gcc_lpass_q6_axi_clk.c, "fe200000.qcom,lpass"),
	CLK_LOOKUP("iface_clk", q6ss_ahb_lfabif_clk.c, "fe200000.qcom,lpass"),
	CLK_LOOKUP("reg_clk",         q6ss_ahbm_clk.c, "fe200000.qcom,lpass"),

	/* PIL-MODEM */
	CLK_LOOKUP("xo",                           xo.c, "fc880000.qcom,mss"),
	CLK_LOOKUP("bus_clk", gcc_mss_q6_bimc_axi_clk.c, "fc880000.qcom,mss"),
	CLK_LOOKUP("iface_clk",   gcc_mss_cfg_ahb_clk.c, "fc880000.qcom,mss"),
	CLK_LOOKUP("mem_clk",    gcc_boot_rom_ahb_clk.c, "fc880000.qcom,mss"),

	/* PIL-PRONTO */
	CLK_LOOKUP("xo", xo.c, "fb21b000.qcom,pronto"),

	/* PIL-VENUS */
	CLK_LOOKUP("src_clk",     vcodec0_clk_src.c, "fdce0000.qcom,venus"),
	CLK_LOOKUP("core_clk", venus0_vcodec0_clk.c, "fdce0000.qcom,venus"),
	CLK_LOOKUP("iface_clk",    venus0_ahb_clk.c, "fdce0000.qcom,venus"),
	CLK_LOOKUP("bus_clk",      venus0_axi_clk.c, "fdce0000.qcom,venus"),
	CLK_LOOKUP("mem_clk",      venus0_ahb_clk.c, "fdce0000.qcom,venus"),

	/* ACPUCLOCK */
	CLK_LOOKUP("xo",     xo_a_clk.c, "f9011050.qcom,acpuclk"),
	CLK_LOOKUP("gpll0",  gpll0_ao.c, "f9011050.qcom,acpuclk"),
	CLK_LOOKUP("a7sspll", a7sspll.c, "f9011050.qcom,acpuclk"),

	/* WCNSS CLOCKS */
	CLK_LOOKUP("xo", xo.c, "fb000000.qcom,wcnss-wlan"),

	/* BUS DRIVER */
	CLK_LOOKUP("bus_clk", cnoc_msmbus_clk.c, "msm_config_noc"),
	CLK_LOOKUP("bus_a_clk", cnoc_msmbus_a_clk.c, "msm_config_noc"),
	CLK_LOOKUP("bus_clk", snoc_msmbus_clk.c, "msm_sys_noc"),
	CLK_LOOKUP("bus_a_clk", snoc_msmbus_a_clk.c, "msm_sys_noc"),
	CLK_LOOKUP("bus_clk", pnoc_msmbus_clk.c, "msm_periph_noc"),
	CLK_LOOKUP("bus_a_clk", pnoc_msmbus_a_clk.c, "msm_periph_noc"),
	CLK_LOOKUP("mem_clk", bimc_msmbus_clk.c, "msm_bimc"),
	CLK_LOOKUP("mem_a_clk", bimc_msmbus_a_clk.c, "msm_bimc"),
	CLK_LOOKUP("mem_clk", bimc_acpu_a_clk.c, ""),
	CLK_LOOKUP("ocmem_clk",	ocmemgx_msmbus_clk.c,	  "msm_bus"),
	CLK_LOOKUP("ocmem_a_clk", ocmemgx_msmbus_a_clk.c, "msm_bus"),
	CLK_LOOKUP("bus_clk",	mmss_s0_axi_clk.c,	"msm_mmss_noc"),
	CLK_LOOKUP("bus_a_clk",	mmss_s0_axi_clk.c,	"msm_mmss_noc"),
	CLK_LOOKUP("iface_clk", gcc_mmss_noc_cfg_ahb_clk.c, ""),

	CLK_LOOKUP("core_clk", qdss_clk.c, "coresight-tmc-etr"),
	CLK_LOOKUP("core_clk", qdss_clk.c, "coresight-tpiu"),
	CLK_LOOKUP("core_clk", qdss_clk.c, "coresight-replicator"),
	CLK_LOOKUP("core_clk", qdss_clk.c, "coresight-tmc-etf"),
	CLK_LOOKUP("core_clk", qdss_clk.c, "coresight-funnel-merg"),
	CLK_LOOKUP("core_clk", qdss_clk.c, "coresight-funnel-in0"),
	CLK_LOOKUP("core_clk", qdss_clk.c, "coresight-funnel-in1"),
	CLK_LOOKUP("core_clk", qdss_clk.c, "coresight-funnel-kpss"),
	CLK_LOOKUP("core_clk", qdss_clk.c, "coresight-funnel-mmss"),
	CLK_LOOKUP("core_clk", qdss_clk.c, "coresight-stm"),
	CLK_LOOKUP("core_clk", qdss_clk.c, "coresight-etm0"),
	CLK_LOOKUP("core_clk", qdss_clk.c, "coresight-etm1"),
	CLK_LOOKUP("core_clk", qdss_clk.c, "coresight-etm2"),
	CLK_LOOKUP("core_clk", qdss_clk.c, "coresight-etm3"),

	CLK_LOOKUP("core_a_clk", qdss_a_clk.c, "coresight-tmc-etr"),
	CLK_LOOKUP("core_a_clk", qdss_a_clk.c, "coresight-tpiu"),
	CLK_LOOKUP("core_a_clk", qdss_a_clk.c, "coresight-replicator"),
	CLK_LOOKUP("core_a_clk", qdss_a_clk.c, "coresight-tmc-etf"),
	CLK_LOOKUP("core_a_clk", qdss_a_clk.c, "coresight-funnel-merg"),
	CLK_LOOKUP("core_a_clk", qdss_a_clk.c, "coresight-funnel-in0"),
	CLK_LOOKUP("core_a_clk", qdss_a_clk.c, "coresight-funnel-in1"),
	CLK_LOOKUP("core_a_clk", qdss_a_clk.c, "coresight-funnel-kpss"),
	CLK_LOOKUP("core_a_clk", qdss_a_clk.c, "coresight-funnel-mmss"),
	CLK_LOOKUP("core_a_clk", qdss_a_clk.c, "coresight-stm"),
	CLK_LOOKUP("core_a_clk", qdss_a_clk.c, "coresight-etm0"),
	CLK_LOOKUP("core_a_clk", qdss_a_clk.c, "coresight-etm1"),
	CLK_LOOKUP("core_a_clk", qdss_a_clk.c, "coresight-etm2"),
	CLK_LOOKUP("core_a_clk", qdss_a_clk.c, "coresight-etm3"),

	/* HSUSB-OTG Clocks */
	CLK_LOOKUP("xo",                          xo.c, "f9a55000.usb"),
	CLK_LOOKUP("iface_clk",   gcc_usb_hs_ahb_clk.c, "f9a55000.usb"),
	CLK_LOOKUP("core_clk", gcc_usb_hs_system_clk.c, "f9a55000.usb"),

	/* SPS CLOCKS */
	CLK_LOOKUP("dfab_clk",            pnoc_sps_clk.c, "f9984000.qcom,sps"),
	CLK_LOOKUP("dma_bam_pclk", gcc_bam_dma_ahb_clk.c, "f9884000.qcom,sps"),
	CLK_LOOKUP("dfab_clk",            pnoc_sps_clk.c, "msm_sps"),
	CLK_LOOKUP("dma_bam_pclk", gcc_bam_dma_ahb_clk.c, "msm_sps"),

	/* I2C Clocks */
	CLK_LOOKUP("iface_clk",          gcc_blsp1_ahb_clk.c, "f9926000.i2c"),
	CLK_LOOKUP("core_clk", gcc_blsp1_qup4_i2c_apps_clk.c, "f9926000.i2c"),

	/* lsuart-v14 Clocks */
	CLK_LOOKUP("iface_clk",       gcc_blsp1_ahb_clk.c, "f991f000.serial"),
	CLK_LOOKUP("core_clk", gcc_blsp1_uart3_apps_clk.c, "f991f000.serial"),

	CLK_LOOKUP("iface_clk",       gcc_blsp1_ahb_clk.c, "f995e000.serial"),
	CLK_LOOKUP("core_clk", gcc_blsp1_uart2_apps_clk.c, "f995e000.serial"),

	CLK_LOOKUP("iface_clk",          gcc_blsp1_ahb_clk.c, "f9928000.spi"),
	CLK_LOOKUP("core_clk", gcc_blsp1_qup1_spi_apps_clk.c, "f9928000.spi"),

	CLK_LOOKUP("core_clk",     gcc_ce1_clk.c,         "qseecom"),
	CLK_LOOKUP("iface_clk",    gcc_ce1_ahb_clk.c,     "qseecom"),
	CLK_LOOKUP("bus_clk",      gcc_ce1_axi_clk.c,     "qseecom"),
	CLK_LOOKUP("core_clk_src", ce1_clk_src.c,         "qseecom"),

	/* SDCC */
	CLK_LOOKUP("iface_clk", gcc_sdcc1_ahb_clk.c, "f9824000.qcom,sdcc"),
	CLK_LOOKUP("core_clk", gcc_sdcc1_apps_clk.c, "f9824000.qcom,sdcc"),
	CLK_LOOKUP("iface_clk", gcc_sdcc1_ahb_clk.c, "msm_sdcc.1"),
	CLK_LOOKUP("core_clk", gcc_sdcc1_apps_clk.c, "msm_sdcc.1"),

	CLK_LOOKUP("iface_clk", gcc_sdcc2_ahb_clk.c, "f98a4000.qcom,sdcc"),
	CLK_LOOKUP("core_clk", gcc_sdcc2_apps_clk.c, "f98a4000.qcom,sdcc"),
	CLK_LOOKUP("iface_clk", gcc_sdcc2_ahb_clk.c, "msm_sdcc.2"),
	CLK_LOOKUP("core_clk", gcc_sdcc2_apps_clk.c, "msm_sdcc.2"),

	CLK_LOOKUP("iface_clk", gcc_sdcc3_ahb_clk.c, "msm_sdcc.3"),
	CLK_LOOKUP("core_clk", gcc_sdcc3_apps_clk.c, "msm_sdcc.3"),

	CLK_LOOKUP("sleep_a_clk", gcc_usb2a_phy_sleep_clk.c, "msm_dwc3"),
	CLK_LOOKUP("ref_clk", diff_clk.c, "msm_dwc3"),


	CLK_LOOKUP("bus_clk", pnoc_clk.c, ""),
	CLK_LOOKUP("bus_clk", pnoc_a_clk.c, ""),
	CLK_LOOKUP("bus_clk", snoc_clk.c, ""),
	CLK_LOOKUP("bus_clk", snoc_a_clk.c, ""),
	CLK_LOOKUP("bus_clk", cnoc_clk.c, ""),
	CLK_LOOKUP("bus_clk", cnoc_a_clk.c, ""),
	CLK_LOOKUP("bus_clk", mmssnoc_ahb_clk.c, ""),
	CLK_LOOKUP("bus_clk", mmssnoc_ahb_a_clk.c, ""),
	CLK_LOOKUP("bus_clk", bimc_clk.c, ""),
	CLK_LOOKUP("bus_clk", bimc_a_clk.c, ""),
	CLK_LOOKUP("bus_clk_src", axi_clk_src.c, ""),

	CLK_LOOKUP("gpll0", gpll0.c, ""),
	CLK_LOOKUP("gpll1", gpll1.c, ""),
	CLK_LOOKUP("mmpll0", mmpll0_pll.c, ""),
	CLK_LOOKUP("mmpll1", mmpll1_pll.c, ""),
	CLK_LOOKUP("mmpll2", mmpll2_pll.c, ""),
	CLK_LOOKUP("lpaaudio_pll", lpaaudio_pll.c, ""),

	CLK_LOOKUP("core_clk", gcc_blsp1_qup1_i2c_apps_clk.c, ""),
	CLK_LOOKUP("core_clk", gcc_blsp1_qup2_i2c_apps_clk.c, ""),
	CLK_LOOKUP("core_clk", gcc_blsp1_qup2_spi_apps_clk.c, ""),
	CLK_LOOKUP("core_clk", gcc_blsp1_qup3_i2c_apps_clk.c, ""),
	CLK_LOOKUP("core_clk", gcc_blsp1_qup3_spi_apps_clk.c, ""),
	CLK_LOOKUP("core_clk", gcc_blsp1_qup4_i2c_apps_clk.c, ""),
	CLK_LOOKUP("core_clk", gcc_blsp1_qup4_spi_apps_clk.c, ""),
	CLK_LOOKUP("core_clk", gcc_blsp1_qup5_i2c_apps_clk.c, ""),
	CLK_LOOKUP("core_clk", gcc_blsp1_qup5_spi_apps_clk.c, ""),
	CLK_LOOKUP("core_clk", gcc_blsp1_qup6_i2c_apps_clk.c, ""),
	CLK_LOOKUP("core_clk", gcc_blsp1_qup6_spi_apps_clk.c, ""),
	CLK_LOOKUP("core_clk", gcc_blsp1_uart1_apps_clk.c, ""),
	CLK_LOOKUP("core_clk", gcc_blsp1_uart4_apps_clk.c, ""),
	CLK_LOOKUP("core_clk", gcc_blsp1_uart5_apps_clk.c, ""),
	CLK_LOOKUP("core_clk", gcc_blsp1_uart6_apps_clk.c, ""),
	CLK_LOOKUP("core_clk", gcc_pdm2_clk.c, ""),
	CLK_LOOKUP("iface_clk", gcc_pdm_ahb_clk.c, ""),
	CLK_LOOKUP("iface_clk", gcc_prng_ahb_clk.c, ""),
	CLK_LOOKUP("core_clk", gcc_gp1_clk.c, ""),
	CLK_LOOKUP("core_clk", gcc_gp2_clk.c, ""),
	CLK_LOOKUP("core_clk", gcc_gp3_clk.c, ""),

	CLK_LOOKUP("iface_clk", gcc_usb_hsic_ahb_clk.c,	  "msm_hsic_host"),
	CLK_LOOKUP("phy_clk", gcc_usb_hsic_clk.c,	  "msm_hsic_host"),
	CLK_LOOKUP("cal_clk", gcc_usb_hsic_io_cal_clk.c,  "msm_hsic_host"),
	CLK_LOOKUP("core_clk", gcc_usb_hsic_system_clk.c, "msm_hsic_host"),
	CLK_LOOKUP("ref_clk", div_clk2.c, "msm_smsc_hub"),
	CLK_LOOKUP("iface_clk", gcc_usb_hs_ahb_clk.c,     "msm_ehci_host"),
	CLK_LOOKUP("core_clk", gcc_usb_hs_system_clk.c,   "msm_ehci_host"),
	CLK_LOOKUP("pwm_clk", div_clk2.c, "0-0048"),

	/* Multimedia clocks */
	CLK_LOOKUP("byte_clk", mdss_byte0_clk.c, "fd922800.qcom,mdss_dsi"),
	CLK_LOOKUP("core_clk", mdss_esc0_clk.c, "fd922800.qcom,mdss_dsi"),
	CLK_LOOKUP("pixel_clk", mdss_pclk0_clk.c, "fd922800.qcom,mdss_dsi"),
	CLK_LOOKUP("iface_clk", mdss_ahb_clk.c, "mdss_dsi_clk_ctrl"),

	CLK_LOOKUP("core_clk", mdss_mdp_clk.c, "mdp.0"),
	CLK_LOOKUP("lut_clk", mdss_mdp_lut_clk.c, "mdp.0"),
	CLK_LOOKUP("core_clk_src", mdp_clk_src.c, "mdp.0"),
	CLK_LOOKUP("vsync_clk", mdss_vsync_clk.c, "mdp.0"),
	CLK_LOOKUP("iface_clk", mdss_ahb_clk.c, "mdp.0"),
	CLK_LOOKUP("bus_clk", mdss_axi_clk.c, "mdp.0"),

	CLK_LOOKUP("iface_clk", mdss_ahb_clk.c, "fd928000.qcom,iommu"),
	CLK_LOOKUP("core_clk", mdss_axi_clk.c, "fd928000.qcom,iommu"),

	/* MM sensor clocks */
	CLK_LOOKUP("cam_src_clk", mclk0_clk_src.c, "6e.qcom,camera"),
	CLK_LOOKUP("cam_src_clk", mclk1_clk_src.c, "90.qcom,camera"),
	CLK_LOOKUP("cam_clk", camss_mclk0_clk.c, "6e.qcom,camera"),
	CLK_LOOKUP("cam_clk", camss_mclk1_clk.c, "90.qcom,camera"),

	/* CCI clocks */
	CLK_LOOKUP("camss_top_ahb_clk", camss_top_ahb_clk.c,
		"fda0c000.qcom,cci"),
	CLK_LOOKUP("cci_ahb_clk", camss_cci_cci_ahb_clk.c,
		"fda0c000.qcom,cci"),
	CLK_LOOKUP("cci_src_clk", cci_clk_src.c, "fda0c000.qcom,cci"),
	CLK_LOOKUP("cci_clk", camss_cci_cci_clk.c, "fda0c000.qcom,cci"),

	/* CSIPHY clocks */
	CLK_LOOKUP("camss_top_ahb_clk", camss_top_ahb_clk.c,
		"fda0ac00.qcom,csiphy"),
	CLK_LOOKUP("ispif_ahb_clk", camss_ispif_ahb_clk.c,
		"fda0ac00.qcom,csiphy"),
	CLK_LOOKUP("csiphy_timer_src_clk", csi0phytimer_clk_src.c,
		"fda0ac00.qcom,csiphy"),
	CLK_LOOKUP("csiphy_timer_clk", camss_phy0_csi0phytimer_clk.c,
		"fda0ac00.qcom,csiphy"),
	CLK_LOOKUP("camss_top_ahb_clk", camss_top_ahb_clk.c,
		"fda0b000.qcom,csiphy"),
	CLK_LOOKUP("ispif_ahb_clk", camss_ispif_ahb_clk.c,
		"fda0b000.qcom,csiphy"),
	CLK_LOOKUP("csiphy_timer_src_clk", csi1phytimer_clk_src.c,
		"fda0b000.qcom,csiphy"),
	CLK_LOOKUP("csiphy_timer_clk", camss_phy1_csi1phytimer_clk.c,
		"fda0b000.qcom,csiphy"),

	/* CSID clocks */
	CLK_LOOKUP("camss_top_ahb_clk", camss_top_ahb_clk.c,
		"fda08000.qcom,csid"),
	CLK_LOOKUP("ispif_ahb_clk", camss_ispif_ahb_clk.c,
		"fda08000.qcom,csid"),
	CLK_LOOKUP("csi0_ahb_clk", camss_csi0_ahb_clk.c, "fda08000.qcom,csid"),
	CLK_LOOKUP("csi0_src_clk", csi0_clk_src.c,       "fda08000.qcom,csid"),
	CLK_LOOKUP("csi0_phy_clk", camss_csi0phy_clk.c,  "fda08000.qcom,csid"),
	CLK_LOOKUP("csi0_clk", camss_csi0_clk.c,         "fda08000.qcom,csid"),
	CLK_LOOKUP("csi0_pix_clk", camss_csi0pix_clk.c,  "fda08000.qcom,csid"),
	CLK_LOOKUP("csi0_rdi_clk", camss_csi0rdi_clk.c,  "fda08000.qcom,csid"),

	CLK_LOOKUP("camss_top_ahb_clk", camss_top_ahb_clk.c,
		"fda08400.qcom,csid"),
	CLK_LOOKUP("ispif_ahb_clk", camss_ispif_ahb_clk.c,
		"fda08400.qcom,csid"),
	CLK_LOOKUP("csi0_ahb_clk", camss_csi0_ahb_clk.c, "fda08400.qcom,csid"),
	CLK_LOOKUP("csi1_ahb_clk", camss_csi1_ahb_clk.c, "fda08400.qcom,csid"),
	CLK_LOOKUP("csi0_src_clk", csi0_clk_src.c,       "fda08400.qcom,csid"),
	CLK_LOOKUP("csi1_src_clk", csi1_clk_src.c,       "fda08400.qcom,csid"),
	CLK_LOOKUP("csi0_phy_clk", camss_csi0phy_clk.c,  "fda08400.qcom,csid"),
	CLK_LOOKUP("csi1_phy_clk", camss_csi1phy_clk.c,  "fda08400.qcom,csid"),
	CLK_LOOKUP("csi0_pix_clk", camss_csi0pix_clk.c,  "fda08400.qcom,csid"),
	CLK_LOOKUP("csi1_pix_clk", camss_csi1pix_clk.c,  "fda08400.qcom,csid"),
	CLK_LOOKUP("csi0_rdi_clk", camss_csi0rdi_clk.c,  "fda08400.qcom,csid"),
	CLK_LOOKUP("csi1_rdi_clk", camss_csi1rdi_clk.c,  "fda08400.qcom,csid"),

	/* ISPIF clocks */
	CLK_LOOKUP("camss_vfe_vfe_clk", camss_vfe_vfe0_clk.c,
		"fda0a000.qcom,ispif"),
	CLK_LOOKUP("camss_csi_vfe_clk", camss_csi_vfe0_clk.c,
		"fda0a000.qcom,ispif"),

	/* VFE clocks */
	CLK_LOOKUP("camss_top_ahb_clk", camss_top_ahb_clk.c,
					"fda10000.qcom,vfe"),
	CLK_LOOKUP("vfe_clk_src", vfe0_clk_src.c,	 "fda10000.qcom,vfe"),
	CLK_LOOKUP("camss_vfe_vfe_clk", camss_vfe_vfe0_clk.c,
					"fda10000.qcom,vfe"),
	CLK_LOOKUP("camss_csi_vfe_clk", camss_csi_vfe0_clk.c,
					"fda10000.qcom,vfe"),
	CLK_LOOKUP("iface_clk", camss_vfe_vfe_ahb_clk.c, "fda10000.qcom,vfe"),
	CLK_LOOKUP("bus_clk", camss_vfe_vfe_axi_clk.c,	 "fda10000.qcom,vfe"),

	CLK_LOOKUP("iface_clk", camss_vfe_vfe_ahb_clk.c,
	"fda44000.qcom,iommu"),
	CLK_LOOKUP("core_clk", camss_vfe_vfe_axi_clk.c, "fda44000.qcom,iommu"),
	CLK_LOOKUP("alt_core_clk", camss_top_ahb_clk.c, "fda44000.qcom,iommu"),

	/* Jpeg Clocks */
	CLK_LOOKUP("core_clk", camss_jpeg_jpeg0_clk.c, "fda1c000.qcom,jpeg"),
	CLK_LOOKUP("iface_clk", camss_jpeg_jpeg_ahb_clk.c,
						"fda1c000.qcom,jpeg"),
	CLK_LOOKUP("bus_clk0", camss_jpeg_jpeg_axi_clk.c,
	"fda1c000.qcom,jpeg"),
	CLK_LOOKUP("camss_top_ahb_clk", camss_top_ahb_clk.c,
						"fda1c000.qcom,jpeg"),

	CLK_LOOKUP("alt_core_clk", camss_top_ahb_clk.c, "fda64000.qcom,iommu"),
	CLK_LOOKUP("iface_clk", camss_jpeg_jpeg_ahb_clk.c,
						"fda64000.qcom,iommu"),
	CLK_LOOKUP("core_clk", camss_jpeg_jpeg_axi_clk.c,
						"fda64000.qcom,iommu"),

	/* KGSL Clocks */
	CLK_LOOKUP("core_clk", oxili_gfx3d_clk.c, "fdb00000.qcom,kgsl-3d0"),
	CLK_LOOKUP("iface_clk", oxilicx_ahb_clk.c, "fdb00000.qcom,kgsl-3d0"),

	CLK_LOOKUP("alt_core_clk", oxili_gfx3d_clk.c, "fdb10000.qcom,iommu"),
	CLK_LOOKUP("iface_clk", oxilicx_ahb_clk.c, "fdb10000.qcom,iommu"),
	CLK_LOOKUP("core_clk", oxilicx_axi_clk.c, "fdb10000.qcom,iommu"),

	CLK_LOOKUP("core_clk", ocmemgx_core_clk.c, "fdd00000.qcom,ocmem"),
	CLK_LOOKUP("br_clk", audio_wrapper_br_clk.c, "fdd00000.qcom,ocmem"),

	/* Venus Clocks */
	CLK_LOOKUP("core_clk", venus0_vcodec0_clk.c, "fdc00000.qcom,vidc"),
	CLK_LOOKUP("iface_clk",  venus0_ahb_clk.c, "fdc00000.qcom,vidc"),
	CLK_LOOKUP("bus_clk",  venus0_axi_clk.c, "fdc00000.qcom,vidc"),

	CLK_LOOKUP("alt_core_clk", venus0_vcodec0_clk.c,
	"fdc84000.qcom,iommu"),
	CLK_LOOKUP("iface_clk", venus0_ahb_clk.c, "fdc84000.qcom,iommu"),
	CLK_LOOKUP("core_clk", venus0_axi_clk.c, "fdc84000.qcom,iommu"),

	CLK_LOOKUP("cam_gp0_clk", camss_gp0_clk.c, ""),
	CLK_LOOKUP("cam_gp1_clk", camss_gp1_clk.c, ""),
	CLK_LOOKUP("iface_clk", camss_micro_ahb_clk.c, ""),

	CLK_LOOKUP("", mmss_mmssnoc_ahb_clk.c, ""),
	CLK_LOOKUP("", mmss_mmssnoc_bto_ahb_clk.c, ""),
	CLK_LOOKUP("", mmss_mmssnoc_axi_clk.c, ""),
	CLK_LOOKUP("", mmss_s0_axi_clk.c, ""),
	CLK_LOOKUP("", ocmemcx_ahb_clk.c, ""),

	/* LPASS CLOCKS */
	CLK_LOOKUP("core_clk", audio_core_slimbus_core_clk.c, "fe12f000.slim"),
	CLK_LOOKUP("iface_clk", audio_core_slimbus_lfabif_clk.c,
			"fe12f000.slim"),

	CLK_LOOKUP("core_clk", lpaif_quad_clk_src.c,
			"msm-dai-q6-mi2s.3"),
	CLK_LOOKUP("osr_clk", audio_core_lpaif_quad_osr_clk.c,
			"msm-dai-q6-mi2s.3"),
	CLK_LOOKUP("ebit_clk", audio_core_lpaif_quad_ebit_clk.c,
			"msm-dai-q6-mi2s.3"),
	CLK_LOOKUP("ibit_clk", audio_core_lpaif_quad_ibit_clk.c,
			"msm-dai-q6-mi2s.3"),

	CLK_LOOKUP("pcm_clk", lpaif_pcm0_clk_src.c,
						"msm-dai-q6.4106"),
	CLK_LOOKUP("ibit_clk", audio_core_lpaif_pcm0_ibit_clk.c,
						"msm-dai-q6.4106"),
	CLK_LOOKUP("core_oe_src_clk", lpaif_pcmoe_clk_src.c,
						"msm-dai-q6.4106"),
	CLK_LOOKUP("core_oe_clk", audio_core_lpaif_pcmoe_clk.c,
						"msm-dai-q6.4106"),

	CLK_LOOKUP("pcm_clk", lpaif_pcm0_clk_src.c,
						"msm-dai-q6.4107"),
	CLK_LOOKUP("ibit_clk", audio_core_lpaif_pcm0_ibit_clk.c,
						"msm-dai-q6.4107"),
	CLK_LOOKUP("core_oe_src_clk", lpaif_pcmoe_clk_src.c,
						"msm-dai-q6.4107"),
	CLK_LOOKUP("core_oe_clk", audio_core_lpaif_pcmoe_clk.c,
						"msm-dai-q6.4107"),


	CLK_LOOKUP("bus_clk", audio_core_ixfabric_clk.c, ""),
	CLK_LOOKUP("ebit_clk", audio_core_lpaif_pcm0_ebit_clk.c, ""),
	CLK_LOOKUP("core_clk", lpaif_pcm1_clk_src.c, ""),
	CLK_LOOKUP("ebit_clk", audio_core_lpaif_pcm1_ebit_clk.c, ""),
	CLK_LOOKUP("ibit_clk", audio_core_lpaif_pcm1_ibit_clk.c, ""),
	CLK_LOOKUP("core_clk", lpaif_spkr_clk_src.c, ""),
	CLK_LOOKUP("osr_clk", audio_core_lpaif_codec_spkr_osr_clk.c, ""),
	CLK_LOOKUP("ebit_clk", audio_core_lpaif_codec_spkr_ebit_clk.c, ""),
	CLK_LOOKUP("ibit_clk", audio_core_lpaif_codec_spkr_ibit_clk.c, ""),
	CLK_LOOKUP("core_clk", lpaif_pri_clk_src.c, ""),
	CLK_LOOKUP("osr_clk", audio_core_lpaif_pri_osr_clk.c, ""),

	CLK_LOOKUP("ebit_clk", audio_core_lpaif_pri_ebit_clk.c, ""),
	CLK_LOOKUP("ibit_clk", audio_core_lpaif_pri_ibit_clk.c, ""),
	CLK_LOOKUP("core_clk", lpaif_sec_clk_src.c, ""),

	CLK_LOOKUP("osr_clk", audio_core_lpaif_sec_osr_clk.c, ""),
	CLK_LOOKUP("ebit_clk", audio_core_lpaif_sec_ebit_clk.c, ""),
	CLK_LOOKUP("ibit_clk", audio_core_lpaif_sec_ibit_clk.c, ""),

	CLK_LOOKUP("core_clk", lpaif_ter_clk_src.c, ""),
	CLK_LOOKUP("osr_clk", audio_core_lpaif_ter_osr_clk.c, ""),
	CLK_LOOKUP("ebit_clk", audio_core_lpaif_ter_ebit_clk.c, ""),
	CLK_LOOKUP("ibit_clk", audio_core_lpaif_ter_ibit_clk.c, ""),
};

static struct clk_lookup msm_clocks_8226_rumi[] = {
	CLK_DUMMY("core_clk", BLSP1_UART_CLK, "f991f000.serial", OFF),
	CLK_DUMMY("iface_clk", BLSP1_UART_CLK, "f991f000.serial", OFF),
	CLK_DUMMY("iface_clk", HSUSB_IFACE_CLK, "f9a55000.usb", OFF),
	CLK_DUMMY("core_clk", HSUSB_CORE_CLK, "f9a55000.usb", OFF),
	CLK_DUMMY("iface_clk", NULL, "msm_sdcc.1", OFF),
	CLK_DUMMY("core_clk",  NULL, "msm_sdcc.1", OFF),
	CLK_DUMMY("bus_clk",   NULL, "msm_sdcc.1", OFF),
	CLK_DUMMY("iface_clk", NULL, "msm_sdcc.2", OFF),
	CLK_DUMMY("core_clk",  NULL, "msm_sdcc.2", OFF),
	CLK_DUMMY("bus_clk",   NULL, "msm_sdcc.2", OFF),
};

struct clock_init_data msm8226_rumi_clock_init_data __initdata = {
	.table = msm_clocks_8226_rumi,
	.size = ARRAY_SIZE(msm_clocks_8226_rumi),
};

static struct pll_config_regs gpll0_regs __initdata = {
	.l_reg = (void __iomem *)GPLL0_L_VAL,
	.m_reg = (void __iomem *)GPLL0_M_VAL,
	.n_reg = (void __iomem *)GPLL0_N_VAL,
	.config_reg = (void __iomem *)GPLL0_USER_CTL,
	.mode_reg = (void __iomem *)GPLL0_MODE,
	.base = &virt_bases[GCC_BASE],
};

/* GPLL0 at 600 MHz, main output enabled. */
static struct pll_config gpll0_config __initdata = {
	.l = 0x1f,
	.m = 0x1,
	.n = 0x4,
	.vco_val = 0x0,
	.vco_mask = BM(21, 20),
	.pre_div_val = 0x0,
	.pre_div_mask = BM(14, 12),
	.post_div_val = 0x0,
	.post_div_mask = BM(9, 8),
	.mn_ena_val = BIT(24),
	.mn_ena_mask = BIT(24),
	.main_output_val = BIT(0),
	.main_output_mask = BIT(0),
};

static struct pll_config_regs gpll1_regs __initdata = {
	.l_reg = (void __iomem *)GPLL1_L_VAL,
	.m_reg = (void __iomem *)GPLL1_M_VAL,
	.n_reg = (void __iomem *)GPLL1_N_VAL,
	.config_reg = (void __iomem *)GPLL1_USER_CTL,
	.mode_reg = (void __iomem *)GPLL1_MODE,
	.base = &virt_bases[GCC_BASE],
};

/* GPLL1 at 480 MHz, main output enabled. */
static struct pll_config gpll1_config __initdata = {
	.l = 0x19,
	.m = 0x0,
	.n = 0x1,
	.vco_val = 0x0,
	.vco_mask = BM(21, 20),
	.pre_div_val = 0x0,
	.pre_div_mask = BM(14, 12),
	.post_div_val = 0x0,
	.post_div_mask = BM(9, 8),
	.main_output_val = BIT(0),
	.main_output_mask = BIT(0),
};

static struct pll_config_regs mmpll0_regs __initdata = {
	.l_reg = (void __iomem *)MMPLL0_PLL_L_VAL,
	.m_reg = (void __iomem *)MMPLL0_PLL_M_VAL,
	.n_reg = (void __iomem *)MMPLL0_PLL_N_VAL,
	.config_reg = (void __iomem *)MMPLL0_PLL_USER_CTL,
	.mode_reg = (void __iomem *)MMPLL0_PLL_MODE,
	.base = &virt_bases[MMSS_BASE],
};

/* MMPLL0 at 800 MHz, main output enabled. */
static struct pll_config mmpll0_config __initdata = {
	.l = 0x29,
	.m = 0x2,
	.n = 0x3,
	.vco_val = 0x0,
	.vco_mask = BM(21, 20),
	.pre_div_val = 0x0,
	.pre_div_mask = BM(14, 12),
	.post_div_val = 0x0,
	.post_div_mask = BM(9, 8),
	.mn_ena_val = BIT(24),
	.mn_ena_mask = BIT(24),
	.main_output_val = BIT(0),
	.main_output_mask = BIT(0),
};

static struct pll_config_regs mmpll1_regs __initdata = {
	.l_reg = (void __iomem *)MMPLL1_PLL_L_VAL,
	.m_reg = (void __iomem *)MMPLL1_PLL_M_VAL,
	.n_reg = (void __iomem *)MMPLL1_PLL_N_VAL,
	.config_reg = (void __iomem *)MMPLL1_PLL_USER_CTL,
	.mode_reg = (void __iomem *)MMPLL1_PLL_MODE,
	.base = &virt_bases[MMSS_BASE],
};

/* MMPLL1 at 1000 MHz, main output enabled. */
static struct pll_config mmpll1_config __initdata = {
	.l = 0x2C,
	.m = 0x1,
	.n = 0x10,
	.vco_val = 0x0,
	.vco_mask = BM(21, 20),
	.pre_div_val = 0x0,
	.pre_div_mask = BM(14, 12),
	.post_div_val = 0x0,
	.post_div_mask = BM(9, 8),
	.mn_ena_val = BIT(24),
	.mn_ena_mask = BIT(24),
	.main_output_val = BIT(0),
	.main_output_mask = BIT(0),
};

static struct pll_config_regs lpapll0_regs __initdata = {
	.l_reg = (void __iomem *)LPAAUDIO_PLL_L,
	.m_reg = (void __iomem *)LPAAUDIO_PLL_M,
	.n_reg = (void __iomem *)LPAAUDIO_PLL_N,
	.config_reg = (void __iomem *)LPAAUDIO_PLL_USER_CTL,
	.mode_reg = (void __iomem *)LPAAUDIO_PLL_MODE,
	.base = &virt_bases[LPASS_BASE],
};

/* LPAPLL0 at 491.52 MHz, main output enabled. */
static struct pll_config lpapll0_config __initdata = {
	.l = 0x33,
	.m = 0x1,
	.n = 0x5,
	.vco_val = 0x0,
	.vco_mask = BM(21, 20),
	.pre_div_val = BVAL(14, 12, 0x1),
	.pre_div_mask = BM(14, 12),
	.post_div_val = 0x0,
	.post_div_mask = BM(9, 8),
	.mn_ena_val = BIT(24),
	.mn_ena_mask = BIT(24),
	.main_output_val = BIT(0),
	.main_output_mask = BIT(0),
};

#define PLL_AUX_OUTPUT_BIT 1
#define PLL_AUX2_OUTPUT_BIT 2

#define PWR_ON_MASK		BIT(31)
#define EN_REST_WAIT_MASK	(0xF << 20)
#define EN_FEW_WAIT_MASK	(0xF << 16)
#define CLK_DIS_WAIT_MASK	(0xF << 12)
#define SW_OVERRIDE_MASK	BIT(2)
#define HW_CONTROL_MASK		BIT(1)
#define SW_COLLAPSE_MASK	BIT(0)

/* Wait 2^n CXO cycles between all states. Here, n=2 (4 cycles). */
#define EN_REST_WAIT_VAL	(0x2 << 20)
#define EN_FEW_WAIT_VAL		(0x2 << 16)
#define CLK_DIS_WAIT_VAL	(0x2 << 12)
#define GDSC_TIMEOUT_US		50000

#define PLL_OUTCTRL BIT(0)
#define PLL_BYPASSNL BIT(1)
#define PLL_RESET_N BIT(2)
#define PLL_LOCKED_BIT BIT(16)
#define ENABLE_WAIT_MAX_LOOPS 200

static void __init reg_init(void)
{
	u32 regval, status;
	int ret;

	if (!(readl_relaxed(GCC_REG_BASE(GPLL0_STATUS))
			& gpll0.status_mask))
		configure_sr_hpm_lp_pll(&gpll0_config, &gpll0_regs, 1);

	if (!(readl_relaxed(GCC_REG_BASE(GPLL1_STATUS))
			& gpll1.status_mask))
		configure_sr_hpm_lp_pll(&gpll1_config, &gpll1_regs, 1);

	configure_sr_hpm_lp_pll(&mmpll0_config, &mmpll0_regs, 1);
	configure_sr_hpm_lp_pll(&mmpll1_config, &mmpll1_regs, 1);
	configure_sr_hpm_lp_pll(&lpapll0_config, &lpapll0_regs, 1);

	/* Enable GPLL0's aux outputs. */
	regval = readl_relaxed(GCC_REG_BASE(GPLL0_USER_CTL));
	regval |= BIT(PLL_AUX_OUTPUT_BIT) | BIT(PLL_AUX2_OUTPUT_BIT);
	writel_relaxed(regval, GCC_REG_BASE(GPLL0_USER_CTL));

	/* Vote for GPLL0 to turn on. Needed by acpuclock. */
	regval = readl_relaxed(GCC_REG_BASE(APCS_GPLL_ENA_VOTE));
	regval |= BIT(0);
	writel_relaxed(regval, GCC_REG_BASE(APCS_GPLL_ENA_VOTE));

	/*
	 * TODO: Confirm that no clocks need to be voted on in this sleep vote
	 * register.
	 */
	writel_relaxed(0x0, GCC_REG_BASE(APCS_CLOCK_SLEEP_ENA_VOTE));

	/*
	 * TODO: The following sequence enables the LPASS audio core GDSC.
	 * Remove when this becomes unnecessary.
	 */

	/*
	 * Disable HW trigger: collapse/restore occur based on registers writes
	 * Disable SW override: Use hardware state-machine for sequencing.
	 */
	regval = readl_relaxed(LPASS_REG_BASE(AUDIO_CORE_GDSCR));
	regval &= ~(HW_CONTROL_MASK | SW_OVERRIDE_MASK);

	/* Configure wait time between states. */
	regval &= ~(EN_REST_WAIT_MASK | EN_FEW_WAIT_MASK | CLK_DIS_WAIT_MASK);
	regval |= EN_REST_WAIT_VAL | EN_FEW_WAIT_VAL | CLK_DIS_WAIT_VAL;
	writel_relaxed(regval, LPASS_REG_BASE(AUDIO_CORE_GDSCR));

	regval = readl_relaxed(LPASS_REG_BASE(AUDIO_CORE_GDSCR));
	regval &= ~BIT(0);
	writel_relaxed(regval, LPASS_REG_BASE(AUDIO_CORE_GDSCR));

	ret = readl_poll_timeout(LPASS_REG_BASE(AUDIO_CORE_GDSCR), status,
				status & PWR_ON_MASK, 50, GDSC_TIMEOUT_US);
	WARN(ret, "LPASS Audio Core GDSC did not power on.\n");
}
static void __init msm8226_clock_post_init(void)
{

	/* Set rates for single-rate clocks. */
	clk_set_rate(&usb_hs_system_clk_src.c,
			usb_hs_system_clk_src.freq_tbl[0].freq_hz);
	clk_set_rate(&usb_hsic_clk_src.c,
			usb_hsic_clk_src.freq_tbl[0].freq_hz);
	clk_set_rate(&usb_hsic_io_cal_clk_src.c,
			usb_hsic_io_cal_clk_src.freq_tbl[0].freq_hz);
	clk_set_rate(&usb_hsic_system_clk_src.c,
			usb_hsic_system_clk_src.freq_tbl[0].freq_hz);
	clk_set_rate(&pdm2_clk_src.c, pdm2_clk_src.freq_tbl[0].freq_hz);
	clk_set_rate(&cci_clk_src.c, cci_clk_src.freq_tbl[0].freq_hz);
	clk_set_rate(&mclk0_clk_src.c, mclk0_clk_src.freq_tbl[0].freq_hz);
	clk_set_rate(&mclk1_clk_src.c, mclk1_clk_src.freq_tbl[0].freq_hz);
	clk_set_rate(&esc0_clk_src.c, esc0_clk_src.freq_tbl[0].freq_hz);
	clk_set_rate(&vsync_clk_src.c, vsync_clk_src.freq_tbl[0].freq_hz);
	clk_set_rate(&slimbus_clk_src.c,
			slimbus_clk_src.freq_tbl[0].freq_hz);
}

#define GCC_CC_PHYS		0xFC400000
#define GCC_CC_SIZE		SZ_16K

#define MMSS_CC_PHYS		0xFD8C0000
#define MMSS_CC_SIZE		SZ_256K

#define LPASS_CC_PHYS		0xFE000000
#define LPASS_CC_SIZE		SZ_256K

#define APCS_KPSS_SH_PLL_PHYS	0xF9016000
#define APCS_KPSS_SH_PLL_SIZE	SZ_64

#define APCS_KPSS_GLB_PHYS	0xF9011000
#define APCS_KPSS_GLB_SIZE	SZ_4K


static void __init msm8226_clock_pre_init(void)
{
	virt_bases[GCC_BASE] = ioremap(GCC_CC_PHYS, GCC_CC_SIZE);
	if (!virt_bases[GCC_BASE])
		panic("clock-8226: Unable to ioremap GCC memory!");

	virt_bases[MMSS_BASE] = ioremap(MMSS_CC_PHYS, MMSS_CC_SIZE);
	if (!virt_bases[MMSS_BASE])
		panic("clock-8226: Unable to ioremap MMSS_CC memory!");

	virt_bases[LPASS_BASE] = ioremap(LPASS_CC_PHYS, LPASS_CC_SIZE);
	if (!virt_bases[LPASS_BASE])
		panic("clock-8226: Unable to ioremap LPASS_CC memory!");

	virt_bases[APCS_BASE] = ioremap(APCS_KPSS_GLB_PHYS,
		APCS_KPSS_GLB_SIZE);
	if (!virt_bases[APCS_BASE])
		panic("clock-8226: Unable to ioremap APCS_GCC_CC memory!");

	virt_bases[APCS_PLL_BASE] = ioremap(APCS_KPSS_SH_PLL_PHYS,
		APCS_KPSS_SH_PLL_SIZE);
	if (!virt_bases[APCS_PLL_BASE])
		panic("clock-8226: Unable to ioremap APCS_GCC_CC memory!");

	clk_ops_local_pll.enable = sr_hpm_lp_pll_clk_enable;

	vdd_dig_reg = regulator_get(NULL, "vdd_dig");
	if (IS_ERR(vdd_dig_reg))
		panic("clock-8226: Unable to get the vdd_dig regulator!");

	vdd_sr2_reg = regulator_get(NULL, "vdd_sr2_pll");
	if (IS_ERR(vdd_dig_reg))
		panic("clock-8226: Unable to get the sr2_pll regulator!");

	/*
	 * The SR2 PLL is used at boot. Vote to prevent its regulator from
	 * being turned off while the PLL is still in use.
	 */
	regulator_set_voltage(vdd_sr2_reg, 1800000, 1800000);
	regulator_enable(vdd_sr2_reg);

	/*
	 * TODO: Set a voltage and enable vdd_dig, leaving the voltage high
	 * until late_init. This may not be necessary with clock handoff;
	 * Investigate this code on a real non-simulator target to determine
	 * its necessity.
	 */
	vote_vdd_level(&vdd_dig, VDD_DIG_HIGH);
	regulator_enable(vdd_dig_reg);

	/*
	 * Hold an active set vote at a rate of 40MHz for the MMSS NOC AHB
	 * source. Sleep set vote is 0.
	 * RPM will also turn on gcc_mmss_noc_cfg_ahb_clk, which is needed to
	 * access mmss clock controller registers.
	 */
	clk_set_rate(&mmssnoc_ahb_a_clk.c, 40000000);
	clk_prepare_enable(&mmssnoc_ahb_a_clk.c);

	/*
	 * Hold an active set vote for CXO; this is because CXO is expected
	 * to remain on whenever CPUs aren't power collapsed.
	 */
	clk_prepare_enable(&xo_a_clk.c);

	enable_rpm_scaling();

	reg_init();
	/*
	 * FIXME remove after bus driver is in place
	 * Requires gpll0 to be configured
	 */
	clk_set_rate(&axi_clk_src.c, 200000000);
	clk_prepare_enable(&mmss_s0_axi_clk.c);


	/* TODO: Delete this code once bootloaders enable this clk
	 * Temporarily enable a clock to allow access to LPASS core
	 * registers. Possibly requires gdsc to be enabled.
	 */
	clk_prepare_enable(&audio_core_ixfabric_clk.c);

	/*
	 * TODO: Enable the gcc_bimc_clk smcbc, which is the parent of thhe
	 * mss_gcc_q6_bimc_axi_clk
	 */
	writel_relaxed(0x1, GCC_REG_BASE(0x1118));
}

static int __init msm8226_clock_late_init(void)
{
	return unvote_vdd_level(&vdd_dig, VDD_DIG_HIGH);
}

struct clock_init_data msm8226_clock_init_data __initdata = {
	.table = msm_clocks_8226,
	.size = ARRAY_SIZE(msm_clocks_8226),
	.pre_init = msm8226_clock_pre_init,
	.post_init = msm8226_clock_post_init,
	.late_init = msm8226_clock_late_init,
};
