// SPDX-License-Identifier: GPL-2.0-only WITH linking exception
#include <cstdint>
#include <libHX/string.h>
#include <gromox/defs.h>
#include "auto_response.h"
#include "bounce_audit.h"
#include "exmdb_client.h"
#include "exmdb_local.h"
#include <gromox/config_file.hpp>
#include <gromox/hook_common.h>
#include <gromox/mail_func.hpp>
#include <gromox/util.hpp>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <ctime>

void auto_response_reply(const char *user_home,
	const char *from, const char *rcpt)
{
	MIME *pmime;
	BOOL b_found;
	char *pcontent;
	BOOL b_internal;
	time_t cur_time;
	char charset[32]{};
	struct tm tm_buff;
	int i, j, fd, len;
	char subject[1024];
	char buff[64*1024];
	char date_buff[128];
	char temp_path[256];
	uint8_t reply_state;
	char audit_buff[256];
	MIME_FIELD mime_field;
	struct stat node_stat;
	char content_type[256];
	char template_path[256];
	char new_buff[128*1024];
	MESSAGE_CONTEXT *pcontext;

	if (0 == strcasecmp(from, rcpt) ||
		0 == strcasecmp(rcpt, "none@none")) {
		return;
	}

	auto ptoken = strchr(from, '@');
	auto ptoken1 = strchr(rcpt, '@');
	if (NULL == ptoken || NULL == ptoken1) {
		return;
	}

	if (0 == strcasecmp(ptoken, ptoken1)) {
		b_internal = TRUE;
	} else {
		if (FALSE == exmdb_local_check_domain(ptoken + 1)) {
			b_internal = FALSE;
		} else {
			b_internal = exmdb_local_check_same_org2(
							ptoken + 1, ptoken1 + 1);
		}
	}
	
	snprintf(temp_path, 256, "%s/config/autoreply.cfg", user_home);
	auto pconfig = config_file_init(temp_path);
	if (NULL == pconfig) {
		return;
	}
	auto str_value = config_file_get_value(pconfig, "OOF_STATE");
	if (NULL == str_value) {
		return;
	}
	reply_state = atoi(str_value);
	if (1 != reply_state && 2 != reply_state) {
		return;
	}
	time(&cur_time);
	if (2 == reply_state) {
		str_value = config_file_get_value(pconfig, "START_TIME");
		if (NULL != str_value && atoll(str_value) > cur_time) {
			return;
		}
		str_value = config_file_get_value(pconfig, "END_TIME");
		if (NULL != str_value && cur_time > atoll(str_value)) {
			return;
		}
	}
	if (TRUE == b_internal) {
		snprintf(template_path, 256, "%s/config/internal-reply", user_home);
	} else {
		str_value = config_file_get_value(pconfig, "ALLOW_EXTERNAL_OOF");
		if (NULL == str_value || 0 == atoi(str_value)) {
			return;
		}
		str_value = config_file_get_value(pconfig, "EXTERNAL_AUDIENCE");
		if (NULL != str_value && 0 != atoi(str_value)) {
			if (EXMDB_RESULT_OK != exmdb_client_check_contact_address(
				user_home, rcpt, &b_found) || FALSE == b_found) {
				return;	
			}
		}
		snprintf(template_path, 256, "%s/config/external-reply", user_home);
	}
	snprintf(audit_buff, 256, "%s:%s", from, rcpt);
	if (FALSE == bounce_audit_check(audit_buff)) {
		return;
	}
	fd = open(template_path, O_RDONLY);
	if (-1 == fd) {
		return;
	}
	if (fstat(fd, &node_stat) != 0 || node_stat.st_size == 0 ||
	    static_cast<size_t>(node_stat.st_size) > sizeof(buff) - 1 ||
	    read(fd, buff, node_stat.st_size) != node_stat.st_size) {
		close(fd);
		return;
	}
	close(fd);

	if ('\n' == buff[0]) {
		new_buff[0] = '\r';
		new_buff[1] = '\n';
		i = 1;
		j = 2;
	} else {
		new_buff[0] = buff[0];
		i = 1;
		j = 1;
	}
	for (; i<node_stat.st_size; i++, j++) {
		if ('\n' == buff[i] && '\r' != buff[i - 1]) {
			new_buff[j] = '\r';
			j ++;
		}
		new_buff[j] = buff[i];
	}
	new_buff[j] = '\0';


	i = 0;
	pcontent = NULL;
	strcpy(content_type, "text/plain");
	strcpy(subject, "auto response message");
	while (i < j) {
		auto parsed_length = parse_mime_field(new_buff + i, j - i, &mime_field);
		i += parsed_length;
		if (0 != parsed_length) {
			if (0 == strncasecmp("Content-Type", mime_field.field_name, 12)) {
				if (mime_field.field_value_len > sizeof(content_type) - 1) {
					return;
				}
				memcpy(content_type, mime_field.field_value,
					mime_field.field_value_len);
				content_type[mime_field.field_value_len] = '\0';
				charset[0] = '\0';
				auto ptoken2 = strchr(content_type, ';');
				if (ptoken2 != nullptr) {
					*ptoken2 = '\0';
					++ptoken2;
					ptoken2 = strcasestr(ptoken2, "charset=");
					if (ptoken2 != nullptr) {
						gx_strlcpy(charset, ptoken2 + 8, GX_ARRAY_SIZE(charset));
						ptoken2 = strchr(charset, ';');
						if (ptoken2 != nullptr)
							*ptoken2 = '\0';
						HX_strrtrim(charset);
						HX_strltrim(charset);
						len = strlen(charset);
						if ('"' == charset[len - 1]) {
							len --;
							charset[len] = '\0';
						}
						if ('"' == charset[0]) {
							memmove(charset, charset + 1, len);
						}
					}
				}
			} else if (0 == strncasecmp("Subject", mime_field.field_name, 7)) {
				if (mime_field.field_value_len > sizeof(subject) - 1) {
					return;
				}
				memcpy(subject, mime_field.field_value,
					mime_field.field_value_len);
				subject[mime_field.field_value_len] = '\0';
			}
			if ('\r' == new_buff[i] && '\n' == new_buff[i + 1]) {
				pcontent = new_buff + i + 2;
				break;
			}
		} else {
			return;
		}
	}
	if (NULL == pcontent) {
		return;
	}
	pcontext = get_context();
	if (NULL == pcontext) {
		return;
	}
	auto pdomain = strchr(from, '@') + 1;
	sprintf(pcontext->pcontrol->from, "auto-reply@%s", pdomain);
	
	mem_file_writeline(&pcontext->pcontrol->f_rcpt_to, (char*)rcpt);
	pmime = mail_add_head(pcontext->pmail);
	if (NULL == pmime) {
		put_context(pcontext);
		return;
	}
	mime_set_content_type(pmime, content_type);
	if ('\0' != charset[0]) {
		mime_set_content_param(pmime, "charset", charset);
	}
	mime_set_field(pmime, "Received", "from unknown (helo localhost) "
		"(unknown@127.0.0.1)\r\n\tby herculiz with SMTP");
	mime_set_field(pmime, "From", from);
	mime_set_field(pmime, "To", rcpt);
	mime_set_field(pmime, "MIME-Version", "1.0");
	mime_set_field(pmime, "X-Auto-Response-Suppress", "All");
	strftime(date_buff, 128, "%a, %d %b %Y %H:%M:%S %z",
		localtime_r(&cur_time, &tm_buff));
	mime_set_field(pmime, "Date", date_buff);
	mime_set_field(pmime, "Subject", subject);
	if (FALSE == mime_write_content(pmime, pcontent,
		new_buff + j - pcontent, MIME_ENCODING_BASE64)) {
		put_context(pcontext);
		return;
	}
	enqueue_context(pcontext);
}

