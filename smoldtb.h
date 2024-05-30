#pragma once

#include <stdint.h>
#include <stddef.h>

typedef struct dtb_node_t dtb_node;
typedef struct dtb_prop_t dtb_prop;

/* The 'fdt_*' structs represent data layouts taken directly from the device tree
 * specification. In contrast the 'dtb_*' structs are for the parser.
 *
 * The public API only supports a single (global) active parser, but internally everything
 * is stored within an instance of the 'dtb_state' struct. This should make it easy
 * to support multiple parser instances in the future if needed. Or if you're here to
 * hack that support in, it should hopefully require minimal effort.
 */
struct fdt_header
{
    uint32_t magic;
    uint32_t total_size;
    uint32_t offset_structs;
    uint32_t offset_strings;
    uint32_t offset_memmap_rsvd;
    uint32_t version;
    uint32_t last_comp_version;
    uint32_t boot_cpu_id;
    uint32_t size_strings;
    uint32_t size_structs;
};

struct fdt_reserved_mem_entry
{
    uint64_t base;
    uint64_t length;
};

struct fdt_property
{
    uint32_t length;
    uint32_t name_offset;
};

/* The tree is represented in horizontal slices, where all child nodes are represented
 * in a singly-linked list. Only a pointer to the first child is stored in the parent, and
 * the list is build using the node->sibling pointer.
 * For reference the pointer building the tree are:
 * - parent: go up one level
 * - sibling: the next node on this level. To access the previous node, access the parent and then
 *            the child pointer and iterate to just before the target.
 * - child: the first child node.
 */
struct dtb_node_t
{
    dtb_node* parent;
    dtb_node* sibling;
    dtb_node* child;
    dtb_prop* props;

    const char* name;
    uint8_t addr_cells;
    uint8_t size_cells;
};

/* Similar to nodes, properties are stored a singly linked list. */
struct dtb_prop_t
{
    const char* name;
    const uint32_t* first_cell;
    uint32_t length;
    dtb_prop* next;
};

typedef struct
{
    void* (*malloc)(uint32_t length);
    void (*free)(void* ptr, uint32_t length);
    void (*on_error)(const char* why);
} dtb_ops;

typedef struct
{
    const char* name;
    uint32_t child_count;
    uint32_t prop_count;
    uint32_t sibling_count;
} dtb_node_stat;

void dtb_init(uintptr_t start, dtb_ops ops);

dtb_node* dtb_find_compatible(dtb_node* node, const char* str);
dtb_node* dtb_find_phandle(uint32_t handle);
dtb_node* dtb_find(const char* path);
dtb_node* dtb_find_child(dtb_node* node, const char* name);
dtb_prop* dtb_find_prop(dtb_node* node, const char* name);

dtb_node* dtb_get_sibling(dtb_node* node);
dtb_node* dtb_get_child(dtb_node* node);
dtb_node* dtb_get_parent(dtb_node* node);
dtb_prop* dtb_get_prop(dtb_node* node, uint32_t index);
void dtb_stat_node(dtb_node* node, dtb_node_stat* stat);

const char* dtb_read_prop_string(dtb_prop* prop, uint32_t index);
uint32_t dtb_read_prop_bytestring(dtb_prop* prop, char* vals);
uint32_t dtb_read_prop_cell_array(dtb_prop* prop, uint32_t cell_count, uint32_t* vals);

