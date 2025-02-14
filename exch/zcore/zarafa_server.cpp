// SPDX-License-Identifier: GPL-2.0-only WITH linking exception
// SPDX-FileCopyrightText: 2020–2021 grommunio GmbH
// This file is part of Gromox.
#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <sys/wait.h>
#include <libHX/string.h>
#include <gromox/defs.h>
#include <gromox/fileio.h>
#include <gromox/mapidefs.h>
#include <gromox/zcore_rpc.hpp>
#include <gromox/util.hpp>
#include <gromox/guid.hpp>
#include "rpc_ext.h"
#include "ab_tree.h"
#include <gromox/rop_util.hpp>
#include <gromox/int_hash.hpp>
#include <gromox/safeint.hpp>
#include <gromox/str_hash.hpp>
#include <gromox/ext_buffer.hpp>
#include "user_object.h"
#include "common_util.h"
#include "table_object.h"
#include "zarafa_server.h"
#include "folder_object.h"
#include "message_object.h"
#include "system_services.h"
#include "icsupctx_object.h"
#include "container_object.h"
#include "icsdownctx_object.h"
#include "attachment_object.h"
#include "exmdb_client.h"
#include <gromox/idset.hpp>
#include <sys/socket.h>
#include <cstdio>
#include <poll.h>

using namespace std::string_literals;
using namespace gromox;

namespace {

struct NOTIFY_ITEM {
	NOTIFY_ITEM(const GUID &session, uint32_t store);
	NOTIFY_ITEM(NOTIFY_ITEM &&) = delete;
	~NOTIFY_ITEM();
	void operator=(NOTIFY_ITEM &&) = delete;

	DOUBLE_LIST notify_list{};
	GUID hsession{};
	uint32_t hstore = 0;
	time_t last_time = 0;
};

struct SINK_NODE {
	DOUBLE_LIST_NODE node;
	int clifd;
	time_t until_time;
	NOTIF_SINK sink;
};

struct user_info_del {
	void operator()(USER_INFO *x);
};

}

using USER_INFO_REF = std::unique_ptr<USER_INFO, user_info_del>;

static size_t g_table_size;
static std::atomic<bool> g_notify_stop{false};
static int g_ping_interval;
static pthread_t g_scan_id;
static int g_cache_interval;
static pthread_key_t g_info_key;
static std::mutex g_table_lock, g_notify_lock;
static std::unordered_map<std::string, int> g_user_table;
static std::unordered_map<std::string, NOTIFY_ITEM> g_notify_table;
static std::unordered_map<int, USER_INFO> g_session_table;

USER_INFO::USER_INFO()
{
	double_list_init(&sink_list);
}

USER_INFO::USER_INFO(USER_INFO &&o) :
	hsession(o.hsession), user_id(o.user_id), domain_id(o.domain_id),
	org_id(o.org_id), username(std::move(o.username)),
	lang(std::move(o.lang)), maildir(std::move(o.maildir)),
	homedir(std::move(o.homedir)), cpid(o.cpid), flags(o.flags),
	last_time(o.last_time), reload_time(o.reload_time),
	ptree(std::move(o.ptree)), sink_list(o.sink_list)
{
	o.sink_list = {};
}

USER_INFO::~USER_INFO()
{
	auto pinfo = this;
	DOUBLE_LIST_NODE *pnode;
	while ((pnode = double_list_pop_front(&pinfo->sink_list)) != nullptr) {
		auto psink_node = static_cast<SINK_NODE *>(pnode->pdata);
		close(psink_node->clifd);
		free(psink_node->sink.padvise);
		free(psink_node);
	}
	double_list_free(&pinfo->sink_list);
	if (pinfo->ptree != nullptr) {
		common_util_build_environment();
		pinfo->ptree.reset();
		common_util_free_environment();
	}
}

static int zarafa_server_get_user_id(GUID hsession)
{
	int user_id;
	
	memcpy(&user_id, hsession.node, sizeof(int));
	return user_id;
}

static USER_INFO_REF zarafa_server_query_session(GUID hsession)
{
	int user_id;
	
	user_id = zarafa_server_get_user_id(hsession);
	std::unique_lock tl_hold(g_table_lock);
	auto iter = g_session_table.find(user_id);
	if (iter == g_session_table.end())
		return nullptr;
	auto pinfo = &iter->second;
	if (guid_compare(&hsession, &pinfo->hsession) != 0)
		return nullptr;
	pinfo->reference ++;
	time(&pinfo->last_time);
	tl_hold.unlock();
	pinfo->lock.lock();
	pthread_setspecific(g_info_key, pinfo);
	return USER_INFO_REF(pinfo);
}

USER_INFO *zarafa_server_get_info()
{
	return static_cast<USER_INFO *>(pthread_getspecific(g_info_key));
}

void user_info_del::operator()(USER_INFO *pinfo)
{
	pinfo->lock.unlock();
	std::unique_lock tl_hold(g_table_lock);
	pinfo->reference --;
	tl_hold.unlock();
	pthread_setspecific(g_info_key, NULL);
}

NOTIFY_ITEM::NOTIFY_ITEM(const GUID &ses, uint32_t store) :
	hsession(ses), hstore(store)
{
	double_list_init(&notify_list);
	time(&last_time);
}

NOTIFY_ITEM::~NOTIFY_ITEM()
{
	DOUBLE_LIST_NODE *pnode;
	while ((pnode = double_list_pop_front(&notify_list)) != nullptr) {
		common_util_free_znotification(static_cast<ZNOTIFICATION *>(pnode->pdata));
		free(pnode);
	}
	double_list_free(&notify_list);
}

static void *zcorezs_scanwork(void *param)
{
	int count;
	int tv_msec;
	BINARY tmp_bin;
	time_t cur_time;
	uint8_t tmp_byte;
	struct pollfd fdpoll;
	ZCORE_RPC_RESPONSE response;
	SINK_NODE *psink_node;
	DOUBLE_LIST temp_list;
	DOUBLE_LIST temp_list1;
	DOUBLE_LIST_NODE *pnode;
	DOUBLE_LIST_NODE *ptail;
	
	count = 0;
	double_list_init(&temp_list);
	double_list_init(&temp_list1);
	response.call_id = zcore_callid::NOTIFDEQUEUE;
	response.result = ecSuccess;
	response.payload.notifdequeue.notifications.count = 0;
	response.payload.notifdequeue.notifications.ppnotification = NULL;
	while (!g_notify_stop) {
		sleep(1);
		count ++;
		if (count >= g_ping_interval) {
			count = 0;
		}
		std::unique_lock tl_hold(g_table_lock);
		time(&cur_time);
		for (auto iter = g_session_table.begin(); iter != g_session_table.end(); ) {
			auto pinfo = &iter->second;
			if (0 != pinfo->reference) {
				++iter;
				continue;
			}
			ptail = double_list_get_tail(&pinfo->sink_list);
			while ((pnode = double_list_pop_front(&pinfo->sink_list)) != nullptr) {
				psink_node = (SINK_NODE*)pnode->pdata;
				if (cur_time >= psink_node->until_time) {
					double_list_append_as_tail(&temp_list1, pnode);
				} else {
					double_list_append_as_tail(
						&pinfo->sink_list, pnode);
				}
				if (pnode == ptail) {
					break;
				}
			}
			if (cur_time - pinfo->reload_time >= g_cache_interval) {
				common_util_build_environment();
				auto ptree = object_tree_create(pinfo->get_maildir());
				if (NULL != ptree) {
					pinfo->ptree = std::move(ptree);
					pinfo->reload_time = cur_time;
				}
				common_util_free_environment();
				++iter;
				continue;
			}
			if (cur_time - pinfo->last_time < g_cache_interval) {
				if (0 != count) {
					++iter;
					continue;
				}
				pnode = me_alloc<DOUBLE_LIST_NODE>();
				if (pnode == nullptr) {
					++iter;
					continue;
				}
				pnode->pdata = strdup(pinfo->get_maildir());
				if (NULL == pnode->pdata) {
					free(pnode);
					++iter;
					continue;
				}
				double_list_append_as_tail(&temp_list, pnode);
				++iter;
			} else {
				if (0 != double_list_get_nodes_num(&pinfo->sink_list)) {
					++iter;
					continue;
				}
				common_util_build_environment();
				pinfo->ptree.reset();
				common_util_free_environment();
				double_list_free(&pinfo->sink_list);
				g_user_table.erase(pinfo->username);
				iter = g_session_table.erase(iter);
			}
		}
		tl_hold.unlock();
		while ((pnode = double_list_pop_front(&temp_list)) != nullptr) {
			common_util_build_environment();
			exmdb_client::ping_store(static_cast<char *>(pnode->pdata));
			common_util_free_environment();
			free(pnode->pdata);
			free(pnode);
		}
		while ((pnode = double_list_pop_front(&temp_list1)) != nullptr) {
			psink_node = (SINK_NODE*)pnode->pdata;
			if (TRUE == rpc_ext_push_response(
				&response, &tmp_bin)) {
				tv_msec = SOCKET_TIMEOUT * 1000;
				fdpoll.fd = psink_node->clifd;
				fdpoll.events = POLLOUT|POLLWRBAND;
				if (1 == poll(&fdpoll, 1, tv_msec)) {
					write(psink_node->clifd, tmp_bin.pb, tmp_bin.cb);
				}
				free(tmp_bin.pb);
				shutdown(psink_node->clifd, SHUT_WR);
				if (read(psink_node->clifd, &tmp_byte, 1))
					/* ignore */;
			}
			close(psink_node->clifd);
			free(psink_node->sink.padvise);
			free(psink_node);
		}
		if (0 != count) {
			continue;
		}
		time(&cur_time);
		std::unique_lock nl_hold(g_notify_lock);
		for (auto iter1 = g_notify_table.begin(); iter1 != g_notify_table.end(); ) {
			auto pnitem = &iter1->second;
			if (cur_time - pnitem->last_time >= g_cache_interval)
				iter1 = g_notify_table.erase(iter1);
			else
				++iter1;
		}
	}
	return NULL;
}

static void zarafa_server_notification_proc(const char *dir,
	BOOL b_table, uint32_t notify_id, const DB_NOTIFY *pdb_notify)
{
	int i;
	int tv_msec;
	void *pvalue;
	BINARY *pbin;
	GUID hsession;
	BINARY tmp_bin;
	uint32_t hstore;
	uint8_t tmp_byte;
	uint64_t old_eid;
	uint8_t mapi_type;
	char tmp_buff[256];
	uint64_t folder_id;
	uint64_t parent_id;
	uint64_t message_id;
	struct pollfd fdpoll;
	ZCORE_RPC_RESPONSE response;
	SINK_NODE *psink_node;
	uint64_t old_parentid;
	PROPTAG_ARRAY proptags;
	TPROPVAL_ARRAY propvals;
	DOUBLE_LIST_NODE *pnode;
	uint32_t proptag_buff[2];
	ZNOTIFICATION *pnotification;
	NEWMAIL_ZNOTIFICATION *pnew_mail;
	OBJECT_ZNOTIFICATION *pobj_notify;
	
	if (b_table)
		return;
	snprintf(tmp_buff, arsizeof(tmp_buff), "%u|%s", notify_id, dir);
	std::unique_lock nl_hold(g_notify_lock);
	auto iter = g_notify_table.find(tmp_buff);
	if (iter == g_notify_table.end())
		return;
	auto pitem = &iter->second;
	hsession = pitem->hsession;
	hstore = pitem->hstore;
	nl_hold.unlock();
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return;
	auto pstore = pinfo->ptree->get_object<STORE_OBJECT>(hstore, &mapi_type);
	if (pstore == nullptr || mapi_type != ZMG_STORE ||
	    strcmp(dir, pstore->get_dir()) != 0)
		return;
	pnotification = cu_alloc<ZNOTIFICATION>();
	if (pnotification == nullptr)
		return;
	switch (pdb_notify->type) {
	case DB_NOTIFY_TYPE_NEW_MAIL: {
		pnotification->event_type = EVENT_TYPE_NEWMAIL;
		pnew_mail = cu_alloc<NEWMAIL_ZNOTIFICATION>();
		if (pnew_mail == nullptr)
			return;
		pnotification->pnotification_data = pnew_mail;
		auto nt = static_cast<DB_NOTIFY_NEW_MAIL *>(pdb_notify->pdata);
		folder_id = common_util_convert_notification_folder_id(nt->folder_id);
		message_id = rop_util_make_eid_ex(1, nt->message_id);
		pbin = common_util_to_message_entryid(
				pstore, folder_id, message_id);
		if (pbin == nullptr)
			return;
		pnew_mail->entryid = *pbin;
		pbin = common_util_to_folder_entryid(pstore, folder_id);
		if (pbin == nullptr)
			return;
		pnew_mail->parentid = *pbin;
		proptags.count = 2;
		proptags.pproptag = proptag_buff;
		proptag_buff[0] = PROP_TAG_MESSAGECLASS;
		proptag_buff[1] = PR_MESSAGE_FLAGS;
		if (!exmdb_client::get_message_properties(dir,
			NULL, 0, message_id, &proptags, &propvals)) {
			return;
		}
		pvalue = common_util_get_propvals(
			&propvals, PROP_TAG_MESSAGECLASS);
		if (pvalue == nullptr)
			return;
		pnew_mail->message_class = static_cast<char *>(pvalue);
		pvalue = common_util_get_propvals(&propvals, PR_MESSAGE_FLAGS);
		if (pvalue == nullptr)
			return;
		pnew_mail->message_flags = *(uint32_t*)pvalue;
		break;
	}
	case DB_NOTIFY_TYPE_FOLDER_CREATED: {
		pnotification->event_type = EVENT_TYPE_OBJECTCREATED;
		pobj_notify = cu_alloc<OBJECT_ZNOTIFICATION>();
		if (pobj_notify == nullptr)
			return;
		memset(pobj_notify, 0, sizeof(OBJECT_ZNOTIFICATION));
		pnotification->pnotification_data = pobj_notify;
		auto nt = static_cast<DB_NOTIFY_FOLDER_CREATED *>(pdb_notify->pdata);
		folder_id = common_util_convert_notification_folder_id(nt->folder_id);
		parent_id = common_util_convert_notification_folder_id(nt->parent_id);
		pobj_notify->object_type = OBJECT_FOLDER;
		pbin = common_util_to_folder_entryid(pstore, folder_id);
		if (pbin == nullptr)
			return;
		pobj_notify->pentryid = pbin;
		pbin = common_util_to_folder_entryid(pstore, parent_id);
		if (pbin == nullptr)
			return;
		pobj_notify->pparentid = pbin;
		break;
	}
	case DB_NOTIFY_TYPE_MESSAGE_CREATED: {
		pnotification->event_type = EVENT_TYPE_OBJECTCREATED;
		pobj_notify = cu_alloc<OBJECT_ZNOTIFICATION>();
		if (pobj_notify == nullptr)
			return;
		memset(pobj_notify, 0, sizeof(OBJECT_ZNOTIFICATION));
		pnotification->pnotification_data = pobj_notify;
		auto nt = static_cast<DB_NOTIFY_MESSAGE_CREATED *>(pdb_notify->pdata);
		folder_id = common_util_convert_notification_folder_id(nt->folder_id);
		message_id = rop_util_make_eid_ex(1, nt->message_id);
		pobj_notify->object_type = OBJECT_MESSAGE;
		pbin = common_util_to_message_entryid(
				pstore, folder_id, message_id);
		pobj_notify->pentryid = pbin;
		pbin = common_util_to_folder_entryid(pstore, folder_id);
		if (pbin == nullptr)
			return;
		pobj_notify->pparentid = pbin;
		break;
	}
	case DB_NOTIFY_TYPE_FOLDER_DELETED: {
		pnotification->event_type = EVENT_TYPE_OBJECTDELETED;
		pobj_notify = cu_alloc<OBJECT_ZNOTIFICATION>();
		if (pobj_notify == nullptr)
			return;
		memset(pobj_notify, 0, sizeof(OBJECT_ZNOTIFICATION));
		pnotification->pnotification_data = pobj_notify;
		auto nt = static_cast<DB_NOTIFY_FOLDER_DELETED *>(pdb_notify->pdata);
		folder_id = common_util_convert_notification_folder_id(nt->folder_id);
		parent_id = common_util_convert_notification_folder_id(nt->parent_id);
		pobj_notify->object_type = OBJECT_FOLDER;
		pbin = common_util_to_folder_entryid(pstore, folder_id);
		if (pbin == nullptr)
			return;
		pobj_notify->pentryid = pbin;
		pbin = common_util_to_folder_entryid(pstore, parent_id);
		if (pbin == nullptr)
			return;
		pobj_notify->pparentid = pbin;
		break;
	}
	case DB_NOTIFY_TYPE_MESSAGE_DELETED: {
		pnotification->event_type = EVENT_TYPE_OBJECTDELETED;
		pobj_notify = cu_alloc<OBJECT_ZNOTIFICATION>();
		if (pobj_notify == nullptr)
			return;
		memset(pobj_notify, 0, sizeof(OBJECT_ZNOTIFICATION));
		pnotification->pnotification_data = pobj_notify;
		auto nt = static_cast<DB_NOTIFY_MESSAGE_DELETED *>(pdb_notify->pdata);
		folder_id = common_util_convert_notification_folder_id(nt->folder_id);
		message_id = rop_util_make_eid_ex(1, nt->message_id);
		pobj_notify->object_type = OBJECT_MESSAGE;
		pbin = common_util_to_message_entryid(
				pstore, folder_id, message_id);
		if (pbin == nullptr)
			return;
		pobj_notify->pentryid = pbin;
		pbin = common_util_to_folder_entryid(pstore, folder_id);
		if (pbin == nullptr)
			return;
		pobj_notify->pparentid = pbin;
		break;
	}
	case DB_NOTIFY_TYPE_FOLDER_MODIFIED: {
		pnotification->event_type = EVENT_TYPE_OBJECTMODIFIED;
		pobj_notify = cu_alloc<OBJECT_ZNOTIFICATION>();
		if (pobj_notify == nullptr)
			return;
		memset(pobj_notify, 0, sizeof(OBJECT_ZNOTIFICATION));
		pnotification->pnotification_data = pobj_notify;
		auto nt = static_cast<DB_NOTIFY_FOLDER_MODIFIED *>(pdb_notify->pdata);
		folder_id = common_util_convert_notification_folder_id(nt->folder_id);
		pobj_notify->object_type = OBJECT_FOLDER;
		pbin = common_util_to_folder_entryid(pstore, folder_id);
		if (pbin == nullptr)
			return;
		pobj_notify->pentryid = pbin;
		break;
	}
	case DB_NOTIFY_TYPE_MESSAGE_MODIFIED: {
		pnotification->event_type = EVENT_TYPE_OBJECTMODIFIED;
		pobj_notify = cu_alloc<OBJECT_ZNOTIFICATION>();
		if (pobj_notify == nullptr)
			return;
		memset(pobj_notify, 0, sizeof(OBJECT_ZNOTIFICATION));
		pnotification->pnotification_data = pobj_notify;
		auto nt = static_cast<DB_NOTIFY_MESSAGE_MODIFIED *>(pdb_notify->pdata);
		folder_id = common_util_convert_notification_folder_id(nt->folder_id);
		message_id = rop_util_make_eid_ex(1, nt->message_id);
		pobj_notify->object_type = OBJECT_MESSAGE;
		pbin = common_util_to_message_entryid(
				pstore, folder_id, message_id);
		if (pbin == nullptr)
			return;
		pobj_notify->pentryid = pbin;
		pbin = common_util_to_folder_entryid(pstore, folder_id);
		if (pbin == nullptr)
			return;
		pobj_notify->pparentid = pbin;
		break;
	}
	case DB_NOTIFY_TYPE_FOLDER_MOVED:
	case DB_NOTIFY_TYPE_FOLDER_COPIED: {
		pnotification->event_type = pdb_notify->type == DB_NOTIFY_TYPE_FOLDER_MOVED ?
		                            EVENT_TYPE_OBJECTMOVED : EVENT_TYPE_OBJECTCOPIED;
		pobj_notify = cu_alloc<OBJECT_ZNOTIFICATION>();
		if (pobj_notify == nullptr)
			return;
		memset(pobj_notify, 0, sizeof(OBJECT_ZNOTIFICATION));
		pnotification->pnotification_data = pobj_notify;
		auto nt = static_cast<DB_NOTIFY_FOLDER_MVCP *>(pdb_notify->pdata);
		folder_id = common_util_convert_notification_folder_id(nt->folder_id);
		parent_id = common_util_convert_notification_folder_id(nt->parent_id);
		old_eid = common_util_convert_notification_folder_id(nt->old_folder_id);
		old_parentid = common_util_convert_notification_folder_id(nt->old_parent_id);
		pobj_notify->object_type = OBJECT_FOLDER;
		pbin = common_util_to_folder_entryid(pstore, folder_id);
		if (pbin == nullptr)
			return;
		pobj_notify->pentryid = pbin;
		pbin = common_util_to_folder_entryid(pstore, parent_id);
		if (pbin == nullptr)
			return;
		pobj_notify->pparentid = pbin;
		pbin = common_util_to_folder_entryid(pstore, old_eid);
		if (pbin == nullptr)
			return;
		pobj_notify->pold_entryid = pbin;
		pbin = common_util_to_folder_entryid(pstore, old_parentid);
		if (pbin == nullptr)
			return;
		pobj_notify->pold_parentid = pbin;
		break;
	}
	case DB_NOTIFY_TYPE_MESSAGE_MOVED:
	case DB_NOTIFY_TYPE_MESSAGE_COPIED: {
		pnotification->event_type = pdb_notify->type == DB_NOTIFY_TYPE_MESSAGE_MOVED ?
		                            EVENT_TYPE_OBJECTMOVED : EVENT_TYPE_OBJECTCOPIED;
		pobj_notify = cu_alloc<OBJECT_ZNOTIFICATION>();
		if (pobj_notify == nullptr)
			return;
		memset(pobj_notify, 0, sizeof(OBJECT_ZNOTIFICATION));
		pnotification->pnotification_data = pobj_notify;
		auto nt = static_cast<DB_NOTIFY_MESSAGE_MVCP *>(pdb_notify->pdata);
		old_parentid = common_util_convert_notification_folder_id(nt->old_folder_id);
		old_eid = rop_util_make_eid_ex(1, nt->old_message_id);
		folder_id = common_util_convert_notification_folder_id(nt->folder_id);
		message_id = rop_util_make_eid_ex(1, nt->message_id);
		pobj_notify->object_type = OBJECT_MESSAGE;
		pbin = common_util_to_message_entryid(
				pstore, folder_id, message_id);
		if (pbin == nullptr)
			return;
		pobj_notify->pentryid = pbin;
		pbin = common_util_to_folder_entryid(
							pstore, folder_id);
		if (pbin == nullptr)
			return;
		pobj_notify->pparentid = pbin;
		pbin = common_util_to_message_entryid(
				pstore, old_parentid, old_eid);
		if (pbin == nullptr)
			return;
		pobj_notify->pold_entryid = pbin;
		pbin = common_util_to_folder_entryid(
						pstore, old_parentid);
		if (pbin == nullptr)
			return;
		pobj_notify->pold_parentid = pbin;
		break;
	}
	case DB_NOTIFY_TYPE_SEARCH_COMPLETED: {
		pnotification->event_type = EVENT_TYPE_SEARCHCOMPLETE;
		pobj_notify = cu_alloc<OBJECT_ZNOTIFICATION>();
		if (pobj_notify == nullptr)
			return;
		memset(pobj_notify, 0, sizeof(OBJECT_ZNOTIFICATION));
		pnotification->pnotification_data = pobj_notify;
		auto nt = static_cast<DB_NOTIFY_SEARCH_COMPLETED *>(pdb_notify->pdata);
		folder_id = common_util_convert_notification_folder_id(nt->folder_id);
		pobj_notify->object_type = OBJECT_FOLDER;
		pbin = common_util_to_folder_entryid(pstore, folder_id);
		if (pbin == nullptr)
			return;
		pobj_notify->pentryid = pbin;
		break;
	}
	default:
		return;
	}
	for (pnode=double_list_get_head(&pinfo->sink_list); NULL!=pnode;
		pnode=double_list_get_after(&pinfo->sink_list, pnode)) {
		psink_node = (SINK_NODE*)pnode->pdata;
		for (i=0; i<psink_node->sink.count; i++) {
			if (psink_node->sink.padvise[i].sub_id != notify_id ||
			    hstore != psink_node->sink.padvise[i].hstore)
				continue;
			double_list_remove(&pinfo->sink_list, pnode);
			response.call_id = zcore_callid::NOTIFDEQUEUE;
			response.result = ecSuccess;
			response.payload.notifdequeue.notifications.count = 1;
			response.payload.notifdequeue.notifications.ppnotification =
				&pnotification;
			tv_msec = SOCKET_TIMEOUT * 1000;
			fdpoll.fd = psink_node->clifd;
			fdpoll.events = POLLOUT | POLLWRBAND;
			if (FALSE == rpc_ext_push_response(
				&response, &tmp_bin)) {
				tmp_byte = zcore_response::PUSH_ERROR;
				if (1 == poll(&fdpoll, 1, tv_msec)) {
					write(psink_node->clifd, &tmp_byte, 1);
				}
			} else {
				if (1 == poll(&fdpoll, 1, tv_msec)) {
					write(psink_node->clifd, tmp_bin.pb, tmp_bin.cb);
				}
				free(tmp_bin.pb);
			}
			close(psink_node->clifd);
			free(psink_node->sink.padvise);
			free(psink_node);
			return;
		}
	}
	pnode = me_alloc<DOUBLE_LIST_NODE>();
	if (pnode == nullptr)
		return;
	pnode->pdata = common_util_dup_znotification(pnotification, FALSE);
	if (NULL == pnode->pdata) {
		free(pnode);
		return;
	}
	nl_hold.lock();
	iter = g_notify_table.find(tmp_buff);
	pitem = iter != g_notify_table.end() ? &iter->second : nullptr;
	if (pitem != nullptr)
		double_list_append_as_tail(&pitem->notify_list, pnode);
	nl_hold.unlock();
	if (NULL == pitem) {
		common_util_free_znotification(static_cast<ZNOTIFICATION *>(pnode->pdata));
		free(pnode);
	}
}

void zarafa_server_init(size_t table_size, int cache_interval,
    int ping_interval)
{
	g_table_size = table_size;
	g_cache_interval = cache_interval;
	g_ping_interval = ping_interval;
	pthread_key_create(&g_info_key, NULL);
}

int zarafa_server_run()
{
	g_notify_stop = false;
	auto ret = pthread_create(&g_scan_id, nullptr, zcorezs_scanwork, nullptr);
	if (ret != 0) {
		printf("[zarafa_server]: E-1443: pthread_create: %s\n", strerror(ret));
		return -4;
	}
	pthread_setname_np(g_scan_id, "zarafa");
	exmdb_client_register_proc(reinterpret_cast<void *>(zarafa_server_notification_proc));
	return 0;
}

void zarafa_server_stop()
{
	g_notify_stop = true;
	pthread_kill(g_scan_id, SIGALRM);
	pthread_join(g_scan_id, NULL);
	g_session_table.clear();
	g_user_table.clear();
	g_notify_table.clear();
}

void zarafa_server_free()
{
	pthread_key_delete(g_info_key);
}

int zarafa_server_get_param(int param)
{
	switch (param) {
	case USER_TABLE_SIZE:
		return g_table_size;
	case USER_TABLE_USED:
		return g_user_table.size();
	default:
		return -1;
	}
}

uint32_t zarafa_server_logon(const char *username,
	const char *password, uint32_t flags, GUID *phsession)
{
	int org_id;
	int user_id;
	int domain_id;
	char lang[32];
	char charset[64];
	char reason[256];
	char homedir[256];
	char maildir[256];
	char tmp_name[UADDR_SIZE];
	
	auto pdomain = strchr(username, '@');
	if (pdomain == nullptr)
		return ecUnknownUser;
	pdomain ++;
	if (password != nullptr && !system_services_auth_login(username,
	    password, maildir, lang, reason, arsizeof(reason)))
		return ecLoginFailure;
	gx_strlcpy(tmp_name, username, GX_ARRAY_SIZE(tmp_name));
	HX_strlower(tmp_name);
	std::unique_lock tl_hold(g_table_lock);
	auto iter = g_user_table.find(tmp_name);
	if (iter != g_user_table.end()) {
		user_id = iter->second;
		auto st_iter = g_session_table.find(user_id);
		if (st_iter != g_session_table.end()) {
			auto pinfo = &st_iter->second;
			time(&pinfo->last_time);
			*phsession = pinfo->hsession;
			return ecSuccess;
		}
		g_user_table.erase(iter);
	}
	tl_hold.unlock();
	if (FALSE == system_services_get_id_from_username(
		username, &user_id) ||
		FALSE == system_services_get_homedir(
		pdomain, homedir) ||
		FALSE == system_services_get_domain_ids(
		pdomain, &domain_id, &org_id)) {
		return ecError;
	}
	if (password == nullptr &&
	    (!system_services_get_maildir(username, maildir) ||
	    !system_services_get_user_lang(username, lang)))
		return ecError;

	USER_INFO tmp_info;
	tmp_info.hsession = guid_random_new();
	memcpy(tmp_info.hsession.node, &user_id, sizeof(int));
	tmp_info.user_id = user_id;
	tmp_info.domain_id = domain_id;
	tmp_info.org_id = org_id;
	try {
		tmp_info.username = username;
		HX_strlower(tmp_info.username.data());
		tmp_info.lang = lang;
		tmp_info.maildir = maildir;
		tmp_info.homedir = homedir;
	} catch (const std::bad_alloc &) {
		return ecMAPIOOM;
	}
	tmp_info.cpid = !system_services_lang_to_charset(lang, charset) ? 1252 :
	                system_services_charset_to_cpid(charset);
	tmp_info.flags = flags;
	time(&tmp_info.last_time);
	tmp_info.reload_time = tmp_info.last_time;
	tmp_info.ptree = object_tree_create(maildir);
	if (tmp_info.ptree == nullptr)
		return ecError;
	tl_hold.lock();
	auto st_iter = g_session_table.find(user_id);
	if (st_iter != g_session_table.end()) {
		auto pinfo = &st_iter->second;
		*phsession = pinfo->hsession;
		return ecSuccess;
	}
	if (g_session_table.size() >= g_table_size)
		return ecError;
	try {
		st_iter = g_session_table.try_emplace(user_id, std::move(tmp_info)).first;
	} catch (const std::bad_alloc &) {
		return ecError;
	}
	if (g_user_table.size() >= g_table_size) {
		g_session_table.erase(user_id);
		return ecError;
	}
	try {
		g_user_table.try_emplace(tmp_name, user_id);
	} catch (const std::bad_alloc &) {
		g_session_table.erase(user_id);
		return ecError;
	}
	*phsession = st_iter->second.hsession;
	return ecSuccess;
}

uint32_t zarafa_server_checksession(GUID hsession)
{
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	return ecSuccess;
}

uint32_t zarafa_server_uinfo(const char *username, BINARY *pentryid,
	char **ppdisplay_name, char **ppx500dn, uint32_t *pprivilege_bits)
{
	char x500dn[1024];
	EXT_PUSH ext_push;
	char display_name[1024];
	ADDRESSBOOK_ENTRYID tmp_entryid;
	
	if (FALSE == system_services_get_user_displayname(
		username, display_name) ||
		FALSE == system_services_get_user_privilege_bits(
		username, pprivilege_bits) || FALSE ==
	    common_util_username_to_essdn(username, x500dn, GX_ARRAY_SIZE(x500dn)))
		return ecNotFound;
	tmp_entryid.flags = 0;
	rop_util_get_provider_uid(PROVIDER_UID_ADDRESS_BOOK,
							tmp_entryid.provider_uid);
	tmp_entryid.version = 1;
	tmp_entryid.type = ADDRESSBOOK_ENTRYID_TYPE_LOCAL_USER;
	tmp_entryid.px500dn = x500dn;
	pentryid->pv = common_util_alloc(1280);
	if (pentryid->pv == nullptr ||
	    !ext_push.init(pentryid->pb, 1280, EXT_FLAG_UTF16) ||
	    ext_push.p_abk_eid(&tmp_entryid) != EXT_ERR_SUCCESS)
		return ecError;
	pentryid->cb = ext_push.m_offset;
	*ppdisplay_name = common_util_dup(display_name);
	*ppx500dn = common_util_dup(x500dn);
	if (NULL == *ppdisplay_name || NULL == *ppx500dn) {
		return ecError;
	}
	return ecSuccess;
}

uint32_t zarafa_server_unloadobject(GUID hsession, uint32_t hobject)
{
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	pinfo->ptree->release_object_handle(hobject);
	return ecSuccess;
}


uint32_t zarafa_server_openentry(GUID hsession, BINARY entryid,
	uint32_t flags, uint8_t *pmapi_type, uint32_t *phobject)
{
	int user_id;
	uint64_t eid;
	uint16_t type;
	BOOL b_private;
	int account_id;
	char essdn[1024];
	uint8_t loc_type;
	uint64_t folder_id;
	uint64_t message_id;
	uint32_t address_type;
	
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	type = common_util_get_messaging_entryid_type(entryid);
	switch (type) {
	case EITLT_PRIVATE_FOLDER:
	case EITLT_PUBLIC_FOLDER: {
		if (FALSE == common_util_from_folder_entryid(
			entryid, &b_private, &account_id, &folder_id)) {
			break;
		}
		auto handle = pinfo->ptree->get_store_handle(b_private, account_id);
		if (handle == INVALID_HANDLE)
			return ecNullObject;
		pinfo.reset();
		return zarafa_server_openstoreentry(hsession,
			handle, entryid, flags, pmapi_type, phobject);
	}
	case EITLT_PRIVATE_MESSAGE:
	case EITLT_PUBLIC_MESSAGE: {
		if (FALSE == common_util_from_message_entryid(
			entryid, &b_private, &account_id, &folder_id,
			&message_id)) {
			break;
		}
		auto handle = pinfo->ptree->get_store_handle(b_private, account_id);
		if (handle == INVALID_HANDLE)
			return ecNullObject;
		pinfo.reset();
		return zarafa_server_openstoreentry(hsession,
			handle, entryid, flags, pmapi_type, phobject);
	}
	}
	if (strncmp(entryid.pc, "/exmdb=", 7) == 0) {
		gx_strlcpy(essdn, entryid.pc, sizeof(essdn));
	} else if (common_util_parse_addressbook_entryid(entryid, &address_type,
	    essdn, GX_ARRAY_SIZE(essdn)) && strncmp(essdn, "/exmdb=", 7) == 0 &&
	    ADDRESSBOOK_ENTRYID_TYPE_REMOTE_USER == address_type) {
		/* do nothing */	
	} else {
		return ecInvalidParam;
	}
	if (FALSE == common_util_exmdb_locinfo_from_string(
		essdn + 7, &loc_type, &user_id, &eid)) {
		return ecNotFound;
	}
	switch (loc_type) {
	case LOC_TYPE_PRIVATE_FOLDER:
	case LOC_TYPE_PRIVATE_MESSAGE:
		b_private = TRUE;
		break;
	case LOC_TYPE_PUBLIC_FOLDER:
	case LOC_TYPE_PUBLIC_MESSAGE:
		b_private = FALSE;
		break;
	default:
		return ecNotFound;
	}
	
	auto handle = pinfo->ptree->get_store_handle(b_private, user_id);
	pinfo.reset();
	return zarafa_server_openstoreentry(hsession,
		handle, entryid, flags, pmapi_type, phobject);
}

uint32_t zarafa_server_openstoreentry(GUID hsession,
	uint32_t hobject, BINARY entryid, uint32_t flags,
	uint8_t *pmapi_type, uint32_t *phobject)
{
	BOOL b_del;
	BOOL b_owner;
	BOOL b_exist;
	void *pvalue;
	uint64_t eid;
	uint16_t type;
	BOOL b_private;
	int account_id;
	char essdn[1024];
	uint64_t fid_val;
	uint8_t loc_type;
	uint8_t mapi_type;
	uint64_t folder_id;
	uint64_t message_id;
	uint32_t tag_access;
	uint32_t permission;
	uint32_t folder_type;
	uint32_t address_type;
	
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto pstore = pinfo->ptree->get_object<STORE_OBJECT>(hobject, &mapi_type);
	if (pstore == nullptr)
		return ecNullObject;
	if (mapi_type != ZMG_STORE)
		return ecNotSupported;
	if (0 == entryid.cb) {
		folder_id = rop_util_make_eid_ex(1, pstore->b_private ?
		            PRIVATE_FID_ROOT : PUBLIC_FID_ROOT);
		message_id = 0;
	} else {
		type = common_util_get_messaging_entryid_type(entryid);
		switch (type) {
		case EITLT_PRIVATE_FOLDER:
		case EITLT_PUBLIC_FOLDER:
			if (TRUE == common_util_from_folder_entryid(
				entryid, &b_private, &account_id, &folder_id)) {
				message_id = 0;
				goto CHECK_LOC;
			}
			break;
		case EITLT_PRIVATE_MESSAGE:
		case EITLT_PUBLIC_MESSAGE:
			if (TRUE == common_util_from_message_entryid(
				entryid, &b_private, &account_id, &folder_id,
				&message_id)) {
				goto CHECK_LOC;
			}
			break;
		}
		if (strncmp(entryid.pc, "/exmdb=", 7) == 0) {
			gx_strlcpy(essdn, entryid.pc, sizeof(essdn));
		} else if (common_util_parse_addressbook_entryid(entryid,
		     &address_type, essdn, GX_ARRAY_SIZE(essdn)) &&
		     strncmp(essdn, "/exmdb=", 7) == 0 &&
		     ADDRESSBOOK_ENTRYID_TYPE_REMOTE_USER == address_type) {
			/* do nothing */	
		} else {
			return ecInvalidParam;
		}
		if (FALSE == common_util_exmdb_locinfo_from_string(
			essdn + 7, &loc_type, &account_id, &eid)) {
			return ecNotFound;
		}
		switch (loc_type) {
		case LOC_TYPE_PRIVATE_FOLDER:
			b_private = TRUE;
			folder_id = eid;
			message_id = 0;
			break;
		case LOC_TYPE_PRIVATE_MESSAGE:
			b_private = TRUE;
			message_id = eid;
			break;
		case LOC_TYPE_PUBLIC_FOLDER:
			b_private = FALSE;
			folder_id = eid;
			message_id = 0;
			break;
		case LOC_TYPE_PUBLIC_MESSAGE:
			b_private = FALSE;
			message_id = eid;
			break;
		default:
			return ecNotFound;
		}
		if (LOC_TYPE_PRIVATE_MESSAGE == loc_type ||
			LOC_TYPE_PUBLIC_MESSAGE == loc_type) {
			if (!exmdb_client_get_message_property(pstore->get_dir(),
			    nullptr, 0, message_id, PROP_TAG_PARENTFOLDERID,
			    &pvalue) || pvalue == nullptr)
				return ecError;
			folder_id = *(uint64_t*)pvalue;
		}
 CHECK_LOC:
		if (b_private != pstore->b_private ||
		    account_id != pstore->account_id)
			return ecInvalidParam;
	}
	if (0 != message_id) {
		if (!exmdb_client::check_message_deleted(pstore->get_dir(),
		    message_id, &b_del))
			return ecError;
		if (b_del && !(flags & FLAG_SOFT_DELETE))
			return ecNotFound;
		tag_access = 0;
		if (pstore->check_owner_mode()) {
			tag_access = TAG_ACCESS_MODIFY|
				TAG_ACCESS_READ|TAG_ACCESS_DELETE;
			goto PERMISSION_CHECK;
		}
		if (!exmdb_client::check_folder_permission(pstore->get_dir(),
		    folder_id, pinfo->get_username(), &permission))
			return ecError;
		if (!(permission & (frightsReadAny | frightsVisible | frightsOwner)))
			return ecAccessDenied;
		if (permission & frightsOwner) {
			tag_access = TAG_ACCESS_MODIFY|
				TAG_ACCESS_READ|TAG_ACCESS_DELETE;
			goto PERMISSION_CHECK;
		}
		if (!exmdb_client_check_message_owner(pstore->get_dir(),
		    message_id, pinfo->get_username(), &b_owner))
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
		BOOL b_writable = !(tag_access & TAG_ACCESS_MODIFY) ? false : TRUE;
		auto pmessage = message_object_create(pstore, false,
		                pinfo->cpid, message_id, &folder_id, tag_access,
		                b_writable, nullptr);
		if (pmessage == nullptr)
			return ecError;
		*phobject = pinfo->ptree->add_object_handle(hobject, ZMG_MESSAGE, pmessage.get());
		if (*phobject == INVALID_HANDLE)
			return ecError;
		pmessage.release();
		*pmapi_type = ZMG_MESSAGE;
	} else {
		if (!exmdb_client::check_folder_id(pstore->get_dir(),
		    folder_id, &b_exist))
			return ecError;
		if (!b_exist)
			return ecNotFound;
		if (!pstore->b_private) {
			if (!exmdb_client::check_folder_deleted(pstore->get_dir(),
			    folder_id, &b_del))
				return ecError;
			if (b_del && !(flags & FLAG_SOFT_DELETE))
				return ecNotFound;
		}
		if (!exmdb_client_get_folder_property(pstore->get_dir(), 0,
		    folder_id, PROP_TAG_FOLDERTYPE, &pvalue) ||
		    pvalue == nullptr)
			return ecError;
		folder_type = *(uint32_t*)pvalue;
		if (pstore->check_owner_mode()) {
			tag_access = TAG_ACCESS_MODIFY | TAG_ACCESS_READ |
					TAG_ACCESS_DELETE | TAG_ACCESS_HIERARCHY |
					TAG_ACCESS_CONTENTS | TAG_ACCESS_FAI_CONTENTS;
		} else {
			if (!exmdb_client::check_folder_permission(pstore->get_dir(),
			    folder_id, pinfo->get_username(), &permission))
				return ecError;
			if (permission == rightsNone) {
				fid_val = rop_util_get_gc_value(folder_id);
				if (pstore->b_private) {
					if (PRIVATE_FID_ROOT == fid_val ||
						PRIVATE_FID_IPMSUBTREE == fid_val) {
						permission = frightsVisible;
					}
				} else {
					if (PUBLIC_FID_ROOT == fid_val) {
						permission = frightsVisible;
					}
				}
			}
			if (!(permission & (frightsReadAny | frightsVisible | frightsOwner)))
				return ecNotFound;
			if (permission & frightsOwner) {
				tag_access = TAG_ACCESS_MODIFY | TAG_ACCESS_READ |
					TAG_ACCESS_DELETE | TAG_ACCESS_HIERARCHY |
					TAG_ACCESS_CONTENTS | TAG_ACCESS_FAI_CONTENTS;
			} else {
				tag_access = TAG_ACCESS_READ;
				if (permission & frightsCreate)
					tag_access |= TAG_ACCESS_CONTENTS |
								TAG_ACCESS_FAI_CONTENTS;
				if (permission & frightsCreateSubfolder)
					tag_access |= TAG_ACCESS_HIERARCHY;
			}
		}
		auto pfolder = folder_object_create(pstore,
			folder_id, folder_type, tag_access);
		if (pfolder == nullptr)
			return ecError;
		*phobject = pinfo->ptree->add_object_handle(hobject, ZMG_FOLDER, pfolder.get());
		if (*phobject == INVALID_HANDLE)
			return ecError;
		pfolder.release();
		*pmapi_type = ZMG_FOLDER;
	}
	return ecSuccess;
}

uint32_t zarafa_server_openabentry(GUID hsession,
	BINARY entryid, uint8_t *pmapi_type, uint32_t *phobject)
{
	GUID guid;
	int user_id;
	uint8_t type;
	std::unique_ptr<CONTAINER_OBJECT> contobj;
	std::unique_ptr<USER_OBJECT> userobj;
	void *pobject;
	int domain_id;
	uint32_t minid;
	uint8_t loc_type;
	char essdn[1024];
	char tmp_buff[16];
	uint32_t address_type;
	SIMPLE_TREE_NODE *pnode;
	CONTAINER_ID container_id;
	
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	int base_id = pinfo->org_id == 0 ? -pinfo->domain_id : pinfo->org_id;
	if (0 == entryid.cb) {
		container_id.abtree_id.base_id = base_id;
		container_id.abtree_id.minid = 0xFFFFFFFF;
		contobj = container_object_create(CONTAINER_TYPE_ABTREE, container_id);
		if (contobj == nullptr)
			return ecError;
		*pmapi_type = ZMG_ABCONT;
		*phobject = pinfo->ptree->add_object_handle(ROOT_HANDLE, *pmapi_type, contobj.get());
		if (*phobject == INVALID_HANDLE)
			return ecError;
		contobj.release();
		return ecSuccess;
	}
	if (!common_util_parse_addressbook_entryid(entryid, &address_type,
	    essdn, GX_ARRAY_SIZE(essdn))) {
		return ecInvalidParam;
	}
	if (ADDRESSBOOK_ENTRYID_TYPE_CONTAINER == address_type) {
		HX_strlower(essdn);
		if ('\0' == essdn[0]) {
			type = CONTAINER_TYPE_ABTREE;
			container_id.abtree_id.base_id = base_id;
			container_id.abtree_id.minid = 0xFFFFFFFF;;
		} else if (0 == strcmp(essdn, "/")) {
			type = CONTAINER_TYPE_ABTREE;
			container_id.abtree_id.base_id = base_id;
			container_id.abtree_id.minid = 0;
		} else if (strncmp(essdn, "/exmdb=", 7) == 0) {
			if (FALSE == common_util_exmdb_locinfo_from_string(
			    essdn + 7, &loc_type, &user_id,
			    &container_id.exmdb_id.folder_id) ||
			    LOC_TYPE_PRIVATE_FOLDER != loc_type) {
				return ecNotFound;
			}
			container_id.exmdb_id.b_private = TRUE;
			type = CONTAINER_TYPE_FOLDER;
		} else {
			if (0 != strncmp(essdn, "/guid=", 6) || 38 != strlen(essdn)) {
				return ecNotFound;
			}
			memcpy(tmp_buff, essdn + 6, 8);
			tmp_buff[8] = '\0';
			guid.time_low = strtoll(tmp_buff, NULL, 16);
			memcpy(tmp_buff, essdn + 14, 4);
			tmp_buff[4] = '\0';
			guid.time_mid = strtol(tmp_buff, NULL, 16);
			memcpy(tmp_buff, essdn + 18, 4);
			tmp_buff[4] = '\0';
			guid.time_hi_and_version = strtol(tmp_buff, NULL, 16);
			memcpy(tmp_buff, essdn + 22, 2);
			tmp_buff[2] = '\0';
			guid.clock_seq[0] = strtol(tmp_buff, NULL, 16);
			memcpy(tmp_buff, essdn + 24, 2);
			tmp_buff[2] = '\0';
			guid.clock_seq[1] = strtol(tmp_buff, NULL, 16);
			memcpy(tmp_buff, essdn + 26, 2);
			tmp_buff[2] = '\0';
			guid.node[0] = strtol(tmp_buff, NULL, 16);
			memcpy(tmp_buff, essdn + 28, 2);
			tmp_buff[2] = '\0';
			guid.node[1] = strtol(tmp_buff, NULL, 16);
			memcpy(tmp_buff, essdn + 30, 2);
			tmp_buff[2] = '\0';
			guid.node[2] = strtol(tmp_buff, NULL, 16);
			memcpy(tmp_buff, essdn + 32, 2);
			tmp_buff[2] = '\0';
			guid.node[3] = strtol(tmp_buff, NULL, 16);
			memcpy(tmp_buff, essdn + 34, 2);
			tmp_buff[2] = '\0';
			guid.node[4] = strtol(tmp_buff, NULL, 16);
			memcpy(tmp_buff, essdn + 36, 2);
			tmp_buff[2] = '\0';
			guid.node[5] = strtol(tmp_buff, NULL, 16);
			auto pbase = ab_tree_get_base(base_id);
			if (pbase == nullptr)
				return ecError;
			pnode = ab_tree_guid_to_node(pbase.get(), guid);
			if (pnode == nullptr)
				return ecNotFound;
			minid = ab_tree_get_node_minid(pnode);
			type = CONTAINER_TYPE_ABTREE;
			container_id.abtree_id.base_id = base_id;
			container_id.abtree_id.minid = minid;
		}
		contobj = container_object_create(type, container_id);
		pobject = contobj.get();
		if (pobject == nullptr)
			return ecError;
		*pmapi_type = ZMG_ABCONT;
	} else if (ADDRESSBOOK_ENTRYID_TYPE_DLIST == address_type ||
	    ADDRESSBOOK_ENTRYID_TYPE_LOCAL_USER == address_type) {
		if (FALSE == common_util_essdn_to_ids(
		    essdn, &domain_id, &user_id)) {
			return ecNotFound;
		}
		if (domain_id != pinfo->domain_id && FALSE ==
		    system_services_check_same_org(domain_id,
		    pinfo->domain_id)) {
			base_id = -domain_id;
		}
		minid = ab_tree_make_minid(MINID_TYPE_ADDRESS, user_id);
		userobj = user_object_create(base_id, minid);
		pobject = userobj.get();
		if (userobj == nullptr)
			return ecError;
		if (!userobj->check_valid())
			return ecNotFound;
		*pmapi_type = address_type == ADDRESSBOOK_ENTRYID_TYPE_DLIST ?
			      ZMG_DISTLIST : ZMG_MAILUSER;
	} else {
		return ecInvalidParam;
	}
	*phobject = pinfo->ptree->add_object_handle(ROOT_HANDLE, *pmapi_type, pobject);
	if (*phobject == INVALID_HANDLE)
		return ecError;
	contobj.release();
	userobj.release();
	return ecSuccess;
}

uint32_t zarafa_server_resolvename(GUID hsession,
	const TARRAY_SET *pcond_set, TARRAY_SET *presult_set)
{
	char *pstring;
	SINGLE_LIST temp_list;
	PROPTAG_ARRAY proptags;
	SINGLE_LIST result_list;
	SINGLE_LIST_NODE *pnode;
	
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	int base_id = pinfo->org_id == 0 ? -pinfo->domain_id : pinfo->org_id;
	auto pbase = ab_tree_get_base(base_id);
	if (pbase == nullptr)
		return ecError;
	single_list_init(&result_list);
	for (size_t i = 0; i < pcond_set->count; ++i) {
		pstring = static_cast<char *>(common_util_get_propvals(
		          pcond_set->pparray[i], PR_DISPLAY_NAME));
		if (NULL == pstring) {
			presult_set->count = 0;
			presult_set->pparray = NULL;
			return ecSuccess;
		}
		if (!ab_tree_resolvename(pbase.get(), pinfo->cpid, pstring, &temp_list))
			return ecError;
		switch (single_list_get_nodes_num(&temp_list)) {
		case 0:
			return ecNotFound;
		case 1:
			break;
		default:
			return ecAmbiguousRecip;
		}
		while ((pnode = single_list_pop_front(&temp_list)) != nullptr)
			single_list_append_as_tail(&result_list, pnode);
	}
	presult_set->count = 0;
	if (0 == single_list_get_nodes_num(&result_list)) {
		presult_set->pparray = NULL;
		return ecNotFound;
	}
	presult_set->pparray = cu_alloc<TPROPVAL_ARRAY *>(single_list_get_nodes_num(&result_list));
	if (presult_set->pparray == nullptr)
		return ecError;
	container_object_get_user_table_all_proptags(&proptags);
	for (pnode=single_list_get_head(&result_list); NULL!=pnode;
		pnode=single_list_get_after(&result_list, pnode)) {
		presult_set->pparray[presult_set->count] = cu_alloc<TPROPVAL_ARRAY>();
		if (NULL == presult_set->pparray[presult_set->count] ||
		    !ab_tree_fetch_node_properties(static_cast<SIMPLE_TREE_NODE *>(pnode->pdata),
		    &proptags, presult_set->pparray[presult_set->count])) {
			return ecError;
		}
		presult_set->count ++;
	}
	return ecSuccess;
}

uint32_t zarafa_server_getpermissions(GUID hsession,
	uint32_t hobject, PERMISSION_SET *pperm_set)
{
	uint8_t mapi_type;
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto pobject = pinfo->ptree->get_object<void>(hobject, &mapi_type);
	if (NULL == pobject) {
		pperm_set->count = 0;
		return ecNullObject;
	}
	switch (mapi_type) {
	case ZMG_STORE:
		if (!static_cast<STORE_OBJECT *>(pobject)->get_permissions(pperm_set))
			return ecError;
		break;
	case ZMG_FOLDER:
		if (!static_cast<FOLDER_OBJECT *>(pobject)->get_permissions(pperm_set))
			return ecError;
		break;
	default:
		return ecNotSupported;
	}
	return ecSuccess;
}

uint32_t zarafa_server_modifypermissions(GUID hsession,
	uint32_t hfolder, const PERMISSION_SET *pset)
{
	uint8_t mapi_type;
	
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto pfolder = pinfo->ptree->get_object<FOLDER_OBJECT>(hfolder, &mapi_type);
	if (pfolder == nullptr)
		return ecNullObject;
	if (mapi_type != ZMG_FOLDER)
		return ecNotSupported;
	return pfolder->set_permissions(pset) ? ecSuccess : ecError;
}

uint32_t zarafa_server_modifyrules(GUID hsession,
	uint32_t hfolder, uint32_t flags, const RULE_LIST *plist)
{
	int i;
	uint8_t mapi_type;
	
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto pfolder = pinfo->ptree->get_object<FOLDER_OBJECT>(hfolder, &mapi_type);
	if (pfolder == nullptr)
		return ecNullObject;
	if (mapi_type != ZMG_FOLDER)
		return ecNotSupported;
	if (MODIFY_RULES_FLAG_REPLACE & flags) {
		for (i=0; i<plist->count; i++) {
			if (plist->prule[i].flags != RULE_DATA_FLAG_ADD_ROW) {
				return ecInvalidParam;
			}
		}
	}
	return pfolder->updaterules(flags, plist) ? ecSuccess : ecError;
}

uint32_t zarafa_server_getabgal(GUID hsession, BINARY *pentryid)
{
	void *pvalue;
	
	if (!container_object_fetch_special_property(SPECIAL_CONTAINER_GAL,
	    PR_ENTRYID, &pvalue))
		return ecError;
	if (pvalue == nullptr)
		return ecNotFound;
	pentryid->cb = ((BINARY*)pvalue)->cb;
	pentryid->pb = ((BINARY*)pvalue)->pb;
	return ecSuccess;
}

uint32_t zarafa_server_loadstoretable(
	GUID hsession, uint32_t *phobject)
{
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto ptable = table_object_create(nullptr, nullptr, STORE_TABLE, 0);
	if (ptable == nullptr)
		return ecError;
	*phobject = pinfo->ptree->add_object_handle(ROOT_HANDLE, ZMG_TABLE, ptable.get());
	if (*phobject == INVALID_HANDLE)
		return ecError;
	ptable.release();
	return ecSuccess;
}

uint32_t zarafa_server_openstore(GUID hsession,
	BINARY entryid, uint32_t *phobject)
{
	int user_id;
	char dir[256];
	EXT_PULL ext_pull;
	char username[UADDR_SIZE];
	uint8_t provider_uid[16];
	STORE_ENTRYID store_entryid = {};
	
	ext_pull.init(entryid.pb, entryid.cb, common_util_alloc, EXT_FLAG_UTF16);
	if (ext_pull.g_store_eid(&store_entryid) != EXT_ERR_SUCCESS)
		return ecError;
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	rop_util_get_provider_uid(
		PROVIDER_UID_WRAPPED_PUBLIC, provider_uid);
	if (0 == memcmp(store_entryid.wrapped_provider_uid,
		provider_uid, 16)) {
		*phobject = pinfo->ptree->get_store_handle(false, pinfo->domain_id);
	} else {
		if (FALSE == common_util_essdn_to_uid(
			store_entryid.pmailbox_dn, &user_id)) {
			return ecNotFound;
		}
		if (pinfo->user_id != user_id) {
			if (!system_services_get_username_from_id(user_id,
			    username, GX_ARRAY_SIZE(username)) ||
				FALSE == system_services_get_maildir(
				username, dir)) {
				return ecError;
			}
			uint32_t permission = rightsNone;
			if (!exmdb_client::check_mailbox_permission(dir,
			    pinfo->get_username(), &permission))
				return ecError;
			if (permission == rightsNone)
				return ecLoginPerm;
			if (permission & frightsGromoxStoreOwner) try {
				std::lock_guard lk(pinfo->eowner_lock);
				pinfo->extra_owner.insert_or_assign(user_id, time(nullptr));
			} catch (const std::bad_alloc &) {
			}
		}
		*phobject = pinfo->ptree->get_store_handle(TRUE, user_id);
	}
	return *phobject != INVALID_HANDLE ? ecSuccess : ecError;
}

uint32_t zarafa_server_openpropfilesec(GUID hsession,
	const FLATUID *puid, uint32_t *phobject)
{
	GUID guid;
	BINARY bin;
	
	bin.cb = 16;
	bin.pv = deconst(puid);
	guid = rop_util_binary_to_guid(&bin);
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto ppropvals = pinfo->ptree->get_profile_sec(guid);
	if (ppropvals == nullptr)
		return ecNotFound;
	*phobject = pinfo->ptree->add_object_handle(ROOT_HANDLE, ZMG_PROFPROPERTY, ppropvals);
	return ecSuccess;
}

uint32_t zarafa_server_loadhierarchytable(GUID hsession,
	uint32_t hfolder, uint32_t flags, uint32_t *phobject)
{
	uint8_t mapi_type;
	STORE_OBJECT *pstore;
	std::unique_ptr<TABLE_OBJECT> ptable;
	
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto pobject = pinfo->ptree->get_object<void>(hfolder, &mapi_type);
	if (pobject == nullptr)
		return ecNullObject;
	switch (mapi_type) {
	case ZMG_FOLDER:
		pstore = static_cast<FOLDER_OBJECT *>(pobject)->pstore;
		ptable = table_object_create(pstore,
			pobject, HIERARCHY_TABLE, flags);
		break;
	case ZMG_ABCONT:
		ptable = table_object_create(NULL,
			pobject, CONTAINER_TABLE, flags);
		break;
	default:
		return ecNotSupported;
	}
	if (ptable == nullptr)
		return ecError;
	*phobject = pinfo->ptree->add_object_handle(hfolder, ZMG_TABLE, ptable.get());
	if (*phobject == INVALID_HANDLE)
		return ecError;
	ptable.release();
	return ecSuccess;
}

uint32_t zarafa_server_loadcontenttable(GUID hsession,
	uint32_t hfolder, uint32_t flags, uint32_t *phobject)
{
	uint8_t mapi_type;
	uint32_t permission;
	STORE_OBJECT *pstore;
	std::unique_ptr<TABLE_OBJECT> ptable;
	
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto pobject = pinfo->ptree->get_object<void>(hfolder, &mapi_type);
	if (pobject == nullptr)
		return ecNullObject;
	switch (mapi_type) {
	case ZMG_FOLDER: {
		auto folder = static_cast<FOLDER_OBJECT *>(pobject);
		pstore = folder->pstore;
		if (!pstore->check_owner_mode()) {
			if (!exmdb_client::check_folder_permission(pstore->get_dir(),
			    folder->folder_id, pinfo->get_username(), &permission))
				return ecNotFound;
			if (!(permission & (frightsReadAny | frightsOwner)))
				return ecNotFound;
		}
		ptable = table_object_create(folder->pstore,
		         pobject, CONTENT_TABLE, flags);
		break;
	}
	case ZMG_ABCONT:
		ptable = table_object_create(NULL,
				pobject, USER_TABLE, 0);
		break;
	default:
		return ecNotSupported;
	}
	if (ptable == nullptr)
		return ecError;
	*phobject = pinfo->ptree->add_object_handle(hfolder, ZMG_TABLE, ptable.get());
	if (*phobject == INVALID_HANDLE)
		return ecError;
	ptable.release();
	return ecSuccess;
}

uint32_t zarafa_server_loadrecipienttable(GUID hsession,
	uint32_t hmessage, uint32_t *phobject)
{
	uint8_t mapi_type;
	
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto pmessage = pinfo->ptree->get_object<MESSAGE_OBJECT>(hmessage, &mapi_type);
	if (pmessage == nullptr)
		return ecNullObject;
	if (mapi_type != ZMG_MESSAGE)
		return ecNotSupported;
	auto ptable = table_object_create(pmessage->get_store(),
	              pmessage, RECIPIENT_TABLE, 0);
	if (ptable == nullptr)
		return ecError;
	*phobject = pinfo->ptree->add_object_handle(hmessage, ZMG_TABLE, ptable.get());
	if (*phobject == INVALID_HANDLE)
		return ecError;
	ptable.release();
	return ecSuccess;
}

uint32_t zarafa_server_loadruletable(GUID hsession,
	uint32_t hfolder, uint32_t *phobject)
{
	uint8_t mapi_type;
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto pfolder = pinfo->ptree->get_object<FOLDER_OBJECT>(hfolder, &mapi_type);
	if (pfolder == nullptr)
		return ecNullObject;
	if (mapi_type != ZMG_FOLDER)
		return ecNotSupported;
	auto folder_id = pfolder->folder_id;
	auto ptable = table_object_create(pfolder->pstore, &folder_id, RULE_TABLE, 0);
	if (ptable == nullptr)
		return ecError;
	*phobject = pinfo->ptree->add_object_handle(hfolder, ZMG_TABLE, ptable.get());
	if (*phobject == INVALID_HANDLE)
		return ecError;
	ptable.release();
	return ecSuccess;
}

uint32_t zarafa_server_createmessage(GUID hsession,
	uint32_t hfolder, uint32_t flags, uint32_t *phobject)
{
	void *pvalue;
	uint8_t mapi_type;
	uint32_t tag_access;
	uint32_t permission;
	uint64_t message_id;
	uint32_t proptag_buff[4];
	PROPTAG_ARRAY tmp_proptags;
	TPROPVAL_ARRAY tmp_propvals;
	
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto pfolder = pinfo->ptree->get_object<FOLDER_OBJECT>(hfolder, &mapi_type);
	if (pfolder == nullptr)
		return ecNullObject;
	if (mapi_type != ZMG_FOLDER)
		return ecNotSupported;
	auto folder_id = pfolder->folder_id;
	auto pstore = pfolder->pstore;
	auto hstore = pinfo->ptree->get_store_handle(pstore->b_private, pstore->account_id);
	if (hstore == INVALID_HANDLE)
		return ecNullObject;
	if (!pstore->check_owner_mode()) {
		if (!exmdb_client::check_folder_permission(pstore->get_dir(),
		    pfolder->folder_id, pinfo->get_username(), &permission))
			return ecError;
		if (!(permission & (frightsOwner | frightsCreate)))
			return ecNotFound;
		tag_access = TAG_ACCESS_MODIFY|TAG_ACCESS_READ;
		if (permission & (frightsDeleteOwned | frightsDeleteAny))
			tag_access |= TAG_ACCESS_DELETE;
	} else {
		tag_access = TAG_ACCESS_MODIFY|
			TAG_ACCESS_READ|TAG_ACCESS_DELETE;
	}
	tmp_proptags.count = 4;
	tmp_proptags.pproptag = proptag_buff;
	proptag_buff[0] = PR_MESSAGE_SIZE_EXTENDED;
	proptag_buff[1] = PROP_TAG_STORAGEQUOTALIMIT;
	proptag_buff[2] = PROP_TAG_ASSOCIATEDCONTENTCOUNT;
	proptag_buff[3] = PROP_TAG_CONTENTCOUNT;
	if (!pstore->get_properties(&tmp_proptags, &tmp_propvals))
		return ecError;
	pvalue = common_util_get_propvals(&tmp_propvals, PROP_TAG_STORAGEQUOTALIMIT);
	int64_t max_quota = pvalue == nullptr ? -1 : static_cast<int64_t>(*static_cast<uint32_t *>(pvalue)) * 1024;
	pvalue = common_util_get_propvals(&tmp_propvals, PR_MESSAGE_SIZE_EXTENDED);
	uint64_t total_size = pvalue == nullptr ? 0 : *static_cast<uint64_t *>(pvalue);
	if (max_quota > 0 && total_size > static_cast<uint64_t>(max_quota)) {
		return ecQuotaExceeded;
	}
	pvalue = common_util_get_propvals(&tmp_propvals,
					PROP_TAG_ASSOCIATEDCONTENTCOUNT);
	uint32_t total_mail = pvalue != nullptr ? *static_cast<uint32_t *>(pvalue) : 0;
	pvalue = common_util_get_propvals(&tmp_propvals,
							PROP_TAG_CONTENTCOUNT);
	if (pvalue != nullptr)
		total_mail += *(uint32_t*)pvalue;
	if (total_mail > common_util_get_param(
		COMMON_UTIL_MAX_MESSAGE)) {
		return ecQuotaExceeded;
	}
	if (!exmdb_client::allocate_message_id(pstore->get_dir(),
	    folder_id, &message_id))
		return ecError;
	auto pmessage = message_object_create(pstore, TRUE,
			pinfo->cpid, message_id, &folder_id,
			tag_access, TRUE, NULL);
	if (pmessage == nullptr)
		return ecError;
	BOOL b_fai = (flags & FLAG_ASSOCIATED) ? TRUE : false;
	if (!pmessage->init_message(b_fai, pinfo->cpid))
		return ecError;
	/* add the store handle as the parent object handle
		because the caller normaly will not keep the
		handle of folder */
	*phobject = pinfo->ptree->add_object_handle(hstore, ZMG_MESSAGE, pmessage.get());
	if (*phobject == INVALID_HANDLE)
		return ecError;
	pmessage.release();
	return ecSuccess;
}

uint32_t zarafa_server_deletemessages(GUID hsession,
	uint32_t hfolder, const BINARY_ARRAY *pentryids,
	uint32_t flags)
{
	BOOL b_owner;
	void *pvalue;
	EID_ARRAY ids;
	EID_ARRAY ids1;
	int account_id;
	BOOL b_private;
	BOOL b_partial;
	uint8_t mapi_type;
	uint64_t folder_id;
	uint32_t permission;
	uint64_t message_id;
	MESSAGE_CONTENT *pbrief;
	uint32_t proptag_buff[2];
	PROPTAG_ARRAY tmp_proptags;
	TPROPVAL_ARRAY tmp_propvals;
	BOOL notify_non_read = FALSE; /* TODO: Read from config or USER_INFO. */
	
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return FALSE;
	auto pfolder = pinfo->ptree->get_object<FOLDER_OBJECT>(hfolder, &mapi_type);
	if (pfolder == nullptr)
		return ecNullObject;
	if (mapi_type != ZMG_FOLDER)
		return ecNotSupported;
	auto pstore = pfolder->pstore;
	const char *username = nullptr;
	if (!pstore->check_owner_mode()) {
		if (!exmdb_client::check_folder_permission(pstore->get_dir(),
		    pfolder->folder_id, pinfo->get_username(), &permission))
			return ecError;
		if (permission & (frightsDeleteAny | frightsOwner))
			username = NULL;
		else if (permission & frightsDeleteOwned)
			username = pinfo->get_username();
		else
			return ecNotFound;
	}
	ids.count = 0;
	ids.pids = cu_alloc<uint64_t>(pentryids->count);
	if (ids.pids == nullptr)
		return ecError;
	for (size_t i = 0; i < pentryids->count; ++i) {
		if (FALSE == common_util_from_message_entryid(
			pentryids->pbin[i], &b_private, &account_id,
			&folder_id, &message_id)) {
			return ecError;
		}
		if (b_private != pstore->b_private ||
		    account_id != pstore->account_id ||
		    folder_id != pfolder->folder_id)
			continue;
		ids.pids[ids.count] = message_id;
		ids.count ++;
	}
	BOOL b_hard = (flags & FLAG_HARD_DELETE) ? false : TRUE; /* XXX */
	if (FALSE == notify_non_read) {
		if (!exmdb_client::delete_messages(pstore->get_dir(),
		    pstore->account_id, pinfo->cpid, username,
		    pfolder->folder_id, &ids, b_hard, &b_partial))
			return ecError;
		return ecSuccess;
	}
	ids1.count = 0;
	ids1.pids  = cu_alloc<uint64_t>(ids.count);
	if (ids1.pids == nullptr)
		return ecError;
	for (size_t i = 0; i < ids.count; ++i) {
		if (NULL != username) {
			if (!exmdb_client_check_message_owner(pstore->get_dir(),
			    ids.pids[i], username, &b_owner))
				return ecError;
			if (!b_owner)
				continue;
		}
		tmp_proptags.count = 2;
		tmp_proptags.pproptag = proptag_buff;
		proptag_buff[0] = PROP_TAG_NONRECEIPTNOTIFICATIONREQUESTED;
		proptag_buff[1] = PR_READ;
		if (!exmdb_client::get_message_properties(pstore->get_dir(),
		    nullptr, 0, ids.pids[i], &tmp_proptags, &tmp_propvals))
			return ecError;
		pbrief = NULL;
		pvalue = common_util_get_propvals(&tmp_propvals,
				PROP_TAG_NONRECEIPTNOTIFICATIONREQUESTED);
		if (NULL != pvalue && 0 != *(uint8_t*)pvalue) {
			pvalue = common_util_get_propvals(&tmp_propvals, PR_READ);
			if ((pvalue == nullptr || *static_cast<uint8_t *>(pvalue) == 0) &&
			    !exmdb_client::get_message_brief(pstore->get_dir(),
			    pinfo->cpid, ids.pids[i], &pbrief))
				return ecError;
		}
		ids1.pids[ids1.count] = ids.pids[i];
		ids1.count ++;
		if (pbrief != nullptr)
			common_util_notify_receipt(pstore->get_account(),
				NOTIFY_RECEIPT_NON_READ, pbrief);
	}
	return exmdb_client::delete_messages(pstore->get_dir(),
	       pstore->account_id, pinfo->cpid, username,
	       pfolder->folder_id, &ids1, b_hard, &b_partial) ?
	       ecSuccess : ecError;
}

uint32_t zarafa_server_copymessages(GUID hsession,
	uint32_t hsrcfolder, uint32_t hdstfolder,
	const BINARY_ARRAY *pentryids, uint32_t flags)
{
	BOOL b_done, b_guest = TRUE, b_owner;
	EID_ARRAY ids;
	BOOL b_partial;
	BOOL b_private;
	int account_id;
	uint8_t mapi_type;
	uint64_t folder_id;
	uint64_t message_id;
	uint32_t permission;
	
	if (0 == pentryids->count) {
		return ecSuccess;
	}
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto psrc_folder = pinfo->ptree->get_object<FOLDER_OBJECT>(hsrcfolder, &mapi_type);
	if (psrc_folder == nullptr)
		return ecNullObject;
	if (mapi_type != ZMG_FOLDER)
		return ecNotSupported;
	auto pstore = psrc_folder->pstore;
	auto pdst_folder = pinfo->ptree->get_object<FOLDER_OBJECT>(hdstfolder, &mapi_type);
	if (pdst_folder == nullptr)
		return ecNullObject;
	if (mapi_type != ZMG_FOLDER || pdst_folder->type == FOLDER_TYPE_SEARCH)
		return ecNotSupported;
	auto pstore1 = pdst_folder->pstore;
	BOOL b_copy = (flags & FLAG_MOVE) ? false : TRUE;
	if (pstore != pstore1) {
		if (FALSE == b_copy) {
			b_guest = FALSE;
			if (!pstore->check_owner_mode()) {
				if (!exmdb_client::check_folder_permission(pstore->get_dir(),
				    psrc_folder->folder_id, pinfo->get_username(), &permission))
					return ecError;
				if (permission & frightsDeleteAny)
					/* permission to delete any message */;
				else if (permission & frightsDeleteOwned)
					b_guest = TRUE;
				else
					return ecAccessDenied;
			}
		}
		if (!pstore1->check_owner_mode()) {
			if (!exmdb_client::check_folder_permission(pstore1->get_dir(),
			    pdst_folder->folder_id, pinfo->get_username(), &permission))
				return ecError;
			if (!(permission & frightsCreate))
				return ecAccessDenied;
		}
		for (size_t i = 0; i < pentryids->count; ++i) {
			if (FALSE == common_util_from_message_entryid(
				pentryids->pbin[i], &b_private, &account_id,
				&folder_id, &message_id)) {
				return ecError;
			}
			if (b_private != pstore->b_private ||
			    account_id != pstore->account_id ||
			    folder_id != psrc_folder->folder_id)
				continue;
			gxerr_t err = common_util_remote_copy_message(pstore,
			              message_id, pstore1, pdst_folder->folder_id);
			if (err != GXERR_SUCCESS) {
				return gxerr_to_hresult(err);
			}
			if (FALSE == b_copy) {
				if (TRUE == b_guest) {
					if (!exmdb_client_check_message_owner(pstore->get_dir(),
					    message_id, pinfo->get_username(), &b_owner))
						return ecError;
					if (!b_owner)
						continue;
				}
				if (!exmdb_client_delete_message(pstore->get_dir(),
				    pstore->account_id, pinfo->cpid,
				    psrc_folder->folder_id, message_id, false, &b_done))
					return ecError;
			}
		}
		return ecSuccess;
	}
	ids.count = 0;
	ids.pids = cu_alloc<uint64_t>(pentryids->count);
	if (ids.pids == nullptr)
		return ecError;
	for (size_t i = 0; i < pentryids->count; ++i) {
		if (FALSE == common_util_from_message_entryid(
			pentryids->pbin[i], &b_private, &account_id,
			&folder_id, &message_id)) {
			return ecError;
		}
		if (b_private != pstore->b_private ||
		    account_id != pstore->account_id ||
		    folder_id != psrc_folder->folder_id)
			continue;
		ids.pids[ids.count] = message_id;
		ids.count ++;
	}
	if (!pstore->check_owner_mode()) {
		if (!exmdb_client::check_folder_permission(pstore->get_dir(),
		    pdst_folder->folder_id, pinfo->get_username(), &permission))
			return ecError;
		if (!(permission & frightsCreate))
			return ecAccessDenied;
		b_guest = TRUE;
	} else {
		b_guest = FALSE;
	}
	return exmdb_client::movecopy_messages(pstore->get_dir(),
	       pstore->account_id, pinfo->cpid, b_guest,
	       pinfo->get_username(), psrc_folder->folder_id,
	       pdst_folder->folder_id, b_copy, &ids, &b_partial) ?
	       ecSuccess : ecError;
}

uint32_t zarafa_server_setreadflags(GUID hsession,
	uint32_t hfolder, const BINARY_ARRAY *pentryids,
	uint32_t flags)
{
	void *pvalue;
	BOOL b_private;
	BOOL b_changed;
	int account_id;
	uint64_t read_cn;
	uint8_t tmp_byte;
	uint32_t table_id;
	uint8_t mapi_type;
	uint32_t row_count;
	uint64_t folder_id;
	TARRAY_SET tmp_set;
	uint64_t message_id;
	uint32_t tmp_proptag;
	BOOL b_notify = TRUE; /* TODO: Read from config or USER_INFO. */
	BINARY_ARRAY tmp_bins;
	PROPTAG_ARRAY proptags;
	PROBLEM_ARRAY problems;
	MESSAGE_CONTENT *pbrief;
	TPROPVAL_ARRAY propvals;
	RESTRICTION restriction;
	RESTRICTION_PROPERTY res_prop;
	static constexpr uint8_t fake_false = false;
	TAGGED_PROPVAL propval_buff[2];
	
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto pfolder = pinfo->ptree->get_object<FOLDER_OBJECT>(hfolder, &mapi_type);
	if (pfolder == nullptr)
		return ecNullObject;
	if (mapi_type != ZMG_FOLDER)
		return ecNotSupported;
	auto pstore = pfolder->pstore;
	auto username = pstore->check_owner_mode() ? nullptr : pinfo->get_username();
	if (0 == pentryids->count) {
		restriction.rt = RES_PROPERTY;
		restriction.pres = &res_prop;
		res_prop.relop = flags == FLAG_CLEAR_READ ? RELOP_NE : RELOP_EQ;
		res_prop.proptag = PR_READ;
		res_prop.propval.proptag = PR_READ;
		res_prop.propval.pvalue = deconst(&fake_false);
		if (!exmdb_client::load_content_table(pstore->get_dir(), 0,
		    pfolder->folder_id, username, TABLE_FLAG_NONOTIFICATIONS,
		    &restriction, nullptr, &table_id, &row_count))
			return ecError;
		proptags.count = 1;
		proptags.pproptag = &tmp_proptag;
		tmp_proptag = PR_ENTRYID;
		if (!exmdb_client::query_table(pstore->get_dir(), username,
		    0, table_id, &proptags, 0, row_count, &tmp_set)) {
			exmdb_client::unload_table(pstore->get_dir(), table_id);
			return ecError;
		}
		exmdb_client::unload_table(pstore->get_dir(), table_id);
		if (tmp_set.count > 0) {
			tmp_bins.count = 0;
			tmp_bins.pbin = cu_alloc<BINARY>(tmp_set.count);
			if (tmp_bins.pbin == nullptr)
				return ecError;
			for (size_t i = 0; i < tmp_set.count; ++i) {
				if (1 != tmp_set.pparray[i]->count) {
					continue;
				}
				tmp_bins.pbin[tmp_bins.count] =
					*(BINARY*)tmp_set.pparray[i]->ppropval[0].pvalue;
				tmp_bins.count ++;
			}
			pentryids = &tmp_bins;
		}
	}
	for (size_t i = 0; i < pentryids->count; ++i) {
		if (FALSE == common_util_from_message_entryid(
			pentryids->pbin[i], &b_private, &account_id,
			&folder_id, &message_id)) {
			return ecError;
		}
		if (b_private != pstore->b_private ||
		    account_id != pstore->account_id ||
		    folder_id != pfolder->folder_id)
			continue;
		b_notify = FALSE;
		b_changed = FALSE;
		if (FLAG_CLEAR_READ == flags) {
			if (!exmdb_client_get_message_property(pstore->get_dir(),
			    username, 0, message_id, PR_READ, &pvalue))
				return ecError;
			if (NULL != pvalue && 0 != *(uint8_t*)pvalue) {
				tmp_byte = 0;
				b_changed = TRUE;
			}
		} else {
			if (!exmdb_client_get_message_property(pstore->get_dir(),
			    username, 0, message_id, PR_READ, &pvalue))
				return ecError;
			if (NULL == pvalue || 0 == *(uint8_t*)pvalue) {
				tmp_byte = 1;
				b_changed = TRUE;
				if (!exmdb_client_get_message_property(pstore->get_dir(),
				    username, 0, message_id,
				    PROP_TAG_READRECEIPTREQUESTED, &pvalue))
					return ecError;
				if (pvalue != nullptr && *static_cast<uint8_t *>(pvalue) != 0)
					b_notify = TRUE;
			}
		}
		if (b_changed && !exmdb_client::set_message_read_state(pstore->get_dir(),
		    username, message_id, tmp_byte, &read_cn))
			return ecError;
		if (TRUE == b_notify) {
			if (!exmdb_client::get_message_brief(pstore->get_dir(),
			    pinfo->cpid, message_id, &pbrief))
				return ecError;
			if (pbrief != nullptr)
				common_util_notify_receipt(pstore->get_account(),
					NOTIFY_RECEIPT_READ, pbrief);
			propvals.count = 2;
			propvals.ppropval = propval_buff;
			propval_buff[0].proptag =
				PROP_TAG_READRECEIPTREQUESTED;
			propval_buff[0].pvalue = deconst(&fake_false);
			propval_buff[1].proptag =
				PROP_TAG_NONRECEIPTNOTIFICATIONREQUESTED;
			propval_buff[1].pvalue = deconst(&fake_false);
			exmdb_client::set_message_properties(pstore->get_dir(), username,
				0, message_id, &propvals, &problems);
		}
	}
	return ecSuccess;
}

uint32_t zarafa_server_createfolder(GUID hsession,
	uint32_t hparent_folder, uint32_t folder_type,
	const char *folder_name, const char *folder_comment,
	uint32_t flags, uint32_t *phobject)
{
	XID tmp_xid;
	void *pvalue;
	uint64_t tmp_id;
	uint32_t tmp_type;
	uint8_t mapi_type;
	uint64_t last_time;
	uint64_t parent_id;
	uint64_t folder_id;
	uint64_t change_num;
	uint32_t tag_access;
	uint32_t permission;
	TPROPVAL_ARRAY tmp_propvals;
	PERMISSION_DATA permission_row;
	TAGGED_PROPVAL propval_buff[10];
	
	if (FOLDER_TYPE_SEARCH != folder_type &&
		FOLDER_TYPE_GENERIC != folder_type) {
		return ecNotSupported;
	}
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto pparent = pinfo->ptree->get_object<FOLDER_OBJECT>(hparent_folder, &mapi_type);
	if (pparent == nullptr)
		return ecNullObject;
	if (mapi_type != ZMG_FOLDER)
		return ecNotSupported;
	if (rop_util_get_replid(pparent->folder_id) != 1 ||
	    pparent->type == FOLDER_TYPE_SEARCH)
		return ecNotSupported;
	auto pstore = pparent->pstore;
	if (!pstore->b_private && folder_type == FOLDER_TYPE_SEARCH)
		return ecNotSupported;
	if (!pstore->check_owner_mode()) {
		if (!exmdb_client::check_folder_permission(pstore->get_dir(),
		    pparent->folder_id, pinfo->get_username(), &permission))
			return ecError;
		if (!(permission & (frightsOwner | frightsCreateSubfolder)))
			return ecAccessDenied;
	}
	if (!exmdb_client::get_folder_by_name(pstore->get_dir(),
	    pparent->folder_id, folder_name, &folder_id))
		return ecError;
	if (0 != folder_id) {
		if (!exmdb_client_get_folder_property(pstore->get_dir(), 0,
		    folder_id, PROP_TAG_FOLDERTYPE, &pvalue) ||
		    pvalue == nullptr)
			return ecError;
		if (0 == (flags & FLAG_OPEN_IF_EXISTS) ||
			folder_type != *(uint32_t*)pvalue) {
			return ecDuplicateName;
		}
	} else {
		parent_id = pparent->folder_id;
		if (!exmdb_client::allocate_cn(pstore->get_dir(), &change_num))
			return ecError;
		tmp_type = folder_type;
		last_time = rop_util_current_nttime();
		tmp_propvals.count = 9;
		tmp_propvals.ppropval = propval_buff;
		propval_buff[0].proptag = PROP_TAG_PARENTFOLDERID;
		propval_buff[0].pvalue = &parent_id;
		propval_buff[1].proptag = PROP_TAG_FOLDERTYPE;
		propval_buff[1].pvalue = &tmp_type;
		propval_buff[2].proptag = PR_DISPLAY_NAME;
		propval_buff[2].pvalue = deconst(folder_name);
		propval_buff[3].proptag = PROP_TAG_COMMENT;
		propval_buff[3].pvalue = deconst(folder_comment);
		propval_buff[4].proptag = PR_CREATION_TIME;
		propval_buff[4].pvalue = &last_time;
		propval_buff[5].proptag = PR_LAST_MODIFICATION_TIME;
		propval_buff[5].pvalue = &last_time;
		propval_buff[6].proptag = PROP_TAG_CHANGENUMBER;
		propval_buff[6].pvalue = &change_num;
		tmp_xid.guid = pstore->guid();
		rop_util_get_gc_array(change_num, tmp_xid.local_id);
		propval_buff[7].proptag = PR_CHANGE_KEY;
		propval_buff[7].pvalue = common_util_xid_to_binary(22, &tmp_xid);
		if (propval_buff[7].pvalue == nullptr)
			return ecError;
		propval_buff[8].proptag = PR_PREDECESSOR_CHANGE_LIST;
		propval_buff[8].pvalue = common_util_pcl_append(nullptr, static_cast<BINARY *>(propval_buff[7].pvalue));
		if (propval_buff[8].pvalue == nullptr)
			return ecError;
		if (!exmdb_client::create_folder_by_properties(pstore->get_dir(),
		    pinfo->cpid, &tmp_propvals, &folder_id) || folder_id == 0)
			return ecError;
		if (!pstore->check_owner_mode()) {
			auto pentryid = common_util_username_to_addressbook_entryid(pinfo->get_username());
			if (pentryid == nullptr)
				return ecError;
			tmp_id = 1;
			permission = rightsGromox7;
			permission_row.flags = PERMISSION_DATA_FLAG_ADD_ROW;
			permission_row.propvals.count = 3;
			permission_row.propvals.ppropval = propval_buff;
			propval_buff[0].proptag = PR_ENTRYID;
			propval_buff[0].pvalue = pentryid;
			propval_buff[1].proptag = PROP_TAG_MEMBERID;
			propval_buff[1].pvalue = &tmp_id;
			propval_buff[2].proptag = PROP_TAG_MEMBERRIGHTS;
			propval_buff[2].pvalue = &permission;
			if (!exmdb_client::update_folder_permission(pstore->get_dir(),
			    folder_id, false, 1, &permission_row))
				return ecError;
		}
	}
	tag_access = TAG_ACCESS_MODIFY | TAG_ACCESS_READ |
				TAG_ACCESS_DELETE | TAG_ACCESS_HIERARCHY |
				TAG_ACCESS_CONTENTS | TAG_ACCESS_FAI_CONTENTS;
	auto pfolder = folder_object_create(pstore,
		folder_id, folder_type, tag_access);
	if (pfolder == nullptr)
		return ecError;
	if (FOLDER_TYPE_SEARCH == folder_type) {
		/* add the store handle as the parent object handle
			because the caller normaly will not keep the
			handle of parent folder */
		auto hstore = pinfo->ptree->get_store_handle(TRUE, pstore->account_id);
		if (hstore == INVALID_HANDLE)
			return ecError;
		*phobject = pinfo->ptree->add_object_handle(hstore, ZMG_FOLDER, pfolder.get());
	} else {
		*phobject = pinfo->ptree->add_object_handle(hparent_folder, ZMG_FOLDER, pfolder.get());
	}
	if (*phobject == INVALID_HANDLE)
		return ecError;
	pfolder.release();
	return ecSuccess;
}

uint32_t zarafa_server_deletefolder(GUID hsession,
	uint32_t hparent_folder, BINARY entryid, uint32_t flags)
{
	BOOL b_done;
	void *pvalue;
	BOOL b_exist;
	BOOL b_partial;
	BOOL b_private;
	int account_id;
	uint8_t mapi_type;
	uint64_t folder_id;
	uint32_t permission;
	
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto pfolder = pinfo->ptree->get_object<FOLDER_OBJECT>(hparent_folder, &mapi_type);
	if (pfolder == nullptr)
		return ecNullObject;
	if (mapi_type != ZMG_FOLDER)
		return ecNotSupported;
	auto pstore = pfolder->pstore;
	if (FALSE == common_util_from_folder_entryid(
		entryid, &b_private, &account_id, &folder_id)) {
		return ecError;
	}
	if (b_private != pstore->b_private || account_id != pstore->account_id)
		return ecInvalidParam;
	if (pstore->b_private) {
		if (rop_util_get_gc_value(folder_id) < PRIVATE_FID_CUSTOM) {
			return ecAccessDenied;
		}
	} else {
		if (1 == rop_util_get_replid(folder_id) &&
			rop_util_get_gc_value(folder_id) < PUBLIC_FID_CUSTOM) {
			return ecAccessDenied;
		}
	}
	const char *username = nullptr;
	if (!pstore->check_owner_mode()) {
		if (!exmdb_client::check_folder_permission(pstore->get_dir(),
		    pfolder->folder_id, pinfo->get_username(), &permission))
			return ecError;
		if (!(permission & frightsOwner))
			return ecAccessDenied;
		username = pinfo->get_username();
	}
	if (!exmdb_client::check_folder_id(pstore->get_dir(),
	    pfolder->folder_id, &b_exist))
		return ecError;
	if (!b_exist)
		return ecSuccess;
	BOOL b_normal = (flags & DEL_MESSAGES) ? TRUE : false;
	BOOL b_fai = b_normal;
	BOOL b_sub = (flags & DEL_FOLDERS) ? TRUE : false;
	BOOL b_hard = (flags & DELETE_HARD_DELETE) ? TRUE : false;
	if (pstore->b_private) {
		if (!exmdb_client_get_folder_property(pstore->get_dir(), 0,
		    folder_id, PROP_TAG_FOLDERTYPE, &pvalue))
			return ecError;
		if (pvalue == nullptr)
			return ecSuccess;
		if (FOLDER_TYPE_SEARCH == *(uint32_t*)pvalue) {
			goto DELETE_FOLDER;
		}
	}
	if (TRUE == b_sub || TRUE == b_normal || TRUE == b_fai) {
		if (!exmdb_client::empty_folder(pstore->get_dir(), pinfo->cpid,
		    username, folder_id, b_hard, b_normal, b_fai, b_sub, &b_partial))
			return ecError;
		if (b_partial)
			/* failure occurs, stop deleting folder */
			return ecSuccess;
	}
 DELETE_FOLDER:
	return exmdb_client::delete_folder(pstore->get_dir(),
	       pinfo->cpid, folder_id, b_hard, &b_done) ? ecSuccess : ecError;
}

uint32_t zarafa_server_emptyfolder(GUID hsession,
	uint32_t hfolder, uint32_t flags)
{
	BOOL b_partial;
	uint8_t mapi_type;
	uint32_t permission;
	
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto pfolder = pinfo->ptree->get_object<FOLDER_OBJECT>(hfolder, &mapi_type);
	if (pfolder == nullptr)
		return ecNullObject;
	if (mapi_type != ZMG_FOLDER)
		return ecNotSupported;
	auto pstore = pfolder->pstore;
	if (!pstore->b_private)
		return ecNotSupported;
	auto fid_val = rop_util_get_gc_value(pfolder->folder_id);
	if (PRIVATE_FID_ROOT == fid_val ||
		PRIVATE_FID_IPMSUBTREE == fid_val) {
		return ecAccessDenied;
	}
	const char *username = nullptr;
	if (!pstore->check_owner_mode()) {
		if (!exmdb_client::check_folder_permission(pstore->get_dir(),
		    pfolder->folder_id, pinfo->get_username(), &permission))
			return ecError;
		if (!(permission & (frightsDeleteAny | frightsDeleteOwned)))
			return ecAccessDenied;
		username = pinfo->get_username();
	}
	BOOL b_fai = (flags & FLAG_DEL_ASSOCIATED) ? TRUE : false;
	BOOL b_hard = (flags & FLAG_HARD_DELETE) ? TRUE : false;
	return exmdb_client::empty_folder(pstore->get_dir(),
	       pinfo->cpid, username, pfolder->folder_id,
	       b_hard, TRUE, b_fai, TRUE, &b_partial) ? ecSuccess : ecError;
}

uint32_t zarafa_server_copyfolder(GUID hsession,
	uint32_t hsrc_folder, BINARY entryid, uint32_t hdst_folder,
	const char *new_name, uint32_t flags)
{
	BOOL b_done;
	BOOL b_exist;
	BOOL b_cycle;
	BOOL b_private;
	BOOL b_partial;
	int account_id;
	uint8_t mapi_type;
	uint64_t folder_id;
	uint32_t permission;
	
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto psrc_parent = pinfo->ptree->get_object<FOLDER_OBJECT>(hsrc_folder, &mapi_type);
	if (psrc_parent == nullptr)
		return ecNullObject;
	BOOL b_copy = (flags & FLAG_MOVE) ? false : TRUE;
	if (psrc_parent->type == FOLDER_TYPE_SEARCH && !b_copy)
		return ecNotSupported;
	if (mapi_type != ZMG_FOLDER)
		return ecNotSupported;
	auto pstore = psrc_parent->pstore;
	if (FALSE == common_util_from_folder_entryid(
		entryid, &b_private, &account_id, &folder_id)) {
		return ecError;
	}
	if (b_private != pstore->b_private || account_id != pstore->account_id)
		return ecInvalidParam;
	auto pdst_folder = pinfo->ptree->get_object<FOLDER_OBJECT>(hdst_folder, &mapi_type);
	if (pdst_folder == nullptr)
		return ecNullObject;
	if (mapi_type != ZMG_FOLDER)
		return ecNotSupported;
	auto pstore1 = pdst_folder->pstore;
	if (pstore->b_private) {
		if (PRIVATE_FID_ROOT == rop_util_get_gc_value(folder_id)) {
			return ecAccessDenied;
		}
	} else {
		if (PUBLIC_FID_ROOT == rop_util_get_gc_value(folder_id)) {
			return ecAccessDenied;
		}
	}
	BOOL b_guest = false;
	const char *username = nullptr;
	if (!pstore->check_owner_mode()) {
		if (!exmdb_client::check_folder_permission(pstore->get_dir(),
		    folder_id, pinfo->get_username(), &permission))
			return ecError;
		if (!(permission & frightsReadAny))
			return ecAccessDenied;
		if (!exmdb_client::check_folder_permission(pstore->get_dir(),
		    pdst_folder->folder_id, pinfo->get_username(), &permission))
			return ecError;
		if (!(permission & (frightsOwner | frightsCreateSubfolder)))
			return ecAccessDenied;
		username = pinfo->get_username();
		b_guest = TRUE;
	}
	if (pstore != pstore1) {
		if (!b_copy && !pstore->check_owner_mode()) {
			if (!exmdb_client::check_folder_permission(pstore->get_dir(),
			    psrc_parent->folder_id, pinfo->get_username(), &permission))
				return ecError;
			if (!(permission & frightsOwner))
				return ecAccessDenied;
		}
		gxerr_t err = common_util_remote_copy_folder(pstore, folder_id,
		              pstore1, pdst_folder->folder_id, new_name);
		if (err != GXERR_SUCCESS) {
			return gxerr_to_hresult(err);
		}
		if (FALSE == b_copy) {
			if (!exmdb_client::empty_folder(pstore->get_dir(),
			    pinfo->cpid, username, folder_id, false, TRUE,
			    TRUE, TRUE, &b_partial))
				return ecError;
			if (b_partial)
				/* failure occurs, stop deleting folder */
				return ecSuccess;
			if (!exmdb_client::delete_folder(pstore->get_dir(),
			    pinfo->cpid, folder_id, false, &b_done))
				return ecError;
		}
		return ecSuccess;
	}
	if (!exmdb_client::check_folder_cycle(pstore->get_dir(), folder_id,
	    pdst_folder->folder_id, &b_cycle))
		return ecError;
	if (b_cycle)
		return MAPI_E_FOLDER_CYCLE;
	if (!exmdb_client::movecopy_folder(pstore->get_dir(),
	    pstore->account_id, pinfo->cpid, b_guest, pinfo->get_username(),
	    psrc_parent->folder_id, folder_id, pdst_folder->folder_id,
	    new_name, b_copy, &b_exist, &b_partial))
		return ecError;
	return b_exist ? ecDuplicateName : ecSuccess;
}

uint32_t zarafa_server_getstoreentryid(
	const char *mailbox_dn, BINARY *pentryid)
{
	EXT_PUSH ext_push;
	char username[UADDR_SIZE];
	char tmp_buff[1024];
	STORE_ENTRYID store_entryid = {};
	
	if (0 == strncasecmp(mailbox_dn, "/o=", 3)) {
		if (!common_util_essdn_to_username(mailbox_dn,
		    username, GX_ARRAY_SIZE(username)))
			return ecError;
	} else {
		gx_strlcpy(username, mailbox_dn, GX_ARRAY_SIZE(username));
		if (!common_util_username_to_essdn(username,
		    tmp_buff, GX_ARRAY_SIZE(tmp_buff)))
			return ecError;
		mailbox_dn = tmp_buff;
	}
	store_entryid.flags = 0;
	rop_util_get_provider_uid(PROVIDER_UID_STORE,
					store_entryid.provider_uid);
	store_entryid.version = 0;
	store_entryid.flag = 0;
	snprintf(store_entryid.dll_name, sizeof(store_entryid.dll_name), "emsmdb.dll");
	store_entryid.wrapped_flags = 0;
	rop_util_get_provider_uid(
		PROVIDER_UID_WRAPPED_PRIVATE,
		store_entryid.wrapped_provider_uid);
	store_entryid.wrapped_type = 0x0000000C;
	store_entryid.pserver_name = username;
	store_entryid.pmailbox_dn = deconst(mailbox_dn);
	pentryid->pv = common_util_alloc(1024);
	if (pentryid->pv == nullptr ||
	    !ext_push.init(pentryid->pb, 1024, EXT_FLAG_UTF16) ||
	    ext_push.p_store_eid(&store_entryid) != EXT_ERR_SUCCESS)
		return ecError;
	pentryid->cb = ext_push.m_offset;
	return ecSuccess;
}

uint32_t zarafa_server_entryidfromsourcekey(
	GUID hsession, uint32_t hstore, BINARY folder_key,
	const BINARY *pmessage_key, BINARY *pentryid)
{
	XID tmp_xid;
	BOOL b_found;
	BINARY *pbin;
	int domain_id;
	uint16_t replid;
	uint8_t mapi_type;
	uint64_t folder_id;
	uint64_t message_id;
	
	if (22 != folder_key.cb || (NULL != pmessage_key
		&& 22 != pmessage_key->cb)) {
		return ecInvalidParam;
	}
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto pstore = pinfo->ptree->get_object<STORE_OBJECT>(hstore, &mapi_type);
	if (pstore == nullptr)
		return ecNullObject;
	if (mapi_type != ZMG_STORE)
		return ecNotSupported;
	if (FALSE == common_util_binary_to_xid(
		&folder_key, &tmp_xid)) {
		return ecNotSupported;
	}
	if (pstore->b_private) {
		auto tmp_guid = rop_util_make_user_guid(pstore->account_id);
		if (0 != memcmp(&tmp_guid, &tmp_xid.guid, sizeof(GUID))) {
			return ecInvalidParam;
		}
		folder_id = rop_util_make_eid(1, tmp_xid.local_id);
	} else {
		domain_id = rop_util_make_domain_id(tmp_xid.guid);
		if (-1 == domain_id) {
			return ecInvalidParam;
		}
		if (domain_id == pstore->account_id) {
			replid = 1;
		} else {
			if (pmessage_key != nullptr)
				return ecInvalidParam;
			if (!system_services_check_same_org(domain_id, pstore->account_id))
				return ecInvalidParam;
			if (!exmdb_client::get_mapping_replid(pstore->get_dir(),
			    tmp_xid.guid, &b_found, &replid))
				return ecError;
			if (!b_found)
				return ecNotFound;
		}
		folder_id = rop_util_make_eid(replid, tmp_xid.local_id);
	}
	if (NULL != pmessage_key) {
		if (FALSE == common_util_binary_to_xid(
			pmessage_key, &tmp_xid)) {
			return ecNotSupported;
		}
		if (pstore->b_private) {
			auto tmp_guid = rop_util_make_user_guid(pstore->account_id);
			if (0 != memcmp(&tmp_guid, &tmp_xid.guid, sizeof(GUID))) {
				return ecInvalidParam;
			}
			message_id = rop_util_make_eid(1, tmp_xid.local_id);
		} else {
			domain_id = rop_util_make_domain_id(tmp_xid.guid);
			if (-1 == domain_id) {
				return ecInvalidParam;
			}
			if (domain_id != pstore->account_id)
				return ecInvalidParam;
			message_id = rop_util_make_eid(1, tmp_xid.local_id);
		}
		pbin = common_util_to_message_entryid(
				pstore, folder_id, message_id);
	} else {
		pbin = common_util_to_folder_entryid(pstore, folder_id);
	}
	if (pbin == nullptr)
		return ecError;
	*pentryid = *pbin;
	return ecSuccess;
}

uint32_t zarafa_server_storeadvise(GUID hsession,
	uint32_t hstore, const BINARY *pentryid,
	uint32_t event_mask, uint32_t *psub_id)
{
	char dir[256];
	uint16_t type;
	BOOL b_private;
	int account_id;
	uint8_t mapi_type;
	uint64_t folder_id;
	uint64_t message_id;
	
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto pstore = pinfo->ptree->get_object<STORE_OBJECT>(hstore, &mapi_type);
	if (pstore == nullptr)
		return ecNullObject;
	if (mapi_type != ZMG_STORE)
		return ecNotSupported;
	folder_id = 0;
	message_id = 0;
	if (NULL != pentryid) {
		type = common_util_get_messaging_entryid_type(*pentryid);
		switch (type) {
		case EITLT_PRIVATE_FOLDER:
		case EITLT_PUBLIC_FOLDER:
			if (FALSE == common_util_from_folder_entryid(
				*pentryid, &b_private, &account_id, &folder_id)) {
				return ecError;
			}
			break;
		case EITLT_PRIVATE_MESSAGE:
		case EITLT_PUBLIC_MESSAGE:
			if (FALSE == common_util_from_message_entryid(
				*pentryid, &b_private, &account_id,
				&folder_id, &message_id)) {
				return ecError;
			}
			break;
		default:
			return ecNotFound;
		}
		if (b_private != pstore->b_private || account_id != pstore->account_id)
			return ecInvalidParam;
	}
	if (!exmdb_client::subscribe_notification(pstore->get_dir(),
	    event_mask, TRUE, folder_id, message_id, psub_id))
		return ecError;
	gx_strlcpy(dir, pstore->get_dir(), arsizeof(dir));
	pinfo.reset();
	std::unique_lock nl_hold(g_notify_lock);
	if (g_notify_table.size() == g_table_size) {
		nl_hold.unlock();
		exmdb_client::unsubscribe_notification(dir, *psub_id);
		return ecError;
	}
	try {
		auto tmp_buf = std::to_string(*psub_id) + "|" + dir;
		g_notify_table.try_emplace(std::move(tmp_buf), hsession, hstore);
	} catch (const std::bad_alloc &) {
		nl_hold.unlock();
		exmdb_client::unsubscribe_notification(dir, *psub_id);
		return ecError;
	}
	return ecSuccess;
}

uint32_t zarafa_server_unadvise(GUID hsession, uint32_t hstore,
     uint32_t sub_id) try
{	
	uint8_t mapi_type;
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto pstore = pinfo->ptree->get_object<STORE_OBJECT>(hstore, &mapi_type);
	if (pstore == nullptr)
		return ecNullObject;
	if (mapi_type != ZMG_STORE)
		return ecNotSupported;
	std::string dir = pstore->get_dir();
	pinfo.reset();
	exmdb_client::unsubscribe_notification(dir.c_str(), sub_id);
	auto tmp_buf = std::to_string(sub_id) + "|"s + std::move(dir);
	std::unique_lock nl_hold(g_notify_lock);
	g_notify_table.erase(std::move(tmp_buf));
	return ecSuccess;
} catch (const std::bad_alloc &) {
	fprintf(stderr, "E-1498: ENOMEM\n");
	return ecMAPIOOM;
}

uint32_t zarafa_server_notifdequeue(const NOTIF_SINK *psink,
	uint32_t timeval, ZNOTIFICATION_ARRAY *pnotifications)
{
	int i;
	int count;
	uint8_t mapi_type;
	DOUBLE_LIST_NODE *pnode;
	ZNOTIFICATION* ppnotifications[1024];
	
	auto pinfo = zarafa_server_query_session(psink->hsession);
	if (pinfo == nullptr)
		return ecError;
	count = 0;
	for (i=0; i<psink->count; i++) {
		auto pstore = pinfo->ptree->get_object<STORE_OBJECT>(psink->padvise[i].hstore, &mapi_type);
		if (pstore == nullptr || mapi_type != ZMG_STORE)
			continue;
		std::string tmp_buf;
		try {
			tmp_buf = std::to_string(psink->padvise[i].sub_id) +
			          "|" + pstore->get_dir();
		} catch (const std::bad_alloc &) {
			fprintf(stderr, "E-1496: ENOMEM\n");
			continue;
		}
		std::unique_lock nl_hold(g_notify_lock);
		auto iter = g_notify_table.find(std::move(tmp_buf));
		if (iter == g_notify_table.end())
			continue;
		auto pnitem = &iter->second;
		time(&pnitem->last_time);
		while ((pnode = double_list_pop_front(&pnitem->notify_list)) != nullptr) {
			ppnotifications[count] = common_util_dup_znotification(static_cast<ZNOTIFICATION *>(pnode->pdata), true);
			common_util_free_znotification(static_cast<ZNOTIFICATION *>(pnode->pdata));
			free(pnode);
			if (ppnotifications[count] != nullptr)
				count ++;
			if (1024 == count) {
				break;
			}
		}
		nl_hold.unlock();
		if (1024 == count) {
			break;
		}
	}
	if (count > 0) {
		pinfo.reset();
		pnotifications->count = count;
		pnotifications->ppnotification = cu_alloc<ZNOTIFICATION *>(count);
		if (pnotifications->ppnotification == nullptr)
			return ecError;
		memcpy(pnotifications->ppnotification,
			ppnotifications, sizeof(void*)*count);
		return ecSuccess;
	}
	auto psink_node = me_alloc<SINK_NODE>();
	if (psink_node == nullptr)
		return ecError;
	psink_node->node.pdata = psink_node;
	psink_node->clifd = common_util_get_clifd();
	time(&psink_node->until_time);
	psink_node->until_time += timeval;
	psink_node->sink.hsession = psink->hsession;
	psink_node->sink.count = psink->count;
	psink_node->sink.padvise = me_alloc<ADVISE_INFO>(psink->count);
	if (NULL == psink_node->sink.padvise) {
		free(psink_node);
		return ecError;
	}
	memcpy(psink_node->sink.padvise, psink->padvise,
				psink->count*sizeof(ADVISE_INFO));
	double_list_append_as_tail(
		&pinfo->sink_list, &psink_node->node);
	return ecNotFound;
}

uint32_t zarafa_server_queryrows(
	GUID hsession, uint32_t htable, uint32_t start,
	uint32_t count, const RESTRICTION *prestriction,
	const PROPTAG_ARRAY *pproptags, TARRAY_SET *prowset)
{
	uint32_t row_num;
	int32_t position;
	uint8_t mapi_type;
	TARRAY_SET tmp_set;
	uint32_t *pobject_type = nullptr;
	TAGGED_PROPVAL *ppropvals;
	static const uint32_t object_type_store = OBJECT_STORE;
	static const uint32_t object_type_folder = OBJECT_FOLDER;
	static const uint32_t object_type_message = OBJECT_MESSAGE;
	static const uint32_t object_type_attachment = OBJECT_ATTACHMENT;
	
	if (count > 0x7FFFFFFF) {
		count = 0x7FFFFFFF;
	}
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto ptable = pinfo->ptree->get_object<TABLE_OBJECT>(htable, &mapi_type);
	if (ptable == nullptr)
		return ecNullObject;
	if (mapi_type != ZMG_TABLE)
		return ecNotSupported;
	if (!ptable->check_to_load())
		return ecError;
	auto table_type = ptable->get_table_type();
	if (0xFFFFFFFF != start) {
		ptable->set_position(start);
	}
	if (NULL != prestriction) {
		switch (ptable->get_table_type()) {
		case HIERARCHY_TABLE:
		case CONTENT_TABLE:
		case RULE_TABLE:
			row_num = ptable->get_total();
			if (row_num > count) {
				row_num = count;
			}
			prowset->count = 0;
			prowset->pparray = cu_alloc<TPROPVAL_ARRAY *>(row_num);
			if (prowset->pparray == nullptr)
				return ecError;
			while (TRUE) {
				if (!ptable->match_row(TRUE, prestriction, &position))
					return ecError;
				if (position < 0) {
					break;
				}
				ptable->set_position(position);
				if (!ptable->query_rows(pproptags, 1, &tmp_set))
					return ecError;
				if (1 != tmp_set.count) {
					break;
				}
				ptable->seek_current(TRUE, 1);
				prowset->pparray[prowset->count] = tmp_set.pparray[0];
				prowset->count ++;
				if (count == prowset->count) {
					break;
				}
			}
			break;
		case ATTACHMENT_TABLE:
		case RECIPIENT_TABLE:
		case USER_TABLE:
			if (!ptable->filter_rows(count, prestriction, pproptags, prowset))
				return ecError;
			break;
		default:
			return ecNotSupported;
		}
	} else {
		if (!ptable->query_rows(pproptags, count, prowset))
			return ecError;
		ptable->seek_current(TRUE, prowset->count);
	}
	pinfo.reset();
	if ((STORE_TABLE != table_type &&
		HIERARCHY_TABLE != table_type &&
		CONTENT_TABLE != table_type &&
		ATTACHMENT_TABLE != table_type)
		||
	    (pproptags != nullptr &&
	    common_util_index_proptags(pproptags, PR_OBJECT_TYPE) < 0))
		return ecSuccess;
	switch (table_type) {
	case STORE_TABLE:
		pobject_type = deconst(&object_type_store);
		break;
	case HIERARCHY_TABLE:
		pobject_type = deconst(&object_type_folder);
		break;
	case CONTENT_TABLE:
		pobject_type = deconst(&object_type_message);
		break;
	case ATTACHMENT_TABLE:
		pobject_type = deconst(&object_type_attachment);
		break;
	}
	for (size_t i = 0; i < prowset->count; ++i) {
		ppropvals = cu_alloc<TAGGED_PROPVAL>(prowset->pparray[i]->count + 1);
		if (ppropvals == nullptr)
			return ecError;
		memcpy(ppropvals, prowset->pparray[i]->ppropval,
			sizeof(TAGGED_PROPVAL)*prowset->pparray[i]->count);
		ppropvals[prowset->pparray[i]->count].proptag = PR_OBJECT_TYPE;
		ppropvals[prowset->pparray[i]->count].pvalue = pobject_type;
		prowset->pparray[i]->ppropval = ppropvals;
		prowset->pparray[i]->count ++;
	}
	return ecSuccess;
}
	
uint32_t zarafa_server_setcolumns(GUID hsession, uint32_t htable,
	const PROPTAG_ARRAY *pproptags, uint32_t flags)
{
	uint8_t mapi_type;
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto ptable = pinfo->ptree->get_object<TABLE_OBJECT>(htable, &mapi_type);
	if (ptable == nullptr)
		return ecNullObject;
	if (mapi_type != ZMG_TABLE)
		return ecNotSupported;
	return ptable->set_columns(pproptags) ? ecSuccess : ecError;
}

uint32_t zarafa_server_seekrow(GUID hsession,
	uint32_t htable, uint32_t bookmark, int32_t seek_rows,
	int32_t *psought_rows)
{
	BOOL b_exist;
	uint8_t mapi_type;
	uint32_t original_position;
	
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto ptable = pinfo->ptree->get_object<TABLE_OBJECT>(htable, &mapi_type);
	if (ptable == nullptr)
		return ecNullObject;
	if (mapi_type != ZMG_TABLE)
		return ecNotSupported;
	if (!ptable->check_to_load())
		return ecError;
	switch (bookmark) {
	case BOOKMARK_BEGINNING:
		if (seek_rows < 0) {
			return ecInvalidParam;
		}
		original_position = 0;
		ptable->set_position(seek_rows);
		break;
	case BOOKMARK_END:
		if (seek_rows > 0) {
			return ecInvalidParam;
		}
		original_position = ptable->get_total();
		ptable->set_position(safe_add_s(original_position, seek_rows));
		break;
	case BOOKMARK_CURRENT:
		original_position = ptable->get_position();
		ptable->set_position(safe_add_s(original_position, seek_rows));
		break;
	default: {
		original_position = ptable->get_position();
		if (!ptable->retrieve_bookmark(bookmark, &b_exist))
			return ecError;
		if (!b_exist)
			return ecNotFound;
		auto original_position1 = ptable->get_position();
		ptable->set_position(safe_add_s(original_position1, seek_rows));
		break;
	}
	}
	*psought_rows = ptable->get_position() - original_position;
	return ecSuccess;
}

uint32_t zarafa_server_sorttable(GUID hsession,
	uint32_t htable, const SORTORDER_SET *psortset)
{
	int i, j;
	BOOL b_max;
	uint16_t type;
	uint8_t mapi_type;
	BOOL b_multi_inst;
	uint32_t tmp_proptag;
	
	if (psortset->count > MAXIMUM_SORT_COUNT) {
		return ecTooComplex;
	}
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto ptable = pinfo->ptree->get_object<TABLE_OBJECT>(htable, &mapi_type);
	if (ptable == nullptr)
		return ecNullObject;
	if (mapi_type != ZMG_TABLE)
		return ecNotSupported;
	if (ptable->get_table_type() != CONTENT_TABLE)
		return ecSuccess;
	b_max = FALSE;
	b_multi_inst = FALSE;
	for (i=0; i<psortset->ccategories; i++) {
		for (j=i+1; j<psortset->count; j++) {
			if (psortset->psort[i].propid ==
				psortset->psort[j].propid &&
				psortset->psort[i].type ==
				psortset->psort[j].type) {
				return ecInvalidParam;
			}
		}
	}
	for (i=0; i<psortset->count; i++) {
		tmp_proptag = PROP_TAG(psortset->psort[i].type, psortset->psort[i].propid);
		if (PROP_TAG_DEPTH == tmp_proptag ||
			PROP_TAG_INSTID == tmp_proptag ||
			PROP_TAG_INSTANCENUM == tmp_proptag ||
			PROP_TAG_CONTENTCOUNT == tmp_proptag ||
			PROP_TAG_CONTENTUNREADCOUNT == tmp_proptag) {
			return ecInvalidParam;
		}	
		switch (psortset->psort[i].table_sort) {
		case TABLE_SORT_ASCEND:
		case TABLE_SORT_DESCEND:
			break;
		case TABLE_SORT_MAXIMUM_CATEGORY:
		case TABLE_SORT_MINIMUM_CATEGORY:
			if (0 == psortset->ccategories ||
				psortset->ccategories != i) {
				return ecInvalidParam;
			}
			break;
		default:
			return ecInvalidParam;
		}
		type = psortset->psort[i].type;
		if (type & MV_FLAG) {
			/* we do not support multivalue property
				without multivalue instances */
			if (!(type & MV_INSTANCE)) {
				return ecNotSupported;
			}
			type &= ~MV_INSTANCE;
			/* MUST NOT contain more than one multivalue property! */
			if (b_multi_inst)
				return ecInvalidParam;
			b_multi_inst = TRUE;
		}
		switch (type) {
		case PT_SHORT:
		case PT_LONG:
		case PT_FLOAT:
		case PT_DOUBLE:
		case PT_CURRENCY:
		case PT_APPTIME:
		case PT_BOOLEAN:
		case PT_OBJECT:
		case PT_I8:
		case PT_STRING8:
		case PT_UNICODE:
		case PT_SYSTIME:
		case PT_CLSID:
		case PT_SVREID:
		case PT_SRESTRICT:
		case PT_ACTIONS:
		case PT_BINARY:
		case PT_MV_SHORT:
		case PT_MV_LONG:
		case PT_MV_I8:
		case PT_MV_STRING8:
		case PT_MV_UNICODE:
		case PT_MV_CLSID:
		case PT_MV_BINARY:
			break;
		case PT_UNSPECIFIED:
		case PT_ERROR:
		default:
			return ecInvalidParam;
		}
		if (TABLE_SORT_MAXIMUM_CATEGORY ==
			psortset->psort[i].table_sort ||
			TABLE_SORT_MINIMUM_CATEGORY ==
			psortset->psort[i].table_sort) {
			if (b_max || i != psortset->ccategories)
				return ecInvalidParam;
			b_max = TRUE;
		}
	}
	auto pcolumns = ptable->get_columns();
	if (b_multi_inst && pcolumns != nullptr &&
	    !common_util_verify_columns_and_sorts(pcolumns, psortset))
		return ecNotSupported;
	if (!ptable->set_sorts(psortset))
		return ecError;
	ptable->unload();
	ptable->clear_bookmarks();
	ptable->clear_position();
	return ecSuccess;
}

uint32_t zarafa_server_getrowcount(GUID hsession,
	uint32_t htable, uint32_t *pcount)
{
	uint8_t mapi_type;
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto ptable = pinfo->ptree->get_object<TABLE_OBJECT>(htable, &mapi_type);
	if (ptable == nullptr)
		return ecNullObject;
	if (mapi_type != ZMG_TABLE)
		return ecNotSupported;
	if (!ptable->check_to_load())
		return ecError;
	*pcount = ptable->get_total();
	return ecSuccess;
}

uint32_t zarafa_server_restricttable(GUID hsession, uint32_t htable,
	const RESTRICTION *prestriction, uint32_t flags)
{
	uint8_t mapi_type;
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto ptable = pinfo->ptree->get_object<TABLE_OBJECT>(htable, &mapi_type);
	if (ptable == nullptr)
		return ecNullObject;
	if (mapi_type != ZMG_TABLE)
		return ecNotSupported;
	switch (ptable->get_table_type()) {
	case HIERARCHY_TABLE:
	case CONTENT_TABLE:
	case RULE_TABLE:
	case USER_TABLE:
		break;
	default:
		return ecNotSupported;
	}
	if (!ptable->set_restriction(prestriction))
		return ecError;
	ptable->unload();
	ptable->clear_bookmarks();
	ptable->clear_position();
	return ecSuccess;
}

uint32_t zarafa_server_findrow(GUID hsession, uint32_t htable,
	uint32_t bookmark, const RESTRICTION *prestriction,
	uint32_t flags, uint32_t *prow_idx)
{
	BOOL b_exist;
	int32_t position;
	uint8_t mapi_type;
	
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto ptable = pinfo->ptree->get_object<TABLE_OBJECT>(htable, &mapi_type);
	if (ptable == nullptr)
		return ecNullObject;
	if (mapi_type != ZMG_TABLE)
		return ecNotSupported;
	switch (ptable->get_table_type()) {
	case HIERARCHY_TABLE:
	case CONTENT_TABLE:
	case RULE_TABLE:
		break;
	default:
		return ecNotSupported;
	}
	if (!ptable->check_to_load())
		return ecError;
	switch (bookmark) {
	case BOOKMARK_BEGINNING:
		ptable->set_position(0);
		break;
	case BOOKMARK_END:
		ptable->set_position(ptable->get_total());
		break;
	case BOOKMARK_CURRENT:
		break;
	default:
		if (ptable->get_table_type() == RULE_TABLE)
			return ecNotSupported;
		if (!ptable->retrieve_bookmark(bookmark, &b_exist))
			return ecInvalidBookmark;
		break;
	}
	if (ptable->match_row(TRUE, prestriction, &position))
		return ecError;
	if (position < 0) {
		return ecNotFound;
	}
	ptable->set_position(position);
	*prow_idx = position;
	return ecSuccess;
}

uint32_t zarafa_server_createbookmark(GUID hsession,
	uint32_t htable, uint32_t *pbookmark)
{
	uint8_t mapi_type;
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto ptable = pinfo->ptree->get_object<TABLE_OBJECT>(htable, &mapi_type);
	if (ptable == nullptr)
		return ecNullObject;
	if (mapi_type != ZMG_TABLE)
		return ecNotSupported;
	switch (ptable->get_table_type()) {
	case HIERARCHY_TABLE:
	case CONTENT_TABLE:
		break;
	default:
		return ecNotSupported;
	}
	if (!ptable->check_to_load())
		return ecError;
	return ptable->create_bookmark(pbookmark) ? ecSuccess : ecError;
}

uint32_t zarafa_server_freebookmark(GUID hsession,
	uint32_t htable, uint32_t bookmark)
{
	uint8_t mapi_type;
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto ptable = pinfo->ptree->get_object<TABLE_OBJECT>(htable, &mapi_type);
	if (ptable == nullptr)
		return ecNullObject;
	if (mapi_type != ZMG_TABLE)
		return ecNotSupported;
	switch (ptable->get_table_type()) {
	case HIERARCHY_TABLE:
	case CONTENT_TABLE:
		break;
	default:
		return ecNotSupported;
	}
	ptable->remove_bookmark(bookmark);
	return ecSuccess;
}

uint32_t zarafa_server_getreceivefolder(GUID hsession,
	uint32_t hstore, const char *pstrclass, BINARY *pentryid)
{
	BINARY *pbin;
	uint8_t mapi_type;
	uint64_t folder_id;
	char temp_class[256];
	
	if (pstrclass == nullptr)
		pstrclass = "";
	if (FALSE == common_util_check_message_class(pstrclass)) {
		return ecInvalidParam;
	}
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto pstore = pinfo->ptree->get_object<STORE_OBJECT>(hstore, &mapi_type);
	if (pstore == nullptr)
		return ecNullObject;
	if (mapi_type != ZMG_STORE)
		return ecNotSupported;
	if (!pstore->b_private)
		return ecNotSupported;
	if (!exmdb_client::get_folder_by_class(pstore->get_dir(), pstrclass,
	    &folder_id, temp_class))
		return ecError;
	pbin = common_util_to_folder_entryid(pstore, folder_id);
	if (pbin == nullptr)
		return ecError;
	*pentryid = *pbin;
	return ecSuccess;
}

uint32_t zarafa_server_modifyrecipients(GUID hsession,
	uint32_t hmessage, uint32_t flags, const TARRAY_SET *prcpt_list)
{
	BOOL b_found;
	BINARY *pbin;
	uint32_t *prowid;
	uint8_t mapi_type;
	EXT_PULL ext_pull;
	uint32_t tmp_flags;
	char tmp_buff[256];
	uint8_t tmp_uid[16];
	uint32_t last_rowid;
	TPROPVAL_ARRAY *prcpt;
	uint8_t fake_true = 1;
	uint8_t fake_false = 0;
	uint8_t provider_uid[16];
	TAGGED_PROPVAL *ppropval;
	TAGGED_PROPVAL tmp_propval;
	ONEOFF_ENTRYID oneoff_entry;
	ADDRESSBOOK_ENTRYID ab_entryid;
	
	if (prcpt_list->count >= 0x7FEF || (MODRECIP_ADD != flags &&
		MODRECIP_MODIFY != flags && MODRECIP_REMOVE != flags)) {
		return ecInvalidParam;
	}
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto pmessage = pinfo->ptree->get_object<MESSAGE_OBJECT>(hmessage, &mapi_type);
	if (pmessage == nullptr)
		return ecNullObject;
	if (mapi_type != ZMG_MESSAGE)
		return ecNotSupported;
	if (MODRECIP_MODIFY == flags) {
		pmessage->empty_rcpts();
	} else if (MODRECIP_REMOVE == flags) {
		for (size_t i = 0; i < prcpt_list->count; ++i) {
			prcpt = prcpt_list->pparray[i];
			b_found = FALSE;
			for (size_t j = 0; j < prcpt->count; ++j) {
				if (PROP_TAG_ROWID == prcpt->ppropval[j].proptag) {
					prcpt->count = 1;
					prcpt->ppropval = prcpt->ppropval + j;
					b_found = TRUE;
					break;
				}
			}
			if (!b_found)
				return ecInvalidParam;
		}
		if (!pmessage->set_rcpts(prcpt_list))
			return ecError;
		return ecSuccess;
	}
	if (!pmessage->get_rowid_begin(&last_rowid))
		return ecError;
	for (size_t i = 0; i < prcpt_list->count; ++i, ++last_rowid) {
		if (common_util_get_propvals(prcpt_list->pparray[i], PR_ENTRYID) == nullptr &&
		    common_util_get_propvals(prcpt_list->pparray[i], PR_EMAIL_ADDRESS) == nullptr &&
		    common_util_get_propvals(prcpt_list->pparray[i], PR_SMTP_ADDRESS) == nullptr)
			return ecInvalidParam;
		prowid = static_cast<uint32_t *>(common_util_get_propvals(
		         prcpt_list->pparray[i], PROP_TAG_ROWID));
		if (NULL != prowid) {
			if (*prowid < last_rowid) {
				*prowid = last_rowid;
			} else {
				last_rowid = *prowid;
			}
		} else {
			prcpt = prcpt_list->pparray[i];
			ppropval = cu_alloc<TAGGED_PROPVAL>(prcpt->count + 1);
			if (ppropval == nullptr)
				return ecError;
			memcpy(ppropval, prcpt->ppropval,
				sizeof(TAGGED_PROPVAL)*prcpt->count);
			ppropval[prcpt->count].proptag = PROP_TAG_ROWID;
			ppropval[prcpt->count].pvalue = cu_alloc<uint32_t>();
			if (ppropval[prcpt->count].pvalue == nullptr)
				return ecError;
			*(uint32_t*)ppropval[prcpt->count].pvalue = last_rowid;
			prcpt->ppropval = ppropval;
			prcpt->count ++;
			pbin = static_cast<BINARY *>(common_util_get_propvals(prcpt, PR_ENTRYID));
			if (pbin == nullptr ||
			    (common_util_get_propvals(prcpt, PR_EMAIL_ADDRESS) != nullptr &&
			    common_util_get_propvals(prcpt, PROP_TAG_ADDRESSTYPE) != nullptr &&
			    common_util_get_propvals(prcpt, PR_DISPLAY_NAME) != nullptr))
				continue;
			ext_pull.init(pbin->pb, pbin->cb, common_util_alloc, 0);
			if (ext_pull.g_uint32(&tmp_flags) != EXT_ERR_SUCCESS ||
			    tmp_flags != 0)
				continue;
			if (ext_pull.g_bytes(provider_uid, arsizeof(provider_uid)) != EXT_ERR_SUCCESS)
				continue;
			rop_util_get_provider_uid(PROVIDER_UID_ADDRESS_BOOK, tmp_uid);
			if (0 == memcmp(tmp_uid, provider_uid, 16)) {
				ext_pull.init(pbin->pb, pbin->cb, common_util_alloc, EXT_FLAG_UTF16);
				if (ext_pull.g_abk_eid(&ab_entryid) != EXT_ERR_SUCCESS)
					continue;
				if (ADDRESSBOOK_ENTRYID_TYPE_LOCAL_USER
					!= ab_entryid.type) {
					continue;
				}
				ppropval = cu_alloc<TAGGED_PROPVAL>(prcpt->count + 4);
				if (ppropval == nullptr)
					return ecError;
				memcpy(ppropval, prcpt->ppropval,
					prcpt->count*sizeof(TAGGED_PROPVAL));
				prcpt->ppropval = ppropval;
				tmp_propval.proptag = PROP_TAG_ADDRESSTYPE;
				tmp_propval.pvalue  = deconst("EX");
				common_util_set_propvals(prcpt, &tmp_propval);
				tmp_propval.proptag = PR_EMAIL_ADDRESS;
				tmp_propval.pvalue = common_util_dup(ab_entryid.px500dn);
				if (tmp_propval.pvalue == nullptr)
					return ecError;
				common_util_set_propvals(prcpt, &tmp_propval);
				tmp_propval.proptag = PR_SMTP_ADDRESS;
				if (!common_util_essdn_to_username(ab_entryid.px500dn,
				    tmp_buff, GX_ARRAY_SIZE(tmp_buff)))
					continue;
				tmp_propval.pvalue = common_util_dup(tmp_buff);
				if (tmp_propval.pvalue == nullptr)
					return ecError;
				common_util_set_propvals(prcpt, &tmp_propval);
				if (FALSE == system_services_get_user_displayname(
					tmp_buff, tmp_buff)) {
					continue;	
				}
				tmp_propval.proptag = PR_DISPLAY_NAME;
				tmp_propval.pvalue = common_util_dup(tmp_buff);
				if (tmp_propval.pvalue == nullptr)
					return ecError;
				common_util_set_propvals(prcpt, &tmp_propval);
				continue;
			}
			rop_util_get_provider_uid(PROVIDER_UID_ONE_OFF, tmp_uid);
			if (0 == memcmp(tmp_uid, provider_uid, 16)) {
				ext_pull.init(pbin->pb, pbin->cb, common_util_alloc, EXT_FLAG_UTF16);
				if (ext_pull.g_oneoff_eid(&oneoff_entry) != EXT_ERR_SUCCESS ||
				    strcasecmp(oneoff_entry.paddress_type, "SMTP") != 0)
					continue;
				ppropval = cu_alloc<TAGGED_PROPVAL>(prcpt->count + 5);
				if (ppropval == nullptr)
					return ecError;
				memcpy(ppropval, prcpt->ppropval,
					prcpt->count*sizeof(TAGGED_PROPVAL));
				prcpt->ppropval = ppropval;
				tmp_propval.proptag = PROP_TAG_ADDRESSTYPE;
				tmp_propval.pvalue  = deconst("SMTP");
				common_util_set_propvals(prcpt, &tmp_propval);
				tmp_propval.proptag = PR_EMAIL_ADDRESS;
				tmp_propval.pvalue = common_util_dup(
						oneoff_entry.pmail_address);
				if (tmp_propval.pvalue == nullptr)
					return ecError;
				common_util_set_propvals(prcpt, &tmp_propval);
				tmp_propval.proptag = PR_SMTP_ADDRESS;
				common_util_set_propvals(prcpt, &tmp_propval);
				tmp_propval.proptag = PR_DISPLAY_NAME;
				tmp_propval.pvalue = common_util_dup(
						oneoff_entry.pdisplay_name);
				if (tmp_propval.pvalue == nullptr)
					return ecError;
				common_util_set_propvals(prcpt, &tmp_propval);
				tmp_propval.proptag = PROP_TAG_SENDRICHINFO;
				tmp_propval.pvalue = (oneoff_entry.ctrl_flags & CTRL_FLAG_NORICH) ? &fake_false : &fake_true;
				common_util_set_propvals(prcpt, &tmp_propval);
			}
		}
	}
	return pmessage->set_rcpts(prcpt_list) ? ecSuccess : ecError;
}

uint32_t zarafa_server_submitmessage(GUID hsession, uint32_t hmessage)
{
	int timer_id;
	void *pvalue;
	BOOL b_marked;
	time_t cur_time;
	uint32_t tmp_num;
	uint8_t mapi_type;
	uint16_t rcpt_num;
	char username[UADDR_SIZE];
	uint32_t mail_length;
	uint64_t submit_time;
	uint32_t deferred_time;
	uint32_t message_flags;
	char command_buff[1024];
	uint32_t proptag_buff[6];
	PROPTAG_ARRAY tmp_proptags;
	TPROPVAL_ARRAY tmp_propvals;
	
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto pmessage = pinfo->ptree->get_object<MESSAGE_OBJECT>(hmessage, &mapi_type);
	if (pmessage == nullptr)
		return ecNullObject;
	if (mapi_type != ZMG_MESSAGE)
		return ecNotSupported;
	auto pstore = pmessage->get_store();
	if (!pstore->b_private)
		return ecNotSupported;
	if (!pstore->check_owner_mode()) {
		uint32_t permission = 0;
		if (!exmdb_client::check_mailbox_permission(pstore->get_dir(),
		    pinfo->get_username(), &permission))
			return ecError;
		if (!(permission & frightsGromoxSendAs))
			return ecAccessDenied;
	}
	if (pmessage->get_id() == 0)
		return ecNotSupported;
	if (pmessage->check_importing() || !pmessage->check_writable())
		return ecAccessDenied;
	if (!pmessage->get_recipient_num(&rcpt_num))
		return ecError;
	if (rcpt_num > common_util_get_param(COMMON_UTIL_MAX_RCPT)) {
		return ecTooManyRecips;
	}
	tmp_proptags.count = 1;
	tmp_proptags.pproptag = proptag_buff;
	proptag_buff[0] = PROP_TAG_ASSOCIATED;
	if (!pmessage->get_properties(&tmp_proptags, &tmp_propvals))
		return ecError;
	pvalue = common_util_get_propvals(
		&tmp_propvals, PROP_TAG_ASSOCIATED);
	/* FAI message cannot be sent */
	if (pvalue != nullptr && *static_cast<uint8_t *>(pvalue) != 0)
		return ecAccessDenied;
	if (!common_util_check_delegate(pmessage, username, GX_ARRAY_SIZE(username))) {
		return ecError;
	}
	auto account = pstore->get_account();
	if ('\0' == username[0]) {
		gx_strlcpy(username, account, GX_ARRAY_SIZE(username));
	} else if (!common_util_check_delegate_permission_ex(account, username)) {
		fprintf(stderr, "I-1334: store %s tried to send with from=%s, but no delegate permission.\n",
		        account, username);
		return ecAccessDenied;
	}
	gxerr_t err = common_util_rectify_message(pmessage, username);
	if (err != GXERR_SUCCESS) {
		return gxerr_to_hresult(err);
	}
	tmp_proptags.count = 3;
	tmp_proptags.pproptag = proptag_buff;
	proptag_buff[0] = PR_MAX_SUBMIT_MESSAGE_SIZE;
	proptag_buff[1] = PROP_TAG_PROHIBITSENDQUOTA;
	proptag_buff[2] = PR_MESSAGE_SIZE_EXTENDED;
	if (!pstore->get_properties(&tmp_proptags, &tmp_propvals))
		return ecError;

	auto sendquota = static_cast<uint32_t *>(common_util_get_propvals(&tmp_propvals, PROP_TAG_PROHIBITSENDQUOTA));
	auto storesize = static_cast<uint64_t *>(common_util_get_propvals(&tmp_propvals, PR_MESSAGE_SIZE_EXTENDED));
	/* Sendquota is in KiB, storesize in bytes */
	if (sendquota != nullptr && storesize != nullptr &&
	    static_cast<uint64_t>(*sendquota) * 1024 <= *storesize) {
		return ecQuotaExceeded;
	}

	pvalue = common_util_get_propvals(&tmp_propvals, PR_MAX_SUBMIT_MESSAGE_SIZE);
	ssize_t max_length = -1;
	if (pvalue != nullptr)
		max_length = *(int32_t*)pvalue;
	tmp_proptags.count = 6;
	tmp_proptags.pproptag = proptag_buff;
	proptag_buff[0] = PR_MESSAGE_SIZE;
	proptag_buff[1] = PR_MESSAGE_FLAGS;
	proptag_buff[2] = PROP_TAG_DEFERREDSENDTIME;
	proptag_buff[3] = PROP_TAG_DEFERREDSENDNUMBER;
	proptag_buff[4] = PROP_TAG_DEFERREDSENDUNITS;
	proptag_buff[5] = PROP_TAG_DELETEAFTERSUBMIT;
	if (!pmessage->get_properties(&tmp_proptags, &tmp_propvals))
		return ecError;
	pvalue = common_util_get_propvals(&tmp_propvals, PR_MESSAGE_SIZE);
	if (pvalue == nullptr)
		return ecError;
	mail_length = *(uint32_t*)pvalue;
	if (max_length > 0 && mail_length > static_cast<size_t>(max_length)) {
		return EC_EXCEEDED_SIZE;
	}
	pvalue = common_util_get_propvals(&tmp_propvals, PR_MESSAGE_FLAGS);
	if (pvalue == nullptr)
		return ecError;
	message_flags = *(uint32_t*)pvalue;
	/* here we handle the submit request
		differently from exchange_emsmdb.
		we always allow a submitted message
		to be resubmitted */
	BOOL b_unsent = (message_flags & MSGFLAG_UNSENT) ? TRUE : false;
	pvalue = common_util_get_propvals(&tmp_propvals,
						PROP_TAG_DELETEAFTERSUBMIT);
	BOOL b_delete = pvalue != nullptr && *static_cast<uint8_t *>(pvalue) != 0 ? TRUE : false;
	if (!(message_flags & MSGFLAG_SUBMITTED)) {
		if (!exmdb_client::try_mark_submit(pstore->get_dir(),
		    pmessage->get_id(), &b_marked))
			return ecError;
		if (!b_marked)
			return ecAccessDenied;
		deferred_time = 0;
		time(&cur_time);
		submit_time = rop_util_unix_to_nttime(cur_time);
		pvalue = common_util_get_propvals(&tmp_propvals,
								PROP_TAG_DEFERREDSENDTIME);
		if (NULL != pvalue) {
			if (submit_time < *(uint64_t*)pvalue) {
				deferred_time = rop_util_nttime_to_unix(
							*(uint64_t*)pvalue) - cur_time;
			}
		} else {
			pvalue = common_util_get_propvals(&tmp_propvals,
								PROP_TAG_DEFERREDSENDNUMBER);
			if (NULL != pvalue) {
				tmp_num = *(uint32_t*)pvalue;
				pvalue = common_util_get_propvals(&tmp_propvals,
									PROP_TAG_DEFERREDSENDUNITS);
				if (NULL != pvalue) {
					switch (*(uint32_t*)pvalue) {
					case 0:
						deferred_time = tmp_num*60;
						break;
					case 1:
						deferred_time = tmp_num*60*60;
						break;
					case 2:
						deferred_time = tmp_num*60*60*24;
						break;
					case 3:
						deferred_time = tmp_num*60*60*24*7;
						break;
					}
				}
			}
		}
	
		if (deferred_time > 0) {
			snprintf(command_buff, 1024, "%s %s %llu",
				common_util_get_submit_command(),
			         pstore->get_account(),
			         static_cast<unsigned long long>(rop_util_get_gc_value(pmessage->get_id())));
			timer_id = system_services_add_timer(
					command_buff, deferred_time);
			if (0 == timer_id) {
				exmdb_client::clear_submit(pstore->get_dir(),
					pmessage->get_id(), b_unsent);
				return ecError;
			}
			exmdb_client::set_message_timer(pstore->get_dir(),
				pmessage->get_id(), timer_id);
			pmessage->reload();
			return ecSuccess;
		}
	}
	if (!common_util_send_message(pstore, pmessage->get_id(), TRUE)) {
		exmdb_client::clear_submit(pstore->get_dir(),
			pmessage->get_id(), b_unsent);
		return ecError;
	}
	if (!b_delete)
		pmessage->reload();
	else
		pmessage->clear_unsent();
	return ecSuccess;
}

uint32_t zarafa_server_loadattachmenttable(GUID hsession,
	uint32_t hmessage, uint32_t *phobject)
{
	uint8_t mapi_type;
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto pmessage = pinfo->ptree->get_object<MESSAGE_OBJECT>(hmessage, &mapi_type);
	if (pmessage == nullptr)
		return ecNullObject;
	if (mapi_type != ZMG_MESSAGE)
		return ecNotSupported;
	auto pstore = pmessage->get_store();
	auto ptable = table_object_create(pstore, pmessage, ATTACHMENT_TABLE, 0);
	if (ptable == nullptr)
		return ecError;
	*phobject = pinfo->ptree->add_object_handle(hmessage, ZMG_TABLE, ptable.get());
	if (*phobject == INVALID_HANDLE)
		return ecError;
	ptable.release();
	return ecSuccess;
}

uint32_t zarafa_server_openattachment(GUID hsession,
	uint32_t hmessage, uint32_t attach_id, uint32_t *phobject)
{
	uint8_t mapi_type;
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto pmessage = pinfo->ptree->get_object<MESSAGE_OBJECT>(hmessage, &mapi_type);
	if (pmessage == nullptr)
		return ecNullObject;
	if (mapi_type != ZMG_MESSAGE)
		return ecNotSupported;
	auto pattachment = attachment_object_create(pmessage, attach_id);
	if (pattachment == nullptr)
		return ecError;
	if (pattachment->get_instance_id() == 0)
		return ecNotFound;
	*phobject = pinfo->ptree->add_object_handle(hmessage, ZMG_ATTACH, pattachment.get());
	if (*phobject == INVALID_HANDLE)
		return ecError;
	pattachment.release();
	return ecSuccess;
}

uint32_t zarafa_server_createattachment(GUID hsession,
	uint32_t hmessage, uint32_t *phobject)
{
	uint8_t mapi_type;
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto pmessage = pinfo->ptree->get_object<MESSAGE_OBJECT>(hmessage, &mapi_type);
	if (pmessage == nullptr)
		return ecNullObject;
	if (mapi_type != ZMG_MESSAGE)
		return ecNotSupported;
	if (!pmessage->check_writable())
		return ecAccessDenied;
	auto pattachment = attachment_object_create(
		pmessage, ATTACHMENT_NUM_INVALID);
	if (pattachment == nullptr)
		return ecError;
	if (pattachment->get_attachment_num() == ATTACHMENT_NUM_INVALID)
		return ecMaxAttachmentExceeded;
	if (!pattachment->init_attachment())
		return ecError;
	*phobject = pinfo->ptree->add_object_handle(hmessage, ZMG_ATTACH, pattachment.get());
	if (*phobject == INVALID_HANDLE)
		return ecError;
	pattachment.release();
	return ecSuccess;
}

uint32_t zarafa_server_deleteattachment(GUID hsession,
	uint32_t hmessage, uint32_t attach_id)
{
	uint8_t mapi_type;
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto pmessage = pinfo->ptree->get_object<MESSAGE_OBJECT>(hmessage, &mapi_type);
	if (pmessage == nullptr)
		return ecNullObject;
	if (mapi_type != ZMG_MESSAGE)
		return ecNotSupported;
	if (!pmessage->check_writable())
		return ecAccessDenied;
	return pmessage->delete_attachment(attach_id) ? ecSuccess : ecError;
}

uint32_t zarafa_server_setpropvals(GUID hsession,
	uint32_t hobject, const TPROPVAL_ARRAY *ppropvals)
{
	int i;
	uint8_t mapi_type;
	uint32_t permission;
	
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto pobject = pinfo->ptree->get_object<void>(hobject, &mapi_type);
	if (pobject == nullptr)
		return ecNullObject;
	switch (mapi_type) {
	case ZMG_PROFPROPERTY:
		for (i=0; i<ppropvals->count; i++) {
			if (!tpropval_array_set_propval(static_cast<TPROPVAL_ARRAY *>(pobject),
			    &ppropvals->ppropval[i])) {
				return ecError;
			}
		}
		pinfo->ptree->touch_profile_sec();
		return ecSuccess;
	case ZMG_STORE: {
		auto store = static_cast<STORE_OBJECT *>(pobject);
		if (!store->check_owner_mode())
			return ecAccessDenied;
		if (!store->set_properties(ppropvals))
			return ecError;
		return ecSuccess;
	}
	case ZMG_FOLDER: {
		auto folder = static_cast<FOLDER_OBJECT *>(pobject);
		auto pstore = folder->pstore;
		if (!pstore->check_owner_mode()) {
			if (!exmdb_client::check_folder_permission(pstore->get_dir(),
			    folder->folder_id, pinfo->get_username(), &permission))
				return ecError;
			if (!(permission & frightsOwner))
				return ecAccessDenied;
		}
		if (!folder->set_properties(ppropvals))
			return ecError;
		return ecSuccess;
	}
	case ZMG_MESSAGE: {
		auto msg = static_cast<MESSAGE_OBJECT *>(pobject);
		if (!msg->check_writable())
			return ecAccessDenied;
		if (!msg->set_properties(ppropvals))
			return ecError;
		return ecSuccess;
	}
	case ZMG_ATTACH: {
		auto atx = static_cast<ATTACHMENT_OBJECT *>(pobject);
		if (!atx->check_writable())
			return ecAccessDenied;
		if (!atx->set_properties(ppropvals))
			return ecError;
		return ecSuccess;
	}
	default:
		return ecNotSupported;
	}
}

uint32_t zarafa_server_getpropvals(GUID hsession,
	uint32_t hobject, const PROPTAG_ARRAY *pproptags,
	TPROPVAL_ARRAY *ppropvals)
{
	int i;
	uint8_t mapi_type;
	PROPTAG_ARRAY proptags;
	
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto pobject = pinfo->ptree->get_object<void>(hobject, &mapi_type);
	if (pobject == nullptr)
		return ecNullObject;
	switch (mapi_type) {
	case ZMG_PROFPROPERTY:
		if (NULL == pproptags) {
			*ppropvals = *(TPROPVAL_ARRAY*)pobject;
		} else {
			ppropvals->count = 0;
			ppropvals->ppropval = cu_alloc<TAGGED_PROPVAL>(pproptags->count);
			if (ppropvals->ppropval == nullptr)
				return ecError;
			for (i=0; i<pproptags->count; i++) {
				ppropvals->ppropval[ppropvals->count].proptag =
										pproptags->pproptag[i];
				ppropvals->ppropval[ppropvals->count].pvalue =
					tpropval_array_get_propval(static_cast<TPROPVAL_ARRAY *>(pobject), pproptags->pproptag[i]);
				if (ppropvals->ppropval[ppropvals->count].pvalue != nullptr)
					ppropvals->count ++;	
			}
		}
		return ecSuccess;
	case ZMG_STORE: {
		auto store = static_cast<STORE_OBJECT *>(pobject);
		if (NULL == pproptags) {
			if (!store->get_all_proptags(&proptags))
				return ecError;
			pproptags = &proptags;
		}
		if (!store->get_properties(pproptags, ppropvals))
			return ecError;
		return ecSuccess;
	}
	case ZMG_FOLDER: {
		auto folder = static_cast<FOLDER_OBJECT *>(pobject);
		if (NULL == pproptags) {
			if (!folder->get_all_proptags(&proptags))
				return ecError;
			pproptags = &proptags;
		}
		if (!folder->get_properties(pproptags, ppropvals))
			return ecError;
		return ecSuccess;
	}
	case ZMG_MESSAGE: {
		auto msg = static_cast<MESSAGE_OBJECT *>(pobject);
		if (NULL == pproptags) {
			if (!msg->get_all_proptags(&proptags))
				return ecError;
			pproptags = &proptags;
		}
		if (!msg->get_properties(pproptags, ppropvals))
			return ecError;
		return ecSuccess;
	}
	case ZMG_ATTACH: {
		auto atx = static_cast<ATTACHMENT_OBJECT *>(pobject);
		if (NULL == pproptags) {
			if (!atx->get_all_proptags(&proptags))
				return ecError;
			pproptags = &proptags;
		}
		if (!atx->get_properties(pproptags, ppropvals))
			return ecError;
		return ecSuccess;
	}
	case ZMG_ABCONT:
		if (NULL == pproptags) {
			container_object_get_container_table_all_proptags(
				&proptags);
			pproptags = &proptags;
		}
		if (!static_cast<CONTAINER_OBJECT *>(pobject)->get_properties(pproptags, ppropvals))
			return ecError;
		return ecSuccess;
	case ZMG_MAILUSER:
	case ZMG_DISTLIST:
		if (NULL == pproptags) {
			container_object_get_user_table_all_proptags(&proptags);
			pproptags = &proptags;
		}
		if (!static_cast<USER_OBJECT *>(pobject)->get_properties(pproptags, ppropvals))
			return ecError;
		return ecSuccess;
	default:
		return ecNotSupported;
	}
}

uint32_t zarafa_server_deletepropvals(GUID hsession,
	uint32_t hobject, const PROPTAG_ARRAY *pproptags)
{
	int i;
	uint8_t mapi_type;
	uint32_t permission;
	
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto pobject = pinfo->ptree->get_object<void>(hobject, &mapi_type);
	if (pobject == nullptr)
		return ecNullObject;
	switch (mapi_type) {
	case ZMG_PROFPROPERTY:
		for (i=0; i<pproptags->count; i++) {
			tpropval_array_remove_propval(static_cast<TPROPVAL_ARRAY *>(pobject), pproptags->pproptag[i]);
		}
		pinfo->ptree->touch_profile_sec();
		return ecSuccess;
	case ZMG_STORE: {
		auto store = static_cast<STORE_OBJECT *>(pobject);
		if (!store->check_owner_mode())
			return ecAccessDenied;
		if (!store->remove_properties(pproptags))
			return ecError;
		return ecSuccess;
	}
	case ZMG_FOLDER: {
		auto folder = static_cast<FOLDER_OBJECT *>(pobject);
		auto pstore = folder->pstore;
		if (!pstore->check_owner_mode()) {
			if (!exmdb_client::check_folder_permission(pstore->get_dir(),
			    folder->folder_id, pinfo->get_username(), &permission))
				return ecError;
			if (!(permission & frightsOwner))
				return ecAccessDenied;
		}
		if (!folder->remove_properties(pproptags))
			return ecError;
		return ecSuccess;
	}
	case ZMG_MESSAGE: {
		auto msg = static_cast<MESSAGE_OBJECT *>(pobject);
		if (!msg->check_writable())
			return ecAccessDenied;
		if (!msg->remove_properties(pproptags))
			return ecError;
		return ecSuccess;
	}
	case ZMG_ATTACH: {
		auto atx = static_cast<ATTACHMENT_OBJECT *>(pobject);
		if (!atx->check_writable())
			return ecAccessDenied;
		if (!atx->remove_properties(pproptags))
			return ecError;
		return ecSuccess;
	}
	default:
		return ecNotSupported;
	}
}

uint32_t zarafa_server_setmessagereadflag(
	GUID hsession, uint32_t hmessage, uint32_t flags)
{
	BOOL b_changed;
	uint8_t mapi_type;
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto pmessage = pinfo->ptree->get_object<MESSAGE_OBJECT>(hmessage, &mapi_type);
	if (pmessage == nullptr)
		return ecNullObject;
	if (mapi_type != ZMG_MESSAGE)
		return ecNotSupported;
	return pmessage->set_readflag(flags, &b_changed) ? ecSuccess : ecError;
}

uint32_t zarafa_server_openembedded(GUID hsession,
	uint32_t hattachment, uint32_t flags, uint32_t *phobject)
{
	uint8_t mapi_type;
	
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto pattachment = pinfo->ptree->get_object<ATTACHMENT_OBJECT>(hattachment, &mapi_type);
	if (pattachment == nullptr)
		return ecNullObject;
	if (mapi_type != ZMG_ATTACH)
		return ecNotSupported;
	auto pstore = pattachment->get_store();
	auto hstore = pinfo->ptree->get_store_handle(pstore->b_private, pstore->account_id);
	if (hstore == INVALID_HANDLE)
		return ecNullObject;
	auto b_writable = pattachment->check_writable();
	auto tag_access = pattachment->get_tag_access();
	if ((flags & FLAG_CREATE) && !b_writable)
		return ecAccessDenied;
	auto pmessage = message_object_create(pstore,
		FALSE, pinfo->cpid, 0, pattachment,
		tag_access, b_writable, NULL);
	if (pmessage == nullptr)
		return ecError;
	if (pmessage->get_instance_id() == 0) {
		if (0 == (FLAG_CREATE & flags)) {
			return ecNotFound;
		}
		if (!b_writable)
			return ecAccessDenied;
		pmessage = message_object_create(pstore, TRUE,
			pinfo->cpid, 0, pattachment, tag_access,
			TRUE, NULL);
		if (pmessage == nullptr)
			return ecError;
		if (!pmessage->init_message(false, pinfo->cpid))
			return ecError;
	}
	/* add the store handle as the parent object handle
		because the caller normaly will not keep the
		handle of attachment */
	*phobject = pinfo->ptree->add_object_handle(hstore, ZMG_MESSAGE, pmessage.get());
	if (*phobject == INVALID_HANDLE)
		return ecError;
	pmessage.release();
	return ecSuccess;
}

uint32_t zarafa_server_getnamedpropids(GUID hsession, uint32_t hstore,
	const PROPNAME_ARRAY *ppropnames, PROPID_ARRAY *ppropids)
{
	uint8_t mapi_type;
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto pstore = pinfo->ptree->get_object<STORE_OBJECT>(hstore, &mapi_type);
	if (pstore == nullptr)
		return ecNullObject;
	if (mapi_type != ZMG_STORE)
		return ecNotSupported;
	return pstore->get_named_propids(TRUE, ppropnames, ppropids) ?
	       ecSuccess : ecError;
}

uint32_t zarafa_server_getpropnames(GUID hsession, uint32_t hstore,
	const PROPID_ARRAY *ppropids, PROPNAME_ARRAY *ppropnames)
{
	uint8_t mapi_type;
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto pstore = pinfo->ptree->get_object<STORE_OBJECT>(hstore, &mapi_type);
	if (pstore == nullptr)
		return ecNullObject;
	if (mapi_type != ZMG_STORE)
		return ecNotSupported;
	return pstore->get_named_propnames(ppropids, ppropnames) ?
	       ecSuccess : ecError;
}

uint32_t zarafa_server_copyto(GUID hsession, uint32_t hsrcobject,
	const PROPTAG_ARRAY *pexclude_proptags, uint32_t hdstobject,
	uint32_t flags)
{
	int i;
	BOOL b_cycle;
	BOOL b_collid;
	BOOL b_partial;
	uint8_t dst_type;
	uint8_t mapi_type;
	uint32_t permission;
	PROPTAG_ARRAY proptags;
	PROPTAG_ARRAY proptags1;
	TPROPVAL_ARRAY propvals;
	PROPTAG_ARRAY tmp_proptags;
	
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto pobject = pinfo->ptree->get_object<void>(hsrcobject, &mapi_type);
	if (pobject == nullptr)
		return ecNullObject;
	auto pobject_dst = pinfo->ptree->get_object<void>(hdstobject, &dst_type);
	if (pobject_dst == nullptr)
		return ecNullObject;
	if (mapi_type != dst_type) {
		return ecNotSupported;
	}
	BOOL b_force = (flags & COPY_FLAG_NOOVERWRITE) ? TRUE : false;
	switch (mapi_type) {
	case ZMG_FOLDER: {
		auto folder = static_cast<FOLDER_OBJECT *>(pobject);
		auto fdst = static_cast<FOLDER_OBJECT *>(pobject_dst);
		auto pstore = folder->pstore;
		if (pstore != fdst->pstore)
			return ecNotSupported;
		/* MS-OXCPRPT 3.2.5.8, public folder not supported */
		if (!pstore->b_private)
			return ecNotSupported;
		const char *username = nullptr;
		if (!pstore->check_owner_mode()) {
			if (!exmdb_client::check_folder_permission(pstore->get_dir(),
			    folder->folder_id, pinfo->get_username(), &permission))
				return ecError;
			if (permission & frightsOwner) {
				username = NULL;
			} else {
				if (!(permission & frightsReadAny))
					return ecAccessDenied;
				username = pinfo->get_username();
			}
			if (!exmdb_client::check_folder_permission(pstore->get_dir(),
			    fdst->folder_id, pinfo->get_username(), &permission))
				return ecError;
			if (!(permission & frightsOwner))
				return ecAccessDenied;
		}
		BOOL b_sub;
		if (common_util_index_proptags(pexclude_proptags,
			PROP_TAG_CONTAINERHIERARCHY) < 0) {
			if (!exmdb_client::check_folder_cycle(pstore->get_dir(),
			    folder->folder_id, fdst->folder_id, &b_cycle))
				return ecError;
			if (b_cycle)
				return MAPI_E_FOLDER_CYCLE;
			b_sub = TRUE;
		} else {
			b_sub = FALSE;
		}
		BOOL b_normal = common_util_index_proptags(pexclude_proptags, PROP_TAG_CONTAINERCONTENTS) < 0 ? TRUE : false;
		BOOL b_fai    = common_util_index_proptags(pexclude_proptags, PROP_TAG_FOLDERASSOCIATEDCONTENTS) < 0 ? TRUE : false;
		if (!static_cast<FOLDER_OBJECT *>(pobject)->get_all_proptags(&proptags))
			return ecError;
		common_util_reduce_proptags(&proptags, pexclude_proptags);
		tmp_proptags.count = 0;
		tmp_proptags.pproptag = cu_alloc<uint32_t>(proptags.count);
		if (tmp_proptags.pproptag == nullptr)
			return ecError;
		if (!b_force && !fdst->get_all_proptags(&proptags1))
			return ecError;
		for (i=0; i<proptags.count; i++) {
			if (fdst->check_readonly_property(proptags.pproptag[i]))
				continue;
			if (!b_force && common_util_index_proptags(&proptags1,
			    proptags.pproptag[i]) >= 0)
				continue;
			tmp_proptags.pproptag[tmp_proptags.count] = 
									proptags.pproptag[i];
			tmp_proptags.count ++;
		}
		if (!folder->get_properties(&tmp_proptags, &propvals))
			return ecError;
		if (TRUE == b_sub || TRUE == b_normal || TRUE == b_fai) {
			BOOL b_guest = username == nullptr ? false : TRUE;
			if (!exmdb_client::copy_folder_internal(pstore->get_dir(),
			    pstore->account_id, pinfo->cpid, b_guest,
			    pinfo->get_username(), folder->folder_id,
			    b_normal, b_fai, b_sub, fdst->folder_id,
			    &b_collid, &b_partial))
				return ecError;
			if (b_collid)
				return ecDuplicateName;
			if (!fdst->set_properties(&propvals))
				return ecError;
			return ecSuccess;
		}
		if (!fdst->set_properties(&propvals))
			return ecError;
		return ecSuccess;
	}
	case ZMG_MESSAGE: {
		auto mdst = static_cast<MESSAGE_OBJECT *>(pobject_dst);
		if (!mdst->check_writable())
			return ecAccessDenied;
		if (!mdst->copy_to(static_cast<MESSAGE_OBJECT *>(pobject),
		    pexclude_proptags, b_force, &b_cycle))
			return ecError;
		return b_cycle ? ecMsgCycle : ecSuccess;
	}
	case ZMG_ATTACH: {
		auto adst = static_cast<ATTACHMENT_OBJECT *>(pobject_dst);
		if (!adst->check_writable())
			return ecAccessDenied;
		if (!adst->copy_properties(static_cast<ATTACHMENT_OBJECT *>(pobject),
		    pexclude_proptags, b_force, &b_cycle))
			return ecError;
		return b_cycle ? ecMsgCycle : ecSuccess;
	}
	default:
		return ecNotSupported;
	}
}

uint32_t zarafa_server_savechanges(GUID hsession, uint32_t hobject)
{
	BOOL b_touched;
	uint8_t mapi_type;
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto pobject = pinfo->ptree->get_object<void>(hobject, &mapi_type);
	if (pobject == nullptr)
		return ecNullObject;
	if (mapi_type == ZMG_MESSAGE) {
		auto msg = static_cast<MESSAGE_OBJECT *>(pobject);
		if (!msg->check_writable())
			return ecAccessDenied;
		if (!msg->check_orignal_touched(&b_touched))
			return ecError;
		if (b_touched)
			return ecObjectModified;
		auto err = msg->save();
		if (err != GXERR_SUCCESS)
			return gxerr_to_hresult(err);
		return ecSuccess;
	} else if (mapi_type == ZMG_ATTACH) {
		auto atx = static_cast<ATTACHMENT_OBJECT *>(pobject);
		if (!atx->check_writable())
			return ecAccessDenied;
		auto err = atx->save();
		if (err != GXERR_SUCCESS)
			return gxerr_to_hresult(err);
		return ecSuccess;
	} else {
		return ecNotSupported;
	}
}

uint32_t zarafa_server_hierarchysync(GUID hsession,
	uint32_t hfolder, uint32_t *phobject)
{
	uint8_t mapi_type;
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto pfolder = pinfo->ptree->get_object<FOLDER_OBJECT>(hfolder, &mapi_type);
	if (pfolder == nullptr)
		return ecNullObject;
	if (mapi_type != ZMG_FOLDER)
		return ecNotSupported;
	auto pstore = pfolder->pstore;
	auto hstore = pinfo->ptree->get_store_handle(pstore->b_private, pstore->account_id);
	if (hstore == INVALID_HANDLE)
		return ecNullObject;
	auto pctx = icsdownctx_object_create(pfolder, SYNC_TYPE_HIERARCHY);
	if (pctx == nullptr)
		return ecError;
	*phobject = pinfo->ptree->add_object_handle(hstore, ZMG_ICSDOWNCTX, pctx.get());
	if (*phobject == INVALID_HANDLE)
		return ecError;
	pctx.release();
	return ecSuccess;
}

uint32_t zarafa_server_contentsync(GUID hsession,
	uint32_t hfolder, uint32_t *phobject)
{
	uint8_t mapi_type;
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto pfolder = pinfo->ptree->get_object<FOLDER_OBJECT>(hfolder, &mapi_type);
	if (pfolder == nullptr)
		return ecNullObject;
	if (mapi_type != ZMG_FOLDER)
		return ecNotSupported;
	auto pstore = pfolder->pstore;
	auto hstore = pinfo->ptree->get_store_handle(pstore->b_private, pstore->account_id);
	if (hstore == INVALID_HANDLE)
		return ecNullObject;
	auto pctx = icsdownctx_object_create(pfolder, SYNC_TYPE_CONTENTS);
	if (pctx == nullptr)
		return ecError;
	*phobject = pinfo->ptree->add_object_handle(hstore, ZMG_ICSDOWNCTX, pctx.get());
	if (*phobject == INVALID_HANDLE)
		return ecError;
	pctx.release();
	return ecSuccess;
}

uint32_t zarafa_server_configsync(GUID hsession, uint32_t hctx, uint32_t flags,
    const BINARY *pstate, const RESTRICTION *prestriction, uint8_t *pb_changed,
    uint32_t *pcount)
{
	uint8_t mapi_type;
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto pctx = pinfo->ptree->get_object<ICSDOWNCTX_OBJECT>(hctx, &mapi_type);
	if (pctx == nullptr)
		return ecNullObject;
	if (mapi_type != ZMG_ICSDOWNCTX)
		return ecNotSupported;
	BOOL b_changed = false;
	if (pctx->get_type() == SYNC_TYPE_CONTENTS) {
		if (!pctx->make_content(pstate, prestriction, flags, &b_changed, pcount))
			return ecError;
	} else {
		if (!pctx->make_hierarchy(pstate, flags, &b_changed, pcount))
			return ecError;
	}
	*pb_changed = !!b_changed;
	return ecSuccess;
}

uint32_t zarafa_server_statesync(GUID hsession,
	uint32_t hctx, BINARY *pstate)
{
	uint8_t mapi_type;
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto pctx = pinfo->ptree->get_object<ICSDOWNCTX_OBJECT>(hctx, &mapi_type);
	if (pctx == nullptr)
		return ecNullObject;
	if (mapi_type != ZMG_ICSDOWNCTX)
		return ecNotSupported;
	auto pbin = pctx->get_state();
	if (pbin == nullptr)
		return ecError;
	*pstate = *pbin;
	return ecSuccess;
}

uint32_t zarafa_server_syncmessagechange(GUID hsession, uint32_t hctx,
    uint8_t *pb_new, TPROPVAL_ARRAY *pproplist)
{
	BOOL b_found;
	uint8_t mapi_type;
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto pctx = pinfo->ptree->get_object<ICSDOWNCTX_OBJECT>(hctx, &mapi_type);
	if (pctx == nullptr)
		return ecNullObject;
	if (mapi_type != ZMG_ICSDOWNCTX || pctx->get_type() != SYNC_TYPE_CONTENTS)
		return ecNotSupported;
	BOOL b_new = false;
	if (!pctx->sync_message_change(&b_found, &b_new, pproplist))
		return ecError;
	*pb_new = !!b_new;
	return b_found ? ecSuccess : ecNotFound;
}

uint32_t zarafa_server_syncfolderchange(GUID hsession,
	uint32_t hctx, TPROPVAL_ARRAY *pproplist)
{
	BOOL b_found;
	uint8_t mapi_type;
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto pctx = pinfo->ptree->get_object<ICSDOWNCTX_OBJECT>(hctx, &mapi_type);
	if (pctx == nullptr)
		return ecNullObject;
	if (mapi_type != ZMG_ICSDOWNCTX || pctx->get_type() != SYNC_TYPE_HIERARCHY)
		return ecNotSupported;
	if (!pctx->sync_folder_change(&b_found, pproplist))
		return ecError;
	return b_found ? ecSuccess : ecNotFound;
}

uint32_t zarafa_server_syncreadstatechanges(
	GUID hsession, uint32_t hctx, STATE_ARRAY *pstates)
{
	uint8_t mapi_type;
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto pctx = pinfo->ptree->get_object<ICSDOWNCTX_OBJECT>(hctx, &mapi_type);
	if (pctx == nullptr)
		return ecNullObject;
	if (mapi_type != ZMG_ICSDOWNCTX || pctx->get_type() != SYNC_TYPE_CONTENTS)
		return ecNotSupported;
	return pctx->sync_readstates(pstates) ? ecSuccess : ecError;
}

uint32_t zarafa_server_syncdeletions(GUID hsession,
	uint32_t hctx, uint32_t flags, BINARY_ARRAY *pbins)
{
	uint8_t mapi_type;
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto pctx = pinfo->ptree->get_object<ICSDOWNCTX_OBJECT>(hctx, &mapi_type);
	if (pctx == nullptr)
		return ecNullObject;
	if (mapi_type != ZMG_ICSDOWNCTX)
		return ecNotSupported;
	return pctx->sync_deletions(flags, pbins) ? ecSuccess : ecError;
}

uint32_t zarafa_server_hierarchyimport(GUID hsession,
	uint32_t hfolder, uint32_t *phobject)
{
	uint8_t mapi_type;
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto pfolder = pinfo->ptree->get_object<FOLDER_OBJECT>(hfolder, &mapi_type);
	if (pfolder == nullptr)
		return ecNullObject;
	if (mapi_type != ZMG_FOLDER || pfolder->type == FOLDER_TYPE_SEARCH)
		return ecNotSupported;
	auto pstore = pfolder->pstore;
	auto hstore = pinfo->ptree->get_store_handle(pstore->b_private, pstore->account_id);
	if (hstore == INVALID_HANDLE)
		return ecNullObject;
	auto pctx = icsupctx_object_create(pfolder, SYNC_TYPE_HIERARCHY);
	if (pctx == nullptr)
		return ecError;
	*phobject = pinfo->ptree->add_object_handle(hstore, ZMG_ICSUPCTX, pctx.get());
	if (*phobject == INVALID_HANDLE)
		return ecError;
	pctx.release();
	return ecSuccess;
}

uint32_t zarafa_server_contentimport(GUID hsession,
	uint32_t hfolder, uint32_t *phobject)
{
	uint8_t mapi_type;
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto pfolder = pinfo->ptree->get_object<FOLDER_OBJECT>(hfolder, &mapi_type);
	if (pfolder == nullptr)
		return ecNullObject;
	if (mapi_type != ZMG_FOLDER)
		return ecNotSupported;
	auto pstore = pfolder->pstore;
	auto hstore = pinfo->ptree->get_store_handle(pstore->b_private, pstore->account_id);
	if (hstore == INVALID_HANDLE)
		return ecNullObject;
	auto pctx = icsupctx_object_create(pfolder, SYNC_TYPE_CONTENTS);
	if (pctx == nullptr)
		return ecError;
	*phobject = pinfo->ptree->add_object_handle(hstore, ZMG_ICSUPCTX, pctx.get());
	if (*phobject == INVALID_HANDLE)
		return ecError;
	pctx.release();
	return ecSuccess;
}

uint32_t zarafa_server_configimport(GUID hsession,
	uint32_t hctx, uint8_t sync_type, const BINARY *pstate)
{
	uint8_t mapi_type;
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto pctx = pinfo->ptree->get_object<ICSUPCTX_OBJECT>(hctx, &mapi_type);
	if (pctx == nullptr)
		return ecNullObject;
	if (mapi_type != ZMG_ICSUPCTX)
		return ecNotSupported;
	return pctx->upload_state(pstate) ? ecSuccess : ecError;
}

uint32_t zarafa_server_stateimport(GUID hsession,
	uint32_t hctx, BINARY *pstate)
{
	uint8_t mapi_type;
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto pctx = pinfo->ptree->get_object<ICSUPCTX_OBJECT>(hctx, &mapi_type);
	if (pctx == nullptr)
		return ecNullObject;
	if (mapi_type != ZMG_ICSUPCTX)
		return ecNotSupported;
	auto pbin = pctx->get_state();
	if (pbin == nullptr)
		return ecError;
	*pstate = *pbin;
	return ecSuccess;
}

uint32_t zarafa_server_importmessage(GUID hsession, uint32_t hctx,
	uint32_t flags, const TPROPVAL_ARRAY *pproplist, uint32_t *phobject)
{
	BOOL b_fai;
	XID tmp_xid;
	BOOL b_exist;
	BOOL b_owner;
	BINARY *pbin;
	void *pvalue;
	uint8_t mapi_type;
	uint64_t message_id;
	uint32_t permission = rightsNone, tag_access = 0;
	
	pvalue = common_util_get_propvals(pproplist, PROP_TAG_ASSOCIATED);
	if (NULL != pvalue) {
		b_fai = *static_cast<uint8_t *>(pvalue) == 0 ? TRUE : false;
	} else {
		pvalue = common_util_get_propvals(pproplist, PR_MESSAGE_FLAGS);
		b_fai = pvalue != nullptr && (*static_cast<uint32_t *>(pvalue) & MSGFLAG_ASSOCIATED) ?
		        TRUE : false;
	}
	/*
	 * If there is no sourcekey, it is a new message. That is how
	 * grommunio-sync creates new items coming from mobile devices.
	 */
	pbin = static_cast<BINARY *>(common_util_get_propvals(pproplist, PR_SOURCE_KEY));
	if (pbin == nullptr)
		flags |= SYNC_NEW_MESSAGE;
	BOOL b_new = (flags & SYNC_NEW_MESSAGE) ? TRUE : false;
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto pctx = pinfo->ptree->get_object<ICSUPCTX_OBJECT>(hctx, &mapi_type);
	if (pctx == nullptr)
		return ecNullObject;
	if (mapi_type != ZMG_ICSUPCTX)
		return ecNotSupported;
	auto pstore = pctx->get_store();
	if (pctx->get_type() != SYNC_TYPE_CONTENTS)
		return ecNotSupported;
	auto folder_id = pctx->get_parent_folder_id();
	if (FALSE == b_new) {
		pbin = static_cast<BINARY *>(common_util_get_propvals(pproplist, PR_SOURCE_KEY));
		if (pbin == nullptr || pbin->cb != 22) {
			return ecInvalidParam;
		}
		if (FALSE == common_util_binary_to_xid(pbin, &tmp_xid)) {
			return ecError;
		}
		auto tmp_guid = pstore->guid();
		if (0 != guid_compare(&tmp_guid, &tmp_xid.guid)) {
			return ecInvalidParam;
		}
		message_id = rop_util_make_eid(1, tmp_xid.local_id);
		if (!exmdb_client::check_message(pstore->get_dir(), folder_id,
		    message_id, &b_exist))
			return ecError;
		if (!b_exist)
			return ecNotFound;
	}
	if (!pstore->check_owner_mode()) {
		if (!exmdb_client::check_folder_permission(pstore->get_dir(),
		    folder_id, pinfo->get_username(), &permission))
			return ecError;
		if (TRUE == b_new) {
			if (!(permission & frightsCreate))
				return ecAccessDenied;
			tag_access = TAG_ACCESS_READ;
			if (permission & (frightsEditAny | frightsEditOwned))
				tag_access |= TAG_ACCESS_MODIFY;	
			if (permission & (frightsDeleteAny | frightsDeleteOwned))
				tag_access |= TAG_ACCESS_DELETE;	
		} else {
			if (permission & frightsOwner) {
				tag_access = TAG_ACCESS_MODIFY|
					TAG_ACCESS_READ|TAG_ACCESS_DELETE;
			} else {
				if (!exmdb_client_check_message_owner(pstore->get_dir(),
				    message_id, pinfo->get_username(), &b_owner))
					return ecError;
				if (b_owner || (permission & frightsReadAny))
					tag_access |= TAG_ACCESS_READ;
				if ((permission & frightsEditAny) ||
				    (b_owner && (permission & frightsEditOwned)))
					tag_access |= TAG_ACCESS_MODIFY;	
				if ((permission & frightsDeleteAny) ||
				    (b_owner && (permission & frightsDeleteOwned)))
					tag_access |= TAG_ACCESS_DELETE;	
			}
		}
	} else {
		tag_access = TAG_ACCESS_MODIFY|TAG_ACCESS_READ|TAG_ACCESS_DELETE;
	}
	if (FALSE == b_new) {
		if (!exmdb_client_get_message_property(pstore->get_dir(),
		    nullptr, 0, message_id, PROP_TAG_ASSOCIATED, &pvalue))
			return ecError;
		if (TRUE == b_fai) {
			if (pvalue == nullptr || *static_cast<uint8_t *>(pvalue) == 0)
				return ecInvalidParam;
		} else {
			if (pvalue != nullptr && *static_cast<uint8_t *>(pvalue) != 0)
				return ecInvalidParam;
		}
	} else {
		if (!exmdb_client::allocate_message_id(pstore->get_dir(),
		    folder_id, &message_id))
			return ecError;
	}
	auto pmessage = message_object_create(pstore, b_new,
		pinfo->cpid, message_id, &folder_id, tag_access,
		OPEN_MODE_FLAG_READWRITE, pctx->pstate);
	if (pmessage == nullptr)
		return ecError;
	if (b_new && !pmessage->init_message(b_fai, pinfo->cpid))
		return ecError;
	*phobject = pinfo->ptree->add_object_handle(hctx, ZMG_MESSAGE, pmessage.get());
	if (*phobject == INVALID_HANDLE)
		return ecError;
	pmessage.release();
	return ecSuccess;
}

uint32_t zarafa_server_importfolder(GUID hsession,
	uint32_t hctx, const TPROPVAL_ARRAY *ppropvals)
{
	int i;
	XID tmp_xid;
	BOOL b_exist;
	BINARY *pbin;
	BOOL b_guest;
	BOOL b_found;
	void *pvalue;
	int domain_id;
	BOOL b_partial;
	uint64_t nttime;
	uint16_t replid;
	uint64_t tmp_fid;
	uint8_t mapi_type;
	uint32_t tmp_type;
	uint64_t folder_id;
	uint64_t parent_id;
	uint64_t parent_id1;
	uint64_t change_num;
	uint32_t permission;
	TPROPVAL_ARRAY *pproplist;
	PROBLEM_ARRAY tmp_problems;
	TPROPVAL_ARRAY tmp_propvals;
	TAGGED_PROPVAL propval_buff[4];
	TPROPVAL_ARRAY hierarchy_propvals;
	
	pproplist = &hierarchy_propvals;
	hierarchy_propvals.count = 4;
	hierarchy_propvals.ppropval = propval_buff;
	propval_buff[0].proptag = PR_PARENT_SOURCE_KEY;
	propval_buff[0].pvalue = common_util_get_propvals(ppropvals, PR_PARENT_SOURCE_KEY);
	if (propval_buff[0].pvalue == nullptr)
		return ecInvalidParam;
	propval_buff[1].proptag = PR_SOURCE_KEY;
	propval_buff[1].pvalue = common_util_get_propvals(ppropvals, PR_SOURCE_KEY);
	if (propval_buff[1].pvalue == nullptr)
		return ecInvalidParam;
	propval_buff[2].proptag = PR_LAST_MODIFICATION_TIME;
	propval_buff[2].pvalue = common_util_get_propvals(ppropvals, PR_LAST_MODIFICATION_TIME);
	if (NULL == propval_buff[2].pvalue) {
		propval_buff[2].pvalue = &nttime;
		nttime = rop_util_current_nttime();
	}
	propval_buff[3].proptag = PR_DISPLAY_NAME;
	propval_buff[3].pvalue = common_util_get_propvals(ppropvals, PR_DISPLAY_NAME);
	if (propval_buff[3].pvalue == nullptr)
		return ecInvalidParam;
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto pctx = pinfo->ptree->get_object<ICSUPCTX_OBJECT>(hctx, &mapi_type);
	if (pctx == nullptr)
		return ecNullObject;
	if (mapi_type != ZMG_ICSUPCTX)
		return ecNotSupported;
	auto pstore = pctx->get_store();
	if (pctx->get_type() != SYNC_TYPE_HIERARCHY)
		return ecNotSupported;
	if (0 == ((BINARY*)pproplist->ppropval[0].pvalue)->cb) {
		parent_id1 = pctx->get_parent_folder_id();
		if (!exmdb_client::check_folder_id(pstore->get_dir(),
		    parent_id1, &b_exist))
			return ecError;
		if (!b_exist)
			return SYNC_E_NO_PARENT;
	} else {
		pbin = static_cast<BINARY *>(pproplist->ppropval[0].pvalue);
		if (pbin == nullptr || pbin->cb != 22) {
			return ecInvalidParam;
		}
		if (FALSE == common_util_binary_to_xid(pbin, &tmp_xid)) {
			return ecError;
		}
		if (pstore->b_private) {
			auto tmp_guid = rop_util_make_user_guid(pstore->account_id);
			if (0 != guid_compare(&tmp_guid, &tmp_xid.guid)) {
				return ecInvalidParam;
			}
		} else {
			auto tmp_guid = rop_util_make_domain_guid(pstore->account_id);
			if (0 != guid_compare(&tmp_guid, &tmp_xid.guid)) {
				return ecAccessDenied;
			}
		}
		parent_id1 = rop_util_make_eid(1, tmp_xid.local_id);
		if (!exmdb_client_get_folder_property(pstore->get_dir(), 0,
		    parent_id1, PROP_TAG_FOLDERTYPE, &pvalue))
			return ecError;
		if (pvalue == nullptr)
			return SYNC_E_NO_PARENT;
	}
	pbin = static_cast<BINARY *>(pproplist->ppropval[1].pvalue);
	if (pbin == nullptr || pbin->cb != 22) {
		return ecInvalidParam;
	}
	if (FALSE == common_util_binary_to_xid(pbin, &tmp_xid)) {
		return ecError;
	}
	if (pstore->b_private) {
		auto tmp_guid = rop_util_make_user_guid(pstore->account_id);
		if (0 != guid_compare(&tmp_guid, &tmp_xid.guid)) {
			return ecInvalidParam;
		}
		folder_id = rop_util_make_eid(1, tmp_xid.local_id);
	} else {
		auto tmp_guid = rop_util_make_domain_guid(pstore->account_id);
		if (0 != guid_compare(&tmp_guid, &tmp_xid.guid)) {
			domain_id = rop_util_make_domain_id(tmp_xid.guid);
			if (-1 == domain_id) {
				return ecInvalidParam;
			}
			if (!system_services_check_same_org(domain_id, pstore->account_id))
				return ecInvalidParam;
			if (!exmdb_client::get_mapping_replid(pstore->get_dir(),
			    tmp_xid.guid, &b_found, &replid))
				return ecError;
			if (!b_found)
				return ecInvalidParam;
			folder_id = rop_util_make_eid(replid, tmp_xid.local_id);
		} else {
			folder_id = rop_util_make_eid(1, tmp_xid.local_id);
		}
	}
	if (!exmdb_client::check_folder_id(pstore->get_dir(), folder_id, &b_exist))
		return ecError;
	if (FALSE == b_exist) {
		if (!pstore->check_owner_mode()) {
			if (!exmdb_client::check_folder_permission(pstore->get_dir(),
			    parent_id1, pinfo->get_username(), &permission))
				return ecError;
			if (!(permission & frightsCreateSubfolder))
				return ecAccessDenied;
		}
		if (!exmdb_client::get_folder_by_name(pstore->get_dir(),
		    parent_id1, static_cast<char *>(pproplist->ppropval[3].pvalue),
		    &tmp_fid)) {
			return ecError;
		}
		if (0 != tmp_fid) {
			return ecDuplicateName;
		}
		if (!exmdb_client::allocate_cn(pstore->get_dir(), &change_num))
			return ecError;
		tmp_propvals.count = 0;
		tmp_propvals.ppropval = cu_alloc<TAGGED_PROPVAL>(8 + ppropvals->count);
		if (tmp_propvals.ppropval == nullptr)
			return ecError;
		tmp_propvals.ppropval[0].proptag = PROP_TAG_FOLDERID;
		tmp_propvals.ppropval[0].pvalue = &folder_id;
		tmp_propvals.ppropval[1].proptag = PROP_TAG_PARENTFOLDERID;
		tmp_propvals.ppropval[1].pvalue = &parent_id1;
		tmp_propvals.ppropval[2].proptag = PR_LAST_MODIFICATION_TIME;
		tmp_propvals.ppropval[2].pvalue = pproplist->ppropval[2].pvalue;
		tmp_propvals.ppropval[3].proptag = PR_DISPLAY_NAME;
		tmp_propvals.ppropval[3].pvalue = pproplist->ppropval[3].pvalue;
		tmp_propvals.ppropval[4].proptag = PROP_TAG_CHANGENUMBER;
		tmp_propvals.ppropval[4].pvalue = &change_num;
		tmp_propvals.count = 5;
		for (i=0; i<ppropvals->count; i++) {
			tmp_propvals.ppropval[tmp_propvals.count] =
								ppropvals->ppropval[i];
			tmp_propvals.count ++;
		}
		if (NULL == common_util_get_propvals(
			&tmp_propvals, PROP_TAG_FOLDERTYPE)) {
			tmp_type = FOLDER_TYPE_GENERIC;
			tmp_propvals.ppropval[tmp_propvals.count].proptag =
											PROP_TAG_FOLDERTYPE;
			tmp_propvals.ppropval[tmp_propvals.count].pvalue =
													&tmp_type;
			tmp_propvals.count ++;
		}
		if (!exmdb_client::create_folder_by_properties(pstore->get_dir(),
		    pinfo->cpid, &tmp_propvals, &tmp_fid) || folder_id != tmp_fid)
			return ecError;
		idset_append(pctx->pstate->pseen, change_num);
		return ecSuccess;
	}
	if (!pstore->check_owner_mode()) {
		if (!exmdb_client::check_folder_permission(pstore->get_dir(),
		    folder_id, pinfo->get_username(), &permission))
			return ecError;
		if (!(permission & frightsOwner))
			return ecAccessDenied;
	}
	if (!exmdb_client_get_folder_property(pstore->get_dir(), 0, folder_id,
	    PROP_TAG_PARENTFOLDERID, &pvalue) || pvalue == nullptr)
		return ecError;
	parent_id = *(uint64_t*)pvalue;
	if (parent_id != parent_id1) {
		/* MS-OXCFXICS 3.3.5.8.8 move folders
		within public mailbox is not supported */
		if (!pstore->b_private)
			return ecNotSupported;
		if (rop_util_get_gc_value(folder_id) < PRIVATE_FID_CUSTOM) {
			return ecAccessDenied;
		}
		if (!pstore->check_owner_mode()) {
			if (!exmdb_client::check_folder_permission(pstore->get_dir(),
			    parent_id1, pinfo->get_username(), &permission))
				return ecError;
			if (!(permission & frightsCreateSubfolder))
				return ecAccessDenied;
			b_guest = TRUE;
		} else {
			b_guest = FALSE;
		}
		if (!exmdb_client::movecopy_folder(pstore->get_dir(),
		    pstore->account_id, pinfo->cpid, b_guest,
		    pinfo->get_username(), parent_id, folder_id, parent_id1,
		    static_cast<char *>(pproplist->ppropval[3].pvalue), false,
		    &b_exist, &b_partial)) {
			return ecError;
		}
		if (b_exist)
			return ecDuplicateName;
		if (b_partial)
			return ecError;
	}
	if (!exmdb_client::allocate_cn(pstore->get_dir(), &change_num))
		return ecError;
	tmp_propvals.count = 0;
	tmp_propvals.ppropval = cu_alloc<TAGGED_PROPVAL>(5 + ppropvals->count);
	if (tmp_propvals.ppropval == nullptr)
		return ecError;
	tmp_propvals.ppropval[0].proptag = PR_LAST_MODIFICATION_TIME;
	tmp_propvals.ppropval[0].pvalue = pproplist->ppropval[2].pvalue;
	tmp_propvals.ppropval[1].proptag = PR_DISPLAY_NAME;
	tmp_propvals.ppropval[1].pvalue = pproplist->ppropval[3].pvalue;
	tmp_propvals.ppropval[2].proptag = PROP_TAG_CHANGENUMBER;
	tmp_propvals.ppropval[2].pvalue = &change_num;
	tmp_propvals.count = 3;
	for (i=0; i<ppropvals->count; i++) {
		tmp_propvals.ppropval[tmp_propvals.count] =
							ppropvals->ppropval[i];
		tmp_propvals.count ++;
	}
	if (!exmdb_client::set_folder_properties(pstore->get_dir(),
	    pinfo->cpid, folder_id, &tmp_propvals, &tmp_problems))
		return ecError;
	idset_append(pctx->pstate->pseen, change_num);
	return ecSuccess;
}

uint32_t zarafa_server_importdeletion(GUID hsession,
	uint32_t hctx, uint32_t flags, const BINARY_ARRAY *pbins)
{
	XID tmp_xid;
	void *pvalue;
	BOOL b_exist;
	BOOL b_found;
	uint64_t eid;
	BOOL b_owner;
	int domain_id;
	BOOL b_result;
	BOOL b_partial;
	uint16_t replid;
	uint8_t mapi_type;
	uint32_t permission;
	EID_ARRAY message_ids;
	
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto pctx = pinfo->ptree->get_object<ICSUPCTX_OBJECT>(hctx, &mapi_type);
	if (pctx == nullptr)
		return ecNullObject;
	if (mapi_type != ZMG_ICSUPCTX)
		return ecNotSupported;
	auto pstore = pctx->get_store();
	auto sync_type = pctx->get_type();
	BOOL b_hard = (flags & SYNC_DELETES_FLAG_HARDDELETE) ? TRUE : false;
	if (flags & SYNC_DELETES_FLAG_HIERARCHY &&
	    sync_type == SYNC_TYPE_CONTENTS)
		return ecNotSupported;
	auto folder_id = pctx->get_parent_folder_id();
	auto username = pinfo->get_username();
	if (pstore->check_owner_mode()) {
		username = NULL;
	} else if (sync_type == SYNC_TYPE_CONTENTS &&
	    !exmdb_client::check_folder_permission(pstore->get_dir(),
	    folder_id, pinfo->get_username(), &permission)) {
		if (permission & (frightsOwner | frightsDeleteAny))
			username = NULL;
		else if (!(permission & frightsDeleteOwned))
			return ecAccessDenied;
	}
	if (SYNC_TYPE_CONTENTS == sync_type) {
		message_ids.count = 0;
		message_ids.pids = cu_alloc<uint64_t>(pbins->count);
		if (message_ids.pids == nullptr)
			return ecError;
	}
	for (size_t i = 0; i < pbins->count; ++i) {
		if (22 != pbins->pbin[i].cb) {
			return ecInvalidParam;
		}
		if (FALSE == common_util_binary_to_xid(
			pbins->pbin + i, &tmp_xid)) {
			return ecError;
		}
		if (pstore->b_private) {
			auto tmp_guid = rop_util_make_user_guid(pstore->account_id);
			if (0 != guid_compare(&tmp_guid, &tmp_xid.guid)) {
				return ecInvalidParam;
			}
			eid = rop_util_make_eid(1, tmp_xid.local_id);
		} else if (sync_type == SYNC_TYPE_CONTENTS) {
			auto tmp_guid = rop_util_make_domain_guid(pstore->account_id);
			if (0 != guid_compare(&tmp_guid, &tmp_xid.guid)) {
				return ecInvalidParam;
			}
			eid = rop_util_make_eid(1, tmp_xid.local_id);
		} else {
			auto tmp_guid = rop_util_make_domain_guid(pstore->account_id);
			if (0 != guid_compare(&tmp_guid, &tmp_xid.guid)) {
				domain_id = rop_util_make_domain_id(tmp_xid.guid);
				if (-1 == domain_id) {
					return ecInvalidParam;
				}
				if (!system_services_check_same_org(domain_id,
				    pstore->account_id))
					return ecInvalidParam;
				if (!exmdb_client::get_mapping_replid(pstore->get_dir(),
				    tmp_xid.guid, &b_found, &replid))
					return ecError;
				if (!b_found)
					return ecInvalidParam;
				eid = rop_util_make_eid(replid, tmp_xid.local_id);
			} else {
				eid = rop_util_make_eid(1, tmp_xid.local_id);
			}
		}
		if (SYNC_TYPE_CONTENTS == sync_type) {
			if (!exmdb_client::check_message(pstore->get_dir(),
			    folder_id, eid, &b_exist))
				return ecError;
		} else {
			if (!exmdb_client::check_folder_id(pstore->get_dir(),
			    eid, &b_exist))
				return ecError;
		}
		if (!b_exist)
			continue;
		if (NULL != username) {
			if (SYNC_TYPE_CONTENTS == sync_type) {
				if (!exmdb_client_check_message_owner(pstore->get_dir(),
				    eid, username, &b_owner))
					return ecError;
				if (!b_owner)
					return ecAccessDenied;
			} else if (!exmdb_client::check_folder_permission(pstore->get_dir(),
			    eid, username, &permission) && !(permission & frightsOwner)) {
				return ecAccessDenied;
			}
		}
		if (SYNC_TYPE_CONTENTS == sync_type) {
			message_ids.pids[message_ids.count] = eid;
			message_ids.count ++;
		} else {
			if (pstore->b_private) {
				if (!exmdb_client_get_folder_property(pstore->get_dir(),
				    0, eid, PROP_TAG_FOLDERTYPE, &pvalue))
					return ecError;
				if (pvalue == nullptr)
					return ecSuccess;
				if (FOLDER_TYPE_SEARCH == *(uint32_t*)pvalue) {
					goto DELETE_FOLDER;
				}
			}
			if (!exmdb_client::empty_folder(pstore->get_dir(),
			    pinfo->cpid, username, eid, b_hard, TRUE, TRUE, TRUE,
			    &b_partial) || b_partial)
				return ecError;
 DELETE_FOLDER:
			if (!exmdb_client::delete_folder(pstore->get_dir(),
			    pinfo->cpid, eid, b_hard, &b_result) || !b_result)
				return ecError;
		}
	}
	if (sync_type == SYNC_TYPE_CONTENTS && message_ids.count > 0 &&
	    (!exmdb_client::delete_messages(pstore->get_dir(),
	    pstore->account_id, pinfo->cpid, nullptr,
	    folder_id, &message_ids, b_hard, &b_partial) || b_partial))
		return ecError;
	return ecSuccess;
}

uint32_t zarafa_server_importreadstates(GUID hsession,
	uint32_t hctx, const STATE_ARRAY *pstates)
{
	XID tmp_xid;
	BOOL b_owner;
	void *pvalue;
	uint64_t read_cn;
	uint8_t mapi_type;
	uint64_t folder_id;
	uint64_t message_id;
	uint32_t permission;
	uint32_t proptag_buff[2];
	PROPTAG_ARRAY tmp_proptags;
	TPROPVAL_ARRAY tmp_propvals;
	
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto pctx = pinfo->ptree->get_object<ICSUPCTX_OBJECT>(hctx, &mapi_type);
	if (pctx == nullptr)
		return ecNullObject;
	if (mapi_type != ZMG_ICSUPCTX)
		return ecNotSupported;
	auto pstore = pctx->get_store();
	if (pctx->get_type() != SYNC_TYPE_CONTENTS)
		return ecNotSupported;
	const char *username = nullptr;
	if (!pstore->check_owner_mode()) {
		folder_id = pctx->get_parent_folder_id();
		if (!exmdb_client::check_folder_permission(pstore->get_dir(),
		    folder_id, pinfo->get_username(), &permission))
			return ecError;
		if (!(permission & frightsReadAny))
			username = pinfo->get_username();
	}
	for (size_t i = 0; i < pstates->count; ++i) {
		if (FALSE == common_util_binary_to_xid(
			&pstates->pstate[i].source_key, &tmp_xid)) {
			return ecNotSupported;
		}
		auto tmp_guid = pstore->guid();
		if (0 != guid_compare(&tmp_guid, &tmp_xid.guid)) {
			continue;
		}
		message_id = rop_util_make_eid(1, tmp_xid.local_id);
		bool mark_as_read = pstates->pstate[i].message_flags & MSGFLAG_READ;
		if (NULL != username) {
			if (!exmdb_client_check_message_owner(pstore->get_dir(),
			    message_id, username, &b_owner))
				return ecError;
			if (!b_owner)
				continue;
		}
		tmp_proptags.count = 2;
		tmp_proptags.pproptag = proptag_buff;
		proptag_buff[0] = PROP_TAG_ASSOCIATED;
		proptag_buff[1] = PR_READ;
		if (!exmdb_client::get_message_properties(pstore->get_dir(),
		    nullptr, 0, message_id, &tmp_proptags, &tmp_propvals))
			return ecError;
		pvalue = common_util_get_propvals(
			&tmp_propvals, PROP_TAG_ASSOCIATED);
		if (pvalue != nullptr && *static_cast<uint8_t *>(pvalue) != 0)
			continue;
		pvalue = common_util_get_propvals(&tmp_propvals, PR_READ);
		if (NULL == pvalue || 0 == *(uint8_t*)pvalue) {
			if (!mark_as_read)
				continue;
		} else {
			if (mark_as_read)
				continue;
		}
		if (pstore->b_private) {
			if (!exmdb_client::set_message_read_state(pstore->get_dir(),
			    nullptr, message_id, mark_as_read, &read_cn))
				return ecError;
		} else {
			if (!exmdb_client::set_message_read_state(pstore->get_dir(),
			    pinfo->get_username(), message_id, mark_as_read, &read_cn))
				return ecError;
		}
		idset_append(pctx->pstate->pread, read_cn);
	}
	return ecSuccess;
}

uint32_t zarafa_server_getsearchcriteria(GUID hsession,
	uint32_t hfolder, BINARY_ARRAY *pfolder_array,
	RESTRICTION **pprestriction, uint32_t *psearch_stat)
{
	BINARY *pbin;
	uint8_t mapi_type;
	LONGLONG_ARRAY folder_ids;
	
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto pfolder = pinfo->ptree->get_object<FOLDER_OBJECT>(hfolder, &mapi_type);
	if (pfolder == nullptr)
		return ecNullObject;
	if (mapi_type != ZMG_FOLDER)
		return ecNotSupported;
	auto pstore = pfolder->pstore;
	if (pfolder->type != FOLDER_TYPE_SEARCH)
		return ecNotSearchFolder;
	if (!exmdb_client::get_search_criteria(pstore->get_dir(),
	    pfolder->folder_id, psearch_stat, pprestriction, &folder_ids))
		return ecError;
	pfolder_array->count = folder_ids.count;
	if (0 == folder_ids.count) {
		pfolder_array->pbin = NULL;
		return ecSuccess;
	}
	pfolder_array->pbin = cu_alloc<BINARY>(folder_ids.count);
	if (pfolder_array->pbin == nullptr)
		return ecError;
	for (size_t i = 0; i < folder_ids.count; ++i) {
		pbin = common_util_to_folder_entryid(
				pstore, folder_ids.pll[i]);
		if (pbin == nullptr)
			return ecError;
		pfolder_array->pbin[i] = *pbin;
	}
	return ecSuccess;
}

uint32_t zarafa_server_setsearchcriteria(
	GUID hsession, uint32_t hfolder, uint32_t flags,
	const BINARY_ARRAY *pfolder_array,
	const RESTRICTION *prestriction)
{
	int db_id;
	BOOL b_result;
	BOOL b_private;
	uint8_t mapi_type;
	uint32_t permission;
	uint32_t search_status;
	LONGLONG_ARRAY folder_ids;
	
	if (!(flags & (SEARCH_FLAG_RESTART | SEARCH_FLAG_STOP)))
		/* make the default search_flags */
		flags |= SEARCH_FLAG_RESTART;	
	if (!(flags & (SEARCH_FLAG_RECURSIVE | SEARCH_FLAG_SHALLOW)))
		/* make the default search_flags */
		flags |= SEARCH_FLAG_SHALLOW;
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto pfolder = pinfo->ptree->get_object<FOLDER_OBJECT>(hfolder, &mapi_type);
	if (pfolder == nullptr)
		return ecNullObject;
	if (mapi_type != ZMG_FOLDER)
		return ecNotSupported;
	auto pstore = pfolder->pstore;
	if (!pstore->b_private)
		return ecNotSupported;
	if (!pstore->check_owner_mode()) {
		if (!exmdb_client::check_folder_permission(pstore->get_dir(),
		    pfolder->folder_id, pinfo->get_username(), &permission))
			return ecError;
		if (!(permission & frightsOwner))
			return ecAccessDenied;
	}
	if (NULL == prestriction || 0 == pfolder_array->count) {
		if (!exmdb_client::get_search_criteria(pstore->get_dir(),
		    pfolder->folder_id, &search_status, nullptr, nullptr))
			return ecError;
		if (SEARCH_STATUS_NOT_INITIALIZED == search_status) {
			return ecNotInitialized;
		}
		if (0 == (flags & SEARCH_FLAG_RESTART) &&
			NULL == prestriction && 0 == pfolder_array->count) {
			return ecSuccess;
		}
	}
	folder_ids.count = pfolder_array->count;
	folder_ids.pll   = cu_alloc<uint64_t>(folder_ids.count);
	if (folder_ids.pll == nullptr)
		return ecError;
	for (size_t i = 0; i < pfolder_array->count; ++i) {
		if (FALSE == common_util_from_folder_entryid(
			pfolder_array->pbin[i], &b_private,
			&db_id, &folder_ids.pll[i])) {
			return ecError;
		}
		if (!b_private || db_id != pstore->account_id)
			return ecSearchFolderScopeViolation;
		if (!pstore->check_owner_mode()) {
			if (!exmdb_client::check_folder_permission(pstore->get_dir(),
			    folder_ids.pll[i], pinfo->get_username(), &permission))
				return ecError;
			if (!(permission & (frightsOwner | frightsReadAny)))
				return ecAccessDenied;
		}
	}
	if (!exmdb_client::set_search_criteria(pstore->get_dir(), pinfo->cpid,
	    pfolder->folder_id, flags, prestriction, &folder_ids, &b_result))
		return ecError;
	return b_result ? ecSuccess : ecSearchFolderScopeViolation;
}

uint32_t zarafa_server_messagetorfc822(GUID hsession,
	uint32_t hmessage, BINARY *peml_bin)
{
	uint8_t mapi_type;
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto pmessage = pinfo->ptree->get_object<MESSAGE_OBJECT>(hmessage, &mapi_type);
	if (pmessage == nullptr)
		return ecNullObject;
	if (mapi_type != ZMG_MESSAGE)
		return ecNotSupported;
	return common_util_message_to_rfc822(pmessage->get_store(),
	       pmessage->get_id(), peml_bin) ? ecSuccess : ecError;
}

uint32_t zarafa_server_rfc822tomessage(GUID hsession,
	uint32_t hmessage, const BINARY *peml_bin)
{
	uint8_t mapi_type;
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto pmessage = pinfo->ptree->get_object<MESSAGE_OBJECT>(hmessage, &mapi_type);
	if (pmessage == nullptr)
		return ecNullObject;
	if (mapi_type != ZMG_MESSAGE)
		return ecNotSupported;
	auto pmsgctnt = common_util_rfc822_to_message(pmessage->get_store(), peml_bin);
	if (pmsgctnt == nullptr)
		return ecError;
	return pmessage->write_message(pmsgctnt) ? ecSuccess : ecError;
}

uint32_t zarafa_server_messagetoical(GUID hsession,
	uint32_t hmessage, BINARY *pical_bin)
{
	uint8_t mapi_type;
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto pmessage = pinfo->ptree->get_object<MESSAGE_OBJECT>(hmessage, &mapi_type);
	if (pmessage == nullptr)
		return ecNullObject;
	if (mapi_type != ZMG_MESSAGE)
		return ecNotSupported;
	return common_util_message_to_ical(pmessage->get_store(),
	       pmessage->get_id(), pical_bin) ? ecSuccess : ecError;
}

uint32_t zarafa_server_icaltomessage(GUID hsession,
	uint32_t hmessage, const BINARY *pical_bin)
{
	uint8_t mapi_type;
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto pmessage = pinfo->ptree->get_object<MESSAGE_OBJECT>(hmessage, &mapi_type);
	if (pmessage == nullptr)
		return ecNullObject;
	if (mapi_type != ZMG_MESSAGE)
		return ecNotSupported;
	auto pmsgctnt = common_util_ical_to_message(pmessage->get_store(), pical_bin);
	if (pmsgctnt == nullptr)
		return ecError;
	return pmessage->write_message(pmsgctnt) ? ecSuccess : ecError;
}

uint32_t zarafa_server_messagetovcf(GUID hsession,
	uint32_t hmessage, BINARY *pvcf_bin)
{
	uint8_t mapi_type;
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto pmessage = pinfo->ptree->get_object<MESSAGE_OBJECT>(hmessage, &mapi_type);
	if (pmessage == nullptr)
		return ecNullObject;
	if (mapi_type != ZMG_MESSAGE)
		return ecNotSupported;
	return common_util_message_to_vcf(pmessage, pvcf_bin) ? ecSuccess : ecError;
}

uint32_t zarafa_server_vcftomessage(GUID hsession,
	uint32_t hmessage, const BINARY *pvcf_bin)
{
	uint8_t mapi_type;
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto pmessage = pinfo->ptree->get_object<MESSAGE_OBJECT>(hmessage, &mapi_type);
	if (pmessage == nullptr)
		return ecNullObject;
	if (mapi_type != ZMG_MESSAGE)
		return ecNotSupported;
	auto pmsgctnt = common_util_vcf_to_message(pmessage->get_store(), pvcf_bin);
	if (pmsgctnt == nullptr)
		return ecError;
	return pmessage->write_message(pmsgctnt) ? ecSuccess : ecError;
}

uint32_t zarafa_server_getuseravailability(GUID hsession,
	BINARY entryid, uint64_t starttime, uint64_t endtime,
	char **ppresult_string)
{
	pid_t pid;
	int status;
	int offset;
	int tmp_len;
	char *ptoken;
	char* argv[3];
	char maildir[256];
	char username[UADDR_SIZE];
	char tool_path[256];
	char cookie_buff[1024];
	int pipes_in[2] = {-1, -1};
	int pipes_out[2] = {-1, -1};
	
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	if (!common_util_addressbook_entryid_to_username(entryid,
	    username, GX_ARRAY_SIZE(username)) ||
	    !system_services_get_maildir(username, maildir)) {
		*ppresult_string = NULL;
		return ecSuccess;
	}
	if (strcasecmp(pinfo->get_username(), username) == 0) {
		tmp_len = gx_snprintf(cookie_buff, GX_ARRAY_SIZE(cookie_buff),
			"starttime=%lu;endtime=%lu;dirs=1;dir0=%s",
			starttime, endtime, maildir);
	} else {
		tmp_len = gx_snprintf(cookie_buff, GX_ARRAY_SIZE(cookie_buff),
			"username=%s;starttime=%lu;endtime=%lu;dirs=1;dir0=%s",
		          pinfo->get_username(), starttime, endtime, maildir);
	}
	pinfo.reset();
	 if (-1 == pipe(pipes_in)) {
		return ecError;
	}
	if (-1 == pipe(pipes_out)) {
		close(pipes_in[0]);
		close(pipes_in[1]);
		return ecError;
	}
	pid = fork();
	if (0 == pid) {
		close(pipes_in[1]);
		close(pipes_out[0]);
		close(0);
		close(1);
		dup2(pipes_in[0], 0);
		dup2(pipes_out[1], 1);
		close(pipes_in[0]);
		close(pipes_out[1]);
		strcpy(tool_path, common_util_get_freebusy_path());
		ptoken = strrchr(tool_path, '/');
		if (ptoken != nullptr)
			++ptoken;
		argv[0] = ptoken;
		argv[1] = NULL;
		execve(tool_path, argv, NULL);
		_exit(-1);
	} else if (pid < 0) {
		close(pipes_in[0]);
		close(pipes_in[1]);
		close(pipes_out[0]);
		close(pipes_out[1]);
		return ecError;
	}
	close(pipes_in[0]);
	close(pipes_out[1]);
	write(pipes_in[1], cookie_buff, tmp_len);
	close(pipes_in[1]);
	*ppresult_string = cu_alloc<char>(1024 * 1024);
	if (NULL == *ppresult_string) {
		waitpid(pid, &status, 0);
		return ecError;
	}
	offset = 0;
	while ((tmp_len = read(pipes_out[0], *ppresult_string
		+ offset, 1024*1024 - offset)) > 0) {
		offset += tmp_len;
		if (offset >= 1024*1024) {
			waitpid(pid, &status, 0);
			close(pipes_out[0]);
			return ecError;
		}
	}
	(*ppresult_string)[offset] = '\0';
	close(pipes_out[0]);
	waitpid(pid, &status, 0);
	return status == 0 ? ecSuccess : ecError;
}

uint32_t zarafa_server_setpasswd(const char *username,
	const char *passwd, const char *new_passwd)
{
	return system_services_set_password(username, passwd, new_passwd) ?
	       ecSuccess : ecAccessDenied;
}

uint32_t zarafa_server_linkmessage(GUID hsession,
	BINARY search_entryid, BINARY message_entryid)
{
	uint32_t cpid;
	BOOL b_result;
	BOOL b_private;
	BOOL b_private1;
	char maildir[256];
	uint8_t mapi_type;
	uint64_t folder_id;
	uint64_t folder_id1;
	uint64_t message_id;
	uint32_t account_id;
	uint32_t account_id1;
	
	if (common_util_get_messaging_entryid_type(search_entryid) != EITLT_PRIVATE_FOLDER ||
	    !common_util_from_folder_entryid(search_entryid, &b_private,
	    reinterpret_cast<int *>(&account_id), &folder_id) ||
	    b_private != TRUE)
		return ecInvalidParam;
	if (common_util_get_messaging_entryid_type(message_entryid) != EITLT_PRIVATE_MESSAGE ||
	    !common_util_from_message_entryid(message_entryid, &b_private1,
	    reinterpret_cast<int *>(&account_id1), &folder_id1, &message_id) ||
	    b_private1 != TRUE || account_id != account_id1)
		return ecInvalidParam;
	auto pinfo = zarafa_server_query_session(hsession);
	if (pinfo == nullptr)
		return ecError;
	auto handle = pinfo->ptree->get_store_handle(b_private, account_id);
	if (handle == INVALID_HANDLE)
		return ecNullObject;
	if (pinfo->user_id < 0 || static_cast<unsigned int>(pinfo->user_id) != account_id) {
		return ecAccessDenied;
	}
	auto pstore = pinfo->ptree->get_object<STORE_OBJECT>(handle, &mapi_type);
	if (pstore == nullptr || mapi_type != ZMG_STORE)
		return ecError;
	gx_strlcpy(maildir, pstore->get_dir(), arsizeof(maildir));
	cpid = pinfo->cpid;
	pinfo.reset();
	return exmdb_client::link_message(maildir, cpid, folder_id, message_id,
	       &b_result) && b_result ? ecSuccess : ecError;
}
