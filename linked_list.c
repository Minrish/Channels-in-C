#include "linked_list.h"


static void init_list(list_t* list) {
    list->head->prev = list->head;
    list->head->next = list->head;
    list->count = 0;
}

static void insert_node(list_node_t* prev_node, list_node_t* next_node, list_node_t* node) {
    prev_node->next = node;
    node->prev = prev_node;
    node->next= next_node;
    next_node->prev = node;
}

static list_node_t * remove_node(list_node_t* node) {
    node->prev->next = node->next;
    node->next->prev = node->prev;
    return node;
}

// Creates and returns a new list
list_t* list_create() {
    list_t* list = malloc(sizeof (list_t));
    list->head = malloc(sizeof(list_node_t));
    init_list(list);
    return list;
}

// Destroys a list
void list_destroy(list_t* list) {
    while (list->count) {
        list_node_t* node = remove_node(list_begin(list));
        list->count -= 1;
        free(node);
    }
    free(list->head);
    free(list);
}

// Returns beginning of the list
list_node_t* list_begin(list_t* list) {
    if (list->count) return list->head->next;
    return NULL;
}

// Returns next element in the list
list_node_t* list_next(list_node_t* node) {
    return node? node->next : NULL;
}

// Returns data in the given list node
void* list_data(list_node_t* node) {
    return node? node->data : NULL;
}

// Returns the number of elements in the list
size_t list_count(list_t* list) {
    return list->count;
}

// Finds the first node in the list with the given data
// Returns NULL if data could not be found
list_node_t* list_find(list_t* list, void* data) {
    list_node_t * node;
    if (!data) return NULL;
    for (node = list->head->next; node != list->head; node = node->next) {
        if (node->data == data) {
            return node;
        }
    }
    return NULL;
}

// Inserts a new node in the list with the given data
void list_insert(list_t* list, void* data) {
    list_node_t* node = malloc(sizeof(list_node_t));
    node->data = data;
    insert_node(list->head->prev, list->head, node);
    list->count += 1;
}

// Removes a node from the list and frees the node resources
void list_remove(list_t* list, list_node_t* node) {
    remove_node(node);
    free(node);
    list->count -= 1;
}

// Executes a function for each element in the list
void list_foreach(list_t* list, void (*func)(void* data)) {
    list_node_t * node;
    for (node = list->head->next; node != list->head; node = node->next) {
        func(node->data);
    }
}
