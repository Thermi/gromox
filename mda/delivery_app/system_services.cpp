// SPDX-License-Identifier: GPL-2.0-only WITH linking exception
#include "system_services.h"
#include "service.h"
#include <cstdio>

void (*system_services_log_info)(unsigned int, const char *, ...);
BOOL (*system_services_check_domain)(const char*);

int system_services_run()
{
#define E(f, s) do { \
	(f) = reinterpret_cast<decltype(f)>(service_query((s), "system", typeid(*(f)))); \
	if ((f) == nullptr) { \
		printf("[%s]: failed to get the \"%s\" service\n", "system_services", (s)); \
		return -1; \
	} \
} while (false)
	E(system_services_log_info, "log_info");
	E(system_services_check_domain, "domain_list_query");
	return 0;
#undef E
}

void system_services_stop()
{
	service_release("log_info", "system");
	service_release("domain_list_query", "system");
}
