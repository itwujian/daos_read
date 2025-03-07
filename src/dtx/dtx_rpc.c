/**
 * (C) Copyright 2019-2022 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */
/**
 * dtx: DTX RPC
 */
#define D_LOGFAC	DD_FAC(dtx)

#include <abt.h>
#include <daos/rpc.h>
#include <daos/btree.h>
#include <daos/pool_map.h>
#include <daos/btree_class.h>
#include <daos_srv/vos.h>
#include <daos_srv/dtx_srv.h>
#include <daos_srv/container.h>
#include <daos_srv/daos_engine.h>
#include "dtx_internal.h"

CRT_RPC_DEFINE(dtx, DAOS_ISEQ_DTX, DAOS_OSEQ_DTX);

#define X(a, b, c, d, e, f)	\
{				\
	.prf_flags   = b,	\
	.prf_req_fmt = c,	\
	.prf_hdlr    = NULL,	\
	.prf_co_ops  = NULL,	\
},

static struct crt_proto_rpc_format dtx_proto_rpc_fmt[] = {
	DTX_PROTO_SRV_RPC_LIST
};

#undef X

struct crt_proto_format dtx_proto_fmt = {
	.cpf_name  = "dtx-proto",
	.cpf_ver   = DAOS_DTX_VERSION,
	.cpf_count = ARRAY_SIZE(dtx_proto_rpc_fmt),
	.cpf_prf   = dtx_proto_rpc_fmt,
	.cpf_base  = DAOS_RPC_OPCODE(0, DAOS_DTX_MODULE, 0)
};

/* Top level DTX RPC args */
struct dtx_req_args {
	ABT_future			 dra_future;
	/* The RPC code */
	crt_opcode_t			 dra_opc;
	/* pool UUID */
	uuid_t				 dra_po_uuid;
	/* container UUID */
	uuid_t				 dra_co_uuid;
	/* The count of sub requests. */
	int				 dra_length;
	/* The collective RPC result. */
	int				 dra_result;
	/* Pointer to the container, used for DTX_REFRESH case. */
	struct ds_cont_child		*dra_cont;
	/* Pointer to the committed DTX list, used for DTX_REFRESH case. */
	d_list_t			*dra_cmt_list;
	/* Pointer to the aborted DTX list, used for DTX_REFRESH case. */
	d_list_t			*dra_abt_list;
	/* Pointer to the active DTX list, used for DTX_REFRESH case. */
	d_list_t			*dra_act_list;
	/* The committed DTX entries on all related participants, for DTX_COMMIT. */
	int				*dra_committed;
};

/* The record for the DTX classify-tree in DRAM.
 * Each dtx_req_rec contains one RPC (to related rank/tag) args.
 */
struct dtx_req_rec {
	/* All the records are linked into one global list,
	 * used for travelling the classify-tree efficiently.
	 */
	d_list_t			   drr_link;
	struct dtx_req_args	  *drr_parent; /* The top level args */
	d_rank_t			   drr_rank; /* The server ID */
	uint32_t			   drr_tag; /* The VOS ID */
	int				       drr_count; /* DTX count */
	int				       drr_result; /* The RPC result */
	uint32_t			   drr_comp:1;
	struct dtx_id		  *drr_dti; /* The DTX array */
	uint32_t			  *drr_flags;
	struct dtx_share_peer **drr_cb_args; /* Used by dtx_req_cb. */
};

struct dtx_cf_rec_bundle {
	union {
		struct {
			d_rank_t	 dcrb_rank;
			uint32_t	 dcrb_tag;
		};
		uint64_t		 dcrb_key;
	};
	/* Pointer to the global list head for all the dtx_req_rec. */
	d_list_t			*dcrb_head;
	/* Pointer to the length of above global list. */
	int				*dcrb_length;
	/* Current DTX to be classified. */
	struct dtx_id			*dcrb_dti;
	/* The number of DTXs to be classified that will be used as
	 * the dtx_req_rec::drr_dti array size when allocating it.
	 */
	int				 dcrb_count;
};

/* Make sure that the "dcrb_key" is consisted of "dcrb_rank" + "dcrb_tag". */
D_CASSERT(sizeof(((struct dtx_cf_rec_bundle *)0)->dcrb_rank) +
	  sizeof(((struct dtx_cf_rec_bundle *)0)->dcrb_tag) ==
	  sizeof(((struct dtx_cf_rec_bundle *)0)->dcrb_key));

uint32_t dtx_rpc_helper_thd;



// 单个的响应回来，只有DTX_REFRESH消息有特殊的处理
static void
dtx_req_cb(const struct crt_cb_info *cb_info)
{
	crt_rpc_t		*req = cb_info->cci_rpc;
	struct dtx_req_rec	*drr = cb_info->cci_arg;
	struct dtx_req_args	*dra = drr->drr_parent;
	struct dtx_in		*din = crt_req_get(req);
	struct dtx_out		*dout;
	int			 rc = cb_info->cci_rc;
	int			 i;

	D_ASSERT(drr->drr_comp == 0);

	if (rc != 0)
		goto out;

	dout = crt_reply_get(req);

	// dtx_commit消息的发送后回的响应，没有特殊处理的，do_misc
	if (dra->dra_opc == DTX_COMMIT) {
		*dra->dra_committed += dout->do_misc;
		D_GOTO(out, rc = dout->do_status);
	}

	if (dout->do_status != 0 || dra->dra_opc != DTX_REFRESH)
		// 非DTX_REFRESH的消息这里直接就出去了，不在走下面
		D_GOTO(out, rc = dout->do_status);

// 走到这里要求：dout->do_status == 0 && dra->dra_opc == DTX_REFRESH

	if (din->di_dtx_array.ca_count != dout->do_sub_rets.ca_count)
		D_GOTO(out, rc = -DER_PROTO);

//  只有DTX_REFRESH消息会走下面
//  要求所有的DTX_REFRESH消息都回来了，然后获取每1个的执行结果
	for (i = 0; i < dout->do_sub_rets.ca_count; i++) {
		struct dtx_share_peer	*dsp;
		int			*ret;
		int			 rc1;

		dsp = drr->drr_cb_args[i];
		if (dsp == NULL)
			continue;

		drr->drr_cb_args[i] = NULL;
		ret = (int *)dout->do_sub_rets.ca_arrays + i;

		switch (*ret) {
			
			case DTX_ST_PREPARED:
				// dtx_refresh消息从主节点查询回来，节点还没有committable则挂链
				/* Not committable yet. */
				if (dra->dra_act_list != NULL)
					d_list_add_tail(&dsp->dsp_link, dra->dra_act_list);
				else
					dtx_dsp_free(dsp);
				break;
				
			case DTX_ST_COMMITTABLE:
				/*
				 * Committable, will be committed soon.
				 * Fall through.
				 */
			case DTX_ST_COMMITTED:
				/* Has been committed on leader, we may miss related
				 * commit request, so let's commit it locally.
				 */

			    // 主告诉我其余节点都已经提交了，我也提交把，嘛法哎，主说了算
				rc1 = vos_dtx_commit(dra->dra_cont->sc_hdl, &dsp->dsp_xid, 1, NULL);
			
				if (rc1 < 0 && rc1 != -DER_NONEXIST && dra->dra_cmt_list != NULL)
					// 糟了，我提交失败了，那就入链表把：dra->dra_cmt_list
					d_list_add_tail(&dsp->dsp_link, dra->dra_cmt_list);
				else
					dtx_dsp_free(dsp);
				
				break;
				
			case DTX_ST_CORRUPTED:
				// 有些兄弟伙的副本信息丢失了，那就不能用了
				/* The DTX entry is corrupted. */
				dtx_dsp_free(dsp);
				D_GOTO(out, rc = -DER_DATA_LOSS);
			
			case -DER_TX_UNCERTAIN:
				// 主上的dtx信息不存在了
				/* Related DTX entry on leader does not exist. We do not know whether it has
				 * been aborted or committed (then removed by DTX aggregation). Then mark it
				 * as 'orphan' that will be handled via some special DAOS tools in future.
				 */
				rc1 = vos_dtx_set_flags(dra->dra_cont->sc_hdl, &dsp->dsp_xid, 1, DTE_ORPHAN);
				if (rc1 == -DER_NONEXIST || rc1 == -DER_NO_PERM) {
					dtx_dsp_free(dsp);
					break;
				}

				D_ERROR("Hit uncertain leaked DTX "DF_DTI", mark it as orphan: "DF_RC"\n", DP_DTI(&dsp->dsp_xid), DP_RC(rc1));
				dtx_dsp_free(dsp);
				D_GOTO(out, rc = -DER_TX_UNCERTAIN);
				
			case -DER_NONEXIST:
				/* The leader does not have related DTX info, we may miss related DTX abort
				 * request, let's abort it locally.
				 */
				rc1 = vos_dtx_abort(dra->dra_cont->sc_hdl, &dsp->dsp_xid, dsp->dsp_epoch);
				if (rc1 < 0 && rc1 != -DER_NONEXIST && rc1 != -DER_NO_PERM &&
				    dra->dra_abt_list != NULL)
					d_list_add_tail(&dsp->dsp_link, dra->dra_abt_list);
				else
					dtx_dsp_free(dsp);
				break;
				
			case -DER_INPROGRESS:
				rc1 = vos_dtx_check(dra->dra_cont->sc_hdl, &dsp->dsp_xid, NULL, NULL, NULL, NULL, false);
				dtx_dsp_free(dsp);
				if (rc1 != DTX_ST_COMMITTED && rc1 != DTX_ST_ABORTED && rc != -DER_NONEXIST)
					D_GOTO(out, rc = *ret);
				break;
				
			default:
				dtx_dsp_free(dsp);
				D_GOTO(out, rc = *ret);
		}
	}

out:
	drr->drr_comp = 1;
	drr->drr_result = rc;
	rc = ABT_future_set(dra->dra_future, drr);
	
	D_ASSERTF(rc == ABT_SUCCESS, "ABT_future_set failed for opc %x to %d/%d: rc = %d.\n", dra->dra_opc, drr->drr_rank, drr->drr_tag, rc);

	D_DEBUG(DB_TRACE,
		"DTX req for opc %x (req %p future %p) got reply from %d/%d: "
		"epoch :"DF_X64", rc %d.\n", dra->dra_opc, req,
		dra->dra_future, drr->drr_rank, drr->drr_tag,
		din != NULL ? din->di_epoch : 0, drr->drr_result);
}

static int
dtx_req_send(struct dtx_req_rec *drr, daos_epoch_t epoch)
{
	struct dtx_req_args	 *dra = drr->drr_parent;
	crt_rpc_t		     *req;
	crt_endpoint_t		  tgt_ep;
	crt_opcode_t		  opc;
	struct dtx_in		 *din = NULL;
	int	rc;

	tgt_ep.ep_grp = NULL;

	// 发送的消息的接收端(group, rank, tgt)
	tgt_ep.ep_rank = drr->drr_rank;
	tgt_ep.ep_tag = daos_rpc_tag(DAOS_REQ_TGT, drr->drr_tag);

	// check abort committed
	opc = DAOS_RPC_OPCODE(dra->dra_opc, DAOS_DTX_MODULE, DAOS_DTX_VERSION);

	rc = crt_req_create(dss_get_module_info()->dmi_ctx, &tgt_ep, opc, &req);
	if (rc == 0) {
		din = crt_req_get(req);
		uuid_copy(din->di_po_uuid, dra->dra_po_uuid);
		uuid_copy(din->di_co_uuid, dra->dra_co_uuid);
		din->di_epoch = epoch;
		din->di_dtx_array.ca_count = drr->drr_count;
		din->di_dtx_array.ca_arrays = drr->drr_dti;
		if (drr->drr_flags != NULL) {
			din->di_flags.ca_count = drr->drr_count;
			din->di_flags.ca_arrays = drr->drr_flags;
		} else {
			din->di_flags.ca_count = 0;
			din->di_flags.ca_arrays = NULL;
		}

		if (dra->dra_opc == DTX_REFRESH && DAOS_FAIL_CHECK(DAOS_DTX_RESYNC_DELAY)) {
			// DTX_REFRESH消息设置3秒的超时时间
			rc = crt_req_set_timeout(req, 3);
			D_ASSERTF(rc == 0, "crt_req_set_timeout failed: %d\n", rc);
		}

        // dtx_req_cb, 消息发送后，等待对端接收replay后的回调，即:收到response的处理函数
        // 消息的真实发送
		rc = crt_req_send(req, dtx_req_cb, drr);
	}

	D_DEBUG(DB_TRACE, "DTX req for opc %x to %d/%d (req %p future %p) sent epoch "DF_X64" : rc %d.\n", dra->dra_opc, drr->drr_rank,
		drr->drr_tag, req, dra->dra_future, din != NULL ? din->di_epoch : 0, rc);

	if (rc != 0 && drr->drr_comp == 0) {
		drr->drr_comp = 1;
		drr->drr_result = rc;
		ABT_future_set(dra->dra_future, drr);
	}

	return rc;
}

// 汇总所有的响应结果时，DTX_CHECK消息处理特殊
static void
dtx_req_list_cb(void **args)
{
	struct dtx_req_rec	*drr = args[0];
	struct dtx_req_args	*dra = drr->drr_parent;
	int			 i;

	if (dra->dra_opc == DTX_CHECK) {
		for (i = 0; i < dra->dra_length; i++) {
			drr = args[i];
			switch (drr->drr_result) {
				case DTX_ST_COMMITTED:
				case DTX_ST_COMMITTABLE:
					dra->dra_result = DTX_ST_COMMITTED;
					/* As long as one target has committed the DTX,
					 * then the DTX is committable on all targets.
					 */
					D_DEBUG(DB_TRACE, "The DTX "DF_DTI" has been committed on %d/%d.\n", DP_DTI(drr->drr_dti), drr->drr_rank, drr->drr_tag);
					return;
				case -DER_EXCLUDED:
					/*
					 * If non-leader is excluded, handle it as 'prepared'. If other
					 * non-leaders are also 'prepared' then related DTX maybe still
					 * committable or 'corrupted'. The subsequent DTX resync logic
					 * will handle related things, see dtx_verify_groups().
					 *
					 * Fall through.
					 */
				case DTX_ST_PREPARED:
					if (dra->dra_result == 0 || dra->dra_result == DTX_ST_CORRUPTED)
						dra->dra_result = DTX_ST_PREPARED;
					break;
				case DTX_ST_CORRUPTED:
					if (dra->dra_result == 0)
						dra->dra_result = drr->drr_result;
					break;
				default:
					dra->dra_result = drr->drr_result >= 0 ? -DER_IO : drr->drr_result;
					break;
			}

			D_DEBUG(DB_TRACE, "The DTX "DF_DTI" RPC req result %d, status is %d.\n", DP_DTI(drr->drr_dti), drr->drr_result, dra->dra_result);
		}
	} 
	
	else {
		// 遍历所有的子请求
		for (i = 0; i < dra->dra_length; i++) {
			drr = args[i];
			
			if (dra->dra_result == 0 || dra->dra_result == -DER_NONEXIST)
				dra->dra_result = drr->drr_result;
		}

		drr = args[0];
		D_CDEBUG(dra->dra_result < 0 && dra->dra_result != -DER_NONEXIST && dra->dra_result != -DER_INPROGRESS, DLOG_ERR, DB_TRACE, 
			"DTX req for opc %x ("DF_DTI") %s, count %d: %d.\n", dra->dra_opc, DP_DTI(drr->drr_dti), 
			dra->dra_result < 0 ? "failed" : "succeed", dra->dra_length, dra->dra_result);
	}
}

static int
dtx_req_wait(struct dtx_req_args *dra)
{
	int	rc;

	rc = ABT_future_wait(dra->dra_future);
	
	D_ASSERTF(rc == ABT_SUCCESS, "ABT_future_wait failed for opc %x, length = %d: rc = %d.\n", dra->dra_opc, dra->dra_length, rc);

	D_DEBUG(DB_TRACE, "DTX req for opc %x, future %p done, rc = %d\n", dra->dra_opc, dra->dra_future, rc);

	ABT_future_free(&dra->dra_future);
	return dra->dra_result;
}

static int
dtx_req_list_send(struct dtx_req_args *dra, crt_opcode_t opc, int *committed, d_list_t *head,
		  int len, uuid_t po_uuid, uuid_t co_uuid, daos_epoch_t epoch,
		  struct ds_cont_child *cont, d_list_t *cmt_list,
		  d_list_t *abt_list, d_list_t *act_list)
{
	ABT_future		    future;
	struct dtx_req_rec	*drr;
	int			 rc;
	int			 i = 0;

	dra->dra_opc = opc;
	uuid_copy(dra->dra_po_uuid, po_uuid);
	uuid_copy(dra->dra_co_uuid, co_uuid);
	dra->dra_length = len;
	dra->dra_result = 0;
	dra->dra_cont = cont;
	dra->dra_cmt_list = cmt_list;
	dra->dra_abt_list = abt_list;
	dra->dra_act_list = act_list;
	dra->dra_committed = committed;

    // dtx_req_list_cb调用； 只要len不是0，ABT_future_set执行1次(dtx_req_cb函数会调用)调用1下；
    // dtx_req_list_cb调用的意思是: 每次发送的响应回来之后会去更改下dtx_req_args里面记录的各个节点回响应的结果
	rc = ABT_future_create(len, dtx_req_list_cb, &future);
	if (rc != ABT_SUCCESS) {
		D_ERROR("ABT_future_create failed for opc %x, len = %d: rc = %d.\n", opc, len, rc);
		return dss_abterr2der(rc);
	}

	D_DEBUG(DB_TRACE, "DTX req for opc %x, future %p start.\n", opc, future);
	
	dra->dra_future = future;

	// 遍历待发送链表:head, 里面是1个个的drr(dtx_req_rec)
	d_list_for_each_entry(drr, head, drr_link) {
	
		drr->drr_parent = dra;
		drr->drr_result = 0;

        // unlikely表示这个分支判断为false的概率更大，执行else的概率更高
        // 写这个的目的是把这个程序的预取告诉CPU，CPU也会预取else的指令，提升CPU的性能
		if (unlikely(opc == DTX_COMMIT && i == 0 && DAOS_FAIL_CHECK(DAOS_DTX_FAIL_COMMIT)))
			rc = dtx_req_send(drr, 1);
		else
			// dtx消息发送
			rc = dtx_req_send(drr, epoch);
		
		if (rc != 0) {
			/* If the first sub-RPC failed, then break, otherwise
			 * other remote replicas may have already received the
			 * RPC and executed it, so have to go ahead.
			 */
			if (i == 0) {
				ABT_future_free(&dra->dra_future);
				dra->dra_future = ABT_FUTURE_NULL;
				return rc;
			}
		}

		/* Yield to avoid holding CPU for too long time. */
		if (++i % DTX_RPC_YIELD_THD == 0)
			ABT_thread_yield();
	}

	return 0;
}

static int
dtx_cf_rec_alloc(struct btr_instance *tins, d_iov_t *key_iov,
		 d_iov_t *val_iov, struct btr_record *rec, d_iov_t *val_out)
{
	struct dtx_req_rec		*drr;
	struct dtx_cf_rec_bundle	*dcrb;

	D_ASSERT(tins->ti_umm.umm_id == UMEM_CLASS_VMEM);

	D_ALLOC_PTR(drr);
	if (drr == NULL)
		return -DER_NOMEM;

	dcrb = val_iov->iov_buf;
	D_ALLOC_ARRAY(drr->drr_dti, dcrb->dcrb_count);
	if (drr->drr_dti == NULL) {
		D_FREE(drr);
		return -DER_NOMEM;
	}

	drr->drr_rank = dcrb->dcrb_rank;
	drr->drr_tag = dcrb->dcrb_tag;
	drr->drr_count = 1;
	drr->drr_comp = 0;
	drr->drr_dti[0] = *dcrb->dcrb_dti;
	d_list_add_tail(&drr->drr_link, dcrb->dcrb_head);
	++(*dcrb->dcrb_length);

	rec->rec_off = umem_ptr2off(&tins->ti_umm, drr);
	return 0;
}

static int
dtx_cf_rec_free(struct btr_instance *tins, struct btr_record *rec, void *args)
{
	struct dtx_req_rec	*drr;

	D_ASSERT(tins->ti_umm.umm_id == UMEM_CLASS_VMEM);

	drr = (struct dtx_req_rec *)umem_off2ptr(&tins->ti_umm, rec->rec_off);
	d_list_del(&drr->drr_link);
	D_FREE(drr->drr_cb_args);
	D_FREE(drr->drr_dti);
	D_FREE(drr->drr_flags);
	D_FREE(drr);

	return 0;
}

static int
dtx_cf_rec_fetch(struct btr_instance *tins, struct btr_record *rec,
		 d_iov_t *key_iov, d_iov_t *val_iov)
{
	D_ASSERTF(0, "We should not come here.\n");
	return 0;
}

static int
dtx_cf_rec_update(struct btr_instance *tins, struct btr_record *rec,
		  d_iov_t *key, d_iov_t *val, d_iov_t *val_out)
{
	struct dtx_req_rec		*drr;
	struct dtx_cf_rec_bundle	*dcrb;

	drr = (struct dtx_req_rec *)umem_off2ptr(&tins->ti_umm, rec->rec_off);
	dcrb = (struct dtx_cf_rec_bundle *)val->iov_buf;
	D_ASSERT(drr->drr_count >= 1);

	if (!daos_dti_equal(&drr->drr_dti[drr->drr_count - 1],
			    dcrb->dcrb_dti)) {
		D_ASSERT(drr->drr_count < dcrb->dcrb_count);

		drr->drr_dti[drr->drr_count++] = *dcrb->dcrb_dti;
	}

	return 0;
}

btr_ops_t dbtree_dtx_cf_ops = {
	.to_rec_alloc	= dtx_cf_rec_alloc,
	.to_rec_free	= dtx_cf_rec_free,
	.to_rec_fetch	= dtx_cf_rec_fetch,
	.to_rec_update	= dtx_cf_rec_update,
};

#define DTX_CF_BTREE_ORDER	20

static int
dtx_classify_one(struct ds_pool *pool, daos_handle_t tree, d_list_t *head, int *length,
		 struct dtx_entry *dte, int count, d_rank_t my_rank, uint32_t my_tgtid)
{
	struct dtx_memberships		*mbs = dte->dte_mbs;
	struct dtx_cf_rec_bundle	 dcrb;
	int				 rc = 0;
	int				 i;

	if (mbs->dm_tgt_cnt == 0)
		return -DER_INVAL;

	if (daos_handle_is_valid(tree)) {
		dcrb.dcrb_count = count;
		dcrb.dcrb_dti = &dte->dte_xid;
		dcrb.dcrb_head = head;
		dcrb.dcrb_length = length;
	}

	if (mbs->dm_flags & DMF_CONTAIN_LEADER)
		/* mbs->dm_tgts[0] is the (current/old) leader, skip it. */
		i = 1;
	else
		i = 0;
	
	for (; i < mbs->dm_tgt_cnt && rc >= 0; i++) {
		
		struct pool_target	*target;

		rc = pool_map_find_target(pool->sp_map, mbs->dm_tgts[i].ddt_id, &target);
		if (rc != 1) {
			D_WARN("Cannot find target %u at %d/%d, flags %x\n", mbs->dm_tgts[i].ddt_id, i, mbs->dm_tgt_cnt, mbs->dm_flags);
			return -DER_UNINIT;
		}

		/* Skip the target that (re-)joined the system after the DTX. */
		// 这个盘是在这条事务之后加入的,tgt不处理
		if (target->ta_comp.co_ver > dte->dte_ver)
			continue;

		/* Skip non-healthy one. */
		if (target->ta_comp.co_status != PO_COMP_ST_UP &&
		    target->ta_comp.co_status != PO_COMP_ST_UPIN &&
		    target->ta_comp.co_status != PO_COMP_ST_NEW &&
		    target->ta_comp.co_status != PO_COMP_ST_DRAIN)
			continue;

		/* Skip myself. */
		if (my_rank == target->ta_comp.co_rank && my_tgtid == target->ta_comp.co_index)
			continue;

		if (daos_handle_is_valid(tree)) {  // 一次处理多个dtx_entry, 将这个查到树里面
			d_iov_t			 kiov;
			d_iov_t			 riov;

			dcrb.dcrb_rank = target->ta_comp.co_rank;
			dcrb.dcrb_tag = target->ta_comp.co_index;

			d_iov_set(&riov, &dcrb, sizeof(dcrb));
			// 这个key牛逼，是联合体里面的数据
			d_iov_set(&kiov, &dcrb.dcrb_key, sizeof(dcrb.dcrb_key));
			rc = dbtree_upsert(tree, BTR_PROBE_EQ, DAOS_INTENT_UPDATE, &kiov, &riov, NULL);
		} else {
			struct dtx_req_rec	*drr;

			D_ALLOC_PTR(drr);
			if (drr == NULL)
				return -DER_NOMEM;

			drr->drr_rank = target->ta_comp.co_rank;
			drr->drr_tag = target->ta_comp.co_index;
			drr->drr_count = 1;
			drr->drr_dti = &dte->dte_xid;
			d_list_add_tail(&drr->drr_link, head);
			(*length)++;
		}
	}

	return rc > 0 ? 0 : rc;
}

// 入参：cont     dtes(dtx_entry)   epoch   count   opc  my_rank  my_tgtid
// 出参：head tree_root  tree_hdl dra dtis
// 当前只有当opc==dtx_committ的时候，count可能会大于1
static int
dtx_rpc_internal(struct ds_cont_child *cont, d_list_t *head, struct btr_root *tree_root,
		 daos_handle_t *tree_hdl, struct dtx_req_args *dra, struct dtx_id dtis[],
		 struct dtx_entry **dtes, daos_epoch_t epoch, int count, int opc,
		 int *committed, d_rank_t my_rank, uint32_t my_tgtid)
{
	struct ds_pool	*pool;
	int			    length = 0;
	int			    rc;
	int			    i;

	D_ASSERT(cont->sc_pool != NULL);
	pool = cont->sc_pool->spc_pool;
	D_ASSERT(pool != NULL);

	if (count > 1) {
		struct umem_attr	uma = { 0 };
		uma.uma_id = UMEM_CLASS_VMEM;
		rc = dbtree_create_inplace(DBTREE_CLASS_DTX_CF, 0, DTX_CF_BTREE_ORDER, &uma, tree_root, tree_hdl);
		if (rc != 0)
			return rc;
	}

	ABT_rwlock_rdlock(pool->sp_lock);
	for (i = 0; i < count; i++) {
		// 场景1： 只处理1个dtx_entry,即count=1: tree_hdl无效的，
		//         返回的length表示的是dtx_entry里面dtx_memberships的有效数量(根据tgt的版本号和状态排除)
		//         返回的head表示将dtx_entry里面的每个dtx_memberships构成dtx_req_rec，然后组成处理链表
		
		// 场景2： 处理多个dtx_entry， tree_hdl有效，新创建的树
		//         那么是将每个dtx_entry里面的每个dtx_memberships组成dtx_cf_rec_bundle作为value, 
		//         每个dtx_memberships的tgt和ran组成key, 插到DBTREE_CLASS_DTX_CF树中
		rc = dtx_classify_one(pool, *tree_hdl, head, &length, dtes[i], count, my_rank, my_tgtid);
		if (rc < 0) {
			ABT_rwlock_unlock(pool->sp_lock);
			return rc;
		}

		if (dtis != NULL)
			dtis[i] = dtes[i]->dte_xid;
	}
	ABT_rwlock_unlock(pool->sp_lock);

	/* For DTX_CHECK, if no other available target(s), then current target is the
	 * unique valid one (and also 'prepared'), then related DTX can be committed.
	 */
	// head为空，如果是commit多个这个直接返回不走dtx_req_list_send
	if (d_list_empty(head))
		return opc == DTX_CHECK ? DTX_ST_PREPARED : 0;

    // head非空，代表是commit单个、abort、check流程往下走
	D_ASSERT(length > 0);

    // committ时epoch为0，其余为有效的epoch
	return dtx_req_list_send(dra, opc, committed, head, length, pool->sp_uuid, cont->sc_uuid, epoch, NULL, NULL, NULL, NULL);
}

struct dtx_helper_args {
	struct ds_cont_child	 *dha_cont;
	d_list_t		 *dha_head;
	struct btr_root		 *dha_tree_root;
	daos_handle_t		 *dha_tree_hdl;
	struct dtx_req_args	 *dha_dra;
	struct dtx_entry	**dha_dtes;
	daos_epoch_t		  dha_epoch;
	int			  dha_count;
	int			  dha_opc;
	int			 *dha_committed;
	d_rank_t		  dha_rank;
	uint32_t		  dha_tgtid;
};

static void
dtx_rpc_helper(void *arg)
{
	struct dtx_helper_args	*dha = arg;

	dtx_rpc_internal(dha->dha_cont, dha->dha_head, dha->dha_tree_root, dha->dha_tree_hdl,
			 dha->dha_dra, NULL, dha->dha_dtes, dha->dha_epoch, dha->dha_count,
			 dha->dha_opc, dha->dha_committed, dha->dha_rank, dha->dha_tgtid);

	D_DEBUG(DB_TRACE, "DTX helper ULT for %u exit\n", dha->dha_opc);

	D_FREE(dha);
}

// opc进来就三种情况：check(dtx_cleanup、dtx_resync、dtx_refresh) commit(dtx_commit) abort(dtx_abort)
// check操作进来的count是1，一次只搞1个

// head为返回的待处理的链表
// tree_root和tree_hdl为当处理多个dtx_entry时，创建的DBTREE_CLASS_DTX_CF树的root和handle

static int
dtx_rpc_prep(struct ds_cont_child *cont, d_list_t *head, struct btr_root *tree_root,
	     daos_handle_t *tree_hdl, struct dtx_req_args *dra, ABT_thread *helper,
	     struct dtx_id dtis[], struct dtx_entry **dtes, daos_epoch_t epoch,
	     uint32_t count, int opc, int *committed)
{
	d_rank_t	my_rank;
	uint32_t	my_tgtid;
	int		rc;

	D_INIT_LIST_HEAD(head);
	dra->dra_future = ABT_FUTURE_NULL;
	crt_group_rank(NULL, &my_rank);
	my_tgtid = dss_get_module_info()->dmi_tgt_id;

	/* Use helper ULT to handle DTX RPC if there are enough helper XS. */

    // 如果线程数量足够并且处理的dtx数量较多，可以创建个ULT搞， 不够就直接搞：check不会进来
    // 核心调用函数 dtx_rpc_internal
	if (dss_has_enough_helper() && (dtes[0]->dte_mbs->dm_tgt_cnt - 1) * count >= dtx_rpc_helper_thd) {
		struct dtx_helper_args	*dha = NULL;

		D_ALLOC_PTR(dha);
		if (dha == NULL)
			return -DER_NOMEM;

		dha->dha_cont = cont;
		dha->dha_head = head;
		dha->dha_tree_root = tree_root;
		dha->dha_tree_hdl = tree_hdl;
		dha->dha_dra = dra;
		dha->dha_dtes = dtes;
		dha->dha_epoch = epoch;
		dha->dha_count = count;
		dha->dha_opc = opc;
		dha->dha_committed = committed;
		dha->dha_rank = my_rank;
		dha->dha_tgtid = my_tgtid;

		rc = dss_ult_create(dtx_rpc_helper, dha, DSS_XS_IOFW, my_tgtid, DSS_DEEP_STACK_SZ, helper);
		if (rc != 0) {
			D_FREE(dha);
		} else if (dtis != NULL) {
			int	i;

			for (i = 0; i < count; i++)
				dtis[i] = dtes[i]->dte_xid;
		}
	} else {
		rc = dtx_rpc_internal(cont, head, tree_root, tree_hdl, dra, dtis, dtes, epoch, count, opc, committed, my_rank, my_tgtid);
	}

	return rc;
}

static int
dtx_rpc_post(d_list_t *head, daos_handle_t *tree_hdl, struct dtx_req_args *dra,
	     ABT_thread *helper, int ret)
{
	struct dtx_req_rec	*drr;
	int			         rc = 0;
	bool			     free_dti = false;

	if (*helper != ABT_THREAD_NULL)
		ABT_thread_free(helper);

	if (dra->dra_future != ABT_FUTURE_NULL)
		rc = dtx_req_wait(dra);

	if (daos_handle_is_valid(*tree_hdl)) {
		dbtree_destroy(*tree_hdl, NULL);
		free_dti = true;
	}

	while ((drr = d_list_pop_entry(head, struct dtx_req_rec, drr_link)) != NULL) {
		if (free_dti) {
			D_FREE(drr->drr_dti);
			D_FREE(drr->drr_flags);
		}
		D_FREE(drr);
	}

	return ret != 0 ? ret : rc;
}

/**
 * Commit the given DTX array globally.
 *
 * For each DTX in the given array, classify(分类) its shards. It is quite possible
 * that the shards for different DTXs reside on the same server (rank + tag),
 * then they can be sent to remote server via single DTX_COMMIT RPC and then
 * be committed by remote server via single PMDK transaction.
 *
 * After the DTX classification, send DTX_COMMIT RPC to related servers, and
 * then call DTX commit locally. For a DTX, it is possible that some targets
 * have committed successfully, but others failed. That is no matter. As long
 * as one target has committed, then the DTX logic can re-sync those failed
 * targets when dtx_resync() is triggered next time.
 */
int
dtx_commit(struct ds_cont_child *cont, struct dtx_entry **dtes, struct dtx_cos_key *dcks, int count)
{
	d_list_t		     head;
	struct btr_root		 tree_root = { 0 };
	daos_handle_t		 tree_hdl = DAOS_HDL_INVAL;
	struct dtx_req_args	 dra;
	ABT_thread		     helper = ABT_THREAD_NULL;
	struct dtx_id		*dtis = NULL;
	bool			    *rm_cos = NULL;
	struct dtx_id		 dti = { 0 };
	bool			     cos = false;
	int			 committed = 0;
	int			 rc;
	int			 rc1 = 0;
	int			 i;

	if (count > 1) {
		D_ALLOC_ARRAY(dtis, count);
		if (dtis == NULL)
			D_GOTO(out, rc = -DER_NOMEM);
	} else {
		dtis = &dti;
	}

    // 除了dtes表示传入的dtx_entry为入参，其余大部分参数为出参
	rc = dtx_rpc_prep(cont, &head, &tree_root, &tree_hdl, &dra, &helper, dtis, dtes, 0/*epoch*/, count, DTX_COMMIT, &committed);

	/*
	 * NOTE: Before committing the DTX on remote participants, we cannot remove the active
	 *	 DTX locally; otherwise, the local committed DTX entry may be removed via DTX
	 *	 aggregation before remote participants commit done. Under such case, if some
	 *	 remote DTX participant triggere DTX_REFRESH for such DTX during the interval,
	 *	 then it will get -DER_TX_UNCERTAIN, that may cause related application to be
	 *	 failed. So here, we let remote participants to commit firstly, if failed, we
	 *	 will ask the leader to retry the commit until all participants got committed.
	 *
	 * Some RPC may has been sent, so need to wait even if dtx_rpc_prep hit failure.
	 */

	// 如果dtx_rpc_prep返回非0，dtx_rpc_post会继续处理，只不过dtx_rpc_post会返回prep的返回值
	// 如果dtx_rpc_prep返回0，dtx_rpc_post会处理，只不过dtx_rpc_post会返回自己的返回值
	rc = dtx_rpc_post(&head, &tree_hdl, &dra, &helper, rc);
	if (rc > 0 || rc == -DER_NONEXIST || rc == -DER_EXCLUDED)
		rc = 0;

	if (rc != 0) {
		/*
		 * Some DTX entries may have been committed on some participants. Then mark all
		 * the DTX entries (in the dtis) as "PARTIAL_COMMITTED" and re-commit them later.
		 * It is harmless to re-commit the DTX that has ever been committed.
		 */
		if (committed > 0)
			// 有部分事务提交成功
			rc1 = vos_dtx_set_flags(cont->sc_hdl, dtis, count, DTE_PARTIAL_COMMITTED);
	} else {
		if (dcks != NULL) {
			if (count > 1) {
				D_ALLOC_ARRAY(rm_cos, count);
				if (rm_cos == NULL)
					D_GOTO(out, rc1 = -DER_NOMEM);
			} else {
				rm_cos = &cos;
			}
		}

		rc1 = vos_dtx_commit(cont->sc_hdl, dtis, count, rm_cos);
		if (rc1 > 0) {
			committed += rc1;
			rc1 = 0;
		} else if (rc1 == -DER_NONEXIST) {
			/* -DER_NONEXIST may be caused by race or repeated commit, ignore it. */
			rc1 = 0;
		}

		if (rc1 == 0 && rm_cos != NULL) {
			for (i = 0; i < count; i++) {
				if (rm_cos[i]) {
					D_ASSERT(!daos_oid_is_null(dcks[i].oid.id_pub));
					dtx_del_cos(cont, &dtis[i], &dcks[i].oid, dcks[i].dkey_hash);
				}
			}
		}

		if (rm_cos != &cos)
			D_FREE(rm_cos);
	}

out:
	if (dtis != &dti)
		D_FREE(dtis);

	if (rc != 0 || rc1 != 0)
		D_ERROR("Failed to commit DTX entries "DF_DTI", count %d, %s committed: %d %d\n",DP_DTI(&dtes[0]->dte_xid), count, committed > 0 ? "partial" : "nothing", rc, rc1);
	else
		D_DEBUG(DB_IO, "Commit DTXs " DF_DTI", count %d\n", DP_DTI(&dtes[0]->dte_xid), count);

	return rc != 0 ? rc : rc1;
}


int
dtx_abort(struct ds_cont_child *cont, struct dtx_entry *dte, daos_epoch_t epoch)
{
	d_list_t		head;
	struct btr_root		tree_root = { 0 };
	daos_handle_t		tree_hdl = DAOS_HDL_INVAL;
	struct dtx_req_args	dra;
	ABT_thread		helper = ABT_THREAD_NULL;
	int			rc;
	int			rc1;
	int			rc2;

	rc = dtx_rpc_prep(cont, &head, &tree_root, &tree_hdl, &dra, &helper, NULL, &dte, epoch, 1, DTX_ABORT, NULL);
	
	rc2 = dtx_rpc_post(&head, &tree_hdl, &dra, &helper, rc);
	if (rc2 > 0 || rc2 == -DER_NONEXIST)
		rc2 = 0;

	/*
	 * NOTE: The DTX abort maybe triggered by dtx_leader_end() for timeout on some DTX
	 *	 participant(s). Under such case, the client side RPC sponsor(发起者) may also hit
	 *	 the RPC timeout and resends related RPC to the leader. Here, to avoid DTX
	 *	 abort and resend RPC forwarding being executed in parallel, we will abort
	 *	 local DTX after remote done, before that the logic of handling resent RPC
	 *	 on server will find the local pinned DTX entry then notify related client
	 *	 to resend sometime later.
	 */

    // 当某个dtx事务的参与者执行超时，dtx_leader_end就会触发dtx_abort.
    // 在这种场景下，客户端RPC的发起者也可能遇到RPC超时，并将相关RPC重发给leader。
    // 这里为了避免dtx_abort和转发的resend rpc并行执行，在其他节点执行abort后，我们才会执行本地的dtx_abort
    // 在此之前(处理resent rpc消息的)server端会找到本地pinned dtx entry,然后通知相关的client稍后重新发送。
	
	if (epoch != 0)
		rc1 = vos_dtx_abort(cont->sc_hdl, &dte->dte_xid, epoch);
	else
		rc1 = vos_dtx_set_flags(cont->sc_hdl, &dte->dte_xid, 1, DTE_CORRUPTED);
	
	if (rc1 > 0 || rc1 == -DER_NONEXIST)
		rc1 = 0;

	D_CDEBUG(rc1 != 0 || rc2 != 0, DLOG_ERR, DB_IO, "Abort DTX "DF_DTI": rc %d %d %d\n", DP_DTI(&dte->dte_xid), rc, rc1, rc2);

	return rc1 != 0 ? rc1 : rc2;
}

int
dtx_check(struct ds_cont_child *cont, struct dtx_entry *dte, daos_epoch_t epoch)
{
	d_list_t		    head;
	struct btr_root		tree_root = { 0 };
	daos_handle_t		tree_hdl = DAOS_HDL_INVAL;
	struct dtx_req_args	dra;
	ABT_thread		    helper = ABT_THREAD_NULL;
	int			        rc;
	int			        rc1;

	/* If no other target, then current target is the unique
	 * one and 'prepared', then related DTX can be committed.
	 */
	if (dte->dte_mbs->dm_tgt_cnt == 1)
		return DTX_ST_PREPARED;

	rc = dtx_rpc_prep(cont, &head, &tree_root, &tree_hdl, &dra, &helper, NULL, &dte, epoch, 1, DTX_CHECK, NULL);

	rc1 = dtx_rpc_post(&head, &tree_hdl, &dra, &helper, rc);

	D_CDEBUG(rc1 < 0, DLOG_ERR, DB_IO, "Check DTX "DF_DTI": rc %d %d\n", DP_DTI(&dte->dte_xid), rc, rc1);

	return rc1;
}

int
dtx_refresh_internal(struct ds_cont_child *cont, int *check_count,
		     d_list_t *check_list, d_list_t *cmt_list,
		     d_list_t *abt_list, d_list_t *act_list, bool failout)
{
	struct ds_pool		    *pool = cont->sc_pool->spc_pool;
	struct pool_target	    *target;
	struct dtx_share_peer	*dsp;
	struct dtx_share_peer	*tmp;
	struct dtx_req_rec	    *drr;
	struct dtx_req_args	     dra;
	
	d_list_t		 head;
	d_list_t		 self;
	d_rank_t		 myrank;
	uint32_t		 flags;
	int			     len = 0;
	int			     rc = 0;
	int			     count;
	bool			 drop;

	D_INIT_LIST_HEAD(&head);
	D_INIT_LIST_HEAD(&self);

	// 获取当前的rank号
	crt_group_rank(NULL, &myrank);

    // 1. 遍历 dth->dth_share_tbd_list 链表, 该链表里面存放的是1个个的dtx_share_peer
	d_list_for_each_entry_safe(dsp, tmp, check_list, dsp_link) {
		count = 0;
		drop = false;

		if (dsp->dsp_mbs == NULL) {
			rc = vos_dtx_load_mbs(cont->sc_hdl, &dsp->dsp_xid, &dsp->dsp_mbs);
			if (rc != 0) {
				if (rc != -DER_NONEXIST && failout)
					goto out;

				drop = true;
				goto next;
			}
		}

again:
        // 2. 获取当前dsp中的tgt(The first UPIN target is the leader of the DTX)
        //    哪个tgt是当前dtx的leader
		rc = dtx_leader_get(pool, dsp->dsp_mbs, &target);
		if (rc < 0) {
			/**
			 * Currently, for EC object, if parity node is
			 * in rebuilding, we will get -DER_STALE, that
			 * is not fatal, the caller or related request
			 * sponsor can retry sometime later.
			 */
			D_WARN("Failed to find DTX leader for "DF_DTI", ver %d: "DF_RC"\n",
			       DP_DTI(&dsp->dsp_xid), pool->sp_map_version, DP_RC(rc));
			if (failout)
				goto out;

			drop = true;
			goto next;
		}


		/* If current server is the leader, then two possible cases:
		 *
		 * 1. In DTX resync, the status may be resolved sometime later.
		 * 2. The DTX resync is done, but failed to handle related DTX.
		 */

		//  如果当前发起dtx_refresh的就是dtx leader(rank号一致，tgt一致)，这个dsp从检查链表中摘除，不处理
		//  这个地方还和dtx_resync相关， 主要关联性是啥???????
		if (myrank == target->ta_comp.co_rank && dss_get_module_info()->dmi_tgt_id == target->ta_comp.co_index) {
			d_list_del(&dsp->dsp_link);
			d_list_add_tail(&dsp->dsp_link, &self);
			if (--(*check_count) == 0)
				break;
			continue;
		}


		/* Usually, we will not elect in-rebuilding server as DTX leader. 
		   But we may be blocked by the ABT_rwlock_rdlock, then pool map may be refreshed during that. 
		   Let's retry to find out the new leader.
		 */

		// 通常情况下，我们不会选到非PO_COMP_ST_UP和非PO_COMP_ST_UPIN状态的leader_tgt; 因为有锁可能会找到
		// 一旦找到的leader_tgt就不是PO_COMP_ST_UP和PO_COMP_ST_UPIN状态的，重新找
		if (target->ta_comp.co_status != PO_COMP_ST_UP && target->ta_comp.co_status != PO_COMP_ST_UPIN) {
			if (unlikely(++count % 10 == 3))
				D_WARN("Get stale DTX leader %u/%u (st: %x) for "DF_DTI" %d times, maybe dead loop\n",
				       target->ta_comp.co_rank, target->ta_comp.co_id,
				       target->ta_comp.co_status, DP_DTI(&dsp->dsp_xid), count);
			goto again;
		}

        // 这里可以确保找到的leader_tgt是UP或UP_IN才走的下来
		if (dsp->dsp_mbs->dm_flags & DMF_CONTAIN_LEADER && dsp->dsp_mbs->dm_tgts[0].ddt_id == target->ta_comp.co_id)
			flags = DRF_INITIAL_LEADER;
		else
			flags = 0;

        // 同一个充当dtx leader的tgt存放在同一个dtx_req_rec下面
		d_list_for_each_entry(drr, &head, drr_link) {
			if (drr->drr_rank == target->ta_comp.co_rank && drr->drr_tag == target->ta_comp.co_index) {
				drr->drr_dti[drr->drr_count] = dsp->dsp_xid;  // dtx_id
				drr->drr_flags[drr->drr_count] = flags;
				drr->drr_cb_args[drr->drr_count++] = dsp;
				goto next;
			}
		}

// 申请 dtx_req_rec *drr内存;
		D_ALLOC_PTR(drr);
		if (drr == NULL)
			D_GOTO(out, rc = -DER_NOMEM);

		D_ALLOC_ARRAY(drr->drr_dti, *check_count);
		if (drr->drr_dti == NULL) {
			D_FREE(drr);
			D_GOTO(out, rc = -DER_NOMEM);
		}

		D_ALLOC_ARRAY(drr->drr_flags, *check_count);
		if (drr->drr_flags == NULL) {
			D_FREE(drr->drr_dti);
			D_FREE(drr);
			D_GOTO(out, rc = -DER_NOMEM);
		}

		D_ALLOC_ARRAY(drr->drr_cb_args, *check_count);
		if (drr->drr_cb_args == NULL) {
			D_FREE(drr->drr_dti);
			D_FREE(drr->drr_flags);
			D_FREE(drr);
			D_GOTO(out, rc = -DER_NOMEM);
		}

		drr->drr_rank = target->ta_comp.co_rank;   // leader_rank
		drr->drr_tag = target->ta_comp.co_index;   // leader_tgtid
		drr->drr_count = 1;   
		drr->drr_dti[0] = dsp->dsp_xid;
		drr->drr_flags[0] = flags;
		drr->drr_cb_args[0] = dsp;
		
		d_list_add_tail(&drr->drr_link, &head);   // 入新的临时链head
		len++;

next:
		d_list_del_init(&dsp->dsp_link);   // 链表操作没有搞懂？？
		
		if (drop)
			dtx_dsp_free(dsp);
		if (--(*check_count) == 0)
			break;
	}


// check list链表遍历完成；都按照dtx的leader塞到了head的新的临时链表中
	if (len > 0) {
		// 发送消息，dra为出参， 消息是DTX_REFRESH
		rc = dtx_req_list_send(&dra, 
		                       DTX_REFRESH,   //DTX_REFRESH
		                       NULL, &head, 
		                       len,   // 链表中drr的个数
				               pool->sp_uuid, cont->sc_uuid, 
				               0,    // epoch
				               cont,
				               cmt_list, 
				               abt_list, 
				               act_list);

        //  发送成功，在这里等
		if (rc == 0)
			rc = dtx_req_wait(&dra); // rc: 汇总各个节点的执行结果

		if (rc != 0)
			goto out;
	}

/* Handle the entries whose leaders are on current server. */
// 处理那些当前节点就是主的dtx
	d_list_for_each_entry_safe(dsp, tmp, &self, dsp_link) {
		struct dtx_entry	dte;

		d_list_del(&dsp->dsp_link);

		dte.dte_xid = dsp->dsp_xid;
		dte.dte_ver = pool->sp_map_version;
		dte.dte_refs = 1;
		dte.dte_mbs = dsp->dsp_mbs;

        // 调用dtx_check
		rc = dtx_status_handle_one(cont, &dte, dsp->dsp_epoch, NULL, NULL);
		switch (rc) {
			case DSHR_NEED_COMMIT: {
				struct dtx_entry	*pdte = &dte;
				struct dtx_cos_key	 dck;

				dck.oid = dsp->dsp_oid;
				dck.dkey_hash = dsp->dsp_dkey_hash;
				rc = dtx_commit(cont, &pdte, &dck, 1);
				if (rc < 0 && rc != -DER_NONEXIST && cmt_list != NULL)
					d_list_add_tail(&dsp->dsp_link, cmt_list);
				else
					dtx_dsp_free(dsp);
				continue;
			}
			case DSHR_NEED_RETRY:
				dtx_dsp_free(dsp);
				if (failout)
					D_GOTO(out, rc = -DER_INPROGRESS);
				continue;
			case DSHR_IGNORE:
				dtx_dsp_free(dsp);
				continue;
			case DSHR_ABORT_FAILED:
				if (abt_list != NULL)
					d_list_add_tail(&dsp->dsp_link, abt_list);
				else
					dtx_dsp_free(dsp);
				continue;
			case DSHR_CORRUPT:
				dtx_dsp_free(dsp);
				if (failout)
					D_GOTO(out, rc = -DER_DATA_LOSS);
				continue;
			default:
				dtx_dsp_free(dsp);
				if (failout)
					goto out;
				continue;
		}
	}

	rc = 0;

out:
	// 处理完毕，释放临时链表head
	while ((drr = d_list_pop_entry(&head, struct dtx_req_rec, drr_link)) != NULL) {
		D_FREE(drr->drr_cb_args);
		D_FREE(drr->drr_dti);
		D_FREE(drr->drr_flags);
		D_FREE(drr);
	}

   // 释放自己就是主的链表
	while ((dsp = d_list_pop_entry(&self, struct dtx_share_peer, dsp_link)) != NULL)
		dtx_dsp_free(dsp);

	return rc;
}

/*
 * Because of async batched commit semantics, the DTX status on the leader
 * maybe different from the one on non-leaders. For the leader, it exactly
 * knows whether the DTX is committable or not, but the non-leader does not
 * know if the DTX is in 'prepared' status. If someone on non-leader wants
 * to know whether some 'prepared' DTX is real committable or not, it needs
 * to refresh such DTX status from the leader. The DTX_REFRESH RPC is used
 * for such purpose.
 */

// 由于异步批量提交的语义，就会导致dtx的状态在leader和非leader上不同。
// leader上当然知道dtx是否提交了，但是非leader却不知道集群内的dtx是否还在prepare。
// dtx_refresh用于非leader向leader查询dtx是否已经commit了，
			 
int
dtx_refresh(struct dtx_handle *dth, struct ds_cont_child *cont)
{
	int	rc;

	if (DAOS_FAIL_CHECK(DAOS_DTX_NO_RETRY))
		return -DER_IO;


    // 遍历dth_share_tbd_list链表， 输出cmt/abt/act链表
	rc = dtx_refresh_internal(cont,            //  ds_cont_child
	              &dth->dth_share_tbd_count,   //  DTX list cont
				  &dth->dth_share_tbd_list,    //  DTX list to be checked
				  &dth->dth_share_cmt_list,    //  Committed or comittable DTX list 
				  &dth->dth_share_abt_list,    //  Aborted DTX list
				  &dth->dth_share_act_list,    //  Active DTX list
				  true);                       //  失败了是否就结束退出
				                               //  (1.dtx_refresh函数为true 2.dtx_cleanup为false)

	/* If we can resolve the DTX status, then return -DER_AGAIN to the caller that will retry related operation locally. */
	if (rc == 0) {
		
		D_ASSERT(dth->dth_share_tbd_count == 0);

		if (dth->dth_aborted) {
			rc = -DER_CANCELED;  // 1018: Operation canceled
		} else {
			vos_dtx_cleanup(dth, false);
			dtx_handle_reinit(dth);
			rc = -DER_AGAIN;    // 1013:  Try again
		}
	}

	return rc;
}
