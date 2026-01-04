#include "hashtable.h"
#include <assert.h>

static void h_init(HTab *htab, size_t n){
    assert( n > 0 && (( n -1)& n) == 0);
    htab->tab = (HNode **)calloc(n,(sizeof(HNode *)));
}

static void h_insert(HTab *htab, HNode *node){
    size_t pos = node->hcode & htab->mask;
    HNode *next = htab->tab[pos];
    node->next = next;
    htab->tab[pos] = node;
    htab->size++;
}

static HNode **hh_lookup(HTab *htab, HNode *key, bool (*eq)(HNode *, HNode *)){
    if (!htab->tab){
        return NULL;
    }
    size_t pos = key->hcode & htab->mask;
    HNode **from = &htab->tab[pos];
}