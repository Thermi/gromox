#pragma once
#include <gromox/common_types.hpp>
#include <gromox/mem_file.hpp>

extern void mod_rewrite_init();
extern int mod_rewrite_run(const char *sdlist);
extern void mod_rewrite_stop(void);
extern void mod_rewrite_free(void);
BOOL mod_rewrite_process(const char *uri_buff,
	int uri_len, MEM_FILE *pf_request_uri);
