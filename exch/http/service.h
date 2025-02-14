#pragma once
#include <typeinfo>
#include <gromox/common_types.hpp>
#include <gromox/plugin.hpp>

struct service_init_param {
	const char *plugin_dir = nullptr, *config_dir = nullptr;
	const char *data_dir = nullptr, *state_dir = nullptr;
	const char *const *plugin_list = nullptr;
	bool plugin_ignloaderr = false;
	unsigned int context_num = 0;
	const char *prog_id = nullptr;
};

extern void service_init(const struct service_init_param &);
extern int service_run();
extern void service_stop();
int service_load_library(const char *path);
int service_unload_library(const char *path);
extern void *service_query(const char *service_name, const char *module, const std::type_info &);
void service_release(const char *service_name, const char *module);
int service_console_talk(int argc, char **argv, char *reason, int len);
extern BOOL service_register_service(const char *func_name, void *addr, const std::type_info &);
extern void service_reload_all();
