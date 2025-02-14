// SPDX-License-Identifier: GPL-2.0-only WITH linking exception
#include <memory>
#include <gromox/defs.h>
#include <gromox/mapidefs.h>
#include "attachment_object.h"
#include <gromox/proptag_array.hpp>
#include "exmdb_client.h"
#include "store_object.h"
#include "common_util.h"
#include <gromox/rop_util.hpp>
#include <cstdlib>
#include <cstring>

std::unique_ptr<ATTACHMENT_OBJECT> attachment_object_create(
	MESSAGE_OBJECT *pparent, uint32_t attachment_num)
{
	std::unique_ptr<ATTACHMENT_OBJECT> pattachment;
	try {
		pattachment = std::make_unique<ATTACHMENT_OBJECT>();
	} catch (const std::bad_alloc &) {
		return NULL;
	}
	pattachment->pparent = pparent;
	pattachment->b_writable = pparent->b_writable;
	if (ATTACHMENT_NUM_INVALID == attachment_num) {
		if (!exmdb_client::create_attachment_instance(pparent->pstore->get_dir(),
		    pparent->instance_id, &pattachment->instance_id,
		    &pattachment->attachment_num))
			return NULL;
		if (0 == pattachment->instance_id &&
			ATTACHMENT_NUM_INVALID != pattachment->attachment_num) {
			return NULL;	
		}
		pattachment->b_new = TRUE;
	} else {
		if (!exmdb_client::load_attachment_instance(pparent->pstore->get_dir(),
		    pparent->instance_id, attachment_num, &pattachment->instance_id))
			return NULL;
		pattachment->attachment_num = attachment_num;
	}
	return pattachment;
}

BOOL ATTACHMENT_OBJECT::init_attachment()
{
	auto pattachment = this;
	void *pvalue;
	PROBLEM_ARRAY problems;
	TPROPVAL_ARRAY propvals;
	
	if (!pattachment->b_new)
		return FALSE;
	propvals.count = 0;
	propvals.ppropval = cu_alloc<TAGGED_PROPVAL>(5);
	if (NULL == propvals.ppropval) {
		return FALSE;
	}
	
	propvals.ppropval[propvals.count].proptag =
							PROP_TAG_ATTACHNUMBER;
	propvals.ppropval[propvals.count].pvalue =
					&pattachment->attachment_num;
	propvals.count ++;
	
	propvals.ppropval[propvals.count].proptag =
					PROP_TAG_RENDERINGPOSITION;
	propvals.ppropval[propvals.count].pvalue = cu_alloc<uint32_t>();
	if (NULL == propvals.ppropval[propvals.count].pvalue) {
		return FALSE;
	}
	*(uint32_t*)propvals.ppropval[propvals.count].pvalue =
												0xFFFFFFFF;
	propvals.count ++;
	
	pvalue = cu_alloc<uint64_t>();
	if (NULL == pvalue) {
		return FALSE;
	}
	*(uint64_t*)pvalue = rop_util_current_nttime();
	
	propvals.ppropval[propvals.count].proptag = PR_CREATION_TIME;
	propvals.ppropval[propvals.count].pvalue = pvalue;
	propvals.count ++;
	propvals.ppropval[propvals.count].proptag = PR_LAST_MODIFICATION_TIME;
	propvals.ppropval[propvals.count].pvalue = pvalue;
	propvals.count ++;
	
	return exmdb_client::set_instance_properties(pattachment->pparent->pstore->get_dir(),
	       pattachment->instance_id, &propvals, &problems);
}

ATTACHMENT_OBJECT::~ATTACHMENT_OBJECT()
{
	auto pattachment = this;
	if (0 != pattachment->instance_id) {
		exmdb_client::unload_instance(pattachment->pparent->pstore->get_dir(),
			pattachment->instance_id);
	}
}

gxerr_t ATTACHMENT_OBJECT::save()
{
	auto pattachment = this;
	uint64_t nt_time;
	TAGGED_PROPVAL tmp_propval;
	TPROPVAL_ARRAY tmp_propvals;
	
	if (FALSE == pattachment->b_writable ||
		FALSE == pattachment->b_touched) {
		return GXERR_SUCCESS;
	}
	tmp_propvals.count = 1;
	tmp_propvals.ppropval = &tmp_propval;
	tmp_propval.proptag = PR_LAST_MODIFICATION_TIME;
	nt_time = rop_util_current_nttime();
	tmp_propval.pvalue = &nt_time;
	if (!set_properties(&tmp_propvals))
		return GXERR_CALL_FAILED;
	gxerr_t e_result = GXERR_CALL_FAILED;
	if (!exmdb_client::flush_instance(pattachment->pparent->pstore->get_dir(),
	    pattachment->instance_id, nullptr, &e_result) || e_result != GXERR_SUCCESS)
		return e_result;
	pattachment->b_new = FALSE;
	pattachment->b_touched = FALSE;
	pattachment->pparent->b_touched = TRUE;
	proptag_array_append(pattachment->pparent->pchanged_proptags, PR_MESSAGE_ATTACHMENTS);
	return GXERR_SUCCESS;
}

BOOL ATTACHMENT_OBJECT::get_all_proptags(PROPTAG_ARRAY *pproptags)
{
	auto pattachment = this;
	PROPTAG_ARRAY tmp_proptags;
	
	if (!exmdb_client::get_instance_all_proptags(pattachment->pparent->pstore->get_dir(),
	    pattachment->instance_id, &tmp_proptags))
		return FALSE;	
	pproptags->count = tmp_proptags.count;
	pproptags->pproptag = cu_alloc<uint32_t>(tmp_proptags.count + 5);
	if (NULL == pproptags->pproptag) {
		return FALSE;
	}
	memcpy(pproptags->pproptag, tmp_proptags.pproptag,
				sizeof(uint32_t)*tmp_proptags.count);
	pproptags->pproptag[pproptags->count++] = PR_ACCESS;
	pproptags->pproptag[pproptags->count++] = PR_ACCESS_LEVEL;
	pproptags->pproptag[pproptags->count] = PR_OBJECT_TYPE;
	pproptags->count ++;
	pproptags->pproptag[pproptags->count++] = PR_STORE_RECORD_KEY;
	pproptags->pproptag[pproptags->count++] = PR_STORE_ENTRYID;
	return TRUE;
}

static BOOL aobj_check_readonly_property(const ATTACHMENT_OBJECT *pattachment,
    uint32_t proptag)
{
	if (PROP_TYPE(proptag) == PT_OBJECT && proptag != PR_ATTACH_DATA_OBJ)
		return TRUE;
	switch (proptag) {
	case PROP_TAG_MID:
	case PR_ACCESS_LEVEL:
	case PROP_TAG_INCONFLICT:
	case PR_OBJECT_TYPE:
	case PR_RECORD_KEY:
	case PR_STORE_ENTRYID:
	case PR_STORE_RECORD_KEY:
		return TRUE;
	case PROP_TAG_ATTACHSIZE:
	case PR_CREATION_TIME:
	case PR_LAST_MODIFICATION_TIME:
		if (pattachment->b_new)
			return FALSE;
		return TRUE;
	}
	return FALSE;
}

static BOOL attachment_object_get_calculated_property(
	ATTACHMENT_OBJECT *pattachment, uint32_t proptag,
	void **ppvalue)
{
	switch (proptag) {
	case PR_ACCESS:
		*ppvalue = &pattachment->pparent->tag_access;
		return TRUE;
	case PR_ACCESS_LEVEL:
		*ppvalue = cu_alloc<uint32_t>();
		if (NULL == *ppvalue) {
			return FALSE;
		}
		*static_cast<uint32_t *>(*ppvalue) = pattachment->b_writable ?
			ACCESS_LEVEL_MODIFY : ACCESS_LEVEL_READ_ONLY;
		return TRUE;
	case PR_OBJECT_TYPE:
		*ppvalue = cu_alloc<uint32_t>();
		if (NULL == *ppvalue) {
			return FALSE;
		}
		*(uint32_t*)(*ppvalue) = OBJECT_ATTACHMENT;
		return TRUE;
	case PR_STORE_RECORD_KEY:
		*ppvalue = common_util_guid_to_binary(pattachment->pparent->pstore->mailbox_guid);
		return TRUE;
	case PR_STORE_ENTRYID:
		*ppvalue = common_util_to_store_entryid(
					pattachment->pparent->pstore);
		if (NULL == *ppvalue) {
			return FALSE;
		}
		return TRUE;
	}
	return FALSE;
}

BOOL ATTACHMENT_OBJECT::get_properties(const PROPTAG_ARRAY *pproptags,
    TPROPVAL_ARRAY *ppropvals)
{
	auto pattachment = this;
	int i;
	void *pvalue;
	PROPTAG_ARRAY tmp_proptags;
	TPROPVAL_ARRAY tmp_propvals;
	
	ppropvals->ppropval = cu_alloc<TAGGED_PROPVAL>(pproptags->count);
	if (NULL == ppropvals->ppropval) {
		return FALSE;
	}
	tmp_proptags.count = 0;
	tmp_proptags.pproptag = cu_alloc<uint32_t>(pproptags->count);
	if (NULL == tmp_proptags.pproptag) {
		return FALSE;
	}
	ppropvals->count = 0;
	for (i=0; i<pproptags->count; i++) {
		if (TRUE == attachment_object_get_calculated_property(
			pattachment, pproptags->pproptag[i], &pvalue)) {
			if (NULL == pvalue) {
				return FALSE;
			}
			ppropvals->ppropval[ppropvals->count].proptag =
										pproptags->pproptag[i];
			ppropvals->ppropval[ppropvals->count].pvalue = pvalue;
			ppropvals->count ++;
			continue;
		}
		tmp_proptags.pproptag[tmp_proptags.count] = pproptags->pproptag[i];
		tmp_proptags.count ++;
	}
	if (0 == tmp_proptags.count) {
		return TRUE;
	}
	if (!exmdb_client::get_instance_properties(pattachment->pparent->pstore->get_dir(),
	    0, pattachment->instance_id, &tmp_proptags, &tmp_propvals))
		return FALSE;	
	if (0 == tmp_propvals.count) {
		return TRUE;
	}
	memcpy(ppropvals->ppropval + ppropvals->count,
		tmp_propvals.ppropval,
		sizeof(TAGGED_PROPVAL)*tmp_propvals.count);
	ppropvals->count += tmp_propvals.count;
	return TRUE;	
}

BOOL ATTACHMENT_OBJECT::set_properties(const TPROPVAL_ARRAY *ppropvals)
{
	auto pattachment = this;
	int i;
	PROBLEM_ARRAY tmp_problems;
	TPROPVAL_ARRAY tmp_propvals;
	
	tmp_propvals.count = 0;
	tmp_propvals.ppropval = cu_alloc<TAGGED_PROPVAL>(ppropvals->count);
	if (NULL == tmp_propvals.ppropval) {
		return FALSE;
	}
	for (i=0; i<ppropvals->count; i++) {
		if (aobj_check_readonly_property(pattachment,
		    ppropvals->ppropval[i].proptag))
			continue;
		tmp_propvals.ppropval[tmp_propvals.count] =
							ppropvals->ppropval[i];
		tmp_propvals.count ++;
	}
	if (0 == tmp_propvals.count) {
		return TRUE;
	}
	if (!exmdb_client::set_instance_properties(pattachment->pparent->pstore->get_dir(),
	    pattachment->instance_id, &tmp_propvals, &tmp_problems))
		return FALSE;	
	if (tmp_problems.count < tmp_propvals.count) {
		pattachment->b_touched = TRUE;
	}
	return TRUE;
}

BOOL ATTACHMENT_OBJECT::remove_properties(const PROPTAG_ARRAY *pproptags)
{
	auto pattachment = this;
	int i;
	PROBLEM_ARRAY tmp_problems;
	PROPTAG_ARRAY tmp_proptags;
	
	tmp_proptags.count = 0;
	tmp_proptags.pproptag = cu_alloc<uint32_t>(pproptags->count);
	if (NULL == tmp_proptags.pproptag) {
		return FALSE;
	}
	for (i=0; i<pproptags->count; i++) {
		if (aobj_check_readonly_property(pattachment,
		    pproptags->pproptag[i]))
			continue;
		tmp_proptags.pproptag[tmp_proptags.count] =
								pproptags->pproptag[i];
		tmp_proptags.count ++;
	}
	if (0 == tmp_proptags.count) {
		return TRUE;
	}
	if (!exmdb_client::remove_instance_properties(pattachment->pparent->pstore->get_dir(),
	    pattachment->instance_id, &tmp_proptags, &tmp_problems))
		return FALSE;	
	if (tmp_problems.count < tmp_proptags.count) {
		pattachment->b_touched = TRUE;
	}
	return TRUE;
}

BOOL ATTACHMENT_OBJECT::copy_properties(ATTACHMENT_OBJECT *pattachment_src,
	const PROPTAG_ARRAY *pexcluded_proptags, BOOL b_force, BOOL *pb_cycle)
{
	auto pattachment = this;
	int i;
	PROBLEM_ARRAY tmp_problems;
	ATTACHMENT_CONTENT attctnt;
	
	if (!exmdb_client::check_instance_cycle(pattachment->pparent->pstore->get_dir(),
	    pattachment_src->instance_id, pattachment->instance_id, pb_cycle))
		return FALSE;	
	if (*pb_cycle)
		return TRUE;
	if (!exmdb_client::read_attachment_instance(pattachment_src->pparent->pstore->get_dir(),
	    pattachment_src->instance_id, &attctnt))
		return FALSE;
	common_util_remove_propvals(&attctnt.proplist, PROP_TAG_ATTACHNUMBER);
	i = 0;
	while (i < attctnt.proplist.count) {
		if (common_util_index_proptags(pexcluded_proptags,
			attctnt.proplist.ppropval[i].proptag) >= 0) {
			common_util_remove_propvals(&attctnt.proplist,
					attctnt.proplist.ppropval[i].proptag);
			continue;
		}
		i ++;
	}
	if (common_util_index_proptags(pexcluded_proptags, PR_ATTACH_DATA_OBJ) >= 0)
		attctnt.pembedded = NULL;
	if (!exmdb_client::write_attachment_instance(pattachment->pparent->pstore->get_dir(),
	    pattachment->instance_id, &attctnt, b_force, &tmp_problems))
		return FALSE;	
	pattachment->b_touched = TRUE;
	return TRUE;
}
