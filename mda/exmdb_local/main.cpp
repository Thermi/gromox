// SPDX-License-Identifier: GPL-2.0-only WITH linking exception
#include <cerrno>
#include <cstring>
#include <libHX/string.h>
#include <gromox/defs.h>
#include <gromox/hook_common.h>
#include <gromox/config_file.hpp>
#include "exmdb_local.h"
#include "cache_queue.h"
#include "net_failure.h"
#include "bounce_audit.h"
#include "exmdb_client.h"
#include "bounce_producer.h"
#include <gromox/util.hpp>
#include <cstdio>

using namespace gromox;

DECLARE_API();

static BOOL hook_exmdb_local(int reason, void **ppdata)
{
	int conn_num;
	char charset[32], tmzone[64];
	char org_name[256];
	char separator[16];
	char temp_buff[45];
	char tmp_path[256];
	int cache_interval;
	int retrying_times;
	int alarm_interval;
	int times, interval;
	char file_name[256];
	char cache_path[256], *psearch;
	int response_capacity;
	int response_interval;
	 
	/* path contains the config files directory */
    switch (reason) {
	case PLUGIN_INIT: {
		LINK_API(ppdata);
		gx_strlcpy(file_name, get_plugin_name(), GX_ARRAY_SIZE(file_name));
		psearch = strrchr(file_name, '.');
		if (NULL != psearch) {
			*psearch = '\0';
		}
		snprintf(tmp_path, GX_ARRAY_SIZE(tmp_path), "%s.cfg", file_name);
		auto pfile = config_file_initd(tmp_path, get_config_path());
		if (NULL == pfile) {
			printf("[exmdb_local]: config_file_initd %s: %s\n",
			       tmp_path, strerror(errno));
			return FALSE;
		}

		auto str_value = config_file_get_value(pfile, "SEPARATOR_FOR_BOUNCE");
		gx_strlcpy(separator, str_value == nullptr ? " " : str_value, GX_ARRAY_SIZE(separator));

		sprintf(cache_path, "%s/cache", get_queue_path());
		
		str_value = config_file_get_value(pfile, "X500_ORG_NAME");
		gx_strlcpy(org_name, str_value != nullptr ? str_value : "Gromox default", arsizeof(org_name));
		printf("[exmdb_local]: x500 org name is \"%s\"\n", org_name);
		
		str_value = config_file_get_value(pfile, "DEFAULT_CHARSET");
		gx_strlcpy(charset, str_value != nullptr ? str_value : "windows-1252", arsizeof(charset));
		printf("[exmdb_local]: default charset is \"%s\"\n", charset);
		
		str_value = config_file_get_value(pfile, "DEFAULT_TIMEZONE");
		gx_strlcpy(tmzone, str_value != nullptr ? str_value : "Asia/Shanghai", arsizeof(tmzone));
		printf("[exmdb_local]: default timezone is \"%s\"\n", tmzone);
		
		str_value = config_file_get_value(pfile, "EXMDB_CONNECTION_NUM");
		conn_num = str_value != nullptr ? strtol(str_value, nullptr, 0) : 5;
		if (conn_num < 2 || conn_num > 100)
			conn_num = 5;
		printf("[exmdb_local]: exmdb connection number is %d\n", conn_num);
		
		str_value = config_file_get_value(pfile, "CACHE_SCAN_INTERVAL");
		if (NULL == str_value) {
			cache_interval = 180;
		} else {
			cache_interval = atoitvl(str_value);
			if (cache_interval <= 0)
				cache_interval = 180;
		}
		itvltoa(cache_interval, temp_buff);
		printf("[exmdb_local]: cache scanning interval is %s\n", temp_buff);

		str_value = config_file_get_value(pfile, "RETRYING_TIMES");
		retrying_times = str_value != nullptr ? strtol(str_value, nullptr, 0) : 30;
		if (retrying_times <= 0)
			retrying_times = 30;
		printf("[exmdb_local]: retrying times on temporary failure is %d\n",
			retrying_times);
		
		str_value = config_file_get_value(pfile, "FAILURE_TIMES_FOR_ALARM");
		times = str_value != nullptr ? strtol(str_value, nullptr, 0) : 10;
		if (times <= 0)
			times = 10;
		printf("[exmdb_local]: failure times for alarm is %d\n", times);

		str_value = config_file_get_value(pfile,
				"INTERVAL_FOR_FAILURE_STATISTIC");
		if (NULL == str_value) {
			interval = 3600;
		} else {
			interval = atoitvl(str_value);
			if (interval <= 0)
				interval = 3600;
		}
		itvltoa(interval, temp_buff);
		printf("[exmdb_local]: interval for failure alarm is %s\n", temp_buff);

		str_value = config_file_get_value(pfile, "ALARM_INTERVAL");
		if (NULL == str_value) {
			alarm_interval = 1800;
		} else {
			alarm_interval = atoitvl(str_value);
			if (alarm_interval <= 0)
				alarm_interval = 1800;
		}
		itvltoa(alarm_interval, temp_buff);
		printf("[exmdb_local]: alarms interval is %s\n", temp_buff);

		str_value = config_file_get_value(pfile, "RESPONSE_AUDIT_CAPACITY");
		response_capacity = str_value != nullptr ? strtol(str_value, nullptr, 0) : 1000;
		if (response_capacity < 0)
			response_capacity = 1000;
		printf("[exmdb_local]: auto response audit capacity is %d\n",
			response_capacity);

		str_value = config_file_get_value(pfile, "RESPONSE_INTERVAL");
		if (NULL == str_value) {
			response_interval = 180;
		} else {
			response_interval = atoitvl(str_value);
			if (response_interval <= 0)
				response_interval = 180;
		}
		itvltoa(response_interval, temp_buff);
		printf("[exmdb_local]: auto response interval is %s\n", temp_buff);

		net_failure_init(times, interval, alarm_interval);
		bounce_producer_init(separator);
		bounce_audit_init(response_capacity, response_interval);
		cache_queue_init(cache_path, cache_interval, retrying_times);
		exmdb_client_init(conn_num);
		exmdb_local_init(org_name, charset, tmzone);
		
		if (0 != net_failure_run()) {
			printf("[exmdb_local]: failed to run net failure\n");
			return FALSE;
		}
		if (0 != bounce_producer_run()) {
			printf("[exmdb_local]: failed to run bounce producer\n");
			return FALSE;
		}
		if (0 != bounce_audit_run()) {
			printf("[exmdb_local]: failed to run bounce audit\n");
			return FALSE;
		}
		if (0 != cache_queue_run()) {
			printf("[exmdb_local]: failed to run cache queue\n");
			return FALSE;
		}
		if (0 != exmdb_client_run()) {
			printf("[exmdb_local]: failed to run exmdb client\n");
			return FALSE;
		}
		if (0 != exmdb_local_run()) {
			printf("[exmdb_local]: failed to run exmdb local\n");
			return FALSE;
		}
		register_talk(exmdb_local_console_talk);
        if (FALSE == register_local(exmdb_local_hook)) {
			printf("[exmdb_local]: failed to register the hook function\n");
            return FALSE;
        }
        return TRUE;
	}
    case PLUGIN_FREE:
		exmdb_client_stop();
		cache_queue_stop();
		cache_queue_free();
		bounce_audit_stop();
		net_failure_free();
        return TRUE;
    }
	return TRUE;
}
HOOK_ENTRY(hook_exmdb_local);
