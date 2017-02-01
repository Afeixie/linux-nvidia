/*
 * Copyright (c) 2015-2017, NVIDIA CORPORATION. All rights reserved
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/err.h>
#include <linux/tegra-powergate.h>
#include <dt-bindings/soc/tegra186-powergate.h>
#include <soc/tegra/bpmp_abi.h>
#include <soc/tegra/tegra_bpmp.h>
#include <soc/tegra/tegra-powergate-driver.h>

struct pg_partition_info {
	const char *name;
	int refcount;
	struct mutex pg_mutex;
};

static struct pg_partition_info t186_partition_info[] = {
	[TEGRA186_POWER_DOMAIN_AUD] = { .name = "audio" },
	[TEGRA186_POWER_DOMAIN_DFD] = { .name = "dfd" },
	[TEGRA186_POWER_DOMAIN_DISP] = { .name = "disp" },
	[TEGRA186_POWER_DOMAIN_DISPB] = { .name = "dispb" },
	[TEGRA186_POWER_DOMAIN_DISPC] = { .name = "dispc" },
	[TEGRA186_POWER_DOMAIN_ISPA] = { .name = "ispa" },
	[TEGRA186_POWER_DOMAIN_NVDEC] = { .name = "nvdec" },
	[TEGRA186_POWER_DOMAIN_NVJPG] = { .name = "nvjpg" },
	[TEGRA186_POWER_DOMAIN_MPE] = { .name = "nvenc" },
	[TEGRA186_POWER_DOMAIN_PCX] = { .name = "pcie" },
	[TEGRA186_POWER_DOMAIN_SAX] = { .name = "sata" },
	[TEGRA186_POWER_DOMAIN_VE] = { .name = "ve" },
	[TEGRA186_POWER_DOMAIN_VIC] = { .name = "vic" },
	[TEGRA186_POWER_DOMAIN_XUSBA] = { .name = "xusba" },
	[TEGRA186_POWER_DOMAIN_XUSBB] = { .name = "xusbb" },
	[TEGRA186_POWER_DOMAIN_XUSBC] = { .name = "xusbc" },
	[TEGRA186_POWER_DOMAIN_GPU] = { .name = "gpu" },
};

struct tegra_powergate_id_info {
	bool valid;
	int part_id;
	struct pg_partition_info *part_info;
};

#define TEGRA186_POWERGATE_ID_INFO(_pg_id, _part_id, _did)		\
[TEGRA_POWERGATE_##_pg_id] = {						\
	.valid = true,							\
	.part_id = _part_id,						\
	.part_info = &t186_partition_info[TEGRA186_POWER_DOMAIN_##_did], \
}

static const struct tegra_powergate_id_info
	t186_powergate_info[TEGRA_NUM_POWERGATE] = {
	TEGRA186_POWERGATE_ID_INFO(APE, 0, AUD),
	TEGRA186_POWERGATE_ID_INFO(DFD, 1, DFD),
	TEGRA186_POWERGATE_ID_INFO(DISA, 2, DISP),
	TEGRA186_POWERGATE_ID_INFO(DISB, 3, DISPB),
	TEGRA186_POWERGATE_ID_INFO(DISC, 4, DISPC),
	TEGRA186_POWERGATE_ID_INFO(ISPA, 5, ISPA),
	TEGRA186_POWERGATE_ID_INFO(NVDEC, 6, NVDEC),
	TEGRA186_POWERGATE_ID_INFO(NVJPG, 7, NVJPG),
	TEGRA186_POWERGATE_ID_INFO(NVENC, 8, MPE),
	TEGRA186_POWERGATE_ID_INFO(PCIE, 9, PCX),
	TEGRA186_POWERGATE_ID_INFO(SATA, 10, SAX),
	TEGRA186_POWERGATE_ID_INFO(VE, 11, VE),
	TEGRA186_POWERGATE_ID_INFO(VIC, 12, VIC),
	TEGRA186_POWERGATE_ID_INFO(XUSBA, 13, XUSBA),
	TEGRA186_POWERGATE_ID_INFO(XUSBB, 14, XUSBB),
	TEGRA186_POWERGATE_ID_INFO(XUSBC, 15, XUSBC),
	TEGRA186_POWERGATE_ID_INFO(GPU, 43, GPU),
};

static int pg_set_state(int id, u32 state)
{
	struct mrq_pg_request req = {
		.cmd = CMD_PG_SET_STATE,
		.id = t186_powergate_info[id].part_id,
		.set_state = {
			.state = state,
		}
	};

	return tegra_bpmp_send_receive(MRQ_PG, &req, sizeof(req), NULL, 0);
}

static int tegra186_pg_query_abi(void)
{
	int ret;
	struct mrq_query_abi_request req = { .mrq = MRQ_PG };
	struct mrq_query_abi_response resp;

	ret = tegra_bpmp_send_receive(MRQ_QUERY_ABI, &req, sizeof(req), &resp,
				      sizeof(resp));
	if (ret)
		return ret;

	return resp.status;
}

static int tegra186_pg_powergate_partition(int id)
{
	int ret = 0;
	struct pg_partition_info *partition = t186_powergate_info[id].part_info;

	mutex_lock(&partition->pg_mutex);
	if (partition->refcount) {
		if (--partition->refcount == 0)
			ret = pg_set_state(id, PG_STATE_OFF);
	} else {
		WARN(1, "partition %s refcount underflow\n",
		     partition->name);
	}
	mutex_unlock(&partition->pg_mutex);

	return ret;
}

static int tegra186_pg_unpowergate_partition(int id)
{
	int ret = 0;
	struct pg_partition_info *partition = t186_powergate_info[id].part_info;

	mutex_lock(&partition->pg_mutex);
	if (partition->refcount++ == 0)
		ret = pg_set_state(id, PG_STATE_ON);
	mutex_unlock(&partition->pg_mutex);

	return ret;
}

static int tegra186_pg_powergate_clk_off(int id)
{
	return tegra186_pg_powergate_partition(id);
}

static int tegra186_pg_unpowergate_clk_on(int id)
{
	int ret = 0;
	struct pg_partition_info *partition = t186_powergate_info[id].part_info;

	mutex_lock(&partition->pg_mutex);
	if (partition->refcount++ == 0)
		ret = pg_set_state(id, PG_STATE_RUNNING);
	mutex_unlock(&partition->pg_mutex);

	return ret;
}

static const char *tegra186_pg_get_name(int id)
{
	if (!t186_powergate_info[id].part_info)
		return NULL;

	return t186_powergate_info[id].part_info->name;
}

static bool tegra186_pg_is_powered(int id)
{
	int ret;
	struct mrq_pg_request req = {
		.cmd = CMD_PG_GET_STATE,
		.id = t186_powergate_info[id].part_id,
	};
	struct mrq_pg_response resp;

	ret = tegra_bpmp_send_receive(MRQ_PG, &req, sizeof(req), &resp, sizeof(resp));
	if (ret)
		return false;

	if (resp.get_state.state == PG_STATE_OFF)
		return false;
	else
		return true;
}

static int tegra186_pg_force_powergate(int id)
{
	int ret;

	ret = pg_set_state(id, PG_STATE_ON);
	if (ret)
		return ret;

	return pg_set_state(id, PG_STATE_OFF);
}

static int tegra186_init_refcount(void)
{
	int i;

	for (i = 0; i < TEGRA_NUM_POWERGATE; ++i) {
		if (!t186_powergate_info[i].valid)
			continue;
		mutex_init(&t186_powergate_info[i].part_info->pg_mutex);
	}

	tegra186_pg_force_powergate(TEGRA_POWERGATE_XUSBA);
	tegra186_pg_force_powergate(TEGRA_POWERGATE_XUSBB);
	tegra186_pg_force_powergate(TEGRA_POWERGATE_XUSBC);
	tegra186_pg_force_powergate(TEGRA_POWERGATE_SATA);
	tegra186_pg_force_powergate(TEGRA_POWERGATE_PCIE);

	/*
	 * WAR: tegra_ape_power_on() avoid calling unpowergate on the AUD
	 * partition the first time it is called as it expects it to already be
	 * on during boot (and the  reason for that is that AGIC need to be
	 * powered on early in boot).
	 * Thus there would be a mismatch in the refcount the first
	 * time tegra_ape_power_off() is called, so fix it up here.
	 * (and this can't easily be fixed in tegra_ape_power_on(), since
	 * that will break t210).
	 *
	 * This WAR can be removed when GIC has proper runtime pm support.
	 */
	t186_partition_info[TEGRA_POWERGATE_APE].refcount = 1;

	return 0;
}

static bool tegra186_powergate_id_is_valid(int id)
{
	if ((id < 0) || (id >= TEGRA_NUM_POWERGATE))
		return false;

	return t186_powergate_info[id].valid;
}

static struct tegra_powergate_driver_ops tegra186_pg_ops = {
	.soc_name = "tegra186",

	.num_powerdomains = TEGRA_NUM_POWERGATE,
	.powergate_id_is_soc_valid = tegra186_powergate_id_is_valid,

	.get_powergate_domain_name = tegra186_pg_get_name,

	.powergate_partition = tegra186_pg_powergate_partition,
	.unpowergate_partition = tegra186_pg_unpowergate_partition,

	.powergate_partition_with_clk_off = tegra186_pg_powergate_clk_off,
	.unpowergate_partition_with_clk_on = tegra186_pg_unpowergate_clk_on,

	.powergate_is_powered = tegra186_pg_is_powered,
	.powergate_init_refcount = tegra186_init_refcount,
};

struct tegra_powergate_driver_ops *tegra186_powergate_init_chip_support(void)
{
	if (tegra186_pg_query_abi()) {
		WARN(1, "Missing BPMP support for MRQ_PG\n");
		return 0;
	}

	return &tegra186_pg_ops;
}
