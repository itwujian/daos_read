/**
 * (C) Copyright 2018-2022 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */

#define D_LOGFAC	DD_FAC(vos)

#include <daos/common.h>
#include <daos/dtx.h>
#include "vea_internal.h"

int
compound_vec_alloc(struct vea_space_info *vsi, struct vea_ext_vector *vec)
{
	/* TODO Add in in-memory extent vector tree */
	return 0;
}

static int
compound_alloc(struct vea_space_info *vsi, struct vea_free_extent *vfe, struct vea_entry *entry)
{
	struct vea_free_extent	*remain;
	d_iov_t			 key;
	int			 rc;

	remain = &entry->ve_ext;
	D_ASSERT(remain->vfe_blk_cnt >= vfe->vfe_blk_cnt);
	D_ASSERT(remain->vfe_blk_off == vfe->vfe_blk_off);

	/* Remove the found free extent from compound index */
	free_class_remove(vsi, entry);

	if (remain->vfe_blk_cnt == vfe->vfe_blk_cnt) {
		d_iov_set(&key, &vfe->vfe_blk_off, sizeof(vfe->vfe_blk_off));
		rc = dbtree_delete(vsi->vsi_free_btr, BTR_PROBE_EQ, &key, NULL);
	} else {
		/* Adjust in-tree offset & length */
		remain->vfe_blk_off += vfe->vfe_blk_cnt;
		remain->vfe_blk_cnt -= vfe->vfe_blk_cnt;

		rc = free_class_add(vsi, entry);
	}

	return rc;
}

int
reserve_hint(struct vea_space_info *vsi, uint32_t blk_cnt,
	     struct vea_resrvd_ext *resrvd)
{
	struct vea_free_extent vfe;
	struct vea_entry *entry;
	d_iov_t key, val;
	int rc;

	/* No hint offset provided */
	if (resrvd->vre_hint_off == VEA_HINT_OFF_INVAL)
		return 0;

	vfe.vfe_blk_off = resrvd->vre_hint_off;
	vfe.vfe_blk_cnt = blk_cnt; // 本次要申请的block的数量

	/* Fetch & operate on the in-tree record */
	d_iov_set(&key, &vfe.vfe_blk_off, sizeof(vfe.vfe_blk_off));
	d_iov_set(&val, NULL, 0);

	D_ASSERT(daos_handle_is_valid(vsi->vsi_free_btr));
	rc = dbtree_fetch(vsi->vsi_free_btr, BTR_PROBE_EQ, DAOS_INTENT_DEFAULT, &key, NULL, &val);
	// 不存在返回0
	if (rc)
		return (rc == -DER_NONEXIST) ? 0 : rc;

// 只有树上找到了，才往下走
// 在load_space_info的时候会compound_free里面会插入
	entry = (struct vea_entry *)val.iov_buf;
	/* The matching free extent isn't big enough */
	if (entry->ve_ext.vfe_blk_cnt < vfe.vfe_blk_cnt)
		return 0;

	rc = compound_alloc(vsi, &vfe, entry);
	if (rc)
		return rc;

	resrvd->vre_blk_off = vfe.vfe_blk_off;
	resrvd->vre_blk_cnt = vfe.vfe_blk_cnt;

	inc_stats(vsi, STAT_RESRV_HINT, 1);

	D_DEBUG(DB_IO, "["DF_U64", %u]\n", resrvd->vre_blk_off, resrvd->vre_blk_cnt);

	return 0;
}

static int
reserve_small(struct vea_space_info *vsi, uint32_t blk_cnt,
	      struct vea_resrvd_ext *resrvd)
{
	daos_handle_t		 btr_hdl;
	struct vea_sized_class	*sc;
	struct vea_free_extent	 vfe;
	struct vea_entry	*entry;
	d_iov_t			 key, val_out;
	uint64_t		 int_key = blk_cnt;
	int			 rc;

	/* Skip huge allocate request */
	if (blk_cnt > vsi->vsi_class.vfc_large_thresh)
		return 0;

	btr_hdl = vsi->vsi_class.vfc_size_btr;
	D_ASSERT(daos_handle_is_valid(btr_hdl));

	d_iov_set(&key, &int_key, sizeof(int_key));
	d_iov_set(&val_out, NULL, 0);

	rc = dbtree_fetch(btr_hdl, BTR_PROBE_GE, DAOS_INTENT_DEFAULT, &key, NULL, &val_out);
	if (rc == -DER_NONEXIST) {
		return 0;
	} else if (rc) {
		D_ERROR("Search size class:%u failed. "DF_RC"\n", blk_cnt, DP_RC(rc));
		return rc;
	}

	sc = (struct vea_sized_class *)val_out.iov_buf;
	D_ASSERT(sc != NULL);
	D_ASSERT(!d_list_empty(&sc->vsc_lru));

	/* Get the least used item from head */
	entry = d_list_entry(sc->vsc_lru.next, struct vea_entry, ve_link);
	D_ASSERT(entry->ve_sized_class == sc);
	D_ASSERT(entry->ve_ext.vfe_blk_cnt >= blk_cnt);

	vfe.vfe_blk_off = entry->ve_ext.vfe_blk_off;
	vfe.vfe_blk_cnt = blk_cnt;

	rc = compound_alloc(vsi, &vfe, entry);
	if (rc)
		return rc;

	resrvd->vre_blk_off = vfe.vfe_blk_off;
	resrvd->vre_blk_cnt = blk_cnt;
	inc_stats(vsi, STAT_RESRV_SMALL, 1);

	D_DEBUG(DB_IO, "["DF_U64", %u]\n", resrvd->vre_blk_off, resrvd->vre_blk_cnt);

	return rc;
}

int
reserve_single(struct vea_space_info *vsi, uint32_t blk_cnt,
	       struct vea_resrvd_ext *resrvd)
{
	struct vea_free_class *vfc = &vsi->vsi_class;
	struct vea_free_extent vfe;
	struct vea_entry *entry;
	struct d_binheap_node *root;
	int rc;

	/* No large free extent available */
	if (d_binheap_is_empty(&vfc->vfc_heap))
		return reserve_small(vsi, blk_cnt, resrvd);

	root = d_binheap_root(&vfc->vfc_heap);
	entry = container_of(root, struct vea_entry, ve_node);

	D_ASSERT(entry->ve_ext.vfe_blk_cnt > vfc->vfc_large_thresh);
	D_DEBUG(DB_IO, "largest free extent ["DF_U64", %u]\n",
	       entry->ve_ext.vfe_blk_off, entry->ve_ext.vfe_blk_cnt);

	/* The largest free extent can't satisfy huge allocate request */
	if (entry->ve_ext.vfe_blk_cnt < blk_cnt)
		return 0;

	/*
	 * If the largest free extent is large enough for splitting, divide it in
	 * half-and-half then reserve from the second half, otherwise, try to
	 * reserve from the small extents first, if it fails, reserve from the
	 * largest free extent.
	 */
	if (entry->ve_ext.vfe_blk_cnt <= (max(blk_cnt, vfc->vfc_large_thresh) * 2)) {
		/* Try small extents first */
		rc = reserve_small(vsi, blk_cnt, resrvd);
		if (rc != 0 || resrvd->vre_blk_cnt != 0)
			return rc;

		vfe.vfe_blk_off = entry->ve_ext.vfe_blk_off;
		vfe.vfe_blk_cnt = blk_cnt;

		rc = compound_alloc(vsi, &vfe, entry);
		if (rc)
			return rc;

	} else {
		uint32_t half_blks, tot_blks;
		uint64_t blk_off;

		blk_off = entry->ve_ext.vfe_blk_off;
		tot_blks = entry->ve_ext.vfe_blk_cnt;
		half_blks = tot_blks >> 1;
		D_ASSERT(tot_blks >= (half_blks + blk_cnt));

		/* Shrink the original extent to half size */
		free_class_remove(vsi, entry);
		entry->ve_ext.vfe_blk_cnt = half_blks;
		rc = free_class_add(vsi, entry);
		if (rc)
			return rc;

		/* Add the remaining part of second half */
		if (tot_blks > (half_blks + blk_cnt)) {
			vfe.vfe_blk_off = blk_off + half_blks + blk_cnt;
			vfe.vfe_blk_cnt = tot_blks - half_blks - blk_cnt;
			vfe.vfe_age = 0;	/* Not used */

			rc = compound_free(vsi, &vfe, VEA_FL_NO_MERGE |
						VEA_FL_NO_ACCOUNTING);
			if (rc)
				return rc;
		}
		vfe.vfe_blk_off = blk_off + half_blks;
	}

	resrvd->vre_blk_off = vfe.vfe_blk_off;
	resrvd->vre_blk_cnt = blk_cnt;

	inc_stats(vsi, STAT_RESRV_LARGE, 1);

	D_DEBUG(DB_IO, "["DF_U64", %u]\n", resrvd->vre_blk_off,
		resrvd->vre_blk_cnt);

	return 0;
}

int
reserve_vector(struct vea_space_info *vsi, uint32_t blk_cnt,
	       struct vea_resrvd_ext *resrvd)
{
	/* TODO reserve extent vector for non-contiguous allocation */
		struct vea_resrvd_ext *resrvd_ext, *ext_tmp;
		d_list_t result_list;
		int rc = 0;
	
		D_INIT_LIST_HEAD(&result_list);
		D_ALLOC_PTR(resrvd->vre_vector);
		resrvd->vre_vector->vev_size = 0;
	
		if (!vsi->vsi_alignment_blk) {
			vsi->vsi_alignment_blk = 1;
		}
	
		while (blk_cnt > 0) {
			uint32_t this_try = 0;
			struct vea_resrvd_ext *ext;
			struct d_binheap_node *root = d_binheap_root(&vsi->vsi_class.vfc_heap);
			if (root == NULL) { // no more space to alloc from heap
				daos_handle_t ih;
				d_iov_t key, val;
				rc = dbtree_iter_prepare(vsi->vsi_free_btr, 0, &ih);
				if (rc != 0) {
					D_ERROR("Failed to create free_btr iterator:"DF_RC"\n", DP_RC(rc));
					vea_cancel(vsi, NULL, &result_list); 
					goto out;
				}
				rc = dbtree_iter_probe(ih, BTR_PROBE_FIRST, DAOS_INTENT_DEFAULT, NULL, NULL);
				if (rc != 0) {
					D_ERROR("Failed to probe free_btr iterator:"DF_RC"\n", DP_RC(rc));
					vea_cancel(vsi, NULL, &result_list);
					goto out;
				}	
				d_iov_set(&key, NULL, 0);
				d_iov_set(&val, NULL, 0);
				rc = dbtree_iter_fetch(ih, &key, &val, NULL);
				if (rc != 0) {
					D_ERROR("Failed to fetch free_btr iterator:"DF_RC"\n", DP_RC(rc));
					vea_cancel(vsi, NULL, &result_list);
					goto out;
				}
				struct vea_entry *entry = (struct vea_entry *)val.iov_buf;
				uint32_t usable_blk = entry->ve_ext.vfe_blk_cnt & (~(vsi->vsi_alignment_blk - 1));
				this_try = min(usable_blk, blk_cnt);
				dbtree_iter_finish(ih);
			} else {
				struct vea_entry *entry = container_of(root, struct vea_entry, ve_node);
				uint32_t usable_blk = 0;
				usable_blk = entry->ve_ext.vfe_blk_cnt & (~(vsi->vsi_alignment_blk - 1));
				this_try = min(usable_blk, blk_cnt);
			}
			D_ASSERTF(this_try <= blk_cnt, "%d/%d", this_try, blk_cnt);
			if (this_try == 0) {
				rc = -DER_NOSPACE;
				D_ERROR("Reserve vector failed: "DF_RC"\n", DP_RC(rc));
				vea_cancel(vsi, NULL, &result_list); 
				goto out;
			}
			D_ALLOC_PTR(ext);
			D_INIT_LIST_HEAD(&ext->vre_link);
	
			rc = reserve_single(vsi, this_try, ext);
			if(this_try == 1 && rc != 0) { //fail to alloc even 1 block
				D_ERROR("Reserve single failed: "DF_RC"\n", DP_RC(rc));
				vea_cancel(vsi, NULL, &result_list); 
				D_FREE(ext);
				goto out;
			}
	
			d_list_add_tail(&result_list, &ext->vre_link);
			resrvd->vre_vector->vev_blk_off[resrvd->vre_vector->vev_size] = ext->vre_blk_off;
			resrvd->vre_vector->vev_blk_cnt[resrvd->vre_vector->vev_size] = ext->vre_blk_cnt;
			resrvd->vre_vector->vev_size ++;
			D_DEBUG(DB_IO, "vec size %u, blk off %lu, blk cnt %u", resrvd->vre_vector->vev_size,
							resrvd->vre_vector->vev_blk_off[resrvd->vre_vector->vev_size - 1],
							resrvd->vre_vector->vev_blk_cnt[resrvd->vre_vector->vev_size - 1]);
			blk_cnt -= this_try;
		}
	
	out:
		d_list_for_each_entry_safe(resrvd_ext, ext_tmp, &result_list, vre_link) {
			d_list_del_init(&resrvd_ext->vre_link);
			D_FREE(resrvd_ext);
		}
	
		if (rc) {
			D_FREE(resrvd->vre_vector);
		}
		return rc;

}

// 在vsi->vsi_md_free_btr树上找到合适此次申请空间大小的区间块
// 同时修改树上的空闲空间块的区间[off, off + blk_cnt]
int
persistent_alloc(struct vea_space_info *vsi, struct vea_free_extent *vfe)
{
	struct vea_free_extent *found, frag = {0};
	daos_handle_t           btr_hdl;
	d_iov_t   key_in, key_out, val;
	uint64_t *blk_off, found_end, vfe_end;
	int rc;

	D_ASSERT(pmemobj_tx_stage() == TX_STAGE_WORK || vsi->vsi_umem->umm_id == UMEM_CLASS_VMEM);
	D_ASSERT(vfe->vfe_blk_off != VEA_HINT_OFF_INVAL);
	D_ASSERT(vfe->vfe_blk_cnt > 0);

	btr_hdl = vsi->vsi_md_free_btr;
	D_ASSERT(daos_handle_is_valid(btr_hdl));

	D_DEBUG(DB_IO, "Persistent alloc ["DF_U64", %u]\n", vfe->vfe_blk_off, vfe->vfe_blk_cnt);

	/* Fetch & operate on the in-tree record */
	d_iov_set(&key_in, &vfe->vfe_blk_off, sizeof(vfe->vfe_blk_off));
	d_iov_set(&key_out, NULL, sizeof(*blk_off));
	d_iov_set(&val, NULL, sizeof(*found));


	rc = dbtree_fetch(btr_hdl, BTR_PROBE_LE, DAOS_INTENT_DEFAULT, &key_in, &key_out, &val);
	if (rc) {
		D_ERROR("failed to find extent ["DF_U64", %u]\n", vfe->vfe_blk_off, vfe->vfe_blk_cnt);
		return rc;
	}

	found = (struct vea_free_extent *)val.iov_buf;
	blk_off = (uint64_t *)key_out.iov_buf;

	rc = verify_free_entry(blk_off, found);
	if (rc)
		return rc;

	found_end = found->vfe_blk_off + found->vfe_blk_cnt;
	vfe_end   = vfe->vfe_blk_off   + vfe->vfe_blk_cnt;

    // 找到的空闲区间块不满足申请的【off, off + blk_cnt】要求
    // 找到的最小的空闲off起始地址都要比申请的地址要大，
	if (found->vfe_blk_off > vfe->vfe_blk_off || found_end < vfe_end) {
		D_ERROR("mismatched extent ["DF_U64", %u] ["DF_U64", %u]\n", found->vfe_blk_off, found->vfe_blk_cnt, vfe->vfe_blk_off, vfe->vfe_blk_cnt);
		return -DER_INVAL;
	}

// 找到了满足要求的：found->vfe_blk_off          <=  vfe->vfe_blk_off && found_end >= vfe_end

	if (found->vfe_blk_off < vfe->vfe_blk_off) {

        // exp1: found_end > vfe_end
        // found=[1000,50], vfe=[1004,2]

        // exp2: found_end == vfe_end
        // found=[1000,50], vfe=[1004,46]

	
		/* Adjust the in-tree free extent length */
		rc = umem_tx_add_ptr(vsi->vsi_umem, &found->vfe_blk_cnt, sizeof(found->vfe_blk_cnt));
		if (rc)
			return rc;

        // exp1:
        // 变成：found=[1000,4], vfe=[1004,2]
        // exp2:
        // 变成：found=[1000,4], vfe=[1004,46]
		found->vfe_blk_cnt = vfe->vfe_blk_off - found->vfe_blk_off;

		/* Add back the rear part of free extent */
		if (found_end > vfe_end) {
			frag.vfe_blk_off = vfe->vfe_blk_off + vfe->vfe_blk_cnt;
			frag.vfe_blk_cnt = found_end - vfe_end;  // 1050-1006=1044

			// exp1:
			// 变成：found=[1000,4], vfe=[1004,2]
			// frag=[1006,1044]

			d_iov_set(&key_in, &frag.vfe_blk_off, sizeof(frag.vfe_blk_off));
			d_iov_set(&val, &frag, sizeof(frag));
			// exp1:
			// frag=[1006,1044]插入树上 
			// 原有的空闲的found=[1000,4]，[1004,2]被分走使用，新插入空闲的[1006,1044]
			rc = dbtree_update(btr_hdl, &key_in, &val);
			if (rc)
				return rc;
		}

		// exp2: found_end == vfe_end
        // 原有的空闲的found=[1000,4]，[1004,46]被分走使用, 无新插入空闲空间
	} 

	else if (found_end > vfe_end) {
		/* Adjust the in-tree extent offset & length */
		rc = umem_tx_add_ptr(vsi->vsi_umem, found, sizeof(*found));
		if (rc)
			return rc;

		// exp3: found->vfe_blk_off = vfe->vfe_blk_off
        // found=[1000,50], vfe=[1000,2]

		found->vfe_blk_off = vfe->vfe_blk_off + vfe->vfe_blk_cnt;
		found->vfe_blk_cnt = found_end - vfe_end;

		// exp3: found_end == vfe_end
        // 原有的空闲的found=[1002, 1048]，[1000, 2]被分走使用, 无新插入空闲空间
	} 

    // 空闲的区间块正好满足申请的需求
	else {
		/* Remove the original free extent from persistent tree */
		rc = dbtree_delete(btr_hdl, BTR_PROBE_BYPASS, &key_out, NULL);
		if (rc)
			return rc;
	}

	return 0;
}
