#ifdef HAVE_CONFIG_H
#	include "config.h"
#endif
#include <libHX/option.h>
#include "mail.h"
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

static unsigned int opt_show_version;

static struct HXoption g_options_table[] = {
	{.ln = "version", .type = HXTYPE_NONE, .ptr = &opt_show_version, .help = "Output version information and exit"},
	HXOPT_AUTOHELP,
	HXOPT_TABLEEND,
};

int main(int argc, const char **argv)
{
	MAIL imail;
	char *pbuff;
	char *pbuff1;
	size_t offset;
	int fd, tmp_len;
	MIME_POOL *ppool;
	struct stat node_stat;
	
	if (HX_getopt(g_options_table, &argc, &argv, HXOPT_USAGEONERR) < 0)
		return EXIT_FAILURE;
	if (opt_show_version) {
		printf("version: %s\n", PROJECT_VERSION);
		return 0;
	}
	if (2 != argc) {
		printf("%s msg-path\n", argv[0]);
		return 1;
	}
    
	if (0 != stat(argv[1], &node_stat)) {
		printf("fail to find %s\n", argv[1]);
		return 1;
	}
	
	if (0 == S_ISREG(node_stat.st_mode)) {
		printf("%s is not regular file\n", argv[1]);
		return 2;
	}
	
	pbuff = malloc(node_stat.st_size);

	if (NULL == pbuff) {
		printf("fail to allocate memory\n");
		return 3;
	}
	
	fd = open(argv[1], O_RDONLY);
	if (-1 == fd) {
		printf("fail to open file %s\n", argv[1]);
		free(pbuff);
		return 4;
	}

	if (node_stat.st_size != read(fd, pbuff, node_stat.st_size)) {
		printf("fail to read file %s\n", argv[1]);
		free(pbuff);
		close(fd);
		return 5;
	}

	close(fd);

	ppool = mime_pool_init(1024, 32, FALSE);

	if (NULL == ppool) {
		free(pbuff);
		printf("fail to init mime pool\n");
		return 6;
	}

	
	mail_init(&imail, ppool);
		
	if (FALSE == mail_retrieve(&imail, pbuff, node_stat.st_size)) {
		free(pbuff);
		mime_pool_free(ppool);
		printf("fail to retrieve file into mail object\n");
		return 7;
	}

	pbuff1 = malloc(1024*1024);
	if (NULL == pbuff1) {
		printf("fail to allocate digest memory\n");
		free(pbuff);
		mime_pool_free(ppool);
		return 8;
	}

	const char *pslash = strrchr(argv[1], '/');
	if (NULL == pslash) {
		pslash = argv[1];
	} else {
		pslash ++;
	}
	

	tmp_len = sprintf(pbuff1, "{\"file\":\"%s\",", pslash);

	if (1 != mail_get_digest(&imail, &offset, pbuff1 + tmp_len,
		1024*1024 - tmp_len - 2)) {
		printf("fail to digest message\n");
		free(pbuff);
		mime_pool_free(ppool);
		return 8;
	}

	tmp_len = strlen(pbuff1);
	memcpy(pbuff1+ tmp_len, "}", 2);
	printf(pbuff1);

	mail_free(&imail);
	mime_pool_free(ppool);
	free(pbuff);
	free(pbuff1);

}

