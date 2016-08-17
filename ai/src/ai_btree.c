/*
 * ai_btree.c
 *
 * Copyright (C) 2013-2014 Aerospike, Inc.
 *
 * Portions may be licensed to Aerospike, Inc. under one or more contributor
 * license agreements.
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option) any
 * later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see http://www.gnu.org/licenses/
 */

#include <sys/time.h>
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "ai.h"
#include "ai_globals.h"
#include "ai_obj.h"
#include "ai_btree.h"
#include "bt_iterator.h"
#include "bt_output.h"
#include "find.h"
#include "base/thr_sindex.h"
#include "base/cfg.h"

#include <citrusleaf/alloc.h>
#include <citrusleaf/cf_clock.h>
#include <citrusleaf/cf_digest.h>
#include <citrusleaf/cf_ll.h>

#include "fault.h"
#include "util.h"

#define DIG_ARRAY_QUEUE_HIGHWATER 512

#define AI_ARR_MAX_USED 32

/*
 *  Default file to use for printing a B-Tree by the "sindex-dump:" Info. command.
 */
#define DEFAULT_BTREE_DUMP_FILENAME "/tmp/BTREE.dump"

/*
 *  Global determining whether to use array rather than B-Tree.
 */
bool g_use_arr = true;

extern pthread_rwlock_t g_ai_rwlock;

#define AI_GRLOCK()													\
	do {																\
		int ret = pthread_rwlock_rdlock(&g_ai_rwlock);					\
		if (ret) cf_warning(AS_SINDEX, "AI_RLOCK (%d) %s:%d", ret, __FILE__, __LINE__); \
	} while (0);

#define AI_GWLOCK()													\
	do {																\
		int ret = pthread_rwlock_wrlock(&g_ai_rwlock);					\
		if (ret) cf_warning(AS_SINDEX, "AI_WLOCK (%d) %s:%d",ret, __FILE__, __LINE__); \
	} while (0);

#define AI_UNLOCK()													\
	do {																\
		int ret = pthread_rwlock_unlock(&g_ai_rwlock);					\
		if (ret) cf_warning(AS_SINDEX, "AI_UNLOCK (%d) %s:%d",ret, __FILE__, __LINE__); \
	} while (0);

static void
cloneDigestFromai_obj(cf_digest *d, ai_obj *akey)
{
	memcpy(d, &akey->y, CF_DIGEST_KEY_SZ);
}

static void
init_ai_objFromDigest(ai_obj *akey, cf_digest *d)
{
	init_ai_objU160(akey, *(uint160 *)d);
}

const byte INIT_CAPACITY = 1;

static ai_arr *
ai_arr_new()
{
	ai_arr *arr = cf_malloc(sizeof(ai_arr) + (INIT_CAPACITY * CF_DIGEST_KEY_SZ));
	if (!arr) return NULL;
	arr->capacity = INIT_CAPACITY;
	arr->used = 0;
	return arr;
}

static void
ai_arr_move_to_tree(ai_arr *arr, bt *nbtr)
{
	for (int i = 0; i < arr->used; i++) {
		ai_obj apk;
		init_ai_objFromDigest(&apk, (cf_digest *)&arr->data[i * CF_DIGEST_KEY_SZ]);
		if (!btIndNodeAdd(nbtr, &apk)) {
			// what to do ??
			continue;
		}
	}
}

/*
 * Side effect if success full *arr will be freed
 */
void
ai_arr_destroy(ai_arr *arr)
{
	if (!arr) return;
	cf_free(arr);
}

static int
ai_arr_size(ai_arr *arr)
{
	if (!arr) return 0;
	return(sizeof(ai_arr) + (arr->capacity * CF_DIGEST_KEY_SZ));
}

/*
 * Finds the digest in the AI array.
 * Returns
 *      idx if found
 *      -1  if not found
 */
static int
ai_arr_find(ai_arr *arr, cf_digest *dig)
{
	for (int i = 0; i < arr->used; i++) {
		if (0 == cf_digest_compare(dig, (cf_digest *)&arr->data[i * CF_DIGEST_KEY_SZ])) {
			return i;
		}
	}
	return -1;
}

static ai_arr *
ai_arr_shrink(ai_arr *arr)
{
	int size = arr->capacity / 2;

	// Do not shrink if the capacity not greater than 4
	// or if the halving capacity is not a extra level
	// over currently used
	if ((arr->capacity <= 4) ||
			(size < arr->used * 2)) {
		return arr;
	}

	ai_arr * temp_arr = cf_realloc(arr, sizeof(ai_arr) + (size * CF_DIGEST_KEY_SZ));
	if (!temp_arr) {
		cf_warning(AS_SINDEX, "Shrink Failed ... ignoring...");
		return arr;
	}
	temp_arr->capacity = size;
	return temp_arr;
}

static ai_arr *
ai_arr_delete(ai_arr *arr, cf_digest *dig, bool *notfound)
{
	int idx = ai_arr_find(arr, dig);
	// Nothing to delete
	if (idx < 0) {
		*notfound = true;
		return arr;
	}
	if (idx != arr->used - 1) {
		int dest_offset = idx * CF_DIGEST_KEY_SZ;
		int src_offset = (arr->used - 1) * CF_DIGEST_KEY_SZ;
		// move last element
		memcpy(&arr->data[dest_offset], &arr->data[src_offset], CF_DIGEST_KEY_SZ);
	}
	arr->used--;
	return ai_arr_shrink(arr);
}

/*
 * Returns
 *      arr pointer in case of successful operation
 *      NULL in case of failure
 */
static ai_arr *
ai_arr_expand(ai_arr *arr)
{
	int size = arr->capacity * 2;

	if (size > AI_ARR_MAX_SIZE) {
		cf_crash(AS_SINDEX, "Refusing to expand ai_arr to %d (beyond limit of %d)", size, AI_ARR_MAX_SIZE);
	}

	arr = cf_realloc(arr, sizeof(ai_arr) + (size * CF_DIGEST_KEY_SZ));
	//cf_info(AS_SINDEX, "EXPAND REALLOC to %d", size);
	if (!arr) {
		return NULL;
	}
	arr->capacity = size;
	return arr;
}

/*
 * Returns
 *      arr in case of success
 *      NULL in case of failure
 */
static ai_arr *
ai_arr_insert(ai_arr *arr, cf_digest *dig, bool *found)
{
	int idx = ai_arr_find(arr, dig);
	// already found
	if (idx >= 0) {
		*found = true;
		return arr;
	}
	if (arr->used == arr->capacity) {
		arr = ai_arr_expand(arr);
	}
	if (!arr) {
		return NULL;
	}
	memcpy(&arr->data[arr->used * CF_DIGEST_KEY_SZ], dig, CF_DIGEST_KEY_SZ);
	arr->used++;
	return arr;
}

/*
 * Returns the size diff
 */
static int
anbtr_check_convert(ai_nbtr *anbtr, uchar pktyp)
{
	// Nothing to do
	if (anbtr->is_btree)
		return 0;

	ai_arr *arr = anbtr->u.arr;
	if (arr && (arr->used >= AI_ARR_MAX_USED)) {
		//cf_info(AS_SINDEX,"Flipped @ %d", arr->used);
		ulong ba = ai_arr_size(arr);
		// Allocate btree move digest from arr to btree
		bt *nbtr = createIndexNode(pktyp, COL_TYPE_NONE);
		if (!nbtr) {
			cf_warning(AS_SINDEX, "btree allocation failure");
			return 0;
		}

		ai_arr_move_to_tree(arr, nbtr);
		ai_arr_destroy(anbtr->u.arr);

		// Update anbtr
		anbtr->u.nbtr = nbtr;
		anbtr->is_btree = true;

		ulong aa = nbtr->msize;
		return (aa - ba);
	}
	return 0;
}

/*
 *  return -1    in case of failure
 *          size of allocation in case of success
 */
static int
anbtr_check_init(ai_nbtr *anbtr, uchar pktyp)
{
	bool create_arr = false;
	bool create_nbtr = false;

	if (anbtr->is_btree) {
		if (anbtr->u.nbtr) {
			create_nbtr = false;
		} else {
			create_nbtr = true;
		}
	} else {
		if (anbtr->u.arr) {
			create_arr = false;
		} else {
			if (g_use_arr) {
				create_arr = true;
			} else {
				create_nbtr = true;
			}
		}
	}

	// create array or btree
	if (create_arr) {
		anbtr->u.arr = ai_arr_new();
		if (!anbtr->u.arr) {
			return -1;
		}
		return ai_arr_size(anbtr->u.arr);
	} else if (create_nbtr) {
		anbtr->u.nbtr = createIndexNode(pktyp, COL_TYPE_NONE);
		if (!anbtr->u.nbtr) {
			return -1;
		}
		anbtr->is_btree = true;
		return anbtr->u.nbtr->msize;
	} else {
		if (!anbtr->u.arr && !anbtr->u.nbtr) {
			cf_warning(AS_SINDEX, "Something wrong!!!");
			return -1;
		}
	}
	return 0;
}

/*
 * Insert operation for the nbtr does the following
 * 1. Sets up anbtr if it is set up
 * 2. Inserts in the arr or nbtr depending number of elements.
 * 3. Cuts over from arr to btr at AI_ARR_MAX_USED
 *
 * Parameter:   ibtr  : Btree of key
 *              acol  : Secondary index key
 *              apk   : value (primary key to be inserted)
 *              pktyp : value type (U160 currently)
 *
 * Returns:
 *      AS_SINDEX_OK        : In case of success
 *      AS_SINDEX_ERR       : In case of failure
 *      AS_SINDEX_KEY_FOUND : If key already exists
 */
static int
reduced_iAdd(bt *ibtr, ai_obj *acol, ai_obj *apk, uchar pktyp)
{
	ai_nbtr *anbtr = (ai_nbtr *)btIndFind(ibtr, acol);
	ulong ba = 0, aa = 0;
	bool allocated_anbtr = false;
	if (!anbtr) {
		anbtr = cf_malloc(sizeof(ai_nbtr));
		aa += sizeof(ai_nbtr);
		if (!anbtr) {
			cf_warning(AS_SINDEX, "Allocation failure for anbtr");
			return AS_SINDEX_ERR;
		}
		memset(anbtr, 0, sizeof(ai_nbtr));
		allocated_anbtr = true;
	}

	// Init the array
	int ret = anbtr_check_init(anbtr, pktyp);
	if (ret < 0) {
		if (allocated_anbtr) {
			cf_free(anbtr);
		}
		return AS_SINDEX_ERR;
	} else if (ret) {
		ibtr->nsize += ret;
		btIndAdd(ibtr, acol, (bt *)anbtr);
	}

	// Convert from arr to nbtr if limit is hit
	ibtr->nsize += anbtr_check_convert(anbtr, pktyp);

	// If already a btree use it
	if (anbtr->is_btree) {
		bt *nbtr = anbtr->u.nbtr;
		if (!nbtr) {
			return AS_SINDEX_ERR;
		}

		if (btIndNodeExist(nbtr, apk)) {
			return AS_SINDEX_KEY_FOUND;
		}

		ba += nbtr->msize;
		if (!btIndNodeAdd(nbtr, apk)) {
			return AS_SINDEX_ERR;
		}
		aa += nbtr->msize;

	} else {
		ai_arr *arr = anbtr->u.arr;
		if (!arr) {
			return AS_SINDEX_ERR;
		}

		ba += ai_arr_size(anbtr->u.arr);
		bool found = false;
		ai_arr *t_arr = ai_arr_insert(arr, (cf_digest *)&apk->y, &found);
		if (!t_arr) {
			return AS_SINDEX_ERR;
		} else if (found) {
			return AS_SINDEX_KEY_FOUND;
		}
		anbtr->u.arr = t_arr;
		aa += ai_arr_size(anbtr->u.arr);
	}
	ibtr->nsize += (aa - ba);  // ibtr inherits nbtr

	return AS_SINDEX_OK;
}

/*
 * Delete operation for the nbtr does the following. Delete in the arr or nbtr
 * based on state of anbtr
 *
 * Parameter:   ibtr  : Btree of key
 *              acol  : Secondary index key
 *              apk   : value (primary key to be inserted)
 *
 * Returns:
 *      AS_SINDEX_OK           : In case of success
 *      AS_SINDEX_ERR          : In case of failure
 *      AS_SINDEX_KEY_NOTFOUND : If key does not exist
 */
static int
reduced_iRem(bt *ibtr, ai_obj *acol, ai_obj *apk)
{
	ai_nbtr *anbtr = (ai_nbtr *)btIndFind(ibtr, acol);
	ulong ba = 0, aa = 0;
	if (!anbtr) {
		return AS_SINDEX_ERR;
	}
	if (anbtr->is_btree) {
		if (!anbtr->u.nbtr) return AS_SINDEX_ERR;

		// Remove from nbtr if found
		bt *nbtr = anbtr->u.nbtr;
		if (!btIndNodeExist(nbtr, apk)) {
			return AS_SINDEX_KEY_NOTFOUND;
		}
		ba = nbtr->msize;
		int nkeys = btIndNodeDelete(nbtr, apk, NULL);
		aa = nbtr->msize;

		// remove from ibtr
		if (!nkeys) {
			btIndDelete(ibtr, acol);
			aa = 0;
			bt_destroy(nbtr);
			ba += sizeof(ai_nbtr);
			cf_free(anbtr);
		}
	} else {
		if (!anbtr->u.arr) return AS_SINDEX_ERR;

		// Remove from arr if found
		bool notfound = false;
		ba = ai_arr_size(anbtr->u.arr);
		anbtr->u.arr = ai_arr_delete(anbtr->u.arr, (cf_digest *)&apk->y, &notfound);
		if (notfound) return AS_SINDEX_KEY_NOTFOUND;
		aa = ai_arr_size(anbtr->u.arr);

		// Remove from ibtr
		if (anbtr->u.arr->used == 0) {
			btIndDelete(ibtr, acol);
			aa = 0;
			ai_arr_destroy(anbtr->u.arr);
			ba += sizeof(ai_nbtr);
			cf_free(anbtr);
		}
	}
	ibtr->nsize -= (ba - aa);

	return AS_SINDEX_OK;
}

static char *
str_concat(char *first, char separator, char *second)
{
	char *str;
	size_t str_len = strlen(first) + strlen(second) + 2;

	if (!(str = cf_malloc(str_len))) {
		return NULL;
	}

	if (0 > snprintf(str, str_len, "%s%c%s", first, separator, second)) {
		cf_free(str);
		return NULL;
	}

	return str;
}

static char *
create_tname(char *ns_name, char *set)
{
	return str_concat(ns_name, '.', (set ? set : ""));
}

static char *
create_tname_from_imd(const as_sindex_metadata *imd)
{
	return create_tname(imd->ns_name, imd->set);
}

static char *
create_cname(char *bin_path, int bin_type, int index_type)
{
	int type_str_size = AS_SINDEX_KTYPE_MAX_TO_STR_SZ + 1 + AS_SINDEX_ITYPE_MAX_TO_STR_SZ;
	char type_str[type_str_size];
	bzero(type_str, type_str_size);
	if (0 > snprintf(type_str, sizeof(type_str), "%d_%d", bin_type, index_type)) {
		return NULL;
	}
	// TODO : CHECK SIZE
	return str_concat(bin_path, '_', type_str);
}

static char *
create_cname_from_imd(const as_sindex_metadata *imd) {
	return create_cname(imd->path_str, imd->btype, imd->itype);
}

static char *
get_iname(char *ns_name, char *iname)
{
	return str_concat(ns_name, '.', iname);
}

static char *
get_iname_from_imd(const as_sindex_metadata *imd)
{
	return get_iname(imd->ns_name, imd->iname);
}

int
ai_btree_key_hash_from_sbin(as_sindex_metadata *imd, as_sindex_bin_data *b)
{
	uint64_t u;

	if (C_IS_Y(imd->dtype)) {
		char *x = (char *) &b->digest; // x += 4;
		u = ((* (uint128 *) x) % imd->nprts);
	} else {
		u = (((uint64_t) b->u.i64) % imd->nprts);
	}

	return (int) u;
}

int
ai_btree_key_hash(as_sindex_metadata *imd, void *skey)
{
	uint64_t u;

	if (C_IS_Y(imd->dtype)) {
		char *x = (char *) ((cf_digest *)skey); // x += 4;
		u = ((* (uint128 *) x) % imd->nprts);
	} else {
		u = ((*(uint64_t*)skey) % imd->nprts);
	}

	return (int) u;
}

int
ai_findandset_imatch(as_sindex_metadata *imd, as_sindex_pmetadata *pimd, int idx)
{
	if (!Num_tbls) {
		return AS_SINDEX_ERR;
	}

	char *tname = NULL;
	char *cname = NULL;
	char *iname = NULL;

	if (!(tname = create_tname_from_imd(imd))) {
		return AS_SINDEX_ERR_NO_MEMORY;
	}

	int ret = AS_SINDEX_ERR;

	AI_GRLOCK();

	int tmatch = find_table(tname);
	if (tmatch == -1) {
		goto END;
	}
	if (imd->iname) {
		// This is always true
		if (!(iname = get_iname_from_imd(imd))) {
			ret = AS_SINDEX_ERR_NO_MEMORY;
			goto END;
		}
		
		char piname[INDD_HASH_KEY_SIZE];
		snprintf(piname, sizeof(piname), "%s.%d", iname, idx);	
		pimd->imatch = match_partial_index_name(piname);
	} else {
		// CAUTION : This will not work. Since ci->list is only populated for 0th pimd
		if (!(cname = create_cname_from_imd(imd))) {
			ret = AS_SINDEX_ERR_NO_MEMORY;
			goto END;
		}
		icol_t *ic = find_column(tmatch, cname);
		if (!ic) {
			goto END;
		}
		pimd->imatch = find_partial_index(tmatch, ic);
		cf_free(ic);
	}
	if (pimd->imatch == -1) {
		cf_debug(AS_SINDEX, "Index %s not found for %dth pimd", imd->iname, idx);
		goto END;
	}

	ret = AS_SINDEX_OK;

END:

	AI_UNLOCK();

	if (tname) {
		cf_free(tname);
	}
	if (iname) { 
		cf_free(iname);
	}
	if (cname) {
		cf_free(cname);
	}
	return ret;
}

/*
 * Return 0  in case of success
 *        -1 in case of failure
 */
static int
btree_addsinglerec(as_sindex_metadata *imd, ai_obj * key, cf_digest *dig, cf_ll *recl, uint64_t *n_bdigs, 
								bool * can_partition_query, bool partitions_pre_reserved)
{
	// The digests which belongs to one of the query-able partitions are elligible to go into recl
	as_partition_id pid =  as_partition_getid(*dig);
	as_namespace * ns = imd->si->ns;
	if (partitions_pre_reserved) {
		if (!can_partition_query[pid]) {
			return 0;
		}
	}
	else {
		if (! client_replica_maps_is_partition_queryable(ns, pid)) {
			return 0;
		}
	}

	bool create                     = (cf_ll_size(recl) == 0) ? true : false;
	as_index_keys_arr * keys_arr    = NULL;
	if (!create) {
		cf_ll_element * ele         = cf_ll_get_tail(recl);
		keys_arr                    = ((as_index_keys_ll_element*)ele)->keys_arr;
		if (keys_arr->num == AS_INDEX_KEYS_PER_ARR) {
			create = true;
		}
	}
	if (create) {
		keys_arr                    = as_index_get_keys_arr();
		if (!keys_arr) {
			cf_warning(AS_SINDEX, "Fail to allocate sindex key value array");
			return -1;
		}
		as_index_keys_ll_element * node =  cf_malloc(sizeof(as_index_keys_ll_element));
		node->keys_arr                  = keys_arr;
		cf_ll_append(recl, (cf_ll_element *)node);
	}
	// Copy the digest (value)
	memcpy(&keys_arr->pindex_digs[keys_arr->num], dig, CF_DIGEST_KEY_SZ);

	// Copy the key
	if (C_IS_Y(imd->dtype)) {
		memcpy(&keys_arr->sindex_keys[keys_arr->num].key.str_key, &key->y, CF_DIGEST_KEY_SZ);
	}
	else {
		keys_arr->sindex_keys[keys_arr->num].key.int_key = key->l;
	}

	keys_arr->num++;
	*n_bdigs = *n_bdigs + 1;
	return 0;
}

/*
 * Return 0 in case of success
 *       -1 in case of failure
 */
static int
add_recs_from_nbtr(as_sindex_metadata *imd, ai_obj *ikey, bt *nbtr, as_sindex_qctx *qctx, bool fullrng)
{
	int ret = 0;
	ai_obj sfk, efk;
	init_ai_obj(&sfk);
	init_ai_obj(&efk);
	btSIter *nbi;
	btEntry *nbe;
	btSIter stack_nbi;

	if (fullrng) {
		nbi = btSetFullRangeIter(&stack_nbi, nbtr, 1, NULL);
	} else { // search from LAST batches end-point
		init_ai_objFromDigest(&sfk, &qctx->bdig);
		assignMaxKey(nbtr, &efk);
		nbi = btSetRangeIter(&stack_nbi, nbtr, &sfk, &efk, 1);
	}
 	if (nbi) {
		while ((nbe = btRangeNext(nbi, 1))) {
			ai_obj *akey = nbe->key;
			// FIRST can be REPEAT (last batch)
			if (!fullrng && ai_objEQ(&sfk, akey)) {
				continue;
			}
			if (btree_addsinglerec(imd, ikey, (cf_digest *)&akey->y, qctx->recl, &qctx->n_bdigs,
									qctx->can_partition_query, qctx->partitions_pre_reserved)) {
				ret = -1;
				break;
			}
			if (qctx->n_bdigs == qctx->bsize) {
				if (ikey) {
					ai_objClone(qctx->bkey, ikey);
				}
				cloneDigestFromai_obj(&qctx->bdig, akey);
				break;
			}
		}
		btReleaseRangeIterator(nbi);
	} else {
		cf_warning(AS_QUERY, "Could not find nbtr iterator.. skipping !!");
	}
	return ret;
}

static int
add_recs_from_arr(as_sindex_metadata *imd, ai_obj *ikey, ai_arr *arr, as_sindex_qctx *qctx)
{
	bool ret = 0;

	for (int i = 0; i < arr->used; i++) {
		if (btree_addsinglerec(imd, ikey, (cf_digest *)&arr->data[i * CF_DIGEST_KEY_SZ], qctx->recl, 
					&qctx->n_bdigs, qctx->can_partition_query, qctx->partitions_pre_reserved)) {
			ret = -1;
			break;
		}
		// do not break on hitting batch limit, if the tree converts to
		// bt from arr, there is no way to know which digest were already
		// returned when attempting subsequent batch. Return the entire
		// thing.
	}
	// mark nbtr as finished and copy the offset
	qctx->nbtr_done = true;
	if (ikey) {
		ai_objClone(qctx->bkey, ikey);
	}

	return ret;
}

/*
 * Return 0  in case of success
 *        -1 in case of failure
 */
static int
get_recl(as_sindex_metadata *imd, ai_obj *afk, as_sindex_qctx *qctx)
{
	as_sindex_pmetadata *pimd = &imd->pimd[qctx->pimd_idx];
	ai_nbtr *anbtr = (ai_nbtr *)btIndFind(pimd->ibtr, afk);

	if (!anbtr) {
		return 0;
	}

	if (anbtr->is_btree) {
		if (add_recs_from_nbtr(imd, afk, anbtr->u.nbtr, qctx, qctx->new_ibtr)) {
			return -1;
		}
	} else {
		// If already entire batch is returned
		if (qctx->nbtr_done) {
			return 0;
		}
		if (add_recs_from_arr(imd, afk, anbtr->u.arr, qctx)) {
			return -1;
		}
	}
	return 0;
}

/*
 * Return 0  in case of success
 *        -1 in case of failure
 */
static int
get_numeric_range_recl(as_sindex_metadata *imd, uint64_t begk, uint64_t endk, as_sindex_qctx *qctx)
{
	ai_obj sfk;
	init_ai_objLong(&sfk, qctx->new_ibtr ? begk : qctx->bkey->l);
	ai_obj efk;
	init_ai_objLong(&efk, endk);
	as_sindex_pmetadata *pimd = &imd->pimd[qctx->pimd_idx];
	bool fullrng              = qctx->new_ibtr;
	int ret                   = 0;
	btSIter *bi               = btGetRangeIter(pimd->ibtr, &sfk, &efk, 1);
	btEntry *be;

	if (bi) {
		while ((be = btRangeNext(bi, 1))) {
			ai_obj  *ikey  = be->key;
			ai_nbtr *anbtr = be->val;

			if (!anbtr) {
				ret = -1;
				break;
			}

			// figure out nbtr to deal with. If the key which was
			// used last time vanishes work with next key. If the
			// key exist but 'last' entry made to list in the last
			// iteration; Move to next nbtr
			if (!fullrng) {
				if (!ai_objEQ(&sfk, ikey)) {
					fullrng = 1; // bkey disappeared
				} else if (qctx->nbtr_done) {
					qctx->nbtr_done = false;
					// If we are moving to the next key, we need 
					// to search the full range.
					fullrng = 1;
					continue;
				}
			}

			if (anbtr->is_btree) {
				if (add_recs_from_nbtr(imd, ikey, anbtr->u.nbtr, qctx, fullrng)) {
					ret = -1;
					break;
				}
			} else {
				if (add_recs_from_arr(imd, ikey, anbtr->u.arr, qctx)) {
					ret = -1;
					break;
				}
			}

			// Since add_recs_from_arr() returns entire thing and do not support the batch limit,
			// >= operator is needed here.
			if (qctx->n_bdigs >= qctx->bsize) {
				break;
			}

			// If it reaches here, this means last key could not fill the batch.
			// So if we are to start a new key, search should be done on full range 
			// and the new nbtr is obviously not done.
			fullrng         = 1;
			qctx->nbtr_done = false;
		}
		btReleaseRangeIterator(bi);
	}
	return ret;
}

int
ai_btree_query(as_sindex_metadata *imd, as_sindex_range *srange, as_sindex_qctx *qctx)
{
	bool err = 1;
	if (!srange->isrange) { // EQUALITY LOOKUP
		ai_obj afk;
		init_ai_obj(&afk);
		if (C_IS_Y(imd->dtype)) {
			init_ai_objFromDigest(&afk, &srange->start.digest);
		}
		else {
			init_ai_objLong(&afk, srange->start.u.i64);
		}
		err = get_recl(imd, &afk, qctx);
	} else {                // RANGE LOOKUP
		err = get_numeric_range_recl(imd, srange->start.u.i64, srange->end.u.i64, qctx);
	}
	return (err ? AS_SINDEX_ERR_NO_MEMORY :
			(qctx->n_bdigs >= qctx->bsize) ? AS_SINDEX_CONTINUE : AS_SINDEX_OK);
}

int
ai_btree_put(as_sindex_metadata *imd, as_sindex_pmetadata *pimd, void *skey, cf_digest *value)
{
	int ret = AS_SINDEX_OK;

	ai_obj ncol;
	if (C_IS_Y(imd->dtype)) {
		init_ai_objFromDigest(&ncol, (cf_digest*)skey);
	}
	else {
		init_ai_objLong(&ncol, *(ulong *)skey);
	}

	ai_obj apk;
	init_ai_objFromDigest(&apk, value);


	ulong bb = pimd->ibtr->msize + pimd->ibtr->nsize;
	ret = reduced_iAdd(pimd->ibtr, &ncol, &apk, COL_TYPE_U160);
	if (ret == AS_SINDEX_KEY_FOUND) {
		goto END;
	} else if (ret != AS_SINDEX_OK) {
		cf_warning(AS_SINDEX, "Insert into the btree failed");
		ret = AS_SINDEX_ERR_NO_MEMORY;
		goto END;
	}
	ulong ab = pimd->ibtr->msize + pimd->ibtr->nsize;
	if (!as_sindex_reserve_data_memory(imd, (ab - bb))) {
		reduced_iRem(pimd->ibtr, &ncol, &apk);
		ret = AS_SINDEX_ERR_NO_MEMORY;
		goto END;
	}

END:

	return ret;
}

int
ai_btree_delete(as_sindex_metadata *imd, as_sindex_pmetadata *pimd, void * skey, cf_digest * value)
{
	int ret = AS_SINDEX_OK;

	if (!pimd->ibtr) {
		return AS_SINDEX_KEY_NOTFOUND;
	}

	ai_obj ncol;
	if (C_IS_Y(imd->dtype)) {
		init_ai_objFromDigest(&ncol, (cf_digest *)skey);
	}
	else {
		init_ai_objLong(&ncol, *(ulong *)skey);
	}

	ai_obj apk;
	init_ai_objFromDigest(&apk, value);
	ulong bb = pimd->ibtr->msize + pimd->ibtr->nsize;
	ret = reduced_iRem(pimd->ibtr, &ncol, &apk);
	ulong ab = pimd->ibtr->msize + pimd->ibtr->nsize;
	as_sindex_release_data_memory(imd, (bb - ab));
	return ret;
}

/*
 * Internal function which adds digests to the defrag_list
 * Mallocs the nodes of defrag_list
 * Returns :
 *      -1 : Error
 *      number of digests found : success
 *
 */
static long
build_defrag_list_from_nbtr(as_namespace *ns, ai_obj *acol, bt *nbtr, long nofst, long *limit, uint64_t * tot_found, cf_ll *gc_list)
{
	int error = -1;
	btEntry *nbe;
	// STEP 1: go thru a portion of the nbtr and find to-be-deleted-PKs
	// TODO: a range query may be smarter then using the Xth Iterator
	btSIter *nbi = (nofst ? btGetFullXthIter(nbtr, nofst, 1, NULL, 0) :
					btGetFullRangeIter(nbtr, 1, NULL));
	if (!nbi) {
		return error;
	}

	long      found             = 0;
	long  processed             = 0;
	uint64_t validation_time_ns = 0;
	while ((nbe = btRangeNext(nbi, 1))) {
		ai_obj *akey = nbe->key;
		// STEP 2: if this PK is to be deleted then add it to PKtoDeleteList
		SET_TIME_FOR_SINDEX_GC_HIST(validation_time_ns);
		int ret = as_sindex_can_defrag_record(ns, (cf_digest *) (&akey->y));
		SINDEX_GC_HIST_INSERT_DATA_POINT(sindex_gc_validate_obj_hist, validation_time_ns);
		validation_time_ns = 0; 

		if (ret == AS_SINDEX_GC_SKIP_ITERATION) {
			*limit = 0;
			break;
		} else if (ret == AS_SINDEX_GC_OK){

			bool create   = (cf_ll_size(gc_list) == 0) ? true : false;
			objs_to_defrag_arr *dt;

			if (!create) {
				cf_ll_element * ele = cf_ll_get_tail(gc_list);
				dt = ((ll_sindex_gc_element*)ele)->objs_to_defrag;
				if (dt->num == SINDEX_GC_NUM_OBJS_PER_ARR) {
					create = true;
				}
			}
			if (create) {
				dt = as_sindex_gc_get_defrag_arr();
				if (!dt) {
					*tot_found += found;
					return -1;
				}
				ll_sindex_gc_element  * node;
				node = cf_malloc(sizeof(ll_sindex_gc_element));
				node->objs_to_defrag = dt;
				cf_ll_append(gc_list, (cf_ll_element *)node);
			}
			cloneDigestFromai_obj(&(dt->acol_digs[dt->num].dig), akey);
			ai_objClone(&(dt->acol_digs[dt->num].acol), acol);

			dt->num += 1;		
			found++;
		}
		processed++;
		(*limit)--;
		if (*limit == 0) break;
	}
	btReleaseRangeIterator(nbi);
	*tot_found += found; 
	return processed;
}

static long
build_defrag_list_from_arr(as_namespace *ns, ai_obj *acol, ai_arr *arr, long nofst, long *limit, uint64_t * tot_found, cf_ll *gc_list)
{
	long     found              = 0;
	long     processed          = 0;
	uint64_t validation_time_ns = 0;
	for (int i = nofst; i < arr->used; i++) {
		SET_TIME_FOR_SINDEX_GC_HIST(validation_time_ns);	
		int ret = as_sindex_can_defrag_record(ns, (cf_digest *) &arr->data[i * CF_DIGEST_KEY_SZ]);
		SINDEX_GC_HIST_INSERT_DATA_POINT(sindex_gc_validate_obj_hist, validation_time_ns);
		validation_time_ns = 0;
		if (ret == AS_SINDEX_GC_SKIP_ITERATION) {
			*limit = 0;
			break;
		} else if (ret == AS_SINDEX_GC_OK) {
			bool create   = (cf_ll_size(gc_list) == 0) ? true : false;
			objs_to_defrag_arr *dt;

			if (!create) {
				cf_ll_element * ele = cf_ll_get_tail(gc_list);
				dt = ((ll_sindex_gc_element*)ele)->objs_to_defrag;
				if (dt->num == SINDEX_GC_NUM_OBJS_PER_ARR) {
					create = true;
				}
			}
			if (create) {
				dt = as_sindex_gc_get_defrag_arr();
				if (!dt) {
					*tot_found += found;
					return -1;
				}
				ll_sindex_gc_element  * node;
				node = cf_malloc(sizeof(ll_sindex_gc_element));
				node->objs_to_defrag = dt;
				cf_ll_append(gc_list, (cf_ll_element *)node);
			}
			memcpy(&(dt->acol_digs[dt->num].dig), (cf_digest *) &arr->data[i * CF_DIGEST_KEY_SZ], CF_DIGEST_KEY_SZ);	
			ai_objClone(&(dt->acol_digs[dt->num].acol), acol);

			dt->num += 1;		
			found++;
		}
		processed++;
		(*limit)--;
		if (*limit == 0) {
			break;
		}
	}
	*tot_found += found; 
	return processed;
}

/*
 * Aerospike Index interface to build a defrag_list.
 *
 * Returns :
 *  AS_SINDEX_DONE     ---> The current pimd has been scanned completely for defragging
 *  AS_SINDEX_CONTINUE ---> Current pimd sill may have some candidate digest to be defragged
 *  AS_SINDEX_ERR      ---> Error. Abort this pimd.
 *
 *  Notes :  Caller has the responsibility to free the iterators.
 *           Requires a proper offset value from the caller.
 */
int
ai_btree_build_defrag_list(as_sindex_metadata *imd, as_sindex_pmetadata *pimd, ai_obj *icol,
						   long *nofst, long limit, uint64_t * tot_processed, uint64_t * tot_found, cf_ll *gc_list)
{
	int ret = AS_SINDEX_ERR;

	if (!pimd || !imd) {
		return ret;
	}

	as_namespace *ns = imd->si->ns;
	if (!ns) {
		ns = as_namespace_get_byname((char *)imd->ns_name);
	}
	char *iname = get_iname_from_imd(imd);
	if (!iname) {
		ret = AS_SINDEX_ERR_NO_MEMORY;
		return ret;
	}
	if (!pimd || !pimd->ibtr || !pimd->ibtr->numkeys) {
		goto END;
	}
	//Entry is range query, FROM previous icol TO maxKey(ibtr)
	if (icol->empty) {
		assignMinKey(pimd->ibtr, icol); // init first call
	}
	ai_obj iH;
	assignMaxKey(pimd->ibtr, &iH);
	btEntry *be = NULL;
	btSIter *bi = btGetRangeIter(pimd->ibtr, icol, &iH, 1);
	if (!bi) {
		goto END;
	}

	while ( true ) {
		be = btRangeNext(bi, 1);
		if (!be) {
			ret = AS_SINDEX_DONE;
			break;
		}
		ai_obj *acol = be->key;
		ai_nbtr *anbtr = be->val;
		long processed = 0;
		if (!anbtr) {
			break;
		}
		if (anbtr->is_btree) {
			processed = build_defrag_list_from_nbtr(ns, acol, anbtr->u.nbtr, *nofst, &limit, tot_found, gc_list);
		} else {
			processed = build_defrag_list_from_arr(ns, acol, anbtr->u.arr, *nofst, &limit, tot_found, gc_list);
		}

		if (processed < 0) {    // error .. abort everything.
			cf_detail(AS_SINDEX, "build_defrag_list returns an error. Aborting defrag on current pimd");
			ret = AS_SINDEX_ERR;
			break;
		}
		*tot_processed += processed;
		// This tree may have some more digest to defrag
		if (limit == 0) {
			*nofst = *nofst + processed;
			ai_objClone(icol, acol);
			cf_detail(AS_SINDEX, "Current pimd may need more iteration of defragging.");
			ret = AS_SINDEX_CONTINUE;
			break;
		}

		// We have finished this tree. Yet we have not reached our limit to defrag.
		// Goes to next iteration
		*nofst = 0;
		ai_objClone(icol, acol);
	};
	btReleaseRangeIterator(bi);
END:
	cf_free(iname);

	return ret;
}

/*
 * Deletes the digest as in the passed in as gc_list, bound by n2del number of
 * elements per iteration, with *deleted successful deletes.
 */
bool
ai_btree_defrag_list(as_sindex_metadata *imd, as_sindex_pmetadata *pimd, cf_ll *gc_list, ulong n2del, ulong *deleted)
{
	// If n2del is zero here, that means caller do not want to defrag
	if (n2del == 0 ) {
		return false;
	}
	ulong success = 0;
	as_namespace *ns = imd->si->ns;
	// STEP 3: go thru the PKtoDeleteList and delete the keys
	ulong bb = pimd->ibtr->msize + pimd->ibtr->nsize;
	uint64_t validation_time_ns = 0;
	uint64_t deletion_time_ns   = 0;
	while (cf_ll_size(gc_list)) {
		cf_ll_element        * ele  = cf_ll_get_head(gc_list);
		ll_sindex_gc_element * node = (ll_sindex_gc_element * )ele;
		objs_to_defrag_arr   * dt   = node->objs_to_defrag;

		// check before deleting. The digest may re-appear after the list
		// creation and before deletion from the secondary index

		int i = 0;
		while (dt->num != 0) {
			i = dt->num - 1;
			SET_TIME_FOR_SINDEX_GC_HIST(validation_time_ns);
			int ret = as_sindex_can_defrag_record(ns, &(dt->acol_digs[i].dig));
			SINDEX_GC_HIST_INSERT_DATA_POINT(sindex_gc_validate_obj_hist, validation_time_ns);
			validation_time_ns = 0;
			if (ret == AS_SINDEX_GC_SKIP_ITERATION) {
				goto END;
			} else if (ret == AS_SINDEX_GC_OK) {
				ai_obj           apk;
				init_ai_objFromDigest(&apk, &(dt->acol_digs[i].dig));
				ai_obj          *acol = &(dt->acol_digs[i].acol);
				cf_detail(AS_SINDEX, "Defragged %lu %ld", acol->l, *((uint64_t *)&apk.y));
				
				SET_TIME_FOR_SINDEX_GC_HIST(deletion_time_ns);
				if (reduced_iRem(pimd->ibtr, acol, &apk) == AS_SINDEX_OK) {
					success++;
					SINDEX_GC_HIST_INSERT_DATA_POINT(sindex_gc_delete_obj_hist, deletion_time_ns);
				}
				deletion_time_ns = 0;
			}
			dt->num -= 1;
			n2del--;
			if (n2del == 0) {
				goto END;
			}
		}
		cf_ll_delete(gc_list, (cf_ll_element*)node);
	}

END:
	as_sindex_release_data_memory(imd, (bb -  pimd->ibtr->msize - pimd->ibtr->nsize));
	*deleted += success;
	return cf_ll_size(gc_list) ? true : false;
}

/* NOTE: The creation of a secondary index is the following two commands
          0.) optional: CREATE TABLE namespace (pk U160, __dummy TEXT)
          1.) ALTER TABLE namespace ADD COLUMN binname columntype
          2.) CREATE [UNIQUE] INDEX indexname ON namespace (binname)
 */
int
ai_btree_create(as_sindex_metadata *imd, int simatch, int *bimatch, int nprts)
{
	char *iname = NULL, *cname = NULL, *tname = NULL;
	int ret = AS_SINDEX_ERR, rv;

	if (!(tname = create_tname_from_imd(imd))) {
		return AS_SINDEX_ERR_NO_MEMORY;
	}

	if (!(cname = create_cname_from_imd(imd))) {
		if (tname) {
			cf_free(tname);
		}
		return AS_SINDEX_ERR_NO_MEMORY;
	}

	AI_GWLOCK();

	// TODO : ai_create_table has this check. So this is redundant
	// 3 shash_get can be reduced to 1 through ai_get_or_create_table func
	int tmatch = find_table(tname);
	if (tmatch == -1) {
		if (0 > (rv = ai_create_table(tname))) {
			cf_warning(AS_SINDEX, "Create table %s failed (rv %d)", tname, rv);
			goto END;
		}
		tmatch = find_table(tname);
	}
	r_tbl_t *rt = &Tbl[tmatch];

	// 1.) add entries in Aerospike Index's virtual TABLE
	int col_type = imd->btype;
	icol_t *ic = find_column(tmatch, cname);
	if (!ic) { // COLUMN does not exist
		if (0 > ai_add_column(tname, cname, col_type)) {
			goto END;
		}
		// Add (cmatch+1) always non-zero
		cf_debug(AS_SINDEX, "Added Mapping [BINNAME=%s: BINID=%d: COLID%d] [IMATCH=%d: SIMATCH=%d: INAME=%s]",
				imd->bname, imd->binid, rt->col_count, Num_indx - 1, simatch, imd->iname);
	}
	else {
		cf_free(ic);
	}

	//NOTE: COMMAND: CREATE PARTITIONED INDEX iname ON tname (cname) NUM = nprts
	if (!(iname = get_iname_from_imd(imd))) {
		ret = AS_SINDEX_ERR_NO_MEMORY;
		goto END;
	}

	if (0 > (rv = ai_create_index(iname, tname, cname, col_type, nprts))) {
		cf_warning(AS_SINDEX, "Create index %s failed (rv %d)", iname, rv);
		goto END;
	}

	*bimatch = match_partial_index_name(iname);
	cf_debug(AS_SINDEX, "cr8SecIndex: iname: %s bname: %s type: %d ns: %s set: %s tmatch: %d bimatch: %d",
		   imd->iname, imd->bname, imd->btype, imd->ns_name, imd->set, tmatch, *bimatch);

	ret = AS_SINDEX_OK;

END:

	AI_UNLOCK();

	if (tname) {
		cf_free(tname);
	}
	if (cname) {
		cf_free(cname);
	}
	if (iname) {
		cf_free(iname);
	}
	return ret;
}

int
ai_post_index_creation_setup_pmetadata(as_sindex_metadata *imd, as_sindex_pmetadata *pimd, int simatch, int idx)
{
	if (idx == 0) {
		pimd->imatch = imd->bimatch;
	} else if (AS_SINDEX_OK != ai_findandset_imatch(imd, pimd, idx)) {
		return AS_SINDEX_ERR;
	}

	r_ind_t *ri = &Index[pimd->imatch];
	ri->simatch = simatch; //ref for simatch to enable search through Aerospike Index
	ri->done = true;
	if (idx == 0) { // idx of 0 means fill these in
		imd->dtype = ri->dtype;
		imd->btype = ri->dtype;
	}
	pimd->tmatch = ri->tmatch;
	pimd->ibtr = ri->btr;

	return AS_SINDEX_OK;
}

int
ai_btree_destroy(as_sindex_metadata *imd)
{
	char *tname, *cname, *iname;

	AI_GWLOCK();


	if (!(tname = create_tname_from_imd(imd))) {
		return AS_SINDEX_ERR_NO_MEMORY;
	}

	if (!(cname = create_cname_from_imd(imd))) {
		return AS_SINDEX_ERR_NO_MEMORY;
	}

	if (0 > ai_drop_column(tname, cname)) {
		cf_warning(AS_SINDEX, "Failed to drop column %s from table %s", cname, tname);
	}

	cf_free(tname);
	cf_free(cname);

	if (!(iname = get_iname_from_imd(imd))) {
		return AS_SINDEX_ERR_NO_MEMORY;
	}

	if (0 > ai_drop_index(iname)) {
		cf_warning(AS_SINDEX, "Failed to drop index %s", iname);
	}

	cf_free(iname);

	AI_UNLOCK();

	return AS_SINDEX_OK;
}

int
ai_btree_dump(char *ns_name, char *setname, char *filename)
{
	char *tname;

	if (!(tname = create_tname(ns_name, setname))) {
		return -1;
	}

	AI_GRLOCK();

	int retval = dump_btree(tname, (filename ? filename : DEFAULT_BTREE_DUMP_FILENAME));

	AI_UNLOCK();

	cf_free(tname);

	return retval;
}

// Returns AS_SINDEX_ERR in case of failure
uint64_t
ai_btree_get_numkeys(as_sindex_metadata *imd)
{
	uint64_t val = 0;
	if ((!imd->ns_name)) {
		return AS_SINDEX_ERR;
	}

	for (int i = 0; i < imd->nprts; i++) {
		val += imd->pimd[i].ibtr->numkeys;
	}

	return val;
}

// Returns AS_SINDEX_ERR in case of failure
uint64_t
ai_btree_get_isize(as_sindex_metadata *imd)
{
	uint64_t size = 0;
	if ((!imd->ns_name)) {
		return AS_SINDEX_ERR;
	}

	for (int i = 0; i < imd->nprts; i++) {
		if (imd->pimd[i].ibtr->msize > 0) {
			size += imd->pimd[i].ibtr->msize;
		}
	}

	return size;
}

// Returns AS_SINDEX_ERR in case of failure
uint64_t
ai_btree_get_nsize(as_sindex_metadata *imd)
{
	uint64_t size = 0;
	if ((!imd->ns_name)) {
		return AS_SINDEX_ERR;
	}

	for (int i = 0; i < imd->nprts; i++) {
		if (imd->pimd[i].ibtr->nsize > 0) {
			size += imd->pimd[i].ibtr->nsize;
		}
	}

	return size;
}

void
ai_btree_reinit_pimd(as_sindex_pmetadata * pimd)
{
	if(!pimd->ibtr)	{
		cf_crash(AS_SINDEX, "IBTR is null");
	}

	r_ind_t *ri = &Index[pimd->imatch];
	ri->btr = createIndexBT(ri->dtype, pimd->imatch);
	pimd->ibtr = ri->btr;
}

void
ai_btree_delete_ibtr(bt * ibtr, int imatch)
{
	if(!ibtr)	{
		cf_crash(AS_SINDEX, "IBTR is null");
	}

	ai_destroy_index(ibtr, imatch);	
}
