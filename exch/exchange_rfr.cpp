// SPDX-License-Identifier: GPL-2.0-only WITH linking exception
#define DECLARE_API_STATIC
#include <cstdint>
#include <libHX/string.h>
#include <gromox/defs.h>
#include <gromox/proc_common.h>
#include <gromox/ndr_stack.hpp>
#include <gromox/util.hpp>
#include <gromox/guid.hpp>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#define TRY(expr) do { int v = (expr); if (v != NDR_ERR_SUCCESS) return v; } while (false)

enum {
	RfrGetNewDSA = 0,
	RfrGetFQDNFromServerDN = 1,
};

namespace {

struct RFRGETNEWDSA_IN {
	uint32_t flags;
	char puserdn[1024];
	char punused[256];
	char pserver[256];
};

struct RFRGETNEWDSA_OUT {
	char punused[256];
	char pserver[256];
	uint32_t result;
};

struct RFRGETFQDNFROMLEGACYDN_IN {
	uint32_t flags;
	uint32_t cb;
	char mbserverdn[1024];
};

struct RFRGETFQDNFROMLEGACYDN_OUT {
	char serverfqdn[256];
	uint32_t result;
};

}

static int exchange_rfr_ndr_pull(int opnum, NDR_PULL* pndr, void **pin);

static int exchange_rfr_dispatch(int opnum, const GUID *pobject,
	uint64_t handle, void *pin, void **pout);

static int exchange_rfr_ndr_push(int opnum, NDR_PUSH *pndr, void *pout);

static BOOL (*get_id_from_username)(const char *username, int *puser_id);

static BOOL proc_exchange_rfr(int reason, void **ppdata)
{
	void *pendpoint1;
	void *pendpoint2;
	DCERPC_INTERFACE interface;
	
	/* path contains the config files directory */
	switch (reason) {
    case PLUGIN_INIT:
		LINK_API(ppdata);
		query_service1(get_id_from_username);
		if (NULL == get_id_from_username) {
			printf("[exchange_rfr]: failed to get service \"get_id_from_username\"\n");
			return FALSE;
		}
		pendpoint1 = register_endpoint("*", 6001);
		if (NULL == pendpoint1) {
			printf("[exchange_rfr]: failed to register endpoint with port 6001\n");
			return FALSE;
		}
		pendpoint2 = register_endpoint("*", 6002);
		if (NULL == pendpoint2) {
			printf("[exchange_rfr]: failed to register endpoint with port 6002\n");
			return FALSE;
		}
		strcpy(interface.name, "exchangeRFR");
		guid_from_string(&interface.uuid, "1544f5e0-613c-11d1-93df-00c04fd7bd09");
		interface.version = 1;
		interface.ndr_pull = exchange_rfr_ndr_pull;
		interface.dispatch = exchange_rfr_dispatch;
		interface.ndr_push = exchange_rfr_ndr_push;
		interface.unbind = NULL;
		interface.reclaim = NULL;
		if (FALSE == register_interface(pendpoint1, &interface) ||
			FALSE == register_interface(pendpoint2, &interface)) {
			printf("[exchange_rfr]: failed to register interface\n");
			return FALSE;
		}
		printf("[exchange_rfr]: plugin is loaded into system\n");
		return TRUE;
	case PLUGIN_FREE:
		return TRUE;
	}
	return TRUE;
}
PROC_ENTRY(proc_exchange_rfr);

static uint32_t rfr_get_newdsa(uint32_t flags, const char *puserdn,
    char *punused, char *pserver, size_t svlen)
{
	int user_id;
	char *ptoken;
	char username[UADDR_SIZE];
	char hex_string[32];
	
	*punused = '\0';
	auto rpc_info = get_rpc_info();
	get_id_from_username(rpc_info.username, &user_id);
	memset(username, 0, sizeof(username));
	gx_strlcpy(username, rpc_info.username, GX_ARRAY_SIZE(username));
	ptoken = strchr(username, '@');
	HX_strlower(username);
	if (ptoken != nullptr)
		ptoken ++;
	else
		ptoken = username;
	encode_hex_int(user_id, hex_string);
	snprintf(pserver, svlen, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x"
			"-%02x%02x%s@%s", username[0], username[1], username[2],
			username[3], username[4], username[5], username[6],
			username[7], username[8], username[9], username[10],
			username[11], hex_string, ptoken);
	return ecSuccess;
}

static uint32_t rfr_get_fqdnfromlegacydn(uint32_t flags, uint32_t cb,
    const char *mbserverdn, char *serverfqdn, size_t svlen)
{
	char *ptoken;
	char tmp_unused[16];
	char tmp_buff[1024];
	
	gx_strlcpy(tmp_buff, mbserverdn, GX_ARRAY_SIZE(tmp_buff));
	ptoken = strrchr(tmp_buff, '/');
	if (ptoken == nullptr || strncasecmp(ptoken, "/cn=", 4) != 0)
		return rfr_get_newdsa(flags, NULL, tmp_unused, serverfqdn, svlen);
	*ptoken = '\0';
	ptoken = strrchr(tmp_buff, '/');
	if (ptoken == nullptr || strncasecmp(ptoken, "/cn=", 4) != 0)
		return rfr_get_newdsa(flags, NULL, tmp_unused, serverfqdn, svlen);
	gx_strlcpy(serverfqdn, ptoken + 4, svlen);
	return ecSuccess;
}

static int exchange_rfr_ndr_pull(int opnum, NDR_PULL* pndr, void **ppin)
{
	uint32_t ptr;
	uint32_t size;
	uint32_t offset;
	uint32_t length;
	RFRGETNEWDSA_IN *prfr;
	RFRGETFQDNFROMLEGACYDN_IN *prfr_dn;

	
	switch (opnum) {
	case RfrGetNewDSA:
		prfr = ndr_stack_anew<RFRGETNEWDSA_IN>(NDR_STACK_IN);
		if (prfr == nullptr)
			return NDR_ERR_ALLOC;
		memset(prfr, 0, sizeof(RFRGETNEWDSA_IN));
		TRY(ndr_pull_uint32(pndr, &prfr->flags));
		TRY(ndr_pull_ulong(pndr, &size));
		TRY(ndr_pull_ulong(pndr, &offset));
		TRY(ndr_pull_ulong(pndr, &length));
		if (offset != 0 || length > size || length > 1024)
			return NDR_ERR_ARRAY_SIZE;
		TRY(ndr_pull_check_string(pndr, length, sizeof(uint8_t)));
		TRY(ndr_pull_string(pndr, prfr->puserdn, length));
		TRY(ndr_pull_generic_ptr(pndr, &ptr));
		if (0 != ptr) {
			TRY(ndr_pull_generic_ptr(pndr, &ptr));
			if (0 != ptr) {
				TRY(ndr_pull_ulong(pndr, &size));
				TRY(ndr_pull_ulong(pndr, &offset));
				TRY(ndr_pull_ulong(pndr, &length));
				if (offset != 0 || length > size || length > 256)
					return NDR_ERR_ARRAY_SIZE;
				TRY(ndr_pull_check_string(pndr, length, sizeof(uint8_t)));
				TRY(ndr_pull_string(pndr, prfr->punused, length));
			} else {
				prfr->punused[0] = '\0';
			}
		} else {
			prfr->punused[0] = '\0';
		}
		TRY(ndr_pull_generic_ptr(pndr, &ptr));
		if (0 != ptr) {
			TRY(ndr_pull_generic_ptr(pndr, &ptr));
			if (0 != ptr) {
				TRY(ndr_pull_ulong(pndr, &size));
				TRY(ndr_pull_ulong(pndr, &offset));
				TRY(ndr_pull_ulong(pndr, &length));
				if (offset != 0 || length > size || length > 256)
					return NDR_ERR_ARRAY_SIZE;
				TRY(ndr_pull_check_string(pndr, length, sizeof(uint8_t)));
				TRY(ndr_pull_string(pndr, prfr->pserver, length));
			} else {
				prfr->pserver[0] = '\0';
			}
		} else {
			prfr->pserver[0] = '\0';
		}
		*ppin = prfr;
		return NDR_ERR_SUCCESS;
	case RfrGetFQDNFromServerDN:
		prfr_dn = ndr_stack_anew<RFRGETFQDNFROMLEGACYDN_IN>(NDR_STACK_IN);
		if (prfr_dn == nullptr)
			return NDR_ERR_ALLOC;
		memset(prfr_dn, 0, sizeof(RFRGETFQDNFROMLEGACYDN_IN));
		TRY(ndr_pull_uint32(pndr, &prfr_dn->flags));
		TRY(ndr_pull_uint32(pndr, &prfr_dn->cb));
		if (prfr_dn->cb < 10 || prfr_dn->cb > 1024)
			return NDR_ERR_RANGE;
		TRY(ndr_pull_ulong(pndr, &size));
		TRY(ndr_pull_ulong(pndr, &offset));
		TRY(ndr_pull_ulong(pndr, &length));
		if (offset != 0 || length > size || length > 1024)
			return NDR_ERR_ARRAY_SIZE;
		TRY(ndr_pull_check_string(pndr, length, sizeof(uint8_t)));
		TRY(ndr_pull_string(pndr, prfr_dn->mbserverdn, length));
		*ppin = prfr_dn;
		return NDR_ERR_SUCCESS;
	default:
		return NDR_ERR_BAD_SWITCH;
	}
}

static int exchange_rfr_dispatch(int opnum, const GUID *pobject,
	uint64_t handle, void *pin, void **ppout)
{
	RFRGETNEWDSA_IN *prfr_in;
	RFRGETNEWDSA_OUT *prfr_out;
	RFRGETFQDNFROMLEGACYDN_IN *prfr_dn_in;
	RFRGETFQDNFROMLEGACYDN_OUT *prfr_dn_out;
	
	switch (opnum) {
	case RfrGetNewDSA:
		prfr_in = (RFRGETNEWDSA_IN*)pin;
		prfr_out = ndr_stack_anew<RFRGETNEWDSA_OUT>(NDR_STACK_OUT);
		if (prfr_out == nullptr)
			return DISPATCH_FAIL;
		prfr_out->result = rfr_get_newdsa(prfr_in->flags, prfr_in->puserdn,
		                   prfr_in->punused, prfr_in->pserver,
		                   GX_ARRAY_SIZE(prfr_in->pserver));
		strcpy(prfr_out->punused, prfr_in->punused);
		strcpy(prfr_out->pserver, prfr_in->pserver);
		*ppout = prfr_out;
		return DISPATCH_SUCCESS;
	case RfrGetFQDNFromServerDN:
		prfr_dn_in = (RFRGETFQDNFROMLEGACYDN_IN*)pin;
		prfr_dn_out = ndr_stack_anew<RFRGETFQDNFROMLEGACYDN_OUT>(NDR_STACK_OUT);
		if (prfr_dn_out == nullptr)
			return DISPATCH_FAIL;
		prfr_dn_out->result = rfr_get_fqdnfromlegacydn(prfr_dn_in->flags,
		                      prfr_dn_in->cb, prfr_dn_in->mbserverdn,
		                      prfr_dn_out->serverfqdn,
		                      GX_ARRAY_SIZE(prfr_dn_out->serverfqdn));
		*ppout = prfr_dn_out;
		return DISPATCH_SUCCESS;
	default:
		return DISPATCH_FAIL;
	}
}

static int exchange_rfr_ndr_push(int opnum, NDR_PUSH *pndr, void *pout)
{
	int length;
	RFRGETNEWDSA_OUT *prfr;
	RFRGETFQDNFROMLEGACYDN_OUT *prfr_dn;
	
	switch (opnum) {
	case RfrGetNewDSA:
		prfr = (RFRGETNEWDSA_OUT*)pout;
		if ('\0' == *prfr->punused) {
			TRY(ndr_push_unique_ptr(pndr, nullptr));
		} else {
			TRY(ndr_push_unique_ptr(pndr, reinterpret_cast<void *>(0x1)));
			length = strlen(prfr->punused) + 1;
			TRY(ndr_push_unique_ptr(pndr, prfr->punused));
			TRY(ndr_push_ulong(pndr, length));
			TRY(ndr_push_ulong(pndr, 0));
			TRY(ndr_push_ulong(pndr, length));
			TRY(ndr_push_string(pndr, prfr->punused, length));
		}
		
		if ('\0' == *prfr->pserver) {
			TRY(ndr_push_unique_ptr(pndr, nullptr));
		} else {
			TRY(ndr_push_unique_ptr(pndr, reinterpret_cast<void *>(0x2)));
			length = strlen(prfr->pserver) + 1;
			TRY(ndr_push_unique_ptr(pndr, prfr->pserver));
			TRY(ndr_push_ulong(pndr, length));
			TRY(ndr_push_ulong(pndr, 0));
			TRY(ndr_push_ulong(pndr, length));
			TRY(ndr_push_string(pndr, prfr->pserver, length));
		}
		return ndr_push_uint32(pndr, prfr->result);
	case RfrGetFQDNFromServerDN:
		prfr_dn = (RFRGETFQDNFROMLEGACYDN_OUT*)pout;
		if ('\0' == *prfr_dn->serverfqdn) {
			TRY(ndr_push_unique_ptr(pndr, nullptr));
		} else {
			length = strlen(prfr_dn->serverfqdn) + 1;
			TRY(ndr_push_unique_ptr(pndr, prfr_dn->serverfqdn));
			TRY(ndr_push_ulong(pndr, length));
			TRY(ndr_push_ulong(pndr, 0));
			TRY(ndr_push_ulong(pndr, length));
			TRY(ndr_push_string(pndr, prfr_dn->serverfqdn, length));
		}
		return ndr_push_uint32(pndr, prfr_dn->result);
	}
	return NDR_ERR_SUCCESS;
}
