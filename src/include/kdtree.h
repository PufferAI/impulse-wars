/*
This file is part of ``kdtree'', a library for working with kd-trees.
Copyright (C) 2007-2011 John Tsiombikas <nuclear@member.fsf.org>

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.
3. The name of the author may not be used to endorse or promote products
   derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
OF SUCH DAMAGE.
*/
/* single nearest neighbor search written by Tamas Nepusz <tamas@cs.rhul.ac.uk> */
/* in_bounds written by Philip Kovac <philip.kovac@iweave.com> */

#ifndef _KDTREE_H_
#define _KDTREE_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct kdtree kdtree;
typedef struct kdres kdres;

/* create a kd-tree for "k"-dimensional data */
struct kdtree *kd_create(int k);

/* free the struct kdtree */
void kd_free(struct kdtree *tree);

/* remove all the elements from the tree */
void kd_clear(struct kdtree *tree);

/* if called with non-null 2nd argument, the function provided
 * will be called on data pointers (see kd_insert) when nodes
 * are to be removed from the tree.
 */
void kd_data_destructor(struct kdtree *tree, void (*destr)(void *));

/* insert a node, specifying its position, and optional data */
int kd_insert(struct kdtree *tree, float x, float y, void *data);

/* delete a node at the given position*/
bool kd_delete(struct kdtree *tree, float x, float y);

/* Find the nearest node from a given point.
 *
 * This function returns a pointer to a result set with at most one element.
 */
struct kdres *kd_nearest(struct kdtree *tree, float x, float y);

/* Find the N nearest nodes from a given point.
 *
 * This function returns a pointer to a result set, with at most N elements,
 * which can be manipulated with the kd_res_* functions.
 * The returned pointer can be null as an indication of an error. Otherwise
 * a valid result set is always returned which may contain 0 or more elements.
 * The result set must be deallocated with kd_res_free after use.
 */
struct kdres *kd_nearest_n(struct kdtree *tree, float x, float y, int num);

/* Find any nearest nodes from a given point within a range.
 *
 * This function returns a pointer to a result set, which can be manipulated
 * by the kd_res_* functions.
 * The returned pointer can be null as an indication of an error. Otherwise
 * a valid result set is always returned which may contain 0 or more elements.
 * The result set must be deallocated with kd_res_free after use.
 */
struct kdres *kd_nearest_range(struct kdtree *tree, float x, float y, float range);

/* frees a result set returned by kd_nearest_range() */
void kd_res_free(struct kdres *set);

/* returns the size of the result set (in elements) */
int kd_res_size(struct kdres *set);

/* rewinds the result set iterator */
void kd_res_rewind(struct kdres *set);

/* returns non-zero if the set iterator reached the end after the last element */
int kd_res_end(struct kdres *set);

/* advances the result set iterator, returns non-zero on success, zero if
 * there are no more elements in the result set.
 */
int kd_res_next(struct kdres *set);

/* returns the data pointer (can be null) of the current result set item
 * and optionally sets its position to the pointers(s) if not null.
 */
void *kd_res_item(struct kdres *set, float *x, float *y);

/* equivalent to kd_res_item(set, 0, 0) */
void *kd_res_item_data(struct kdres *set);

/* returns the distance between the requested position and the found point.
 */
float kd_res_dist(struct kdres *set);

// manually added to control the build I want
#define USE_LIST_NODE_ALLOCATOR
#define NO_PTHREADS

#ifdef USE_LIST_NODE_ALLOCATOR

#ifndef NO_PTHREADS
#include <pthread.h>
#else

#endif /* pthread support */
#endif /* use list node allocator */

struct kdhyperrect {
    int dim;
    float *min, *max; /* minimum/maximum coords */
};

struct kdnode {
    float *pos;
    int dir;
    void *data;

    struct kdnode *parent;
    struct kdnode *left, *right; /* negative/positive side */
};

struct res_node {
    struct kdnode *item;
    float dist_sq;
    struct res_node *next;
};

struct kdtree {
    int dim;
    struct kdnode *root;
    struct kdhyperrect *rect;

    struct kdnode **node_stack;
    size_t depth, node_stack_max;
    int node_stack_head;
    void (*destr)(void *);
};

struct kdres {
    struct kdtree *tree;
    struct res_node *rlist, *riter;
    int size;
};

#define SQ(x) ((x) * (x))

static void clear_rec(struct kdnode *node, void (*destr)(void *));
static int insert_rec(struct kdnode **nptr, struct kdnode *parent, const float *pos, void *data, int dir, int dim);
static int rlist_insert(struct res_node *list, struct kdnode *item, float dist_sq);
static struct res_node *rlist_pop_back(struct res_node *list);
static void clear_results(struct kdres *set);

static struct kdhyperrect *hyperrect_create(int dim, const float *min, const float *max);
static void hyperrect_free(struct kdhyperrect *rect);
static struct kdhyperrect *hyperrect_duplicate(const struct kdhyperrect *rect);
static void hyperrect_extend(struct kdhyperrect *rect, const float *pos);
static float hyperrect_dist_sq(struct kdhyperrect *rect, const float *pos);

#ifdef USE_LIST_NODE_ALLOCATOR
static struct res_node *alloc_resnode(void);
static void free_resnode(struct res_node *);
static void free_resnode_buffer();
#else
#define alloc_resnode() malloc(sizeof(struct res_node))
#define free_resnode(n) free(n)
#endif

struct kdtree *kd_create(int k) {
    struct kdtree *tree;

    if (!(tree = (struct kdtree *)malloc(sizeof(struct kdtree)))) {
        return 0;
    }

    tree->dim = k;
    tree->root = NULL;
    tree->destr = NULL;
    tree->rect = NULL;
    tree->depth = 0;
    tree->node_stack = NULL;
    tree->node_stack_max = 0;
    tree->node_stack_head = 0;

    return tree;
}

void kd_free(struct kdtree *tree) {
    if (tree) {
        kd_clear(tree);
        free(tree);
    }

#ifdef USE_LIST_NODE_ALLOCATOR
    free_resnode_buffer();
#endif
}

static void clear_rec(struct kdnode *node, void (*destr)(void *)) {
    if (!node) {
        return;
    }

    clear_rec(node->left, destr);
    clear_rec(node->right, destr);

    if (destr) {
        destr(node->data);
    }
    free(node->pos);
    free(node);
}

void kd_clear(struct kdtree *tree) {
    clear_rec(tree->root, tree->destr);
    tree->root = NULL;

    if (tree->rect) {
        hyperrect_free(tree->rect);
        tree->rect = NULL;
    }
}

void kd_data_destructor(struct kdtree *tree, void (*destr)(void *)) {
    tree->destr = destr;
}

static int insert_rec(struct kdnode **nptr, struct kdnode *parent, const float *pos, void *data, int dir, int k) {
    int new_dir;
    struct kdnode *node;

    if (*nptr == NULL) {
        if (!(node = malloc(sizeof *node))) {
            return -1;
        }
        if (!(node->pos = malloc(k * sizeof(float)))) {
            free(node);
            return -1;
        }
        memcpy(node->pos, pos, k * sizeof(float));
        node->data = data;
        node->dir = dir;
        node->parent = parent;
        node->left = node->right = NULL;
        *nptr = node;
        return 0;
    }

    node = *nptr;
    new_dir = (node->dir + 1) % k;
    if (pos[node->dir] < node->pos[node->dir]) {
        return insert_rec(&(*nptr)->left, *nptr, pos, data, new_dir, k);
    }
    return insert_rec(&(*nptr)->right, *nptr, pos, data, new_dir, k);
}

int _kd_insert(struct kdtree *tree, const float *pos, void *data) {
    if (insert_rec(&tree->root, NULL, pos, data, 0, tree->dim)) {
        return -1;
    }

    if (tree->rect == 0) {
        tree->rect = hyperrect_create(tree->dim, pos, pos);
    } else {
        hyperrect_extend(tree->rect, pos);
    }
    return 0;
}

int kd_insert(struct kdtree *tree, float x, float y, void *data) {
    float buf[2];
    buf[0] = x;
    buf[1] = y;
    return _kd_insert(tree, buf, data);
}

struct kdnode *minNode(struct kdnode *a, struct kdnode *b, struct kdnode *c, int dim) {
    struct kdnode *res = a;
    if (b != NULL && b->pos[dim] < res->pos[dim]) {
        res = b;
    }
    if (c != NULL && c->pos[dim] < res->pos[dim]) {
        res = c;
    }
    return res;
}

struct kdnode *findMinRec(struct kdnode *root, int dim, int k) {
    if (root == NULL) {
        return NULL;
    }

    // Compare point with root with respect to cd (Current dimension)
    if (root->dir == dim) {
        if (root->left == NULL) {
            return root;
        }
        return findMinRec(root->left, dim, k);
    }

    // If current dimension is different then minimum can be anywhere
    // in this subtree
    return minNode(root,
                   findMinRec(root->left, dim, k),
                   findMinRec(root->right, dim, k), dim);
}

struct kdnode *findMinNode(struct kdnode *root, int dim, int k) {
    return findMinRec(root, dim, k);
}

static bool pointsEqual(float *a, float *b, int k) {
    for (int i = 0; i < k; ++i) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

static struct kdnode *delete_node(struct kdnode *root, float *pos, bool *deleted, const int k) {
    if (root == NULL) {
        return NULL;
    }

    if (pointsEqual(root->pos, pos, k)) {
        if (root->right != NULL) {
            struct kdnode *min = findMinNode(root->right, root->dir, k);
            memcpy(root->pos, min->pos, k * sizeof(float));
            root->right = delete_node(root->right, min->pos, deleted, k);
        } else if (root->left != NULL) {
            struct kdnode *min = findMinNode(root->left, root->dir, k);
            memcpy(root->pos, min->pos, k * sizeof(float));
            root->left = delete_node(root->left, min->pos, deleted, k);
        } else {
            // node is leaf node, just delete it
            free(root->pos);
            free(root);
            *deleted = true;
            return NULL;
        }
        return root;
    }

    // if the current node doesn't contain pos, search downwards
    if (pos[root->dir] < root->pos[root->dir]) {
        root->left = delete_node(root->left, pos, deleted, k);
    } else {
        root->right = delete_node(root->right, pos, deleted, k);
    }

    return root;
}

static bool _kd_delete(struct kdtree *tree, float *pos) {
    bool deleted = false;
    delete_node(tree->root, pos, &deleted, tree->dim);
    return deleted;
}

bool kd_delete(struct kdtree *tree, float x, float y) {
    float buf[2];
    buf[0] = x;
    buf[1] = y;
    return _kd_delete(tree, buf);
}

static int find_nearest(struct kdnode *node, const float *pos, float range, struct res_node *list, int ordered, int dim) {
    float dist_sq, dx;
    int i, ret, added_res = 0;

    if (!node) {
        return 0;
    }

    dist_sq = 0;
    for (i = 0; i < dim; i++) {
        dist_sq += SQ(node->pos[i] - pos[i]);
    }
    if (dist_sq <= SQ(range)) {
        if (rlist_insert(list, node, ordered ? dist_sq : -1.0) == -1) {
            return -1;
        }
        added_res = 1;
    }

    dx = pos[node->dir] - node->pos[node->dir];

    ret = find_nearest(dx <= 0.0 ? node->left : node->right, pos, range, list, ordered, dim);
    if (ret >= 0 && fabs(dx) < range) {
        added_res += ret;
        ret = find_nearest(dx <= 0.0 ? node->right : node->left, pos, range, list, ordered, dim);
    }
    if (ret == -1) {
        return -1;
    }
    added_res += ret;

    return added_res;
}

static int find_nearest_n(struct kdnode *node, const float *pos, int num, int *size, float *dist_max, struct res_node *list, int dim) {
    float dist_sq, dx;
    int i, ret;

    if (!node) {
        return 0;
    }

    dist_sq = 0;
    for (i = 0; i < dim; i++) {
        dist_sq += SQ(node->pos[i] - pos[i]);
    }

    if (dist_sq < *dist_max) {
        if (*size < num) {
            ++(*size);
        } else {
            struct res_node *back = rlist_pop_back(list);
            if (back == 0) {
                return -1;
            }
            *dist_max = back->dist_sq > dist_sq ? back->dist_sq : dist_sq;
        }
        if (rlist_insert(list, node, dist_sq) == -1) {
            return -1;
        }
    }

    /* find signed distance from the splitting plane */
    dx = pos[node->dir] - node->pos[node->dir];

    ret = find_nearest_n(dx <= 0.0 ? node->left : node->right, pos, num, size, dist_max, list, dim);
    if (ret >= 0 && SQ(dx) < *dist_max) {
        ret = find_nearest_n(dx <= 0.0 ? node->right : node->left, pos, num, size, dist_max, list, dim);
    }
    return ret;
}

static void kd_nearest_i(struct kdnode *node, const float *pos, struct kdnode **result, float *result_dist_sq, struct kdhyperrect *rect) {
    int dir = node->dir;
    int i;
    float dummy, dist_sq;
    struct kdnode *nearer_subtree, *farther_subtree;
    float *nearer_hyperrect_coord, *farther_hyperrect_coord;

    /* Decide whether to go left or right in the tree */
    dummy = pos[dir] - node->pos[dir];
    if (dummy <= 0) {
        nearer_subtree = node->left;
        farther_subtree = node->right;
        nearer_hyperrect_coord = rect->max + dir;
        farther_hyperrect_coord = rect->min + dir;
    } else {
        nearer_subtree = node->right;
        farther_subtree = node->left;
        nearer_hyperrect_coord = rect->min + dir;
        farther_hyperrect_coord = rect->max + dir;
    }

    if (nearer_subtree) {
        /* Slice the hyperrect to get the hyperrect of the nearer subtree */
        dummy = *nearer_hyperrect_coord;
        *nearer_hyperrect_coord = node->pos[dir];
        /* Recurse down into nearer subtree */
        kd_nearest_i(nearer_subtree, pos, result, result_dist_sq, rect);
        /* Undo the slice */
        *nearer_hyperrect_coord = dummy;
    }

    /* Check the distance of the point at the current node, compare it
     * with our best so far */
    dist_sq = 0;
    for (i = 0; i < rect->dim; i++) {
        dist_sq += SQ(node->pos[i] - pos[i]);
    }
    if (dist_sq < *result_dist_sq) {
        *result = node;
        *result_dist_sq = dist_sq;
    }

    if (farther_subtree) {
        /* Get the hyperrect of the farther subtree */
        dummy = *farther_hyperrect_coord;
        *farther_hyperrect_coord = node->pos[dir];
        /* Check if we have to recurse down by calculating the closest
         * point of the hyperrect and see if it's closer than our
         * minimum distance in result_dist_sq. */
        if (hyperrect_dist_sq(rect, pos) < *result_dist_sq) {
            /* Recurse down into farther subtree */
            kd_nearest_i(farther_subtree, pos, result, result_dist_sq, rect);
        }
        /* Undo the slice on the hyperrect */
        *farther_hyperrect_coord = dummy;
    }
}

struct kdres *_kd_nearest(struct kdtree *kd, const float *pos) {
    struct kdhyperrect *rect;
    struct kdnode *result;
    struct kdres *rset;
    float dist_sq;
    int i;

    if (!kd) {
        return 0;
    }
    if (!kd->rect) {
        return 0;
    }

    /* Allocate result set */
    if (!(rset = (struct kdres *)malloc(sizeof(struct kdres)))) {
        return 0;
    }
    if (!(rset->rlist = alloc_resnode())) {
        free(rset);
        return 0;
    }
    rset->rlist->next = NULL;
    rset->tree = kd;

    /* Duplicate the bounding hyperrectangle, we will work on the copy */
    if (!(rect = hyperrect_duplicate(kd->rect))) {
        kd_res_free(rset);
        return 0;
    }

    /* Our first guesstimate is the root node */
    result = kd->root;
    dist_sq = 0;
    for (i = 0; i < kd->dim; i++) {
        dist_sq += SQ(result->pos[i] - pos[i]);
    }

    /* Search for the nearest neighbour recursively */
    kd_nearest_i(kd->root, pos, &result, &dist_sq, rect);

    /* Free the copy of the hyperrect */
    hyperrect_free(rect);

    /* Store the result */
    if (result) {
        if (rlist_insert(rset->rlist, result, -1.0) == -1) {
            kd_res_free(rset);
            return 0;
        }
        rset->size = 1;
        kd_res_rewind(rset);
        return rset;
    } else {
        kd_res_free(rset);
        return 0;
    }
}

struct kdres *kd_nearest(struct kdtree *tree, float x, float y) {
    float pos[2];
    pos[0] = x;
    pos[1] = y;
    return _kd_nearest(tree, pos);
}

/* ---- nearest N search ---- */

struct kdres *_kd_nearest_n(struct kdtree *kd, const float *pos, int num) {
    int ret, size = 0;
    struct kdres *rset;
    float dist_max = FLT_MAX;

    if (!(rset = (struct kdres *)malloc(sizeof(struct kdres)))) {
        return 0;
    }
    if (!(rset->rlist = alloc_resnode())) {
        free(rset);
        return 0;
    }
    rset->rlist->next = NULL;
    rset->tree = kd;

    ret = find_nearest_n(kd->root, pos, num, &size, &dist_max, rset->rlist, kd->dim);
    if (ret == -1) {
        kd_res_free(rset);
        return 0;
    }
    rset->size = size;
    kd_res_rewind(rset);
    return rset;
}

struct kdres *kd_nearest_n(struct kdtree *tree, float x, float y, int num) {
    float pos[2];
    pos[0] = x;
    pos[1] = y;
    return _kd_nearest_n(tree, pos, num);
}

struct kdres *_kd_nearest_range(struct kdtree *kd, const float *pos, float range) {
    int ret;
    struct kdres *rset;

    if (!(rset = (struct kdres *)malloc(sizeof(struct kdres)))) {
        return 0;
    }
    if (!(rset->rlist = alloc_resnode())) {
        free(rset);
        return 0;
    }
    rset->rlist->next = NULL;
    rset->tree = kd;

    if ((ret = find_nearest(kd->root, pos, range, rset->rlist, 0, kd->dim)) == -1) {
        kd_res_free(rset);
        return 0;
    }
    rset->size = ret;
    kd_res_rewind(rset);
    return rset;
}

struct kdres *kd_nearest_range(struct kdtree *tree, float x, float y, float range) {
    float buf[2];
    buf[0] = x;
    buf[1] = y;
    return _kd_nearest_range(tree, buf, range);
}

void kd_res_free(struct kdres *rset) {
    clear_results(rset);
    free_resnode(rset->rlist);
    free(rset);
}

int kd_res_size(struct kdres *set) {
    return (set->size);
}

void kd_res_rewind(struct kdres *rset) {
    rset->riter = rset->rlist->next;
}

int kd_res_end(struct kdres *rset) {
    return rset->riter == NULL;
}

int kd_res_next(struct kdres *rset) {
    rset->riter = rset->riter->next;
    return rset->riter != NULL;
}

void *kd_res_item(struct kdres *rset, float *x, float *y) {
    if (rset->riter) {
        if (x) {
            *x = rset->riter->item->pos[0];
        }
        if (y) {
            *y = rset->riter->item->pos[1];
        }
        return rset->riter->item->data;
    }
    return 0;
}

void *kd_res_item_data(struct kdres *set) {
    return kd_res_item(set, NULL, NULL);
}

float kd_res_dist(struct kdres *set) {
    return sqrtf(set->riter->dist_sq);
}

/* ---- hyperrectangle helpers ---- */
static struct kdhyperrect *hyperrect_create(int dim, const float *min, const float *max) {
    size_t size = dim * sizeof(float);
    struct kdhyperrect *rect = 0;

    if (!(rect = (struct kdhyperrect *)malloc(sizeof(struct kdhyperrect)))) {
        return 0;
    }

    rect->dim = dim;
    if (!(rect->min = (float *)malloc(size))) {
        free(rect);
        return 0;
    }
    if (!(rect->max = (float *)malloc(size))) {
        free(rect->min);
        free(rect);
        return 0;
    }
    memcpy(rect->min, min, size);
    memcpy(rect->max, max, size);

    return rect;
}

static void hyperrect_free(struct kdhyperrect *rect) {
    free(rect->min);
    free(rect->max);
    free(rect);
}

static struct kdhyperrect *hyperrect_duplicate(const struct kdhyperrect *rect) {
    return hyperrect_create(rect->dim, rect->min, rect->max);
}

static void hyperrect_extend(struct kdhyperrect *rect, const float *pos) {
    int i;

    for (i = 0; i < rect->dim; i++) {
        if (pos[i] < rect->min[i]) {
            rect->min[i] = pos[i];
        }
        if (pos[i] > rect->max[i]) {
            rect->max[i] = pos[i];
        }
    }
}

static float hyperrect_dist_sq(struct kdhyperrect *rect, const float *pos) {
    int i;
    float result = 0;

    for (i = 0; i < rect->dim; i++) {
        if (pos[i] < rect->min[i]) {
            result += SQ(rect->min[i] - pos[i]);
        } else if (pos[i] > rect->max[i]) {
            result += SQ(rect->max[i] - pos[i]);
        }
    }

    return result;
}

/* ---- static helpers ---- */

#ifdef USE_LIST_NODE_ALLOCATOR
/* special list node allocators. */
static struct res_node *free_nodes;

#ifndef NO_PTHREADS
static pthread_mutex_t alloc_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

static struct res_node *alloc_resnode(void) {
    struct res_node *node;

#ifndef NO_PTHREADS
    pthread_mutex_lock(&alloc_mutex);
#endif

    if (!free_nodes) {
        node = (struct res_node *)malloc(sizeof(struct res_node));
    } else {
        node = free_nodes;
        free_nodes = free_nodes->next;
        node->next = NULL;
    }

#ifndef NO_PTHREADS
    pthread_mutex_unlock(&alloc_mutex);
#endif

    return node;
}

static void free_resnode(struct res_node *node) {
#ifndef NO_PTHREADS
    pthread_mutex_lock(&alloc_mutex);
#endif

    node->next = free_nodes;
    free_nodes = node;

#ifndef NO_PTHREADS
    pthread_mutex_unlock(&alloc_mutex);
#endif
}

static void free_resnode_buffer() {
#ifndef NO_PTHREADS
    pthread_mutex_lock(&alloc_mutex);
#endif

    if (free_nodes) {
        struct res_node *ptr = free_nodes;
        while (ptr) {
            ptr = ptr->next;
            free(free_nodes);
            free_nodes = ptr;
        }
        free_nodes = 0;
    }

#ifndef NO_PTHREADS
    pthread_mutex_unlock(&alloc_mutex);
#endif
}

#endif /* list node allocator or not */

/* inserts the item. if dist_sq is >= 0, then do an ordered insert */
/* TODO make the ordering code use heapsort */
static int rlist_insert(struct res_node *list, struct kdnode *item, float dist_sq) {
    struct res_node *rnode;

    if (!(rnode = alloc_resnode())) {
        return -1;
    }
    rnode->item = item;
    rnode->dist_sq = dist_sq;

    if (dist_sq >= 0.0) {
        while (list->next && list->next->dist_sq < dist_sq) {
            list = list->next;
        }
    }
    rnode->next = list->next;
    list->next = rnode;
    return 0;
}

static struct res_node *rlist_pop_back(struct res_node *list) {
    struct res_node *previous = NULL;
    while (list->next) {
        previous = list;
        list = list->next;
    }
    if (previous) {
        previous->next = NULL;
    }
    free_resnode(list);
    return previous;
}

static void clear_results(struct kdres *rset) {
    struct res_node *tmp, *node = rset->rlist->next;

    while (node) {
        tmp = node;
        node = node->next;
        free_resnode(tmp);
    }

    rset->rlist->next = NULL;
}

#ifdef __cplusplus
}
#endif

#endif /* _KDTREE_H_ */
