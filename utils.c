
#include "utils.h"

Node *node_create(void *value) {
  Node *tmp = (Node *)malloc(sizeof(Node));
  tmp->value = value;
  tmp->next = NULL;
  return tmp;
}

List *list_create() {
  List *tmp = (List *)malloc(sizeof(List));
  tmp->first = NULL;
  tmp->last = NULL;
  tmp->count = 0;
  return tmp;
}

void list_append_int(List *list, int num){
  int *value = (int *)malloc(sizeof(int));
  *value=num;
  list_append(list,value);
}

void list_append_u_long(List *list, unsigned long num){
  unsigned long *value = (unsigned long *)malloc(sizeof(unsigned long));
  *value=num;
  list_append(list,value);
}

void list_append(List *list, void *value) {
  Node *node = node_create(value);
  if (list->count == 0) {
    list->first = node;
  } else {
    list->last->next = node;
  }
  list->last = node;
  list->count++;
}

void list_free(List *list) {
  Node *node = list->first;
  Node *nextNode = NULL;
  while (node != NULL) {
    nextNode = node->next;
    free(node->value);
    free(node);
    node = nextNode;
  }
  free(list);
}

int node_get_int(Node *node){
  return *((int *)node->value);
}
unsigned long node_get_u_long(Node *node){
  return *((unsigned long *)node->value);
}
