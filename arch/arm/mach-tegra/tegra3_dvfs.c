/*
 * arch/arm/mach-tegra/tegra3_dvfs.c
 *
 * Copyright (C) 2010-2012, NVIDIA CORPORATION. All rights reserved.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/string.h>
#include <linux/module.h>
#include <linux/clk.h>
#include <linux/kobject.h>
#include <linux/err.h>
#include <linux/time.h>

#include "clock.h"
#include "dvfs.h"
#include "fuse.h"
#include "board.h"
#include "tegra3_emc.h"
#include <mach/board-cardhu-misc.h>
#include <mach/hundsbuah.h>

#ifdef CONFIG_VOLTAGE_CONTROL
int user_mv_table[MAX_DVFS_FREQS] = {
		762, 780, 805, 825, 875, 925, 950,  1000, 1050, 1100, 1150, 1175, 1237, 1270, 1300, 1330, HUNDSBUAH_MAX_CPU_VOLTAGE  };
#endif

#define CPU_MILLIVOLTS {\
		762, 780, 805, 825, 875, 925, 950,  1000, 1050, 1100, 1150, 1175, 1237, 1270, 1300, 1330, HUNDSBUAH_MAX_CPU_VOLTAGE  };

static const unsigned int cpu_cold_offs_mhz[MAX_DVFS_FREQS] = {
		50, 50, 50, 50, 50, 50, 50,  50, 50, 50, 50, 50, 50, 50, 50, 50, 50 };
    
static bool tegra_dvfs_cpu_disabled;
static bool tegra_dvfs_core_disabled;
static struct dvfs *cpu_dvfs;

int cpu_millivolts[MAX_DVFS_FREQS] = CPU_MILLIVOLTS;

static int cpu_millivolts_aged[MAX_DVFS_FREQS] = CPU_MILLIVOLTS;

int core_millivolts[MAX_DVFS_FREQS] = {
	950,   1000,   1050,   1100,   1150,    1200,    1250,   1300, 1350,   1375,   1410,   1430,   HUNDSBUAH_MAX_CORE_VOLTAGE };

#define KHZ 1000
#define MHZ 1000000

/* VDD_CPU >= (VDD_CORE - cpu_below_core) */
/* VDD_CORE >= min_level(VDD_CPU), see tegra3_get_core_floor_mv() below */
#define VDD_CPU_BELOW_VDD_CORE		300
static int cpu_below_core = VDD_CPU_BELOW_VDD_CORE;

#define VDD_SAFE_STEP			100

static struct dvfs_rail tegra3_dvfs_rail_vdd_cpu = {
	.reg_id = "vdd_cpu",
	.max_millivolts = HUNDSBUAH_CPU_VOLTAGE_CAP,
	.min_millivolts = HUNDSBUAH_MIN_CPU_VOLTAGE,
	.step = VDD_SAFE_STEP,
	.jmp_to_zero = true,
};

static struct dvfs_rail tegra3_dvfs_rail_vdd_core = {
	.reg_id = "vdd_core",
	.max_millivolts = HUNDSBUAH_CORE_VOLTAGE_CAP,
	.min_millivolts = HUNDSBUAH_MIN_CORE_VOLTAGE,
	.step = VDD_SAFE_STEP,
};

static struct dvfs_rail *tegra3_dvfs_rails[] = {
	&tegra3_dvfs_rail_vdd_cpu,
	&tegra3_dvfs_rail_vdd_core,
};

static int tegra3_get_core_floor_mv(int cpu_mv)
{
	if (cpu_mv <= cpu_millivolts[0])
		return core_millivolts[0];
	
   if (cpu_mv < cpu_millivolts[1])
		return core_millivolts[1];
	
   if (cpu_mv < cpu_millivolts[2])
		return core_millivolts[2];
	
   if (cpu_mv < cpu_millivolts[3])
		return core_millivolts[3];
	
   if (cpu_mv < cpu_millivolts[4])
		return core_millivolts[4];
	
   if (cpu_mv < cpu_millivolts[5])
      return core_millivolts[5];
   
   if (cpu_mv < cpu_millivolts[6])
      return core_millivolts[6];
   
   if (cpu_mv < cpu_millivolts[7])
      return core_millivolts[7];
   
   if (cpu_mv < cpu_millivolts[8])
      return core_millivolts[8];
   
   if (cpu_mv < cpu_millivolts[9])
      return core_millivolts[9];

   if (cpu_mv < cpu_millivolts[10])
      return core_millivolts[10];
      
   if ((cpu_mv >= cpu_millivolts[11]) && (cpu_mv <= HUNDSBUAH_CPU_VOLTAGE_CAP))
      return core_millivolts[11];
}

/* vdd_core must be >= min_level as a function of vdd_cpu */
static int tegra3_dvfs_rel_vdd_cpu_vdd_core(struct dvfs_rail *vdd_cpu,
	struct dvfs_rail *vdd_core)
{
	int core_floor = max(vdd_cpu->new_millivolts, vdd_cpu->millivolts);
	core_floor = tegra3_get_core_floor_mv(core_floor);
	return max(vdd_core->new_millivolts, core_floor);
}

/* vdd_cpu must be >= (vdd_core - cpu_below_core) */
static int tegra3_dvfs_rel_vdd_core_vdd_cpu(struct dvfs_rail *vdd_core,
	struct dvfs_rail *vdd_cpu)
{
	int cpu_floor;

	if (vdd_cpu->new_millivolts == 0)
		return 0; /* If G CPU is off, core relations can be ignored */

	cpu_floor = max(vdd_core->new_millivolts, vdd_core->millivolts) -
		cpu_below_core;
	return max(vdd_cpu->new_millivolts, cpu_floor);
}

static struct dvfs_relationship tegra3_dvfs_relationships[] = {
	{
		.from = &tegra3_dvfs_rail_vdd_cpu,
		.to = &tegra3_dvfs_rail_vdd_core,
		.solve = tegra3_dvfs_rel_vdd_cpu_vdd_core,
		.solved_at_nominal = true,
	},
	{
		.from = &tegra3_dvfs_rail_vdd_core,
		.to = &tegra3_dvfs_rail_vdd_cpu,
		.solve = tegra3_dvfs_rel_vdd_core_vdd_cpu,
	},
};

#define CPU_DVFS(_clk_name, _speedo_id, _process_id, _mult, _freqs...)	\
	{								\
		.clk_name	= _clk_name,				\
		.speedo_id	= _speedo_id,				\
		.process_id	= _process_id,				\
		.freqs		= {_freqs},				\
		.freqs_mult	= _mult,				\
		.millivolts	= cpu_millivolts,			\
		.auto_dvfs	= true,					\
		.dvfs_rail	= &tegra3_dvfs_rail_vdd_cpu,		\
	}

static struct dvfs cpu_dvfs_table[] = {
	/* Cpu voltages (mV):	      762, 780, 805, 825, 875, 925, 950,  1000, 1050, 1100, 1150, 1175, 1237, 1270, 1300, 1330, HUNDSBUAH_MAX_CPU_VOLTAGE  }; */
	CPU_DVFS("cpu_g", 12, 3, MHZ, 475, 513, 579, 620, 760, 910, 1000, 1150, 1300, 1400, 1500, 1600, 1700, 1750, 1800, 1850, HUNDSBUAH_MAX_CPU_FREQUENCY),
	CPU_DVFS("cpu_g", 12, 4, MHZ, 475, 513, 579, 620, 760, 910, 1000, 1150, 1300, 1400, 1500, 1600, 1700, 1750, 1800, 1850, HUNDSBUAH_MAX_CPU_FREQUENCY),

	/*
	 * "Safe entry" to be used when no match for chip speedo, process
	 *  corner is found (just to boot at low rate); must be the last one
	 */
	CPU_DVFS("cpu_g", -1, -1, MHZ, 1,   1, 216, 216, 300),
};

static struct dvfs cpu_0_dvfs_table[] = {
	/* Cpu voltages (mV):	      762, 780, 805, 825, 875, 925, 950,  1000, 1050, 1100, 1150, 1175, 1237, 1270, 1300, 1330, HUNDSBUAH_MAX_CPU_VOLTAGE  }; */
	CPU_DVFS("cpu_0", 12, 3, MHZ, 475, 513, 579, 620, 760, 910, 1000, 1150, 1300, 1400, 1500, 1600, 1700, 1750, 1800, 1850, HUNDSBUAH_MAX_CPU_FREQUENCY),
	CPU_DVFS("cpu_0", 12, 4, MHZ, 475, 513, 579, 620, 760, 910, 1000, 1150, 1300, 1400, 1500, 1600, 1700, 1750, 1800, 1850, HUNDSBUAH_MAX_CPU_FREQUENCY),
};

#define CORE_DVFS(_clk_name, _speedo_id, _auto, _mult, _freqs...)	\
	{							\
		.clk_name	= _clk_name,			\
		.speedo_id	= _speedo_id,			\
		.process_id	= -1,				\
		.freqs		= {_freqs},			\
		.freqs_mult	= _mult,			\
		.millivolts	= core_millivolts,		\
		.auto_dvfs	= _auto,			\
		.dvfs_rail	= &tegra3_dvfs_rail_vdd_core,	\
	}
/* For better reading in the DVFS Table */
#define HOST1X ( HUNDSBUAH_MAX_HOST1X_FREQUENCY * 1000 ) 
static struct dvfs core_dvfs_table[] = {
	/* Core voltages (mV):		  	        950,     1000,     1050,     1100,     1150,        1200,        1250,     1300,     1350,     1375,     1410,     1430,   HUNDSBUAH_MAX_CORE_VOLTAGE } */
	CORE_DVFS("cpu_lp",     2, 1, KHZ, 204000,   295000,   370000,   428000,   475000,      513000,      579000,   620000,   620000,   620000,   620000,   620000,   620000),
	CORE_DVFS("emc",        2, 1, KHZ, 102000,   450000,   450000,   450000,   450000,      667000,      667000,   800000,   900000,   900000,   900000,   900000,   900000),
	CORE_DVFS("sbus",       2, 1, KHZ, 102000,   205000,   205000,   227000,   227000,      267000,      334000,   334000,   334000,   334000,   334000,   334000,   334000),
	CORE_DVFS("vi",         2, 1, KHZ,      1,   219000,   267000,   300000,   371000,      409000,      425000,   425000,   425000,   425000,   425000,   425000,   425000),
	CORE_DVFS("vde",        2, 1, KHZ, 200000,   247000,   304000,   352000,   400000,      437000,      484000,   520000,   600000,   650000,   700000,   750000,   HUNDSBUAH_MAX_CORE_FREQUENCY   *   1000   ),
	CORE_DVFS("mpe",        2, 1, KHZ, 200000,   247000,   304000,   361000,   408000,      446000,      484000,   520000,   600000,   650000,   700000,   750000,   HUNDSBUAH_MAX_CORE_FREQUENCY   *   1000   ),
	CORE_DVFS("2d",         2, 1, KHZ, 200000,   267000,   304000,   361000,   408000,      446000,      484000,   520000,   600000,   650000,   700000,   750000,   HUNDSBUAH_MAX_CORE_FREQUENCY   *   1000   ),
	CORE_DVFS("epp",        2, 1, KHZ, 200000,   267000,   304000,   361000,   408000,      446000,      484000,   520000,   600000,   650000,   700000,   750000,   HUNDSBUAH_MAX_CORE_FREQUENCY   *   1000   ),
	CORE_DVFS("3d",         2, 1, KHZ, 200000,   247000,   304000,   361000,   408000,      446000,      484000,   520000,   600000,   650000,   700000,   750000,   HUNDSBUAH_MAX_CORE_FREQUENCY   *   1000   ),
	CORE_DVFS("3d2",        2, 1, KHZ, 200000,   247000,   304000,   361000,   408000,      446000,      484000,   520000,   600000,   650000,   700000,   750000,   HUNDSBUAH_MAX_CORE_FREQUENCY   *   1000   ),
	CORE_DVFS("se",         2, 1, KHZ, 200000,   267000,   304000,   361000,   408000,      446000,      484000,   520000,   600000,   650000,   700000,   750000,   HUNDSBUAH_MAX_CORE_FREQUENCY   *   1000   ),
	CORE_DVFS("host1x",     2, 1, KHZ, 100000,   152000,   188000,   222000,   254000,      267000,      267000,   267000,   300000,   325000,   HOST1X,   HOST1X,   HOST1X),
	CORE_DVFS("cbus",       2, 1, KHZ, 200000,   247000,   304000,   352000,   400000,      437000,      484000,   520000,   600000,   650000,   700000,   750000,   HUNDSBUAH_MAX_CORE_FREQUENCY   *   1000   ),
	CORE_DVFS("pll_c",     -1, 1, KHZ, 533000,   667000,   667000,   800000,   800000,     1066000,     1066000,  1066000,  1200000,  1300000,  1400000,  1500000,   HUNDSBUAH_MAX_CORE_FREQUENCY   *   1000   *   2   ),
	CORE_DVFS("mipi",       2, 1, KHZ,      1,        1,        1,        1,        1,       60000,       60000,    60000,    60000,    60000,    60000,    60000,    60000),
	CORE_DVFS("fuse_burn", -1, 1, KHZ,      1,        1,        1,        1,    26000,       26000,       26000,    26000,    26000,    26000,    26000,    26000,    26000),
	CORE_DVFS("sdmmc1",    -1, 1, KHZ, 104000,   104000,   104000,   104000,   104000,      208000,      208000,   208000,   208000,   208000,   208000,   208000,   208000),
	CORE_DVFS("sdmmc3",    -1, 1, KHZ, 104000,   104000,   104000,   104000,   104000,      208000,      208000,   208000,   208000,   208000,   208000,   208000,   208000),
	CORE_DVFS("sdmmc4",    -1, 1, KHZ,  51000,   102000,   102000,   102000,   102000,      102000,      102000,   102000,   102000,   102000,   102000,   102000,   102000),
	CORE_DVFS("ndflash",   -1, 1, KHZ, 120000,   120000,   120000,   120000,   200000,      200000,      200000,   200000,   200000,   200000,   200000,   200000,   200000),
	CORE_DVFS("nor",        2, 1, KHZ, 102000,   115000,   130000,   130000,   133000,      133000,      133000,   133000,   133000,   133000,   133000,   133000,   133000),
	CORE_DVFS("sbc1",      -1, 1, KHZ,  36000,    52000,    60000,    60000,    60000,      100000,      100000,   100000,   100000,   100000,   100000,   100000,   100000),
	CORE_DVFS("sbc2",      -1, 1, KHZ,  36000,    52000,    60000,    60000,    60000,      100000,      100000,   100000,   100000,   100000,   100000,   100000,   100000),
	CORE_DVFS("sbc3",      -1, 1, KHZ,  36000,    52000,    60000,    60000,    60000,      100000,      100000,   100000,   100000,   100000,   100000,   100000,   100000),
	CORE_DVFS("sbc4",      -1, 1, KHZ,  36000,    52000,    60000,    60000,    60000,      100000,      100000,   100000,   100000,   100000,   100000,   100000,   100000),
	CORE_DVFS("sbc5",      -1, 1, KHZ,  36000,    52000,    60000,    60000,    60000,      100000,      100000,   100000,   100000,   100000,   100000,   100000,   100000),
	CORE_DVFS("sbc6",      -1, 1, KHZ,  36000,    52000,    60000,    60000,    60000,      100000,      100000,   100000,   100000,   100000,   100000,   100000,   100000),
	CORE_DVFS("sata",      -1, 1, KHZ,      1,   216000,   216000,   216000,   216000,      216000,      216000,   216000,   216000,   216000,   216000,   216000,   216000),
	CORE_DVFS("sata_oob",  -1, 1, KHZ,      1,   216000,   216000,   216000,   216000,      216000,      216000,   216000,   216000,   216000,   216000,   216000,   216000),
	CORE_DVFS("tvo",       -1, 1, KHZ,      1,        1,   297000,   297000,   297000,      297000,      297000,   297000,   297000,   297000,   297000,   297000,   297000),
	CORE_DVFS("cve",       -1, 1, KHZ,      1,        1,   297000,   297000,   297000,      297000,      297000,   297000,   297000,   297000,   297000,   297000,   297000),
	CORE_DVFS("dsia",      -1, 1, KHZ, 432500,   432500,   432500,   432500,   432500,      432500,      432500,   432500,   432500,   432500,   432500,   432500,   432500),
	CORE_DVFS("dsib",      -1, 1, KHZ, 432500,   432500,   432500,   432500,   432500,      432500,      432500,   432500,   432500,   432500,   432500,   432500,   432500),
   CORE_DVFS("pwm",       -1, 1, KHZ, 204000,   408000,   408000,   408000,   408000,      408000,      408000,   408000,   408000,   408000,   408000,   408000,   408000),
	CORE_DVFS("disp1",      2, 0, KHZ, 155000,   155000,   268000,   268000,   268000,      268000,      268000,   268000,   268000,   268000,   268000,   268000,   268000),
	CORE_DVFS("disp2",      2, 0, KHZ, 155000,   155000,   268000,   268000,   268000,      268000,      268000,   268000,   268000,   268000,   268000,   268000,   268000),
   CORE_DVFS("pll_m",     -1, 1, KHZ, 533000,   667000,   667000,   800000,   800000,     1066000,     1066000,  1066000,  1066000,  1066000,  1066000,  1066000,  1066000),
#ifdef CONFIG_TEGRA_PLLM_RESTRICTED
	CORE_DVFS("pll_m",      2, 1, KHZ, 533000,   900000,   900000,   900000,   900000,     1066000,     1066000,  1066000,  1066000,  1066000,  1066000,  1066000,  1066000),
#endif
};

/* CPU alternative DVFS table for cold zone */
static unsigned long cpu_cold_freqs[MAX_DVFS_FREQS];

/* CPU alternative DVFS table for single G CPU core 0 */
static unsigned long *cpu_0_freqs;

int tegra_dvfs_disable_core_set(const char *arg, const struct kernel_param *kp)
{
	int ret;

	ret = param_set_bool(arg, kp);
	if (ret)
		return ret;

	if (tegra_dvfs_core_disabled)
		tegra_dvfs_rail_disable(&tegra3_dvfs_rail_vdd_core);
	else
		tegra_dvfs_rail_enable(&tegra3_dvfs_rail_vdd_core);

	return 0;
}

int tegra_dvfs_disable_cpu_set(const char *arg, const struct kernel_param *kp)
{
	int ret;

	ret = param_set_bool(arg, kp);
	if (ret)
		return ret;

	if (tegra_dvfs_cpu_disabled)
		tegra_dvfs_rail_disable(&tegra3_dvfs_rail_vdd_cpu);
	else
		tegra_dvfs_rail_enable(&tegra3_dvfs_rail_vdd_cpu);

	return 0;
}

int tegra_dvfs_disable_get(char *buffer, const struct kernel_param *kp)
{
	return param_get_bool(buffer, kp);
}

static struct kernel_param_ops tegra_dvfs_disable_core_ops = {
	.set = tegra_dvfs_disable_core_set,
	.get = tegra_dvfs_disable_get,
};

static struct kernel_param_ops tegra_dvfs_disable_cpu_ops = {
	.set = tegra_dvfs_disable_cpu_set,
	.get = tegra_dvfs_disable_get,
};

module_param_cb(disable_core, &tegra_dvfs_disable_core_ops,
	&tegra_dvfs_core_disabled, 0644);
module_param_cb(disable_cpu, &tegra_dvfs_disable_cpu_ops,
	&tegra_dvfs_cpu_disabled, 0644);

static bool __init is_pllm_dvfs(struct clk *c, struct dvfs *d)
{
#ifdef CONFIG_TEGRA_PLLM_RESTRICTED
	/* Do not apply common PLLM dvfs table on T30, T33, T37 rev A02+ and
	   do not apply restricted PLLM dvfs table for other SKUs/revs */
	int cpu = tegra_cpu_speedo_id();
	if (((cpu == 2) || (cpu == 5) || (cpu == 13)) ==
	    (d->speedo_id == -1))
		return false;
#endif
	/* Check if PLLM boot frequency can be applied to clock tree at
	   minimum voltage. If yes, no need to enable dvfs on PLLM */
	if (clk_get_rate_all_locked(c) <= d->freqs[0] * d->freqs_mult)
		return false;

	return true;
}

static void __init init_dvfs_one(struct dvfs *d, int nominal_mv_index)
{
	int ret;
	struct clk *c = tegra_get_clock_by_name(d->clk_name);

	if (!c) {
		pr_debug("tegra3_dvfs: no clock found for %s\n",
			d->clk_name);
		return;
	}

	/*
	 * Update max rate for auto-dvfs clocks, except EMC.
	 * EMC is a special case, since EMC dvfs is board dependent: max rate
	 * and EMC scaling frequencies are determined by tegra BCT (flashed
	 * together with the image) and board specific EMC DFS table; we will
	 * check the scaling ladder against nominal core voltage when the table
	 * is loaded (and if on particular board the table is not loaded, EMC
	 * scaling is disabled).
	 */
	if (!(c->flags & PERIPH_EMC_ENB) && d->auto_dvfs) {
		BUG_ON(!d->freqs[nominal_mv_index]);
		tegra_init_max_rate(
			c, d->freqs[nominal_mv_index] * d->freqs_mult);
	}
	d->max_millivolts = d->dvfs_rail->nominal_millivolts;

	/*
	 * Check if we may skip enabling dvfs on PLLM. PLLM is a special case,
	 * since its frequency never exceeds boot rate, and configuration with
	 * restricted PLLM usage is possible.
	 */
	if (!(c->flags & PLLM) || is_pllm_dvfs(c, d)) {
		ret = tegra_enable_dvfs_on_clk(c, d);
		if (ret)
			pr_err("tegra3_dvfs: failed to enable dvfs on %s\n",
				c->name);
	}
}

static void __init init_dvfs_cold(struct dvfs *d, int nominal_mv_index)
{
	int i;
	unsigned long offs;

	BUG_ON((nominal_mv_index == 0) || (nominal_mv_index > d->num_freqs));

	for (i = 0; i < d->num_freqs; i++) {
		offs = cpu_cold_offs_mhz[i] * MHZ;
		if (i > nominal_mv_index)
			cpu_cold_freqs[i] = cpu_cold_freqs[i - 1];
		else if (d->freqs[i] > offs)
			cpu_cold_freqs[i] = d->freqs[i] - offs;
		else {
			cpu_cold_freqs[i] = d->freqs[i];
			pr_warn("tegra3_dvfs: cold offset %lu is too high for"
				" regular dvfs limit %lu\n", offs, d->freqs[i]);
		}

		if (i)
			BUG_ON(cpu_cold_freqs[i] < cpu_cold_freqs[i - 1]);
	}
}

static bool __init match_dvfs_one(struct dvfs *d, int speedo_id, int process_id)
{
	if ((d->process_id != -1 && d->process_id != process_id) ||
		(d->speedo_id != -1 && d->speedo_id != speedo_id)) {
		pr_debug("tegra3_dvfs: rejected %s speedo %d,"
			" process %d\n", d->clk_name, d->speedo_id,
			d->process_id);
		return false;
	}
	return true;
}

static void __init init_cpu_0_dvfs(struct dvfs *cpud)
{
	int i;
	struct dvfs *d = NULL;

	/* Init single G CPU core 0 dvfs if this particular SKU/bin has it.
	   Max rates in multi-core and single-core tables must be the same */
	for (i = 0; i <  ARRAY_SIZE(cpu_0_dvfs_table); i++) {
		if (match_dvfs_one(&cpu_0_dvfs_table[i],
				   cpud->speedo_id, cpud->process_id)) {
			d = &cpu_0_dvfs_table[i];
			break;
		}
	}

	if (d) {
		for (i = 0; i < cpud->num_freqs; i++) {
			d->freqs[i] *= d->freqs_mult;
			if (d->freqs[i] == 0) {
				BUG_ON(i == 0);
				d->freqs[i] = d->freqs[i - 1];
			}
		}
		BUG_ON(cpud->freqs[cpud->num_freqs - 1] !=
		       d->freqs[cpud->num_freqs - 1]);
		cpu_0_freqs = d->freqs;
	}
}

static int __init get_cpu_nominal_mv_index(
	int speedo_id, int process_id, struct dvfs **cpu_dvfs)
{
	int i, j, mv;
	struct dvfs *d;
	struct clk *c;

	/*
	 * Find maximum cpu voltage that satisfies cpu_to_core dependency for
	 * nominal core voltage ("solve from cpu to core at nominal"). Clip
	 * result to the nominal cpu level for the chips with this speedo_id.
	 */
	mv = tegra3_dvfs_rail_vdd_core.nominal_millivolts;
        pr_info("tegra3_dvfs: %s: tegra3_dvfs_rail_vdd_core.nominal_millivolts mV for cpu_speedo_id: %u is %umV\n",__func__,speedo_id,mv);
	for (i = 0; i < MAX_DVFS_FREQS; i++) {
		if ((cpu_millivolts[i] == 0) ||
		    tegra3_get_core_floor_mv(cpu_millivolts[i]) > mv)
			break;
	}
	BUG_ON(i == 0);
	mv = cpu_millivolts[i - 1];
        pr_info("tegra3_dvfs: %s: cpu mv: %i\n", __func__, mv);
	BUG_ON(mv < tegra3_dvfs_rail_vdd_cpu.min_millivolts);
	mv = min(mv, tegra_cpu_speedo_mv());
        pr_info("tegra3_dvfs: %s: nominal mV for cpu_speedo_id:%u is %umV\n",__func__,speedo_id,mv);

	/*
	 * Find matching cpu dvfs entry, and use it to determine index to the
	 * final nominal voltage, that satisfies the following requirements:
	 * - allows CPU to run at minimum of the maximum rates specified in
	 *   the dvfs entry and clock tree
	 * - does not violate cpu_to_core dependency as determined above
	 */
	for (i = 0, j = 0; j <  ARRAY_SIZE(cpu_dvfs_table); j++) {
		d = &cpu_dvfs_table[j];
		if (match_dvfs_one(d, speedo_id, process_id)) {
			c = tegra_get_clock_by_name(d->clk_name);
			BUG_ON(!c);

			for (; i < MAX_DVFS_FREQS; i++) {
				if ((d->freqs[i] == 0) ||
				    (cpu_millivolts[i] == 0) ||
				    (mv < cpu_millivolts[i]))
					break;

				if (c->max_rate <= d->freqs[i]*d->freqs_mult) {
					i++;
					break;
				}
			}
			break;
		}
	}

	BUG_ON(i == 0);
	if (j == (ARRAY_SIZE(cpu_dvfs_table) - 1))
		pr_err("tegra3_dvfs: WARNING!!!\n"
		       "tegra3_dvfs: no cpu dvfs table found for chip speedo_id"
		       " %d and process_id %d: set CPU rate limit at %lu\n"
		       "tegra3_dvfs: WARNING!!!\n",
		       speedo_id, process_id, d->freqs[i-1] * d->freqs_mult);

	*cpu_dvfs = d;
        pr_info("tegra3_dvfs: %s: cpu_nominal_mv_index: %i\n",__func__, i - 1);

	return (i - 1);
}

static int __init get_core_nominal_mv_index(int speedo_id)
{
	int i;
	int mv = tegra_core_speedo_mv();
	int core_edp_limit = get_core_edp();

	/*
	 * Start with nominal level for the chips with this speedo_id. Then,
	 * make sure core nominal voltage is below edp limit for the board
	 * (if edp limit is set).
	 */
	if (core_edp_limit)
		mv = min(mv, core_edp_limit);

	/* Round nominal level down to the nearest core scaling step */
	for (i = 0; i < MAX_DVFS_FREQS; i++) {
		if ((core_millivolts[i] == 0) || (mv < core_millivolts[i]))
			break;
	}

	if (i == 0) {
		pr_err("tegra3_dvfs: unable to adjust core dvfs table to"
		       " nominal voltage %d\n", mv);
		return -ENOSYS;
	}
	return (i - 1);
}

static void tegra_adjust_cpu_mvs(int mvs)
{
	int i;

	BUG_ON(ARRAY_SIZE(cpu_millivolts) != ARRAY_SIZE(cpu_millivolts_aged));

	for (i = 0; i < ARRAY_SIZE(cpu_millivolts); i++)
		cpu_millivolts[i] = cpu_millivolts_aged[i] - mvs;
}

/**
 * Adjust VDD_CPU to offset aging.
 * 25mV for 1st year
 * 12mV for 2nd and 3rd year
 * 0mV for 4th year onwards
 */
void tegra_dvfs_age_cpu(int cur_linear_age)
{
	int chip_linear_age;
	int chip_life;
	chip_linear_age = tegra_get_age();
	chip_life = cur_linear_age - chip_linear_age;

	/*For T37 and AP37*/
	if (tegra_cpu_speedo_id() == 12 || tegra_cpu_speedo_id() == 13) {
		if (chip_linear_age <= 0) {
			return;
		} else if (chip_life <= 12) {
			tegra_adjust_cpu_mvs(25);
		} else if (chip_life <= 36) {
			tegra_adjust_cpu_mvs(13);
		}
	}
}

void __init tegra_soc_init_dvfs(void)
{
	int cpu_speedo_id = tegra_cpu_speedo_id();
	int soc_speedo_id = tegra_soc_speedo_id();
	int cpu_process_id = tegra_cpu_process_id();
	int core_process_id = tegra_core_process_id();

	int i;
	int core_nominal_mv_index;
	int cpu_nominal_mv_index;

#ifndef CONFIG_TEGRA_CORE_DVFS
	tegra_dvfs_core_disabled = true;
#endif
#ifndef CONFIG_TEGRA_CPU_DVFS
	tegra_dvfs_cpu_disabled = true;
#endif

	/*
	 * Find nominal voltages for core (1st) and cpu rails before rail
	 * init. Nominal voltage index in the scaling ladder will also be
	 * used to determine max dvfs frequency for the respective domains.
	 */
	core_nominal_mv_index = get_core_nominal_mv_index(soc_speedo_id);
	if (core_nominal_mv_index < 0) {
		tegra3_dvfs_rail_vdd_core.disabled = true;
		tegra_dvfs_core_disabled = true;
		core_nominal_mv_index = 0;
	}
	tegra3_dvfs_rail_vdd_core.nominal_millivolts =
		core_millivolts[core_nominal_mv_index];

   pr_info("%s: core nominal mv index: %d\n", __func__, core_nominal_mv_index);
      
	cpu_nominal_mv_index = get_cpu_nominal_mv_index(
		cpu_speedo_id, cpu_process_id, &cpu_dvfs);
	BUG_ON((cpu_nominal_mv_index < 0) || (!cpu_dvfs));
	tegra3_dvfs_rail_vdd_cpu.nominal_millivolts =
		cpu_millivolts[cpu_nominal_mv_index];
   
   pr_info("%s: cpu nominal mv index: %d\n", __func__, cpu_nominal_mv_index);
	
   /* Init rail structures and dependencies */
	tegra_dvfs_init_rails(tegra3_dvfs_rails, ARRAY_SIZE(tegra3_dvfs_rails));
	tegra_dvfs_add_relationships(tegra3_dvfs_relationships,
		ARRAY_SIZE(tegra3_dvfs_relationships));

	/* Search core dvfs table for speedo/process matching entries and
	   initialize dvfs-ed clocks */
	for (i = 0; i <  ARRAY_SIZE(core_dvfs_table); i++) {
		struct dvfs *d = &core_dvfs_table[i];
		if (!match_dvfs_one(d, soc_speedo_id, core_process_id))
			continue;
		init_dvfs_one(d, core_nominal_mv_index);
	}

	/* Initialize matching cpu dvfs entry already found when nominal
	   voltage was determined */
	init_dvfs_one(cpu_dvfs, cpu_nominal_mv_index);

	/* Initialize alternative cold zone and single core tables */
	init_dvfs_cold(cpu_dvfs, cpu_nominal_mv_index);
	init_cpu_0_dvfs(cpu_dvfs);

	/* Finally disable dvfs on rails if necessary */
	if (tegra_dvfs_core_disabled)
		tegra_dvfs_rail_disable(&tegra3_dvfs_rail_vdd_core);
	if (tegra_dvfs_cpu_disabled)
		tegra_dvfs_rail_disable(&tegra3_dvfs_rail_vdd_cpu);

	pr_info("tegra dvfs: VDD_CPU nominal %dmV, scaling %s\n",
		tegra3_dvfs_rail_vdd_cpu.nominal_millivolts,
		tegra_dvfs_cpu_disabled ? "disabled" : "enabled");
	pr_info("tegra dvfs: VDD_CORE nominal %dmV, scaling %s\n",
		tegra3_dvfs_rail_vdd_core.nominal_millivolts,
		tegra_dvfs_core_disabled ? "disabled" : "enabled");
}

int tegra_cpu_dvfs_alter(int edp_thermal_index, const cpumask_t *cpus,
			  bool before_clk_update, int cpu_event)
{
	bool cpu_warm = !!edp_thermal_index;
	unsigned int n = cpumask_weight(cpus);
	unsigned long *alt_freqs = cpu_warm ?
		(n > 1 ? NULL : cpu_0_freqs) : cpu_cold_freqs;

	if (cpu_event || (cpu_warm == before_clk_update)) {
		int ret = tegra_dvfs_alt_freqs_set(cpu_dvfs, alt_freqs);
		if (ret) {
			pr_err("tegra dvfs: failed to set alternative dvfs on "
			       "%u %s CPUs\n", n, cpu_warm ? "warm" : "cold");
			return ret;
		}
	}
	return 0;
}

int tegra_dvfs_rail_disable_prepare(struct dvfs_rail *rail)
{
	int ret = 0;

	if (tegra_emc_get_dram_type() != DRAM_TYPE_DDR3)
		return ret;

	if (((&tegra3_dvfs_rail_vdd_core == rail) &&
	     (rail->nominal_millivolts > TEGRA_EMC_BRIDGE_MVOLTS_MIN)) ||
	    ((&tegra3_dvfs_rail_vdd_cpu == rail) &&
	     (tegra3_get_core_floor_mv(rail->nominal_millivolts) >
	      TEGRA_EMC_BRIDGE_MVOLTS_MIN))) {
		struct clk *bridge = tegra_get_clock_by_name("bridge.emc");
		BUG_ON(!bridge);

		ret = clk_enable(bridge);
		pr_info("%s: %s: %s bridge.emc\n", __func__,
			rail->reg_id, ret ? "failed to enable" : "enabled");
	}
	return ret;
}

int tegra_dvfs_rail_post_enable(struct dvfs_rail *rail)
{
	if (tegra_emc_get_dram_type() != DRAM_TYPE_DDR3)
		return 0;

	if (((&tegra3_dvfs_rail_vdd_core == rail) &&
	     (rail->nominal_millivolts > TEGRA_EMC_BRIDGE_MVOLTS_MIN)) ||
	    ((&tegra3_dvfs_rail_vdd_cpu == rail) &&
	     (tegra3_get_core_floor_mv(rail->nominal_millivolts) >
	      TEGRA_EMC_BRIDGE_MVOLTS_MIN))) {
		struct clk *bridge = tegra_get_clock_by_name("bridge.emc");
		BUG_ON(!bridge);

		clk_disable(bridge);
		pr_info("%s: %s: disabled bridge.emc\n",
			__func__, rail->reg_id);
	}
	return 0;
}

/*
 * sysfs and dvfs interfaces to cap tegra core domains frequencies
 */
static DEFINE_MUTEX(core_cap_lock);

struct core_cap {
	int refcnt;
	int level;
};
static struct core_cap tegra3_core_cap;
static struct core_cap kdvfs_core_cap;
static struct core_cap user_core_cap;

static struct core_cap user_cbus_cap;

static struct kobject *cap_kobj;

/* Arranged in order required for enabling/lowering the cap */
static struct {
	const char *cap_name;
	struct clk *cap_clk;
	unsigned long freqs[MAX_DVFS_FREQS];
} core_cap_table[] = {
	{ .cap_name = "cap.cbus" },
	{ .cap_name = "cap.sclk" },
	{ .cap_name = "cap.emc" },
};


static void core_cap_level_set(int level)
{
	int i, j;

	for (j = 0; j < ARRAY_SIZE(core_millivolts); j++) {
		int v = core_millivolts[j];
		if ((v == 0) || (level < v))
			break;
	}
	j = (j == 0) ? 0 : j - 1;
	level = core_millivolts[j];

	if (level < tegra3_core_cap.level) {
		for (i = 0; i < ARRAY_SIZE(core_cap_table); i++)
			if (core_cap_table[i].cap_clk)
				clk_set_rate(core_cap_table[i].cap_clk,
					     core_cap_table[i].freqs[j]);
	} else if (level > tegra3_core_cap.level) {
		for (i = ARRAY_SIZE(core_cap_table) - 1; i >= 0; i--)
			if (core_cap_table[i].cap_clk)
				clk_set_rate(core_cap_table[i].cap_clk,
					     core_cap_table[i].freqs[j]);
	}
	tegra3_core_cap.level = level;
}

static void core_cap_update(void)
{
	int new_level = tegra3_dvfs_rail_vdd_core.max_millivolts;

	if (kdvfs_core_cap.refcnt)
		new_level = min(new_level, kdvfs_core_cap.level);
	if (user_core_cap.refcnt)
		new_level = min(new_level, user_core_cap.level);

	if (tegra3_core_cap.level != new_level)
		core_cap_level_set(new_level);
}

static void core_cap_enable(bool enable)
{
	if (enable)
		tegra3_core_cap.refcnt++;
	else if (tegra3_core_cap.refcnt)
		tegra3_core_cap.refcnt--;

	core_cap_update();
}

static ssize_t
core_cap_state_show(struct kobject *kobj, struct kobj_attribute *attr,
		    char *buf)
{
	return sprintf(buf, "%d (%d)\n", tegra3_core_cap.refcnt ? 1 : 0,
			user_core_cap.refcnt ? 1 : 0);
}
static ssize_t
core_cap_state_store(struct kobject *kobj, struct kobj_attribute *attr,
		     const char *buf, size_t count)
{
	int state;

	if (sscanf(buf, "%d", &state) != 1)
		return -1;

	mutex_lock(&core_cap_lock);

	if (state) {
		user_core_cap.refcnt++;
		if (user_core_cap.refcnt == 1)
			core_cap_enable(true);
	} else if (user_core_cap.refcnt) {
		user_core_cap.refcnt=0;
		if (user_core_cap.refcnt == 0)
			core_cap_enable(false);
	}

	mutex_unlock(&core_cap_lock);
	return count;
}

static ssize_t
gpu_voltages_show(struct kobject *kobj, struct kobj_attribute *attr,
		    char *buf)
{
   unsigned int idx = 0;
   int ret = 0;

   for(idx = 0; idx <= HUNDSBUAH_CORE_MAXFREQ_IDX; idx++)
   {
      ret += sprintf (&buf[ret], "%d ", core_millivolts[idx]);
   }

   ret += sprintf(&buf[ret], "\n");

	return ret;
}

static ssize_t
gpu_voltages_store(struct kobject *kobj, struct kobj_attribute *attr,
		     const char *buf, size_t count)
{
   return count;
}

static ssize_t
core_cap_level_show(struct kobject *kobj, struct kobj_attribute *attr,
		    char *buf)
{
	return sprintf(buf, "%d\n", tegra3_core_cap.level);
}
static ssize_t
core_cap_level_store(struct kobject *kobj, struct kobj_attribute *attr,
		     const char *buf, size_t count)
{
	int level;
   int idx;
   unsigned int gpu_frequency;
   struct clk *three_d = tegra_get_clock_by_name("3d");
	u32 project_id = tegra3_get_project_id();

   
	if( project_id == TEGRA3_PROJECT_TF700T || project_id == TEGRA3_PROJECT_P1801)
	{
      if (sscanf(buf, "%d", &level) != 1)
         return -1;
         
      /* TODO: Check if no entry is found! */
      for(idx = 0; idx < ARRAY_SIZE(core_dvfs_table); idx++)
      {
         if(level == core_millivolts[idx])
         {
            gpu_frequency = three_d->dvfs->freqs[idx];
            printk("Limiting (GPU etc...) to %dMHz (%dmV)!\n", gpu_frequency / 1000000, level);
            pr_info("Limiting (GPU etc...) to %dMHz (%dmV)!\n", gpu_frequency/ 1000000, level);
            break;
         }
         if(core_millivolts[idx] == 0)
         {
            printk("Limiting (GPU etc...): wrong voltage cap!");
            pr_info("Limiting (GPU etc...): wrong voltage cap!");
            break;
         }
      }
      if(level > HUNDSBUAH_MAX_CORE_VOLTAGE)
         level = HUNDSBUAH_MAX_CORE_VOLTAGE;
	}
	mutex_lock(&core_cap_lock);
	user_core_cap.level = level;
	core_cap_update();
	mutex_unlock(&core_cap_lock);
	return count;
}

static void cbus_cap_update(void)
{
	static struct clk *cbus_cap;

	if (!cbus_cap) {
		cbus_cap = tegra_get_clock_by_name("cap.profile.cbus");
		if (!cbus_cap) {
			WARN_ONCE(1, "tegra3_dvfs: cbus profiling is not supported");
			return;
		}
	}

	if (user_cbus_cap.refcnt)
		clk_set_rate(cbus_cap, user_cbus_cap.level);
	else
		clk_set_rate(cbus_cap, clk_get_max_rate(cbus_cap));
}

static ssize_t
cbus_cap_state_show(struct kobject *kobj, struct kobj_attribute *attr,
		    char *buf)
{
	return sprintf(buf, "%d\n", user_cbus_cap.refcnt ? 1 : 0);
}
static ssize_t
cbus_cap_state_store(struct kobject *kobj, struct kobj_attribute *attr,
		     const char *buf, size_t count)
{
	int state;

	if (sscanf(buf, "%d", &state) != 1)
		return -1;

	mutex_lock(&core_cap_lock);

	if (state) {
		user_cbus_cap.refcnt++;
		if (user_cbus_cap.refcnt == 1)
			cbus_cap_update();
	} else if (user_cbus_cap.refcnt) {
		user_cbus_cap.refcnt--;
		if (user_cbus_cap.refcnt == 0)
			cbus_cap_update();
	}

	mutex_unlock(&core_cap_lock);
	return count;
}

static ssize_t
cbus_cap_level_show(struct kobject *kobj, struct kobj_attribute *attr,
		    char *buf)
{
	return sprintf(buf, "%d\n", user_cbus_cap.level);
}
static ssize_t
cbus_cap_level_store(struct kobject *kobj, struct kobj_attribute *attr,
		     const char *buf, size_t count)
{
	int level;

	if (sscanf(buf, "%d", &level) != 1)
		return -1;

	mutex_lock(&core_cap_lock);
	user_cbus_cap.level = level;
	cbus_cap_update();
	mutex_unlock(&core_cap_lock);
	return count;
}

static struct kobj_attribute cap_state_attribute =
	__ATTR(core_cap_state, 0644, core_cap_state_show, core_cap_state_store);
static struct kobj_attribute cap_level_attribute =
	__ATTR(core_cap_level, 0644, core_cap_level_show, core_cap_level_store);
static struct kobj_attribute cbus_state_attribute =
	__ATTR(cbus_cap_state, 0644, cbus_cap_state_show, cbus_cap_state_store);
static struct kobj_attribute cbus_level_attribute =
	__ATTR(cbus_cap_level, 0644, cbus_cap_level_show, cbus_cap_level_store);
static struct kobj_attribute gpu_voltages_attribute =
	__ATTR(gpu_voltages, 0644, gpu_voltages_show, gpu_voltages_store);
   
const struct attribute *cap_attributes[] = {
	&cap_state_attribute.attr,
	&cap_level_attribute.attr,
	&cbus_state_attribute.attr,
	&cbus_level_attribute.attr,
   &gpu_voltages_attribute.attr,
	NULL,
};

void tegra_dvfs_core_cap_enable(bool enable)
{
	mutex_lock(&core_cap_lock);

	if (enable) {
		kdvfs_core_cap.refcnt++;
		if (kdvfs_core_cap.refcnt == 1)
			core_cap_enable(true);
	} else if (kdvfs_core_cap.refcnt) {
		kdvfs_core_cap.refcnt--;
		if (kdvfs_core_cap.refcnt == 0)
			core_cap_enable(false);
	}
	mutex_unlock(&core_cap_lock);
}

void tegra_dvfs_core_cap_level_set(int level)
{
	mutex_lock(&core_cap_lock);
	kdvfs_core_cap.level = level;
	core_cap_update();
	mutex_unlock(&core_cap_lock);
}

static int __init init_core_cap_one(struct clk *c, unsigned long *freqs)
{
	int i, v, next_v = 0;
	unsigned long rate, next_rate = 0;

	for (i = 0; i < ARRAY_SIZE(core_millivolts); i++) {
		v = core_millivolts[i];
		if (v == 0)
			break;

		for (;;) {
			rate = next_rate;
			next_rate = clk_round_rate(c, rate + 1000);
			if (IS_ERR_VALUE(next_rate)) {
				pr_debug("tegra3_dvfs: failed to round %s"
					   " rate %lu", c->name, rate);
				return -EINVAL;
			}
			if (rate == next_rate)
				break;

			next_v = tegra_dvfs_predict_millivolts(
				c->parent, next_rate);
			if (IS_ERR_VALUE(next_v)) {
				pr_debug("tegra3_dvfs: failed to predict %s mV"
					 " for rate %lu", c->name, next_rate);
				return -EINVAL;
			}
			if (next_v > v)
				break;
		}

		if (rate == 0) {
			rate = next_rate;
			pr_warn("tegra3_dvfs: minimum %s rate %lu requires"
				" %d mV", c->name, rate, next_v);
		}
		freqs[i] = rate;
		next_rate = rate;
	}
	return 0;
}

static int __init tegra_dvfs_init_core_cap(void)
{
	int i;
	struct clk *c = NULL;

	tegra3_core_cap.level = kdvfs_core_cap.level = user_core_cap.level =
		tegra3_dvfs_rail_vdd_core.max_millivolts;

	for (i = 0; i < ARRAY_SIZE(core_cap_table); i++) {
		c = tegra_get_clock_by_name(core_cap_table[i].cap_name);
		if (!c || !c->parent ||
		    init_core_cap_one(c, core_cap_table[i].freqs)) {
			pr_err("tegra3_dvfs: failed to initialize %s frequency"
			       " table", core_cap_table[i].cap_name);
			continue;
		}
		core_cap_table[i].cap_clk = c;
	}

	cap_kobj = kobject_create_and_add("tegra_cap", kernel_kobj);
	if (!cap_kobj) {
		pr_err("tegra3_dvfs: failed to create sysfs cap object");
		return 0;
	}

	if (sysfs_create_files(cap_kobj, cap_attributes)) {
		pr_err("tegra3_dvfs: failed to create sysfs cap interface");
		return 0;
	}
	pr_info("tegra dvfs: tegra sysfs cap interface is initialized\n");

	return 0;
}
late_initcall(tegra_dvfs_init_core_cap);
