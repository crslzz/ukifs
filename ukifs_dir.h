/*
 * Directory Management
 */

#ifndef UKIFS_DIR_H
#define UKIFS_DIR_H

struct uki_entry;

/*
 * Directory Node Structure
 * Simplified for flat structure (root directory + section files)
 */
typedef struct dir_node {
    char *name;                    /* Directory name (always "/" for root) */
    struct uki_entry *entry;       /* Corresponding entry (or NULL for root) */
    struct dir_node **children;    /* Array of child nodes */
    size_t num_children;           /* Number of children (section files) */
    size_t children_capacity;      /* Allocated capacity */
    struct dir_node *parent;       /* Parent directory (always NULL for root) */
} dir_node_t;

dir_node_t *create_dir_node(const char *name);
void free_dir_node(dir_node_t *node);
int add_child(dir_node_t *parent, dir_node_t *child);
dir_node_t *find_dir_node(dir_node_t *root, const char *path);
int add_to_dir_tree(dir_node_t *root, struct uki_entry *entry);

#endif /* UKIFS_DIR_H */
