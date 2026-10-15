/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NVME_HOST_CDL_H
#define NVME_HOST_CDL_H

#include <linux/nvme.h>

/* Experimental FEMU identity; these are not standard NVMe allocations. */
#define NVME_CDL_FID		0xc0
#define NVME_CDL_CONFIG_LOG	0xc0
#define NVME_CDL_STATS_LOG	0xc1
#define NVME_CDL_POLICY		2
#define NVME_CDL_MAX_CLASSES	256
#define NVME_CDL_HEADER_SIZE	64

struct nvme_ctrl;

struct nvme_cdl_caps {
	u16 classes;
	u16 max_sqs;
	u16 max_cqs;
	u16 class_sqs;
	u16 per_class;
	u16 bound_classes;
};

struct nvme_cdl_desc {
	__le32 duration_us;
	u8 policy;
	u8 reserved[3];
};

struct nvme_cdl_snapshot {
	u64 generation;
	u8 store_id[16];
	u32 flags;
	struct nvme_cdl_desc desc[NVME_CDL_MAX_CLASSES];
	u8 saved[NVME_CDL_MAX_CLASSES];
};

int nvme_cdl_identify(struct nvme_ctrl *ctrl, const struct nvme_id_ctrl *id);
int nvme_cdl_snapshot(struct nvme_ctrl *ctrl, struct nvme_cdl_snapshot *state);
int nvme_cdl_discover(struct nvme_ctrl *ctrl);
int nvme_cdl_get_desc(struct nvme_ctrl *ctrl, unsigned int cls, unsigned int sel,
		      struct nvme_cdl_desc *desc);
int nvme_cdl_set_desc(struct nvme_ctrl *ctrl, unsigned int cls,
		      const struct nvme_cdl_desc *desc);
int nvme_cdl_enable(struct nvme_ctrl *ctrl, bool enable);
int nvme_cdl_get_enable(struct nvme_ctrl *ctrl, unsigned int sel, bool *enable);
int nvme_cdl_queue_class(struct nvme_ctrl *ctrl, u16 qid, u8 *cls);

#endif
