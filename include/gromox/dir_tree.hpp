#pragma once
#include <gromox/simple_tree.hpp>
#include <gromox/lib_buffer.hpp>
#include <gromox/mem_file.hpp>

struct DIR_NODE {
	SIMPLE_TREE_NODE node;
	BOOL b_loaded;
	char name[256];
	LIB_BUFFER *ppool;
};

struct DIR_TREE {
	SIMPLE_TREE tree;
	LIB_BUFFER  *ppool;
};

typedef void (*DIR_TREE_ENUM)(DIR_NODE*, void*);

LIB_BUFFER* dir_tree_allocator_init(size_t max_size, BOOL thread_safe);
void dir_tree_allocator_free(LIB_BUFFER *pallocator);
void dir_tree_init(DIR_TREE *ptree, LIB_BUFFER *pallocator);
void dir_tree_retrieve(DIR_TREE *ptree, MEM_FILE *pfile);
DIR_NODE* dir_tree_match(DIR_TREE *ptree, const char *path);
DIR_NODE* dir_tree_get_child(DIR_NODE* pdir);
void dir_tree_free(DIR_TREE *ptree);
