/*
 * Tegra Graphics Virtualization Host functions for HOST1X
 *
 * Copyright (c) 2014-2015, NVIDIA Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/slab.h>
#include <linux/uaccess.h>

#include "vhost.h"
#include "../host1x/host1x.h"

static inline int vhost_comm_init(struct platform_device *pdev)
{
	size_t queue_sizes[] = { TEGRA_VHOST_QUEUE_SIZES };

	return tegra_gr_comm_init(pdev, TEGRA_GR_COMM_CTX_CLIENT, 3,
				queue_sizes, TEGRA_VHOST_QUEUE_CMD,
				ARRAY_SIZE(queue_sizes));
}

static inline void vhost_comm_deinit(void)
{
	size_t queue_sizes[] = { TEGRA_VHOST_QUEUE_SIZES };

	tegra_gr_comm_deinit(TEGRA_GR_COMM_CTX_CLIENT, TEGRA_VHOST_QUEUE_CMD,
			ARRAY_SIZE(queue_sizes));
}

int vhost_virt_moduleid(int moduleid)
{
	switch (moduleid) {
	case NVHOST_MODULE_NONE:
		return TEGRA_VHOST_MODULE_HOST;
	case NVHOST_MODULE_ISP:
		return TEGRA_VHOST_MODULE_ISP;
	case (1 << 16) | NVHOST_MODULE_ISP:
		return (1 << 16) | TEGRA_VHOST_MODULE_ISP;
	case NVHOST_MODULE_VI:
		return TEGRA_VHOST_MODULE_VI;
	case (1 << 16) | NVHOST_MODULE_VI:
		return (1 << 16) | TEGRA_VHOST_MODULE_VI;
	case NVHOST_MODULE_MSENC:
		return TEGRA_VHOST_MODULE_MSENC;
	case NVHOST_MODULE_VIC:
		return TEGRA_VHOST_MODULE_VIC;
	default:
		pr_err("module %d not virtualized\n", moduleid);
		return -1;
	}
}

static u64 vhost_virt_connect(int moduleid)
{
	struct tegra_vhost_cmd_msg msg;
	struct tegra_vhost_connect_params *p = &msg.params.connect;
	int err;

	msg.cmd = TEGRA_VHOST_CMD_CONNECT;
	p->module = vhost_virt_moduleid(moduleid);
	if (p->module == -1)
		return 0;

	err = vhost_sendrecv(&msg);

	return (err || msg.ret) ? 0 : p->handle;
}

int vhost_sendrecv(struct tegra_vhost_cmd_msg *msg)
{
	void *handle;
	size_t size = sizeof(*msg);
	size_t size_out = size;
	void *data = msg;
	int err;

	err = tegra_gr_comm_sendrecv(TEGRA_GR_COMM_CTX_CLIENT,
				tegra_gr_comm_get_server_vmid(),
				TEGRA_VHOST_QUEUE_CMD, &handle, &data, &size);
	if (!err) {
		WARN_ON(size < size_out);
		memcpy(msg, data, size_out);
		tegra_gr_comm_release(handle);
	}

	return err;
}

int nvhost_virt_init(struct platform_device *dev, int moduleid)
{
	struct nvhost_virt_ctx *virt_ctx =
				kzalloc(sizeof(*virt_ctx), GFP_KERNEL);
	int err;

	if (!virt_ctx)
		return -ENOMEM;

	/* If host1x, init comm */
	if (moduleid == NVHOST_MODULE_NONE) {
		err = vhost_comm_init(dev);
		if (err) {
			dev_err(&dev->dev, "failed to init comm interface\n");
			goto fail;
		}
	}

	virt_ctx->handle = vhost_virt_connect(moduleid);
	if (!virt_ctx->handle) {
		dev_err(&dev->dev,
			"failed to connect to server node\n");
		if (moduleid == NVHOST_MODULE_NONE)
			vhost_comm_deinit();
		err = -ENOMEM;
		goto fail;
	}

	nvhost_set_virt_data(dev, virt_ctx);
	return 0;

fail:
	kfree(virt_ctx);
	return err;
}

void nvhost_virt_deinit(struct platform_device *dev)
{
	struct nvhost_virt_ctx *virt_ctx = nvhost_get_virt_data(dev);

	if (virt_ctx) {
		/* FIXME: add virt disconnect */
		vhost_comm_deinit();
		kfree(virt_ctx);
	}
}

static int vhost_host1x_regrdwr(u64 handle, u32 moduleid, u32 num_offsets,
			u32 block_size, u32 *offs, u32 *vals, u32 write)
{
	struct tegra_vhost_cmd_msg msg;
	struct tegra_vhost_channel_regrdwr_params *p =
			&msg.params.regrdwr;
	int err;
	u32 num_per_block = block_size >> 2;
	u32 remaining = num_offsets * num_per_block;
	u32 i, n = 0;
	u32 *ptr;

	msg.cmd = TEGRA_VHOST_CMD_HOST1X_REGRDWR;
	msg.handle = handle;
	p->moduleid = moduleid;
	p->write = write;

	/* For writes, fill the back end of the msg buffer with offset/value
	 * pairs. For reads, it's all offsets, which will be replaced by
	 * the returned register values.
	 */
	if (write) {
		while (remaining > 0) {
			p->count = min(remaining, REGRDWR_ARRAY_SIZE >> 1);
			remaining -= p->count;

			ptr = p->regs;
			for (i = 0; i < p->count; i++) {
				*ptr++ = *offs + n * 4;
				*ptr++ = *vals++;
				if (++n == num_per_block) {
					offs++;
					n = 0;
				}
			}
			err = vhost_sendrecv(&msg);
			if (err || msg.ret)
				return -1;
		}
	} else {
		while (remaining > 0) {
			p->count = min(remaining, REGRDWR_ARRAY_SIZE);
			remaining -= p->count;

			ptr = p->regs;
			for (i = 0; i < p->count; i++) {
				*ptr++ = *offs + n * 4;
				if (++n == num_per_block) {
					offs++;
					n = 0;
				}
			}
			err = vhost_sendrecv(&msg);
			if (err || msg.ret)
				return -1;
			memcpy(vals, p->regs, p->count * sizeof(u32));
			vals += p->count;
		}
	}

	return 0;
}

int vhost_rdwr_module_regs(struct platform_device *ndev, u32 num_offsets,
			u32 block_size, u32 __user *offsets,
			u32 __user *values, u32 write)
{
	struct nvhost_device_data *pdata = platform_get_drvdata(ndev);
	struct nvhost_master *nvhost_master = nvhost_get_host(ndev);
	struct nvhost_virt_ctx *ctx = nvhost_get_virt_data(nvhost_master->dev);
	u32 *vals, *offs;
	int err;

	vals = kmalloc(num_offsets * block_size, GFP_KERNEL);
	if (!vals)
		return -ENOMEM;

	offs = kmalloc(num_offsets * sizeof(u32), GFP_KERNEL);
	if (!offs) {
		kfree(vals);
		return -ENOMEM;
	}

	if (copy_from_user((char *)offs, (char *)offsets,
			num_offsets * sizeof(u32))) {
		err = -EFAULT;
		goto done;
	}

	if (write) {
		if (copy_from_user((char *)vals, (char *)values,
				num_offsets * block_size)) {
			err = -EFAULT;
			goto done;
		}
	}
	err = vhost_host1x_regrdwr(ctx->handle,
				vhost_virt_moduleid(pdata->moduleid),
				num_offsets, block_size, offs, vals, write);

	if (err) {
		err = -EFAULT;
		goto done;
	}

	if (!write) {
		if (copy_to_user((char *)values, (char *)vals,
				num_offsets * block_size))
			err = -EFAULT;
	}

done:
	kfree(vals);
	kfree(offs);
	return err;
}
