// SPDX-License-Identifier: GPL-2.0-only WITH linking exception
#include <cerrno>
#include <unistd.h>
#include <libHX/string.h>
#include <gromox/defs.h>
#include <gromox/system_log.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <ctime>
#define DEF_MODE            S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH|S_IWOTH

static char g_log_path[256];
static int g_log_fd;

void system_log_init(const char *path)
{
	gx_strlcpy(g_log_path, path, GX_ARRAY_SIZE(g_log_path));
}

int system_log_run()
{
	struct stat node_stat;

	if (strcmp(g_log_path, "-") == 0) {
		g_log_fd = STDERR_FILENO;
		return 0;
	}
	if (0 != stat(g_log_path, &node_stat)) {
		g_log_fd = open(g_log_path, O_WRONLY|O_CREAT|O_TRUNC, DEF_MODE);
	} else {
		if (node_stat.st_size > 16*1024*1024) {
			g_log_fd = open(g_log_path, O_WRONLY|O_CREAT|O_TRUNC, DEF_MODE);
		} else {
			g_log_fd = open(g_log_path, O_WRONLY|O_APPEND);
		}
	}
	if (-1 == g_log_fd) {
		g_log_fd = STDERR_FILENO;
		system_log_info("Unable to open logfile %s: %s. Logging to stderr.", g_log_path, strerror(errno));
	}
	return 0;
}

void system_log_info(const char *format, ...)
{
	va_list ap;
	int len;
	char log_buf[4096];
	time_t time_now;
	
	time(&time_now);
	len = strftime(log_buf, 32, "%Y/%m/%d %H:%M:%S\t",
			localtime(&time_now));
	va_start(ap, format);
	len += vsnprintf(log_buf + len, sizeof(log_buf) - len - 1, format, ap);
	va_end(ap);
	log_buf[len++]  = '\n';
	write(g_log_fd, log_buf, len);
}

void system_log_stop()
{
	if (g_log_fd != -1 && g_log_fd != STDERR_FILENO)
		close(g_log_fd);
}

void system_log_free()
{
	g_log_path[0] = '\0';
}

