#include <string.h>
#include <libHX/ctype_helper.h>
#include <libHX/misc.h>
#include <gromox/resolv.h>
#include <gromox/socket.h>
#include "list_file.h"
#include "mail_func.h"
#include <gromox/system_log.h>
#include "util.h"
#undef NOERROR                  /* in <sys/streams.h> on solaris 2.x */
#include <arpa/nameser.h>
#include <sys/param.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <resolv.h>
#include <netdb.h>
#include <time.h>
#include <pthread.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include "smtp_sender.h"

#define MAXPACKET			8192 /* max size of packet */
#define MAXBUF				256          
#define MAXMXHOSTS			32   /* max num of mx records we want to see */
#define MAXMXBUFSIZ			(MAXMXHOSTS * (MAXBUF+1)) 
#define SOCKET_TIMEOUT		180


enum{
    SMTP_TIME_OUT,
    SMTP_TEMP_ERROR,
    SMTP_UNKOWN_RESPONSE,
    SMTP_PERMANENT_ERROR,
    SMTP_RESPONSE_OK
};

static BOOL smtp_sender_send_command(int sockd, const char *command,
	int command_len);

static int smtp_sender_get_response(int sockd, char *response, 
	int response_len, BOOL expect_3xx);


void smtp_sender_init()
{
	/* do nothing */
}

int smtp_sender_run()
{
	/* do nothing */
	return 0;
}

int smtp_sender_stop()
{
	/* do nothing */
	return 0;
}

void smtp_sender_free()
{
	/* do nothing */
}

void smtp_sender_send(const char *sender, const char *address,
	const char *pbuff, int size)
{
	char **p_addr;
	char *pdomain, ip[16];
	char command_line[1024];
	char response_line[1024];
	int	i, times, num;
	int command_len, sockd, port;
	struct in_addr ip_addr;
	char **mx_buff = NULL;
	struct hostent *phost;
	
	pdomain = strchr(address, '@');
	if (NULL == pdomain) {
		return;
	}
	pdomain ++;
	num = gx_getmxbyname(pdomain, &mx_buff);
	if (num <= 0) {
		if (mx_buff != NULL)
			HX_zvecfree(mx_buff);
		return;
	}
	memset(ip, 0, 16);
	for (i = 0; i < num && i < MAXMXHOSTS; ++i) {
		if (NULL == extract_ip(mx_buff[i], ip)) {
			if (NULL == (phost = gethostbyname(mx_buff[i]))) {
				continue;
			}
			p_addr = phost->h_addr_list;
			for (; NULL != (*p_addr); p_addr++) {
				ip_addr.s_addr = *((unsigned int *)*p_addr);
				strcpy(ip, inet_ntoa(ip_addr));
				break;
			}
			if ('\0' != ip[0]) {
				break;
			}
		} else {
			break;
		}
	}
	if (mx_buff != NULL)
		HX_zvecfree(mx_buff);
	if ('\0' == ip[0]) {
		system_log_info("can not find mx record for %s", address);
		return;
	}
	port = 25;
	times = 0;
SENDING_RETRY:
	/* try to connect to the destination MTA */
	sockd = gx_inet_connect(ip, port, 0);
	if (sockd < 0) {
		close(sockd);
		times ++;
		if (3 == times) {
			system_log_info("cannot connect to %s for %s", ip, address);
			return;
		} else {
			goto SENDING_RETRY;
		}
	}
	/* read welcome information of MTA */
	switch (smtp_sender_get_response(sockd, response_line, 1024, FALSE)) {
	case SMTP_TIME_OUT:
		close(sockd);
		times ++;
		if (3 == times) {
			system_log_info("connection time out for %s", address);
			return;
		} else {
			goto SENDING_RETRY;
		}
	case SMTP_PERMANENT_ERROR:
	case SMTP_TEMP_ERROR:
	case SMTP_UNKOWN_RESPONSE:
		strcpy(command_line, "quit\r\n");
        /* send quit command to server */
        smtp_sender_send_command(sockd, command_line, 6);
		close(sockd);
		system_log_info("remote MTA answer %s after connected for %s",
			response_line, address);
		return;
	}

	/* send helo xxx to server */
	if (FALSE == smtp_sender_send_command(sockd, "helo system.mail\r\n", 18)) {
		close(sockd);
		times ++;
		if (3 == times) {
			system_log_info("connection time out for %s", address);
			return;
		} else {
			goto SENDING_RETRY;
		}
	}
	switch (smtp_sender_get_response(sockd, response_line, 1024, FALSE)) {
	case SMTP_TIME_OUT:
		close(sockd);
		times ++;
		if (3 == times) {
			system_log_info("connection time out for %s", address);
			return;
		} else {
			goto SENDING_RETRY;
		}
	case SMTP_PERMANENT_ERROR:
	case SMTP_TEMP_ERROR:
	case SMTP_UNKOWN_RESPONSE:
		strcpy(command_line, "quit\r\n");
		/* send quit command to server */
		smtp_sender_send_command(sockd, command_line, 6);
		close(sockd);
		system_log_info("remote MTA answer %s after helo command for %s",
			response_line, address);
		return;
	}

	/* send mail from:<...> */
	command_len = sprintf(command_line, "mail from:<%s>\r\n", sender);
	if (FALSE == smtp_sender_send_command(sockd, command_line, command_len)) {
		close(sockd);
		times ++;
		if (3 == times) {
			system_log_info("connection time out for %s", address);
			return;
		} else {
			goto SENDING_RETRY;
		}
	}
	/* read mail from response information */
    switch (smtp_sender_get_response(sockd, response_line, 1024, FALSE)) {
    case SMTP_TIME_OUT:
		close(sockd);
		times ++;
		if (3 == times) {
			system_log_info("connection time out for %s", address);
			return;
		} else {
			goto SENDING_RETRY;
		}
	case SMTP_PERMANENT_ERROR:
	case SMTP_TEMP_ERROR:
	case SMTP_UNKOWN_RESPONSE:
		strcpy(command_line, "quit\r\n");
        /* send quit command to server */
        smtp_sender_send_command(sockd, command_line, 6);
		close(sockd);
		system_log_info("remote MTA answer %s after mail from command for %s",
			response_line, address);
        return;
    }

	/* send rcpt to:<...> */
	
	command_len = sprintf(command_line, "rcpt to:<%s>\r\n", address);
	if (FALSE == smtp_sender_send_command(sockd, command_line, command_len)) {
		close(sockd);
		times ++;
		if (3 == times) {
			system_log_info("connection time out for %s", address);
			return;
		} else {
			goto SENDING_RETRY;
		}
	}
	/* read rcpt to response information */
    switch (smtp_sender_get_response(sockd, response_line, 1024, FALSE)) {
    case SMTP_TIME_OUT:
		close(sockd);
		times ++;
		if (3 == times) {
			system_log_info("connection time out for %s", address);
			return;
		} else {
			goto SENDING_RETRY;
		}
	case SMTP_PERMANENT_ERROR:
	case SMTP_TEMP_ERROR:
	case SMTP_UNKOWN_RESPONSE:
		strcpy(command_line, "quit\r\n");
		/* send quit command to server */
		smtp_sender_send_command(sockd, command_line, 6);
		close(sockd);
		system_log_info("remote MTA answer %s after rcpt to command for %s",
			response_line, address);
		return;
	}
	
	/* send data */
	strcpy(command_line, "data\r\n");
	if (FALSE == smtp_sender_send_command(sockd, command_line, 6)) {
		close(sockd);
		times ++;
		if (3 == times) {
			system_log_info("connection time out for %s", address);
			return;
		} else {
			goto SENDING_RETRY;
		}
	}

	/* read data response information */
    switch (smtp_sender_get_response(sockd, response_line, 1024, TRUE)) {
    case SMTP_TIME_OUT:
		close(sockd);
		times ++;
		if (3 == times) {
			system_log_info("connection time out for %s", address);
			return;
		} else {
			goto SENDING_RETRY;
		}
	case SMTP_PERMANENT_ERROR:
	case SMTP_TEMP_ERROR:
	case SMTP_UNKOWN_RESPONSE:
		strcpy(command_line, "quit\r\n");
        /* send quit command to server */
        smtp_sender_send_command(sockd, command_line, 6);
		close(sockd);
		system_log_info("remote MTA answer %s after data command for %s",
			response_line, address);
		return;
    }

	if (FALSE == smtp_sender_send_command(sockd, pbuff, size)) {
		close(sockd);
		times ++;
		if (3 == times) {
			system_log_info("connection time out for %s", address);
			return;
		} else {
			goto SENDING_RETRY;
		}
	}
	if (FALSE == smtp_sender_send_command(sockd, "\r\n.\r\n", 5)) {
		close(sockd);
		times ++;
		if (3 == times) {
			system_log_info("connection time out for %s", address);
			return;
		} else {
			goto SENDING_RETRY;
		}
	}
	switch (smtp_sender_get_response(sockd, response_line, 1024, FALSE)) {
	case SMTP_TIME_OUT:
		close(sockd);
		times ++;
		if (3 == times) {
			system_log_info("connection time out for %s", address);
			return;
		} else {
			goto SENDING_RETRY;
		}
	case SMTP_PERMANENT_ERROR:
	case SMTP_TEMP_ERROR:
	case SMTP_UNKOWN_RESPONSE:	
		strcpy(command_line, "quit\r\n");
		/* send quit command to server */
        smtp_sender_send_command(sockd, command_line, 6);
		close(sockd);
		system_log_info("remote MTA answer %s after CRLF.CRLF command for %s",
			response_line, address);
		return;
	case SMTP_RESPONSE_OK:
		strcpy(command_line, "quit\r\n");
		/* send quit command to server */
		smtp_sender_send_command(sockd, command_line, 6);
		close(sockd);
		system_log_info("remote MTA return OK for %s", address);
		return;
	}
}


/*
 *	send a command string to destination
 *	@param
 *		sockd				socket fd
 *		command [in]		command string to be sent
 *		command_len			command string length
 *	@return
 *		TRUE				OK
 *		FALSE				time out
 */
static BOOL smtp_sender_send_command(int sockd, const char *command, int command_len)
{
	int write_len;

	write_len = write(sockd, command, command_len);
    if (write_len != command_len) {
		return FALSE;
	}
	return TRUE;
}

/*
 *	get response from server
 *	@param
 *		sockd					socket fd
 *		response [out]			buffer for save response
 *		response_len			response buffer length
 *		reason [out]			fail reason
 *	@retrun
 *		SMTP_TIME_OUT			time out
 *		SMTP_TEMP_ERROR		temporary failure
 *		SMTP_UNKOWN_RESPONSE	unknown failure
 *		SMTP_PERMANENT_ERROR	permanent failure
 *		SMTP_RESPONSE_OK		OK
 */
static int smtp_sender_get_response(int sockd, char *response, int response_len,
	BOOL expect_3xx)
{
	int read_len;
	fd_set myset;
	struct timeval tv;
	
	/* wait the socket data to be available */
	tv.tv_sec = SOCKET_TIMEOUT;
	tv.tv_usec = 0;
	FD_ZERO(&myset);
	FD_SET(sockd, &myset);
	if (select(sockd + 1, &myset, NULL, NULL, &tv) <= 0) {
		return SMTP_TIME_OUT;
	}

	memset(response, 0, response_len);
	read_len = read(sockd, response, response_len);
	if (-1 == read_len || 0 == read_len) {
		return SMTP_TIME_OUT;
	}
	if ('\n' == response[read_len - 1] && '\r' == response[read_len - 2]){
		/* remove /r/n at the end of response */
		read_len -= 2;
	}
	response[read_len] = '\0';
	if (FALSE == expect_3xx && '2' == response[0] &&
	    HX_isdigit(response[1]) && HX_isdigit(response[2])) {
		return SMTP_RESPONSE_OK;
	} else if(TRUE == expect_3xx && '3' == response[0] &&
	    HX_isdigit(response[1]) && HX_isdigit(response[2])) {
		return SMTP_RESPONSE_OK;
	} else {
		if ('4' == response[0]) {
           	return SMTP_TEMP_ERROR;	
		} else if ('5' == response[0]) {
			return SMTP_PERMANENT_ERROR;
		} else {
			return SMTP_UNKOWN_RESPONSE;
		}
	}
}