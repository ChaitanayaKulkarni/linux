// SPDX-License-Identifier: GPL-2.0
/* Exact discovery and readback for the experimental CDL2FEMU profile. */
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include "nvme.h"
#include "cdl.h"

static bool cdl_zero(const void *p, size_t size)
{
	return !memchr_inv(p, 0, size);
}

int nvme_cdl_identify(struct nvme_ctrl *ctrl, const struct nvme_id_ctrl *id)
{
	const u8 *p = id->vs;
	struct nvme_cdl_caps caps;
	struct pci_dev *pdev;

	BUILD_BUG_ON(offsetof(struct nvme_id_ctrl, vs) != 3072);
	BUILD_BUG_ON(sizeof(struct nvme_cdl_desc) != 8);
	ctrl->cdl_supported = false;
	ctrl->cdl_profile = false;
	if (ctrl->ops->flags & NVME_F_FABRICS || !dev_is_pci(ctrl->dev))
		return 0;
	pdev = to_pci_dev(ctrl->dev);
	if (pdev->vendor != 0x8086 || pdev->device != 0x5845 ||
	    le16_to_cpu(id->vid) != 0x8086 || memcmp(p, "CDL2FEMU", 8))
		return 0;
	if (get_unaligned_le16(p + 8) != 2 ||
	    get_unaligned_le16(p + 10) != 1 ||
	    get_unaligned_le16(p + 12) != 64 ||
	    p[16] != NVME_CDL_FID || p[17] != NVME_CDL_CONFIG_LOG ||
	    p[18] != NVME_CDL_STATS_LOG || p[19] != NVME_CDL_POLICY ||
	    get_unaligned_le32(p + 20) != 3 ||
	    memcmp(p + 24, "TP4221V2", 8) || !cdl_zero(p + 42, 22) ||
	    !(le32_to_cpu(id->ctratt) & BIT(28)) ||
	    !(le16_to_cpu(id->oncs) & BIT(4)) || !(id->lpa & BIT(2)))
		return -EPROTO;
	caps.classes = get_unaligned_le16(p + 14);
	caps.max_sqs = get_unaligned_le16(p + 32);
	caps.max_cqs = get_unaligned_le16(p + 34);
	caps.class_sqs = get_unaligned_le16(p + 36);
	caps.per_class = get_unaligned_le16(p + 38);
	caps.bound_classes = get_unaligned_le16(p + 40);
	if (!caps.classes || caps.classes > NVME_CDL_MAX_CLASSES ||
	    !caps.max_sqs || !caps.max_cqs)
		return -EPROTO;
	if (caps.classes == 1) {
		if (caps.class_sqs || caps.per_class || caps.bound_classes)
			return -EPROTO;
	} else if (!caps.bound_classes || caps.bound_classes >= caps.classes ||
		   !caps.per_class || caps.per_class > caps.class_sqs ||
		   caps.bound_classes > caps.class_sqs ||
		   caps.class_sqs > caps.max_sqs ||
		   caps.class_sqs > (u32)caps.bound_classes * caps.per_class) {
		return -EPROTO;
	}
	ctrl->cdl_caps = caps;
	ctrl->cdl_profile = true;
	return 0;
}

static int cdl_feature(struct nvme_ctrl *ctrl, bool set, unsigned int sel,
		       u32 dw11, u32 dw12, void *buf, size_t size, u32 *result)
{
	struct nvme_command cmd = { };
	union nvme_result res = { };
	int ret;

	if (!ctrl->cdl_profile || sel > 3 || (set && sel))
		return -EOPNOTSUPP;
	cmd.features.opcode = set ? nvme_admin_set_features : nvme_admin_get_features;
	cmd.features.fid = cpu_to_le32(NVME_CDL_FID | (sel << 8));
	cmd.features.dword11 = cpu_to_le32(dw11);
	cmd.features.dword12 = cpu_to_le32(dw12);
	if (set && (dw11 & 3) == 2)
		cmd.features.dword13 = cpu_to_le32(NVME_CDL_POLICY);
	ret = __nvme_submit_sync_cmd(ctrl->admin_q, &cmd, &res, buf, size,
				    NVME_QID_ANY, 0);
	if (!ret && result)
		*result = le32_to_cpu(res.u32);
	return ret;
}

int nvme_cdl_get_desc(struct nvme_ctrl *ctrl, unsigned int cls, unsigned int sel,
		      struct nvme_cdl_desc *desc)
{
	u32 result;
	int ret;

	if (!cls || cls >= ctrl->cdl_caps.classes || sel > 2)
		return -EINVAL;
	memset(desc, 0, sizeof(*desc));
	ret = cdl_feature(ctrl, false, sel, 2 | (cls << 8), 0,
			  desc, sizeof(*desc), &result);
	if (ret)
		return ret;
	if (result || !cdl_zero(desc->reserved, sizeof(desc->reserved)) ||
	    (desc->duration_us ? desc->policy != NVME_CDL_POLICY : desc->policy))
		return -EPROTO;
	return 0;
}
EXPORT_SYMBOL_GPL(nvme_cdl_get_desc);

int nvme_cdl_set_desc(struct nvme_ctrl *ctrl, unsigned int cls,
		      const struct nvme_cdl_desc *desc)
{
	u32 result;
	int ret;

	if (!cls || cls >= ctrl->cdl_caps.classes || !desc->duration_us ||
	    desc->policy != NVME_CDL_POLICY ||
	    !cdl_zero(desc->reserved, sizeof(desc->reserved)))
		return -EINVAL;
	ret = cdl_feature(ctrl, true, 0, 2 | (cls << 8),
			  le32_to_cpu(desc->duration_us), NULL, 0, &result);
	return ret ?: (result ? -EPROTO : 0);
}
EXPORT_SYMBOL_GPL(nvme_cdl_set_desc);

int nvme_cdl_enable(struct nvme_ctrl *ctrl, bool enable)
{
	u32 result;
	int ret = cdl_feature(ctrl, true, 0, enable << 2, 0, NULL, 0, &result);

	return ret ?: (result ? -EPROTO : 0);
}
EXPORT_SYMBOL_GPL(nvme_cdl_enable);

int nvme_cdl_get_enable(struct nvme_ctrl *ctrl, unsigned int sel, bool *enable)
{
	u32 result;
	int ret;

	if (sel > 2)
		return -EINVAL;
	ret = cdl_feature(ctrl, false, sel, 0, 0, NULL, 0, &result);
	if (ret)
		return ret;
	if (result & ~1U)
		return -EPROTO;
	*enable = result;
	return 0;
}
EXPORT_SYMBOL_GPL(nvme_cdl_get_enable);

int nvme_cdl_queue_class(struct nvme_ctrl *ctrl, u16 qid, u8 *cls)
{
	u32 result;
	int ret;

	if (!qid)
		return -EINVAL;
	ret = cdl_feature(ctrl, false, 0, 1 | ((u32)qid << 16), 0,
			  NULL, 0, &result);
	if (ret)
		return ret;
	if (result >= ctrl->cdl_caps.classes)
		return -EPROTO;
	*cls = result;
	return 0;
}
EXPORT_SYMBOL_GPL(nvme_cdl_queue_class);

static int cdl_log(struct nvme_ctrl *ctrl, u8 lid, u16 entry_size, u8 **image)
{
	size_t len = NVME_CDL_HEADER_SIZE + ctrl->cdl_caps.classes * entry_size;
	u8 *p;
	int ret;

	if (len > (u64)queue_max_hw_sectors(ctrl->admin_q) << SECTOR_SHIFT)
		return -EOPNOTSUPP;
	p = kzalloc(len, GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	ret = nvme_get_log(ctrl, 0, lid, 0, 0, p, len, 0);
	if (ret)
		goto fail;
	if (memcmp(p, "CDL2", 4) || get_unaligned_le16(p + 4) != 2 ||
	    get_unaligned_le16(p + 6) || get_unaligned_le16(p + 8) != 64 ||
	    get_unaligned_le16(p + 10) != entry_size ||
	    get_unaligned_le16(p + 12) != ctrl->cdl_caps.classes ||
	    !cdl_zero(p + 14, 2) || get_unaligned_le32(p + 16) != len ||
	    get_unaligned_le32(p + 20) & ~3U || cdl_zero(p + 32, 16) ||
	    !cdl_zero(p + 56, 8)) {
		ret = -EPROTO;
		goto fail;
	}
	*image = p;
	return 0;
fail:
	kfree(p);
	return ret;
}

int nvme_cdl_snapshot(struct nvme_ctrl *ctrl, struct nvme_cdl_snapshot *state)
{
	u8 *config, *stats;
	unsigned int c;
	int ret;

	if (!ctrl->cdl_profile)
		return -EOPNOTSUPP;
	ret = cdl_log(ctrl, NVME_CDL_CONFIG_LOG, 16, &config);
	if (ret)
		return ret;
	ret = cdl_log(ctrl, NVME_CDL_STATS_LOG, 24, &stats);
	if (ret)
		goto out_config;
	ret = -EPROTO;
	if (memcmp(config + 32, stats + 32, 16) ||
	    !cdl_zero(config + 64, 16) || !cdl_zero(stats + 64, 24))
		goto out;
	memset(state, 0, sizeof(*state));
	state->generation = get_unaligned_le64(config + 24);
	state->flags = get_unaligned_le32(config + 20);
	memcpy(state->store_id, config + 32, 16);
	for (c = 1; c < ctrl->cdl_caps.classes; c++) {
		const u8 *ce = config + 64 + c * 16;
		const u8 *se = stats + 64 + c * 24;
		u32 duration = get_unaligned_le32(ce + 4);
		u64 timed = get_unaligned_le64(se + 8);
		u64 miss = get_unaligned_le64(se + 16);

		if (ce[0] != c || ce[1] & ~3U || !cdl_zero(ce + 2, 2) ||
		    !cdl_zero(ce + 9, 7) || !!(ce[1] & 1) != !!duration ||
		    (duration ? ce[8] != NVME_CDL_POLICY : ce[8]) ||
		    se[0] != c || se[1] & ~3U || !cdl_zero(se + 2, 6) ||
		    miss > timed || !!(se[1] & 1) != (timed == U64_MAX) ||
		    !!(se[1] & 2) != (miss == U64_MAX))
			goto out;
		state->desc[c].duration_us = cpu_to_le32(duration);
		state->desc[c].policy = ce[8];
		state->saved[c] = !!(ce[1] & 2);
	}
	ret = 0;
out:
	kfree(stats);
out_config:
	kfree(config);
	return ret;
}
EXPORT_SYMBOL_GPL(nvme_cdl_snapshot);

int nvme_cdl_discover(struct nvme_ctrl *ctrl)
{
	struct nvme_cdl_snapshot *state;
	u32 result;
	int ret;

	ctrl->cdl_supported = false;
	if (!ctrl->cdl_profile)
		return 0;
	state = kzalloc_obj(*state);
	if (!state)
		return -ENOMEM;
	ret = nvme_cdl_snapshot(ctrl, state);
	if (!ret) {
		ret = cdl_feature(ctrl, false, 3, 0, 0, NULL, 0, &result);
		if (!ret && result != 5)
			ret = -EPROTO;
	}
	if (!ret)
		ctrl->cdl_supported = true;
	kfree(state);
	return ret;
}
EXPORT_SYMBOL_GPL(nvme_cdl_discover);
