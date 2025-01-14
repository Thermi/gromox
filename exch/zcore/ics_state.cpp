// SPDX-License-Identifier: GPL-2.0-only WITH linking exception
#include <cstdint>
#include <gromox/tpropval_array.hpp>
#include "common_util.h"
#include <gromox/ext_buffer.hpp>
#include "ics_state.h"
#include <gromox/rop_util.hpp>
#include <gromox/idset.hpp>
#include <cstdlib>
#include <cstring>

static void ics_state_clear(ICS_STATE *pstate)
{
	if (NULL != pstate->pgiven) {
		idset_free(pstate->pgiven);
		pstate->pgiven = NULL;
	}
	if (NULL != pstate->pseen) {
		idset_free(pstate->pseen);
		pstate->pseen = NULL;
	}
	if (NULL != pstate->pseen_fai) {
		idset_free(pstate->pseen_fai);
		pstate->pseen_fai = NULL;
	}
	if (NULL != pstate->pread) {
		idset_free(pstate->pread);
		pstate->pread = NULL;
	}
}

static BOOL ics_state_init(ICS_STATE *pstate)
{
	pstate->pgiven = idset_init(TRUE, REPL_TYPE_ID);
	if (NULL == pstate->pgiven) {
		return FALSE;
	}
	pstate->pseen = idset_init(TRUE, REPL_TYPE_ID);
	if (NULL == pstate->pseen) {
		return FALSE;
	}
	if (ICS_TYPE_CONTENTS == pstate->type) {
		pstate->pseen_fai = idset_init(TRUE, REPL_TYPE_ID);
		if (NULL == pstate->pseen_fai) {
			return FALSE;
		}
		pstate->pread = idset_init(TRUE, REPL_TYPE_ID);
		if (NULL == pstate->pread) {
			return FALSE;
		}
	}
	return TRUE;
}

ICS_STATE* ics_state_create(uint8_t type)
{
	auto pstate = me_alloc<ICS_STATE>();
	if (NULL == pstate) {
		return NULL;
	}
	memset(pstate, 0, sizeof(ICS_STATE));
	pstate->type = type;
	if (FALSE == ics_state_init(pstate)) {
		ics_state_clear(pstate);
		free(pstate);
		return NULL;
	}
	return pstate;
}

void ics_state_free(ICS_STATE *pstate)
{
	ics_state_clear(pstate);
	free(pstate);
}

BINARY* ics_state_serialize(ICS_STATE *pstate)
{
	BINARY *pbin;
	EXT_PUSH ext_push;
	TAGGED_PROPVAL propval;
	TPROPVAL_ARRAY *pproplist;
	static uint8_t bin_buff[8];
	static const BINARY fake_bin = {sizeof(bin_buff), {(uint8_t *)bin_buff}};
	
	if (ICS_TYPE_CONTENTS == pstate->type) {
		if (TRUE == idset_check_empty(pstate->pgiven) &&
			TRUE == idset_check_empty(pstate->pseen) &&
			TRUE == idset_check_empty(pstate->pseen_fai) &&
			TRUE == idset_check_empty(pstate->pread)) {
			return deconst(&fake_bin);
		}
	} else {
		if (TRUE == idset_check_empty(pstate->pgiven) &&
			TRUE == idset_check_empty(pstate->pseen)) {
			return deconst(&fake_bin);
		}
	}
	pproplist = tpropval_array_init();
	if (NULL == pproplist) {
		return NULL;
	}
	
	pbin = idset_serialize(pstate->pgiven);
	if (NULL == pbin) {
		tpropval_array_free(pproplist);
		return NULL;
	}
	propval.proptag = META_TAG_IDSETGIVEN1;
	propval.pvalue = pbin;
	if (!tpropval_array_set_propval(pproplist, &propval)) {
		rop_util_free_binary(pbin);
		tpropval_array_free(pproplist);
		return NULL;
	}
	rop_util_free_binary(pbin);
	
	pbin = idset_serialize(pstate->pseen);
	if (NULL == pbin) {
		tpropval_array_free(pproplist);
		return NULL;
	}
	propval.proptag = META_TAG_CNSETSEEN;
	propval.pvalue = pbin;
	if (!tpropval_array_set_propval(pproplist, &propval)) {
		rop_util_free_binary(pbin);
		tpropval_array_free(pproplist);
		return NULL;
	}
	rop_util_free_binary(pbin);
	
	if (ICS_TYPE_CONTENTS == pstate->type) {
		pbin = idset_serialize(pstate->pseen_fai);
		if (NULL == pbin) {
			tpropval_array_free(pproplist);
			return NULL;
		}
		propval.proptag = META_TAG_CNSETSEENFAI;
		propval.pvalue = pbin;
		if (!tpropval_array_set_propval(pproplist, &propval)) {
			rop_util_free_binary(pbin);
			tpropval_array_free(pproplist);
			return NULL;
		}
		rop_util_free_binary(pbin);
	}
	
	if (ICS_TYPE_CONTENTS == pstate->type &&
		FALSE == idset_check_empty(pstate->pread)) {
		pbin = idset_serialize(pstate->pread);
		if (NULL == pbin) {
			tpropval_array_free(pproplist);
			return NULL;
		}
		propval.proptag = META_TAG_CNSETREAD;
		propval.pvalue = pbin;
		if (!tpropval_array_set_propval(pproplist, &propval)) {
			rop_util_free_binary(pbin);
			tpropval_array_free(pproplist);
			return NULL;
		}
		rop_util_free_binary(pbin);
	}
	if (!ext_push.init(nullptr, 0, 0) ||
	    ext_push.p_tpropval_a(pproplist) != EXT_ERR_SUCCESS) {
		tpropval_array_free(pproplist);
		return NULL;
	}
	tpropval_array_free(pproplist);
	pbin = cu_alloc<BINARY>();
	pbin->cb = ext_push.m_offset;
	pbin->pv = common_util_alloc(pbin->cb);
	if (pbin->pv == nullptr) {
		return NULL;
	}
	memcpy(pbin->pv, ext_push.m_udata, pbin->cb);
	return pbin;
}

BOOL ics_state_deserialize(ICS_STATE *pstate, const BINARY *pbin)
{
	int i;
	IDSET *pset;
	EXT_PULL ext_pull;
	TPROPVAL_ARRAY propvals;
	
	ics_state_clear(pstate);
	ics_state_init(pstate);
	if (pbin->cb <= 16) {
		return TRUE;
	}
	ext_pull.init(pbin->pb, pbin->cb, common_util_alloc, 0);
	if (ext_pull.g_tpropval_a(&propvals) != EXT_ERR_SUCCESS)
		return FALSE;	
	for (i=0; i<propvals.count; i++) {
		switch (propvals.ppropval[i].proptag) {
		case META_TAG_IDSETGIVEN1:
			pset = idset_init(FALSE, REPL_TYPE_ID);
			if (NULL == pset) {
				return FALSE;
			}
			if (!idset_deserialize(pset, static_cast<BINARY *>(propvals.ppropval[i].pvalue)) ||
				FALSE == idset_convert(pset)) {
				idset_free(pset);
				return FALSE;
			}
			idset_free(pstate->pgiven);
			pstate->pgiven = pset;
			break;
		case META_TAG_CNSETSEEN:
			pset = idset_init(FALSE, REPL_TYPE_ID);
			if (NULL == pset) {
				return FALSE;
			}
			if (!idset_deserialize(pset, static_cast<BINARY *>(propvals.ppropval[i].pvalue)) ||
				FALSE == idset_convert(pset)) {
				idset_free(pset);
				return FALSE;
			}
			idset_free(pstate->pseen);
			pstate->pseen = pset;
			break;
		case META_TAG_CNSETSEENFAI:
			if (ICS_TYPE_CONTENTS == pstate->type) {
				pset = idset_init(FALSE, REPL_TYPE_ID);
				if (NULL == pset) {
					return FALSE;
				}
				if (!idset_deserialize(pset, static_cast<BINARY *>(propvals.ppropval[i].pvalue)) ||
					FALSE == idset_convert(pset)) {
					idset_free(pset);
					return FALSE;
				}
				idset_free(pstate->pseen_fai);
				pstate->pseen_fai = pset;
			}
			break;
		case META_TAG_CNSETREAD:
			if (ICS_TYPE_CONTENTS == pstate->type) {
				pset = idset_init(FALSE, REPL_TYPE_ID);
				if (NULL == pset) {
					return FALSE;
				}
				if (!idset_deserialize(pset, static_cast<BINARY *>(propvals.ppropval[i].pvalue)) ||
					FALSE == idset_convert(pset)) {
					idset_free(pset);
					return FALSE;
				}
				idset_free(pstate->pread);
				pstate->pread = pset;
			}
			break;
		}
	}
	return TRUE;
}
