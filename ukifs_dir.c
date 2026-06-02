/*
 * uki_dir.c - Directory Tree Management
 */

#include <stdlib.h>
#include <string.h>

#include "ukifs_core.h"

/* Create a new directory node */
dir_node_t *create_dir_node(const char *name) {
    DEBUG_ENTER();

    dir_node_t *node = calloc(1, sizeof(dir_node_t));
    if (!node) {
        DEBUG_LOG("Failed to allocate dir_node");
        DEBUG_EXIT(0);
        return NULL;
    }

    node->name = strdup(name);
    if (!node->name) {
        DEBUG_LOG("Failed to duplicate name");
        free(node);
        DEBUG_EXIT(0);
        return NULL;
    }

    node->children_capacity = DIR_INITIAL_CAPACITY;
    node->children = calloc(node->children_capacity, sizeof(dir_node_t *));
    if (!node->children) {
        DEBUG_LOG("Failed to allocate children array");
        free(node->name);
        free(node);
        DEBUG_EXIT(0);
        return NULL;
    }

    DEBUG_EXIT(0);
    return node;
}

/* Free a directory node and all its children recursively */
void free_dir_node(dir_node_t *node) {
    DEBUG_ENTER();

    if (!node) {
        DEBUG_EXIT(0);
        return;
    }

    /* Free all children */
    for (size_t i = 0; i < node->num_children; i++) {
        free_dir_node(node->children[i]);
    }

    free(node->children);
    free(node->name);
    free(node);
    DEBUG_EXIT(0);
}

/* Add a child to a directory node */
int add_child(dir_node_t *parent, dir_node_t *child) {
    DEBUG_ENTER();

    if (!parent || !child) {
        DEBUG_LOG("parent or child is NULL");
        DEBUG_EXIT(-1);
        return -1;
    }

    /* Check if we need to grow the children array */
    if (parent->num_children >= parent->children_capacity) {
        size_t new_capacity = parent->children_capacity * 2;
        DEBUG_LOG("Growing children array from %zu to %zu",
                  parent->children_capacity, new_capacity);
        dir_node_t **new_children = realloc(parent->children,
					    new_capacity * sizeof(dir_node_t *));
        if (!new_children) {
            DEBUG_LOG("Failed to grow children array");
            DEBUG_EXIT(-1);
            return -1;
        }
        parent->children = new_children;
        parent->children_capacity = new_capacity;
    }

    /* Add child */
    parent->children[parent->num_children++] = child;
    child->parent = parent;

    DEBUG_EXIT(0);
    return 0;
}

/* Find a directory node by path */
dir_node_t *find_dir_node(dir_node_t *root, const char *path) {
    DEBUG_ENTER();

    if (!root || !path) {
        DEBUG_LOG("root or path is NULL");
        DEBUG_EXIT(0);
        return NULL;
    }

    /* Handle root directory */
    if (strcmp(path, "/") == 0 || strcmp(path, "") == 0) {
        DEBUG_LOG("path is root");
        DEBUG_EXIT(0);
        return root;
    }

    /* For UKI filesystem, we only have a flat structure (root + files)
     * So any other path is not a directory */
    DEBUG_LOG("path is not root, returning NULL");
    DEBUG_EXIT(0);
    return NULL;
}

/* Add an entry to the directory tree (flat structure: all files under root) */
int add_to_dir_tree(dir_node_t *root, uki_entry_t *entry) {
    DEBUG_ENTER();

    if (!root || !entry) {
        DEBUG_LOG("root or entry is NULL");
        DEBUG_EXIT(-1);
        return -1;
    }

    DEBUG_LOG("adding entry '%s'", entry->name);

    /* Create a directory node for this file entry */
    dir_node_t *file_node = calloc(1, sizeof(dir_node_t));
    if (!file_node) {
        DEBUG_LOG("Failed to allocate file node");
        DEBUG_EXIT(-1);
        return -1;
    }

    file_node->name = strdup(entry->name);
    if (!file_node->name) {
        DEBUG_LOG("Failed to duplicate name");
        free(file_node);
        DEBUG_EXIT(-1);
        return -1;
    }

    file_node->entry = entry;
    file_node->children = NULL;
    file_node->num_children = 0;
    file_node->children_capacity = 0;

    /* Add to root's children */
    if (add_child(root, file_node) != 0) {
        DEBUG_LOG("Failed to add child to root");
        free(file_node->name);
        free(file_node);
        DEBUG_EXIT(-1);
        return -1;
    }

    DEBUG_LOG("entry '%s' added to directory tree", entry->name);
    DEBUG_EXIT(0);
    return 0;
}
