//#include <stdio.h>
#ifndef UTILS_H
#define UTILS_H
#include <stdlib.h>

typedef struct node {
  void *value;
  struct node *next;
} Node;

typedef struct list {
  Node *first;
  Node *last;
  int count;
} List;

Node *node_create(void *value);

List *list_create();

void list_append(List *list, void *value);
void list_append_int(List *list, int num);
void list_append_u_long(List *list, unsigned long num);
void list_free(List *list);
int node_get_int(Node *node);
unsigned long node_get_u_long(Node *node);
#endif
