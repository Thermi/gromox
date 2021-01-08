#pragma once
#include "common_types.h"
#include <time.h>

enum {
    BOUNCE_AUDIT_INTERVAL = 0,
	BOUNCE_AUDIT_CAPABILITY
};

#ifdef  __cplusplus
extern "C" {
#endif

void bounce_audit_init(int audit_num, int audit_interval);

int bounce_audit_set_param(int type, int value);

int bounce_audit_get_param(int type);
extern int bounce_audit_run(void);
extern int bounce_audit_stop(void);
extern void bounce_audit_free(void);
BOOL bounce_audit_check(const char *audit_string);

#ifdef  __cplusplus
}
#endif