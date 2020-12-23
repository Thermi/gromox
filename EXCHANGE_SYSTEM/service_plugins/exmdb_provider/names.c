#include <libHX/defs.h>
#include <gromox/defs.h>
#include "common_util.h"

#define EXP(s) CALL_ID_ ## s
#define E(s) [EXP(s)] = #s
static const char *const exmdb_rpc_names[] = {
	E(CONNECT),
	E(LISTEN_NOTIFICATION),
	E(PING_STORE),
	E(GET_ALL_NAMED_PROPIDS),
	E(GET_NAMED_PROPIDS),
	E(GET_NAMED_PROPNAMES),
	E(GET_MAPPING_GUID),
	E(GET_MAPPING_REPLID),
	E(GET_STORE_ALL_PROPTAGS),
	E(GET_STORE_PROPERTIES),
	E(SET_STORE_PROPERTIES),
	E(REMOVE_STORE_PROPERTIES),
	E(CHECK_MAILBOX_PERMISSION),
	E(GET_FOLDER_BY_CLASS),
	E(SET_FOLDER_BY_CLASS),
	E(GET_FOLDER_CLASS_TABLE),
	E(CHECK_FOLDER_ID),
	E(QUERY_FOLDER_MESSAGES),
	E(CHECK_FOLDER_DELETED),
	E(GET_FOLDER_BY_NAME),
	E(CHECK_FOLDER_PERMISSION),
	E(CREATE_FOLDER_BY_PROPERTIES),
	E(GET_FOLDER_ALL_PROPTAGS),
	E(GET_FOLDER_PROPERTIES),
	E(SET_FOLDER_PROPERTIES),
	E(REMOVE_FOLDER_PROPERTIES),
	E(DELETE_FOLDER),
	E(EMPTY_FOLDER),
	E(CHECK_FOLDER_CYCLE),
	E(COPY_FOLDER_INTERNAL),
	E(GET_SEARCH_CRITERIA),
	E(SET_SEARCH_CRITERIA),
	E(MOVECOPY_MESSAGE),
	E(MOVECOPY_MESSAGES),
	E(MOVECOPY_FOLDER),
	E(DELETE_MESSAGES),
	E(GET_MESSAGE_BRIEF),
	E(SUM_HIERARCHY),
	E(LOAD_HIERARCHY_TABLE),
	E(SUM_CONTENT),
	E(LOAD_CONTENT_TABLE),
	E(LOAD_PERMISSION_TABLE),
	E(LOAD_RULE_TABLE),
	E(UNLOAD_TABLE),
	E(SUM_TABLE),
	E(QUERY_TABLE),
	E(MATCH_TABLE),
	E(LOCATE_TABLE),
	E(READ_TABLE_ROW),
	E(MARK_TABLE),
	E(GET_TABLE_ALL_PROPTAGS),
	E(EXPAND_TABLE),
	E(COLLAPSE_TABLE),
	E(STORE_TABLE_STATE),
	E(RESTORE_TABLE_STATE),
	E(CHECK_MESSAGE),
	E(CHECK_MESSAGE_DELETED),
	E(LOAD_MESSAGE_INSTANCE),
	E(LOAD_EMBEDDED_INSTANCE),
	E(GET_EMBEDDED_CN),
	E(RELOAD_MESSAGE_INSTANCE),
	E(CLEAR_MESSAGE_INSTANCE),
	E(READ_MESSAGE_INSTANCE),
	E(WRITE_MESSAGE_INSTANCE),
	E(LOAD_ATTACHMENT_INSTANCE),
	E(CREATE_ATTACHMENT_INSTANCE),
	E(READ_ATTACHMENT_INSTANCE),
	E(WRITE_ATTACHMENT_INSTANCE),
	E(DELETE_MESSAGE_INSTANCE_ATTACHMENT),
	E(FLUSH_INSTANCE),
	E(UNLOAD_INSTANCE),
	E(GET_INSTANCE_ALL_PROPTAGS),
	E(GET_INSTANCE_PROPERTIES),
	E(SET_INSTANCE_PROPERTIES),
	E(REMOVE_INSTANCE_PROPERTIES),
	E(CHECK_INSTANCE_CYCLE),
	E(EMPTY_MESSAGE_INSTANCE_RCPTS),
	E(GET_MESSAGE_INSTANCE_RCPTS_NUM),
	E(GET_MESSAGE_INSTANCE_RCPTS_ALL_PROPTAGS),
	E(GET_MESSAGE_INSTANCE_RCPTS),
	E(UPDATE_MESSAGE_INSTANCE_RCPTS),
	E(EMPTY_MESSAGE_INSTANCE_ATTACHMENTS),
	E(GET_MESSAGE_INSTANCE_ATTACHMENTS_NUM),
	E(GET_MESSAGE_INSTANCE_ATTACHMENT_TABLE_ALL_PROPTAGS),
	E(QUERY_MESSAGE_INSTANCE_ATTACHMENT_TABLE),
	E(SET_MESSAGE_INSTANCE_CONFLICT),
	E(GET_MESSAGE_RCPTS),
	E(GET_MESSAGE_PROPERTIES),
	E(SET_MESSAGE_PROPERTIES),
	E(SET_MESSAGE_READ_STATE),
	E(REMOVE_MESSAGE_PROPERTIES),
	E(ALLOCATE_MESSAGE_ID),
	E(ALLOCATE_CN),
	E(MARK_MODIFIED),
	E(GET_MESSAGE_GROUP_ID),
	E(SET_MESSAGE_GROUP_ID),
	E(SAVE_CHANGE_INDICES),
	E(GET_CHANGE_INDICES),
	E(TRY_MARK_SUBMIT),
	E(CLEAR_SUBMIT),
	E(LINK_MESSAGE),
	E(UNLINK_MESSAGE),
	E(RULE_NEW_MESSAGE),
	E(SET_MESSAGE_TIMER),
	E(GET_MESSAGE_TIMER),
	E(EMPTY_FOLDER_PERMISSION),
	E(UPDATE_FOLDER_PERMISSION),
	E(EMPTY_FOLDER_RULE),
	E(UPDATE_FOLDER_RULE),
	E(DELIVERY_MESSAGE),
	E(WRITE_MESSAGE),
	E(READ_MESSAGE),
	E(GET_CONTENT_SYNC),
	E(GET_HIERARCHY_SYNC),
	E(ALLOCATE_IDS),
	E(SUBSCRIBE_NOTIFICATION),
	E(UNSUBSCRIBE_NOTIFICATION),
	E(TRANSPORT_NEW_MAIL),
	E(RELOAD_CONTENT_TABLE),
	E(COPY_INSTANCE_RCPTS),
	E(COPY_INSTANCE_ATTACHMENTS),
	E(CHECK_CONTACT_ADDRESS),
	E(GET_PUBLIC_FOLDER_UNREAD_COUNT),
	E(UNLOAD_STORE),
};
#undef E
#undef EXP

const char *exmdb_rpc_idtoname(unsigned int i)
{
	const char *s = i < ARRAY_SIZE(exmdb_rpc_names) ? exmdb_rpc_names[i] : nullptr;
	return s != nullptr ? s : "";
}