/*
 * Email Address Kids Lib Header
 */
#pragma once
#include <ctime>
#include <string>
#include <gromox/defs.h>
#include <gromox/mem_file.hpp>
#define MIME_NAME_LEN 80U
#define MIME_FIELD_LEN (64U * 1024)

struct EMAIL_ADDR {
	char display_name[256], local_part[ULCLPART_SIZE], domain[UDOM_SIZE];
};

struct MIME_FIELD {
	unsigned int field_name_len, field_value_len;
    char field_name[MIME_NAME_LEN];
    char field_value[MIME_FIELD_LEN];
};

struct ENCODE_STRING {
    char encoding[32];
    char charset[32];
    char title[1024];
};

extern const char *extract_ip(const char *in, char *out);
void parse_email_addr(EMAIL_ADDR *e_addr, const char *email);
void parse_mime_addr(EMAIL_ADDR *e_addr, const char *email);
BOOL parse_uri(const char *uri_buff, char *parsed_uri);
extern GX_EXPORT size_t parse_mime_field(char *, size_t, MIME_FIELD *);
void parse_field_value(char *in_buff, long buff_len, char *value, long val_len,
	MEM_FILE *pfile);
void parse_mime_encode_string(char *in_buff, long buff_len,
	ENCODE_STRING *encode_string);
char* find_url (char *buf, size_t howmuch, int *count);
int utf7_to_utf8 (const char *u7, size_t u7len, char *u8, size_t u8len);
int utf8_to_utf7 (const char *u8, size_t u8len, char *u7, size_t u7len);
int parse_imap_args(char *cmdline, int cmdlen, char **argv, int argmax);
time_t make_gmtime(struct tm *ptm);
void make_gmtm(time_t gm_time, struct tm *ptm);
BOOL parse_rfc822_timestamp(const char *str_time, time_t *ptime);
BOOL mime_string_to_utf8(const char *charset,
	const char *mime_string, char *out_string);
void enriched_to_html(const char *enriched_txt,
	char *html, int max_len);
extern GX_EXPORT int html_to_plain(const void *inbuf, int inlen, std::string &outbuf);
extern GX_EXPORT char *plain_to_html(const char *rbuf);
