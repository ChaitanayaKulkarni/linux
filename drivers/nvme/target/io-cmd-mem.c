// SPDX-License-Identifier: GPL-2.0
/*
 * NVMe over Fabrics target in-memory I/O command implementation.
 * Copyright (c) 2024 NVIDIA Corporation.
 * Author: Chaitanya Kulkarni <kch@nvidia.com>
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/module.h>
#include <linux/xarray.h>
#include <linux/highmem.h>
#include <linux/overflow.h>
#include "nvmet.h"

/* Convert sector to xarray page index */
#define SECTOR_TO_PAGE_IDX(sector) \
	((sector) >> (PAGE_SHIFT - SECTOR_SHIFT))

/* Calculate byte offset within a page for a given sector */
#define SECTOR_TO_OFFSET_IN_PAGE(sector) \
	(((sector) << SECTOR_SHIFT) & ~PAGE_MASK)

/*
 * Validate LBA range against namespace size.
 * Returns 0 if valid, NVMe error status if out of bounds.
 */
static u16 nvmet_mem_check_range(struct nvmet_ns *ns, u64 slba, u32 nlb)
{
	u64 end_lba;

	/* Check for overflow in end_lba calculation */
	if (unlikely(check_add_overflow(slba, nlb, &end_lba)))
		return NVME_SC_LBA_RANGE | NVME_STATUS_DNR;

	/* Convert namespace size (bytes) to LBAs */
	if (unlikely(end_lba > (ns->size >> ns->blksize_shift)))
		return NVME_SC_LBA_RANGE | NVME_STATUS_DNR;

	return 0;
}

/*
 * Look up a page with refcount grabbed.
 * Returns page with reference, or NULL if not found.
 */
static struct page *nvmet_mem_lookup_page(struct nvmet_ns *ns, pgoff_t idx)
{
	struct page *page;

	rcu_read_lock();
repeat:
	page = xa_load(&ns->mem_pages, idx);
	if (!page)
		goto out;

	if (!get_page_unless_zero(page))
		goto repeat;

	/*
	 * Verify page is still in tree after getting refcount.
	 * If not, it's being removed - drop ref and retry.
	 */
	if (unlikely(page != xa_load(&ns->mem_pages, idx))) {
		put_page(page);
		goto repeat;
	}
out:
	rcu_read_unlock();
	return page;
}

/*
 * Allocate and insert a page into the namespace xarray.
 * Returns the page with reference grabbed on success, ERR_PTR on failure.
 * Caller must call put_page() when done.
 */
static struct page *nvmet_mem_insert_page(struct nvmet_ns *ns, sector_t sect)
{
	pgoff_t idx = SECTOR_TO_PAGE_IDX(sect);
	struct page *page, *ret;

	/* Fast path: check if page already exists */
	page = nvmet_mem_lookup_page(ns, idx);
	if (page)
		return page;

	/* Allocate new page outside of lock */
	page = alloc_page(GFP_NOWAIT | __GFP_ZERO | __GFP_NOWARN);
	if (!page)
		return ERR_PTR(-ENOMEM);

	/* Try to insert - handle race with xa_cmpxchg */
	xa_lock(&ns->mem_pages);
	ret = __xa_cmpxchg(&ns->mem_pages, idx, NULL, page, GFP_ATOMIC);

	if (!ret) {
		/* We successfully inserted the page */
		ns->mem_nr_pages++;
		get_page(page);  /* Reference for caller */
		xa_unlock(&ns->mem_pages);
		return page;
	}

	if (!xa_is_err(ret)) {
		/* Another thread won the race - use their page */
		get_page(ret);  /* Reference for caller */
		xa_unlock(&ns->mem_pages);
		put_page(page);  /* Free our allocated page */
		return ret;
	}

	/* Insertion failed due to xarray error */
	xa_unlock(&ns->mem_pages);
	put_page(page);
	return ERR_PTR(xa_err(ret));
}

static int nvmet_mem_read_chunk(struct nvmet_ns *ns, void *sgl_addr,
				sector_t sect, unsigned int copy_len)
{
	unsigned int offset = SECTOR_TO_OFFSET_IN_PAGE(sect);
	pgoff_t idx = SECTOR_TO_PAGE_IDX(sect);
	struct page *page;
	void *src;

	page = nvmet_mem_lookup_page(ns, idx);
	if (page) {
		src = kmap_local_page(page);
		memcpy(sgl_addr, src + offset, copy_len);
		kunmap_local(src);
		put_page(page);  /* Drop reference */
	} else {
		memset(sgl_addr, 0, copy_len);
	}

	return 0;
}

static int nvmet_mem_write_chunk(struct nvmet_ns *ns, void *sgl_addr,
				 sector_t sect, unsigned int copy_len)
{
	unsigned int offset = SECTOR_TO_OFFSET_IN_PAGE(sect);
	struct page *page;
	void *dst;

	page = nvmet_mem_insert_page(ns, sect);
	if (IS_ERR(page))
		return PTR_ERR(page);

	dst = kmap_local_page(page);
	memcpy(dst + offset, sgl_addr, copy_len);
	kunmap_local(dst);
	put_page(page);

	return 0;
}

static void nvmet_mem_execute_rw(struct nvmet_req *req)
{
	int (*process_chunk)(struct nvmet_ns *ns, void *sgl_addr,
			     sector_t sect, unsigned int copy_len);
	struct nvmet_ns *ns = req->ns;
	struct sg_mapping_iter miter;
	unsigned int sg_flags;
	sector_t sect;
	u16 status = NVME_SC_SUCCESS;
	int ret;

	if (!nvmet_check_transfer_len(req, nvmet_rw_data_len(req)))
		return;

	status = nvmet_mem_check_range(ns, le64_to_cpu(req->cmd->rw.slba),
				       le16_to_cpu(req->cmd->rw.length) + 1);
	if (status) {
		nvmet_req_complete(req, status);
		return;
	}

	sect = nvmet_lba_to_sect(ns, req->cmd->rw.slba);

	if (req->cmd->rw.opcode == nvme_cmd_write) {
		sg_flags = SG_MITER_FROM_SG;
		process_chunk = nvmet_mem_write_chunk;
	} else {
		sg_flags = SG_MITER_TO_SG;
		process_chunk = nvmet_mem_read_chunk;
	}

	sg_miter_start(&miter, req->sg, req->sg_cnt, sg_flags);

	while (sg_miter_next(&miter)) {
		unsigned int miter_offset = 0;
		unsigned int miter_len = miter.length;

		while (miter_len > 0) {
			unsigned int offset, copy_len;

			offset = SECTOR_TO_OFFSET_IN_PAGE(sect);
			copy_len = min_t(unsigned int, miter_len,
					 PAGE_SIZE - offset);

			ret = process_chunk(ns, miter.addr + miter_offset,
					    sect, copy_len);
			if (ret) {
				status = NVME_SC_INTERNAL | NVME_STATUS_DNR;
				goto out;
			}

			sect += copy_len >> SECTOR_SHIFT;
			miter_offset += copy_len;
			miter_len -= copy_len;
		}
	}

out:
	sg_miter_stop(&miter);
	nvmet_req_complete(req, status);
}

/*
 * Flush command - no-op for memory backend (no persistent storage).
 */
static void nvmet_mem_execute_flush(struct nvmet_req *req)
{
	if (!nvmet_check_transfer_len(req, 0))
		return;
	nvmet_req_complete(req, NVME_SC_SUCCESS);
}

/*
 * Discard/TRIM command - delete pages in the range.
 * With PAGE_SIZE LBAs, each LBA maps to exactly one page,
 * so we simply delete the corresponding pages from xarray.
 */
static void nvmet_mem_execute_discard(struct nvmet_req *req)
{
	struct nvmet_ns *ns = req->ns;
	struct nvme_dsm_range range;
	u16 status = NVME_SC_SUCCESS;
	int i;

	for (i = 0; i < le32_to_cpu(req->cmd->dsm.nr) + 1; i++) {
		sector_t sect, nr_sect;
		pgoff_t idx;
		struct page *page;

		status = nvmet_copy_from_sgl(req, i * sizeof(range), &range,
					     sizeof(range));
		if (status)
			break;

		status = nvmet_mem_check_range(ns, le64_to_cpu(range.slba),
					       le32_to_cpu(range.nlb));
		if (status)
			break;

		sect = nvmet_lba_to_sect(ns, range.slba);
		nr_sect = le32_to_cpu(range.nlb) <<
			     (ns->blksize_shift - SECTOR_SHIFT);

		/* Skip zero-length discard */
		if (nr_sect == 0)
			continue;

		/*
		 * With PAGE_SIZE LBAs, sectors align with pages.
		 * Delete all pages in range.
		 */
		xa_lock(&ns->mem_pages);
		for (idx = SECTOR_TO_PAGE_IDX(sect);
		     idx < SECTOR_TO_PAGE_IDX(sect + nr_sect);
		     idx++) {
			page = __xa_erase(&ns->mem_pages, idx);
			if (page) {
				put_page(page);
				ns->mem_nr_pages--;
			}
		}
		xa_unlock(&ns->mem_pages);
	}

	nvmet_req_complete(req, status);
}

/*
 * Write Zeroes command - allocate zeroed pages in the range.
 * With PAGE_SIZE LBAs, each LBA maps to exactly one page.
 * Allocate pages and zero them (allocation gives zeroed pages).
 */
static void nvmet_mem_execute_write_zeroes(struct nvmet_req *req)
{
	struct nvme_write_zeroes_cmd *wz = &req->cmd->write_zeroes;
	struct nvmet_ns *ns = req->ns;
	sector_t nr_sects, sect;
	pgoff_t idx, start_idx, end_idx;
	struct page *page;
	void *kaddr;
	u16 status = NVME_SC_SUCCESS;

	status = nvmet_mem_check_range(ns, le64_to_cpu(wz->slba),
				       le16_to_cpu(wz->length) + 1);
	if (status)
		goto out;

	sect = nvmet_lba_to_sect(ns, wz->slba);
	nr_sects = (le16_to_cpu(wz->length) + 1) <<
		   (ns->blksize_shift - SECTOR_SHIFT);

	start_idx = SECTOR_TO_PAGE_IDX(sect);
	end_idx = SECTOR_TO_PAGE_IDX(sect + nr_sects - 1);

	/*
	 * With PAGE_SIZE LBAs, sectors align with pages.
	 * Allocate and zero all pages in range.
	 */
	for (idx = start_idx; idx <= end_idx; idx++) {
		sector_t pg_sect = idx << (PAGE_SHIFT - SECTOR_SHIFT);

		page = nvmet_mem_insert_page(ns, pg_sect);
		if (IS_ERR(page)) {
			status = NVME_SC_INTERNAL | NVME_STATUS_DNR;
			goto out;
		}

		kaddr = kmap_local_page(page);
		memset(kaddr, 0, PAGE_SIZE);
		kunmap_local(kaddr);

		put_page(page);
	}

out:
	nvmet_req_complete(req, status);
}

/*
 * Setup namespace for memory backend.
 */
int nvmet_mem_ns_enable(struct nvmet_ns *ns)
{
	if (!ns->size) {
		pr_err("memory backend: namespace size not configured\n");
		return -EINVAL;
	}

	xa_init(&ns->mem_pages);
	ns->mem_nr_pages = 0;

	/* Set block size shift - memory backend uses page-sized LBAs */
	ns->blksize_shift = PAGE_SHIFT;  /* 2^12 = 4096 bytes */

	pr_info("memory backend: enabled namespace %u, size %lld, lba_size %u\n",
		ns->nsid, ns->size, 1 << ns->blksize_shift);

	return 0;
}

/*
 * Disable namespace and free all pages.
 */
void nvmet_mem_ns_disable(struct nvmet_ns *ns)
{
	unsigned long nr_freed = 0;
	struct page *pages[32];  /* Batch size */
	unsigned long idx;
	int count, i;

	/* Free all allocated pages using batch collection */
	do {
		count = 0;
		idx = 0;

		xa_lock(&ns->mem_pages);
		while (count < 32) {
			pages[count] = xa_find(&ns->mem_pages, &idx,
					       ULONG_MAX, XA_PRESENT);
			if (!pages[count])
				break;
			__xa_erase(&ns->mem_pages, idx);
			count++;
			idx++;
		}
		xa_unlock(&ns->mem_pages);

		for (i = 0; i < count; i++) {
			put_page(pages[i]);
			nr_freed++;
		}

		cond_resched();

	} while (count > 0);

	xa_destroy(&ns->mem_pages);
	ns->mem_nr_pages = 0;

	pr_info("memory backend: disabled namespace %u, freed %lu pages\n",
		ns->nsid, nr_freed);
}

void nvmet_mem_ns_revalidate(struct nvmet_ns *ns)
{
	/* Nothing to revalidate for memory backend */
}

u16 nvmet_mem_parse_io_cmd(struct nvmet_req *req)
{
	struct nvme_command *cmd = req->cmd;

	switch (cmd->common.opcode) {
	case nvme_cmd_read:
	case nvme_cmd_write:
		req->execute = nvmet_mem_execute_rw;
		return 0;
	case nvme_cmd_flush:
		req->execute = nvmet_mem_execute_flush;
		return 0;
	case nvme_cmd_dsm:
		req->execute = nvmet_mem_execute_discard;
		return 0;
	case nvme_cmd_write_zeroes:
		req->execute = nvmet_mem_execute_write_zeroes;
		return 0;
	default:
		return NVME_SC_INVALID_OPCODE | NVME_STATUS_DNR;
	}
}
