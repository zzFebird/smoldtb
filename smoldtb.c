#include "smoldtb.h"
#include <stdio.h>
#include <stdbool.h>

#define FDT_MAGIC 0xD00DFEED
#define FDT_BEGIN_NODE 1
#define FDT_END_NODE 2
#define FDT_PROP 3
#define FDT_NOP 4

#define FDT_CELL_SIZE 4
#define ROOT_NODE_STR "/"

#define DBG 1
#if DBG
uintptr_t dtb_base;
uintptr_t cells_base;
#endif

struct dtb_state
{
    const uint32_t* cells;
    const char* strings;
    uint32_t cell_count;
    dtb_node* root;

    dtb_node** handle_lookup;
    dtb_node* node_buff;
    uint32_t node_alloc_head;
    uint32_t node_alloc_max;
    dtb_prop* prop_buff;
    uint32_t prop_alloc_head;
    uint32_t prop_alloc_max;

    dtb_ops ops;
};

struct dtb_state state;

#ifdef SMOLDTB_STATIC_BUFFER_SIZE
    uint8_t big_buff[SMOLDTB_STATIC_BUFFER_SIZE];
#endif

/* Reads a big-endian 32-bit uint into a native uint32 */
static uint32_t be32(uint32_t input)
{
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return input;
#else
    uint32_t temp = 0;
    temp |= (input & 0xFF) << 24;
    temp |= (input & 0xFF00) << 8;
    temp |= (input & 0xFF0000) >> 8;
    temp |= (input & 0xFF000000) >> 24;
    return temp;
#endif
}

/* Like `strlen`, just an internal version */
static uint32_t string_len(const char* str)
{
    uint32_t count = 0;
    while (str[count] != 0)
        count++;
    return count;
}

/* Returns non-zero if the contents of two strings match, else zero. */
static bool strings_eq(const char* a, const char* b)
{
    uint32_t count = 0;
    while (a[count] == b[count])
    {
        if (a[count] == 0)
            return true;
        count++;
    }
    return false;
}

/* Similar to the above function, but returns success if it reaches the end of the string
 * OR it reaches the length. 
 */
static bool strings_eq_bounded(const char* a, const char* b, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++)
    {
        if (a[i] == 0 && b[i] == 0)
            return true;
        if (a[i] != b[i])
            return false;
    }

    return true;
}

/* Finds the first instance of a character within a string, or returns ~0u */
static uint32_t string_find_char(const char* str, char target)
{
    uint32_t i = 0;
    while (str[i] != target)
    {
        if (str[i] == 0)
            return ~0u;
        i++;
    }
    return i;
}

static uint32_t dtb_align_up(uint32_t input, uint32_t alignment)
{
    return ((input + alignment - 1) / alignment) * alignment;
}

/* Allocator design:
 * The parser allocates a single large buffer from the host kernel, or uses the compile time
 * statically allocated buffer. It then internally breaks that into 3 other buffers:
 * - the first is `node_buff` which is used as a bump allocator for node structs. This function
 *   (`alloc_node()`) uses this buffer to return new pointers.
 * - the second is `prop_buffer` which is similar, but for property structs instead of nodes.
 *   The `alloc_prop()` function uses that buffer.
 * - the last buffer isn't allocated from, but is an array of pointers (one per node). This is
 *   used for phandle lookup. The phandle is the index, and if the array element is non-null,
 *   it points to the dtb_node struct representing the associated node. This makes the assumption
 *   that phandles were allocated in a sane manner (asking a lot of some hardware vendors).
 *   This buffer is populated when nodes are parsed: if a node has a phandle property, it's
 *   inserted into the array at the corresponding index to the phandle value.
 */
static dtb_node* alloc_node()
{
    if (state.node_alloc_head + 1 < state.node_alloc_max)
        return &state.node_buff[state.node_alloc_head++];

    if (state.ops.on_error)
        state.ops.on_error("Node allocator ran out of space");
    return NULL;
}

static dtb_prop* alloc_prop()
{
    if (state.prop_alloc_head + 1 < state.prop_alloc_max)
        return &state.prop_buff[state.prop_alloc_head++];

    if (state.ops.on_error)
        state.ops.on_error("Property allocator ran out of space");
    return NULL;
}

static void free_buffers()
{
#ifdef SMOLDTB_STATIC_BUFFER_SIZE
    state.node_alloc_head = state.node_alloc_max;
    state.prop_alloc_head = state.prop_alloc_max;
#else
    if (state.ops.free == NULL)
    {
        if (state.ops.on_error)
            state.ops.on_error("ops.free() is NULL while trying to free buffers.");
        return;
    }

    uint32_t buff_size = state.node_alloc_max * sizeof(dtb_node);
    buff_size += state.prop_alloc_max * sizeof(dtb_prop);
    buff_size += state.node_alloc_max * sizeof(void*);

    state.ops.free(state.node_buff, buff_size);
    state.node_buff = NULL;
    state.prop_buff = NULL;
    state.handle_lookup = NULL;
    state.node_alloc_max = state.prop_alloc_max = 0;
#endif
}

static void alloc_buffers()
{
    state.node_alloc_max = 0;
    state.prop_alloc_max = 0;
    for (uint32_t i = 0; i < state.cell_count; i++)
    {
        if (be32(state.cells[i]) == FDT_BEGIN_NODE)
            state.node_alloc_max++;
        else if (be32(state.cells[i]) == FDT_PROP)
            state.prop_alloc_max++;
    }

    uint32_t total_size = state.node_alloc_max * sizeof(dtb_node);
    total_size += state.prop_alloc_max * sizeof(dtb_prop);
    total_size += state.node_alloc_max * sizeof(void*); //we assume the worst case and that each node has a phandle prop

#ifdef SMOLDTB_STATIC_BUFFER_SIZE
    if (total_size >= SMOLDTB_STATIC_BUFFER_SIZE)
    {
        if (state.ops.on_error)
            state.ops.on_error("Too much data for statically allocated buffer.");
        return;
    }
    uint8_t* buffer = big_buff;
#else
    uint8_t* buffer = state.ops.malloc(total_size);
#endif

    for (uint32_t i = 0; i < total_size; i++)
        buffer[i] = 0;

    state.node_buff = (dtb_node*)buffer;
    state.node_alloc_head = 0;
    state.prop_buff = (dtb_prop*)&state.node_buff[state.node_alloc_max];
    state.prop_alloc_head = 0;
    state.handle_lookup = (dtb_node**)&state.prop_buff[state.prop_alloc_max];
}

/* This runs on every new property found, and handles some special cases for us. */
static void check_for_special_prop(dtb_node* node, dtb_prop* prop)
{
    const char name0 = prop->name[0];
    if (name0 != '#' && name0 != 'p' && name0 != 'l')
        return; //short circuit to save processing

    const uint32_t name_len = string_len(prop->name);
    const char* name_phandle = "phandle";
    const uint32_t len_phandle = string_len(name_phandle);
    if (name_len == len_phandle && strings_eq(prop->name, name_phandle))
    {
        uint32_t handle;
        dtb_read_prop_cell_array(prop, 1, &handle);
        state.handle_lookup[handle] = node;
        return;
    }

    const char* name_linuxhandle = "linux,phandle";
    const uint32_t len_linuxhandle = string_len(name_linuxhandle);
    if (name_len == len_linuxhandle && strings_eq(prop->name, name_linuxhandle))
    {
        uint32_t handle;
        dtb_read_prop_cell_array(prop, 1, &handle);
        state.handle_lookup[handle] = node;
        return;
    }

    const char* name_addrcells = "#address-cells";
    const uint32_t len_addrcells = string_len(name_addrcells);
    if (name_len == len_addrcells && strings_eq(prop->name, name_addrcells))
    {
        uint32_t cells;
        dtb_read_prop_cell_array(prop, 1, &cells);
        node->addr_cells = cells;
        return;
    }

    const char* name_sizecells = "#size-cells";
    const uint32_t len_sizecells = string_len(name_sizecells);
    if (name_len == len_sizecells && strings_eq(prop->name, name_sizecells))
    {
        uint32_t cells;
        dtb_read_prop_cell_array(prop, 1, &cells);
        node->size_cells = cells;
        return;
    }
}

static dtb_prop* parse_prop(uint32_t* offset)
{
    if (be32(state.cells[*offset]) != FDT_PROP)
        return NULL;

    (*offset)++;
    dtb_prop* prop = alloc_prop();

    const struct fdt_property* fdtprop = (struct fdt_property*)(state.cells + *offset);
    prop->name = (const char*)(state.strings + be32(fdtprop->name_offset));
    prop->first_cell = state.cells + *offset + 2;
    prop->length = be32(fdtprop->length);
    (*offset) += (dtb_align_up(prop->length, FDT_CELL_SIZE) / FDT_CELL_SIZE) + 2;
    
    return prop;
}

static dtb_node* parse_node(uint32_t* offset, uint8_t addr_cells, uint8_t size_cells)
{
    if (be32(state.cells[*offset]) != FDT_BEGIN_NODE)
        return NULL;

    dtb_node* node = alloc_node(); 
    node->name = (const char*)(state.cells + (*offset) + 1);
    node->addr_cells = addr_cells;
    node->size_cells = size_cells;

    const uint32_t name_len = string_len(node->name);
    *offset += (dtb_align_up(name_len + 1, FDT_CELL_SIZE) / FDT_CELL_SIZE) + 1;

    while (*offset < state.cell_count)
    {
        const uint32_t test = be32(state.cells[*offset]);
        if (test == FDT_END_NODE)
        {
            (*offset)++;
            return node;
        }
        else if (test == FDT_BEGIN_NODE)
        {
            dtb_node* child = parse_node(offset, node->addr_cells, node->size_cells);
            if (child)
            {
                child->sibling = node->child;
                node->child = child;
                child->parent = node;
            }
        }
        else if (test == FDT_PROP)
        {
            dtb_prop* prop = parse_prop(offset);
            if (prop)
            {
                prop->next = node->props;
                node->props = prop;
                check_for_special_prop(node, prop);
            }
        }
        else
            (*offset)++;
    }

    if (state.ops.on_error)
        state.ops.on_error("Node has no terminating tag.");
    return NULL;
}

void dtb_init(uintptr_t start, dtb_ops ops)
{
    state.ops = ops;
#if DBG
    dtb_base = start;
#endif
#ifndef SMOLDTB_STATIC_BUFFER_SIZE
    if (!state.ops.malloc)
    {
        if (state.ops.on_error)
            state.ops.on_error("ops.malloc is NULL");
        return;
    }
#endif

    struct fdt_header* header = (struct fdt_header*)start;
    if (be32(header->magic) != FDT_MAGIC)
    {
        if (state.ops.on_error)
            state.ops.on_error("FDT has incorrect magic number.");
        return;
    }

    state.cells = (const uint32_t*)(start + be32(header->offset_structs));
    state.cell_count = be32(header->size_structs) / sizeof(uint32_t);
    state.strings = (const char*)(start + be32(header->offset_strings));

    if (state.node_buff)
        free_buffers();
    alloc_buffers();

    for (uint32_t i = 0; i < state.cell_count; i++)
    {
        if (be32(state.cells[i]) != FDT_BEGIN_NODE)
            continue;

        dtb_node* sub_root = parse_node(&i, 2, 1);
        if (sub_root == NULL)
            continue;
        sub_root->sibling = state.root;
        state.root = sub_root;
    }
}

dtb_node* dtb_find_compatible(dtb_node* start, const char* str)
{
    uint32_t begin_index = 0;
    if (start != NULL)
    {
        const uintptr_t offset = (uintptr_t)start - (uintptr_t)state.node_buff;
        begin_index = offset / sizeof(dtb_node);
        begin_index++; //we want to start searching AFTER this node.
    }

    for (uint32_t i = begin_index; i < state.node_alloc_head; i++)
    {
        dtb_node* node = &state.node_buff[i];
        dtb_prop* compat = dtb_find_prop(node, "compatible");
        if (compat == NULL)
            continue;

        for (uint32_t ci = 0; ; ci++)
        {
            const char* compat_str = dtb_read_prop_string(compat, ci);
            if (compat_str == NULL)
                break;
            if (strings_eq(compat_str, str))
                return node;
        }
    }

    return NULL;
} 

dtb_node* dtb_find_phandle(uint32_t handle)
{
    if (handle < state.node_alloc_max)
        return state.handle_lookup[handle];
    return NULL; //TODO: would it be nicer to just search the tree in this case?
}

static dtb_node* find_child_internal(dtb_node* start, const char* name, uint32_t name_bounds)
{
    dtb_node* scan = start->child;
    while (scan != NULL)
    {
        uint32_t child_name_len = string_find_char(scan->name, '@');
        if (child_name_len == ~0u)
            child_name_len = string_len(scan->name);

        if (child_name_len == name_bounds && strings_eq_bounded(scan->name, name, name_bounds))
            return scan;

        scan = scan->sibling;
    }

    return NULL;
}

dtb_node* dtb_find(const char* name)
{
    uint32_t seg_len;
    dtb_node* scan = state.root;
    while (scan)
    {
        while (name[0] == '/') {
            name++;
        }

        seg_len = string_find_char(name, '/');
        if (seg_len == ~0u)
            seg_len = string_len(name);
        if (seg_len == 0)
            return scan;

        scan = find_child_internal(scan, name, seg_len);
        name += seg_len;
    }

    return NULL;
} 

dtb_node* dtb_find_child(dtb_node* start, const char* name)
{
    if (start == NULL)
        return NULL;

    return find_child_internal(start, name, string_len(name));
}

dtb_prop* dtb_find_prop(dtb_node* node, const char* name)
{
    if (node == NULL)
        return NULL;

    const uint32_t name_len = string_len(name);
    dtb_prop* prop = node->props;
    while (prop)
    {
        const uint32_t prop_name_len = string_len(prop->name);
        if (prop_name_len == name_len && strings_eq(prop->name, name))
            return prop;
        prop = prop->next;
    }

    return NULL;
}

dtb_node* dtb_get_sibling(dtb_node* node)
{
    if (node == NULL || node->sibling == NULL)
        return NULL;
    return node->sibling;
}

dtb_node* dtb_get_child(dtb_node* node)
{
    if (node == NULL)
        return NULL;
    return node->child;
}

dtb_node* dtb_get_parent(dtb_node* node)
{
    if (node == NULL)
        return NULL;
    return node->parent;
}

dtb_prop* dtb_get_prop(dtb_node* node, uint32_t index)
{
    if (node == NULL)
        return NULL;
    
    dtb_prop* prop = node->props;
    while (prop != NULL)
    {
        if (index == 0)
            return prop;

        index--;
        prop = prop->next;
    }

    return NULL;
}

void dtb_stat_node(dtb_node* node, dtb_node_stat* stat)
{
    if (node == NULL)
        return;

    stat->name = ( node == state.root ) ? ROOT_NODE_STR : node->name;

    stat->prop_count = 0;
    dtb_prop* prop = node->props;
    while (prop != NULL)
    {
        prop = prop->next;
        stat->prop_count++;
    }

    stat->child_count = 0;
    dtb_node* child = node->child;
    while (child != NULL)
    {
        child = child->sibling;
        stat->child_count++;
    }

    stat->sibling_count = 0;
    if (node->parent)
    {
        dtb_node* prime = node->parent->child;
        while (prime != NULL)
        {
            prime = prime->sibling;
            stat->sibling_count++;
        }
    }
}

static void extract_cells(const uint32_t* cells, uint32_t count, uint32_t* vals)
{
    for (uint32_t i = 0; i < count; i++)
        vals[i] = be32(cells[i]);
}

const char* dtb_read_prop_string(dtb_prop* prop, uint32_t index)
{
    if (prop == NULL)
        return NULL;
    
    const uint8_t* name = (const uint8_t*)prop->first_cell;
    uint32_t curr_index = 0;
    for (uint32_t scan = 0; scan < prop->length; scan++)
    {
        if (name[scan] == 0)
        {
            curr_index++;
            continue;
        }
        if (curr_index == index)
            return (const char*)&name[scan];
    }

    return NULL;
}

uint32_t dtb_read_prop_bytestring(dtb_prop* prop, char* vals)
{
    if (prop == NULL)
        return 0;
    
    const struct fdt_property* fdtprop = (const struct fdt_property*)(prop->first_cell - 2);
    const uint32_t count = be32(fdtprop->length);
    if (vals == NULL)
        return count;

    for (uint32_t i = 0; i < count; i++)
    {
        const char* base = (char*)prop->first_cell;
        vals[i] = base[i];
    }

    return count;
}

uint32_t dtb_read_prop_cell_array(dtb_prop* prop, uint32_t cell_count, uint32_t* vals)
{
    if (prop == NULL || cell_count == 0)
        return 0;
    
    const struct fdt_property* fdtprop = (const struct fdt_property*)(prop->first_cell - 2);
    const uint32_t count = be32(fdtprop->length) / (cell_count * FDT_CELL_SIZE);
    if (vals == NULL)
        return count;

    for (uint32_t i = 0; i < count; i++)
    {
        const uint32_t* base = prop->first_cell + i * cell_count;
        extract_cells(base, cell_count, vals + i * cell_count);
    }

    return count;
}
