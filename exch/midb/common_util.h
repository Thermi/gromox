#pragma once
#include <cstdint>
#include <cstdlib>
#include <gromox/defs.h>
#include <gromox/common_types.hpp>
#include <gromox/element_data.hpp>
#define SOCKET_TIMEOUT										60
#define MAX_DIGLEN											256*1024

extern void common_util_init();
extern void common_util_free();
BOOL common_util_build_environment(const char *maildir);
extern void common_util_free_environment();
void* common_util_alloc(size_t size);
template<typename T> T *cu_alloc() { return static_cast<T *>(common_util_alloc(sizeof(T))); }
template<typename T> T *cu_alloc(size_t elem) { return static_cast<T *>(common_util_alloc(sizeof(T) * elem)); }
template<typename T> T *me_alloc() { return static_cast<T *>(malloc(sizeof(T))); }
template<typename T> T *me_alloc(size_t elem) { return static_cast<T *>(malloc(sizeof(T) * elem)); }
extern BOOL common_util_switch_allocator();
void common_util_set_maildir(const char *maildir);
extern const char* common_util_get_maildir();
char* common_util_dup(const char *pstr);
void* common_util_get_propvals(
	const TPROPVAL_ARRAY *parray, uint32_t proptag);
BINARY* common_util_xid_to_binary(uint8_t size, const XID *pxid);
BINARY* common_util_pcl_append(const BINARY *pbin_pcl,
	const BINARY *pchange_key);
BOOL common_util_create_folder(const char *dir, int user_id,
	uint64_t parent_id, const char *folder_name, uint64_t *pfolder_id);
BOOL common_util_get_propids(const PROPNAME_ARRAY *ppropnames,
	PROPID_ARRAY *ppropids);
extern BOOL common_util_get_propids_create(const PROPNAME_ARRAY *, PROPID_ARRAY *);
BOOL common_util_get_propname(
	uint16_t propid, PROPERTY_NAME **pppropname);
