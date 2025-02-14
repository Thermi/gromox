#pragma once
#include <cstdint>
#include <memory>
#include <gromox/defs.h>
#include "message_object.h"

/* MESSAGE_OBJECT and ATTACHMENT_OBJECT are friend classes,
	so they can operate internal variables of each other */
struct ATTACHMENT_OBJECT {
	~ATTACHMENT_OBJECT();
	uint32_t get_instance_id() const { return instance_id; }
	BOOL init_attachment();
	uint32_t get_attachment_num() const { return attachment_num; }
	uint32_t get_tag_access() const { return pparent->tag_access; }
	uint8_t get_open_flags() const { return open_flags; }
	void set_open_flags(uint8_t open_flags);
	uint32_t get_cpid() const { return pparent->cpid; }
	gxerr_t save();
	BOOL append_stream_object(STREAM_OBJECT *);
	BOOL commit_stream_object(STREAM_OBJECT *);
	BOOL flush_streams();
	BOOL get_all_proptags(PROPTAG_ARRAY *);
	BOOL check_readonly_property(uint32_t proptag) const;
	BOOL get_properties(uint32_t size_limit, const PROPTAG_ARRAY *, TPROPVAL_ARRAY *);
	BOOL set_properties(const TPROPVAL_ARRAY *, PROBLEM_ARRAY *);
	BOOL remove_properties(const PROPTAG_ARRAY *, PROBLEM_ARRAY *);
	BOOL copy_properties(ATTACHMENT_OBJECT *atsrc, const PROPTAG_ARRAY *exclprop, BOOL force, BOOL *cycle, PROBLEM_ARRAY *);

	BOOL b_new = false, b_touched = false;
	MESSAGE_OBJECT *pparent = nullptr;
	uint32_t instance_id = 0, attachment_num = 0;
	uint8_t open_flags = 0;
	DOUBLE_LIST stream_list{};
};

extern std::unique_ptr<ATTACHMENT_OBJECT> attachment_object_create(MESSAGE_OBJECT *parent, uint32_t at_num, uint8_t open_flags);
