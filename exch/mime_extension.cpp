// SPDX-License-Identifier: GPL-2.0-only WITH linking exception
#include <cerrno>
#include <libHX/defs.h>
#include <libHX/string.h>
#include <gromox/svc_common.h>
#include "list_file.h"
#include "int_hash.h"
#include "str_hash.h"
#include "util.h"
#include <pthread.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>

DECLARE_API;


static STR_HASH_TABLE *g_mime_hash;
static STR_HASH_TABLE *g_extension_hash;

static pthread_mutex_t g_mime_lock;
static pthread_mutex_t g_extension_lock;


static const char* mime_to_extension(const char *ptype)
{
	char tmp_type[256];
	
	strncpy(tmp_type, ptype, 256);
	HX_strlower(tmp_type);
	pthread_mutex_lock(&g_mime_lock);
	auto pextension = static_cast<char *>(str_hash_query(g_mime_hash, tmp_type));
	pthread_mutex_unlock(&g_mime_lock);
	return pextension;
}

static const char* extension_to_mime(const char *pextension)
{
	char tmp_extension[16];
	
	strncpy(tmp_extension, pextension, 16);
	HX_strlower(tmp_extension);
	pthread_mutex_lock(&g_extension_lock);
	auto ptype = static_cast<char *>(str_hash_query(g_extension_hash, tmp_extension));
	pthread_mutex_unlock(&g_extension_lock);
	return ptype;
}

BOOL SVC_LibMain(int reason, void **ppdata)
{
	int i;
	int item_num;
	char *psearch;
	LIST_FILE *pfile;
	char tmp_path[256];
	char file_name[256];
	
	
	switch(reason) {
	case PLUGIN_INIT: {
		LINK_API(ppdata);
		
		pthread_mutex_init(&g_mime_lock, NULL);
		pthread_mutex_init(&g_extension_lock, NULL);
		
		/* get the plugin name from system api */
		strcpy(file_name, get_plugin_name());
		psearch = strrchr(file_name, '.');
		if (NULL != psearch) {
			*psearch = '\0';
		}
		sprintf(tmp_path, "%s/%s.txt", get_data_path(), file_name);
		struct srcitem { char ext[16], mimetype[64]; };
		pfile = list_file_init(tmp_path, "%s:16%s:64");
		if (NULL == pfile) {
			printf("[mime_extension]: list_file_init %s/%s: %s\n",
				get_data_path(), file_name, strerror(errno));
			return FALSE;
		}
		item_num = list_file_get_item_num(pfile);
		auto pitem = reinterpret_cast<srcitem *>(list_file_get_list(pfile));
		g_mime_hash = str_hash_init(item_num + 1, 16, NULL);
		if (NULL == g_mime_hash) {
			printf("[mime_extension]: Failed to init MIME hash table\n");
			list_file_free(pfile);
			return FALSE;
		}
		g_extension_hash = str_hash_init(item_num + 1, 64, NULL);
		if (NULL == g_extension_hash) {
			printf("[mime_extension]: Failed to init extension hash table\n");
			list_file_free(pfile);
			return FALSE;
		}
		for (i=0; i<item_num; i++) {
			HX_strlower(pitem[i].ext);
			HX_strlower(pitem[i].mimetype);
			str_hash_add(g_extension_hash, pitem[i].ext, pitem[i].mimetype);
			str_hash_add(g_mime_hash, pitem[i].mimetype, pitem[i].ext);
		}
		list_file_free(pfile);
		if (!register_service("mime_to_extension", reinterpret_cast<void *>(mime_to_extension))) {
			printf("[mime_extension]: failed to register"
				" \"mime_to_extension\" service\n");
			return FALSE;
		}
		if (!register_service("extension_to_mime", reinterpret_cast<void *>(extension_to_mime))) {
			printf("[mime_extension]: failed to register"
				" \"extension_to_mime\" service\n");
			return FALSE;
		}
		printf("[mime_extension]: plugin is loaded into system\n");
		return TRUE;
	}
	case PLUGIN_FREE:
		if (NULL != g_mime_hash) {
			str_hash_free(g_mime_hash);
			g_mime_hash = NULL;
		}
		if (NULL != g_extension_hash) {
			str_hash_free(g_extension_hash);
			g_extension_hash = NULL;
		}
		pthread_mutex_destroy(&g_mime_lock);
		pthread_mutex_destroy(&g_extension_lock);
		return TRUE;
	}
	return false;
}