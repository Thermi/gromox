#pragma once
#include <gromox/hook_common.h>

enum {
	DELIVERY_NO_USER,
	DELIVERY_MAILBOX_FULL,
	DELIVERY_OPERATION_ERROR,
	DELIVERY_OPERATION_FAILURE,
	DELIVERY_OPERATION_OK,
	DELIVERY_OPERATION_DELIVERED
};

#define BOUND_NOTLOCAL					7

#ifdef  __cplusplus
extern "C" {
#endif

extern BOOL (*exmdb_local_check_domain)(const char *domainname);

extern BOOL (*exmdb_local_get_lang)(const char *username, char *lang);

extern BOOL (*exmdb_local_get_timezone)(
	const char *username, char *timezone);

extern BOOL (*exmdb_local_check_same_org2)(
	const char *domainname1, const char *domainname2);

extern BOOL (*exmdb_local_lang_to_charset)(
	const char *lang, char *charset);
void exmdb_local_init(const char *config_path,
	const char *org_name, const char *default_charset,
	const char *default_timezone, const char *propname_path);
extern int exmdb_local_run(void);
extern int exmdb_local_stop(void);
extern void exmdb_local_free(void);
BOOL exmdb_local_hook(MESSAGE_CONTEXT *pcontext);

int exmdb_local_deliverquota(MESSAGE_CONTEXT *pcontext, const char *address);
extern void exmdb_local_log_info(MESSAGE_CONTEXT *pcontext, const char *rcpt_to, int level, const char *format, ...);
void exmdb_local_console_talk(int argc, char **argv, char *result, int length);

#ifdef  __cplusplus
}
#endif