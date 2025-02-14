#pragma once
#include <cstdint>
#include <memory>
#include "ftstream_producer.h"
#include "ics_state.h"

struct FASTDOWNCTX_OBJECT final {
	~FASTDOWNCTX_OBJECT();
	/* make_xxx function can be invoked only once on the object */
	BOOL make_messagecontent(MESSAGE_CONTENT *);
	BOOL make_attachmentcontent(ATTACHMENT_CONTENT *);
	BOOL make_foldercontent(BOOL subfolders, std::unique_ptr<FOLDER_CONTENT> &&);
	BOOL make_topfolder(std::unique_ptr<FOLDER_CONTENT> &&);
	BOOL make_messagelist(BOOL chginfo, EID_ARRAY *msglst);
	BOOL make_state(ICS_STATE *);
	BOOL get_buffer(void *buf, uint16_t *len, BOOL *last, uint16_t *progress, uint16_t *total);

	std::unique_ptr<FTSTREAM_PRODUCER> pstream;
	BOOL b_back = false, b_last = false, b_chginfo = false;
	EID_ARRAY *pmsglst = nullptr;
	std::unique_ptr<FOLDER_CONTENT> pfldctnt;
	DOUBLE_LIST flow_list{};
	uint32_t total_steps = 0, progress_steps = 0;
};

extern std::unique_ptr<FASTDOWNCTX_OBJECT> fastdownctx_object_create(LOGON_OBJECT *, uint8_t string_option);
