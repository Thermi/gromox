// SPDX-License-Identifier: GPL-2.0-only WITH linking exception
#ifdef HAVE_CONFIG_H
#	include "config.h"
#endif
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <string>
#include <utility>
#include <vector>
#include <libHX/option.h>
#include <libHX/string.h>
#include <gromox/paths.h>
#include <gromox/socket.h>
#include <gromox/util.hpp>
#include <gromox/fifo.hpp>
#include <gromox/scope.hpp>
#include <gromox/mem_file.hpp>
#include <gromox/list_file.hpp>
#include <gromox/config_file.hpp>
#include <gromox/double_list.hpp>
#include <ctime>
#include <cstdio>
#include <unistd.h>
#include <csignal>
#include <pthread.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#define SOCKET_TIMEOUT			60

#define SELECT_INTERVAL			24*60*60

#define HOST_INTERVAL			20*60

#define SCAN_INTERVAL			10*60

#define FIFO_AVERAGE_LENGTH		128

#define MAX_CMD_LENGTH			64*1024

#define HASH_CAPABILITY			10000

using namespace gromox;

namespace {

struct ENQUEUE_NODE {
	~ENQUEUE_NODE() { if (sockd >= 0) close(sockd); }

	char res_id[128]{};
	int sockd = -1;
	int offset = 0;
	char buffer[MAX_CMD_LENGTH]{};
	char line[MAX_CMD_LENGTH]{};
};

struct DEQUEUE_NODE {
	~DEQUEUE_NODE();

	char res_id[128]{};
	int sockd = -1;
	FIFO fifo{};
	std::mutex lock, cond_mutex;
	std::condition_variable waken_cond;
};

struct HOST_NODE {
	DOUBLE_LIST_NODE node{};
	char res_id[128]{};
	time_t last_time = 0;
	std::unordered_map<std::string, time_t> hash;
	std::vector<std::shared_ptr<DEQUEUE_NODE>> list;
};

}

static std::atomic<bool> g_notify_stop{false};
static unsigned int g_threads_num;
static LIB_BUFFER *g_fifo_alloc;
static LIB_BUFFER *g_file_alloc;
static std::vector<std::string> g_acl_list;
static std::list<ENQUEUE_NODE> g_enqueue_list, g_enqueue_list1;
static std::vector<std::shared_ptr<DEQUEUE_NODE>> g_dequeue_list1;
static std::list<HOST_NODE> g_host_list;
static std::mutex g_enqueue_lock, g_dequeue_lock, g_host_lock;
static std::mutex g_enqueue_cond_mutex, g_dequeue_cond_mutex;
static std::condition_variable g_enqueue_waken_cond, g_dequeue_waken_cond;
static char *opt_config_file;
static unsigned int opt_show_version;

static struct HXoption g_options_table[] = {
	{nullptr, 'c', HXTYPE_STRING, &opt_config_file, nullptr, nullptr, 0, "Config file to read", "FILE"},
	{"version", 0, HXTYPE_NONE, &opt_show_version, nullptr, nullptr, 0, "Output version information and exit"},
	HXOPT_AUTOHELP,
	HXOPT_TABLEEND,
};

static void *ev_acceptwork(void *);
static void *ev_enqwork(void *);
static void *ev_deqwork(void *);
static void *ev_scanwork(void *);
static BOOL read_response(int sockd);

static BOOL read_mark(ENQUEUE_NODE *penqueue);

static void term_handler(int signo);

DEQUEUE_NODE::~DEQUEUE_NODE()
{
	if (sockd >= 0)
		close(sockd);
	fifo_free(&fifo);
}

int main(int argc, const char **argv)
{
	int listen_port;
	char listen_ip[40];
	pthread_attr_t thr_attr;

	setvbuf(stdout, nullptr, _IOLBF, 0);
	if (HX_getopt(g_options_table, &argc, &argv, HXOPT_USAGEONERR) != HXOPT_ERR_SUCCESS)
		return EXIT_FAILURE;
	if (opt_show_version) {
		printf("version: %s\n", PROJECT_VERSION);
		return 0;
	}
	struct sigaction sact{};
	sigemptyset(&sact.sa_mask);
	sact.sa_handler = SIG_IGN;
	sact.sa_flags   = SA_RESTART;
	sigaction(SIGPIPE, &sact, nullptr);
	auto pconfig = config_file_prg(opt_config_file, "event.cfg");
	if (opt_config_file != nullptr && pconfig == nullptr)
		printf("[system]: config_file_init %s: %s\n", opt_config_file, strerror(errno));
	if (pconfig == nullptr)
		return 2;

	char config_dir[256];
	auto str_value = config_file_get_value(pconfig, "config_file_path");
	gx_strlcpy(config_dir, str_value != nullptr ? str_value :
	           PKGSYSCONFDIR "/event:" PKGSYSCONFDIR, GX_ARRAY_SIZE(config_dir));
	str_value = config_file_get_value(pconfig, "EVENT_LISTEN_IP");
	gx_strlcpy(listen_ip, str_value != nullptr ? str_value : "::1",
	           GX_ARRAY_SIZE(listen_ip));

	str_value = config_file_get_value(pconfig, "EVENT_LISTEN_PORT");
	if (NULL == str_value) {
		listen_port = 33333;
	} else {
		listen_port = atoi(str_value);
		if (listen_port <= 0)
			listen_port = 33333;
	}
	printf("[system]: listen address is [%s]:%d\n",
	       *listen_ip == '\0' ? "*" : listen_ip, listen_port);

	str_value = config_file_get_value(pconfig, "EVENT_THREADS_NUM");
	if (NULL == str_value) {
		g_threads_num = 50;
	} else {
		g_threads_num = strtoul(str_value, nullptr, 0);
		if (g_threads_num < 1)
			g_threads_num = 1;
		if (g_threads_num > 1000)
			g_threads_num = 1000;
	}

	printf("[system]: threads number is 2*%d\n", g_threads_num);
	
	g_threads_num ++;
	g_fifo_alloc = fifo_allocator_init(sizeof(MEM_FILE),
					g_threads_num*FIFO_AVERAGE_LENGTH, TRUE);
	if (NULL == g_fifo_alloc) {
		printf("[system]: Failed to init queue allocator\n");
		return 3;
	}
	auto cl_0 = make_scope_exit([&]() { fifo_allocator_free(g_fifo_alloc); });
	g_file_alloc = lib_buffer_init(FILE_ALLOC_SIZE,
					g_threads_num*FIFO_AVERAGE_LENGTH, TRUE);
	if (NULL == g_file_alloc) {
		printf("[system]: Failed to init file allocator\n");
		return 4;
	}
	auto cl_1 = make_scope_exit([&]() { lib_buffer_free(g_file_alloc); });
	auto sockd = gx_inet_listen(listen_ip, listen_port);
	if (sockd < 0) {
		printf("[system]: failed to create listen socket: %s\n", strerror(-sockd));
		return 5;
	}
	auto cl_2 = make_scope_exit([&]() { close(sockd); });
	g_dequeue_list1.reserve(g_threads_num);
	pthread_attr_init(&thr_attr);
	auto cl_3 = make_scope_exit([&]() { pthread_attr_destroy(&thr_attr); });
	pthread_attr_setstacksize(&thr_attr, 1024*1024);
	
	std::vector<pthread_t> tidlist;
	tidlist.reserve(g_threads_num * 2);
	auto cl_4 = make_scope_exit([&]() {
		g_enqueue_waken_cond.notify_all();
		g_dequeue_waken_cond.notify_all();
		for (auto tid : tidlist) {
			pthread_kill(tid, SIGALRM);
			pthread_join(tid, nullptr);
		}
	});
	size_t i;
	for (i=0; i<g_threads_num; i++) {
		pthread_t tid;
		auto ret = pthread_create(&tid, &thr_attr, ev_enqwork, nullptr);
		if (ret != 0) {
			printf("[system]: failed to create enqueue pool thread: %s\n", strerror(ret));
			break;
		}
		tidlist.push_back(tid);
		char buf[32];
		snprintf(buf, sizeof(buf), "enqueue/%zu", i);
		pthread_setname_np(tid, buf);

		ret = pthread_create(&tid, &thr_attr, ev_deqwork, nullptr);
		if (ret != 0) {
			printf("[system]: failed to create dequeue pool thread: %s\n", strerror(ret));
			break;
		}
		tidlist.push_back(tid);
		snprintf(buf, sizeof(buf), "dequeue/%zu", i);
		pthread_setname_np(tid, buf);
	}
	
	if (i != g_threads_num) {
		g_notify_stop = true;
		return 9;
	}

	auto ret = list_file_read_fixedstrings("event_acl.txt", config_dir, g_acl_list);
	if (ret == -ENOENT) {
		printf("[system]: defaulting to implicit access ACL containing ::1.\n");
		g_acl_list = {"::1"};
	} else if (ret < 0) {
		printf("[system]: list_file_initd event_acl.txt: %s\n", strerror(-ret));
		g_notify_stop = true;
		return 10;
	}

	pthread_t acc_thr{}, scan_thr{};
	ret = pthread_create(&acc_thr, nullptr, ev_acceptwork,
	      reinterpret_cast<void *>(static_cast<intptr_t>(sockd)));
	if (ret != 0) {
		printf("[system]: failed to create accept thread: %s\n", strerror(ret));
		g_notify_stop = true;
		return 11;
	}
	auto cl_5 = make_scope_exit([&]() {
		pthread_kill(acc_thr, SIGALRM); /* kick accept() */
		pthread_join(acc_thr, nullptr);
	});
	pthread_setname_np(acc_thr, "accept");
	ret = pthread_create(&scan_thr, nullptr, ev_scanwork, nullptr);
	if (ret != 0) {
		printf("[system]: failed to create scanning thread: %s\n", strerror(ret));
		g_notify_stop = true;
		return 11;
	}
	auto cl_6 = make_scope_exit([&]() {
		pthread_kill(scan_thr, SIGALRM); /* kick sleep() */
		pthread_join(scan_thr, nullptr);
	});
	pthread_setname_np(scan_thr, "scan");

	sact.sa_handler = [](int) {};
	sact.sa_flags   = 0;
	sigaction(SIGALRM, &sact, nullptr);
	sact.sa_handler = term_handler;
	sact.sa_flags   = SA_RESETHAND;
	sigaction(SIGINT, &sact, nullptr);
	sigaction(SIGTERM, &sact, nullptr);
	printf("[system]: EVENT is now running\n");
	while (!g_notify_stop) {
		sleep(1);
	}
	return 0;
}

static void *ev_scanwork(void *param)
{
	int i = 0;
	time_t cur_time;
	
	while (!g_notify_stop) {
		if (i < SCAN_INTERVAL) {
			sleep(1);
			i ++;
			continue;
		}
		i = 0;
		std::unique_lock hl_hold(g_host_lock);
		time(&cur_time);
		auto ptail = g_host_list.size() > 0 ? &g_host_list.back() : nullptr;
		while (g_host_list.size() > 0) {
			std::list<HOST_NODE> tmp_list;
			auto phost = &g_host_list.front();
			tmp_list.splice(tmp_list.end(), g_host_list, g_host_list.begin());
			if (phost->list.size() == 0 &&
				cur_time - phost->last_time > HOST_INTERVAL) {
			} else {
				for (auto it = phost->hash.begin(); it != phost->hash.end(); ) {
					if (cur_time - it->second > SELECT_INTERVAL)
						it = phost->hash.erase(it);
					else
						++it;
				}
				g_host_list.splice(g_host_list.end(), tmp_list);
			}
			if (phost == ptail)
				break;
		}
		hl_hold.unlock();
	}
	return NULL;
}

static void *ev_acceptwork(void *param)
{
	socklen_t addrlen;
	int sockd, sockd2;
	char client_hostip[40];
	struct sockaddr_storage peer_name;
	ENQUEUE_NODE *penqueue;

	sockd = (int)(long)param;
	while (!g_notify_stop) {
		/* wait for an incoming connection */
        addrlen = sizeof(peer_name);
        sockd2 = accept(sockd, (struct sockaddr*)&peer_name, &addrlen);
		if (-1 == sockd2) {
			continue;
		}
		int ret = getnameinfo(reinterpret_cast<sockaddr *>(&peer_name),
		          addrlen, client_hostip, sizeof(client_hostip),
		          nullptr, 0, NI_NUMERICHOST | NI_NUMERICSERV);
		if (ret != 0) {
			printf("getnameinfo: %s\n", gai_strerror(ret));
			close(sockd2);
			continue;
		}
		if (std::find(g_acl_list.cbegin(), g_acl_list.cend(),
		    client_hostip) == g_acl_list.cend()) {
			write(sockd2, "Access Deny\r\n", 13);
			close(sockd2);
			continue;
		}

		std::unique_lock eq_hold(g_enqueue_lock);
		if (g_enqueue_list.size() + 1 + g_enqueue_list1.size() >= g_threads_num) {
			eq_hold.unlock();
			write(sockd2, "Maximum Connection Reached!\r\n", 29);
			close(sockd2);
			continue;
		}
		try {
			g_enqueue_list1.emplace_back();
			penqueue = &g_enqueue_list1.back();
		} catch (const std::bad_alloc &) {
			eq_hold.unlock();
			write(sockd2, "ENOMEM\r\n", 8);
			close(sockd2);
			continue;
		}

		penqueue->sockd = sockd2;
		eq_hold.unlock();
		write(sockd2, "OK\r\n", 4);
		g_enqueue_waken_cond.notify_one();
	}
	return nullptr;
}

static void *ev_enqwork(void *param)
{
	int temp_len;
	char *pspace;
	char *pspace1;
	char *pspace2;
	BOOL b_result;
	time_t cur_time;
	MEM_FILE temp_file;
	char temp_string[256];
	
 NEXT_LOOP:
	if (g_notify_stop)
		return nullptr;
	std::unique_lock cm_hold(g_enqueue_cond_mutex);
	g_enqueue_waken_cond.wait(cm_hold);
	cm_hold.unlock();
	if (g_notify_stop)
		return nullptr;
	std::unique_lock eq_hold(g_enqueue_lock);
	if (g_enqueue_list1.size() == 0)
		goto NEXT_LOOP;
	auto eq_node = g_enqueue_list1.begin();
	auto penqueue = &*eq_node;
	g_enqueue_list.splice(g_enqueue_list.end(), g_enqueue_list1, eq_node);
	eq_hold.unlock();
	
	while (TRUE) {
		if (FALSE == read_mark(penqueue)) {
			eq_hold.lock();
			g_enqueue_list.erase(eq_node);
			goto NEXT_LOOP;
		}
		
		if (0 == strncasecmp(penqueue->line, "ID ", 3)) {
			strncpy(penqueue->res_id, penqueue->line + 3, 128);
			write(penqueue->sockd, "TRUE\r\n", 6);
			continue;
		} else if (0 == strncasecmp(penqueue->line, "LISTEN ", 7)) {
			HOST_NODE *phost = nullptr;
			std::shared_ptr<DEQUEUE_NODE> pdequeue;
			try {
				pdequeue = std::make_shared<DEQUEUE_NODE>();
			} catch (const std::bad_alloc &) {
				write(penqueue->sockd, "FALSE\r\n", 7);
				continue;
			}
			strncpy(pdequeue->res_id, penqueue->line + 7, 128);
			fifo_init(&pdequeue->fifo, g_fifo_alloc, sizeof(MEM_FILE),
				FIFO_AVERAGE_LENGTH);
			std::unique_lock hl_hold(g_host_lock);
			auto host_it = std::find_if(g_host_list.begin(), g_host_list.end(),
			               [&](const HOST_NODE &h) { return strcmp(h.res_id, penqueue->line + 7) == 0; });
			if (host_it == g_host_list.end()) {
				try {
					g_host_list.emplace_back();
					phost = &g_host_list.back();
				} catch (const std::bad_alloc &) {
					hl_hold.unlock();
					write(penqueue->sockd, "FALSE\r\n", 7);
					continue;
				}
				gx_strlcpy(phost->res_id, penqueue->line + 7, GX_ARRAY_SIZE(phost->res_id));
			} else {
				phost = &*host_it;
			}
			time(&phost->last_time);
			try {
				phost->list.push_back(pdequeue);
			} catch (const std::bad_alloc &) {
				write(pdequeue->sockd, "FALSE\r\n", 7);
				continue;
			}
			try {
				std::lock_guard dq_hold(g_dequeue_lock);
				g_dequeue_list1.push_back(pdequeue);
			} catch (const std::bad_alloc &) {
				phost->list.pop_back();
				write(pdequeue->sockd, "FALSE\r\n", 7);
				continue;
			}
			pdequeue->sockd = penqueue->sockd;
			penqueue->sockd = -1;
			hl_hold.unlock();
			write(pdequeue->sockd, "TRUE\r\n", 6);
			g_dequeue_waken_cond.notify_one();
			eq_hold.lock();
			g_enqueue_list.erase(eq_node);
			goto NEXT_LOOP;
		} else if (0 == strncasecmp(penqueue->line, "SELECT ", 7)) {
			pspace = strchr(penqueue->line + 7, ' ');
			temp_len = pspace - (penqueue->line + 7);
			if (NULL == pspace ||  temp_len > 127 || strlen(pspace + 1) > 63) {
				write(penqueue->sockd, "FALSE\r\n", 7);
				continue;
			}
			memcpy(temp_string, penqueue->line + 7, temp_len);
			temp_string[temp_len] = ':';
			temp_len ++;
			temp_string[temp_len] = '\0';
			HX_strlower(temp_string);
			strcat(temp_string, pspace + 1);
			
			b_result = FALSE;
			std::unique_lock hl_hold(g_host_lock);
			for (auto &hnode : g_host_list) {
				auto phost = &hnode;
				if (0 == strcmp(penqueue->res_id, phost->res_id)) {
					time(&cur_time);
					auto time_it = phost->hash.find(temp_string);
					if (time_it != phost->hash.end()) {
						time_it->second = cur_time;
					} else try {
						phost->hash.emplace(temp_string, cur_time);
					} catch (const std::bad_alloc &) {
					}
					b_result = TRUE;
					break;
				}
			}
			hl_hold.unlock();
			if (TRUE == b_result) {
				write(penqueue->sockd, "TRUE\r\n", 6);
			} else {
				write(penqueue->sockd, "FALSE\r\n", 7);
			}
			continue;
		} else if (0 == strncasecmp(penqueue->line, "UNSELECT ", 9)) {
			pspace = strchr(penqueue->line + 9, ' ');
			temp_len = pspace - (penqueue->line + 9);
			if (NULL == pspace ||  temp_len > 127 || strlen(pspace + 1) > 63) {
				write(penqueue->sockd, "FALSE\r\n", 7);
				continue;
			}
			memcpy(temp_string, penqueue->line + 9, temp_len);
			temp_string[temp_len] = ':';
			temp_len ++;
			temp_string[temp_len] = '\0';
			HX_strlower(temp_string);
			strcat(temp_string, pspace + 1);
			
			std::unique_lock hl_hold(g_host_lock);
			auto phost = std::find_if(g_host_list.begin(), g_host_list.end(),
			             [&](const HOST_NODE &h) { return strcmp(penqueue->res_id, h.res_id) == 0; });
			if (phost != g_host_list.end())
				phost->hash.erase(temp_string);
			hl_hold.unlock();
			write(penqueue->sockd, "TRUE\r\n", 6);
			continue;
		} else if (0 == strcasecmp(penqueue->line, "QUIT")) {
			write(penqueue->sockd, "BYE\r\n", 5);
			eq_hold.lock();
			g_enqueue_list.erase(eq_node);
			goto NEXT_LOOP;
		} else if (0 == strcasecmp(penqueue->line, "PING")) {
			write(penqueue->sockd, "TRUE\r\n", 6);	
			continue;
		} else {
			pspace = strchr(penqueue->line, ' ');
			if (NULL == pspace) {
				write(penqueue->sockd, "FALSE\r\n", 7);
				continue;
			}
			pspace1 = strchr(pspace + 1, ' ');
			if (NULL == pspace1) {
				write(penqueue->sockd, "FALSE\r\n", 7);
				continue;
			}
			pspace2 = strchr(pspace1 + 1, ' ');
			if (NULL == pspace2) {
				pspace2 = penqueue->line + strlen(penqueue->line);
			}
			if (pspace1 - pspace > 128 || pspace2 - pspace1 > 64) {
				write(penqueue->sockd, "FALSE\r\n", 7);
				continue;
			}
			temp_len = pspace1 - (pspace + 1);
			memcpy(temp_string, pspace + 1, temp_len);
			temp_string[temp_len] = ':';
			temp_len ++;
			temp_string[temp_len] = '\0';
			HX_strlower(temp_string);
			memcpy(temp_string + temp_len, pspace1 + 1, pspace2 - pspace1 - 1);
			temp_string[temp_len + (pspace2 - pspace1 - 1)] = '\0';

			std::unique_lock hl_hold(g_host_lock);
			for (auto &hnode : g_host_list) {
				auto phost = &hnode;
				if (0 == strcmp(penqueue->res_id, phost->res_id) ||
				    phost->hash.find(temp_string) == phost->hash.cend())
					continue;
				
				if (phost->list.size() > 0) {
					auto pdequeue = phost->list.front();
					phost->list.erase(phost->list.begin());
					mem_file_init(&temp_file, g_file_alloc);
					mem_file_write(&temp_file, penqueue->line,
						strlen(penqueue->line));
					std::unique_lock dl_hold(pdequeue->lock);
					b_result = fifo_enqueue(&pdequeue->fifo, &temp_file);
					dl_hold.unlock();
					if (FALSE == b_result) {
						mem_file_free(&temp_file);
					} else {
						pdequeue->waken_cond.notify_one();
					}
					phost->list.push_back(pdequeue);
				}
			}
			hl_hold.unlock();
			write(penqueue->sockd, "TRUE\r\n", 6);
			continue;
		}
	}
	return NULL;
}

static void *ev_deqwork(void *param)
{
	MEM_FILE *pfile;
	time_t cur_time;
	time_t last_time;
	MEM_FILE temp_file;
	char buff[MAX_CMD_LENGTH];
	
 NEXT_LOOP:
	std::unique_lock dc_hold(g_dequeue_cond_mutex);
	g_dequeue_waken_cond.wait(dc_hold);
	dc_hold.unlock();
	if (g_notify_stop)
		return nullptr;
	std::unique_lock dq_hold(g_dequeue_lock);
	if (g_dequeue_list1.size() == 0)
		goto NEXT_LOOP;
	auto pdequeue = g_dequeue_list1.front();
	g_dequeue_list1.erase(g_dequeue_list1.begin());
	dq_hold.unlock();
	
	time(&last_time);
	std::unique_lock hl_hold(g_host_lock);
	auto phost = std::find_if(g_host_list.begin(), g_host_list.end(),
	             [&](const HOST_NODE &h) { return strcmp(h.res_id, pdequeue->res_id) == 0; });
	if (phost == g_host_list.end())
		goto NEXT_LOOP;
	hl_hold.unlock();
	
	while (!g_notify_stop) {
		dc_hold.lock();
		pdequeue->waken_cond.wait_for(dc_hold, std::chrono::seconds(1));
		dc_hold.unlock();
		if (g_notify_stop)
			break;
		dq_hold.lock();
		pfile = static_cast<MEM_FILE *>(fifo_get_front(&pdequeue->fifo));
		if (NULL != pfile) {
			temp_file = *pfile;
			fifo_dequeue(&pdequeue->fifo);
		}
		dq_hold.unlock();
		time(&cur_time);
		
		if (NULL == pfile) {	
			if (cur_time - last_time >= SOCKET_TIMEOUT - 3) {
				if (6 != write(pdequeue->sockd, "PING\r\n", 6) ||
					FALSE == read_response(pdequeue->sockd)) {
					hl_hold.lock();
					auto it = std::find(phost->list.begin(), phost->list.end(), pdequeue);
					if (it != phost->list.end())
						phost->list.erase(it);
					hl_hold.unlock();
					close(pdequeue->sockd);
					pdequeue->sockd = -1;
					while ((pfile = static_cast<MEM_FILE *>(fifo_get_front(&pdequeue->fifo))) != nullptr) {
						mem_file_free(pfile);
						fifo_dequeue(&pdequeue->fifo);
					}
					goto NEXT_LOOP;
				}
				last_time = cur_time;
				hl_hold.lock();
				phost->last_time = cur_time;
				hl_hold.unlock();
			}
			continue;
		}
		
		int len = mem_file_read(&temp_file, buff, arsizeof(buff) - 2);
		buff[len] = '\r';
		len ++;
		buff[len] = '\n';
		len ++;
		mem_file_free(&temp_file);
		if (len != write(pdequeue->sockd, buff, len) ||
			FALSE == read_response(pdequeue->sockd)) {
			hl_hold.lock();
			auto it = std::find(phost->list.begin(), phost->list.end(), pdequeue);
			if (it != phost->list.end())
				phost->list.erase(it);
			hl_hold.unlock();
			close(pdequeue->sockd);
			pdequeue->sockd = -1;
			while ((pfile = static_cast<MEM_FILE *>(fifo_get_front(&pdequeue->fifo))) != nullptr) {
				mem_file_free(pfile);
				fifo_dequeue(&pdequeue->fifo);
			}
			goto NEXT_LOOP;
		}
		
		last_time = cur_time;
		hl_hold.lock();
		phost->last_time = cur_time;
		hl_hold.unlock();
	}	
	return NULL;
}

static BOOL read_response(int sockd)
{
	fd_set myset;
	int offset;
	int read_len;
	char buff[1024];
	struct timeval tv;

	offset = 0;
	while (TRUE) {
		tv.tv_usec = 0;
		tv.tv_sec = SOCKET_TIMEOUT;
		FD_ZERO(&myset);
		FD_SET(sockd, &myset);
		if (select(sockd + 1, &myset, NULL, NULL, &tv) <= 0) {
			return FALSE;
		}
		read_len = read(sockd, buff + offset, 1024 - offset);
		if (read_len <= 0) {
			return FALSE;
		}
		offset += read_len;
		
		if (6 == offset) {
			if (0 == strncasecmp(buff, "TRUE\r\n", 6)) {
				return TRUE;
			} else {
				return FALSE;
			}
		}
		
		if (offset > 6) {
			return FALSE;
		}
	}
}

static BOOL read_mark(ENQUEUE_NODE *penqueue)
{
	fd_set myset;
	int i, read_len;
	struct timeval tv;

	while (TRUE) {
		tv.tv_usec = 0;
		tv.tv_sec = SOCKET_TIMEOUT;
		FD_ZERO(&myset);
		FD_SET(penqueue->sockd, &myset);
		if (select(penqueue->sockd + 1, &myset, NULL, NULL, &tv) <= 0) {
			return FALSE;
		}
		read_len = read(penqueue->sockd, penqueue->buffer +
		penqueue->offset, MAX_CMD_LENGTH - penqueue->offset);
		if (read_len <= 0) {
			return FALSE;
		}
		penqueue->offset += read_len;
		for (i=0; i<penqueue->offset-1; i++) {
			if ('\r' == penqueue->buffer[i] &&
				'\n' == penqueue->buffer[i + 1]) {
				memcpy(penqueue->line, penqueue->buffer, i);
				penqueue->line[i] = '\0';
				penqueue->offset -= i + 2;
				memmove(penqueue->buffer, penqueue->buffer + i + 2,
					penqueue->offset);
				return TRUE;
			}
		}
		if (MAX_CMD_LENGTH == penqueue->offset) {
			return FALSE;
		}
	}
}

static void term_handler(int signo)
{
	g_notify_stop = true;
}
