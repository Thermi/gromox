// SPDX-License-Identifier: GPL-2.0-only WITH linking exception
// SPDX-FileCopyrightText: 2020–2021 grommunio GmbH
// This file is part of Gromox.
/*
 *  mail queue have three parts, tape, mess, message queue.when a mail
 *  is put into mail queue, first, check whether it is less
 *  than 64K, if it is, get a block (128K) from tape, and write the mail
 *  into this block; or, create a file in mess directory and write the
 *  mail into file. after mail is saved, system will send a message to
 *  message queue to indicate there's a new mail arrived!
 */
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <mutex>
#include <string>
#include <libHX/string.h>
#include <gromox/defs.h>
#include <gromox/fileio.h>
#include "message_dequeue.h"
#include "system_services.h"
#include <gromox/util.hpp>
#include <gromox/int_hash.hpp>
#include <gromox/scope.hpp>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <pthread.h>
#include <cstdio>
#include "transporter.h"
#define DEF_MODE    S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH|S_IWOTH
#define TOKEN_MESSAGE_QUEUE		1
#define BLOCK_SIZE				64*1024*2
#define SLEEP_INTERVAL			50000

using namespace std::string_literals;
using namespace gromox;

namespace {

struct MSG_BUFF {
	long msg_type;
	int msg_content;
};

}

static std::string g_path, g_path_mess, g_path_save;
static int				g_msg_id;	    /* message queue id */
static size_t			g_message_units;/* allocated message units number */
static size_t			g_max_memory;   /* maximum allocated memory for mess*/
static size_t			g_current_mem;  /*current allocated memory */
static MESSAGE			*g_message_ptr;
static SINGLE_LIST				g_used_list;
static INT_HASH_TABLE	*g_mess_hash;
static SINGLE_LIST				g_free_list;
static std::mutex g_hash_mutex, g_used_mutex, g_free_mutex, g_mess_mutex;
static pthread_t		g_thread_id;
static std::atomic<bool> g_notify_stop{false};
static int				g_dequeued_num;

static BOOL message_dequeue_check();
static MESSAGE *message_dequeue_get_from_free(int message_option, size_t size);

static void message_dequeue_put_to_free(MESSAGE *pmessage);

static void message_dequeue_put_to_used(MESSAGE *pmessage);
static void message_dequeue_load_from_mess(int mess);
static void message_dequeue_collect_resource();
static void *mdq_thrwork(void *);

/* 
 *	@param
 *		path [in]	path of directory
 *		max_memory	maximum memory system allowed concurrently
 */
void message_dequeue_init(const char *path, size_t max_memory)
{	
	g_path = path;
	g_path_mess = path + "/mess"s;
	g_path_save = path + "/save"s;
	g_max_memory = ((max_memory-1)/(BLOCK_SIZE/2) + 1) * (BLOCK_SIZE/2);
	single_list_init(&g_used_list);
	single_list_init(&g_free_list);
	g_current_mem = 0;
	g_msg_id = -1;
	g_message_ptr = NULL;
	g_mess_hash = NULL;
	g_notify_stop = false;
	g_dequeued_num = 0;
}


/*
 *	check the message queue's parameters is same as the entity in file system
 *	@return
 *		TRUE				OK
 *		FALSE				fail
 */
static BOOL message_dequeue_check()
{
	struct	stat node_stat;

	/* check if the directory exists and is a real directory */
	if (stat(g_path.c_str(), &node_stat) != 0) {
		printf("[message_dequeue]: cannot find directory %s\n", g_path.c_str());
		return FALSE;
	}
	if (0 == S_ISDIR(node_stat.st_mode)) {
		printf("[message_dequeue]: %s is not a directory\n", g_path.c_str());
		return FALSE;
	}
	/* mess directory is used to save the message larger than BLOCK_SIZE */
	if (stat(g_path_mess.c_str(), &node_stat) != 0) {
		printf("[message_dequeue]: cannot find directory %s\n", g_path_mess.c_str());
        return FALSE;
    }
    if (0 == S_ISDIR(node_stat.st_mode)) {
		printf("[message_dequeue]: %s is not a directory\n", g_path_mess.c_str());
        return FALSE;
    }
	/* save directory is used to save the message for debugging */
	if (stat(g_path_save.c_str(), &node_stat) != 0) {
		printf("[message_dequeue]: cannot find directory %s\n", g_path_save.c_str());
        return FALSE;
    }
    if (0 == S_ISDIR(node_stat.st_mode)) {
		printf("[message_dequeue]: %s is not a directory\n", g_path_save.c_str());
        return FALSE;
    }
	return TRUE;
}

static void message_dequeue_collect_resource()
{
	if (NULL != g_message_ptr) {
		free(g_message_ptr);
		g_message_ptr = NULL;	
	}
	if (NULL != g_mess_hash) {
		int_hash_free(g_mess_hash);
		g_mess_hash = NULL;
	}
}

int message_dequeue_run()
{
	size_t size;
	MESSAGE *pmessage;
	pthread_attr_t attr;

	if (FALSE == message_dequeue_check()) {
		return -1;
	}
	std::string name;
	try {
		name = g_path + "/token.ipc"s;
	} catch (const std::bad_alloc &) {
		printf("[message_dequeue]: MDQ-167\n");
		return false;
	}
	int fd = open(name.c_str(), O_CREAT | O_WRONLY, S_IRUSR | S_IWUSR);
	if (fd >= 0)
		close(fd);
	auto k_msg = ftok(name.c_str(), TOKEN_MESSAGE_QUEUE);
    if (-1 == k_msg) {
		printf("[message_dequeue]: ftok %s: %s\n", name.c_str(), strerror(errno));
        return -2;
    }
	/* create the message queue */
	g_msg_id = msgget(k_msg, 0666|IPC_CREAT);
	if (-1 == g_msg_id) {
		printf("[message_dequeue]: msgget: %s\n", strerror(errno));
		message_dequeue_collect_resource();
		return -6;
	}
	g_message_units = g_max_memory/(BLOCK_SIZE/2);
	size = sizeof(MESSAGE)*g_message_units;
	g_message_ptr = (MESSAGE*)malloc(size);
	if (NULL == g_message_ptr) {
		printf("[message_dequeue]: fail to allcate message nodes\n");
		message_dequeue_collect_resource();
		return -7;
	}
	memset(g_message_ptr, 0, size);
	/* append rest of message node into free list */
	for (size_t i = 0; i < g_message_units; ++i) {
		pmessage = g_message_ptr + i;
        pmessage->node.pdata = pmessage;
		single_list_append_as_tail(&g_free_list, &pmessage->node);
	}
	g_mess_hash = int_hash_init(2 * g_message_units + 1, sizeof(void *));
	if (g_mess_hash == nullptr) {
		printf("[message_dequeue]: failed to initialize hash table\n");
		message_dequeue_collect_resource();
		return -8;
	}
	pthread_attr_init(&attr);
	auto ret = pthread_create(&g_thread_id, &attr, mdq_thrwork, nullptr);
	if (ret != 0) {
		printf("[message_dequeue]: failed to create message dequeue thread: %s\n", strerror(ret));
		message_dequeue_collect_resource();
		return -9;
	}
	pthread_setname_np(g_thread_id, "msg_dequeue");
	return 0;
}

/*
 *	get a mail message from mail queue
 *	@return
 *		pointer to mail message, NULL means none message
 */
MESSAGE* message_dequeue_get()
{
	SINGLE_LIST_NODE *pnode;

	std::unique_lock h(g_used_mutex);
	pnode = single_list_pop_front(&g_used_list);
	if (NULL == pnode) {
		return NULL;
	}
	return (MESSAGE*)pnode->pdata;
}

/*
 *	put message back to mail queue
 *	@param
 *		pmessage [in]		pointer to message struct
 */
void message_dequeue_put(MESSAGE *pmessage) try
{
	free(pmessage->begin_address);
	pmessage->begin_address = NULL;
	auto name = g_path_mess + "/" + std::to_string(pmessage->message_data);
	if (remove(name.c_str()) < 0 && errno != ENOENT)
		fprintf(stderr, "W-1352: remove %s: %s\n", name.c_str(), strerror(errno));
	std::unique_lock h(g_hash_mutex);
	int_hash_remove(g_mess_hash, pmessage->message_data);
	h.unlock();
	message_dequeue_put_to_free(pmessage);
	g_dequeued_num ++;
} catch (const std::bad_alloc &) {
	printf("[message_dequeue]: MDQ-254\n");
}

void message_dequeue_stop()
{
	g_notify_stop = true;
	pthread_kill(g_thread_id, SIGALRM);
	pthread_join(g_thread_id, NULL);

	message_dequeue_collect_resource();
}

void message_dequeue_free()
{
    g_max_memory = 0;
	g_current_mem  = 0;
    g_msg_id = -1;
	g_message_ptr = NULL;
	g_mess_hash = NULL;
	g_notify_stop = true;
}

/*
 *	@param
 *		pmessage [out]			message struct pointer
 *		in_buff [in]			buffer of mail message
 */
static void message_dequeue_retrieve_to_message(MESSAGE *pmessage,
    const char *in_buff)
{
	pmessage->begin_address = deconst(in_buff);
	pmessage->mail_begin = deconst(in_buff) + sizeof(size_t);
	memcpy(&pmessage->mail_length, in_buff, sizeof(size_t));
	memcpy(&pmessage->flush_ID, in_buff + sizeof(size_t) + pmessage->mail_length, sizeof(int));
	memcpy(&pmessage->bound_type, in_buff + sizeof(size_t) + sizeof(int) + pmessage->mail_length, sizeof(int));
	int z;
	memcpy(&z, in_buff + sizeof(size_t) + 2 * sizeof(int) + pmessage->mail_length, sizeof(int));
	pmessage->is_spam = z;
	pmessage->envelope_from = deconst(in_buff) + sizeof(size_t) + 3*sizeof(int) + pmessage->mail_length;
	pmessage->envelope_rcpt = pmessage->envelope_from + strlen(pmessage->envelope_from) + 1;
}

/*
 *	get a message node back from free list
 *	@param
 *		message_option			MESSAGE_MESS
 *	@return
 *		message pointer, NULL means fail to get
 */
static MESSAGE *message_dequeue_get_from_free(int message_option, size_t size)
{
	SINGLE_LIST_NODE *pnode;
	MESSAGE *pmessage;

	/* at a certain time, number of mess message node is limited */
	if (MESSAGE_MESS == message_option) {
		std::unique_lock h(g_mess_mutex);
		if (g_current_mem + size > g_max_memory) {
			return NULL;
		} else {
			g_current_mem += size;
		}
	}
	std::unique_lock fr_hold(g_free_mutex);
	pnode = single_list_pop_front(&g_free_list);
	fr_hold.unlock();
	if (NULL == pnode) {
		debug_info("[message_dequeue]: fatal error in "
			"message_dequeue_get_from_free\n");
		return NULL;
	}
	pmessage = (MESSAGE*) pnode->pdata;
	pmessage->message_option = message_option;
	if (MESSAGE_MESS == message_option) {
		pmessage->size = size;
	} else {
		pmessage->size = 0;
	}
	pmessage->begin_address = NULL;
	pmessage->mail_begin = NULL;
	pmessage->mail_length = 0;
	pmessage->envelope_from = pmessage->envelope_rcpt = nullptr;
	return pmessage;
}

/*
 *	put message node back to free list
 *	@param
 *		pmessage [in]		pointer to message struct
 */
static void message_dequeue_put_to_free(MESSAGE *pmessage)
{
	if (MESSAGE_MESS == pmessage->message_option) {
		std::unique_lock h(g_mess_mutex);
		g_current_mem -= pmessage->size;
	}
	std::unique_lock h(g_free_mutex);
	single_list_append_as_tail(&g_free_list, &pmessage->node);
}

/*
 *	add a message struct into used list
 *	@param
 *		pmessage [in]		pointer to message struct
 */
static void message_dequeue_put_to_used(MESSAGE *pmessage)
{
	std::unique_lock h(g_used_mutex);
    single_list_append_as_tail(&g_used_list, &pmessage->node);
	h.unlock();
	/* send a signal to threads pool in transporter */
	transporter_wakeup_one_thread();
}

/*
 *	load a mess file into used list, and so threads of other modules can
 *	get message from used list
 *	@param
 *		mess			mess ID
 */
static void message_dequeue_load_from_mess(int mess)
{
	struct stat node_stat;
	MESSAGE *pmessage;
	char *ptr;
	size_t size;

	std::unique_lock h(g_hash_mutex);
    pmessage = (MESSAGE*)int_hash_query(g_mess_hash, mess);
	h.unlock();
	if (NULL != pmessage) {
		return;
	}
	std::string name;
	try {
		name = g_path_mess + "/" + std::to_string(mess);
	} catch (const std::bad_alloc &) {
		printf("[message_dequeue]: MDQ-390\n");
		return;
	}
	wrapfd fd = open(name.c_str(), O_RDONLY);
	if (fd.get() < 0 || fstat(fd.get(), &node_stat) != 0 ||
	    !S_ISREG(node_stat.st_mode))
		return;
	size = ((node_stat.st_size - 1)/(64 * 1024) + 1) * 64 * 1024;
	pmessage = message_dequeue_get_from_free(MESSAGE_MESS, size);
	if (NULL == pmessage) {
		return;
	}
	pmessage->message_data = mess;
	ptr = (char*)malloc(size);
	if (NULL == ptr) {
		message_dequeue_put_to_free(pmessage);
        return;
	}
	if (read(fd.get(), ptr, node_stat.st_size) != node_stat.st_size) {
		message_dequeue_put_to_free(pmessage);
		free(ptr);
        return;
	}
	/* check if it is an incomplete message */
	if (0 == *(int*)ptr) {
		message_dequeue_put_to_free(pmessage);
		free(ptr);
		return;	
	}
	message_dequeue_retrieve_to_message(pmessage, ptr);
	message_dequeue_put_to_used(pmessage);
	h.lock();
	int_hash_add(g_mess_hash, mess, pmessage);
}

static void *mdq_thrwork(void *arg)
{
	MSG_BUFF msg;
	size_t size;
    DIR *dirp;
    struct dirent *direntp;
	int len, mess;

	while ((dirp = opendir(g_path_mess.c_str())) == nullptr) {
		printf("[message_dequeue]: failed to open directory %s: %s\n",
		       g_path_mess.c_str(), strerror(errno));
        sleep(1);
    }

	while (!g_notify_stop) {
		if (-1 != msgrcv(g_msg_id, &msg, sizeof(int), 0, IPC_NOWAIT)) {
			switch(msg.msg_type) {
			case MESSAGE_MESS:
				message_dequeue_load_from_mess(msg.msg_content);
				break;
			default:
				printf("[message_dequeue]: unknown message queue type %ld, "
					"should be MESSAGE_MESS\n", msg.msg_type);
			}
			continue;
		}
		usleep(SLEEP_INTERVAL);
		if (single_list_get_nodes_num(&g_free_list) != g_message_units) {
			continue;
		}
		/* clean up mess */
		seekdir(dirp, 0);
		while ((direntp = readdir(dirp)) != NULL) {
        	if (0 == strcmp(direntp->d_name, ".") ||
				0 == strcmp(direntp->d_name, "..")) {
				continue;
			}
			if (g_current_mem == g_max_memory) {
				break;
			}
			std::string file_name;
			try {
				file_name = g_path_mess + "/" + direntp->d_name;
			} catch (const std::bad_alloc &) {
				continue;
			}
			auto mess_fd = open(file_name.c_str(), O_RDONLY);
			if (-1 == mess_fd) {
				continue;
			}
			len = read(mess_fd, &size, sizeof(size_t));
			close(mess_fd);
			if (len != sizeof(size_t)) {
				continue;
			}
			if (0 == size) {
				continue;
			}
			mess = atoi(direntp->d_name);
			message_dequeue_load_from_mess(mess);
		}
	}
	closedir(dirp);
	return NULL;
}

/*
 *  message dequeue's console talk function
 *  @param
 *  	param			MESSAGE_DEQUEUE_HOLDING
 *						MESSAGE_DEQUEUE_PROCESSING
 *						MESSAGE_DEQUEUE_DEQUEUED
 *						MESSAGE_DEQUEUE_ALLOCATED
 *	@return
 *		value
 */
int message_dequeue_get_param(int param)
{
	int ret_val;

	switch(param) {
	case MESSAGE_DEQUEUE_HOLDING:
		return single_list_get_nodes_num(&g_used_list);
	case MESSAGE_DEQUEUE_PROCESSING:
		ret_val = g_message_units - single_list_get_nodes_num(&g_used_list) -
				  single_list_get_nodes_num(&g_free_list);
		return ret_val;
	case MESSAGE_DEQUEUE_DEQUEUED:
		ret_val = g_dequeued_num;
		g_dequeued_num = 0;
		return ret_val;
	case MESSAGE_DEQUEUE_ALLOCATED:
		return g_current_mem/(BLOCK_SIZE/2);
	default:
		return 0;
	}
}

/*
 *	for debuging when fail to retrieve message into mail object
 *	@param
 *		pmessage [in]			message object
 */
void message_dequeue_save(MESSAGE *pmessage)
{
	std::string new_file, old_file;
	char *ptr;
	int len;
	
	try {
		new_file = g_path_save + "/" + std::to_string(pmessage->flush_ID);
		old_file = g_path_mess + "/" + std::to_string(pmessage->message_data);
	} catch (const std::bad_alloc &) {
		fprintf(stderr, "[message_dequeue]: MDQ-536\n");
		return;
	}
	if (MESSAGE_MESS == pmessage->message_option) {
		link(old_file.c_str(), new_file.c_str());
	} else {
		int fd = open(new_file.c_str(), O_WRONLY|O_CREAT|O_TRUNC, DEF_MODE);
		if (fd < 0) {
			printf("[message_dequeue]: open/w %s: %s\n",
			       new_file.c_str(), strerror(errno));
			return;
		}
		write(fd, pmessage->begin_address, pmessage->mail_length + 4*sizeof(int));
		len = strlen(pmessage->envelope_from);
		write(fd, pmessage->envelope_from, len + 1);
		ptr = pmessage->envelope_rcpt;
		while ((len = strlen(ptr)) != 0) {
			len ++;
			write(fd, ptr, len);
			ptr += len;
		}
		close(fd);
	}
}

