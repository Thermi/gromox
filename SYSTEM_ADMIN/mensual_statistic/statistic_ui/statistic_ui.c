#include "statistic_ui.h"
#include "lang_resource.h"
#include "acl_control.h"
#include "system_log.h"
#include "list_file.h"
#include "util.h"
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define HTML_COMMON_1	\
"<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 4.0 Transitional//EN\">\n\
<HTML><HEAD><TITLE>"

/* fill html title here */

#define HTML_COMMON_2	\
"</TITLE><LINK href=\"../data/css/result.css\" type=text/css rel=stylesheet>\n\
<META http-equiv=Content-Type content=\"text/html; charset="

/* fill charset here */

#define HTML_COMMON_3	\
"\"><META content=\"MSHTML 6.00.2900.2963\" name=GENERATOR></HEAD>\n\
<BODY bottomMargin=0 leftMargin=0 topMargin=0 rightMargin=0\n\
marginheight=\"0\" marginwidth=\"0\"><CENTER>\n\
<TABLE cellSpacing=0 cellPadding=0 width=\"100%\" border=0>\n\
<TBODY><TR><TD noWrap align=middle background=\"../data/picture/di1.gif\"\n\
height=55><SPAN class=ReportTitle> "

/* fill statistic result title here */

#define HTML_COMMON_4	\
"</SPAN></TD><TD vAlign=bottom noWrap width=\"22%\"\n\
background=\"../data/picture/di1.gif\"><A href=\""

/* fill logo url link here */

#define HTML_MAIN_5	\
"\" target=_blank><IMG height=48 src=\"../data/picture/logo_bb.gif\"\n\
width=195 align=right border=0></A></TD></TR></TBODY></TABLE><BR><BR>\n\
<FORM class=SearchForm name=searchpattern method=get action="

#define HTML_MAIN_6	\
" ><INPUT type=hidden value=%s name=session />\n\
<SPAN>%s: <SELECT name=year>"

/* fill years here */

#define HTML_MAIN_7	\
"</SELECT></SPAN><SPAN>&nbsp;&nbsp;&nbsp;&nbsp;%s: <SELECT name=month>"

/* fill months here */

#define HTML_MAIN_8	\
"</SELECT></SPAN>&nbsp;&nbsp;&nbsp;&nbsp;<INPUT type=submit \n\
value=\"    %s    \" /></FORM>\n\
<TABLE cellSpacing=1 cellPadding=1 width=\"90%\"\n\
border=0><TBODY><TR><TD noWrap align=left height=23></TD></TR>\n\
</TBODY></TABLE><BR><TABLE cellSpacing=1 cellPadding=2 width=\"100%\" \n\
border=0><TBODY><TABLE class=ChartTable cellSpacing=0 cellPadding=2 \n\
width=\"100%\" border=0><TBODY><TR><TD align=middle><CENTER>\n\
<TABLE><TBODY><TR vAlign=bottom><TD>&nbsp;</TD>\n"

/* fill chart here */

#define HTML_MAIN_9	\
"<TD>&nbsp;</TD></TR><TR vAlign=center><TD>&nbsp;</TD>\n"

/* fill unit lable here */

#define HTML_MAIN_10	\
"<TD>&nbsp;</TD></TR></TBODY></TABLE><BR>\n\
<TABLE><TBODY><TR><TD width=80 bgColor=#ececec>%s</TD>\n\
<TD width=160 bgColor=#ffb055>%s</TD>\n\
<TD width=160 bgColor=#4477dd>%s</TD>\
<TD width=160 bgColor=#66f0ff>%s</TD></TR>\n"

/* fill data report here */

#define HTML_MAIN_11	\
"</TBODY></TABLE><BR></CENTER></TD></TR></TBODY></TABLE></TD></TR> \
</TBODY></TABLE></TD></TR><BR></CENTER></BODY></HTML>"

#define HTML_ERROR_5    \
"\" target=_blank><IMG height=48 src=\"../data/picture/logo_bb.gif\"\n\
width=195 align=right border=0></A></TD></TR></TBODY></TABLE><BR><BR>\n\
<P align=right><A href=admin_main target=_parent>%s</A>&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;\n\
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp\n\
</P><BR><BR>%s</CENTER></BODY></HTML>"

#define HTML_OPTION_NORMAL	"<OPTION value=%d>%d</OPTION>"

#define HTML_OPTION_SELECTED "<OPTION value=%d selected>%d</OPTION>"

#define HTML_TBCELL_BEGIN	"<TD>"
#define HTML_TBCELL_END		"</TD>\n"
#define HTML_TBLINE_BEGIN	"<TR>"
#define HTML_TBLINE_END		"</TR>\n"

#define HTML_SUMMARY_LINE	"<TR><TD colspan=4><HR></TD>"

#define HTML_CHART_SPAM	\
"<IMG title=\"%s: %d\" src=\"../data/picture/vu.png\" height=%d width=12 \
align=bottom>"

#define HTML_CHART_NORMAL	\
"<IMG title=\"%s: %d\" src=\"../data/picture/vp.png\" height=%d width=12 \
align=bottom>"


typedef struct _STATISTIC_ITEM {
	char date[16];
	int spam;
	int normal;
} STATISTIC_ITEM;

static void statistic_ui_error_html(const char *error_string);

static void statistic_ui_main_html(const char *session, int year, int month);

static BOOL statistic_ui_get_self(char *url_buff, int length);

static void statistic_ui_unencode(char *src, char *last, char *dest);

static char g_list_path[1024];
static char g_logo_link[1024];
static char g_resource_path[256];
static LANG_RESOURCE *g_lang_resource;

void statistic_ui_init(const char *list_path, const char *url_link,
	const char *resource_path)
{
	strcpy(g_list_path, list_path);
	strcpy(g_logo_link, url_link);
	strcpy(g_resource_path, resource_path);
}

int statistic_ui_run()
{
	struct tm *ptm;
	time_t temp_time;
	char *query;
	char *request;
	char *language;
	char *remote_ip;
	char *ptr1, *ptr2;
	char session[256];
	char password[256];
	char post_buff[1024];
	char search_buff[1024];
	char temp_buff[8];
	int len, year, month;

	language = getenv("HTTP_ACCEPT_LANGUAGE");
	if (NULL == language) {
		statistic_ui_error_html(NULL);
		return 0;
	}
	g_lang_resource = lang_resource_init(g_resource_path);
	if (NULL == g_lang_resource) {
		system_log_info("[statistic_ui]: fail to init language resource");
		return -1;
	}
	request = getenv("REQUEST_METHOD");
	if (NULL == request) {
		system_log_info("[statistic_ui]: fail to get REQUEST_METHOD"
			" environment!");
		return -2;
	}
	remote_ip = getenv("REMOTE_ADDR");
	if (NULL == remote_ip) {
		system_log_info("[statistic_ui]: fail to get REMOTE_ADDR environment!");
		return -3;
	}
	if (0 == strcmp(request, "GET")) {
		query = getenv("QUERY_STRING");
		if (NULL == query) {
			system_log_info("[statistic_ui]: fail to get QUERY_STRING "
				"environment!");
			statistic_ui_error_html(lang_resource_get(g_lang_resource,"ERROR_REQUEST",
				language));
			return 0;
		} else {
			len = strlen(query);
			if (0 == len || len > 256) {
				system_log_info("[statistic_ui]: query string too long!");
				statistic_ui_error_html(lang_resource_get(g_lang_resource,"ERROR_REQUEST",
					language));
				return 0;
			}
			ptr1 = search_string(query, "session=", len);
			if (NULL == ptr1) {
				system_log_info("[statistic_ui]: query string of GET "
					"format error");
				statistic_ui_error_html(lang_resource_get(g_lang_resource,"ERROR_REQUEST",
					language));
				return 0;
			}
			ptr1 += 8;
			ptr2 = search_string(query, "&year=", len);
			if (NULL == ptr2) {
				if (query + len - ptr1 > 256) {
					system_log_info("[statistic_ui]: query string of GET "
						"format error");
					statistic_ui_error_html(lang_resource_get(g_lang_resource,"ERROR_REQUEST",
						language));
					return 0;
				}
				memcpy(session, ptr1, query + len - ptr1);
				session[query + len - ptr1] = '\0';
				
				switch (acl_control_check(session, remote_ip,
					ACL_PRIVILEGE_STATUS)) {
				case ACL_SESSION_OK:
					break;
				case ACL_SESSION_TIMEOUT:
					statistic_ui_error_html(lang_resource_get(g_lang_resource,
						"ERROR_TIMEOUT", language));
					return 0;
				case ACL_SESSION_PRIVILEGE:
					statistic_ui_error_html(lang_resource_get(g_lang_resource,
						"ERROR_PRIVILEGE", language));
					return 0;
				default:
					statistic_ui_error_html(lang_resource_get(g_lang_resource,
						"ERROR_SESSION", language));
					return 0;
				}
				
				time(&temp_time);
				ptm = localtime(&temp_time);
				statistic_ui_main_html(session, ptm->tm_year + 1900,
					ptm->tm_mon + 1);
				return 0;
			}
			if (ptr2 < ptr1 || ptr2 - ptr1 > 255) {
				system_log_info("[statistic_ui]: query string of GET "
					"format error");
				statistic_ui_error_html(lang_resource_get(g_lang_resource,"ERROR_REQUEST",
					language));
				return 0;
			}
			memcpy(session, ptr1, ptr2 - ptr1);
			session[ptr2 - ptr1] = '\0';
			
			ptr1 = ptr2 + 6;
			ptr2 = search_string(query, "&month=", len);
			if (NULL == ptr2) {
				system_log_info("[statistic_ui]: query string of GET "
					"format error");
				statistic_ui_error_html(lang_resource_get(g_lang_resource,"ERROR_REQUEST",
					language));
				return 0;
			}
			if (ptr2 - ptr1 != 4) {
				system_log_info("[statistic_ui]: query string of GET "
					"format error");
				statistic_ui_error_html(lang_resource_get(g_lang_resource,"ERROR_REQUEST",
					language));
				return 0;
			}
			memcpy(temp_buff, ptr1, 4);
			temp_buff[4] = '\0';
			year = atoi(temp_buff);
			
			ptr1 = ptr2 + 7;
			if (query + len - ptr1 > 2) {
				system_log_info("[statistic_ui]: query string of GET "
					"format error");
				statistic_ui_error_html(lang_resource_get(g_lang_resource,"ERROR_REQUEST",
						language));
				return 0;
			}
			memcpy(temp_buff, ptr1, query + len - ptr1);
			temp_buff[query + len - ptr1] = '\0';
			month = atoi(temp_buff);
			
			switch (acl_control_check(session, remote_ip, ACL_PRIVILEGE_STATUS)) {
			case ACL_SESSION_OK:
				break;
			case ACL_SESSION_TIMEOUT:
				statistic_ui_error_html(lang_resource_get(g_lang_resource,"ERROR_TIMEOUT",
					language));
				return 0;
			case ACL_SESSION_PRIVILEGE:
				statistic_ui_error_html(lang_resource_get(g_lang_resource,"ERROR_PRIVILEGE",
					language));
				return 0;
			default:
				statistic_ui_error_html(lang_resource_get(g_lang_resource,"ERROR_SESSION",
					language));
				return 0;
			}
			
			statistic_ui_main_html(session, year, month);
			return 0;
		}
	} else {
		system_log_info("[statistic_ui]: unrecognized REQUEST_METHOD \"%s\"!",
			request);
		statistic_ui_error_html(lang_resource_get(g_lang_resource,"ERROR_REQUEST", language));
		return 0;
	}
}

int statistic_ui_stop()
{
	if (NULL != g_lang_resource) {
		lang_resource_free(g_lang_resource);
		g_lang_resource = NULL;
	}
	return 0;

}

void statistic_ui_free()
{
	/* do nothing */
}

static BOOL statistic_ui_get_self(char *url_buff, int length)
{
	host = getenv("HTTP_HOST");
	script = getenv("SCRIPT_NAME");
	https = getenv("HTTPS");
	if (NULL == host || NULL == script) {
		system_log_info("[statistic_ui]: fail to get"
			" HTTP_HOST or SCRIPT_NAME environment!");
		return FALSE;
	}
	if (NULL == https || 0 != strcasecmp(https, "ON")) {
		snprintf(url_buff, length, "http://%s%s", host, script);
	} else {
		snprintf(url_buff, length, "https://%s%s", host, script);
	}
	return TRUE;
}

static void statistic_ui_error_html(const char *error_string)
{
	char *language;
	
	if (NULL == error_string) {
		error_string = "fatal error!!!";
	}
	
	language = getenv("HTTP_ACCEPT_LANGUAGE");
	if (NULL == language) {
		language = "en";
	}
	printf("Content-Type:text/html;charset=%s\n\n",
		lang_resource_get(g_lang_resource,"CHARSET", language));
	printf(HTML_COMMON_1);
	printf(lang_resource_get(g_lang_resource,"ERROR_HTML_TITLE", language));
	printf(HTML_COMMON_2);
	printf(lang_resource_get(g_lang_resource,"CHARSET", language));
	printf(HTML_COMMON_3);
	printf(lang_resource_get(g_lang_resource,"ERROR_HTML_TITLE", language));
	printf(HTML_COMMON_4);
	printf(g_logo_link);
	printf(HTML_ERROR_5, lang_resource_get(g_lang_resource,"BACK_LABEL", language),
		error_string);
}

static void statistic_ui_main_html(const char *session, int year, int month)
{
	int i, len;
	int height;
	int max_num;
	int item_num;
	int first_year;
	int first_month;
	int last_year;
	int last_month;
	int total_spam;
	int total_normal;
	time_t cur_time;
	char *language;
	LIST_FILE *pfile;
	char url_buff[1024];
	STATISTIC_ITEM *pitem;
	struct tm temp_tm, *ptm;
	
	
	language = getenv("HTTP_ACCEPT_LANGUAGE");
	if (FALSE == statistic_ui_get_self(url_buff, 1024)) {
		statistic_ui_error_html(lang_resource_get(g_lang_resource,"ERROR_INTERNAL",
			language));
		return;
	}
	
	pfile = list_file_init(g_list_path, "%s:16%d%d");
	if (NULL == pfile) {
		system_log_info("[statistic_ui]: fail to open list file %s",
			g_list_path);
		statistic_ui_error_html(lang_resource_get(g_lang_resource,"ERROR_INTERNAL",
			language));
		return;
	}
	pitem = (STATISTIC_ITEM*)list_file_get_list(pfile);
	item_num = list_file_get_item_num(pfile);
	if (0 != item_num && NULL == strptime(pitem->date, "%Y-%m-%d", &temp_tm)) {
		system_log_info("[statistic_ui]: first line in list file format error!");
		statistic_ui_error_html(lang_resource_get(g_lang_resource,"ERROR_INTERNAL",language));
		return;
	}
	first_year = temp_tm.tm_year + 1900;
	first_month = temp_tm.tm_mon + 1;
	time(&cur_time);
	ptm = localtime(&cur_time);
	last_year = ptm->tm_year + 1900;
	last_month = ptm->tm_mon + 1;
	printf("Content-Type:text/html;charset=%s\n\n",
		lang_resource_get(g_lang_resource,"CHARSET", language));
	printf(HTML_COMMON_1);
	printf(lang_resource_get(g_lang_resource,"MAIN_HTML_TITLE", language));
	printf(HTML_COMMON_2);
	printf(lang_resource_get(g_lang_resource,"CHARSET", language));
	printf(HTML_COMMON_3);
	printf(lang_resource_get(g_lang_resource,"MAIN_HTML_TITLE", language));
	printf(HTML_COMMON_4);
	printf(g_logo_link);
	printf(HTML_MAIN_5);
	printf(url_buff);
	printf(HTML_MAIN_6, session, lang_resource_get(g_lang_resource,
		"MAIN_YEAR", language));
	for (i=first_year; i<=last_year; i++) {
		if (i != year) {
			printf(HTML_OPTION_NORMAL, i, i);
		} else {
			printf(HTML_OPTION_SELECTED, i, i);
		}
	}
	printf(HTML_MAIN_7, lang_resource_get(g_lang_resource,"MAIN_MONTH", language));
	for (i=1; i<=12; i++) {
		if (i != month) {
			printf(HTML_OPTION_NORMAL, i, i);	
		} else {
			printf(HTML_OPTION_SELECTED, i, i);
		}
	}
	printf(HTML_MAIN_8, lang_resource_get(g_lang_resource,"DISPLAY_LABEL", language));
	max_num = 0;
	for (i=0; i<item_num; i++) {
		strptime(pitem[i].date, "%Y-%m-%d", &temp_tm);
		if (temp_tm.tm_year + 1900 != year || temp_tm.tm_mon + 1 != month) {
			continue;
		}
		if (pitem[i].spam > max_num) {
			max_num = pitem[i].spam;
		}
		if (pitem[i].normal > max_num) {
			max_num = pitem[i].normal;
		}
	}
	for (i=0; i<item_num; i++) {
		strptime(pitem[i].date, "%Y-%m-%d", &temp_tm);
		if (temp_tm.tm_year + 1900 != year || temp_tm.tm_mon + 1 != month) {
			continue;
		}
		printf(HTML_TBCELL_BEGIN);
		if (0 == pitem[i].spam || 0 == max_num) {
			height = 1;
		} else {
			height = ((double)pitem[i].spam)/max_num*200;
		}
		printf(HTML_CHART_SPAM, lang_resource_get(g_lang_resource,"MAIN_SPAM", language),
			pitem[i].spam, height);
		if (0 == pitem[i].normal || 0 == max_num) {
			height = 1;
		} else {
			height = ((double)pitem[i].normal)/max_num*200;
		}
		printf(HTML_CHART_NORMAL, lang_resource_get(g_lang_resource,"MAIN_NORMAL", language),
			pitem[i].normal, height);
		printf(HTML_TBCELL_END);	
	}
	printf(HTML_MAIN_9);
	for (i=0; i<item_num; i++) {
		strptime(pitem[i].date, "%Y-%m-%d", &temp_tm);
		if (temp_tm.tm_year + 1900 != year || temp_tm.tm_mon + 1 != month) {
			continue;
		}
		printf(HTML_TBCELL_BEGIN);
		printf("%d", temp_tm.tm_mday);
		printf(HTML_TBCELL_END);
	}
	printf(HTML_MAIN_10, lang_resource_get(g_lang_resource,"MAIN_DATE", language),
		lang_resource_get(g_lang_resource,"MAIN_SPAM", language),
		lang_resource_get(g_lang_resource,"MAIN_NORMAL", language),
		lang_resource_get(g_lang_resource,"MAIN_PERCENTAGE", language));
	total_spam = 0;
	total_normal = 0;
	for (i=0; i<item_num; i++) {
		strptime(pitem[i].date, "%Y-%m-%d", &temp_tm);
		if (temp_tm.tm_year + 1900 != year || temp_tm.tm_mon + 1 != month) {
			continue;
		}
		printf(HTML_TBLINE_BEGIN);
		printf(HTML_TBCELL_BEGIN);
		printf("%d", temp_tm.tm_mday);
		printf(HTML_TBCELL_END);
		printf(HTML_TBCELL_BEGIN);
		printf("%d", pitem[i].spam);
		printf(HTML_TBCELL_END);
		printf(HTML_TBCELL_BEGIN);
		printf("%d", pitem[i].normal);
		printf(HTML_TBCELL_END);
		printf(HTML_TBCELL_BEGIN);
		if (0 == pitem[i].spam + pitem[i].normal) {
			printf("0%%");
		} else {
			printf("%d%%", (100*pitem[i].spam)/(pitem[i].spam+pitem[i].normal));
		}
		printf(HTML_TBCELL_END);
		printf(HTML_TBLINE_END);
		total_spam += pitem[i].spam;
		total_normal += pitem[i].normal;
	}
	if (0 != total_spam + total_normal) {
		printf(HTML_SUMMARY_LINE);
		printf(HTML_TBLINE_BEGIN);
		printf(HTML_TBCELL_BEGIN);
		printf(lang_resource_get(g_lang_resource,"MAIN_TOTAL", language));
		printf(HTML_TBCELL_END);
		printf(HTML_TBCELL_BEGIN);
		printf("%d", total_spam);
		printf(HTML_TBCELL_END);
		printf(HTML_TBCELL_BEGIN);
		printf("%d", total_normal);
		printf(HTML_TBCELL_END);
		printf(HTML_TBCELL_BEGIN);
		printf("%d%%", (int)(((double)total_spam)/(total_spam+total_normal)*100));
		printf(HTML_TBCELL_END);
		printf(HTML_TBLINE_END);
	}
	list_file_free(pfile);
	printf(HTML_MAIN_11);
}

static void statistic_ui_unencode(char *src, char *last, char *dest)
{
	int code;
	
	for (; src != last; src++, dest++) {
		if (*src == '+') {
			*dest = ' ';
		} else if (*src == '%') {
			if (sscanf(src+1, "%2x", &code) != 1) {
				code = '?';
			}
			*dest = code;
			src +=2;
		} else {
			*dest = *src;
		}
	}
	*dest = '\n';
	*++dest = '\0';
}

