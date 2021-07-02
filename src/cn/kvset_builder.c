/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2015-2020 Micron Technology, Inc.  All rights reserved.
 */

#define MTF_MOCK_IMPL_kvset_builder
#include <hse_ikvdb/kvset_builder.h>
#include <hse_ikvdb/key_hash.h>
#include <hse_ikvdb/limits.h>
#include <hse_ikvdb/cn.h>

#include <hse/hse_limits.h>

#include <hse_util/platform.h>
#include <hse_util/event_counter.h>
#include <hse_util/slab.h>
#include <hse_util/bonsai_tree.h>
#include <hse_util/compression.h>

#include "kcompact.h"
#include "spill.h"

#include "kblock_builder.h"
#include "vblock_builder.h"
#include "vblock_reader.h"
#include "blk_list.h"
#include "kvset_builder_internal.h"

merr_t
kvset_builder_create(
    struct kvset_builder **bld_out,
    struct cn *            cn,
    struct perfc_set *     pc,
    u64                    vgroup,
    uint                   flags)
{
    struct kvset_builder *bld;
    merr_t                err;

    bld = malloc(sizeof(*bld));
    if (!bld)
        return merr(ev(ENOMEM));

    memset(bld, 0, sizeof(*bld));

    bld->seqno_min = U64_MAX;

    err = kbb_create(&bld->kbb, cn, pc, flags);
    if (ev(err))
        goto err_exit1;

    err = vbb_create(&bld->vbb, cn, pc, vgroup, flags);
    if (ev(err))
        goto err_exit2;

    bld->cn = cn;
    bld->key_stats.seqno_prev = U64_MAX;
    bld->key_stats.seqno_prev_ptomb = U64_MAX;

    bld->compress = vcomp_compress_ops(cn_get_rp(cn));

    *bld_out = bld;
    return 0;

err_exit2:
    kbb_destroy(bld->kbb);
err_exit1:
    free(bld);
    return err;
}

static int
reserve_kmd(struct kmd_info *ki)
{
    uint initial = 16*1024;
    uint need = 256;
    uint min_size = ki->kmd_used + need;
    uint new_size;
    u8 * new_mem;

    if (ki->kmd_size >= min_size)
        return 0;

    if (ki->kmd_size)
        new_size = 2 * ki->kmd_size;
    else
        new_size = initial;

    if (new_size < min_size)
        new_size = min_size;

    new_mem = malloc(new_size);
    if (ev(!new_mem))
        return -1;

    if (ki->kmd) {
        memcpy(new_mem, ki->kmd, ki->kmd_used);
        free(ki->kmd);
    }

    ki->kmd = new_mem;
    ki->kmd_size = new_size;
    return 0;
}

merr_t
kvset_builder_add_key(struct kvset_builder *self, const struct key_obj *kobj)
{
    merr_t err = 0;
    uint   klen;

    if (ev(!kobj))
        return merr(EINVAL);

    klen = key_obj_len(kobj);
    if (ev(!klen || klen > HSE_KVS_KLEN_MAX))
        return merr(EINVAL);

    if (self->key_stats.nptombs > 0) {
        err = kbb_add_ptomb(self->kbb, kobj, self->sec.kmd, self->sec.kmd_used, &self->key_stats);
        if (ev(err))
            return err;

        /* track the highest seen ptomb if this is a capped cn */
        if (cn_get_flags(self->cn) & CN_CFLAG_CAPPED)
            key_obj_copy(self->last_ptomb, sizeof(self->last_ptomb), &self->last_ptlen, kobj);
    }

    if (self->key_stats.nvals > 0) {
        err = kbb_add_entry(self->kbb, kobj, self->main.kmd, self->main.kmd_used, &self->key_stats);
        if (ev(err))
            return err;
    }

    self->key_stats.nvals = 0;
    self->key_stats.ntombs = 0;
    self->key_stats.nptombs = 0;
    self->key_stats.tot_vlen = 0;
    self->key_stats.seqno_prev = U64_MAX;
    self->key_stats.seqno_prev_ptomb = U64_MAX;

    self->main.kmd_used = 0;
    self->sec.kmd_used = 0;

    return ev(err);
}

/**
 * kvset_builder_add_val() - Add a value or a tombstone to a kvset entry.
 * @builder: Kvset builder object.
 * @seq: Sequence number of value or tombstone.
 * @vdata: Pointer to @vlen bytes of uncompressed value data, @complen
 *         bytes of compressed value data, or a special tombstone pointer.
 * @vlen: Length of uncompressed value.
 * @complen: Length of compressed value if value is compressed. Must
 *           be set to 0 if value is not compressed.
 * @c1: Handle for c1 builder. Set during ingest from c0/c1 into cn and
 *      used to determine if value exists in a c1 vblock.
 *
 * Notes on compression:
 * - If @complen > 0, then the value is already compressed and will be
 *   stored on media as is (even if compression is not enabled for this
 *   kvset).
 * - If @complen == 0, and compression is enabled for this kvset, then the
 *   value will be compressed before writing to media if compression
 *   results in a worthwhile size reduction.  For example, a 10,000 byte
 *   value that compressed to 9999 bytes will not be stored in compressed
 *   from.
 *
 * Special cases for tombstones:
 *  - If @vdata == %HSE_CORE_TOMB_PFX, then a prefix tombstone is added
 *    and @vlen is ignored.
 *  - If @vdata == %HSE_CORE_TOMB_REG, then a regular tombstone is added
 *    and @vlen is ignored.
 *  - If @vdata == NULL or @vlen == 0, then a zero-length value is added.
 *  - Otherwise, a non-zero length value is added.
 */
//#define DEBUG_KVSET_BUILDER_ADD_VAL
merr_t
kvset_builder_add_val(
    struct kvset_builder   *self,
    u64                     seq,
    const void             *vdata,
    uint                    vlen,
    uint                    complen,
    struct c1_bonsai_vbldr *c1,
    uint 		debug)
{
    merr_t           err;
    u64              seqno_prev;
    struct kmd_info *ki = vdata == HSE_CORE_TOMB_PFX ? &self->sec : &self->main;

#ifdef DEBUG_KVSET_BUILDER_ADD_VAL
    struct timeval startstamp;
    static int count;
    static u64 intv1 = 0, intv2 = 0, intv3 = 0, intv4 = 0;
    u64 t1 = 0, t2 = 0, t3 = 0, t4 = 0, t5 = 0;
    gettimeofday(&startstamp, NULL);
#endif

#ifdef DEBUG_KVSET_BUILDER_ADD_VAL
    if (debug)
	t1 = get_time_ns();
#endif

    if (ev(reserve_kmd(ki)))
        return merr(ENOMEM);

    if (vdata == HSE_CORE_TOMB_REG) {
        kmd_add_tomb(self->main.kmd, &self->main.kmd_used, seq);
        self->key_stats.ntombs++;
    } else if (vdata == HSE_CORE_TOMB_PFX) {
        kmd_add_ptomb(self->sec.kmd, &self->sec.kmd_used, seq);
        self->key_stats.nptombs++;
        self->last_ptseq = seq;
    } else if (!vdata || vlen == 0) {
        kmd_add_zval(self->main.kmd, &self->main.kmd_used, seq);
    } else if (complen == 0 && vlen <= CN_SMALL_VALUE_THRESHOLD) {
        /* Do not currently support compressed valus in KMD as an "ival", so
         * complen must be zero.
         */
        kmd_add_ival(self->main.kmd, &self->main.kmd_used, seq, vdata, vlen);
        self->key_stats.tot_vlen += vlen;
    } else {

        uint vbidx = 0, vboff = 0;
        u64 vbid = 0;
        bool c1value;
        uint omlen; /* on media length */

        assert(vdata);

        c1value = c1 && kvset_vbuilder_vblock_exists(self, seq, vdata, vlen,
            c1, &vbidx, &vboff, &vbid);

        if (c1value) {
            /* value already in a c1 vlbock.
             * c1 compressed values not currently supported.
             */
            if (ev(complen > 0)) {
                assert(0);
                return merr(EBUG);
            }

            omlen = vlen;

            self->key_stats.c1_vlen += vlen;

        } else {

            /* add value to vblock */

            if (self->compress && complen == 0) {
                /* Try to compress value.  If it can't be compressed
                 * for whatever reason, just leave complen == 0 and
                 * the following code will figure it out.
                 */
                complen = self->compress->cop_estimate(vdata, vlen);

                if (unlikely(!complen)) {
                    /* compression library says "no" */
                    goto add_entry;
                }

                if (unlikely(self->compress_buf_sz < complen)) {
                    /* need a bigger buffer */
                    free(self->compress_buf);
                    self->compress_buf_sz = ALIGN(complen, PAGE_SIZE);
                    self->compress_buf = malloc(self->compress_buf_sz);
                    if (unlikely(!self->compress_buf)) {
                        self->compress_buf_sz = 0;
                        complen = 0;
                        goto add_entry;
                    }
                }

                err = self->compress->cop_compress(vdata, vlen,
                    self->compress_buf, self->compress_buf_sz,
                    &complen);

                if (complen > HSE_KVS_VLEN_MAX || ev(err))
                    complen = 0;
                else
                    vdata = self->compress_buf;
            }

          add_entry:
#ifdef DEBUG_KVSET_BUILDER_ADD_VAL
            if (debug)
    	        t2 = get_time_ns();
#endif
            /* vblock builder needs on-media length */
            omlen = complen ? complen : vlen;
            err = vbb_add_entry(self->vbb, vdata, omlen, &vbid, &vbidx, &vboff, debug);
#ifdef DEBUG_KVSET_BUILDER_ADD_VAL
            if (debug)
    	        t3 = get_time_ns();
#endif
            if (ev(err))
                return err;

            self->key_stats.c0_vlen += omlen;
        }

        if (complen)
            kmd_add_cval(self->main.kmd, &self->main.kmd_used, seq, vbidx, vboff, vlen, complen);
        else
            kmd_add_val(self->main.kmd, &self->main.kmd_used, seq, vbidx, vboff, vlen);

#ifdef DEBUG_KVSET_BUILDER_ADD_VAL
        if (debug)
    	    t4 = get_time_ns();
#endif
        /* stats (and space amp) use on-media length */
        self->vused += omlen;
        self->key_stats.tot_vlen += omlen;
    }

    self->seqno_max = max_t(u64, self->seqno_max, seq);
    self->seqno_min = min_t(u64, self->seqno_min, seq);

    if (vdata == HSE_CORE_TOMB_PFX) {
        seqno_prev = self->key_stats.seqno_prev_ptomb;
        self->key_stats.seqno_prev_ptomb = seq;
    } else {
        seqno_prev = self->key_stats.seqno_prev;
        self->key_stats.nvals++;
        self->key_stats.seqno_prev = seq;
    }

    assert(seq <= seqno_prev);

    if (seq > seqno_prev)
        return merr(ev(EINVAL));

#ifdef DEBUG_KVSET_BUILDER_ADD_VAL
    if (debug && t1 != 0 && t2 != 0 && t3 != 0 && t4 != 0) {
	count ++;
    	t5 = get_time_ns();
    	intv1 += t2-t1;
    	intv2 += t3-t2;
    	intv3 += t4-t3;
    	intv4 += t5-t4;
	if (count > 1000000) {
	    printf("[%s] %ld.%06ld %d %lu %lu %lu %lu\n", 
			    __func__, 
			    startstamp.tv_sec,
			    startstamp.tv_usec,
			    count, 
			    intv1, 
			    intv2,
			    intv3, 
			    intv4);
	    count = intv1 = intv2 = intv3 = intv4 = 0;
	}
    }
#endif
    return 0;
}

/**
 * kvset_builder_add_vref() - add a vtype_val or vtype_cval entry its a kvset
 *
 * If @complen > 0, a vtype_cval entry will written to media.
 * If @complen == 0, a vtype_val entry will written to media.
 */
merr_t
kvset_builder_add_vref(
    struct kvset_builder   *self,
    u64                     seq,
    uint                    vbidx,
    uint                    vboff,
    uint                    vlen,
    uint                    complen)
{
    uint om_len = complen ? complen : vlen; /* on-media length */

    if (reserve_kmd(&self->main))
        return merr(ev(ENOMEM));

    if (complen > 0)
        kmd_add_cval(self->main.kmd, &self->main.kmd_used, seq, vbidx, vboff, vlen, complen);
    else
        kmd_add_val(self->main.kmd, &self->main.kmd_used, seq, vbidx, vboff, vlen);

    self->vused += om_len;
    self->key_stats.tot_vlen += om_len;
    self->key_stats.nvals++;

    self->seqno_max = max_t(u64, self->seqno_max, seq);
    self->seqno_min = min_t(u64, self->seqno_min, seq);

    return 0;
}

merr_t
kvset_builder_add_nonval(struct kvset_builder *self, u64 seq, enum kmd_vtype vtype)
{
    struct kmd_info *ki = vtype == vtype_ptomb ? &self->sec : &self->main;

    if (reserve_kmd(ki))
        return merr(ev(ENOMEM));

    assert(vtype != vtype_zval);
    if (vtype == vtype_tomb) {
        kmd_add_tomb(self->main.kmd, &self->main.kmd_used, seq);
        self->key_stats.ntombs++;
        self->key_stats.nvals++;
    } else if (vtype == vtype_ptomb) {
        kmd_add_ptomb(self->sec.kmd, &self->sec.kmd_used, seq);
        self->key_stats.nptombs++;
    } else {
        return merr(ev(EBUG));
    }

    self->seqno_max = max_t(u64, self->seqno_max, seq);
    self->seqno_min = min_t(u64, self->seqno_min, seq);

    return 0;
}

void
kvset_builder_destroy(struct kvset_builder *bld)
{
    if (ev(!bld))
        return;

    abort_mblocks(cn_get_dataset(bld->cn), &bld->kblk_list);
    blk_list_free(&bld->kblk_list);

    abort_mblocks(cn_get_dataset(bld->cn), &bld->vblk_list);
    blk_list_free(&bld->vblk_list);

    kbb_destroy(bld->kbb);
    vbb_destroy(bld->vbb);

    free(bld->main.kmd);
    free(bld->sec.kmd);
    free(bld->compress_buf);
    free(bld);
}

void
kvset_mblocks_destroy(struct kvset_mblocks *blks)
{
    if (blks) {
        blk_list_free(&blks->kblks);
        blk_list_free(&blks->vblks);
    }
}

static merr_t
kvset_builder_finish(struct kvset_builder *imp)
{
    merr_t err = 0;

    assert(imp->kbb);
    assert(imp->vbb);

    err = kbb_finish(imp->kbb, &imp->kblk_list, imp->seqno_min, imp->seqno_max);
    if (ev(err))
        return err;

    if (imp->kblk_list.n_blks == 0) {
        /* There are no kblocks. This happens when each input key has
         * a tombstone and we are in "drop_tomb" mode.  This
         * output kvset is empty and should not be created.  Destroy
         * the corresponding vblock generator (which aborts any
         * mblocks it has already allocated) and move on.  The empty
         * kblk_list will prevent htis kvset from being created. */
        vbb_destroy(imp->vbb);
        imp->vbb = 0;
        return 0;
    }

    err = vbb_finish(imp->vbb, &imp->vblk_list);
    if (ev(err))
        return err;

    return 0;
}

merr_t
kvset_builder_get_mblocks(struct kvset_builder *self, struct kvset_mblocks *mblks)
{
    merr_t           err;
    struct blk_list *list;

    err = kvset_builder_finish(self);
    if (ev(err))
        return err;

    /* transfer kblock ids to caller */
    list = &self->kblk_list;
    mblks->kblks.blks = list->blks;
    mblks->kblks.n_blks = list->n_blks;
    list->blks = 0;
    list->n_blks = 0;

    /* ditto for vblock ids */
    list = &self->vblk_list;
    mblks->vblks.blks = list->blks;
    mblks->vblks.n_blks = list->n_blks;
    list->blks = 0;
    list->n_blks = 0;

    mblks->bl_vused = self->vused;
    mblks->bl_seqno_max = self->seqno_max;
    mblks->bl_seqno_min = self->seqno_min;

    /* copy highest seen ptomb in the builder to cn */
    if (cn_get_flags(self->cn) & CN_CFLAG_CAPPED) {
        mblks->bl_last_ptomb = self->last_ptomb;
        mblks->bl_last_ptlen = self->last_ptlen;
        mblks->bl_last_ptseq = self->last_ptseq;
    }

    return 0;
}

merr_t
kvset_builder_merge_vblocks(struct kvset_builder *dst, struct kvset_builder *src)
{
    struct blk_list blk;
    merr_t          err;
    u32             baseidx;

    u32 nblks __maybe_unused;
    u32 count __maybe_unused;

    assert(dst);
    assert(src);

    err = vbb_finish(src->vbb, &blk);
    if (err)
        return err;

    baseidx = vbb_get_blk_count(dst->vbb);

    /* Update base index post merge so that the function
     * kvset_vbuilder_vblock_exists() which gets called later
     * retrieves the actual index in the merged kvset builder.
     */
    src->vblk_baseidx = baseidx;

    nblks = blk.n_blks;

    err = vbb_blk_list_merge(dst->vbb, src->vbb, &blk);
    if (ev(err))
        return err;

    count = vbb_get_blk_count(dst->vbb);

    assert(count == (baseidx + nblks));

    return 0;
}

void
kvset_builder_set_agegroup(struct kvset_builder *self, enum hse_mclass_policy_age age)
{
    assert(age < HSE_MPOLICY_AGE_CNT);
    kbb_set_agegroup(self->kbb, age);
    vbb_set_agegroup(self->vbb, age);
}

void
kvset_builder_set_merge_stats(struct kvset_builder *self, struct cn_merge_stats *stats)
{
    kbb_set_merge_stats(self->kbb, stats);
    vbb_set_merge_stats(self->vbb, stats);
}

#if defined(HSE_UNIT_TEST_MODE) && HSE_UNIT_TEST_MODE == 1
#include "kvset_builder_ut_impl.i"
#endif /* HSE_UNIT_TEST_MODE */
