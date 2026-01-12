#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "utils.h"

// 1. Format network speed for display
void format_speed(char *buf, size_t len, unsigned long bytes) {
    if (bytes >= 1024 * 1024 * 1024) {
        snprintf(buf, len, "%.2f GB/s", bytes / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024 * 1024) {
        snprintf(buf, len, "%.2f MB/s", bytes / (1024.0 * 1024.0));
    } else if (bytes >= 1024) {
        snprintf(buf, len, "%.2f KB/s", bytes / 1024.0);
    } else {
        snprintf(buf, len, "%lu B/s", bytes);
    }
}

// 2. Create a new linked list for graph data
List* list_create() {
    List *list = malloc(sizeof(List));
    list->first = NULL;
    list->last = NULL;
    list->count = 0;
    return list;
}

// 3. Append integer data (for CPU/RAM percentages)
void list_append_int(List *list, int value) {
    Node *node = malloc(sizeof(Node));
    node->value = malloc(sizeof(int));
    *(int*)node->value = value;
    node->next = NULL;
    if (list->last) list->last->next = node;
    else list->first = node;
    list->last = node;
    list->count++;
}

// 4. Append long data (for Network speeds)
void list_append_u_long(List *list, unsigned long value) {
    Node *node = malloc(sizeof(Node));
    node->value = malloc(sizeof(unsigned long));
    *(unsigned long*)node->value = value;
    node->next = NULL;
    if (list->last) list->last->next = node;
    else list->first = node;
    list->last = node;
    list->count++;
}

// 5. Helper to retrieve integer values from nodes
int node_get_int(Node *node) {
    return *(int*)node->value;
}

// 6. Helper to retrieve long values from nodes
unsigned long node_get_u_long(Node *node) {
    return *(unsigned long*)node->value;
}

// 7. Free the list memory
void list_free(List *list) {
    Node *node = list->first;
    while (node) {
        Node *next = node->next;
        free(node->value);
        free(node);
        node = next;
    }
    free(list);
}
