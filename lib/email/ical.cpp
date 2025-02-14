// SPDX-License-Identifier: GPL-2.0-only WITH linking exception
#include <algorithm>
#include <cerrno>
#include <list>
#include <new>
#include <optional>
#include <string>
#include <libHX/ctype_helper.h>
#include <libHX/string.h>
#include <gromox/defs.h>
#include <gromox/fileio.h>
#include <gromox/mail_func.hpp>
#include <gromox/timezone.hpp>
#include <gromox/ical.hpp>
#include <gromox/util.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#define MAX_LINE							73

namespace {

struct LINE_ITEM {
	char *ptag;
	char *pvalue;
};

}

static char* ical_get_tag_comma(char *pstring)
{
	int i;
	int tmp_len;
	BOOL b_quote;
	
	b_quote = FALSE;
	tmp_len = strlen(pstring);
	for (i=0; i<tmp_len; i++) {
		if (TRUE == b_quote) {
			if ('"' == pstring[i]) {
				memmove(pstring + i, pstring + i + 1, tmp_len - i);
				pstring[tmp_len] = '\0';
				tmp_len --;
				i --;
				b_quote = FALSE;
			}
			continue;
		}
		if ('"' == pstring[i]) {
			memmove(pstring + i, pstring + i + 1, tmp_len - i);
			pstring[tmp_len] = '\0';
			tmp_len --;
			i --;
			b_quote = TRUE;
		} else if (',' == pstring[i]) {
			pstring[i] = '\0';
			return pstring + i + 1;
		}
	}
	return NULL;
}

static char* ical_get_tag_semicolon(char *pstring)
{
	int i;
	int tmp_len;
	BOOL b_quote;
	
	b_quote = FALSE;
	tmp_len = strlen(pstring);
	for (i=0; i<tmp_len; i++) {
		if (TRUE == b_quote) {
			if ('"' == pstring[i]) {
				b_quote = FALSE;
			}
			continue;
		}
		if ('"' == pstring[i]) {
			b_quote = TRUE;
		} else if (';' == pstring[i]) {
			pstring[i] = '\0';
			for (i+=1; i<tmp_len; i++) {
				if (' ' != pstring[i] && '\t' != pstring[i]) {
					break;
				}
			}
			return pstring + i;
		}
	}
	return NULL;
}

static char *ical_get_value_sep(char *pstring, char sep)
{
	int i;
	int tmp_len;
	
	tmp_len = strlen(pstring);
	for (i=0; i<tmp_len; i++) {
		if ('\\' == pstring[i]) {
			if (pstring[i+1] == '\\' || pstring[i+1] == sep) {
				memmove(pstring + i, pstring + i + 1, tmp_len - i - 1);
				pstring[tmp_len-1] = '\0';
				tmp_len --;
			} else if ('n' == pstring[i + 1] || 'N' == pstring[i + 1]) {
				pstring[i] = '\r';
				pstring[i + 1] = '\n';
			}
		} else if (pstring[i] == sep) {
			pstring[i] = '\0';
			for (i+=1; i<tmp_len; i++) {
				if (' ' != pstring[i] && '\t' != pstring[i]) {
					break;
				}
			}
			return pstring + i;
		}
	}
	return NULL;
}

static int ical_init_component(ICAL_COMPONENT *pcomponent, const char *name)
{
	try {
		pcomponent->m_name = name;
	} catch (...) {
		return -ENOMEM;
	}
	return 0;
}


static void ical_clear_component(ICAL_COMPONENT *pcomponent)
{
	pcomponent->component_list.clear();
}

std::shared_ptr<ICAL_COMPONENT> ical_new_component(const char *name)
{
	try {
		auto c = std::make_shared<ICAL_COMPONENT>();
		auto ret = ical_init_component(c.get(), name);
		if (ret < 0) {
			errno = -ret;
			return nullptr;
		}
		return c;
	} catch (...) {
	}
	errno = ENOMEM;
	return nullptr;
}

int ical_init(ICAL *pical)
{
	return ical_init_component(pical, "VCALENDAR");
}

static bool ical_retrieve_line_item(char *pline, LINE_ITEM *pitem)
{
	BOOL b_quote;
	BOOL b_value;
	pitem->ptag = NULL;
	pitem->pvalue = NULL;
	
	b_value = FALSE;
	b_quote = FALSE;
	while ('\0' != *pline) {
		if ((NULL == pitem->ptag ||
			(TRUE == b_value && NULL == pitem->pvalue))
			&& (' ' == *pline || '\t' == *pline)) {
			pline ++;
			continue;
		}
		if (NULL == pitem->ptag) {
			pitem->ptag = pline;
			pline ++;
			continue;
		}
		if (FALSE == b_value) {
			if ('"' == *pline) {
				if (FALSE == b_quote) {
					b_quote = TRUE;
				} else {
					b_quote = FALSE;
				}
			}
			if (TRUE == b_quote) {
				pline ++;
				continue;
			}
			if (':' == *pline) {
				*pline = '\0';
				b_value = TRUE;
			}
		} else {
			if (NULL == pitem->pvalue) {
				pitem->pvalue = pline;
				break;
			}
		}
		pline ++;
	}
	if (NULL == pitem->ptag) {
		return false;
	}
	return true;
}

static char* ical_get_string_line(char *pbuff, size_t max_length)
{
	size_t i;
	char *pnext;
	bool b_searched = false;

	for (i=0; i<max_length; i++) {
		if ('\r' == pbuff[i]) {
			pbuff[i] = '\0';
			if (!b_searched)
				b_searched = true;
			if (i + 1 < max_length && '\n' == pbuff[i + 1]) {
				pnext = pbuff + i + 2;
				if (' ' == *pnext || '\t' == *pnext) {
					pnext ++;
					size_t bytes = pbuff + max_length - pnext;
					memmove(pbuff + i, pnext, bytes);
					pbuff[i+bytes] = '\0';
					max_length -= pnext - (pbuff + i);
					continue;
				}
			} else {
				pnext = pbuff + i + 1;
				if (' ' == *pnext || '\t' == *pnext) {
					pnext ++;
					size_t bytes = pbuff + max_length - pnext;
					memmove(pbuff + i, pnext, bytes);
					pbuff[i+bytes] = '\0';
					max_length -= pnext - (pbuff + i);
					continue;
				}
			}
			return pnext;
		} else if ('\n' == pbuff[i]) {
			pbuff[i] = '\0';
			if (!b_searched)
				b_searched = true;
			pnext = pbuff + i + 1;
			if (' ' == *pnext || '\t' == *pnext) {
				pnext ++;
				size_t bytes = pbuff + max_length - pnext;
				memmove(pbuff + i, pnext, bytes);
				pbuff[i+bytes] = '\0';
				max_length -= pnext - (pbuff + i);
				continue;
			}
			return pnext;
		}
	}
	return NULL;
}

static bool ical_check_empty_line(const char *pline)
{	
	while ('\0' != *pline) {
		if (' ' != *pline && '\t' != *pline) {
			return false;
		}
	}
	return true;
}

static std::shared_ptr<ICAL_PARAM> ical_retrieve_param(char *ptag)
{
	char *ptr;
	char *pnext;
	
	ptr = strchr(ptag, '=');
	if (NULL == ptr) {
		return NULL;
	}
	*ptr = '\0';
	ptr ++;
	auto piparam = ical_new_param(ptag);
	if (NULL == piparam) {
		return NULL;
	}
	do {
		pnext = ical_get_tag_comma(ptr);
		if (!piparam->append_paramval(ptr))
			return NULL;
	} while ((ptr = pnext) != NULL);
	return piparam;
}

static std::shared_ptr<ICAL_LINE> ical_retrieve_tag(char *ptag)
{
	char *ptr;
	char *pnext;
	
	ptr = strchr(ptag, ';');
	if (NULL != ptr) {
		*ptr = '\0';
	}
	auto piline = ical_new_line(ptag);
	if (NULL == piline) {
		return NULL;
	}
	if (NULL == ptr) {
		return piline;
	}
	ptr ++;
	do {
		pnext = ical_get_tag_semicolon(ptr);
		auto piparam = ical_retrieve_param(ptr);
		if (NULL == piparam) {
			return nullptr;
		}
		if (piline->append_param(piparam) < 0)
			return nullptr;
	} while ((ptr = pnext) != NULL);
	return piline;
}

static bool ical_check_base64(ICAL_LINE *piline)
{
	return std::find_if(piline->param_list.cbegin(), piline->param_list.cend(),
	       [](const auto &e) { return strcasecmp(e->name.c_str(), "ENCODING") == 0; }) !=
	       piline->param_list.cend();
}

static BOOL ical_retrieve_value(ICAL_LINE *piline, char *pvalue)
{
	char *ptr;
	char *ptr1;
	char *pnext;
	char *pnext1;
	std::shared_ptr<ICAL_VALUE> pivalue;
	
	auto b_base64 = ical_check_base64(piline);
	ptr = pvalue;
	do {
		pnext = ical_get_value_sep(ptr, ';');
		if (!b_base64) {
			ptr1 = strchr(ptr, '=');
			if (NULL != ptr1) {
				*ptr1 = '\0';
			}
		} else {
			ptr1 = NULL;
		}
		if (NULL == ptr1) {
			pivalue = ical_new_value(NULL);
			ptr1 = ptr;
		} else {
			pivalue = ical_new_value(ptr);
			ptr1 ++;
		}
		if (pivalue == nullptr)
			return FALSE;
		if (piline->append_value(pivalue) < 0)
			return false;
		do {
			pnext1 = ical_get_value_sep(ptr1, ',');
			if ('\0' == *ptr1) {
				if (!pivalue->append_subval(nullptr))
					return FALSE;
			} else {
				if (!pivalue->append_subval(ptr1))
					return FALSE;
			}
		} while ((ptr1 = pnext1) != NULL);
	} while ((ptr = pnext) != NULL);
	return TRUE;
}

static void ical_unescape_string(char *pstring)
{
	int i;
	int tmp_len;
	
	tmp_len = strlen(pstring);
	for (i=0; i<tmp_len; i++) {
		if ('\\' == pstring[i]) {
			if ('\\' == pstring[i + 1] || ';' == pstring[i + 1] ||
				',' == pstring[i + 1]) {
				memmove(pstring + i, pstring + i + 1, tmp_len - i);
				pstring[tmp_len] = '\0';
				tmp_len --;
			} else if ('n' == pstring[i + 1] || 'N' == pstring[i + 1]) {
				pstring[i] = '\r';
				pstring[i + 1] = '\n';
			}
		}
	}
}

static inline bool r1something(const char *s)
{
	return strcasecmp(s, "ATTACH") == 0 || strcasecmp(s, "COMMENT") == 0 ||
	       strcasecmp(s, "DESCRIPTION") == 0 || strcasecmp(s, "X-ALT-DESC") == 0 ||
	       strcasecmp(s, "LOCATION") == 0 || strcasecmp(s, "SUMMARY") == 0 ||
	       strcasecmp(s, "CONTACT") == 0 || strcasecmp(s, "URL") == 0 ||
	       strcasecmp(s, "UID") == 0 || strcasecmp(s, "TZNAME") == 0 ||
	       strcasecmp(s, "TZURL") == 0 || strcasecmp(s, "PRODID") == 0 ||
	       strcasecmp(s, "VERSION") == 0;
}

static bool ical_retrieve_component(ICAL_COMPONENT *pcomponent,
    char *in_buff, char **ppnext)
{
	char *pline;
	char *pnext;
	size_t length;
	std::shared_ptr<ICAL_LINE> piline;
	LINE_ITEM tmp_item;
	
	ical_clear_component(pcomponent);
	pline = in_buff;
	length = strlen(in_buff);
	do {
		pnext = ical_get_string_line(pline, length - (pline - in_buff));
		if (ical_check_empty_line(pline))
			continue;
		if (!ical_retrieve_line_item(pline, &tmp_item))
			break;
		if (0 == strcasecmp(tmp_item.ptag, "BEGIN")) {
			if (NULL == tmp_item.pvalue) {
				break;
			}
			auto pcomponent1 = ical_new_component(tmp_item.pvalue);
			if (NULL == pcomponent1) {
				break;
			}
			if (!ical_retrieve_component(pcomponent1.get(), pnext, &pnext)) {
				break;
			}
			if (pcomponent->append_comp(pcomponent1) < 0)
				break;
			continue;
		}
		if (0 == strcasecmp(tmp_item.ptag, "END")) {
			if (tmp_item.pvalue == nullptr ||
			    strcasecmp(pcomponent->m_name.c_str(), tmp_item.pvalue) != 0)
				break;
			if (NULL != ppnext) {
				*ppnext = pnext;
			}
			return true;
		}
		piline = ical_retrieve_tag(tmp_item.ptag);
		if (NULL == piline) {
			break;
		}
		if (pcomponent->append_line(piline) < 0)
			break;

		if (NULL != tmp_item.pvalue) {
			if (r1something(piline->m_name.c_str())) {
				auto pivalue = ical_new_value(nullptr);
				if (NULL == pivalue) {
					break;
				}
				if (piline->append_value(pivalue) < 0)
					break;
				ical_unescape_string(tmp_item.pvalue);
				if (!pivalue->append_subval(tmp_item.pvalue))
					break;
			} else if (!ical_retrieve_value(piline.get(), tmp_item.pvalue)) {
				break;
			}
		}
	} while ((pline = pnext) != NULL);
	ical_clear_component(pcomponent);
	return false;
}

bool ical_retrieve(ICAL *pical, char *in_buff)
{
	char *pline;
	char *pnext;
	size_t length;
	LINE_ITEM tmp_item;
	
	ical_clear_component(pical);
	pnext = in_buff;
	length = strlen(in_buff);
	do {
		pline = pnext;
		pnext = ical_get_string_line(pline, length - (pline - in_buff));
		if (NULL == pnext) {
			ical_clear_component(pical);
			return false;
		}
	} while (ical_check_empty_line(pline));
	if (!ical_retrieve_line_item(pline, &tmp_item)) {
		ical_clear_component(pical);
		return false;
	}
	if (0 == strcasecmp(tmp_item.ptag, "BEGIN") &&
		NULL != pnext && (NULL != tmp_item.pvalue &&
		0 == strcasecmp(tmp_item.pvalue, "VCALENDAR"))) {
		return ical_retrieve_component(pical, pnext, NULL);
	}
	ical_clear_component(pical);
	return false;
}

static size_t ical_serialize_tag_string(char *pbuff,
	size_t max_length, const char *string)
{
	size_t i;
	BOOL b_quote;
	size_t tmp_len;
	
	b_quote = FALSE;
	tmp_len = strlen(string);
	if (tmp_len > max_length) {
		return max_length;
	}
	for (i=0; i<tmp_len; i++) {
		if (',' == string[i] || ';' == string[i] || ':' == string[i]) {
			b_quote = TRUE;
		}
	}
	if (TRUE == b_quote) {
		if (tmp_len + 2 >= max_length) {
			return max_length;
		}
		pbuff[0] = '"';
		memcpy(pbuff + 1, string, tmp_len);
		pbuff[tmp_len + 1] = '"';
		return tmp_len + 2;
	} else {
		memcpy(pbuff, string, tmp_len);
		return tmp_len;
	}
}

static size_t ical_serialize_value_string(char *pbuff,
	size_t max_length, int line_offset, const char *string)
{
	size_t i;
	size_t offset;
	size_t tmp_len;
	
	if (line_offset >= MAX_LINE) {
		line_offset %= MAX_LINE;
	}
	offset = 0;
	tmp_len = strlen(string);
	for (i=0; i<tmp_len; i++) {
		if (offset >= max_length) {
			return offset;
		}
		if (line_offset >= MAX_LINE) {
			if (offset + 3 >= max_length) {
				return max_length;
			}
			memcpy(pbuff + offset, "\r\n ", 3);
			offset += 3;
			line_offset = 0;
		}
		if ('\\' == string[i] || ';' == string[i] || ',' == string[i]) {
			if (offset + 1 >= max_length) {
				return max_length;
			}
			pbuff[offset] = '\\';
			offset ++;
			if (line_offset >= 0) {
				line_offset ++;
			}
		} else if ('\n' == string[i] || ('\r' ==
			string[i] && '\n' == string[i + 1])) {
			if (offset + 1 >= max_length) {
				return max_length;
			}
			pbuff[offset] = '\\';
			offset ++;
			pbuff[offset] = 'n';
			offset ++;
			if ('\r' == string[i]) {
				i ++;
			}
			if (line_offset >= 0) {
				line_offset += 2;
			}
			continue;
		}
		pbuff[offset] = string[i];
		offset ++;
		if (line_offset >= 0) {
			line_offset ++;
		}
	}
	return offset;
}

static size_t ical_serialize_component(ICAL_COMPONENT *pcomponent,
	char *out_buff, size_t max_length)
{
	size_t offset1;
	BOOL need_comma;
	size_t line_begin;
	BOOL need_semicolon;
	
	size_t offset = gx_snprintf(out_buff, max_length, "BEGIN:%s\r\n",
	                pcomponent->m_name.c_str());
	if (offset >= max_length) {
		return 0;
	}
	for (auto piline : pcomponent->line_list) {
		line_begin = offset;
		offset += gx_snprintf(out_buff + offset,
		          max_length - offset, "%s", piline->m_name.c_str());
		if (offset >= max_length) {
			return 0;
		}
		for (const auto &piparam : piline->param_list) {
			if (offset + 1 >= max_length) {
				return 0;
			}
			out_buff[offset] = ';';
			offset ++;
			offset += gx_snprintf(out_buff + offset,
			          max_length - offset, "%s=", piparam->name.c_str());
			if (offset >= max_length) {
				return 0;
			}
			need_comma = FALSE;
			for (const auto &pdata2 : piparam->paramval_list) {
				if (FALSE == need_comma) {
					need_comma = TRUE;
				} else {
					if (offset + 1 >= max_length) {
						return 0;
					}
					out_buff[offset] = ',';
					offset ++;
				}
				offset += ical_serialize_tag_string(out_buff + offset,
				          max_length - offset, pdata2.c_str());
				if (offset >= max_length) {
					return 0;
				}
			}
		}
		out_buff[offset] = ':';
		offset ++;
		if (offset >= max_length) {
			return 0;
		}
		need_semicolon = FALSE;
		for (const auto &pivalue : piline->value_list) {
			if (FALSE == need_semicolon) {
				need_semicolon = TRUE;
			} else {
				if (offset + 1 >= max_length) {
					return 0;
				}
				out_buff[offset] = ';';
				offset ++;
			}
			if ('\0' != pivalue->name[0]) {
				offset += gx_snprintf(out_buff + offset,
				          max_length - offset, "%s=", pivalue->name.c_str());
				if (offset >= max_length) {
					return 0;
				}
			}
			need_comma = FALSE;
			for (const auto &pnv2 : pivalue->subval_list) {
				if (FALSE == need_comma) {
					need_comma = TRUE;
				} else {
					if (offset + 1 >= max_length) {
						return 0;
					}
					out_buff[offset] = ',';
					offset ++;
				}
				if (pnv2.has_value()) {
					offset += ical_serialize_value_string(
						out_buff + offset, max_length - offset,
					          offset - line_begin, pnv2->c_str());
					if (offset >= max_length) {
						return 0;
					}
				}
			}
		}
		if (offset + 2 >= max_length) {
			return 0;
		}
		out_buff[offset] = '\r';
		offset ++;
		out_buff[offset] = '\n';
		offset ++;
	}
	for (auto comp : pcomponent->component_list) {
		offset1 = ical_serialize_component(comp.get(), out_buff + offset, max_length - offset);
		if (0 == offset1) {
			return 0;
		}
		offset += offset1;
	}
	offset += gx_snprintf(out_buff + offset, max_length - offset,
	          "END:%s\r\n", pcomponent->m_name.c_str());
	if (offset >= max_length) {
		return 0;
	}
	return offset;
}

bool ical_serialize(ICAL *pical, char *out_buff, size_t max_length)
{
	if (0 == ical_serialize_component(pical, out_buff, max_length)) {
		return false;
	}
	return true;
}

bool ICAL_PARAM::append_paramval(const char *paramval)
{
	try {
		paramval_list.push_back(paramval);
		return true;
	} catch (...) {
	}
	return false;
}

static ical_svlist *ical_get_subval_list_internal(const ical_vlist *pvalue_list, const char *name)
{
	auto end = pvalue_list->cend();
	auto it  = std::find_if(pvalue_list->cbegin(), end,
	           [=](const auto &e) { return strcasecmp(e->name.c_str(), name) == 0; });
	if (it == end)
		return nullptr;
	return &it->get()->subval_list;
}

static const char *ical_get_first_subvalue_by_name_internal(
    const ical_vlist *pvalue_list, const char *name)
{
	if ('\0' == name[0]) {
		return NULL;
	}
	auto plist = ical_get_subval_list_internal(pvalue_list, name);
	if (NULL == plist) {
		return NULL;
	}
	if (plist->size() != 1)
		return NULL;
	const auto &pnode = plist->front();
	return pnode.has_value() ? pnode->c_str() : nullptr;
}

const char *ICAL_LINE::get_first_subvalue_by_name(const char *name)
{
	return ical_get_first_subvalue_by_name_internal(&value_list, name);
}

const char *ICAL_LINE::get_first_subvalue()
{
	if (value_list.size() == 0)
		return NULL;
	const auto &pivalue = value_list.front();
	if ('\0' != pivalue->name[0]) {
		return NULL;
	}
	if (pivalue->subval_list.size() != 1)
		return NULL;
	const auto &pnv2 = pivalue->subval_list.front();
	return pnv2.has_value() ? pnv2->c_str() : nullptr;
}

ical_svlist *ICAL_LINE::get_subval_list(const char *name)
{
	return ical_get_subval_list_internal(&value_list, name);
}

std::shared_ptr<ICAL_LINE> ical_new_simple_line(const char *name, const char *value)
{
	auto piline = ical_new_line(name);
	if (NULL == piline) {
		return NULL;
	}
	auto pivalue = ical_new_value(nullptr);
	if (NULL == pivalue) {
		return NULL;
	}
	if (piline->append_value(pivalue) < 0)
		return nullptr;
	if (!pivalue->append_subval(value))
		return NULL;
	return piline;
}

bool ical_parse_utc_offset(const char *str_offset, int *phour, int *pminute)
{
	int hour;
	int minute;
	int factor;
	char tmp_buff[8];
	char str_zone[16];
	
	gx_strlcpy(str_zone, str_offset, GX_ARRAY_SIZE(str_zone));
	HX_strrtrim(str_zone);
	HX_strltrim(str_zone);
	if ('-' == str_zone[0]) {
		factor = 1;
	} else if ('+' == str_zone[0]) {
		factor = -1;
	} else {
		return false;
	}
	if (!HX_isdigit(str_zone[1]) || !HX_isdigit(str_zone[2]) ||
	    !HX_isdigit(str_zone[3]) || !HX_isdigit(str_zone[4]))
		return false;
	tmp_buff[0] = str_zone[1];
	tmp_buff[1] = str_zone[2];
	tmp_buff[2] = '\0';
	hour = atoi(tmp_buff);
	if (hour < 0 || hour > 23) {
		return false;
	}

	tmp_buff[0] = str_zone[3];
	tmp_buff[1] = str_zone[4];
	tmp_buff[2] = '\0';
	minute = atoi(tmp_buff);
	if (minute < 0 || minute > 59) {
		return false;
	}
	*phour = factor * hour;
	*pminute = factor * minute;
	return true;
}

bool ical_parse_date(const char *str_date, int *pyear, int *pmonth, int *pday)
{
	char tmp_buff[128];
	
	gx_strlcpy(tmp_buff, str_date, GX_ARRAY_SIZE(tmp_buff));
	HX_strrtrim(tmp_buff);
	HX_strltrim(tmp_buff);
	if (sscanf(tmp_buff, "%04d%02d%02d", pyear, pmonth, pday) < 3) {
		return false;
	}
	return true;
}

bool ical_parse_datetime(const char *str_datetime, bool *b_utc, ICAL_TIME *pitime)
{
	int len;
	char tsep;
	char tmp_buff[128];
	
	gx_strlcpy(tmp_buff, str_datetime, GX_ARRAY_SIZE(tmp_buff));
	HX_strrtrim(tmp_buff);
	HX_strltrim(tmp_buff);
	len = strlen(tmp_buff);
	if ('Z' == tmp_buff[len - 1]) {
		*b_utc = true;
		len --;
		tmp_buff[len] = '\0';
	} else {
		*b_utc = false;
	}
	if (15 == len) {
		if (7 != sscanf(tmp_buff, "%04d%02d%02d%c%02d%02d%02d",
			&pitime->year, &pitime->month, &pitime->day, &tsep,
			&pitime->hour, &pitime->minute, &pitime->second)) {
			return false;
		}
		pitime->leap_second = 0;
	} else if (17 == len) {
		if (8 != sscanf(tmp_buff, "%04d%02d%02d%c%02d%02d%02d%02d",
			&pitime->year, &pitime->month, &pitime->day, &tsep,
			&pitime->hour, &pitime->minute, &pitime->second,
			&pitime->leap_second)) {
			return false;
		}
	} else {
		return false;
	}
	if ('T' != tsep) {
		return false;
	}
	return true;
}

int ical_cmp_time(ICAL_TIME itime1, ICAL_TIME itime2)
{
	if (itime1.year > itime2.year) {
		return 1;
	} else if (itime1.year < itime2.year) {
		return -1;
	}
	if (itime1.month > itime2.month) {
		return 1;
	} else if (itime1.month < itime2.month) {
		return -1;
	}
	if (itime1.day > itime2.day) {
		return 1;
	} else if (itime1.day < itime2.day) {
		return -1;
	}
	if (itime1.hour > itime2.hour) {
		return 1;
	} else if (itime1.hour < itime2.hour) {
		return -1;
	}
	if (itime1.minute > itime2.minute) {
		return 1;
	} else if (itime1.minute < itime2.minute) {
		return -1;
	}
	if (itime1.second > itime2.second) {
		return 1;
	} else if (itime1.second < itime2.second) {
		return -1;
	}
	if (itime1.leap_second > 59 && itime2.leap_second <= 59) {
		return 1;
	}
	return 0;
}

static bool ical_check_leap_year(unsigned int year)
{
	if ((0 == year%4 && 0 != year%100) || (0 == year%400)) {
		return true;
	}
	return false;
}

unsigned int ical_get_dayofweek(unsigned int year, unsigned int month,
    unsigned int day)
{
	return (day += month < 3 ? year -- : year - 2, 23*month/9
			+ day + 4 + year/4 - year/100 + year/400) % 7; 	
}

unsigned int ical_get_dayofyear(unsigned int year, unsigned int month,
    unsigned int day)
{
	static const int days[2][12] = {
		{0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334},
		{0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335}};
	if (ical_check_leap_year(year))
		return days[1][month - 1] + day;
	return days[0][month - 1] + day;
}

unsigned int ical_get_monthdays(unsigned int year, unsigned int month)
{
	static const int days[2][12] = {
		{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31},
		{31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31}};
	if (ical_check_leap_year(year))
		return days[1][month - 1];
	return days[0][month - 1];
}

int ical_get_monthweekorder(int day)
{	
	return (day - 1)/7 + 1;
}

int ical_get_negative_monthweekorder(
	int year, int month, int day)
{
	return (day - ical_get_monthdays(year, month))/7 - 1;
}

int ical_get_yearweekorder(int year, int month, int day)
{
	return (ical_get_dayofyear(year, month, day) - 1)/7 + 1;
}

int ical_get_negative_yearweekorder(
	int year, int month, int day)
{
	int yearday;
	int yeardays;
	
	if (ical_check_leap_year(year))
		yeardays = 366;
	else
		yeardays = 365;
	yearday = ical_get_dayofyear(year, month, day);
	return (yearday - yeardays)/7 - 1;
}

int ical_get_dayofmonth(int year, int month, int order, int dayofweek)
{
	int day;
	int tmp_dow;
	int monthdays;
	
	if (order > 0) {
		tmp_dow = ical_get_dayofweek(year, month, 1);
		if (dayofweek >= tmp_dow) {
			day = 7*(order - 1) + 1 + dayofweek - tmp_dow;
		} else {
			day = 7*order + 1 + dayofweek - tmp_dow;
		}
	} else {
		monthdays = ical_get_monthdays(year, month);
		tmp_dow = ical_get_dayofweek(year, month, monthdays);
		if (tmp_dow >= dayofweek) {
			day = monthdays - tmp_dow + 7*(order + 1) + dayofweek;
		} else {
			day = monthdays - tmp_dow + 7*order + dayofweek;
		}
	}
	return day;
}

void ical_get_itime_from_yearday(int year, int yearday, ICAL_TIME *pitime)
{
	static const int days[2][13] = {
		{0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365},
		{0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335, 366}};
	
	pitime->year = year;
	if (ical_check_leap_year(year)) {
		for (pitime->month=1; pitime->month<=12; pitime->month++) {
			if (yearday <= days[1][pitime->month]) {
				pitime->day = yearday - days[1][pitime->month - 1];
				return;
			}
		}
	} else {
		for (pitime->month=1; pitime->month<=12; pitime->month++) {
			if (yearday <= days[0][pitime->month]) {
				pitime->day = yearday - days[0][pitime->month - 1];
				return;
			}
		}
	}
}

static unsigned int ical_get_yearweeks(unsigned int year)
{
	auto dayofweek = ical_get_dayofweek(year, 1, 1);
	/*
	 * DOW    CW     YEARTYPE        DOW    CW     #WKS
	 * JAN01  JAN01  (EXAMPLE)       DEC31  DEC31  INYEAR
	 * mo     01     regular (2001)  mo     53/01  52
	 * mo     01     leap    (2024)  tu     53/01  52
	 * tu     01     regular (2002)  tu     53/01  52
	 * tu     01     leap    (2008)  we     53/01  52
	 * we     01     regular (2003)  we     53/01  52
	 * we     01     leap    (2020)  th     53/00  53
	 * th     01     regular (2009)  th     53/00  53
	 * th     01     leap    (2004)  fr     53/00  53
	 * fr     00     regular (2010)  fr     52/00  52
	 * fr     00     leap    (2016)  sa     52/00  52
	 * sa     00     regular (2005)  sa     52/00  52
	 * sa     00     leap    (2028)  su     52     52
	 * su     00     regular (2006)  su     52     52
	 * su     00     leap    (2012)  mo     53/01  52
	 */
	return dayofweek == 4 || (dayofweek == 3 && ical_check_leap_year(year)) ? 53 : 52;
}

static int ical_get_weekofyear(int year, int month,
	int day, int weekstart, BOOL *pb_yeargap)
{
	int dayofweek;
	unsigned int weeknumber;
	
	*pb_yeargap = FALSE;
	dayofweek = ical_get_dayofweek(year, month, day) - weekstart + 1;
	if (dayofweek <= 0) {
		dayofweek += 7;
	}
	weeknumber = (ical_get_dayofyear(year, month, day) - dayofweek + 10)/7;
	if (weeknumber < 1) {
		*pb_yeargap = TRUE;
		weeknumber = ical_get_yearweeks(year - 1);
	} else if (weeknumber > ical_get_yearweeks(year)) {
		*pb_yeargap = TRUE;
		weeknumber = 1;
	}
	return weeknumber;
}

static int ical_get_negative_weekofyear(int year, int month,
	int day, int weekstart, BOOL *pb_yeargap)
{
	*pb_yeargap = FALSE;
	auto dayofweek = ical_get_dayofweek(year, month, day) - weekstart + 1;
	if (dayofweek <= 0) {
		dayofweek += 7;
	}
	auto weeknumber = (ical_get_dayofyear(year, month, day) - dayofweek + 10)/7;
	auto yearweeks = ical_get_yearweeks(year);
	if (weeknumber < 1) {
		*pb_yeargap = TRUE;
		return -1;
	} else if (weeknumber > ical_get_yearweeks(year)) {
		*pb_yeargap = TRUE;
		return -ical_get_yearweeks(year + 1);
	}
	return weeknumber - yearweeks - 1;
}

void ical_add_year(ICAL_TIME *pitime, int years)
{
	pitime->year += years;
	if (0 == years % 4) {
		return;
	}
	if (2 == pitime->month && 29 == pitime->day) {
		pitime->day = 28;
	}
}

void ical_add_month(ICAL_TIME *pitime, int months)
{
	int monthdays;
	
	pitime->year += months/12;
	pitime->month += months%12;
	if (pitime->month > 12) {
		pitime->year ++;
		pitime->month -= 12;
	}
	monthdays = ical_get_monthdays(pitime->year, pitime->month);
	if (pitime->day > monthdays) {
		pitime->day = monthdays;
	}
}

void ical_add_day(ICAL_TIME *pitime, int days)
{
	int yearday;
	
	yearday = ical_get_dayofyear(pitime->year,
				pitime->month, pitime->day);
	yearday += days;
	while (true) {
		if (ical_check_leap_year(pitime->year)) {
			if (yearday > 366) {
				pitime->year ++;
				pitime->month = 1;
				pitime->day = 1;
				yearday -= 366;
				continue;
			}
		} else {
			if (yearday > 365) {
				pitime->year ++;
				pitime->month = 1;
				pitime->day = 1;
				yearday -= 365;
				continue;
			}
		}
		break;
	}
	ical_get_itime_from_yearday(pitime->year, yearday, pitime);
}

void ical_subtract_day(ICAL_TIME *pitime, int days)
{
	int yearday;
	
	yearday = ical_get_dayofyear(pitime->year,
				pitime->month, pitime->day);
	while (yearday <= days) {
		days -= yearday;
		pitime->year --;
		pitime->month = 12;
		pitime->day = 31;
		if (ical_check_leap_year(pitime->year))
			yearday = 366;
		else
			yearday = 365;
	}
	yearday -= days;
	ical_get_itime_from_yearday(pitime->year, yearday, pitime);
}

int ical_delta_day(ICAL_TIME itime1, ICAL_TIME itime2)
{
	int yearday;
	int monthdays;
	int delta_days;
	
	if (ical_cmp_time(itime1, itime2) < 0) {
		return ical_delta_day(itime2, itime1);
	}
	delta_days = 0;
	while (itime2.year != itime1.year) {
		yearday = ical_get_dayofyear(itime2.year, itime2.month, itime2.day); 
		if (ical_check_leap_year(itime2.year))
			delta_days += 366 + 1 - yearday;
		else
			delta_days += 365 + 1 - yearday;
		itime2.year ++;
		itime2.month = 1;
		itime2.day = 1;
	}
	while (itime2.month != itime1.month) {
		monthdays = ical_get_monthdays(itime2.year, itime2.month);
		delta_days += monthdays + 1 - itime2.day;
		itime2.month ++;
		itime2.day = 1;
	}
	delta_days += itime1.day - itime2.day;
	return delta_days;
}

void ical_add_hour(ICAL_TIME *pitime, int hours)
{
	if (hours > 23) {
		ical_add_day(pitime, hours/24);
	}
	pitime->hour += hours%24;
	if (pitime->hour > 23) {
		ical_add_day(pitime, 1);
		pitime->hour -= 24;
	}
}

void ical_add_minute(ICAL_TIME *pitime, int minutes)
{
	if (minutes > 59) {
		ical_add_hour(pitime, minutes/60);
	}
	pitime->minute += minutes%60;
	if (pitime->minute > 59) {
		ical_add_hour(pitime, 1);
		pitime->minute -= 60;
	}
}

void ical_add_second(ICAL_TIME *pitime, int seconds)
{
	if (seconds > 59) {
		ical_add_minute(pitime, seconds/60);
	}
	pitime->second += seconds%60;
	if (pitime->second > 59) {
		ical_add_minute(pitime, 1);
		pitime->second -= 60;
	}
}

bool ical_parse_byday(const char *str_byday, int *pdayofweek, int *pweekorder)
{
	char *pbegin;
	BOOL b_negative;
	char tmp_num[3];
	char tmp_buff[16];
	
	gx_strlcpy(tmp_buff, str_byday, GX_ARRAY_SIZE(tmp_buff));
	HX_strrtrim(tmp_buff);
	HX_strltrim(tmp_buff);
	if ('-' == tmp_buff[0]) {
		b_negative = TRUE;
		pbegin = tmp_buff + 1;
	} else if ('+' == tmp_buff[0]) {
		b_negative = FALSE;
		pbegin = tmp_buff + 1;
	} else {
		b_negative = FALSE;
		pbegin = tmp_buff;
	}
	if (!HX_isdigit(*pbegin)) {
		*pweekorder = 0;
		goto PARSE_WEEKDAY;
	}
	tmp_num[0] = *pbegin;
	pbegin ++;
	tmp_num[1] = '\0';
	if (HX_isdigit(*pbegin)) {
		tmp_num[1] = *pbegin;
		pbegin ++;
		tmp_num[2] = '\0';
	}
	*pweekorder = atoi(tmp_num);
	if (*pweekorder < 1 || *pweekorder > 53) {
		return false;
	}
	if (TRUE == b_negative) {
		*pweekorder *= -1;
	}
 PARSE_WEEKDAY:
	if (0 == strcasecmp(pbegin, "SU")) {
		*pdayofweek = 0;
	} else if (0 == strcasecmp(pbegin, "MO")) {
		*pdayofweek = 1;
	} else if (0 == strcasecmp(pbegin, "TU")) {
		*pdayofweek = 2;
	} else if (0 == strcasecmp(pbegin, "WE")) {
		*pdayofweek = 3;
	} else if (0 == strcasecmp(pbegin, "TH")) {
		*pdayofweek = 4;
	} else if (0 == strcasecmp(pbegin, "FR")) {
		*pdayofweek = 5;
	} else if (0 == strcasecmp(pbegin, "SA")) {
		*pdayofweek = 6;
	} else {
		return false;
	}
	return true;
}

bool ical_parse_duration(const char *str_duration, long *pseconds)
{
	int day;
	int week;
	int hour;
	int minute;
	int second;
	int factor;
	BOOL b_time;
	char *ptoken;
	char *ptoken1;
	char tmp_buff[128];
	
	gx_strlcpy(tmp_buff, str_duration, GX_ARRAY_SIZE(tmp_buff));
	HX_strrtrim(tmp_buff);
	HX_strltrim(tmp_buff);
	ptoken = tmp_buff;
	if ('+' == *ptoken) {
		factor = 1;
		ptoken ++;
	} else if ('-' == *ptoken) {
		factor = -1;
		ptoken ++;
	} else {
		factor = 1;
	}
	if ('P' != *ptoken) {
		return false;
	}
	ptoken ++;
	b_time = FALSE;
	week = -1;
	day = -1;
	hour = -1;
	minute = -1;
	second = -1;
	for (ptoken1=ptoken; '\0'!=*ptoken1; ptoken1++) {
		switch (*ptoken1) {
		case 'W':
			if (ptoken1 == ptoken || -1 != week || TRUE == b_time) {
				return false;
			}
			*ptoken1 = '\0';
			week = atoi(ptoken);
			ptoken = ptoken1 + 1;
			break;
		case 'D':
			if (ptoken1 == ptoken || -1 != day || TRUE == b_time) {
				return false;
			}
			*ptoken1 = '\0';
			day = atoi(ptoken);
			ptoken = ptoken1 + 1;
			break;
		case 'T':
			if (ptoken != ptoken1 || TRUE == b_time) {
				return false;
			}
			b_time = TRUE;
			ptoken = ptoken1 + 1;
			break;
		case 'H':
			if (ptoken1 == ptoken || -1 != hour || FALSE == b_time) {
				return false;
			}
			*ptoken1 = '\0';
			hour = atoi(ptoken);
			ptoken = ptoken1 + 1;
			break;
		case 'M':
			if (ptoken1 == ptoken || -1 != minute || FALSE == b_time) {
				return false;
			}
			*ptoken1 = '\0';
			minute = atoi(ptoken);
			ptoken = ptoken1 + 1;
			break;
		case 'S':
			if (ptoken1 == ptoken || -1 != second || FALSE == b_time) {
				return false;
			}
			*ptoken1 = '\0';
			second = atoi(ptoken);
			ptoken = ptoken1 + 1;
			break;
		default:
			if (!HX_isdigit(*ptoken1))
				return false;
			break;
		}
	}
	*pseconds = 0;
	if (-1 != week) {
		*pseconds += 7*24*60*60*week;
	}
	if (-1 != day) {
		*pseconds += 24*60*60*day;
	}
	if (-1 != hour) {
		*pseconds += 60*60*hour;
	}
	if (-1 != minute) {
		*pseconds += 60*minute;
	}
	if (-1 != second) {
		*pseconds += second;
	}
	*pseconds *= factor;
	return true;
}

static const char *ical_get_datetime_offset(std::shared_ptr<ICAL_COMPONENT> ptz_component,
    ICAL_TIME itime)
{
	int hour;
	int month;
	int minute;
	int second;
	bool b_utc;
	int weekorder;
	int dayofweek;
	int dayofmonth;
	time_t tmp_time;
	BOOL b_standard;
	BOOL b_daylight;
	ICAL_TIME itime1;
	ICAL_TIME itime2;
	struct tm tmp_tm;
	std::shared_ptr<ICAL_LINE> piline;
	const char *pvalue;
	const char *pvalue1;
	const char *pvalue2;
	ICAL_TIME itime_standard;
	ICAL_TIME itime_daylight;
	const char *standard_offset = nullptr, *daylight_offset = nullptr;
	
	b_standard = FALSE;
	b_daylight = FALSE;
	for (auto pcomponent : ptz_component->component_list) {
		if (strcasecmp(pcomponent->m_name.c_str(), "STANDARD") != 0 &&
		    strcasecmp(pcomponent->m_name.c_str(), "DAYLIGHT") != 0)
			return NULL;
		piline = pcomponent->get_line("DTSTART");
		if (NULL == piline) {
			return NULL;
		}
		if (piline->get_first_paramval("TZID") != nullptr)
			return NULL;
		pvalue = piline->get_first_subvalue();
		if (NULL == pvalue) {
			return NULL;
		}
		if (!ical_parse_datetime(pvalue, &b_utc, &itime1) || b_utc)
			return NULL;
		if (ical_cmp_time(itime, itime1) < 0) {
			continue;
		}
		piline = pcomponent->get_line("RRULE");
		if (NULL == piline) {
			goto FOUND_COMPONENT;
		}
		pvalue = piline->get_first_subvalue_by_name("UNTIL");
		if (NULL == pvalue) {
			goto FOUND_COMPONENT;
		}
		if (!ical_parse_datetime(pvalue, &b_utc, &itime2)) {
			itime2.hour = 0;
			itime2.minute = 0;
			itime2.second = 0;
			itime2.leap_second = 0;
			if (!ical_parse_date(pvalue, &itime2.year,
			    &itime2.month, &itime2.day))
				return nullptr;
		} else {
			if (!ical_datetime_to_utc(nullptr, pvalue, &tmp_time))
				return nullptr;
			piline = pcomponent->get_line("TZOFFSETTO");
			if (NULL == piline) {
				return NULL;
			}
			pvalue = piline->get_first_subvalue();
			if (NULL == pvalue) {
				return NULL;
			}
			if (!ical_parse_utc_offset(pvalue, &hour, &minute))
				return nullptr;
			tmp_time -= 60*60*hour + 60*minute;
			make_gmtm(tmp_time, &tmp_tm);
			itime2.year = tmp_tm.tm_year + 1900;
			itime2.month = tmp_tm.tm_mon + 1;
			itime2.day = tmp_tm.tm_mday;
			itime2.hour = tmp_tm.tm_hour;
			itime2.minute = tmp_tm.tm_min;
			itime2.second = tmp_tm.tm_sec;
			itime2.leap_second = 0;
		}
		if (ical_cmp_time(itime, itime2) > 0) {
			continue;
		}
 FOUND_COMPONENT:
		piline = pcomponent->get_line("TZOFFSETTO");
		if (NULL == piline) {
			return NULL;
		}
		pvalue = piline->get_first_subvalue();
		if (NULL == pvalue) {
			return NULL;
		}
		if (strcasecmp(pcomponent->m_name.c_str(), "STANDARD") == 0) {
			b_standard = TRUE;
			standard_offset = pvalue;
			itime_standard = itime1;
		} else {
			b_daylight = TRUE;
			daylight_offset = pvalue;
			itime_daylight = itime1;
		}
		piline = pcomponent->get_line("RRULE");
		if (NULL != piline) {
			pvalue = piline->get_first_subvalue_by_name("FREQ");
			if (NULL == pvalue || 0 != strcasecmp(pvalue, "YEARLY")) {
				return NULL;
			}
			pvalue = piline->get_first_subvalue_by_name("BYDAY");
			pvalue1 = piline->get_first_subvalue_by_name("BYMONTHDAY");
			if ((NULL == pvalue && NULL == pvalue1) ||
				(NULL != pvalue && NULL != pvalue1)) {
				return NULL;
			}
			pvalue2 = piline->get_first_subvalue_by_name("BYMONTH");
			if (NULL == pvalue2) {
				month = itime1.month;
			} else {
				month = atoi(pvalue2);
				if (month < 1 || month > 12) {
					return NULL;
				}
			}
			if (strcasecmp(pcomponent->m_name.c_str(), "STANDARD") == 0) {
				itime_standard.year = itime.year;
				itime_standard.month = month;
			} else {
				itime_daylight.year = itime.year;
				itime_daylight.month = month;
			}
			if (NULL != pvalue) {
				if (!ical_parse_byday(pvalue, &dayofweek, &weekorder))
					return NULL;
				if (weekorder > 5 || weekorder < -5 || 0 == weekorder) {
					return NULL;
				}
				dayofmonth = ical_get_dayofmonth(itime.year,
						itime.month, weekorder, dayofweek);
			} else {
				dayofmonth = atoi(pvalue1);
				if (abs(dayofmonth) < 1 || abs(dayofmonth) > 31) {
					return NULL;
				}
				if (dayofmonth < 0) {
					dayofmonth += ical_get_monthdays(
								itime.year, month) + 1;
				}
				if (dayofmonth <= 0) {
					return NULL;
				}
			}
			pvalue = piline->get_first_subvalue_by_name("BYHOUR");
			if (NULL == pvalue) {
				hour = itime1.hour;
			} else {
				hour = atoi(pvalue);
				if (hour < 0 || hour > 23) {
					return NULL;
				}
			}
			pvalue = piline->get_first_subvalue_by_name("BYMINUTE");
			if (NULL == pvalue) {
				minute = itime1.minute;
			} else {
				minute = atoi(pvalue);
				if (minute < 0 || minute > 59) {
					return NULL;
				}
			}
			pvalue = piline->get_first_subvalue_by_name("BYSECOND");
			if (NULL == pvalue) {
				second = itime1.second;
			} else {
				second = atoi(pvalue);
				if (second < 0 || second > 59) {
					return NULL;
				}
			}
			if (strcasecmp(pcomponent->m_name.c_str(), "STANDARD") == 0) {
				itime_standard.day = dayofmonth;
				itime_standard.hour = hour;
				itime_standard.minute = minute;
				itime_standard.second = second;
				itime_standard.leap_second = 0;
			} else {
				itime_daylight.day = dayofmonth;
				itime_daylight.hour = hour;
				itime_daylight.minute = minute;
				itime_daylight.second = second;
				itime_daylight.leap_second = 0;
			}
		} else {
			if (strcasecmp(pcomponent->m_name.c_str(), "STANDARD") == 0)
				itime_standard.year = itime.year;
			else
				itime_daylight.year = itime.year;
		}
		if (TRUE == b_standard && TRUE == b_daylight) {
			break;
		}
	}
	if (FALSE == b_standard && FALSE == b_daylight) {
		return NULL;
	}
	if (TRUE == b_standard && FALSE == b_daylight) {
		return standard_offset;
	}
	if (FALSE == b_standard && TRUE == b_daylight) {
		return daylight_offset;
	}
	if (itime.year != itime_standard.year ||
		itime.year != itime_daylight.year) {
		return NULL;
	}
	if (ical_cmp_time(itime_standard, itime_daylight) >= 0) {
		if (ical_cmp_time(itime, itime_daylight) < 0 ||
			ical_cmp_time(itime, itime_standard) >= 0) {
			return standard_offset;
		} else {
			return daylight_offset;
		}
	} else {
		if (ical_cmp_time(itime, itime_standard) < 0 ||
			ical_cmp_time(itime, itime_daylight) >= 0) {
			return daylight_offset;
		} else {
			return standard_offset;
		}
	}
}

bool ical_itime_to_utc(std::shared_ptr<ICAL_COMPONENT> ptz_component,
    ICAL_TIME itime, time_t *ptime)
{
	int hour_offset;
	struct tm tmp_tm;
	int minute_offset;
	const char *str_offset;
	
	if (itime.leap_second >= 60) {
		tmp_tm.tm_sec = itime.leap_second;
	} else {
		tmp_tm.tm_sec = itime.second;
	}
	tmp_tm.tm_min = itime.minute;
	tmp_tm.tm_hour = itime.hour;
	tmp_tm.tm_mday = itime.day;
	tmp_tm.tm_mon = itime.month - 1;
	tmp_tm.tm_year = itime.year - 1900;
	tmp_tm.tm_wday = 0;
	tmp_tm.tm_yday = 0;
	tmp_tm.tm_isdst = 0;
	*ptime = make_gmtime(&tmp_tm);
	if (NULL == ptz_component) {
		return true;
	}
	str_offset = ical_get_datetime_offset(ptz_component, itime);
	if (NULL == str_offset) {
		return false;
	}
	if (!ical_parse_utc_offset(str_offset, &hour_offset, &minute_offset))
		return false;
	*ptime += 60*60*hour_offset + 60*minute_offset;
	return true;
}

bool ical_datetime_to_utc(std::shared_ptr<ICAL_COMPONENT> ptz_component,
	const char *str_datetime, time_t *ptime)
{
	bool b_utc;
	ICAL_TIME itime;
	struct tm tmp_tm;
	
	if (!ical_parse_datetime(str_datetime, &b_utc, &itime))
		return false;
	if (itime.leap_second >= 60) {
		tmp_tm.tm_sec = itime.leap_second;
	} else {
		tmp_tm.tm_sec = itime.second;
	}
	if (b_utc) {
		tmp_tm.tm_min = itime.minute;
		tmp_tm.tm_hour = itime.hour;
		tmp_tm.tm_mday = itime.day;
		tmp_tm.tm_mon = itime.month - 1;
		tmp_tm.tm_year = itime.year - 1900;
		tmp_tm.tm_wday = 0;
		tmp_tm.tm_yday = 0;
		tmp_tm.tm_isdst = 0;
		*ptime = make_gmtime(&tmp_tm);
		return true;
	}
	return ical_itime_to_utc(ptz_component, itime, ptime);
}

bool ical_utc_to_datetime(std::shared_ptr<ICAL_COMPONENT> ptz_component,
	time_t utc_time, ICAL_TIME *pitime)
{
	int hour;
	int minute;
	time_t tmp_time;
	struct tm tmp_tm;
	std::shared_ptr<ICAL_LINE> piline;
	const char *pvalue;
	
	if (NULL == ptz_component) {
		/* UTC time */
		make_gmtm(utc_time, &tmp_tm);
		pitime->year = tmp_tm.tm_year + 1900;
		pitime->month = tmp_tm.tm_mon + 1;
		pitime->day = tmp_tm.tm_mday;
		pitime->hour = tmp_tm.tm_hour;
		pitime->minute = tmp_tm.tm_min;
		pitime->second = tmp_tm.tm_sec;
		pitime->leap_second = 0;
		return true;
	}
	for (auto pcomponent : ptz_component->component_list) {
		if (strcasecmp(pcomponent->m_name.c_str(), "STANDARD") != 0 &&
		    strcasecmp(pcomponent->m_name.c_str(), "DAYLIGHT") != 0)
			return false;
		piline = pcomponent->get_line("TZOFFSETTO");
		if (NULL == piline) {
			return false;
		}
		pvalue = piline->get_first_subvalue();
		if (NULL == pvalue) {
			return false;
		}
		if (!ical_parse_utc_offset(pvalue, &hour, &minute))
			return false;
		tmp_time = utc_time - 60*60*hour - 60*minute;
		make_gmtm(tmp_time, &tmp_tm);
		pitime->year = tmp_tm.tm_year + 1900;
		pitime->month = tmp_tm.tm_mon + 1;
		pitime->day = tmp_tm.tm_mday;
		pitime->hour = tmp_tm.tm_hour;
		pitime->minute = tmp_tm.tm_min;
		pitime->second = tmp_tm.tm_sec;
		pitime->leap_second = 0;
		if (!ical_itime_to_utc(ptz_component, *pitime, &tmp_time))
			return false;
		if (tmp_time == utc_time) {
			return true;
		}
	}
	return false;
}

static bool ical_parse_until(std::shared_ptr<ICAL_COMPONENT> ptz_component,
	const char *str_until, time_t *ptime)
{
	bool b_utc;
	ICAL_TIME itime;
	
	if (!ical_parse_datetime(str_until, &b_utc, &itime)) {
		if (!ical_parse_date(str_until, &itime.year,
		    &itime.month, &itime.day))
			return false;
		itime.hour = 0;
		itime.minute = 0;
		itime.second = 0;
		itime.leap_second = 0;
		return ical_itime_to_utc(ptz_component, itime, ptime);
	} else {
		if (!b_utc)
			return ical_itime_to_utc(ptz_component, itime, ptime);
		return ical_datetime_to_utc(nullptr, str_until, ptime);
	}
}

static bool ical_hint_bitmap(unsigned char *pbitmap, unsigned int index)
{
	int bits;
	int bytes;
	unsigned char mask;
	
	bytes = index/8;
	bits = index%8;
	mask = 1 << bits;
	return pbitmap[bytes] & mask;
}

static void ical_set_bitmap(unsigned char *pbitmap, unsigned int index)
{
	int bits;
	int bytes;
	unsigned char mask;
	
	bytes = index/8;
	bits = index%8;
	mask = 1 << bits;
	pbitmap[bytes] |= mask;
}

static int ical_hint_rrule(ICAL_RRULE *pirrule, ICAL_TIME itime)
{
	int yearday;
	int yeardays;
	int dayofweek;
	int weekorder;
	int nweekorder;
	BOOL b_yeargap;
	
	if (pirrule->by_mask[RRULE_BY_MONTH])
		if (!ical_hint_bitmap(pirrule->month_bitmap, itime.month - 1))
			return RRULE_BY_MONTH;
	if (pirrule->by_mask[RRULE_BY_WEEKNO]) {
		weekorder = ical_get_weekofyear(itime.year, itime.month,
					itime.day, pirrule->weekstart, &b_yeargap);
		if (TRUE == b_yeargap && ICAL_FREQUENCY_YEAR
			== pirrule->frequency) {
			return RRULE_BY_WEEKNO;
		}
		nweekorder = ical_get_negative_weekofyear(
				itime.year, itime.month, itime.day,
				pirrule->weekstart, &b_yeargap);
		if (TRUE == b_yeargap && ICAL_FREQUENCY_YEAR
			== pirrule->frequency) {
			return RRULE_BY_WEEKNO;
		}
		if (!ical_hint_bitmap(pirrule->week_bitmap, weekorder - 1) &&
		    !ical_hint_bitmap(pirrule->nweek_bitmap, -nweekorder - 1))
			return RRULE_BY_WEEKNO;
	}
	if (pirrule->by_mask[RRULE_BY_YEARDAY]) {
		if (ical_check_leap_year(itime.year))
			yeardays = 366;
		else
			yeardays = 365;
		yearday = ical_get_dayofyear(itime.year, itime.month, itime.day);
		if (!ical_hint_bitmap(pirrule->yday_bitmap, yearday - 1) &&
		    !ical_hint_bitmap(pirrule->nyday_bitmap, yeardays - yearday))
			return RRULE_BY_YEARDAY;
	}
	if (pirrule->by_mask[RRULE_BY_MONTHDAY])
		if (!ical_hint_bitmap(pirrule->mday_bitmap, itime.day - 1) &&
		    !ical_hint_bitmap(pirrule->nmday_bitmap,
		    ical_get_monthdays(itime.year, itime.month) - itime.day))
			return RRULE_BY_MONTHDAY;
	if (pirrule->by_mask[RRULE_BY_DAY]) {
		dayofweek = ical_get_dayofweek(itime.year,
						itime.month, itime.day);
		if (ICAL_FREQUENCY_WEEK == pirrule->frequency) {
			weekorder = ical_delta_day(itime, pirrule->base_itime)/7 + 1;
			nweekorder = -(ical_delta_day(itime,
				pirrule->next_base_itime) - 1)/7 - 1;
		} else {
			if (ICAL_FREQUENCY_MONTH == pirrule->frequency ||
			    pirrule->by_mask[RRULE_BY_MONTH]) {
				weekorder = ical_get_monthweekorder(itime.day);
				nweekorder = ical_get_negative_monthweekorder(
							itime.year, itime.month, itime.day);
			} else {
				weekorder = ical_get_yearweekorder(
					itime.year, itime.month, itime.day);
				nweekorder = ical_get_negative_yearweekorder(
							itime.year, itime.month, itime.day);
			}
		}
		if (!ical_hint_bitmap(pirrule->wday_bitmap, 7 * (weekorder - 1) + dayofweek) &&
		    !ical_hint_bitmap(pirrule->nwday_bitmap, 7 * (-nweekorder - 1) + dayofweek))
			return RRULE_BY_DAY;
	}
	if (pirrule->by_mask[RRULE_BY_HOUR])
		if (!ical_hint_bitmap(pirrule->hour_bitmap, itime.hour))
			return RRULE_BY_HOUR;
	if (pirrule->by_mask[RRULE_BY_MINUTE])
		if (!ical_hint_bitmap(pirrule->minute_bitmap, itime.minute))
			return RRULE_BY_MINUTE;
	if (pirrule->by_mask[RRULE_BY_SECOND])
		if (!ical_hint_bitmap(pirrule->second_bitmap, itime.second))
			return RRULE_BY_SECOND;
	return 0;
}

static bool ical_hint_setpos(ICAL_RRULE *pirrule)
{
	if (!ical_hint_bitmap(pirrule->setpos_bitmap, pirrule->cur_setpos - 1) &&
	    !ical_hint_bitmap(pirrule->nsetpos_bitmap, pirrule->setpos_count - pirrule->cur_setpos))
		return false;
	return true;
}

static ICAL_TIME ical_next_rrule_itime(ICAL_RRULE *pirrule,
	int hint_result, ICAL_TIME itime)
{
	int dayofweek;
	
	if (0 == hint_result) {
		switch (pirrule->real_frequency) {
		case ICAL_FREQUENCY_YEAR:
			ical_add_year(&itime, pirrule->interval);
			break;
		case ICAL_FREQUENCY_MONTH:
			if (pirrule->real_frequency == pirrule->frequency) {
				ical_add_month(&itime, pirrule->interval);
			} else {
				ical_add_month(&itime, 1);
			}
			break;
		case ICAL_FREQUENCY_WEEK:
			if (pirrule->real_frequency == pirrule->frequency) {
				ical_add_day(&itime, 7*pirrule->interval);
			} else {
				ical_add_day(&itime, 7);
			}
			break;
		case ICAL_FREQUENCY_DAY:
			if (pirrule->real_frequency == pirrule->frequency) {
				ical_add_day(&itime, pirrule->interval);
			} else {
				ical_add_day(&itime, 1);
			}
			break;
		case ICAL_FREQUENCY_HOUR:
			if (pirrule->real_frequency == pirrule->frequency) {
				ical_add_hour(&itime, pirrule->interval);
			} else {
				ical_add_hour(&itime, 1);
			}
			break;
		case ICAL_FREQUENCY_MINUTE:
			if (pirrule->real_frequency == pirrule->frequency) {
				ical_add_minute(&itime, pirrule->interval);
			} else {
				ical_add_minute(&itime, 1);
			}
			break;
		case ICAL_FREQUENCY_SECOND:
			if (pirrule->real_frequency == pirrule->frequency) {
				ical_add_second(&itime, pirrule->interval);
			} else {
				ical_add_second(&itime, 1);
			}
			break;
		}
		return itime;
	}
	switch (pirrule->frequency) {
	case ICAL_FREQUENCY_YEAR:
	case ICAL_FREQUENCY_MONTH:
		switch (hint_result) {
		case RRULE_BY_MONTH:
			dayofweek = ical_get_dayofweek(itime.year,
							itime.month, itime.day);
			ical_add_month(&itime, 1);
			if (pirrule->by_mask[RRULE_BY_WEEKNO])
				itime.day = ical_get_dayofmonth(itime.year,
								itime.month, 1, dayofweek);
			if (pirrule->by_mask[RRULE_BY_YEARDAY] ||
			    pirrule->by_mask[RRULE_BY_MONTHDAY] ||
			    pirrule->by_mask[RRULE_BY_DAY])
				itime.day = 1;
			if (pirrule->by_mask[RRULE_BY_HOUR])
				itime.hour = 0;
			if (pirrule->by_mask[RRULE_BY_MINUTE])
				itime.minute = 0;
			if (pirrule->by_mask[RRULE_BY_SECOND])
				itime.second = 0;
			break;
		case RRULE_BY_WEEKNO:
			ical_add_day(&itime, 7);
			if (pirrule->by_mask[RRULE_BY_YEARDAY] ||
			    pirrule->by_mask[RRULE_BY_MONTHDAY] ||
			    pirrule->by_mask[RRULE_BY_DAY]) {
				dayofweek = ical_get_dayofweek(itime.year,
								itime.month, itime.day);
				if (dayofweek >= pirrule->weekstart) {
					ical_subtract_day(&itime,
						dayofweek - pirrule->weekstart);
				} else {
					ical_subtract_day(&itime, 7 +
						dayofweek - pirrule->weekstart);
				}
			}
			if (pirrule->by_mask[RRULE_BY_HOUR])
				itime.hour = 0;
			if (pirrule->by_mask[RRULE_BY_MINUTE])
				itime.minute = 0;
			if (pirrule->by_mask[RRULE_BY_SECOND])
				itime.second = 0;
			break;
		case RRULE_BY_YEARDAY:
		case RRULE_BY_MONTHDAY:
		case RRULE_BY_DAY:
			ical_add_day(&itime, 1);
			if (pirrule->by_mask[RRULE_BY_HOUR])
				itime.hour = 0;
			if (pirrule->by_mask[RRULE_BY_MINUTE])
				itime.minute = 0;
			if (pirrule->by_mask[RRULE_BY_SECOND])
				itime.second = 0;
			break;
		case RRULE_BY_HOUR:
			ical_add_hour(&itime, 1);
			if (pirrule->by_mask[RRULE_BY_MINUTE])
				itime.minute = 0;
			if (pirrule->by_mask[RRULE_BY_SECOND])
				itime.second = 0;
			break;
		case RRULE_BY_MINUTE:
			ical_add_minute(&itime, 1);
			if (pirrule->by_mask[RRULE_BY_SECOND])
				itime.second = 0;
			break;
		case RRULE_BY_SECOND:
			ical_add_second(&itime, 1);
			break;
		}
		break;
	case ICAL_FREQUENCY_WEEK:
		switch (hint_result) {
		case RRULE_BY_YEARDAY:
		case RRULE_BY_MONTHDAY:
		case RRULE_BY_DAY:
			ical_add_day(&itime, 1);
			break;
		case RRULE_BY_HOUR:
			ical_add_hour(&itime, 1);
			break;
		case RRULE_BY_MINUTE:
			ical_add_minute(&itime, 1);
			break;
		case RRULE_BY_SECOND:
			ical_add_second(&itime, 1);
			break;
		default:
			ical_add_day(&itime, 7);
			break;
		}
		break;
	case ICAL_FREQUENCY_DAY:
		switch (hint_result) {
		case RRULE_BY_HOUR:
			ical_add_hour(&itime, 1);
			break;
		case RRULE_BY_MINUTE:
			ical_add_minute(&itime, 1);
			break;
		case RRULE_BY_SECOND:
			ical_add_second(&itime, 1);
			break;
		default:
			ical_add_day(&itime, 1);
			break;
		}
		break;
	case ICAL_FREQUENCY_HOUR:
		switch (hint_result) {
		case RRULE_BY_MINUTE:
			ical_add_minute(&itime, 1);
			break;
		case RRULE_BY_SECOND:
			ical_add_second(&itime, 1);
			break;
		default:
			ical_add_hour(&itime, 1);
			break;
		}
		break;
	case ICAL_FREQUENCY_MINUTE:
		switch (hint_result) {
		case RRULE_BY_SECOND:
			ical_add_second(&itime, 1);
			break;
		default:
			ical_add_minute(&itime, 1);
			break;
		}
		break;
	case ICAL_FREQUENCY_SECOND:
		ical_add_second(&itime, 1);
		break;
	}
	switch (pirrule->frequency) {
	case ICAL_FREQUENCY_YEAR:
		if (itime.year > pirrule->base_itime.year) {
			itime = pirrule->next_base_itime;
		}
		break;
	case ICAL_FREQUENCY_MONTH:
		if (itime.month > pirrule->base_itime.month) {
			itime = pirrule->next_base_itime;
		}
		break;
	case ICAL_FREQUENCY_WEEK:
		if (ical_delta_day(itime, pirrule->base_itime) >= 7) {
			itime = pirrule->next_base_itime;
		}
		break;
	case ICAL_FREQUENCY_DAY:
		if (itime.day > pirrule->base_itime.day) {
			itime = pirrule->next_base_itime;
		}
		break;
	case ICAL_FREQUENCY_HOUR:
		if (itime.hour > pirrule->base_itime.hour) {
			itime = pirrule->next_base_itime;
		}
		break;
	case ICAL_FREQUENCY_MINUTE:
		if (itime.minute > pirrule->base_itime.minute) {
			itime = pirrule->next_base_itime;
		}
		break;
	case ICAL_FREQUENCY_SECOND:
		if (itime.second > pirrule->base_itime.second) {
			itime = pirrule->next_base_itime;
		}
		break;
	}
	return itime;
}
	
static void ical_calculate_setpos(ICAL_RRULE *pirrule)
{
	int hint_result;
	 ICAL_TIME itime;
	
	pirrule->cur_setpos = 0;
	pirrule->setpos_count = 0;
	itime = pirrule->base_itime;
	while (ical_cmp_time(pirrule->next_base_itime, itime) > 0) {
		hint_result = ical_hint_rrule(pirrule, itime);
		if (0 == hint_result) {
			pirrule->setpos_count ++;
		}
		itime = ical_next_rrule_itime(pirrule, hint_result, itime);
	}
}

static void ical_next_rrule_base_itime(ICAL_RRULE *pirrule)
{
	pirrule->next_base_itime = pirrule->base_itime;
	switch (pirrule->frequency) {
	case ICAL_FREQUENCY_YEAR:
		ical_add_year(&pirrule->next_base_itime, pirrule->interval);
		break;
	case ICAL_FREQUENCY_MONTH:
		ical_add_month(&pirrule->next_base_itime, pirrule->interval);
		break;
	case ICAL_FREQUENCY_WEEK:
		ical_add_day(&pirrule->next_base_itime, 7*pirrule->interval);
		break;
	case ICAL_FREQUENCY_DAY:
		ical_add_day(&pirrule->next_base_itime, pirrule->interval);
		break;
	case ICAL_FREQUENCY_HOUR:
		ical_add_hour(&pirrule->next_base_itime, pirrule->interval);
		break;
	case ICAL_FREQUENCY_MINUTE:
		ical_add_minute(&pirrule->next_base_itime, pirrule->interval);
		break;
	case ICAL_FREQUENCY_SECOND:
		ical_add_second(&pirrule->next_base_itime, pirrule->interval);
		break;
	}
}

/* ptz_component can be NULL, represents UTC */
bool ical_parse_rrule(std::shared_ptr<ICAL_COMPONENT> ptz_component,
    time_t start_time, const ical_vlist *pvalue_list, ICAL_RRULE *pirrule)
{
	int i;
	int tmp_int;
	int dayofweek;
	int weekorder;
	int cmp_result;
	int hint_result;
	ICAL_TIME itime;
	time_t until_time;
	const char *pvalue;
	ICAL_TIME base_itime;
	
	memset(pirrule, 0, sizeof(ICAL_RRULE));
	pvalue = ical_get_first_subvalue_by_name_internal(
								pvalue_list, "FREQ");
	if (NULL == pvalue) {
		return false;
	}
	if (0 == strcasecmp(pvalue, "SECONDLY")) {
		pirrule->frequency = ICAL_FREQUENCY_SECOND;
	} else if (0 == strcasecmp(pvalue, "MINUTELY")) {
		pirrule->frequency = ICAL_FREQUENCY_MINUTE;
	} else if (0 == strcasecmp(pvalue, "HOURLY")) {
		pirrule->frequency = ICAL_FREQUENCY_HOUR;
	} else if (0 == strcasecmp(pvalue, "DAILY")) {
		pirrule->frequency = ICAL_FREQUENCY_DAY;
	} else if (0 == strcasecmp(pvalue, "WEEKLY")) {
		pirrule->frequency = ICAL_FREQUENCY_WEEK;
	} else if (0 == strcasecmp(pvalue, "MONTHLY")) {
		pirrule->frequency = ICAL_FREQUENCY_MONTH;
	} else if (0 == strcasecmp(pvalue, "YEARLY")) {
		pirrule->frequency = ICAL_FREQUENCY_YEAR;
	} else {
		return false;
	}
	pirrule->real_frequency = pirrule->frequency;
	pvalue = ical_get_first_subvalue_by_name_internal(
							pvalue_list, "INTERVAL");
	if (NULL == pvalue) {
		pirrule->interval = 1;
	} else {
		pirrule->interval = atoi(pvalue);
		if (pirrule->interval <= 0) {
			return false;
		}
	}
	pvalue = ical_get_first_subvalue_by_name_internal(
								pvalue_list, "COUNT");
	if (NULL == pvalue) {
		pirrule->total_count = 0;
	} else {
		pirrule->total_count = atoi(pvalue);
		if (pirrule->total_count <= 0) {
			return false;
		}
	}
	pvalue = ical_get_first_subvalue_by_name_internal(
								pvalue_list, "UNTIL");
	if (NULL != pvalue) {
		if (0 != pirrule->total_count) {
			return false;
		}
		if (!ical_parse_until(ptz_component, pvalue, &until_time))
			return false;
		if (until_time <= start_time) {
			return false;
		}
		pirrule->b_until = true;
		ical_utc_to_datetime(ptz_component,
			until_time, &pirrule->until_itime);
	}
	ical_utc_to_datetime(ptz_component,
		start_time, &pirrule->instance_itime);
	auto pbysecond_list = ical_get_subval_list_internal(pvalue_list, "BYSECOND");
	if (NULL != pbysecond_list) {
		for (const auto &pnv2 : *pbysecond_list) {
			if (!pnv2.has_value())
				return false;
			tmp_int = strtol(pnv2->c_str(), nullptr, 0);
			if (tmp_int < 0 || tmp_int > 59) {
				return false;
			}
			ical_set_bitmap(pirrule->second_bitmap, tmp_int);
		}
		if (pirrule->real_frequency > ICAL_FREQUENCY_SECOND) {
			pirrule->real_frequency = ICAL_FREQUENCY_SECOND;
		}
		pirrule->by_mask[RRULE_BY_SECOND] = true;
	}
	auto pbyminute_list = ical_get_subval_list_internal(pvalue_list, "BYMINUTE");
	if (NULL != pbyminute_list) {
		for (const auto &pnv2 : *pbyminute_list) {
			if (!pnv2.has_value())
				return false;
			tmp_int = strtol(pnv2->c_str(), nullptr, 0);
			if (tmp_int < 0 || tmp_int > 59) {
				return false;
			}
			ical_set_bitmap(pirrule->minute_bitmap, tmp_int);
		}
		if (pirrule->real_frequency > ICAL_FREQUENCY_MINUTE) {
			pirrule->real_frequency = ICAL_FREQUENCY_MINUTE;
		}
		pirrule->by_mask[RRULE_BY_MINUTE] = true;
	}
	auto pbyhour_list = ical_get_subval_list_internal(pvalue_list, "BYHOUR");
	if (NULL != pbyhour_list) {
		for (const auto &pnv2 : *pbyhour_list) {
			if (!pnv2.has_value())
				return false;
			tmp_int = strtol(pnv2->c_str(), nullptr, 0);
			if (tmp_int < 0 || tmp_int > 23) {
				return false;
			}
			ical_set_bitmap(pirrule->hour_bitmap, tmp_int);
		}
		if (pirrule->real_frequency > ICAL_FREQUENCY_HOUR) {
			pirrule->real_frequency = ICAL_FREQUENCY_HOUR;
		}
		pirrule->by_mask[RRULE_BY_HOUR] = true;
	}
	auto pbymday_list = ical_get_subval_list_internal(pvalue_list, "BYMONTHDAY");
	if (NULL != pbymday_list) {
		for (const auto &pnv2 : *pbymday_list) {
			if (!pnv2.has_value())
				return false;
			tmp_int = strtol(pnv2->c_str(), nullptr, 0);
			if (tmp_int < -31 || 0 == tmp_int || tmp_int > 31) {
				return false;
			}
			if (tmp_int > 0) {
				ical_set_bitmap(pirrule->mday_bitmap, tmp_int - 1);
			} else {
				ical_set_bitmap(pirrule->nmday_bitmap, -tmp_int - 1);
			}
		}
		if (pirrule->real_frequency > ICAL_FREQUENCY_DAY) {
			pirrule->real_frequency = ICAL_FREQUENCY_DAY;
		}
		pirrule->by_mask[RRULE_BY_MONTHDAY] = true;
	}
	auto pbyyday_list = ical_get_subval_list_internal(pvalue_list, "BYYEARDAY");
	if (NULL != pbyyday_list) {
		for (const auto &pnv2 : *pbyyday_list) {
			if (!pnv2.has_value())
				return false;
			tmp_int = strtol(pnv2->c_str(), nullptr, 0);
			if (tmp_int < -366 || 0 == tmp_int || tmp_int > 366) {
				return false;
			}	
			if (tmp_int > 0) {
				ical_set_bitmap(pirrule->yday_bitmap, tmp_int - 1);
			} else {
				ical_set_bitmap(pirrule->nyday_bitmap, -tmp_int - 1);
			}
		}
		if (pirrule->real_frequency > ICAL_FREQUENCY_DAY) {
			pirrule->real_frequency = ICAL_FREQUENCY_DAY;
		}
		pirrule->by_mask[RRULE_BY_YEARDAY] = true;
	}
	auto pbywday_list = ical_get_subval_list_internal(pvalue_list, "BYDAY");
	if (NULL != pbywday_list) {
		if (ICAL_FREQUENCY_WEEK != pirrule->frequency &&
			ICAL_FREQUENCY_MONTH != pirrule->frequency &&
			ICAL_FREQUENCY_YEAR != pirrule->frequency) {
			return false;
		}
		for (const auto &pnv2 : *pbywday_list) {
			if (!pnv2.has_value())
				return false;
			if (!ical_parse_byday(pnv2->c_str(), &dayofweek, &weekorder))
				return false;
			if (ICAL_FREQUENCY_MONTH == pirrule->frequency) {
				if (weekorder > 5 || weekorder < -5) {
					return false;
				} else if (weekorder > 0) {
					ical_set_bitmap(pirrule->wday_bitmap,
						7*(weekorder - 1) + dayofweek);
				} else if (weekorder < 0) {
					ical_set_bitmap(pirrule->nwday_bitmap,
						7 * (-weekorder - 1) + dayofweek);
				} else {
					for (i=0; i<5; i++) {
						ical_set_bitmap(pirrule->wday_bitmap, 7*i + dayofweek); 
					}
				}
			} else if (ICAL_FREQUENCY_YEAR == pirrule->frequency) {
				if (weekorder > 0) {
					ical_set_bitmap(pirrule->wday_bitmap,
						7*(weekorder - 1) + dayofweek);
				} else if (weekorder < 0) {
					ical_set_bitmap(pirrule->nwday_bitmap,
						7 * (-weekorder - 1) + dayofweek);
				} else {
					for (i=0; i<53; i++) {
						ical_set_bitmap(pirrule->wday_bitmap, 7*i + dayofweek); 
					}
				}
			} else {
				if (0 != weekorder) {
					return false;
				}
				ical_set_bitmap(pirrule->wday_bitmap, dayofweek);
			}
		}
		if (pirrule->real_frequency > ICAL_FREQUENCY_DAY) {
			pirrule->real_frequency = ICAL_FREQUENCY_DAY;
		}
		pirrule->by_mask[RRULE_BY_DAY] = true;
	}
	auto pbywnum_list = ical_get_subval_list_internal(pvalue_list, "BYWEEKNO");
	if (NULL != pbywnum_list) {
		for (const auto &pnv2 : *pbywnum_list) {
			if (!pnv2.has_value())
				return false;
			tmp_int = strtol(pnv2->c_str(), nullptr, 0);
			if (tmp_int < -53 || 0 == tmp_int || tmp_int > 53) {
				return false;
			}	
			if (tmp_int > 0) {
				ical_set_bitmap(pirrule->week_bitmap, tmp_int - 1);
			} else {
				ical_set_bitmap(pirrule->nweek_bitmap, -tmp_int - 1);
			}
		}
		if (pirrule->real_frequency > ICAL_FREQUENCY_WEEK) {
			pirrule->real_frequency = ICAL_FREQUENCY_WEEK;
		}
		pirrule->by_mask[RRULE_BY_WEEKNO] = true;
	}
	auto pbymonth_list = ical_get_subval_list_internal(pvalue_list, "BYMONTH");
	if (NULL != pbymonth_list) {
		for (const auto &pnv2 : *pbymonth_list) {
			if (!pnv2.has_value())
				return false;
			tmp_int = strtol(pnv2->c_str(), nullptr, 0);
			if (tmp_int < 1 || tmp_int > 12) {
				return false;
			}
			ical_set_bitmap(pirrule->month_bitmap, tmp_int - 1);
		}
		if (pirrule->real_frequency > ICAL_FREQUENCY_MONTH) {
			pirrule->real_frequency = ICAL_FREQUENCY_MONTH;
		}
		pirrule->by_mask[RRULE_BY_MONTH] = true;
	}
	auto psetpos_list = ical_get_subval_list_internal(pvalue_list, "BYSETPOS");
	if (NULL != psetpos_list) {
		switch (pirrule->frequency) {
		case ICAL_FREQUENCY_SECOND:
			return false;
		case ICAL_FREQUENCY_MINUTE:
			if (pirrule->real_frequency != ICAL_FREQUENCY_SECOND) {
				return false;
			}
			if (60*pirrule->interval > 366) {
				return false;
			}
			break;
		case ICAL_FREQUENCY_HOUR:
			if (pirrule->real_frequency != ICAL_FREQUENCY_MINUTE) {
				return false;
			}
			if (60*pirrule->interval > 366) {
				return false;
			}
			break;
		case ICAL_FREQUENCY_DAY:
			if (pirrule->real_frequency != ICAL_FREQUENCY_HOUR) {
				return false;
			}
			if (24*pirrule->interval > 366) {
				return false;
			}
			break;
		case ICAL_FREQUENCY_WEEK:
			if (pirrule->real_frequency == ICAL_FREQUENCY_DAY) {
				break;
			} else if (pirrule->real_frequency == ICAL_FREQUENCY_HOUR) {
				if (7*24*pirrule->interval > 366) {
					return false;
				}
				break;
			}
			return false;
		case ICAL_FREQUENCY_MONTH:
			if (pirrule->real_frequency == ICAL_FREQUENCY_DAY) {
				if (31*pirrule->interval > 366) {
					return false;
				}
			} else if (pirrule->real_frequency == ICAL_FREQUENCY_WEEK) {
				if (5*pirrule->interval > 366) {
					return false;
				}
			} else {
				return false;
			}
			break;
		case ICAL_FREQUENCY_YEAR:
			if (pirrule->real_frequency == ICAL_FREQUENCY_DAY) {
				if (pirrule->interval > 1) {
					return false;
				}
			} else if (pirrule->real_frequency == ICAL_FREQUENCY_WEEK) {
				if (pirrule->interval > 8) {
					return false;
				}
			} else if (pirrule->real_frequency == ICAL_FREQUENCY_MONTH) {
				if (pirrule->interval > 30) {
					return false;
				}
			} else {
				return false;
			}
			break;
		}
		for (const auto &pnv2 : *psetpos_list) {
			if (!pnv2.has_value())
				return false;
			tmp_int = strtol(pnv2->c_str(), nullptr, 0);
			if (tmp_int < -366 || 0 == tmp_int || tmp_int > 366) {
				return false;
			}
			if (tmp_int > 0) {
				ical_set_bitmap(pirrule->setpos_bitmap, tmp_int - 1);
			} else {
				ical_set_bitmap(pirrule->nsetpos_bitmap, -tmp_int - 1);
			}
		}
		pirrule->by_mask[RRULE_BY_SETPOS] = true;
	}
	pvalue = ical_get_first_subvalue_by_name_internal(
								pvalue_list, "WKST");
	if (NULL != pvalue) {
		if (0 == strcasecmp(pvalue, "SU")) {
			pirrule->weekstart = 0;
		} else if (0 == strcasecmp(pvalue, "MO")) {
			pirrule->weekstart = 1;
		} else if (0 == strcasecmp(pvalue, "TU")) {
			pirrule->weekstart = 2;
		} else if (0 == strcasecmp(pvalue, "WE")) {
			pirrule->weekstart = 3;
		} else if (0 == strcasecmp(pvalue, "TH")) {
			pirrule->weekstart = 4;
		} else if (0 == strcasecmp(pvalue, "FR")) {
			pirrule->weekstart = 5;
		} else if (0 == strcasecmp(pvalue, "SA")) {
			pirrule->weekstart = 6;
		} else {
			return false;
		}
	} else {
		if (NULL != pbywnum_list) {
			pirrule->weekstart = 1;
		} else {
			pirrule->weekstart = 0;
		}
	}
	itime = pirrule->instance_itime;
	switch (pirrule->frequency) {
	case ICAL_FREQUENCY_MINUTE:
		if (NULL != pbysecond_list) {
			itime.second = 0;
		}
		break;
	case ICAL_FREQUENCY_HOUR:
		if (NULL != pbyminute_list) {
			itime.minute = 0;
		}
		if (NULL != pbysecond_list) {
			itime.second = 0;
		}
		break;
	case ICAL_FREQUENCY_DAY:
		if (NULL != pbyhour_list) {
			itime.hour = 0;
		}
		if (NULL != pbyminute_list) {
			itime.minute = 0;
		}
		if (NULL != pbysecond_list) {
			itime.second = 0;
		}
		break;
	case ICAL_FREQUENCY_WEEK:
		if (NULL != pbywday_list) {
			dayofweek = ical_get_dayofweek(itime.year,
								itime.month, itime.day);
			if (dayofweek >= pirrule->weekstart) {
				ical_subtract_day(&itime,
					dayofweek - pirrule->weekstart);
			} else {
				ical_subtract_day(&itime, 7 +
					dayofweek - pirrule->weekstart);
			}
		}
		if (NULL != pbyhour_list) {
			itime.hour = 0;
		}
		if (NULL != pbyminute_list) {
			itime.minute = 0;
		}
		if (NULL != pbysecond_list) {
			itime.second = 0;
		}
		break;
	case ICAL_FREQUENCY_MONTH:
		if (NULL != pbyyday_list ||
			NULL != pbymday_list ||
			NULL != pbywday_list) {
			itime.day = 1;
		}
		if (NULL != pbyhour_list) {
			itime.hour = 0;
		}
		if (NULL != pbyminute_list) {
			itime.minute = 0;
		}
		if (NULL != pbysecond_list) {
			itime.second = 0;
		}
		break;
	case ICAL_FREQUENCY_YEAR:
		if (NULL != pbymonth_list) {
			itime.month = 1;
		}
		if (NULL != pbyyday_list ||
			NULL != pbymday_list ||
			NULL != pbywday_list) {
			itime.day = 1;
		}
		if (NULL != pbyhour_list) {
			itime.hour = 0;
		}
		if (NULL != pbyminute_list) {
			itime.minute = 0;
		}
		if (NULL != pbysecond_list) {
			itime.second = 0;
		}
		break;
	}
	pirrule->base_itime = itime;
	ical_next_rrule_base_itime(pirrule);
	if (pirrule->by_mask[RRULE_BY_SETPOS])
		ical_calculate_setpos(pirrule);
	while (ical_cmp_time(itime, pirrule->next_base_itime) < 0) {
		if (pirrule->b_until &&
		    ical_cmp_time(itime, pirrule->until_itime) > 0)
			return false;
		hint_result = ical_hint_rrule(pirrule, itime);
		if (0 == hint_result) {
			if (pirrule->by_mask[RRULE_BY_SETPOS]) {
				pirrule->cur_setpos ++;
				if (!ical_hint_setpos(pirrule)) {
					itime = ical_next_rrule_itime(
						pirrule, hint_result, itime);
					continue;
				}
			}
			cmp_result = ical_cmp_time(itime, pirrule->instance_itime);
			if (cmp_result < 0) {
				itime = ical_next_rrule_itime(pirrule, hint_result, itime);
				continue;
			} else if (cmp_result > 0) {
				pirrule->b_start_exceptional = true;
				pirrule->real_start_itime = itime;
				pirrule->current_instance = 1;
				pirrule->next_base_itime = pirrule->base_itime;
				return true;
			}
			pirrule->current_instance = 1;
			return true;
		}
		itime = ical_next_rrule_itime(pirrule, hint_result, itime);
	}
	base_itime = pirrule->base_itime;
	itime = pirrule->instance_itime;
	pirrule->current_instance = 1;
	pirrule->instance_itime = pirrule->next_base_itime;
	if (!ical_rrule_iterate(pirrule)) {
		pirrule->total_count = 1;
		pirrule->instance_itime = itime;
	} else {
		pirrule->real_start_itime = pirrule->instance_itime;
		pirrule->next_base_itime = pirrule->base_itime;
		pirrule->base_itime = base_itime;
		pirrule->instance_itime = itime;
	}
	pirrule->current_instance = 1;
	pirrule->b_start_exceptional = true;
	return true;
}

bool ical_rrule_iterate(ICAL_RRULE *pirrule)
{
	ICAL_TIME itime;
	int hint_result;
	
	if (0 != pirrule->total_count &&
		pirrule->current_instance >= pirrule->total_count) {
		return false;
	}
	if (pirrule->b_start_exceptional) {
		itime = pirrule->real_start_itime;
		if (pirrule->b_until &&
		    ical_cmp_time(itime, pirrule->until_itime) > 0)
			return false;
		pirrule->b_start_exceptional = false;
		pirrule->current_instance ++;
		pirrule->instance_itime = itime;
		pirrule->base_itime = pirrule->next_base_itime;
		ical_next_rrule_base_itime(pirrule);
		return true;
	}
	hint_result = 0;
	itime = pirrule->instance_itime;
	while (true) {
		itime = ical_next_rrule_itime(pirrule, hint_result, itime);
		if (pirrule->b_until &&
		    ical_cmp_time(itime, pirrule->until_itime) > 0)
			return false;
		if (ical_cmp_time(itime, pirrule->next_base_itime) >= 0) {
			pirrule->base_itime = pirrule->next_base_itime;
			itime = pirrule->next_base_itime;
			ical_next_rrule_base_itime(pirrule);
			if (pirrule->by_mask[RRULE_BY_SETPOS])
				ical_calculate_setpos(pirrule);
		}
		hint_result = ical_hint_rrule(pirrule, itime);
		if (0 == hint_result) {
			if (pirrule->by_mask[RRULE_BY_SETPOS]) {
				pirrule->cur_setpos ++;
				if (!ical_hint_setpos(pirrule))
					continue;
			}
			pirrule->current_instance ++;
			pirrule->instance_itime = itime;
			return true;
		}
	}
}

int ical_rrule_weekstart(ICAL_RRULE *pirrule)
{
	return pirrule->weekstart;
}

bool ical_rrule_endless(ICAL_RRULE *pirrule)
{
	if (pirrule->total_count == 0 && !pirrule->b_until)
		return true;
	return false;
}

const ICAL_TIME* ical_rrule_until_itime(ICAL_RRULE *pirrule)
{
	if (!pirrule->b_until)
		return nullptr;
	else
		return &pirrule->until_itime;
}

int ical_rrule_total_count(ICAL_RRULE *pirrule)
{
	return pirrule->total_count;
}

bool ical_rrule_exceptional(ICAL_RRULE *pirrule)
{
	return pirrule->b_start_exceptional;
}

ICAL_TIME ical_rrule_base_itime(ICAL_RRULE *pirrule)
{
	return pirrule->base_itime;
}

int ical_rrule_sequence(ICAL_RRULE *pirrule)
{
	return pirrule->current_instance;
}

ICAL_TIME ical_rrule_instance_itime(ICAL_RRULE *pirrule)
{
	return pirrule->instance_itime;
}

int ical_rrule_interval(ICAL_RRULE *pirrule)
{
	return pirrule->interval;
}

int ical_rrule_frequency(ICAL_RRULE *pirrule)
{
	return pirrule->frequency;
}

bool ical_rrule_check_bymask(ICAL_RRULE *pirrule, int rrule_by)
{
	return pirrule->by_mask[rrule_by];
}
