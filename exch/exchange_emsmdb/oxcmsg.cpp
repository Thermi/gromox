// SPDX-License-Identifier: GPL-2.0-only WITH linking exception
// SPDX-FileCopyrightText: 2020–2021 grommunio GmbH
// This file is part of Gromox.
#include <climits>
#include <cstdint>
#include <gromox/defs.h>
#include "rops.h"
#include <gromox/rop_util.hpp>
#include "common_util.h"
#include <gromox/proc_common.h>
#include "exmdb_client.h"
#include "logon_object.h"
#include "table_object.h"
#include "rop_processor.h"
#include "message_object.h"
#include "processor_types.h"
#include "emsmdb_interface.h"
#include "attachment_object.h"


uint32_t rop_openmessage(uint16_t cpid,
	uint64_t folder_id, uint8_t open_mode_flags,
	uint64_t message_id, uint8_t *phas_named_properties,
	TYPED_STRING *psubject_prefix, TYPED_STRING *pnormalized_subject,
	uint16_t *precipient_count, PROPTAG_ARRAY *precipient_columns,
	uint8_t *prow_count, OPENRECIPIENT_ROW **pprecipient_row,
	void *plogmap, uint8_t logon_id, uint32_t hin, uint32_t *phout)
{
	BOOL b_del;
	int rop_num;
	BOOL b_exist;
	BOOL b_owner;
	void *pvalue;
	int object_type;
	TARRAY_SET rcpts;
	uint32_t tag_access;
	uint32_t permission;
	PROPTAG_ARRAY proptags;
	TPROPVAL_ARRAY propvals;
	uint32_t proptag_buff[3];
	
	if (0x0FFF == cpid) {
		auto pinfo = emsmdb_interface_get_emsmdb_info();
		if (NULL == pinfo) {
			return ecError;
		}
		cpid = pinfo->cpid;
	}
	if (!common_util_verify_cpid(cpid))
		return MAPI_E_UNKNOWN_CPID;
	auto plogon = rop_processor_get_logon_object(plogmap, logon_id);
	if (NULL == plogon) {
		return ecError;
	}
	if (NULL == rop_processor_get_object(
		plogmap, logon_id, hin, &object_type)) {
		return ecNullObject;
	}
	if (OBJECT_TYPE_LOGON != object_type &&
		OBJECT_TYPE_FOLDER != object_type) {
		return ecNotSupported;
	}
	if (!exmdb_client_check_message(plogon->get_dir(), folder_id,
	    message_id, &b_exist))
		return ecError;
	if (FALSE == b_exist) {
		return ecNotFound;
	}
	if (!exmdb_client_get_message_property(plogon->get_dir(), nullptr, 0,
	    message_id, PROP_TAG_FOLDERID, &pvalue) || pvalue == nullptr)
		return ecError;
	folder_id = *(uint64_t*)pvalue;
	if (!exmdb_client_check_message_deleted(plogon->get_dir(), message_id, &b_del))
		return ecError;
	if (TRUE == b_del && 0 == (open_mode_flags &
		OPEN_MODE_FLAG_OPENSOFTDELETE)) {
		return ecNotFound;
	}
	
	tag_access = 0;
	auto rpc_info = get_rpc_info();
	if (plogon->logon_mode == LOGON_MODE_OWNER) {
		tag_access = TAG_ACCESS_MODIFY|TAG_ACCESS_READ|TAG_ACCESS_DELETE;
		goto PERMISSION_CHECK;
	}
	if (!exmdb_client_check_folder_permission(plogon->get_dir(), folder_id,
	    rpc_info.username, &permission))
		return ecError;
	if (!(permission & (frightsReadAny | frightsVisible | frightsOwner)))
		return ecAccessDenied;
	if (permission & frightsOwner) {
		tag_access = TAG_ACCESS_MODIFY|TAG_ACCESS_READ|TAG_ACCESS_DELETE;
		goto PERMISSION_CHECK;
	}
	if (!exmdb_client_check_message_owner(plogon->get_dir(), message_id,
	    rpc_info.username, &b_owner))
		return ecError;
	if (b_owner || (permission & frightsReadAny))
		tag_access |= TAG_ACCESS_READ;
	if ((permission & frightsEditAny) ||
	    (b_owner && (permission & frightsEditOwned)))
		tag_access |= TAG_ACCESS_MODIFY;	
	if ((permission & frightsDeleteAny) ||
	    (b_owner && (permission & frightsDeleteOwned)))
		tag_access |= TAG_ACCESS_DELETE;	
 PERMISSION_CHECK:
	if (0 == (TAG_ACCESS_READ & tag_access)) {
		return ecAccessDenied;
	}
	if (0 == (open_mode_flags & OPEN_MODE_FLAG_READWRITE) &&
		0 == (TAG_ACCESS_MODIFY & tag_access)) {
		if (open_mode_flags & OPEN_MODE_FLAG_BESTACCESS) {
			open_mode_flags &= ~OPEN_MODE_FLAG_BESTACCESS;
		} else {
			return ecAccessDenied;
		}
	}
	
	auto pmessage = message_object_create(plogon, false,
				cpid, message_id, &folder_id,
				tag_access, open_mode_flags, NULL);
	if (NULL == pmessage) {
		return ecMAPIOOM;
	}
	proptags.count = 3;
	proptags.pproptag = proptag_buff;
	proptag_buff[0] = PROP_TAG_HASNAMEDPROPERTIES;
	proptag_buff[1] = PR_SUBJECT_PREFIX;
	proptag_buff[2] = PR_NORMALIZED_SUBJECT;
	if (!pmessage->get_properties(0, &proptags, &propvals))
		return ecError;
	pvalue = common_util_get_propvals(&propvals,
				PROP_TAG_HASNAMEDPROPERTIES);
	*phas_named_properties = pvalue == nullptr || *static_cast<uint8_t *>(pvalue) == 0; /* XXX */
	pvalue = common_util_get_propvals(&propvals, PR_SUBJECT_PREFIX);
	if (NULL == pvalue) {
		psubject_prefix->string_type = STRING_TYPE_EMPTY;
		psubject_prefix->pstring = NULL;
	} else {
		psubject_prefix->string_type = STRING_TYPE_UNICODE;
		psubject_prefix->pstring = static_cast<char *>(pvalue);
	}
	pvalue = common_util_get_propvals(&propvals, PR_NORMALIZED_SUBJECT);
	if (NULL == pvalue) {
		pnormalized_subject->string_type = STRING_TYPE_EMPTY;
		pnormalized_subject->pstring = NULL;
	} else {
		pnormalized_subject->string_type = STRING_TYPE_UNICODE;
		pnormalized_subject->pstring = static_cast<char *>(pvalue);
	}
	if (!pmessage->get_recipient_num(precipient_count))
		return ecError;
	auto pcolumns = pmessage->get_rcpt_columns();
	*precipient_columns = *pcolumns;
	emsmdb_interface_get_rop_num(&rop_num);
	uint8_t rcpt_num = rop_num == 1 ? 0xFE : 5;
	if (!pmessage->read_recipients(0, rcpt_num, &rcpts))
		return ecError;
	*prow_count = rcpts.count;
	if (rcpts.count > 0) {
		*pprecipient_row = cu_alloc<OPENRECIPIENT_ROW>(rcpts.count);
		if (NULL == *pprecipient_row) {
			return ecMAPIOOM;
		}
	}
	for (size_t i = 0; i < rcpts.count; ++i) {
		if (FALSE == common_util_propvals_to_openrecipient(
			cpid, rcpts.pparray[i], pcolumns,
			(*pprecipient_row) + i)) {
			return ecMAPIOOM;
		}
	}
	auto hnd = rop_processor_add_object_handle(plogmap,
	           logon_id, hin, OBJECT_TYPE_MESSAGE, pmessage.get());
	if (hnd < 0) {
		return ecError;
	}
	pmessage.release();
	*phout = hnd;
	return ecSuccess;
}

uint32_t rop_createmessage(uint16_t cpid,
	uint64_t folder_id, uint8_t associated_flag,
	uint64_t **ppmessage_id, void *plogmap,
	uint8_t logon_id, uint32_t hin, uint32_t *phout)
{
	int object_type;
	uint32_t tag_access;
	uint32_t permission;
	uint32_t proptag_buff[4];
	PROPTAG_ARRAY tmp_proptags;
	TPROPVAL_ARRAY tmp_propvals;
	
	if (0x0FFF == cpid) {
		auto pinfo = emsmdb_interface_get_emsmdb_info();
		if (NULL == pinfo) {
			return ecError;
		}
		cpid = pinfo->cpid;
	}
	if (!common_util_verify_cpid(cpid))
		return MAPI_E_UNKNOWN_CPID;
	auto plogon = rop_processor_get_logon_object(plogmap, logon_id);
	if (NULL == plogon) {
		return ecError;
	}
	if (NULL == rop_processor_get_object(
		plogmap, logon_id, hin, &object_type)) {
		return ecNullObject;
	}
	if (OBJECT_TYPE_LOGON != object_type &&
		OBJECT_TYPE_FOLDER != object_type) {
		return ecNotSupported;
	}
	auto rpc_info = get_rpc_info();
	if (plogon->logon_mode != LOGON_MODE_OWNER) {
		if (!exmdb_client_check_folder_permission(plogon->get_dir(),
		    folder_id, rpc_info.username, &permission))
			return ecError;
		if (!(permission & (frightsOwner | frightsCreate)))
			return ecAccessDenied;
		tag_access = TAG_ACCESS_MODIFY|TAG_ACCESS_READ;
		if (permission & (frightsDeleteOwned | frightsDeleteAny))
			tag_access |= TAG_ACCESS_DELETE;
	} else {
		tag_access = TAG_ACCESS_MODIFY|TAG_ACCESS_READ|TAG_ACCESS_DELETE;
	}
	tmp_proptags.count = 4;
	tmp_proptags.pproptag = proptag_buff;
	proptag_buff[0] = PR_MESSAGE_SIZE_EXTENDED;
	proptag_buff[1] = PROP_TAG_STORAGEQUOTALIMIT;
	proptag_buff[2] = PROP_TAG_ASSOCIATEDCONTENTCOUNT;
	proptag_buff[3] = PROP_TAG_CONTENTCOUNT;
	if (!plogon->get_properties(&tmp_proptags, &tmp_propvals))
		return ecError;
	auto pvalue = common_util_get_propvals(&tmp_propvals, PROP_TAG_STORAGEQUOTALIMIT);
	uint64_t max_quota = ULLONG_MAX;
	if (pvalue != nullptr) {
		max_quota = *static_cast<uint32_t *>(pvalue);
		max_quota = max_quota >= ULLONG_MAX / 1024 ? ULLONG_MAX : max_quota * 1024ULL;
	}
	pvalue = common_util_get_propvals(&tmp_propvals, PR_MESSAGE_SIZE_EXTENDED);
	uint64_t total_size = pvalue == nullptr ? 0 : *static_cast<uint64_t *>(pvalue);
	if (total_size > max_quota)
		return ecQuotaExceeded;
	pvalue = common_util_get_propvals(&tmp_propvals,
					PROP_TAG_ASSOCIATEDCONTENTCOUNT);
	uint32_t total_mail = pvalue != nullptr ? *static_cast<uint32_t *>(pvalue) : 0;
	pvalue = common_util_get_propvals(&tmp_propvals,
							PROP_TAG_CONTENTCOUNT);
	if (NULL != pvalue) {
		total_mail += *(uint32_t*)pvalue;
	}
	if (total_mail > common_util_get_param(
		COMMON_UTIL_MAX_MESSAGE)) {
		return ecQuotaExceeded;
	}
	*ppmessage_id = cu_alloc<uint64_t>();
	if (NULL == *ppmessage_id) {
		return ecMAPIOOM;
	}
	if (!exmdb_client_allocate_message_id(plogon->get_dir(),
	    folder_id, *ppmessage_id))
		return ecError;
	auto pmessage = message_object_create(plogon, TRUE, cpid,
				**ppmessage_id, &folder_id, tag_access,
				OPEN_MODE_FLAG_READWRITE, NULL);
	if (NULL == pmessage) {
		return ecMAPIOOM;
	}
	BOOL b_fai = associated_flag == 0 ? false : TRUE;
	if (!pmessage->init_message(b_fai, cpid))
		return ecError;
	auto hnd = rop_processor_add_object_handle(plogmap,
	           logon_id, hin, OBJECT_TYPE_MESSAGE, pmessage.get());
	if (hnd < 0) {
		return ecError;
	}
	pmessage.release();
	*phout = hnd;
	return ecSuccess;
}

uint32_t rop_savechangesmessage(uint8_t save_flags,
	uint64_t *pmessage_id, void *plogmap, uint8_t logon_id,
	uint32_t hresponse, uint32_t hin)
{
	BOOL b_touched;
	int object_type;
	uint32_t tmp_proptag;
	PROPTAG_ARRAY proptags;
	TPROPVAL_ARRAY propvals;
	
	save_flags &= SAVE_FLAG_KEEPOPENREADONLY |
					SAVE_FLAG_KEEPOPENREADWRITE |
					SAVE_FLAG_FORCESAVE;
	auto pmessage = static_cast<MESSAGE_OBJECT *>(rop_processor_get_object(plogmap,
	                logon_id, hin, &object_type));
	if (NULL == pmessage) {
		return ecNullObject;
	}
	if (OBJECT_TYPE_MESSAGE != object_type) {
		return ecNotSupported;
	}
	auto tag_access = pmessage->get_tag_access();
	if (0 == (TAG_ACCESS_MODIFY & tag_access)) {
		return ecAccessDenied;
	}
	auto open_flags = pmessage->get_open_flags();
	if (0 == (open_flags & OPEN_MODE_FLAG_READWRITE) &&
		SAVE_FLAG_FORCESAVE != save_flags) {
		return ecAccessDenied;
	}
	if (SAVE_FLAG_FORCESAVE != save_flags) {
		if (!pmessage->check_orignal_touched(&b_touched))
			return ecError;
		if (TRUE == b_touched) {
			return ecObjectModified;
		}
	}
	proptags.count = 1;
	proptags.pproptag = &tmp_proptag;
	tmp_proptag = PROP_TAG_MID;
	if (!pmessage->get_properties(0, &proptags, &propvals))
		return ecError;
	auto pvalue = common_util_get_propvals(&propvals, PROP_TAG_MID);
	if (NULL == pvalue) {
		return ecError;
	}
	*pmessage_id = *(uint64_t*)pvalue;
	auto err = pmessage->save();
	if (err != GXERR_SUCCESS)
		return gxerr_to_hresult(err);
	switch (save_flags) {
	case SAVE_FLAG_KEEPOPENREADWRITE:
	case SAVE_FLAG_FORCESAVE:
		open_flags = OPEN_MODE_FLAG_READWRITE;
		pmessage->set_open_flags(open_flags);
		break;
	}
	return ecSuccess;
}

uint32_t rop_removeallrecipients(uint32_t reserved,
	void *plogmap, uint8_t logon_id, uint32_t hin)
{
	int object_type;
	
	auto pmessage = static_cast<MESSAGE_OBJECT *>(rop_processor_get_object(plogmap,
	                logon_id, hin, &object_type));
	if (NULL == pmessage) {
		return ecNullObject;
	}
	if (OBJECT_TYPE_MESSAGE != object_type) {
		return ecNotSupported;
	}
	pmessage->empty_rcpts();
	return ecSuccess;
}

uint32_t rop_modifyrecipients(const PROPTAG_ARRAY *pproptags,
	uint16_t count, const MODIFYRECIPIENT_ROW *prow,
	void *plogmap, uint8_t logon_id, uint32_t hin)
{
	int i;
	int object_type;
	TARRAY_SET tmp_set;
	TPROPVAL_ARRAY *ppropvals;
	
	if (pproptags->count >= 0x7FEF || count >= 0x7FEF) {
		return ecInvalidParam;
	}
	for (i=0; i<pproptags->count; i++) {
		switch (pproptags->pproptag[i]) {
		case PROP_TAG_ADDRESSTYPE:
		case PR_DISPLAY_NAME:
		case PR_EMAIL_ADDRESS:
		case PR_ENTRYID:
		case PROP_TAG_INSTANCEKEY:
		case PROP_TAG_RECIPIENTTYPE:
		case PROP_TAG_SEARCHKEY:
		case PROP_TAG_SENDRICHINFO:
		case PROP_TAG_TRANSMITTABLEDISPLAYNAME:
			return ecInvalidParam;
		}
	}
	auto pmessage = static_cast<MESSAGE_OBJECT *>(rop_processor_get_object(plogmap,
	                logon_id, hin, &object_type));
	if (NULL == pmessage) {
		return ecNullObject;
	}
	if (OBJECT_TYPE_MESSAGE != object_type) {
		return ecNotSupported;
	}
	auto pinfo = emsmdb_interface_get_emsmdb_info();
	if (NULL == pinfo) {
		return ecError;
	}
	tmp_set.count = count;
	tmp_set.pparray = cu_alloc<TPROPVAL_ARRAY *>(count);
	if (NULL == tmp_set.pparray) {
		return ecMAPIOOM;
	}
	for (i=0; i<count; i++) {
		ppropvals = cu_alloc<TPROPVAL_ARRAY>();
		if (NULL == ppropvals) {
			return ecMAPIOOM;
		}
		if (NULL == prow[i].precipient_row) {
			ppropvals->count = 1;
			ppropvals->ppropval = cu_alloc<TAGGED_PROPVAL>();
			if (NULL == ppropvals->ppropval) {
				return ecMAPIOOM;
			}
			ppropvals->ppropval->proptag = PROP_TAG_ROWID;
			ppropvals->ppropval->pvalue = (void*)&prow[i].row_id;
		} else {
			if (FALSE == common_util_modifyrecipient_to_propvals(
				pinfo->cpid, prow + i, pproptags, ppropvals)) {
				return ecMAPIOOM;
			}
		}
		tmp_set.pparray[i] = ppropvals;
	}
	if (!pmessage->set_rcpts(&tmp_set))
		return ecError;
	return ecSuccess;
}

uint32_t rop_readrecipients(uint32_t row_id,
	uint16_t reserved, uint8_t *pcount, EXT_PUSH *pext,
	void *plogmap, uint8_t logon_id, uint32_t hin)
{
	auto &ext = *pext;
	size_t i;
	int object_type;
	TARRAY_SET tmp_set;
	READRECIPIENT_ROW tmp_row;
	
	auto pmessage = static_cast<MESSAGE_OBJECT *>(rop_processor_get_object(plogmap,
	                logon_id, hin, &object_type));
	if (NULL == pmessage) {
		return ecNullObject;
	}
	if (OBJECT_TYPE_MESSAGE != object_type) {
		return ecNotSupported;
	}
	if (!pmessage->read_recipients(row_id, 0xFE, &tmp_set))
		return ecError;
	if (0 == tmp_set.count) {
		return ecNotFound;
	}
	for (i = 0; i < tmp_set.count; ++i) {
		if (!common_util_propvals_to_readrecipient(pmessage->get_cpid(),
		    tmp_set.pparray[i], pmessage->get_rcpt_columns(), &tmp_row))
			return ecMAPIOOM;
		uint32_t last_offset = ext.m_offset;
		if (pext->p_readrecipient_row(pmessage->get_rcpt_columns(),
		    &tmp_row) != EXT_ERR_SUCCESS) {
			ext.m_offset = last_offset;
			break;
		}
	}
	if (0 == i) {
		return ecBufferTooSmall;
	}
	*pcount = i;
	return ecSuccess;
}

uint32_t rop_reloadcachedinformation(uint16_t reserved,
	uint8_t *phas_named_properties, TYPED_STRING *psubject_prefix,
	TYPED_STRING *pnormalized_subject, uint16_t *precipient_count,
	PROPTAG_ARRAY *precipient_columns, uint8_t *prow_count,
	OPENRECIPIENT_ROW **pprecipient_row, void *plogmap,
	uint8_t logon_id, uint32_t hin)
{
	int object_type;
	TARRAY_SET rcpts;
	PROPTAG_ARRAY proptags;
	TPROPVAL_ARRAY propvals;
	uint32_t proptag_buff[3];
	
	auto pmessage = static_cast<MESSAGE_OBJECT *>(rop_processor_get_object(plogmap,
	                logon_id, hin, &object_type));
	if (NULL == pmessage) {
		return ecNullObject;
	}
	if (OBJECT_TYPE_MESSAGE != object_type) {
		return ecNotSupported;
	}
	proptags.count = 3;
	proptags.pproptag = proptag_buff;
	proptag_buff[0] = PROP_TAG_HASNAMEDPROPERTIES;
	proptag_buff[1] = PR_SUBJECT_PREFIX;
	proptag_buff[2] = PR_NORMALIZED_SUBJECT;
	if (!pmessage->get_properties(0, &proptags, &propvals))
		return ecError;
	auto pvalue = common_util_get_propvals(&propvals,
				PROP_TAG_HASNAMEDPROPERTIES);
	*phas_named_properties = pvalue == nullptr || *static_cast<uint8_t *>(pvalue) == 0; /* XXX */
	pvalue = common_util_get_propvals(&propvals, PR_SUBJECT_PREFIX);
	if (NULL == pvalue) {
		psubject_prefix->string_type = STRING_TYPE_EMPTY;
		psubject_prefix->pstring = NULL;
	} else {
		psubject_prefix->string_type = STRING_TYPE_UNICODE;
		psubject_prefix->pstring = static_cast<char *>(pvalue);
	}
	pvalue = common_util_get_propvals(&propvals, PR_NORMALIZED_SUBJECT);
	if (NULL == pvalue) {
		pnormalized_subject->string_type = STRING_TYPE_EMPTY;
		pnormalized_subject->pstring = NULL;
	} else {
		pnormalized_subject->string_type = STRING_TYPE_UNICODE;
		pnormalized_subject->pstring = static_cast<char *>(pvalue);
	}
	if (!pmessage->get_recipient_num(precipient_count))
		return ecError;
	auto pcolumns = pmessage->get_rcpt_columns();
	*precipient_columns = *pcolumns;
	if (!pmessage->read_recipients(0, 0xFE, &rcpts))
		return ecError;
	*prow_count = rcpts.count;
	*pprecipient_row = cu_alloc<OPENRECIPIENT_ROW>(rcpts.count);
	if (NULL == *pprecipient_row) {
		return ecMAPIOOM;
	}
	for (size_t i = 0; i < rcpts.count; ++i) {
		if (!common_util_propvals_to_openrecipient(pmessage->get_cpid(),
		    rcpts.pparray[i], pcolumns, *pprecipient_row + i))
			return ecMAPIOOM;
	}
	return ecSuccess;
}

uint32_t rop_setmessagestatus(uint64_t message_id,
	uint32_t message_status, uint32_t status_mask,
	uint32_t *pmessage_status, void *plogmap,
	uint8_t logon_id, uint32_t hin)
{
	void *pvalue;
	uint32_t result;
	int object_type;
	uint32_t new_status;
	TAGGED_PROPVAL propval;
	uint32_t original_status;
	
	auto plogon = rop_processor_get_logon_object(plogmap, logon_id);
	if (NULL == plogon) {
		return ecError;
	}
	if (NULL == rop_processor_get_object(
		plogmap, logon_id, hin, &object_type)) {
		return ecNullObject;
	}
	if (OBJECT_TYPE_FOLDER != object_type) {
		return ecNotSupported;
	}
	/* we do not check permission because it's maybe
		not an important property for the message.
		also, we don't know the message location */
	if (!exmdb_client_get_message_property(plogon->get_dir(), nullptr, 0,
	    message_id, PROP_TAG_MESSAGESTATUS, &pvalue))
		return ecError;
	if (NULL == pvalue) {
		return ecNotFound;
	}
	original_status = *(uint32_t*)pvalue;
	new_status = message_status & status_mask;
	if (new_status & MESSAGE_STATUS_IN_CONFLICT) {
		return ecAccessDenied;
	}
	new_status |= original_status & ~(status_mask & ~new_status);
	*pmessage_status = new_status;
	propval.proptag = PROP_TAG_MESSAGESTATUS;
	propval.pvalue = &new_status;
	if (!exmdb_client_set_message_property(plogon->get_dir(), nullptr, 0,
	    message_id, &propval, &result))
		return ecError;
	return result;
}

uint32_t rop_getmessagestatus(uint64_t message_id,
	uint32_t *pmessage_status, void *plogmap,
	uint8_t logon_id, uint32_t hin)
{
	void *pvalue;
	int object_type;
	
	auto plogon = rop_processor_get_logon_object(plogmap, logon_id);
	if (NULL == plogon) {
		return ecError;
	}
	if (NULL == rop_processor_get_object(
		plogmap, logon_id, hin, &object_type)) {
		return ecNullObject;
	}
	if (OBJECT_TYPE_FOLDER != object_type) {
		return ecNotSupported;
	}
	if (!exmdb_client_get_message_property(plogon->get_dir(), nullptr, 0,
	    message_id, PROP_TAG_MESSAGESTATUS, &pvalue))
		return ecError;
	if (NULL == pvalue) {
		return ecNotFound;
	}
	*pmessage_status = *(uint32_t*)pvalue;
	return ecSuccess;
}

static BOOL oxcmsg_setreadflag(LOGON_OBJECT *plogon,
	uint64_t message_id, uint8_t read_flag)
{
	void *pvalue;
	BOOL b_notify;
	BOOL b_changed;
	uint64_t read_cn;
	uint8_t tmp_byte;
	PROBLEM_ARRAY problems;
	MESSAGE_CONTENT *pbrief;
	TPROPVAL_ARRAY propvals;
	static constexpr uint8_t fake_false = 0;
	TAGGED_PROPVAL propval_buff[2];
	
	auto rpc_info = get_rpc_info();
	auto pinfo = emsmdb_interface_get_emsmdb_info();
	auto username = plogon->check_private() ? nullptr : rpc_info.username;
	b_notify = FALSE;
	b_changed = FALSE;
	switch (read_flag) {
	case MSG_READ_FLAG_DEFAULT:
	case MSG_READ_FLAG_SUPPRESS_RECEIPT:
		if (!exmdb_client_get_message_property(plogon->get_dir(),
		    username, 0, message_id, PR_READ, &pvalue))
			return FALSE;	
		if (NULL == pvalue || 0 == *(uint8_t*)pvalue) {
			tmp_byte = 1;
			b_changed = TRUE;
			if (MSG_READ_FLAG_DEFAULT == read_flag) {
				if (!exmdb_client_get_message_property(plogon->get_dir(),
				    username, 0, message_id,
				    PROP_TAG_READRECEIPTREQUESTED, &pvalue))
					return FALSE;
				if (NULL != pvalue && 0 != *(uint8_t*)pvalue) {
					b_notify = TRUE;
				}
			}
		}
		break;
	case MSG_READ_FLAG_CLEAR_READ_FLAG:
		if (!exmdb_client_get_message_property(plogon->get_dir(),
		    username, 0, message_id, PR_READ, &pvalue))
			return FALSE;	
		if (NULL != pvalue && 0 != *(uint8_t*)pvalue) {
			tmp_byte = 0;
			b_changed = TRUE;
		}
		break;
	case MSG_READ_FLAG_GENERATE_RECEIPT_ONLY:
		if (!exmdb_client_get_message_property(plogon->get_dir(),
		    username, 0, message_id, PROP_TAG_READRECEIPTREQUESTED,
		    &pvalue))
			return FALSE;
		if (NULL != pvalue && 0 != *(uint8_t*)pvalue) {
			b_notify = TRUE;
		}
		break;
	case MSG_READ_FLAG_CLEAR_NOTIFY_READ:
	case MSG_READ_FLAG_CLEAR_NOTIFY_UNREAD:
	case MSG_READ_FLAG_CLEAR_NOTIFY_READ |
		MSG_READ_FLAG_CLEAR_NOTIFY_UNREAD:
		if ((read_flag & MSG_READ_FLAG_CLEAR_NOTIFY_READ) &&
		    exmdb_client_get_message_property(plogon->get_dir(),
		    username, 0, message_id,
		    PROP_TAG_READRECEIPTREQUESTED, &pvalue) &&
		    pvalue != nullptr && *static_cast<uint8_t *>(pvalue) != 0 &&
		    !exmdb_client_remove_message_property(plogon->get_dir(),
		     pinfo->cpid, message_id, PROP_TAG_READRECEIPTREQUESTED))
			return FALSE;
		if ((read_flag & MSG_READ_FLAG_CLEAR_NOTIFY_UNREAD) &&
		    exmdb_client_get_message_property(plogon->get_dir(),
		    username, 0, message_id,
		    PROP_TAG_NONRECEIPTNOTIFICATIONREQUESTED, &pvalue) &&
		    pvalue != nullptr && *static_cast<uint8_t *>(pvalue) != 0 &&
		    !exmdb_client_remove_message_property(plogon->get_dir(),
		    pinfo->cpid, message_id, PROP_TAG_NONRECEIPTNOTIFICATIONREQUESTED))
			return FALSE;
		if (!exmdb_client_mark_modified(plogon->get_dir(), message_id))
			return FALSE;	
		return TRUE;
	default:
		return TRUE;
	}
	if (TRUE == b_changed) {
		if (!exmdb_client_set_message_read_state(plogon->get_dir(),
		    username, message_id, tmp_byte, &read_cn))
			return FALSE;
	}
	if (TRUE == b_notify) {
		if (!exmdb_client_get_message_brief(plogon->get_dir(),
		    pinfo->cpid, message_id, &pbrief))
			return FALSE;	
		if (NULL != pbrief) {
			common_util_notify_receipt(plogon->get_account(),
				NOTIFY_RECEIPT_READ, pbrief);
		}
		propvals.count = 2;
		propvals.ppropval = propval_buff;
		propval_buff[0].proptag = PROP_TAG_READRECEIPTREQUESTED;
		propval_buff[0].pvalue = deconst(&fake_false);
		propval_buff[1].proptag = PROP_TAG_NONRECEIPTNOTIFICATIONREQUESTED;
		propval_buff[1].pvalue = deconst(&fake_false);
		exmdb_client_set_message_properties(plogon->get_dir(), username,
			0, message_id, &propvals, &problems);
	}
	return TRUE;
}

uint32_t rop_setreadflags(uint8_t want_asynchronous,
	uint8_t read_flags,	const LONGLONG_ARRAY *pmessage_ids,
	uint8_t *ppartial_completion, void *plogmap,
	uint8_t logon_id, uint32_t hin)
{
	BOOL b_partial;
	int object_type;
	
	auto plogon = rop_processor_get_logon_object(plogmap, logon_id);
	if (NULL == plogon) {
		return ecError;
	}
	if (NULL == rop_processor_get_object(
		plogmap, logon_id, hin, &object_type)) {
		return ecNullObject;
	}
	if (OBJECT_TYPE_FOLDER != object_type) {
		return ecNotSupported;
	}
	b_partial = FALSE;
	for (size_t i = 0; i < pmessage_ids->count; ++i) {
		if (FALSE == oxcmsg_setreadflag(plogon,
			pmessage_ids->pll[i], read_flags)) {
			b_partial = TRUE;	
		}
	}
	*ppartial_completion = !!b_partial;
	return ecSuccess;
}

uint32_t rop_setmessagereadflag(uint8_t read_flags,
	const LONG_TERM_ID *pclient_data, uint8_t *pread_change,
	void *plogmap, uint8_t logon_id, uint32_t hresponse, uint32_t hin)
{
	BOOL b_changed;
	int object_type;
	
	auto plogon = rop_processor_get_logon_object(plogmap, logon_id);
	if (NULL == plogon) {
		return ecError;
	}
	if (NULL == rop_processor_get_object(
		plogmap, logon_id, hresponse, &object_type)) {
		return ecNullObject;
	}
	auto pmessage = static_cast<MESSAGE_OBJECT *>(rop_processor_get_object(plogmap,
	                logon_id, hin, &object_type));
	if (NULL == pmessage) {
		return ecNullObject;
	}
	if (OBJECT_TYPE_MESSAGE != object_type) {
		return ecNotSupported;
	}
	if (!pmessage->set_readflag(read_flags, &b_changed))
		return ecError;
	*pread_change = !b_changed;
	return ecSuccess;
}

uint32_t rop_openattachment(uint8_t flags, uint32_t attachment_id,
	void *plogmap, uint8_t logon_id, uint32_t hin, uint32_t *phout)
{
	int object_type;
	
	auto plogon = rop_processor_get_logon_object(plogmap, logon_id);
	if (NULL == plogon) {
		return ecError;
	}
	auto pmessage = static_cast<MESSAGE_OBJECT *>(rop_processor_get_object(
	                plogmap, logon_id, hin, &object_type));
	if (NULL == pmessage) {
		return ecNullObject;
	}
	if (OBJECT_TYPE_MESSAGE != object_type) {
		return ecNotSupported;
	}
	if (flags & OPEN_MODE_FLAG_READWRITE) {
		auto tag_access = pmessage->get_tag_access();
		if (0 == (tag_access & TAG_ACCESS_MODIFY)) {
			if (flags & OPEN_MODE_FLAG_BESTACCESS) {
				flags &= ~OPEN_MODE_FLAG_BESTACCESS;
			} else {
				return ecAccessDenied;
			}
		}
	}
	auto pattachment = attachment_object_create(
			pmessage, attachment_id, flags);
	if (NULL == pattachment) {
		return ecError;
	}
	if (pattachment->get_instance_id() == 0)
		return ecNotFound;
	auto hnd = rop_processor_add_object_handle(plogmap, logon_id,
	           hin, OBJECT_TYPE_ATTACHMENT, pattachment.get());
	if (hnd < 0) {
		return ecError;
	}
	pattachment.release();
	*phout = hnd;
	return ecSuccess;
}

uint32_t rop_createattachment(uint32_t *pattachment_id,
	void *plogmap, uint8_t logon_id, uint32_t hin, uint32_t *phout)
{
	int object_type;
	
	auto plogon = rop_processor_get_logon_object(plogmap, logon_id);
	if (NULL == plogon) {
		return ecError;
	}
	auto pmessage = static_cast<MESSAGE_OBJECT *>(rop_processor_get_object(
	                plogmap, logon_id, hin, &object_type));
	if (NULL == pmessage) {
		return ecNullObject;
	}
	if (OBJECT_TYPE_MESSAGE != object_type) {
		return ecNotSupported;
	}
	auto tag_access = pmessage->get_tag_access();
	if (0 == (tag_access & TAG_ACCESS_MODIFY)) {
		return ecAccessDenied;
	}
	auto pattachment = attachment_object_create(pmessage,
		ATTACHMENT_NUM_INVALID, OPEN_MODE_FLAG_READWRITE);
	if (NULL == pattachment) {
		return ecError;
	}
	*pattachment_id = pattachment->get_attachment_num();
	if (ATTACHMENT_NUM_INVALID == *pattachment_id) {
		return ecMaxAttachmentExceeded;
	}
	if (!pattachment->init_attachment())
		return ecError;
	auto hnd = rop_processor_add_object_handle(plogmap, logon_id,
	           hin, OBJECT_TYPE_ATTACHMENT, pattachment.get());
	if (hnd < 0) {
		return ecError;
	}
	pattachment.release();
	*phout = hnd;
	return ecSuccess;
}

uint32_t rop_deleteattachment(uint32_t attachment_id,
	void *plogmap, uint8_t logon_id, uint32_t hin)
{
	int object_type;
	auto pmessage = static_cast<MESSAGE_OBJECT *>(rop_processor_get_object(plogmap,
	                logon_id, hin, &object_type));
	if (NULL == pmessage) {
		return ecNullObject;
	}
	if (OBJECT_TYPE_MESSAGE != object_type) {
		return ecNotSupported;
	}
	auto tag_access = pmessage->get_tag_access();
	if (0 == (TAG_ACCESS_MODIFY & tag_access)) {
		return ecAccessDenied;
	}
	if (!pmessage->delete_attachment(attachment_id))
		return ecError;
	return ecSuccess;
}

uint32_t rop_savechangesattachment(uint8_t save_flags,
	void *plogmap, uint8_t logon_id, uint32_t hresponse, uint32_t hin)
{
	int object_type;
	
	save_flags &= SAVE_FLAG_KEEPOPENREADONLY |
					SAVE_FLAG_KEEPOPENREADWRITE |
					SAVE_FLAG_FORCESAVE;
	if (NULL == rop_processor_get_object(plogmap,
		logon_id, hresponse, &object_type)) {
		return ecNullObject;
	}
	if (OBJECT_TYPE_MESSAGE != object_type) {
		return ecNotSupported;
	}
	auto pattachment = static_cast<ATTACHMENT_OBJECT *>(rop_processor_get_object(plogmap,
	                   logon_id, hin, &object_type));
	if (NULL == pattachment) {
		return ecNullObject;
	}
	if (OBJECT_TYPE_ATTACHMENT != object_type) {
		return ecNotSupported;
	}
	auto tag_access = pattachment->get_tag_access();
	if (0 == (TAG_ACCESS_MODIFY & tag_access)) {
		return ecAccessDenied;
	}
	auto open_flags = pattachment->get_open_flags();
	if (0 == (open_flags & OPEN_MODE_FLAG_READWRITE) &&
		SAVE_FLAG_FORCESAVE != save_flags) {
		return ecAccessDenied;
	}
	auto err = pattachment->save();
	if (err != GXERR_SUCCESS)
		return gxerr_to_hresult(err);
	switch (save_flags) {
	case SAVE_FLAG_KEEPOPENREADWRITE:
	case SAVE_FLAG_FORCESAVE:
		open_flags = OPEN_MODE_FLAG_READWRITE;
		pattachment->set_open_flags(open_flags);
		break;
	}
	return ecSuccess;
}

uint32_t rop_openembeddedmessage(uint16_t cpid,
	uint8_t open_embedded_flags, uint8_t *preserved,
	uint64_t *pmessage_id, uint8_t *phas_named_properties,
	TYPED_STRING *psubject_prefix, TYPED_STRING *pnormalized_subject,
	uint16_t *precipient_count, PROPTAG_ARRAY *precipient_columns,
	uint8_t *prow_count, OPENRECIPIENT_ROW **pprecipient_row,
	void *plogmap, uint8_t logon_id, uint32_t hin, uint32_t *phout)
{
	int object_type;
	TARRAY_SET rcpts;
	PROPTAG_ARRAY proptags;
	TPROPVAL_ARRAY propvals;
	uint32_t proptag_buff[4];
	
	*preserved = 0;
	if (0x0FFF == cpid) {
		auto pinfo = emsmdb_interface_get_emsmdb_info();
		if (NULL == pinfo) {
			return ecError;
		}
		cpid = pinfo->cpid;
	}
	if (!common_util_verify_cpid(cpid))
		return MAPI_E_UNKNOWN_CPID;
	auto plogon = rop_processor_get_logon_object(plogmap, logon_id);
	if (NULL == plogon) {
		return ecError;
	}
	auto pattachment = static_cast<ATTACHMENT_OBJECT *>(rop_processor_get_object(plogmap,
	                   logon_id, hin, &object_type));
	if (NULL == pattachment) {
		return ecNullObject;
	}
	if (OBJECT_TYPE_ATTACHMENT != object_type) {
		return ecNotSupported;
	}
	auto tag_access = pattachment->get_tag_access();
	if (0 == (tag_access & TAG_ACCESS_MODIFY) &&
		((OPEN_EMBEDDED_FLAG_READWRITE & open_embedded_flags))) {
		return ecAccessDenied;
	}	
	auto pmessage = message_object_create(plogon, false,
				cpid, 0, pattachment, tag_access,
				open_embedded_flags, NULL);
	if (NULL == pmessage) {
		return ecError;
	}
	if (pmessage->get_instance_id() == 0) {
		if (0 == (OPEN_EMBEDDED_FLAG_CREATE & open_embedded_flags)) {
			return ecNotFound;
		}
		if (0 == (tag_access & TAG_ACCESS_MODIFY)) {
			return ecAccessDenied;
		}
		pmessage = message_object_create(plogon, TRUE,
					cpid, 0, pattachment, tag_access,
					OPEN_MODE_FLAG_READWRITE, NULL);
		if (NULL == pmessage) {
			return ecError;
		}
		if (!pmessage->init_message(false, cpid))
			return ecError;
		proptags.count = 1;
		proptags.pproptag = proptag_buff;
		proptag_buff[0] = PROP_TAG_MID;
		if (!pmessage->get_properties(0, &proptags, &propvals))
			return ecError;
		auto pvalue = common_util_get_propvals(&propvals, PROP_TAG_MID);
		if (NULL == pvalue) {
			return ecError;
		}
		*pmessage_id = *(uint64_t*)pvalue;
		auto hnd = rop_processor_add_object_handle(plogmap,
		           logon_id, hin, OBJECT_TYPE_MESSAGE, pmessage.get());
		if (hnd < 0) {
			return ecError;
		}
		pmessage.release();
		*phout = hnd;
		*phas_named_properties = 0;
		psubject_prefix->string_type = STRING_TYPE_EMPTY;
		psubject_prefix->pstring = NULL;
		pnormalized_subject->string_type = STRING_TYPE_EMPTY;
		pnormalized_subject->pstring = NULL;
		precipient_columns->count = 0;
		precipient_columns->pproptag = NULL;
		*precipient_count = 0;
		*prow_count = 0;
		*pprecipient_row = NULL;
		return ecSuccess;
	}
	proptags.count = 4;
	proptags.pproptag = proptag_buff;
	proptag_buff[0] = PROP_TAG_MID;
	proptag_buff[1] = PROP_TAG_HASNAMEDPROPERTIES;
	proptag_buff[2] = PR_SUBJECT_PREFIX;
	proptag_buff[3] = PR_NORMALIZED_SUBJECT;
	if (!pmessage->get_properties(0, &proptags, &propvals))
		return ecError;
	auto pvalue = common_util_get_propvals(&propvals, PROP_TAG_MID);
	if (NULL == pvalue) {
		return ecError;
	}
	*pmessage_id = *(uint64_t*)pvalue;
	pvalue = common_util_get_propvals(&propvals,
				PROP_TAG_HASNAMEDPROPERTIES);
	*phas_named_properties = pvalue == nullptr || *static_cast<uint8_t *>(pvalue) == 0; /* XXX */
	pvalue = common_util_get_propvals(&propvals, PR_SUBJECT_PREFIX);
	if (NULL == pvalue) {
		psubject_prefix->string_type = STRING_TYPE_EMPTY;
		psubject_prefix->pstring = NULL;
	} else {
		psubject_prefix->string_type = STRING_TYPE_UNICODE;
		psubject_prefix->pstring = static_cast<char *>(pvalue);
	}
	pvalue = common_util_get_propvals(&propvals, PR_NORMALIZED_SUBJECT);
	if (NULL == pvalue) {
		pnormalized_subject->string_type = STRING_TYPE_EMPTY;
		pnormalized_subject->pstring = NULL;
	} else {
		pnormalized_subject->string_type = STRING_TYPE_UNICODE;
		pnormalized_subject->pstring = static_cast<char *>(pvalue);
	}
	if (!pmessage->get_recipient_num(precipient_count))
		return ecError;
	auto pcolumns = pmessage->get_rcpt_columns();
	*precipient_columns = *pcolumns;
	if (!pmessage->read_recipients(0, 0xFE, &rcpts))
		return ecError;
	*prow_count = rcpts.count;
	*pprecipient_row = cu_alloc<OPENRECIPIENT_ROW>(rcpts.count);
	if (NULL == *pprecipient_row) {
		return ecMAPIOOM;
	}
	for (size_t i = 0; i < rcpts.count; ++i) {
		if (!common_util_propvals_to_openrecipient(pmessage->get_cpid(),
		    rcpts.pparray[i], pcolumns, *pprecipient_row + i))
			return ecMAPIOOM;
	}
	auto hnd = rop_processor_add_object_handle(plogmap,
	           logon_id, hin, OBJECT_TYPE_MESSAGE, pmessage.get());
	if (hnd < 0) {
		return ecError;
	}
	pmessage.release();
	*phout = hnd;
	return ecSuccess;
}

uint32_t rop_getattachmenttable(uint8_t table_flags,
	void *plogmap, uint8_t logon_id, uint32_t hin, uint32_t *phout)
{
	int object_type;
	
	auto plogon = rop_processor_get_logon_object(plogmap, logon_id);
	if (NULL == plogon) {
		return ecError;
	}
	auto pmessage = static_cast<MESSAGE_OBJECT *>(rop_processor_get_object(plogmap,
	                logon_id, hin, &object_type));
	if (NULL == pmessage) {
		return ecNullObject;
	}
	if (OBJECT_TYPE_MESSAGE != object_type) {
		return ecNotSupported;
	}
	auto ptable = table_object_create(plogon, pmessage, table_flags,
	              ropGetAttachmentTable, logon_id);
	if (NULL == ptable) {
		return ecMAPIOOM;
	}
	auto hnd = rop_processor_add_object_handle(plogmap,
	           logon_id, hin, OBJECT_TYPE_TABLE, ptable.get());
	if (hnd < 0) {
		return ecError;
	}
	ptable->set_handle(hnd);
	ptable.release();
	*phout = hnd;
	return ecSuccess;
}

uint32_t rop_getvalidattachments(LONG_ARRAY *pattachment_ids,
	void *plogmap, uint8_t logon_id, uint32_t hin)
{
	/* just like exchange 2010 or later,
		we do not implement this rop */
	return NotImplemented;
}
