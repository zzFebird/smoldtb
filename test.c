#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include "smoldtb.h"

#define B(x, n) ((uint8_t)(x >> (n * 8)) & 0xff)

void dtb_on_error(const char* why)
{
    printf("smol-dtb error: %s\r\n", why);
    exit(1);
}

void* dtb_malloc(uint32_t length)
{
    return malloc(length);
}

void print_node(dtb_node* node, uint32_t indent)
{
    const uint32_t indent_scale = 2;
    if (node == NULL)
        return;

    char indent_buff[indent + 1];
    for (uint32_t i = 0; i < indent; i++)
        indent_buff[i] = ' ';
    indent_buff[indent] = 0;
    
    dtb_node_stat stat;
    dtb_stat_node(node, &stat);
    printf("%s[+] %s: %u siblings, %u children, %u properties.\r\n", indent_buff, 
        stat.name, stat.sibling_count, stat.child_count, stat.prop_count);

    for (uint32_t i = 0; i < stat.prop_count; i++)
    {
        dtb_prop* prop = dtb_get_prop(node, i);
        if (prop == NULL)
            break;
        //NOTE: DO NOT DO THIS! This is a hack for testing purposes for I can make print pretty
        //trees and check all properties are read correctly. There's a reason these structs are
        //opaque to calling code, and their underlying definitions can change at any time.
        const char* name = *(const char**)prop;
        printf("%s  | %s\r\n", indent_buff, name);
    }

    dtb_node* child = dtb_get_child(node);
    while (child != NULL)
    {
        print_node(child, indent + indent_scale);
        child = dtb_get_sibling(child);
    }
}

void display_file(const char* filename)
{
    int fd = open(filename, O_RDONLY);
    if (fd == -1)
    {
        printf("Could not open file %s\r\n", filename);
        return;
    }

    struct stat sb;
    if (fstat(fd, &sb) == -1)
    {
        printf("Could not stat file %s\r\n", filename);
        return;
    }

    void* buffer = mmap(NULL, sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (buffer == NULL)
    {
        printf("mmap() failed\r\n");
        return;
    }

    dtb_ops ops;
    ops.malloc = dtb_malloc;
    ops.on_error = dtb_on_error;
    dtb_init((uintptr_t)buffer, ops);

#if 0
    dtb_node *root = dtb_find("/");
    while (root != NULL)
    {
        print_node(root, 0);
        root = dtb_get_sibling(root);
    }
#endif

    char byte;
    uint32_t val;
    dtb_node *node, *cpus;
    dtb_prop *prop;
    node = dtb_find("chosen");
    if (node != NULL) {
        prop = dtb_find_prop(node, "bootargs");
        if (prop != NULL) {
            dtb_read_prop_bytestring(prop, &byte);
            printf("bootargs %d\n", byte);
        }
    }

    cpus = dtb_find("cpus");
    if (cpus != NULL) {
        node = dtb_find_child(cpus, "cpu");
        if (node != NULL) {
            prop = dtb_find_prop(node, "phandle");
            if (prop != NULL) {
                dtb_read_prop_cell_array(prop, node->addr_cells, &val);
                printf("cpus->cpu: phandle %u\n", val);
            }
        }

        node = dtb_find_child(cpus, "cpu-map");
        if (node != NULL) {
            node = dtb_find_child(node, "cluster0");
            if (node != NULL) {
                node = dtb_find_child(node, "core1");
                if (node != NULL) {
                    prop = dtb_find_prop(node, "cpu");
                    if (prop != NULL) {
                        dtb_read_prop_cell_array(prop, node->addr_cells, &val);
                        node = dtb_find_phandle(val);
                        if (node != NULL) {
                            printf("cpu-map->cluster0->core1: cpu %u, node %s\n", val, node->name);
                        }
                    }
                }
            }
        }
    }

    node = dtb_find("soc");
    if (node != NULL) {
        node = dtb_find_compatible(node, "ns16550a");
        if (node != NULL) {
            printf("compatible ns16550a: %s\n", node->name);
        }
    }

    munmap(buffer, sb.st_size);
    close(fd);
}

void show_usage()
{
    printf("Usage: \
            readfdt <filename.dtb> \
            \
            This program will parse a flattened device tree/device tree blob and \
            output a summary of it's contents. \
            This programs intended purpose is for testing the smoldtb library code. \
            \r\n");
}

int main(int argc, char** argv)
{
    if (argc == 1)
    {
        show_usage();
        return 0;
    }

    display_file(argv[1]);
    return 0;
}

