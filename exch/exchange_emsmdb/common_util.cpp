// SPDX-License-Identifier: GPL-2.0-only WITH linking exception
#ifdef HAVE_CONFIG_H
#	include "config.h"
#endif
#include <cstdint>
#include <mutex>
#include <libHX/ctype_helper.h>
#include <libHX/string.h>
#include <gromox/defs.h>
#include <gromox/fileio.h>
#include <gromox/mapidefs.h>
#include <gromox/socket.h>
#include <gromox/pcl.hpp>
#include <gromox/util.hpp>
#include <gromox/oxcmail.hpp>
#include <gromox/rop_util.hpp>
#include <gromox/ext_buffer.hpp>
#include <gromox/proc_common.h>
#include "common_util.h"
#include "exmdb_client.h"
#include <gromox/element_data.hpp>
#include <gromox/proptag_array.hpp>
#include "bounce_producer.h"
#include "emsmdb_interface.h"
#include <ctime>
#include <cerrno>
#include <cstdio>
#include <iconv.h>
#include <fcntl.h>
#include <cstdarg>
#include <cstdlib>
#include <unistd.h>
#include <cstring>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>

#define SOCKET_TIMEOUT						60

using namespace gromox;

enum {
	SMTP_SEND_OK = 0,
	SMTP_CANNOT_CONNECT,
	SMTP_CONNECT_ERROR,
	SMTP_TIME_OUT,
	SMTP_TEMP_ERROR,
	SMTP_UNKOWN_RESPONSE,
	SMTP_PERMANENT_ERROR
};

static int g_max_rcpt;
static uint16_t g_smtp_port;
static int g_max_message;
static char g_smtp_ip[40], g_emsmdb_org_name[256];
static int g_faststream_id;
static int g_average_blocks;
static MIME_POOL *g_mime_pool;
static pthread_key_t g_dir_key;
static std::mutex g_id_lock;
static char g_submit_command[1024];
static unsigned int g_max_mail_len;
static unsigned int g_max_rule_len;
static LIB_BUFFER *g_file_allocator;

#define E(s) decltype(common_util_ ## s) common_util_ ## s;
E(get_maildir)
E(get_homedir)
E(get_user_displayname)
E(check_mlist_include)
E(get_user_lang)
E(get_timezone)
E(get_username_from_id)
E(get_id_from_username)
E(get_user_ids)
E(get_domain_ids)
E(check_same_org)
E(get_homedir_by_id)
E(get_domainname_from_id)
E(get_id_from_maildir)
E(get_id_from_homedir)
E(lang_to_charset)
E(cpid_to_charset)
E(verify_cpid)
E(add_timer)
E(cancel_timer)
#undef E

static const char *(*common_util_lcid_to_ltag)(uint32_t lcid);
static uint32_t (*common_util_charset_to_cpid)(const char *charset);
static uint32_t (*common_util_ltag_to_lcid)(const char *ltag);
static const char* (*common_util_mime_to_extension)(const char *ptype);

static const char* (*common_util_extension_to_mime)(const char *pext);
static void log_err(const char *format, ...) __attribute__((format(printf, 1, 2)));

void* common_util_alloc(size_t size)
{
	return ndr_stack_alloc(NDR_STACK_IN, size);
}

int common_util_mb_from_utf8(uint32_t cpid,
	const char *src, char *dst, size_t len)
{
	size_t in_len;
	size_t out_len;
	char *pin, *pout;
	iconv_t conv_id;
	const char *charset;
	char temp_charset[256];
	
	charset = common_util_cpid_to_charset(cpid);
	if (NULL == charset) {
		return -1;
	}
	sprintf(temp_charset, "%s//IGNORE",
		replace_iconv_charset(charset));
	conv_id = iconv_open(temp_charset, "UTF-8");
	pin = (char*)src;
	pout = dst;
	in_len = strlen(src) + 1;
	memset(dst, 0, len);
	out_len = len;
	iconv(conv_id, &pin, &in_len, &pout, &len);
	iconv_close(conv_id);
	return out_len - len;
}

int common_util_mb_to_utf8(uint32_t cpid,
	const char *src, char *dst, size_t len)
{
	size_t in_len;
	size_t out_len;
	char *pin, *pout;
	iconv_t conv_id;
	const char *charset;
	
	charset = common_util_cpid_to_charset(cpid);
	if (NULL == charset) {
		return -1;
	}
	conv_id = iconv_open("UTF-8//IGNORE",
		replace_iconv_charset(charset));
	pin = (char*)src;
	pout = dst;
	in_len = strlen(src) + 1;
	memset(dst, 0, len);
	out_len = len;
	iconv(conv_id, &pin, &in_len, &pout, &len);	
	iconv_close(conv_id);
	return out_len - len;
}

static char* common_util_dup_mb_to_utf8(
	uint32_t cpid, const char *src)
{
	int len;
	char *pdst;
	
	len = 2*strlen(src) + 1;
	pdst = cu_alloc<char>(len);
	if (NULL == pdst) {
		return NULL;
	}
	if (common_util_mb_to_utf8(cpid, src, pdst, len) < 0) {
		return NULL;
	}
	return pdst;
}

/* only for being invoked under rop environment */
int common_util_convert_string(BOOL to_utf8,
	const char *src, char *dst, size_t len)
{
	auto pinfo = emsmdb_interface_get_emsmdb_info();
	if (NULL == pinfo) {
		return -1;
	}
	return to_utf8 ? common_util_mb_to_utf8(pinfo->cpid, src, dst, len) :
	       common_util_mb_from_utf8(pinfo->cpid, src, dst, len);
}

void common_util_obfuscate_data(uint8_t *data, uint32_t size)
{
	uint32_t i;

	for (i=0; i<size; i++) {
		data[i] ^= 0xA5;
	}
}

BOOL common_util_essdn_to_username(const char *pessdn,
    char *username, size_t ulen)
{
	char *pat;
	int user_id;
	const char *plocal;
	char tmp_essdn[1024];
	
	auto tmp_len = gx_snprintf(tmp_essdn, GX_ARRAY_SIZE(tmp_essdn),
			"/o=%s/ou=Exchange Administrative Group "
			"(FYDIBOHF23SPDLT)/cn=Recipients/cn=",
	               g_emsmdb_org_name);
	if (0 != strncasecmp(pessdn, tmp_essdn, tmp_len)) {
		return FALSE;
	}
	if ('-' != pessdn[tmp_len + 16]) {
		return FALSE;
	}
	plocal = pessdn + tmp_len + 17;
	user_id = decode_hex_int(pessdn + tmp_len + 8);
	if (!common_util_get_username_from_id(user_id, username, ulen))
		return FALSE;
	pat = strchr(username, '@');
	if (NULL == pat) {
		return FALSE;
	}
	if (0 != strncasecmp(username, plocal, pat - username)) {
		return FALSE;
	}
	return TRUE;
}

BOOL common_util_username_to_essdn(const char *username, char *pessdn, size_t dnmax)
{
	int user_id;
	int domain_id;
	char *pdomain;
	int address_type;
	char tmp_name[UADDR_SIZE];
	char hex_string[16];
	char hex_string2[16];
	
	gx_strlcpy(tmp_name, username, GX_ARRAY_SIZE(tmp_name));
	pdomain = strchr(tmp_name, '@');
	if (NULL == pdomain) {
		return FALSE;
	}
	*pdomain = '\0';
	pdomain ++;
	if (FALSE == common_util_get_user_ids(username,
		&user_id, &domain_id, &address_type)) {
		return FALSE;
	}
	encode_hex_int(user_id, hex_string);
	encode_hex_int(domain_id, hex_string2);
	snprintf(pessdn, dnmax, "/o=%s/ou=Exchange Administrative Group "
			"(FYDIBOHF23SPDLT)/cn=Recipients/cn=%s%s-%s",
		g_emsmdb_org_name, hex_string2, hex_string, tmp_name);
	HX_strupper(pessdn);
	return TRUE;
}

BOOL common_util_essdn_to_public(const char *pessdn, char *domainname)
{
	//TODO
	return FALSE;
}

BOOL common_util_public_to_essdn(const char *username, char *pessdn, size_t dnmax)
{
	//TODO
	return FALSE;
}

const char* common_util_essdn_to_domain(const char *pessdn)
{
	int tmp_len;
	char tmp_essdn[1024];
	
	tmp_len = sprintf(tmp_essdn,
		"/o=%s/ou=Exchange Administrative Group "
		"(FYDIBOHF23SPDLT)/cn=Configuration/cn=Servers/cn="
		"f98430ae-22ad-459a-afba-68c972eefc56@", g_emsmdb_org_name);
	if (0 != strncasecmp(pessdn, tmp_essdn, tmp_len)) {
		return NULL;
	}
	return pessdn + tmp_len;
}

void common_util_domain_to_essdn(const char *pdomain, char *pessdn, size_t dnmax)
{
	snprintf(pessdn, dnmax, "/o=%s/ou=Exchange Administrative Group "
		"(FYDIBOHF23SPDLT)/cn=Configuration/cn=Servers/cn="
		"f98430ae-22ad-459a-afba-68c972eefc56@%s", g_emsmdb_org_name, pdomain);
}

BOOL common_util_entryid_to_username(const BINARY *pbin,
    char *username, size_t ulen)
{
	uint32_t flags;
	EXT_PULL ext_pull;
	uint8_t tmp_uid[16];
	uint8_t provider_uid[16];
	ONEOFF_ENTRYID oneoff_entry;
	ADDRESSBOOK_ENTRYID ab_entryid;
	
	if (pbin->cb < 20) {
		return FALSE;
	}
	ext_pull.init(pbin->pb, 20, common_util_alloc, 0);
	if (ext_pull.g_uint32(&flags) != EXT_ERR_SUCCESS || flags != 0 ||
	    ext_pull.g_bytes(provider_uid, arsizeof(provider_uid)) != EXT_ERR_SUCCESS)
		return FALSE;	
	rop_util_get_provider_uid(PROVIDER_UID_ADDRESS_BOOK, tmp_uid);
	if (0 == memcmp(tmp_uid, provider_uid, 16)) {
		ext_pull.init(pbin->pb, pbin->cb, common_util_alloc, EXT_FLAG_UTF16);
		if (ext_pull.g_abk_eid(&ab_entryid) != EXT_ERR_SUCCESS)
			return FALSE;	
		if (ADDRESSBOOK_ENTRYID_TYPE_LOCAL_USER != ab_entryid.type) {
			return FALSE;
		}
		return common_util_essdn_to_username(ab_entryid.px500dn,
		       username, ulen);
	}
	rop_util_get_provider_uid(PROVIDER_UID_ONE_OFF, tmp_uid);
	if (0 == memcmp(tmp_uid, provider_uid, 16)) {
		ext_pull.init(pbin->pb, pbin->cb, common_util_alloc, EXT_FLAG_UTF16);
		if (ext_pull.g_oneoff_eid(&oneoff_entry) != EXT_ERR_SUCCESS)
			return FALSE;	
		if (0 != strcasecmp(oneoff_entry.paddress_type, "SMTP")) {
			return FALSE;
		}
		strncpy(username, oneoff_entry.pmail_address, 128);
		return TRUE;
	}
	return FALSE;
}

void common_util_get_domain_server(const char *account_name, char *pserver)
{
	sprintf(pserver, "f98430ae-22ad-459a-afba-68c972eefc56@%s", account_name);
}

BINARY* common_util_username_to_addressbook_entryid(const char *username)
{
	char x500dn[1024];
	EXT_PUSH ext_push;
	ADDRESSBOOK_ENTRYID tmp_entryid;
	
	if (!common_util_username_to_essdn(username, x500dn, GX_ARRAY_SIZE(x500dn)))
		return NULL;
	tmp_entryid.flags = 0;
	rop_util_get_provider_uid(PROVIDER_UID_ADDRESS_BOOK,
							tmp_entryid.provider_uid);
	tmp_entryid.version = 1;
	tmp_entryid.type = ADDRESSBOOK_ENTRYID_TYPE_LOCAL_USER;
	tmp_entryid.px500dn = x500dn;
	auto pbin = cu_alloc<BINARY>();
	if (NULL == pbin) {
		return NULL;
	}
	pbin->pv = common_util_alloc(1280);
	if (pbin->pv == nullptr ||
	    !ext_push.init(pbin->pv, 1280, EXT_FLAG_UTF16) ||
	    ext_push.p_abk_eid(&tmp_entryid) != EXT_ERR_SUCCESS)
		return NULL;	
	pbin->cb = ext_push.m_offset;
	return pbin;
}

BINARY* common_util_public_to_addressbook_entryid(const char *domainname)
{
	char x500dn[1024];
	EXT_PUSH ext_push;
	ADDRESSBOOK_ENTRYID tmp_entryid;
	
	if (!common_util_public_to_essdn(domainname, x500dn, GX_ARRAY_SIZE(x500dn)))
		return NULL;
	tmp_entryid.flags = 0;
	rop_util_get_provider_uid(PROVIDER_UID_ADDRESS_BOOK,
							tmp_entryid.provider_uid);
	tmp_entryid.version = 1;
	tmp_entryid.type = ADDRESSBOOK_ENTRYID_TYPE_LOCAL_USER;
	tmp_entryid.px500dn = x500dn;
	auto pbin = cu_alloc<BINARY>();
	if (NULL == pbin) {
		return NULL;
	}
	pbin->pv = common_util_alloc(1280);
	if (pbin->pv == nullptr ||
	    !ext_push.init(pbin->pv, 1280, EXT_FLAG_UTF16) ||
	    ext_push.p_abk_eid(&tmp_entryid) != EXT_ERR_SUCCESS)
		return NULL;	
	pbin->cb = ext_push.m_offset;
	return pbin;
}

BINARY* common_util_to_folder_entryid(
	LOGON_OBJECT *plogon, uint64_t folder_id)
{
	BOOL b_found;
	BINARY tmp_bin;
	uint16_t replid;
	EXT_PUSH ext_push;
	FOLDER_ENTRYID tmp_entryid;
	
	tmp_entryid.flags = 0;
	if (plogon->check_private()) {
		tmp_bin.cb = 0;
		tmp_bin.pb = tmp_entryid.provider_uid;
		rop_util_guid_to_binary(plogon->mailbox_guid, &tmp_bin);
		tmp_entryid.database_guid = rop_util_make_user_guid(plogon->account_id);
		tmp_entryid.folder_type = EITLT_PRIVATE_FOLDER;
	} else {
		rop_util_get_provider_uid(PROVIDER_UID_PUBLIC,
							tmp_entryid.provider_uid);
		replid = rop_util_get_replid(folder_id);
		if (1 != replid) {
			if (!exmdb_client_get_mapping_guid(plogon->get_dir(),
			    replid, &b_found, &tmp_entryid.database_guid))
				return NULL;	
			if (FALSE == b_found) {
				return NULL;
			}
		} else {
			tmp_entryid.database_guid = rop_util_make_domain_guid(plogon->account_id);
		}
		tmp_entryid.folder_type = EITLT_PUBLIC_FOLDER;
	}
	rop_util_get_gc_array(folder_id, tmp_entryid.global_counter);
	tmp_entryid.pad[0] = 0;
	tmp_entryid.pad[1] = 0;
	auto pbin = cu_alloc<BINARY>();
	if (NULL == pbin) {
		return NULL;
	}
	pbin->pv = common_util_alloc(256);
	if (pbin->pv == nullptr || !ext_push.init(pbin->pv, 256, 0) ||
	    ext_push.p_folder_eid(&tmp_entryid) != EXT_ERR_SUCCESS)
		return NULL;	
	pbin->cb = ext_push.m_offset;
	return pbin;
}

BINARY* common_util_calculate_folder_sourcekey(
	LOGON_OBJECT *plogon, uint64_t folder_id)
{
	BOOL b_found;
	uint16_t replid;
	EXT_PUSH ext_push;
	LONG_TERM_ID longid;
	
	auto pbin = cu_alloc<BINARY>();
	if (NULL == pbin) {
		return NULL;
	}
	pbin->cb = 22;
	pbin->pv = common_util_alloc(22);
	if (pbin->pv == nullptr)
		return NULL;
	if (plogon->check_private()) {
		longid.guid = rop_util_make_user_guid(plogon->account_id);
	} else {
		replid = rop_util_get_replid(folder_id);
		if (1 == replid) {
			longid.guid = rop_util_make_domain_guid(plogon->account_id);
		} else {
			if (!exmdb_client_get_mapping_guid(plogon->get_dir(),
			    replid, &b_found, &longid.guid))
				return NULL;	
			if (FALSE == b_found) {
				return NULL;
			}
		}	
	}
	rop_util_get_gc_array(folder_id, longid.global_counter);
	if (!ext_push.init(pbin->pv, 22, 0) ||
	    ext_push.p_guid(&longid.guid) != EXT_ERR_SUCCESS ||
	    ext_push.p_bytes(longid.global_counter, 6) != EXT_ERR_SUCCESS)
		return NULL;
	return pbin;
}

BINARY* common_util_to_message_entryid(LOGON_OBJECT *plogon,
	uint64_t folder_id, uint64_t message_id)
{
	BOOL b_found;
	BINARY tmp_bin;
	uint16_t replid;
	EXT_PUSH ext_push;
	MESSAGE_ENTRYID tmp_entryid;
	
	tmp_entryid.flags = 0;
	if (plogon->check_private()) {
		tmp_bin.cb = 0;
		tmp_bin.pb = tmp_entryid.provider_uid;
		rop_util_guid_to_binary(plogon->mailbox_guid, &tmp_bin);
		tmp_entryid.folder_database_guid = rop_util_make_user_guid(plogon->account_id);
		tmp_entryid.message_type = EITLT_PRIVATE_MESSAGE;
	} else {
		rop_util_get_provider_uid(PROVIDER_UID_PUBLIC,
							tmp_entryid.provider_uid);
		replid = rop_util_get_replid(folder_id);
		if (1 != replid) {
			if (!exmdb_client_get_mapping_guid(plogon->get_dir(),
			    replid, &b_found, &tmp_entryid.folder_database_guid))
				return NULL;	
			if (FALSE == b_found) {
				return NULL;
			}
		} else {
			tmp_entryid.folder_database_guid = rop_util_make_domain_guid(plogon->account_id);
		}
		tmp_entryid.message_type = EITLT_PUBLIC_MESSAGE;
	}
	tmp_entryid.message_database_guid = tmp_entryid.folder_database_guid;
	rop_util_get_gc_array(folder_id, tmp_entryid.folder_global_counter);
	rop_util_get_gc_array(message_id, tmp_entryid.message_global_counter);
	tmp_entryid.pad1[0] = 0;
	tmp_entryid.pad1[1] = 0;
	tmp_entryid.pad2[0] = 0;
	tmp_entryid.pad2[1] = 0;
	auto pbin = cu_alloc<BINARY>();
	if (NULL == pbin) {
		return NULL;
	}
	pbin->pv = common_util_alloc(256);
	if (pbin->pv == nullptr || !ext_push.init(pbin->pv, 256, 0) ||
	    ext_push.p_msg_eid(&tmp_entryid) != EXT_ERR_SUCCESS)
		return NULL;	
	pbin->cb = ext_push.m_offset;
	return pbin;
}

BINARY* common_util_calculate_message_sourcekey(
	LOGON_OBJECT *plogon, uint64_t message_id)
{
	EXT_PUSH ext_push;
	LONG_TERM_ID longid;
	
	auto pbin = cu_alloc<BINARY>();
	if (NULL == pbin) {
		return NULL;
	}
	pbin->cb = 22;
	pbin->pv = common_util_alloc(22);
	if (pbin->pv == nullptr)
		return NULL;
	longid.guid = plogon->guid();
	rop_util_get_gc_array(message_id, longid.global_counter);
	if (!ext_push.init(pbin->pv, 22, 0) ||
	    ext_push.p_guid(&longid.guid) != EXT_ERR_SUCCESS ||
	    ext_push.p_bytes(longid.global_counter, 6) != EXT_ERR_SUCCESS)
		return NULL;
	return pbin;
}

BOOL common_util_from_folder_entryid(LOGON_OBJECT *plogon,
	BINARY *pbin, uint64_t *pfolder_id)
{
	BOOL b_found;
	uint16_t replid;
	EXT_PULL ext_pull;
	FOLDER_ENTRYID tmp_entryid;
	
	ext_pull.init(pbin->pb, pbin->cb, common_util_alloc, 0);
	if (ext_pull.g_folder_eid(&tmp_entryid) != EXT_ERR_SUCCESS)
		return FALSE;	
	switch (tmp_entryid.folder_type) {
	case EITLT_PRIVATE_FOLDER: {
		if (!plogon->check_private())
			return FALSE;
		auto tmp_guid = rop_util_make_user_guid(plogon->account_id);
		if (0 != memcmp(&tmp_entryid.database_guid,
			&tmp_guid, sizeof(GUID))) {
			return FALSE;	
		}
		*pfolder_id = rop_util_make_eid(1,
				tmp_entryid.global_counter);
		return TRUE;
	}
	case EITLT_PUBLIC_FOLDER: {
		if (plogon->check_private())
			return FALSE;
		auto tmp_guid = rop_util_make_domain_guid(plogon->account_id);
		if (0 == memcmp(&tmp_entryid.database_guid,
			&tmp_guid, sizeof(GUID))) {
			*pfolder_id = rop_util_make_eid(1,
					tmp_entryid.global_counter);
			return TRUE;
		}
		if (!exmdb_client_get_mapping_replid(plogon->get_dir(),
		    tmp_entryid.database_guid, &b_found, &replid) ||
		    !b_found)
			return FALSE;
		*pfolder_id = rop_util_make_eid(replid,
					tmp_entryid.global_counter);
		return TRUE;
	}
	default:
		return FALSE;
	}
}

BOOL common_util_from_message_entryid(LOGON_OBJECT *plogon,
	BINARY *pbin, uint64_t *pfolder_id, uint64_t *pmessage_id)
{
	BOOL b_found;
	uint16_t replid;
	EXT_PULL ext_pull;
	MESSAGE_ENTRYID tmp_entryid;
	
	ext_pull.init(pbin->pb, pbin->cb, common_util_alloc, 0);
	if (ext_pull.g_msg_eid(&tmp_entryid) != EXT_ERR_SUCCESS)
		return FALSE;	
	if (0 != memcmp(&tmp_entryid.folder_database_guid,
		&tmp_entryid.message_database_guid, sizeof(GUID))) {
		return FALSE;
	}
	switch (tmp_entryid.message_type) {
	case EITLT_PRIVATE_MESSAGE: {
		if (!plogon->check_private())
			return FALSE;
		auto tmp_guid = rop_util_make_user_guid(plogon->account_id);
		if (0 != memcmp(&tmp_entryid.folder_database_guid,
			&tmp_guid, sizeof(GUID))) {
			return FALSE;	
		}
		*pfolder_id = rop_util_make_eid(1,
			tmp_entryid.folder_global_counter);
		*pmessage_id = rop_util_make_eid(1,
			tmp_entryid.message_global_counter);
		return TRUE;
	}
	case EITLT_PUBLIC_MESSAGE: {
		if (plogon->check_private())
			return FALSE;
		auto tmp_guid = rop_util_make_domain_guid(plogon->account_id);
		if (0 == memcmp(&tmp_entryid.folder_database_guid,
			&tmp_guid, sizeof(GUID))) {
			*pfolder_id = rop_util_make_eid(1,
				tmp_entryid.folder_global_counter);
			*pmessage_id = rop_util_make_eid(1,
				tmp_entryid.message_global_counter);
			return TRUE;
		}
		if (!exmdb_client_get_mapping_replid(plogon->get_dir(),
		    tmp_entryid.folder_database_guid, &b_found, &replid) ||
		    !b_found)
			return FALSE;
		*pfolder_id = rop_util_make_eid(replid,
			tmp_entryid.folder_global_counter);
		*pmessage_id = rop_util_make_eid(replid,
			tmp_entryid.message_global_counter);
		return TRUE;
	}
	default:
		return FALSE;
	}
	
}

BINARY* common_util_xid_to_binary(uint8_t size, const XID *pxid)
{
	EXT_PUSH ext_push;
	
	auto pbin = cu_alloc<BINARY>();
	if (NULL == pbin) {
		return NULL;
	}
	pbin->pv = common_util_alloc(24);
	if (pbin->pv == nullptr || !ext_push.init(pbin->pv, 24, 0) ||
	    ext_push.p_xid(size, pxid) != EXT_ERR_SUCCESS)
		return NULL;	
	pbin->cb = ext_push.m_offset;
	return pbin;
}

BOOL common_util_binary_to_xid(const BINARY *pbin, XID *pxid)
{
	EXT_PULL ext_pull;
	
	if (pbin->cb < 17 || pbin->cb > 24) {
		return FALSE;
	}
	ext_pull.init(pbin->pb, pbin->cb, common_util_alloc, 0);
	return ext_pull.g_xid(pbin->cb, pxid) == EXT_ERR_SUCCESS ? TRUE : false;
}

BINARY* common_util_guid_to_binary(GUID guid)
{
	auto pbin = cu_alloc<BINARY>();
	if (NULL == pbin) {
		return NULL;
	}
	pbin->cb = 0;
	pbin->pv = common_util_alloc(16);
	if (pbin->pv == nullptr)
		return NULL;
	rop_util_guid_to_binary(guid, pbin);
	return pbin;
}

BOOL common_util_pcl_compare(const BINARY *pbin_pcl1,
	const BINARY *pbin_pcl2, uint32_t *presult)
{
	PCL *ppcl1;
	PCL *ppcl2;
	
	ppcl1 = pcl_init();
	if (NULL == ppcl1) {
		return FALSE;
	}
	ppcl2 = pcl_init();
	if (NULL == ppcl2) {
		pcl_free(ppcl1);
		return FALSE;
	}
	if (FALSE == pcl_deserialize(ppcl1, pbin_pcl1) ||
		FALSE == pcl_deserialize(ppcl2, pbin_pcl2)) {
		pcl_free(ppcl1);
		pcl_free(ppcl2);
		return FALSE;
	}
	*presult = pcl_compare(ppcl1, ppcl2);
	pcl_free(ppcl1);
	pcl_free(ppcl2);
	return TRUE;
}

BINARY* common_util_pcl_append(const BINARY *pbin_pcl,
	const BINARY *pchange_key)
{
	PCL *ppcl;
	SIZED_XID xid;
	BINARY *ptmp_bin;
	
	auto pbin = cu_alloc<BINARY>();
	if (NULL == pbin) {
		return NULL;
	}
	ppcl = pcl_init();
	if (NULL == ppcl) {
		return NULL;
	}
	if (NULL != pbin_pcl) {
		if (FALSE == pcl_deserialize(ppcl, pbin_pcl)) {
			pcl_free(ppcl);
			return NULL;
		}
	}
	xid.size = pchange_key->cb;
	if (FALSE == common_util_binary_to_xid(pchange_key, &xid.xid)) {
		pcl_free(ppcl);
		return NULL;
	}
	if (FALSE == pcl_append(ppcl, &xid)) {
		pcl_free(ppcl);
		return NULL;
	}
	ptmp_bin = pcl_serialize(ppcl);
	pcl_free(ppcl);
	if (NULL == ptmp_bin) {
		return NULL;
	}
	pbin->cb = ptmp_bin->cb;
	pbin->pv = common_util_alloc(ptmp_bin->cb);
	if (pbin->pv == nullptr) {
		rop_util_free_binary(ptmp_bin);
		return NULL;
	}
	memcpy(pbin->pv, ptmp_bin->pv, pbin->cb);
	rop_util_free_binary(ptmp_bin);
	return pbin;
}

BINARY* common_util_pcl_merge(const BINARY *pbin_pcl1,
	const BINARY *pbin_pcl2)
{
	PCL *ppcl1;
	PCL *ppcl2;
	BINARY *ptmp_bin;
	
	auto pbin = cu_alloc<BINARY>();
	if (NULL == pbin) {
		return NULL;
	}
	ppcl1 = pcl_init();
	if (NULL == ppcl1) {
		return NULL;
	}
	if (FALSE == pcl_deserialize(ppcl1, pbin_pcl1)) {
		pcl_free(ppcl1);
		return NULL;
	}
	ppcl2 = pcl_init();
	if (NULL == ppcl2) {
		pcl_free(ppcl1);
		return NULL;
	}
	if (FALSE == pcl_deserialize(ppcl2, pbin_pcl2)) {
		pcl_free(ppcl1);
		pcl_free(ppcl2);
		return NULL;
	}
	if (FALSE == pcl_merge(ppcl1, ppcl2)) {
		pcl_free(ppcl1);
		pcl_free(ppcl2);
		return NULL;
	}
	ptmp_bin = pcl_serialize(ppcl1);
	pcl_free(ppcl1);
	pcl_free(ppcl2);
	if (NULL == ptmp_bin) {
		return NULL;
	}
	pbin->cb = ptmp_bin->cb;
	pbin->pv = common_util_alloc(ptmp_bin->cb);
	if (pbin->pv == nullptr) {
		rop_util_free_binary(ptmp_bin);
		return NULL;
	}
	memcpy(pbin->pv, ptmp_bin->pv, pbin->cb);
	rop_util_free_binary(ptmp_bin);
	return pbin;
}

BINARY* common_util_to_folder_replica(
	const LONG_TERM_ID *plongid, const char *essdn)
{
	EXT_PUSH ext_push;
	
	auto pbin = cu_alloc<BINARY>();
	if (NULL == pbin) {
		return NULL;
	}
	pbin->pv = common_util_alloc(1024);
	if (pbin->pv == nullptr || !ext_push.init(pbin->pv, 1024, 0) ||
	    ext_push.p_uint32(0) != EXT_ERR_SUCCESS ||
	    ext_push.p_uint32(0) != EXT_ERR_SUCCESS ||
	    ext_push.p_longterm(plongid) != EXT_ERR_SUCCESS ||
	    ext_push.p_uint32(1) != EXT_ERR_SUCCESS ||
	    ext_push.p_uint32(1) != EXT_ERR_SUCCESS ||
	    ext_push.p_str(essdn) != EXT_ERR_SUCCESS)
		return NULL;
	pbin->cb = ext_push.m_offset;
	return pbin;
}

/* [MS-OXCSTOR] section 2.2.1.2.1.1 and 2.2.1.3.1.2 */
BOOL common_util_check_message_class(const char *str_class)
{
	int i;
	int len;
	
	len = strlen(str_class);
	if (len + 1 > 255) {
		return FALSE;
	}
	for (i=0; i<len; i++) {
		if (str_class[i] < 32 || str_class[i] > 126) {
			return FALSE;
		}
		if ('.' == str_class[i] && '.' == str_class[i + 1]) {
			return FALSE;
		}
	}
	if ('.' == str_class[0] || '.' == str_class[len - 1]) {
		return FALSE;
	}
	return TRUE;
}

GUID common_util_get_mapping_guid(BOOL b_private, int account_id)
{
	account_id *= -1;
	return b_private ? rop_util_make_user_guid(account_id) :
	       rop_util_make_domain_guid(account_id);
}

BOOL common_util_mapping_replica(BOOL to_guid,
	void *pparam, uint16_t *preplid, GUID *pguid)
{
	BOOL b_found;
	LOGON_OBJECT *plogon;
	
	plogon = *(LOGON_OBJECT**)pparam;
	if (TRUE == to_guid) {
		if (plogon->check_private()) {
			if (1 != *preplid) {
				return FALSE;
			}
			*pguid = rop_util_make_user_guid(plogon->account_id);
		} else {
			if (1 == *preplid) {
				*pguid = rop_util_make_domain_guid(plogon->account_id);
			} else if (!exmdb_client_get_mapping_guid(plogon->get_dir(),
			    *preplid, &b_found, pguid) || !b_found) {
				return FALSE;
			}
		}
	} else {
		if (plogon->check_private()) {
			auto tmp_guid = rop_util_make_user_guid(plogon->account_id);
			if (0 != memcmp(pguid, &tmp_guid, sizeof(GUID))) {
				return FALSE;
			}
			*preplid = 1;
		} else {
			auto tmp_guid = rop_util_make_domain_guid(plogon->account_id);
			if (memcmp(pguid, &tmp_guid, sizeof(GUID)) == 0)
				*preplid = 1;
			else if (!exmdb_client_get_mapping_replid(plogon->get_dir(),
			    *pguid, &b_found, preplid) || !b_found)
				return FALSE;
		}
	}
	return TRUE;
}

void common_util_set_propvals(TPROPVAL_ARRAY *parray,
	const TAGGED_PROPVAL *ppropval)
{
	int i;
	
	for (i=0; i<parray->count; i++) {
		if (ppropval->proptag == parray->ppropval[i].proptag) {
			parray->ppropval[i].pvalue = ppropval->pvalue;
			return;
		}
	}
	parray->ppropval[parray->count] = *ppropval;
	parray->count ++;
}

void common_util_remove_propvals(
	TPROPVAL_ARRAY *parray, uint32_t proptag)
{
	int i;
	
	for (i=0; i<parray->count; i++) {
		if (proptag == parray->ppropval[i].proptag) {
			parray->count --;
			if (i < parray->count) {
				memmove(parray->ppropval + i, parray->ppropval + i + 1,
					(parray->count - i) * sizeof(TAGGED_PROPVAL));
			}
			return;
		}
	}
}

void* common_util_get_propvals(
	const TPROPVAL_ARRAY *parray, uint32_t proptag)
{
	int i;
	
	for (i=0; i<parray->count; i++) {
		if (proptag == parray->ppropval[i].proptag) {
			return (void*)parray->ppropval[i].pvalue;
		}
	}
	return NULL;
}

BOOL common_util_retag_propvals(TPROPVAL_ARRAY *parray,
	uint32_t orignal_proptag, uint32_t new_proptag)
{
	int i;
	
	for (i=0; i<parray->count; i++) {
		if (orignal_proptag == parray->ppropval[i].proptag) {
			parray->ppropval[i].proptag = new_proptag;
			return TRUE;
		}
	}
	return FALSE;
}

void common_util_reduce_proptags(PROPTAG_ARRAY *pproptags_minuend,
	const PROPTAG_ARRAY *pproptags_subtractor)
{
	int i, j;
	
	for (j=0; j<pproptags_subtractor->count; j++) {
		for (i=0; i<pproptags_minuend->count; i++) {
			if (pproptags_subtractor->pproptag[j] ==
				pproptags_minuend->pproptag[i]) {
				pproptags_minuend->count --;
				if (i < pproptags_minuend->count) {
					memmove(pproptags_minuend->pproptag + i,
						pproptags_minuend->pproptag + i + 1,
						(pproptags_minuend->count - i) *
						sizeof(uint32_t));
				}
				break;
			}
		}
	}
}

int common_util_index_proptags(
	const PROPTAG_ARRAY *pproptags, uint32_t proptag)
{
	int i;
	
	for (i=0; i<pproptags->count; i++) {
		if (proptag == pproptags->pproptag[i]) {
			return i;
		}
	}
	return -1;
}

PROPTAG_ARRAY* common_util_trim_proptags(const PROPTAG_ARRAY *pproptags)
{
	int i;
	
	auto ptmp_proptags = cu_alloc<PROPTAG_ARRAY>();
	if (NULL == ptmp_proptags) {
		return NULL;
	}
	ptmp_proptags->pproptag = cu_alloc<uint32_t>(pproptags->count);
	if (ptmp_proptags->pproptag == nullptr)
		return NULL;
	ptmp_proptags->count = 0;
	for (i=0; i<pproptags->count; i++) {
		if (PROP_TYPE(pproptags->pproptag[i]) == PT_OBJECT)
			continue;
		ptmp_proptags->pproptag[ptmp_proptags->count] = 
									pproptags->pproptag[i];
		ptmp_proptags->count ++;
	}
	return ptmp_proptags;
}

int common_util_problem_compare(const void *pproblem1,
	const void *pproblem2)
{
	auto a = static_cast<const PROPERTY_PROBLEM *>(pproblem1)->index;
	auto b = static_cast<const PROPERTY_PROBLEM *>(pproblem2)->index;
	return a == b ? 0 : a < b ? -1 : 1;
}

BOOL common_util_propvals_to_row(
	const TPROPVAL_ARRAY *ppropvals,
	const PROPTAG_ARRAY *pcolumns, PROPERTY_ROW *prow)
{
	int i;
	FLAGGED_PROPVAL *pflagged_val;
	static const uint32_t errcode = ecNotFound;
	
	for (i=0; i<pcolumns->count; i++) {
		if (NULL == common_util_get_propvals(
			ppropvals, pcolumns->pproptag[i])) {
			break;	
		}
	}
	prow->flag = i < pcolumns->count ? PROPERTY_ROW_FLAG_FLAGGED : PROPERTY_ROW_FLAG_NONE;
	prow->pppropval = cu_alloc<void *>(pcolumns->count);
	if (NULL == prow->pppropval) {
		return FALSE;
	}
	for (i=0; i<pcolumns->count; i++) {
		prow->pppropval[i] = common_util_get_propvals(
					ppropvals, pcolumns->pproptag[i]);
		if (PROPERTY_ROW_FLAG_FLAGGED == prow->flag) {
			pflagged_val = cu_alloc<FLAGGED_PROPVAL>();
			if (NULL == pflagged_val) {
				return FALSE;
			}
			if (NULL == prow->pppropval[i]) {
				pflagged_val->flag = FLAGGED_PROPVAL_FLAG_ERROR;
				pflagged_val->pvalue = common_util_get_propvals(ppropvals,
					CHANGE_PROP_TYPE(pcolumns->pproptag[i], PT_ERROR));
				if (NULL == pflagged_val->pvalue) {
					pflagged_val->pvalue = deconst(&errcode);
				}
			} else {
				pflagged_val->flag = FLAGGED_PROPVAL_FLAG_AVAILABLE;
				pflagged_val->pvalue = prow->pppropval[i];
			}
			prow->pppropval[i] = pflagged_val;
		}
	}
	return TRUE;
}

BOOL common_util_convert_unspecified(uint32_t cpid,
	BOOL b_unicode, TYPED_PROPVAL *ptyped)
{
	size_t tmp_len;
	
	if (TRUE == b_unicode) {
		if (ptyped->type != PT_STRING8)
			return TRUE;
		tmp_len = 2 * strlen(static_cast<char *>(ptyped->pvalue)) + 1;
		auto pvalue = common_util_alloc(tmp_len);
		if (NULL == pvalue) {
			return FALSE;
		}
		if (common_util_mb_to_utf8(cpid, static_cast<char *>(ptyped->pvalue),
		    static_cast<char *>(pvalue), tmp_len) < 0)
			return FALSE;	
		ptyped->pvalue = pvalue;
		return TRUE;
	} else {
		if (ptyped->type != PT_UNICODE)
			return TRUE;
		tmp_len = 2 * strlen(static_cast<char *>(ptyped->pvalue)) + 1;
		auto pvalue = common_util_alloc(tmp_len);
		if (NULL == pvalue) {
			return FALSE;
		}
		if (common_util_mb_from_utf8(cpid, static_cast<char *>(ptyped->pvalue),
		    static_cast<char *>(pvalue), tmp_len) < 0)
			return FALSE;	
		ptyped->pvalue = pvalue;
		return TRUE;
	}
}

BOOL common_util_propvals_to_row_ex(uint32_t cpid,
	BOOL b_unicode, const TPROPVAL_ARRAY *ppropvals,
	const PROPTAG_ARRAY *pcolumns, PROPERTY_ROW *prow)
{
	int i;
	FLAGGED_PROPVAL *pflagged_val;
	static const uint32_t errcode = ecNotFound;
	
	for (i=0; i<pcolumns->count; i++) {
		if (NULL == common_util_get_propvals(
			(TPROPVAL_ARRAY*)ppropvals, pcolumns->pproptag[i])) {
			break;	
		}
	}
	prow->flag = i < pcolumns->count ? PROPERTY_ROW_FLAG_FLAGGED : PROPERTY_ROW_FLAG_NONE;
	prow->pppropval = cu_alloc<void *>(pcolumns->count);
	if (NULL == prow->pppropval) {
		return FALSE;
	}
	for (i=0; i<pcolumns->count; i++) {
		prow->pppropval[i] = common_util_get_propvals(
			(TPROPVAL_ARRAY*)ppropvals, pcolumns->pproptag[i]);
		if (NULL != prow->pppropval[i] &&
		    PROP_TYPE(pcolumns->pproptag[i]) == PT_UNSPECIFIED) {
			if (!common_util_convert_unspecified(cpid, b_unicode,
			    static_cast<TYPED_PROPVAL *>(prow->pppropval[i])))
				return FALSE;
		}
		if (PROPERTY_ROW_FLAG_FLAGGED == prow->flag) {
			pflagged_val = cu_alloc<FLAGGED_PROPVAL>();
			if (NULL == pflagged_val) {
				return FALSE;
			}
			if (NULL == prow->pppropval[i]) {
				pflagged_val->flag = FLAGGED_PROPVAL_FLAG_ERROR;
				pflagged_val->pvalue = common_util_get_propvals(ppropvals,
					CHANGE_PROP_TYPE(pcolumns->pproptag[i], PT_ERROR));
				if (NULL == pflagged_val->pvalue) {
					pflagged_val->pvalue = deconst(&errcode);
				}
			} else {
				pflagged_val->flag = FLAGGED_PROPVAL_FLAG_AVAILABLE;
				pflagged_val->pvalue = prow->pppropval[i];
			}
			prow->pppropval[i] = pflagged_val;
		}
	}
	return TRUE;
}

BOOL common_util_row_to_propvals(
	const PROPERTY_ROW *prow, const PROPTAG_ARRAY *pcolumns,
	TPROPVAL_ARRAY *ppropvals)
{
	int i;
	TAGGED_PROPVAL propval;
	
	for (i=0; i<pcolumns->count; i++) {
		propval.proptag = pcolumns->pproptag[i];
		if (PROPERTY_ROW_FLAG_NONE == prow->flag) {
			propval.pvalue = prow->pppropval[i];
		} else {
			if (FLAGGED_PROPVAL_FLAG_AVAILABLE !=
				((FLAGGED_PROPVAL*)prow->pppropval[i])->flag) {
				continue;	
			}
			propval.pvalue = ((FLAGGED_PROPVAL*)prow->pppropval[i])->pvalue;
		}
		common_util_set_propvals(ppropvals, &propval);
	}
	return TRUE;
}

static BOOL common_util_propvals_to_recipient(uint32_t cpid,
	TPROPVAL_ARRAY *ppropvals, const PROPTAG_ARRAY *pcolumns,
	RECIPIENT_ROW *prow)
{
	memset(prow, 0, sizeof(RECIPIENT_ROW));
	prow->flags |= RECIPIENT_ROW_FLAG_UNICODE;
	auto pvalue = common_util_get_propvals(ppropvals, PROP_TAG_RESPONSIBILITY);
	if (NULL != pvalue && 0 != *(uint8_t*)pvalue) {
		prow->flags |= RECIPIENT_ROW_FLAG_RESPONSIBLE;
	}
	pvalue = common_util_get_propvals(ppropvals, PROP_TAG_SENDRICHINFO);
	if (NULL != pvalue && 0 != *(uint8_t*)pvalue) {
		prow->flags |= RECIPIENT_ROW_FLAG_NONRICH;
	}
	prow->ptransmittable_name = static_cast<char *>(common_util_get_propvals(
	                            ppropvals, PROP_TAG_TRANSMITTABLEDISPLAYNAME));
	if (NULL == prow->ptransmittable_name) {
		pvalue = common_util_get_propvals(ppropvals,
			PROP_TAG_TRANSMITTABLEDISPLAYNAME_STRING8);
		if (NULL != pvalue) {
			prow->ptransmittable_name =
				common_util_dup_mb_to_utf8(cpid, static_cast<char *>(pvalue));
		}
	}
	prow->pdisplay_name = static_cast<char *>(common_util_get_propvals(ppropvals, PR_DISPLAY_NAME));
	if (NULL == prow->pdisplay_name) {
		pvalue = common_util_get_propvals(ppropvals, PR_DISPLAY_NAME_A);
		if (NULL != pvalue) {
			prow->pdisplay_name =
				common_util_dup_mb_to_utf8(cpid, static_cast<char *>(pvalue));
		}
	}
	if (NULL != prow->ptransmittable_name && NULL != prow->pdisplay_name &&
		0 == strcasecmp(prow->pdisplay_name, prow->ptransmittable_name)) {
		prow->flags |= RECIPIENT_ROW_FLAG_SAME;
		prow->ptransmittable_name = NULL;
	}
	if (NULL != prow->ptransmittable_name) {
		prow->flags |= RECIPIENT_ROW_FLAG_TRANSMITTABLE;
	}
	if (NULL != prow->pdisplay_name) {
		prow->flags |= RECIPIENT_ROW_FLAG_DISPLAY;
	}
	prow->psimple_name = static_cast<char *>(common_util_get_propvals(
	                     ppropvals, PR_EMS_AB_DISPLAY_NAME_PRINTABLE));
	if (NULL == prow->psimple_name) {
		pvalue = common_util_get_propvals(ppropvals,
		         PR_EMS_AB_DISPLAY_NAME_PRINTABLE_A);
		if (NULL != pvalue) {
			prow->psimple_name =
				common_util_dup_mb_to_utf8(cpid, static_cast<char *>(pvalue));
		}
	}
	if (NULL != prow->psimple_name) {
		prow->flags |= RECIPIENT_ROW_FLAG_SIMPLE;
	}
	pvalue = common_util_get_propvals(ppropvals, PROP_TAG_ADDRESSTYPE);
	if (NULL != pvalue) {
		if (strcasecmp(static_cast<char *>(pvalue), "EX") == 0) {
			prow->flags |= RECIPIENT_ROW_TYPE_X500DN;
			static constexpr uint8_t dummy_zero = 0;
			prow->pprefix_used = deconst(&dummy_zero);
			pvalue = common_util_get_propvals(ppropvals, PR_DISPLAY_TYPE);
			if (NULL == pvalue) {
				prow->display_type = DT_MAILUSER;
			} else {
				prow->display_type = *static_cast<uint32_t *>(pvalue);
				if (prow->display_type >= DT_ROOM)
					prow->display_type = DT_MAILUSER;
			}
			prow->have_display_type = true;
			prow->px500dn = static_cast<char *>(common_util_get_propvals(
			                ppropvals, PR_EMAIL_ADDRESS));
			if (NULL == prow->px500dn) {
				return FALSE;
			}
		} else if (strcasecmp(static_cast<char *>(pvalue), "SMTP") == 0) {
			prow->flags |= RECIPIENT_ROW_TYPE_SMTP |
							RECIPIENT_ROW_FLAG_EMAIL;
			prow->pmail_address = static_cast<char *>(common_util_get_propvals(
			                      ppropvals, PR_EMAIL_ADDRESS));
			if (NULL == prow->pmail_address) {
				prow->pmail_address = static_cast<char *>(common_util_get_propvals(
				                      ppropvals, PR_SMTP_ADDRESS));
				if (NULL == prow->pmail_address) {
					return FALSE;
				}
			}
		} else {
			prow->flags |= RECIPIENT_ROW_FLAG_EMAIL |
					RECIPIENT_ROW_FLAG_OUTOFSTANDARD;
			prow->paddress_type = static_cast<char *>(pvalue);
			prow->pmail_address = static_cast<char *>(common_util_get_propvals(
			                      ppropvals, PR_EMAIL_ADDRESS));
			if (NULL == prow->pmail_address) {
				return FALSE;
			}
		}
	}
	prow->count = pcolumns->count;
	return common_util_propvals_to_row(ppropvals, pcolumns, &prow->properties);
}

static BOOL common_util_recipient_to_propvals(uint32_t cpid,
	RECIPIENT_ROW *prow, const PROPTAG_ARRAY *pcolumns,
	TPROPVAL_ARRAY *ppropvals)
{
	uint8_t fake_true = 1;
	uint8_t fake_false = 0;
	TAGGED_PROPVAL propval;
	PROPTAG_ARRAY tmp_columns;
	BOOL b_unicode = (prow->flags & RECIPIENT_ROW_FLAG_UNICODE) ? TRUE : false;
	
	propval.proptag = PROP_TAG_RESPONSIBILITY;
	propval.pvalue = (prow->flags & RECIPIENT_ROW_FLAG_RESPONSIBLE) ? &fake_true : &fake_false;
	common_util_set_propvals(ppropvals, &propval);
	propval.proptag = PROP_TAG_SENDRICHINFO;
	propval.pvalue = (prow->flags & RECIPIENT_ROW_FLAG_NONRICH) ? &fake_true : &fake_false;
	common_util_set_propvals(ppropvals, &propval);
	if (NULL != prow->ptransmittable_name) {
		propval.proptag = PROP_TAG_TRANSMITTABLEDISPLAYNAME;
		if (TRUE == b_unicode) {
			propval.pvalue = prow->ptransmittable_name;
		} else {
			propval.pvalue = common_util_dup_mb_to_utf8(cpid,
								prow->ptransmittable_name);
			if (NULL == propval.pvalue) {
				return FALSE;
			}
		}
		common_util_set_propvals(ppropvals, &propval);
	}
	if (NULL != prow->pdisplay_name) {
		propval.proptag = PR_DISPLAY_NAME;
		propval.pvalue = b_unicode ? prow->pdisplay_name :
		                 common_util_dup_mb_to_utf8(cpid, prow->pdisplay_name);
		if (NULL != propval.pvalue) {
			common_util_set_propvals(ppropvals, &propval);
		}
	}
	if (NULL != prow->pmail_address) {
		propval.proptag = PR_EMAIL_ADDRESS;
		if (TRUE == b_unicode) {
			propval.pvalue = prow->pmail_address;
		} else {
			propval.pvalue = common_util_dup_mb_to_utf8(
								cpid, prow->pmail_address);
			if (NULL == propval.pvalue) {
				return FALSE;
			}
		}
		common_util_set_propvals(ppropvals, &propval);
	}
	propval.proptag = PROP_TAG_ADDRESSTYPE;
	switch (prow->flags & 0x0007) {
	case RECIPIENT_ROW_TYPE_NONE:
		if (NULL != prow->paddress_type) {
			propval.pvalue = prow->paddress_type;
			common_util_set_propvals(ppropvals, &propval);
		}
		break;
	case RECIPIENT_ROW_TYPE_X500DN:
		if (NULL == prow->px500dn) {
			return FALSE;
		}
		propval.pvalue = deconst("EX");
		common_util_set_propvals(ppropvals, &propval);
		propval.proptag = PR_EMAIL_ADDRESS;
		propval.pvalue = prow->px500dn;
		common_util_set_propvals(ppropvals, &propval);
		break;
	case RECIPIENT_ROW_TYPE_SMTP:
		propval.pvalue = deconst("SMTP");
		common_util_set_propvals(ppropvals, &propval);
		break;
	default:
		/* we do not support other address types */
		return FALSE;
	}
	tmp_columns.count = prow->count;
	tmp_columns.pproptag = pcolumns->pproptag;
	if (FALSE == common_util_row_to_propvals(
		&prow->properties, &tmp_columns, ppropvals)) {
		return FALSE;	
	}
	auto pvalue = common_util_get_propvals(ppropvals, PR_DISPLAY_NAME);
	if (NULL == pvalue || '\0' == *(char*)pvalue ||
	    strcmp(static_cast<char *>(pvalue), "''") == 0 ||
	    strcmp(static_cast<char *>(pvalue), "\"\"") == 0) {
		propval.proptag = PR_DISPLAY_NAME;
		propval.pvalue = common_util_get_propvals(
			ppropvals, PROP_TAG_RECIPIENTDISPLAYNAME);
		if (NULL == propval.pvalue) {
			propval.pvalue = common_util_get_propvals(ppropvals, PR_SMTP_ADDRESS);
		}
		if (NULL == propval.pvalue) {
			propval.pvalue = deconst("Undisclosed-Recipients");
		}
		common_util_set_propvals(ppropvals, &propval);
	}
	return TRUE;
}

BOOL common_util_propvals_to_openrecipient(uint32_t cpid,
	TPROPVAL_ARRAY *ppropvals, const PROPTAG_ARRAY *pcolumns,
	OPENRECIPIENT_ROW *prow)
{
	auto pvalue = common_util_get_propvals(ppropvals, PROP_TAG_RECIPIENTTYPE);
	prow->recipient_type = pvalue == nullptr ? RECIPIENT_TYPE_NONE : *static_cast<uint32_t *>(pvalue);
	prow->reserved = 0;
	prow->cpid = cpid;
	return common_util_propvals_to_recipient(cpid,
		ppropvals, pcolumns, &prow->recipient_row);
}

BOOL common_util_propvals_to_readrecipient(uint32_t cpid,
	TPROPVAL_ARRAY *ppropvals, const PROPTAG_ARRAY *pcolumns,
	READRECIPIENT_ROW *prow)
{
	auto pvalue = common_util_get_propvals(ppropvals, PROP_TAG_ROWID);
	if (NULL == pvalue) {
		return FALSE;
	}
	prow->row_id = *(uint32_t*)pvalue;
	pvalue = common_util_get_propvals(ppropvals, PROP_TAG_RECIPIENTTYPE);
	prow->recipient_type = pvalue == nullptr ? RECIPIENT_TYPE_NONE : *static_cast<uint32_t *>(pvalue);
	prow->reserved = 0;
	prow->cpid = cpid;
	return common_util_propvals_to_recipient(cpid,
		ppropvals, pcolumns, &prow->recipient_row);
}

BOOL common_util_modifyrecipient_to_propvals(
	 uint32_t cpid, const MODIFYRECIPIENT_ROW *prow,
	const PROPTAG_ARRAY *pcolumns, TPROPVAL_ARRAY *ppropvals)
{
	TAGGED_PROPVAL propval;
	
	ppropvals->count = 0;
	ppropvals->ppropval = cu_alloc<TAGGED_PROPVAL>(16 + pcolumns->count);
	if (NULL == ppropvals->ppropval) {
		return FALSE;
	}
	propval.proptag = PROP_TAG_ROWID;
	propval.pvalue = (void*)&prow->row_id;
	common_util_set_propvals(ppropvals, &propval);
	propval.proptag = PROP_TAG_RECIPIENTTYPE;
	propval.pvalue = cu_alloc<uint32_t>();
	if (NULL == propval.pvalue) {
		return FALSE;
	}
	*(uint32_t*)propval.pvalue = prow->recipient_type;
	common_util_set_propvals(ppropvals, &propval);
	if (NULL == prow->precipient_row) {
		return TRUE;
	}
	return common_util_recipient_to_propvals(cpid,
			prow->precipient_row, pcolumns, ppropvals);
}

static void common_util_convert_proptag(BOOL to_unicode, uint32_t *pproptag)
{
	if (TRUE == to_unicode) {
		if (PROP_TYPE(*pproptag) == PT_STRING8)
			*pproptag = CHANGE_PROP_TYPE(*pproptag, PT_UNICODE);
		else if (PROP_TYPE(*pproptag) == PT_MV_STRING8)
			*pproptag = CHANGE_PROP_TYPE(*pproptag, PT_MV_UNICODE);
	} else {
		if (PROP_TYPE(*pproptag) == PT_UNICODE)
			*pproptag = CHANGE_PROP_TYPE(*pproptag, PT_STRING8);
		else if (PROP_TYPE(*pproptag) == PT_MV_UNICODE)
			*pproptag = CHANGE_PROP_TYPE(*pproptag, PT_MV_STRING8);
	}
}

/* only for being invoked in rop environment */
BOOL common_util_convert_tagged_propval(
	BOOL to_unicode, TAGGED_PROPVAL *ppropval)
{
	char *pstring;
	
	if (TRUE == to_unicode) {
		switch (PROP_TYPE(ppropval->proptag)) {
		case PT_STRING8: {
			auto len = 2 * strlen(static_cast<char *>(ppropval->pvalue)) + 1;
			pstring = cu_alloc<char>(len);
			if (NULL == pstring) {
				return FALSE;
			}
			if (common_util_convert_string(TRUE,
			    static_cast<char *>(ppropval->pvalue), pstring, len) < 0) {
				return FALSE;	
			}
			ppropval->pvalue = pstring;
			common_util_convert_proptag(TRUE, &ppropval->proptag);
			break;
		}
		case PT_MV_STRING8: {
			auto sa = static_cast<STRING_ARRAY *>(ppropval->pvalue);
			for (size_t i = 0; i < sa->count; ++i) {
				auto len = 2 * strlen(sa->ppstr[i]) + 1;
				pstring = cu_alloc<char>(len);
				if (NULL == pstring) {
					return FALSE;
				}
				if (common_util_convert_string(TRUE, sa->ppstr[i], pstring, len) < 0)
					return FALSE;	
				sa->ppstr[i] = pstring;
			}
			common_util_convert_proptag(TRUE, &ppropval->proptag);
			break;
		}
		case PT_SRESTRICT:
			if (!common_util_convert_restriction(TRUE,
			    static_cast<RESTRICTION *>(ppropval->pvalue)))
				return FALSE;	
			break;
		case PT_ACTIONS:
			if (!common_util_convert_rule_actions(TRUE,
			    static_cast<RULE_ACTIONS *>(ppropval->pvalue)))
				return FALSE;	
			break;
		}
	} else {
		switch (PROP_TYPE(ppropval->proptag)) {
		case PT_UNICODE: {
			auto len = 2 * strlen(static_cast<char *>(ppropval->pvalue)) + 1;
			pstring = cu_alloc<char>(len);
			if (NULL == pstring) {
				return FALSE;
			}
			if (common_util_convert_string(FALSE,
			    static_cast<char *>(ppropval->pvalue), pstring, len) < 0)
				return FALSE;	
			ppropval->pvalue = pstring;
			common_util_convert_proptag(FALSE, &ppropval->proptag);
			break;
		}
		case PT_MV_UNICODE: {
			auto sa = static_cast<STRING_ARRAY *>(ppropval->pvalue);
			for (size_t i = 0; i < sa->count; ++i) {
				auto len = 2 * strlen(sa->ppstr[i]) + 1;
				pstring = cu_alloc<char>(len);
				if (NULL == pstring) {
					return FALSE;
				}
				if (common_util_convert_string(FALSE, sa->ppstr[i], pstring, len) < 0)
					return FALSE;	
				sa->ppstr[i] = pstring;
			}
			common_util_convert_proptag(FALSE, &ppropval->proptag);
			break;
		}
		case PT_SRESTRICT:
			if (!common_util_convert_restriction(FALSE,
			    static_cast<RESTRICTION *>(ppropval->pvalue)))
				return FALSE;	
			break;
		case PT_ACTIONS:
			if (!common_util_convert_rule_actions(FALSE,
			    static_cast<RULE_ACTIONS *>(ppropval->pvalue)))
				return FALSE;	
			break;
		}
	}
	return TRUE;
}

/* only for being invoked in rop environment */
BOOL common_util_convert_restriction(BOOL to_unicode, RESTRICTION *pres)
{
	switch (pres->rt) {
	case RES_AND:
	case RES_OR:
		for (size_t i = 0; i < pres->andor->count; ++i)
			if (!common_util_convert_restriction(to_unicode, &pres->andor->pres[i]))
				return FALSE;	
		break;
	case RES_NOT:
		if (!common_util_convert_restriction(to_unicode, &pres->xnot->res))
			return FALSE;	
		break;
	case RES_CONTENT:
		if (!common_util_convert_tagged_propval(to_unicode, &pres->cont->propval))
			return FALSE;	
		common_util_convert_proptag(to_unicode, &pres->cont->proptag);
		break;
	case RES_PROPERTY:
		if (!common_util_convert_tagged_propval(to_unicode, &pres->prop->propval))
			return FALSE;	
		common_util_convert_proptag(to_unicode, &pres->prop->proptag);
		break;
	case RES_PROPCOMPARE:
		common_util_convert_proptag(to_unicode, &pres->pcmp->proptag1);
		common_util_convert_proptag(to_unicode, &pres->pcmp->proptag2);
		break;
	case RES_BITMASK:
		common_util_convert_proptag(to_unicode, &pres->bm->proptag);
		break;
	case RES_SIZE:
		common_util_convert_proptag(to_unicode, &pres->size->proptag);
		break;
	case RES_EXIST:
		common_util_convert_proptag(to_unicode, &pres->exist->proptag);
		break;
	case RES_SUBRESTRICTION:
		if (!common_util_convert_restriction(to_unicode, &pres->sub->res))
			return FALSE;	
		break;
	case RES_COMMENT: {
		auto rcom = pres->comment;
		for (size_t i = 0; i < rcom->count; ++i)
			if (!common_util_convert_tagged_propval(to_unicode, &rcom->ppropval[i]))
				return FALSE;	
		if (rcom->pres != nullptr)
			if (!common_util_convert_restriction(to_unicode, rcom->pres))
				return FALSE;	
		break;
	}
	case RES_COUNT:
		if (!common_util_convert_restriction(to_unicode, &pres->count->sub_res))
			return FALSE;	
		break;
	default:
		return TRUE;
	}
	return TRUE;
}

static BOOL common_util_convert_recipient_block(
	BOOL to_unicode, RECIPIENT_BLOCK *prcpt)
{
	int i;
	
	for (i=0; i<prcpt->count; i++) {
		if (FALSE == common_util_convert_tagged_propval(
			to_unicode, prcpt->ppropval + i)) {
			return FALSE;	
		}
	}
	return TRUE;
}

static BOOL common_util_convert_forwarddelegate_action(
	BOOL to_unicode, FORWARDDELEGATE_ACTION *pfwd)
{
	int i;
	
	for (i=0; i<pfwd->count; i++) {
		if (FALSE == common_util_convert_recipient_block(
			to_unicode, pfwd->pblock + i)) {
			return FALSE;	
		}
	}
	return TRUE;
}

static BOOL common_util_convert_action_block(
	BOOL to_unicode, ACTION_BLOCK *pblock)
{
	switch (pblock->type) {
	case OP_MOVE:
	case OP_COPY:
		break;
	case OP_REPLY:
	case OP_OOF_REPLY:
		break;
	case OP_DEFER_ACTION:
		break;
	case OP_BOUNCE:
		break;
	case OP_FORWARD:
	case OP_DELEGATE:
		if (!common_util_convert_forwarddelegate_action(to_unicode,
		    static_cast<FORWARDDELEGATE_ACTION *>(pblock->pdata)))
			return FALSE;	
		break;
	case OP_TAG:
		if (!common_util_convert_tagged_propval(to_unicode,
		    static_cast<TAGGED_PROPVAL *>(pblock->pdata)))
			return FALSE;	
		break;
	case OP_DELETE:
		break;
	case OP_MARK_AS_READ:
		break;
	}
	return TRUE;
}

BOOL common_util_convert_rule_actions(BOOL to_unicode, RULE_ACTIONS *pactions)
{
	int i;
	
	for (i=0; i<pactions->count; i++) {
		if (FALSE == common_util_convert_action_block(
			to_unicode, pactions->pblock + i)) {
			return FALSE;	
		}
	}
	return TRUE;
}

void common_util_notify_receipt(const char *username,
	int type, MESSAGE_CONTENT *pbrief)
{
	MAIL imail;
	DOUBLE_LIST_NODE node;
	DOUBLE_LIST rcpt_list;
	
	node.pdata = common_util_get_propvals(&pbrief->proplist,
					PROP_TAG_SENTREPRESENTINGSMTPADDRESS);
	if (NULL == node.pdata) {
		return;
	}
	double_list_init(&rcpt_list);
	double_list_append_as_tail(&rcpt_list, &node);
	mail_init(&imail, g_mime_pool);
	int bounce_type = type == NOTIFY_RECEIPT_READ ? BOUNCE_NOTIFY_READ : BOUNCE_NOTIFY_NON_READ;
	if (FALSE == bounce_producer_make(username,
		pbrief, bounce_type, &imail)) {
		mail_free(&imail);
		return;
	}
	common_util_send_mail(&imail, username, &rcpt_list);
	mail_free(&imail);
}

BOOL common_util_save_message_ics(LOGON_OBJECT *plogon,
	uint64_t message_id, PROPTAG_ARRAY *pchanged_proptags)
{
	int i;
	XID tmp_xid;
	uint32_t tmp_index;
	uint32_t *pgroup_id;
	uint64_t change_num;
	PROPTAG_ARRAY *pindices;
	PROBLEM_ARRAY tmp_problems;
	PROPERTY_GROUPINFO *pgpinfo;
	TPROPVAL_ARRAY tmp_propvals;
	TAGGED_PROPVAL propval_buff[2];
	PROPTAG_ARRAY *pungroup_proptags;
	
	if (!exmdb_client_allocate_cn(plogon->get_dir(), &change_num))
		return FALSE;	
	tmp_xid.guid = plogon->guid();
	rop_util_get_gc_array(change_num, tmp_xid.local_id);
	tmp_propvals.count = 2;
	tmp_propvals.ppropval = propval_buff;
	propval_buff[0].proptag = PROP_TAG_CHANGENUMBER;
	propval_buff[0].pvalue = &change_num;
	propval_buff[1].proptag = PR_CHANGE_KEY;
	propval_buff[1].pvalue = common_util_xid_to_binary(22, &tmp_xid);
	if (NULL == propval_buff[1].pvalue) {
		return FALSE;
	}
	if (!exmdb_client_set_message_properties(plogon->get_dir(), nullptr, 0,
	    message_id, &tmp_propvals, &tmp_problems))
		return FALSE;	
	if (!exmdb_client_get_message_group_id(plogon->get_dir(),
	    message_id, &pgroup_id))
		return FALSE;	
	if (NULL == pgroup_id) {
		pgpinfo = plogon->get_last_property_groupinfo();
		if (NULL == pgpinfo) {
			return FALSE;
		}
		if (!exmdb_client_set_message_group_id(plogon->get_dir(),
		    message_id, pgpinfo->group_id))
			return FALSE;	
	}  else {
		pgpinfo = plogon->get_property_groupinfo(*pgroup_id);
		if (NULL == pgpinfo) {
			return FALSE;
		}
	}
	/* memory format of PROPTAG_ARRAY is identical to LONG_ARRAY */
	pindices = proptag_array_init();
	if (NULL == pindices) {
		return FALSE;
	}
	pungroup_proptags = proptag_array_init();
	if (NULL == pungroup_proptags) {
		proptag_array_free(pindices);
		return FALSE;
	}
	if (!property_groupinfo_get_partial_index(pgpinfo, PR_CHANGE_KEY, &tmp_index)) {
		if (!proptag_array_append(pungroup_proptags, PR_CHANGE_KEY)) {
			proptag_array_free(pindices);
			proptag_array_free(pungroup_proptags);
			return FALSE;
		}
	} else {
		if (!proptag_array_append(pindices, tmp_index)) {
			proptag_array_free(pindices);
			proptag_array_free(pungroup_proptags);
			return FALSE;
		}
	}
	if (NULL != pchanged_proptags) {
		for (i=0; i<pchanged_proptags->count; i++) {
			if (FALSE == property_groupinfo_get_partial_index(
				pgpinfo, pchanged_proptags->pproptag[i], &tmp_index)) {
				if (!proptag_array_append(pungroup_proptags,
				    pchanged_proptags->pproptag[i])) {
					proptag_array_free(pindices);
					proptag_array_free(pungroup_proptags);
					return FALSE;
				}
			} else {
				if (!proptag_array_append(pindices, tmp_index)) {
					proptag_array_free(pindices);
					proptag_array_free(pungroup_proptags);
					return FALSE;
				}
			}
		}
		
	}
	if (!exmdb_client_save_change_indices(plogon->get_dir(), message_id,
	    change_num, pindices, pungroup_proptags)) {
		proptag_array_free(pindices);
		proptag_array_free(pungroup_proptags);
		return FALSE;
	}
	proptag_array_free(pindices);
	proptag_array_free(pungroup_proptags);
	return TRUE;
}

static BOOL common_util_send_command(int sockd,
	const char *command, int command_len)
{
	int write_len;

	write_len = write(sockd, command, command_len);
    if (write_len != command_len) {
		return FALSE;
	}
	return TRUE;
}

static int common_util_get_response(int sockd,
	char *response, int response_len, BOOL expect_3xx)
{
	int read_len;

	memset(response, 0, response_len);
	read_len = read(sockd, response, response_len);
	if (-1 == read_len || 0 == read_len) {
		return SMTP_TIME_OUT;
	}
	if ('\n' == response[read_len - 1] && '\r' == response[read_len - 2]){
		/* remove /r/n at the end of response */
		read_len -= 2;
	}
	response[read_len] = '\0';
	if (FALSE == expect_3xx && '2' == response[0] &&
	    HX_isdigit(response[1]) && HX_isdigit(response[2])) {
		return SMTP_SEND_OK;
	} else if(TRUE == expect_3xx && '3' == response[0] &&
	    HX_isdigit(response[1]) && HX_isdigit(response[2])) {
		return SMTP_SEND_OK;
	} else {
		if ('4' == response[0]) {
           	return SMTP_TEMP_ERROR;	
		} else if ('5' == response[0]) {
			return SMTP_PERMANENT_ERROR;
		} else {
			return SMTP_UNKOWN_RESPONSE;
		}
	}
}

BOOL common_util_send_mail(MAIL *pmail,
	const char *sender, DOUBLE_LIST *prcpt_list)
{
	int res_val;
	int command_len;
	DOUBLE_LIST_NODE *pnode;
	char last_command[1024];
	char last_response[1024];
	
	int sockd = gx_inet_connect(g_smtp_ip, g_smtp_port, 0);
	if (sockd < 0) {
		log_err("Cannot connect to SMTP server [%s]:%hu: %s",
			g_smtp_ip, g_smtp_port, strerror(-sockd));
		return FALSE;
	}
	/* read welcome information of MTA */
	res_val = common_util_get_response(sockd, last_response, 1024, FALSE);
	switch (res_val) {
	case SMTP_TIME_OUT:
		close(sockd);
		log_err("Timeout with SMTP server [%s]:%hu",
			g_smtp_ip, g_smtp_port);
		return FALSE;
	case SMTP_PERMANENT_ERROR:
	case SMTP_TEMP_ERROR:
	case SMTP_UNKOWN_RESPONSE:
        /* send quit command to server */
        common_util_send_command(sockd, "quit\r\n", 6);
		close(sockd);
		log_err("Failed to connect to SMTP. "
			"Server response is \"%s\"", last_response);
		return FALSE;
	}

	/* send helo xxx to server */
	snprintf(last_command, 1024, "helo %s\r\n", get_host_ID());
	command_len = strlen(last_command);
	if (FALSE == common_util_send_command(
		sockd, last_command, command_len)) {
		close(sockd);
		log_err("Failed to send \"HELO\" command");
		return FALSE;
	}
	res_val = common_util_get_response(sockd, last_response, 1024, FALSE);
	switch (res_val) {
	case SMTP_TIME_OUT:
		close(sockd);
		log_err("Timeout with SMTP "
			"server [%s]:%hu", g_smtp_ip, g_smtp_port);
		return FALSE;
	case SMTP_PERMANENT_ERROR:
	case SMTP_TEMP_ERROR:
	case SMTP_UNKOWN_RESPONSE:
		/* send quit command to server */
		common_util_send_command(sockd, "quit\r\n", 6);
		close(sockd);
		log_err("SMTP server responded with \"%s\" "
			"after sending \"HELO\" command", last_response);
		return FALSE;
	}

	command_len = sprintf(last_command, "mail from:<%s>\r\n", sender);
	
	if (FALSE == common_util_send_command(
		sockd, last_command, command_len)) {
		close(sockd);
		log_err("Failed to send \"MAIL FROM\" command");
		return FALSE;
	}
	/* read mail from response information */
	res_val = common_util_get_response(sockd, last_response, 1024, FALSE);
	switch (res_val) {
	case SMTP_TIME_OUT:
		close(sockd);
		log_err("Timeout with SMTP server [%s]:%hu",
			g_smtp_ip, g_smtp_port);
		return FALSE;
	case SMTP_PERMANENT_ERROR:
		case SMTP_TEMP_ERROR:
		case SMTP_UNKOWN_RESPONSE:
		/* send quit command to server */
		common_util_send_command(sockd, "quit\r\n", 6);
		close(sockd);
		log_err("SMTP server responded \"%s\" "
			"after sending \"MAIL FROM\" command", last_response);
		return FALSE;
	}

	for (pnode=double_list_get_head(prcpt_list); NULL!=pnode;
		pnode=double_list_get_after(prcpt_list, pnode)) {
		if (strchr(static_cast<char *>(pnode->pdata), '@') == nullptr) {
			command_len = sprintf(last_command,
				"rcpt to:<%s@none>\r\n",
				static_cast<const char *>(pnode->pdata));
		} else {
			command_len = sprintf(last_command,
				"rcpt to:<%s>\r\n",
				static_cast<const char *>(pnode->pdata));
		}
		if (FALSE == common_util_send_command(
			sockd, last_command, command_len)) {
			close(sockd);
			log_err("Failed to send \"RCPT TO\" command");
			return FALSE;
		}
		/* read rcpt to response information */
		res_val = common_util_get_response(sockd, last_response, 1024, FALSE);
		switch (res_val) {
		case SMTP_TIME_OUT:
			close(sockd);
			log_err("Timeout with SMTP server [%s]:%hu",
				g_smtp_ip, g_smtp_port);
			return FALSE;
		case SMTP_PERMANENT_ERROR:
		case SMTP_TEMP_ERROR:
		case SMTP_UNKOWN_RESPONSE:
			common_util_send_command(sockd, "quit\r\n", 6);
			close(sockd);
			log_err("SMTP server responded with \"%s\" "
				"after sending \"RCPT TO\" command", last_response);
			return FALSE;
		}						
	}
	/* send data */
	strcpy(last_command, "data\r\n");
	command_len = strlen(last_command);
	if (FALSE == common_util_send_command(
		sockd, last_command, command_len)) {
		close(sockd);
		log_err("Sender %s: Failed "
			"to send \"DATA\" command", sender);
		return FALSE;
	}

	/* read data response information */
	res_val = common_util_get_response(sockd, last_response, 1024, TRUE);
	switch (res_val) {
	case SMTP_TIME_OUT:
		close(sockd);
		log_err("Sender %s: Timeout with SMTP server [%s]:%hu",
			sender, g_smtp_ip, g_smtp_port);
		return FALSE;
	case SMTP_PERMANENT_ERROR:
	case SMTP_TEMP_ERROR:
	case SMTP_UNKOWN_RESPONSE:
		common_util_send_command(sockd, "quit\r\n", 6);
		close(sockd);
		log_err("Sender %s: SMTP server responded \"%s\" "
			"after sending \"DATA\" command", sender, last_response);
		return FALSE;
	}

	mail_set_header(pmail, "X-Mailer", "gromox-emsmdb " PACKAGE_VERSION);
	if (FALSE == mail_to_file(pmail, sockd) ||
		FALSE == common_util_send_command(sockd, ".\r\n", 3)) {
		close(sockd);
		log_err("Sender %s: Failed to send mail content", sender);
		return FALSE;
	}
	res_val = common_util_get_response(sockd, last_response, 1024, FALSE);
	switch (res_val) {
	case SMTP_TIME_OUT:
		close(sockd);
		log_err("Sender %s: Timeout with SMTP server [%s]:%hu",
			sender, g_smtp_ip, g_smtp_port);
		return FALSE;
	case SMTP_PERMANENT_ERROR:
	case SMTP_TEMP_ERROR:
	case SMTP_UNKOWN_RESPONSE:	
        common_util_send_command(sockd, "quit\r\n", 6);
		close(sockd);
		log_err("Sender %s: SMTP server responded \"%s\" "
					"after sending mail content", sender, last_response);
		return FALSE;
	case SMTP_SEND_OK:
		common_util_send_command(sockd, "quit\r\n", 6);
		close(sockd);
		log_err("SMTP server [%s]:%hu has received"
			" message from %s", g_smtp_ip, g_smtp_port, sender);
		return TRUE;
	}
	return false;
}

static void common_util_set_dir(const char *dir)
{
	pthread_setspecific(g_dir_key, dir);
}

static const char* common_util_get_dir()
{
	return static_cast<char *>(pthread_getspecific(g_dir_key));
}

static BOOL common_util_get_propids(
	const PROPNAME_ARRAY *ppropnames,
	PROPID_ARRAY *ppropids)
{
	return exmdb_client_get_named_propids(
			common_util_get_dir(), FALSE,
			ppropnames, ppropids);
}

static BOOL common_util_get_propname(
	uint16_t propid, PROPERTY_NAME **pppropname)
{
	PROPID_ARRAY propids;
	PROPNAME_ARRAY propnames;
	
	propids.count = 1;
	propids.ppropid = &propid;
	if (FALSE == exmdb_client_get_named_propnames(
		common_util_get_dir(), &propids, &propnames)) {
		return FALSE;
	}
	*pppropname = propnames.count != 1 ? nullptr : propnames.ppropname;
	return TRUE;
}

BOOL common_util_send_message(LOGON_OBJECT *plogon,
	uint64_t message_id, BOOL b_submit)
{
	MAIL imail;
	void *pvalue;
	BOOL b_result;
	BOOL b_delete;
	int body_type;
	EID_ARRAY ids;
	BOOL b_partial;
	uint64_t new_id;
	BINARY *ptarget;
	char username[UADDR_SIZE];
	uint64_t parent_id;
	uint64_t folder_id;
	TARRAY_SET *prcpts;
	DOUBLE_LIST temp_list;
	uint32_t message_flags;
	DOUBLE_LIST_NODE *pnode;
	TAGGED_PROPVAL *ppropval;
	MESSAGE_CONTENT *pmsgctnt;
#define LLU(x) static_cast<unsigned long long>(x)
	
	auto pinfo = emsmdb_interface_get_emsmdb_info();
	uint32_t cpid = pinfo == nullptr ? 1252 : pinfo->cpid;
	if (!exmdb_client_get_message_property(plogon->get_dir(), nullptr, 0,
	    message_id, PROP_TAG_PARENTFOLDERID, &pvalue) || pvalue == nullptr) {
		log_err("W-1289: Cannot get parent folder_id of mid:0x%llx", LLU(message_id));
		return FALSE;
	}
	parent_id = *(uint64_t*)pvalue;
	if (!exmdb_client_read_message(plogon->get_dir(), nullptr, cpid,
	    message_id, &pmsgctnt) || pmsgctnt == nullptr) {
		log_err("W-1288: Failed to read mid:0x%llx from exmdb", LLU(message_id));
		return FALSE;
	}
	if (common_util_get_propvals(&pmsgctnt->proplist, PR_INTERNET_CPID) == nullptr) {
		ppropval = cu_alloc<TAGGED_PROPVAL>(pmsgctnt->proplist.count + 1);
		if (NULL == ppropval) {
			return FALSE;
		}
		memcpy(ppropval, pmsgctnt->proplist.ppropval,
			sizeof(TAGGED_PROPVAL)*pmsgctnt->proplist.count);
		ppropval[pmsgctnt->proplist.count].proptag = PR_INTERNET_CPID;
		ppropval[pmsgctnt->proplist.count].pvalue = &cpid;
		pmsgctnt->proplist.ppropval = ppropval;
		pmsgctnt->proplist.count ++;
	}
	pvalue = common_util_get_propvals(&pmsgctnt->proplist, PR_MESSAGE_FLAGS);
	if (NULL == pvalue) {
		log_err("W-1287: Failed to get message_flag of mid:0x%llx", LLU(message_id));
		return FALSE;
	}
	message_flags = *(uint32_t*)pvalue;
	BOOL b_resend = (message_flags & MSGFLAG_RESEND) ? TRUE : false;
	prcpts = pmsgctnt->children.prcpts;
	if (NULL == prcpts) {
		log_err("W-1286: Missing recipients for message mid:0x%llx", LLU(message_id));
		return FALSE;
	}
	double_list_init(&temp_list);
	for (size_t i = 0; i < prcpts->count; ++i) {
		pnode = cu_alloc<DOUBLE_LIST_NODE>();
		if (NULL == pnode) {
			return FALSE;
		}
		if (TRUE == b_resend) {
			pvalue = common_util_get_propvals(
				prcpts->pparray[i], PROP_TAG_RECIPIENTTYPE);
			if (NULL == pvalue) {
				continue;
			}
			if (!(*static_cast<uint32_t *>(pvalue) & RECIPIENT_TYPE_NEED_RESEND))
				continue;	
		}
		/*
		if (FALSE == b_submit) {
			pvalue = common_util_get_propvals(
				prcpts->pparray[i], PROP_TAG_RESPONSIBILITY);
			if (NULL == pvalue || 0 != *(uint8_t*)pvalue) {
				continue;
			}
		}
		*/
		pnode->pdata = common_util_get_propvals(prcpts->pparray[i], PR_SMTP_ADDRESS);
		if (NULL != pnode->pdata && '\0' != ((char*)pnode->pdata)[0]) {
			double_list_append_as_tail(&temp_list, pnode);
			continue;
		}
		pvalue = common_util_get_propvals(
			prcpts->pparray[i], PROP_TAG_ADDRESSTYPE);
		if (NULL == pvalue) {
 CONVERT_ENTRYID:
			pvalue = common_util_get_propvals(prcpts->pparray[i], PR_ENTRYID);
			if (NULL == pvalue) {
				log_err("W-1285: Cannot get recipient entryid while sending mid:0x%llx", LLU(message_id));
				return FALSE;
			}
			if (!common_util_entryid_to_username(static_cast<BINARY *>(pvalue),
			    username, GX_ARRAY_SIZE(username))) {
				log_err("W-1284: Cannot convert recipient entryid to SMTP address while sending mid:0x%llx", LLU(message_id));
				return FALSE;	
			}
			auto tmp_len = strlen(username) + 1;
			pnode->pdata = common_util_alloc(tmp_len);
			if (NULL == pnode->pdata) {
				return FALSE;
			}
			memcpy(pnode->pdata, username, tmp_len);
		} else {
			if (strcasecmp(static_cast<char *>(pvalue), "SMTP") == 0) {
				pnode->pdata = common_util_get_propvals(prcpts->pparray[i], PR_EMAIL_ADDRESS);
				if (NULL == pnode->pdata) {
					log_err("W-1283: Cannot get email address of recipient of SMTP address type while sending mid:0x%llx", LLU(message_id));
					return FALSE;
				}
			} else if (strcasecmp(static_cast<char *>(pvalue), "EX") == 0) {
				pvalue = common_util_get_propvals(prcpts->pparray[i], PR_EMAIL_ADDRESS);
				if (NULL == pvalue) {
					goto CONVERT_ENTRYID;
				}
				if (!common_util_essdn_to_username(static_cast<char *>(pvalue),
				    username, GX_ARRAY_SIZE(username)))
					goto CONVERT_ENTRYID;
				auto tmp_len = strlen(username) + 1;
				pnode->pdata = common_util_alloc(tmp_len);
				if (NULL == pnode->pdata) {
					return FALSE;
				}
				memcpy(pnode->pdata, username, tmp_len);
			} else {
				goto CONVERT_ENTRYID;
			}
		}
		double_list_append_as_tail(&temp_list, pnode);
	}
	if (0 == double_list_get_nodes_num(&temp_list)) {
		log_err("W-1282: Empty converted recipients list while sending mid:0x%llx", LLU(message_id));
		return FALSE;
	}
	pvalue = common_util_get_propvals(&pmsgctnt->proplist,
					PROP_TAG_INTERNETMAILOVERRIDEFORMAT);
	if (NULL == pvalue) {
		body_type = OXCMAIL_BODY_PLAIN_AND_HTML;
	} else {
		if (*(uint32_t*)pvalue & MESSAGE_FORMAT_PLAIN_AND_HTML) {
			body_type = OXCMAIL_BODY_PLAIN_AND_HTML;
		} else if (*(uint32_t*)pvalue & MESSAGE_FORMAT_HTML_ONLY) {
			body_type = OXCMAIL_BODY_HTML_ONLY;
		} else {
			body_type = OXCMAIL_BODY_PLAIN_ONLY;
		}
	}
	common_util_set_dir(plogon->get_dir());
	/* try to avoid TNEF message */
	if (FALSE == oxcmail_export(pmsgctnt, FALSE,
		body_type, g_mime_pool, &imail, common_util_alloc,
		common_util_get_propids, common_util_get_propname)) {
		log_err("W-1281: Failed to export to RFC5322 mail while sending mid:0x%llx", LLU(message_id));
		return FALSE;	
	}
	if (!common_util_send_mail(&imail, plogon->get_account(), &temp_list)) {
		mail_free(&imail);
		log_err("W-1280: Failed to send mid:0x%llx via SMTP", LLU(message_id));
		return FALSE;
	}
	mail_free(&imail);
	
	pvalue = common_util_get_propvals(&pmsgctnt->proplist,
							PROP_TAG_DELETEAFTERSUBMIT);
	b_delete = FALSE;
	if (NULL != pvalue && 0 != *(uint8_t*)pvalue) {
		b_delete = TRUE;
	}
	common_util_remove_propvals(&pmsgctnt->proplist,
							PROP_TAG_SENTMAILSVREID);
	ptarget = static_cast<BINARY *>(common_util_get_propvals(&pmsgctnt->proplist, PROP_TAG_TARGETENTRYID));
	if (NULL != ptarget) {
		if (FALSE == common_util_from_message_entryid(
			plogon, ptarget, &folder_id, &new_id)) {
			log_err("W-1279: Failed to retrieve target entryid while sending mid:0x%llx", LLU(message_id));
			return FALSE;	
		}
		if (!exmdb_client_clear_submit(plogon->get_dir(), message_id, false)) {
			log_err("W-1278: Failed to clear submit flag while sending mid:0x%llx", LLU(message_id));
			return FALSE;
		}
		if (!exmdb_client_movecopy_message(plogon->get_dir(),
		    plogon->account_id, cpid, message_id, folder_id,
		    new_id, TRUE, &b_result)) {
			log_err("W-1277: Failed to move to target folder while sending mid:0x%llx", LLU(message_id));
			return FALSE;
		}
	} else if (TRUE == b_delete) {
		exmdb_client_delete_message(plogon->get_dir(),
			plogon->account_id, cpid, parent_id, message_id,
			TRUE, &b_result);
	} else {
		if (!exmdb_client_clear_submit(plogon->get_dir(), message_id, false)) {
			log_err("W-1276: Failed to clear submit flag while sending mid:0x%llx", LLU(message_id));
			return FALSE;
		}
		ids.count = 1;
		ids.pids = &message_id;
		if (!exmdb_client_movecopy_messages(plogon->get_dir(),
		    plogon->account_id, cpid, false, nullptr, parent_id,
		    rop_util_make_eid_ex(1, PRIVATE_FID_SENT_ITEMS),
		    false, &ids, &b_partial)) {
			log_err("W-1275: Failed to move to \"Sent\" folder while sending mid:0x%llx", LLU(message_id));
			return FALSE;	
		}
	}
	return TRUE;
}

LIB_BUFFER* common_util_get_allocator()
{
	return g_file_allocator;
}

void common_util_init(const char *org_name, int average_blocks,
	int max_rcpt, int max_message, unsigned int max_mail_len,
	unsigned int max_rule_len, const char *smtp_ip, uint16_t smtp_port,
	const char *submit_command)
{
	gx_strlcpy(g_emsmdb_org_name, org_name, arsizeof(g_emsmdb_org_name));
	g_average_blocks = average_blocks;
	g_max_rcpt = max_rcpt;
	g_max_message = max_message;
	g_max_mail_len = max_mail_len;
	g_max_rule_len = max_rule_len;
	gx_strlcpy(g_smtp_ip, smtp_ip, GX_ARRAY_SIZE(g_smtp_ip));
	g_smtp_port = smtp_port;
	gx_strlcpy(g_submit_command, submit_command, GX_ARRAY_SIZE(g_submit_command));
	g_faststream_id = 0;
	pthread_key_create(&g_dir_key, NULL);
}

int common_util_run()
{
	int mime_num;
	int context_num;
	
	context_num = get_context_num();

#define E(f, s) do { \
	query_service2(s, f); \
	if ((f) == nullptr) { \
		printf("[%s]: failed to get the \"%s\" service\n", "exchange_emsmdb", (s)); \
		return -1; \
	} \
} while (false)

	E(common_util_get_username_from_id, "get_username_from_id");
	E(common_util_get_maildir, "get_maildir");
	E(common_util_get_homedir, "get_homedir");
	E(common_util_get_user_displayname, "get_user_displayname");
	E(common_util_check_mlist_include, "check_mlist_include");
	E(common_util_get_user_lang, "get_user_lang");
	E(common_util_get_timezone, "get_timezone");
	E(common_util_get_id_from_username, "get_id_from_username");
	E(common_util_get_user_ids, "get_user_ids");
	E(common_util_get_domain_ids, "get_domain_ids");
	E(common_util_check_same_org, "check_same_org");
	E(common_util_get_homedir_by_id, "get_homedir_by_id");
	E(common_util_get_domainname_from_id, "get_domainname_from_id");
	E(common_util_get_id_from_maildir, "get_id_from_maildir");
	E(common_util_get_id_from_homedir, "get_id_from_homedir");
	E(common_util_lang_to_charset, "lang_to_charset");
	E(common_util_cpid_to_charset, "cpid_to_charset");
	E(common_util_charset_to_cpid, "charset_to_cpid");
	E(common_util_lcid_to_ltag, "lcid_to_ltag");
	E(common_util_ltag_to_lcid, "ltag_to_lcid");
	E(common_util_verify_cpid, "verify_cpid");
	E(common_util_add_timer, "add_timer");
	E(common_util_cancel_timer, "cancel_timer");
	E(common_util_mime_to_extension, "mime_to_extension");
	E(common_util_extension_to_mime, "extension_to_mime");
#undef E

	if (!oxcmail_init_library(g_emsmdb_org_name,
		common_util_get_user_ids, common_util_get_username_from_id,
		common_util_ltag_to_lcid, common_util_lcid_to_ltag,
		common_util_charset_to_cpid, common_util_cpid_to_charset,
		common_util_mime_to_extension, common_util_extension_to_mime)) {
		printf("[exchange_emsmdb]: Failed to init oxcmail library\n");
		return -2;
	}
	g_file_allocator = lib_buffer_init(FILE_ALLOC_SIZE,
						g_average_blocks*context_num, TRUE);
	if (NULL == g_file_allocator) {
		printf("[exchange_emsmdb]: Failed to init mem file allocator\n");
		return -3;
	}
	mime_num = 16*context_num;
	if (mime_num < 1024) {
		mime_num = 1024;
	} else if (mime_num > 16*1024) {
		mime_num = 16*1024;
	}
	g_mime_pool = mime_pool_init(mime_num, 16, TRUE);
	if (NULL == g_mime_pool) {
		printf("[exchange_emsmdb]: Failed to init MIME pool\n");
		return -4;
	}
	return 0;
}

void common_util_stop()
{
	if (NULL != g_file_allocator) {
		lib_buffer_free(g_file_allocator);
		g_file_allocator = NULL;
	}
	if (NULL != g_mime_pool) {
		mime_pool_free(g_mime_pool);
		g_mime_pool = NULL;
	}
}

void common_util_free()
{
	pthread_key_delete(g_dir_key);
}

unsigned int common_util_get_param(int param)
{
	switch (param) {
	case COMMON_UTIL_MAX_RCPT:
		return g_max_rcpt;
	case COMMON_UTIL_MAX_MESSAGE:
		return g_max_message;
	case COMMON_UTIL_MAX_MAIL_LENGTH:
		return g_max_mail_len;
	case COMMON_UTIL_MAX_EXTRULE_LENGTH:
		return g_max_rule_len;
	}
	return 0;
}

void common_util_set_param(int param, unsigned int value)
{
	switch (param) {
	case COMMON_UTIL_MAX_RCPT:
		g_max_rcpt = value;
		break;
	case COMMON_UTIL_MAX_MESSAGE:
		g_max_message = value;
		break;
	case COMMON_UTIL_MAX_MAIL_LENGTH:
		g_max_mail_len = value;
		break;
	case COMMON_UTIL_MAX_EXTRULE_LENGTH:
		g_max_rule_len = value;
		break;
	}
}

const char* common_util_get_submit_command()
{
	return g_submit_command;
}

uint32_t common_util_get_ftstream_id()
{
	uint32_t last_id;
	
	std::lock_guard id_hold(g_id_lock);
	last_id = g_faststream_id;
	g_faststream_id ++;
	return last_id;
}

MIME_POOL* common_util_get_mime_pool()
{
	return g_mime_pool;
}

static void log_err(const char *format, ...)
{
	va_list ap;
	char log_buf[2048];
	
	auto rpc_info = get_rpc_info();
	if (NULL == rpc_info.username) {
		return;
	}
	va_start(ap, format);
	vsnprintf(log_buf, sizeof(log_buf) - 1, format, ap);
	va_end(ap);
	log_buf[sizeof(log_buf) - 1] = '\0';
	log_info(3, "user=%s host=%s  %s",
		rpc_info.username, rpc_info.client_ip, log_buf);
}
