#pragma once

/*
 * File-Descriptor List
 */

#include <c-macro.h>
#include <stdlib.h>
#include <sys/socket.h>

typedef struct FDList FDList;

struct FDList {
        bool consumed : 1;
        struct cmsghdr cmsg[];
};

int fdlist_new_with_fds(FDList **listp, const int *fds, size_t n_fds);
int fdlist_new_consume_fds(FDList **listp, const int *fds, size_t n_fds);
FDList *fdlist_free(FDList *list);
void fdlist_truncate(FDList *list, size_t n_fds);

static inline int *fdlist_data(FDList *list) {
        return (int *)CMSG_DATA(list->cmsg);
}

static inline size_t fdlist_count(FDList *list) {
        return (list->cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
}

static inline int fdlist_get(FDList *list, size_t index) {
        assert(index < fdlist_count(list));
        return fdlist_data(list)[index];
}

C_DEFINE_CLEANUP(FDList *, fdlist_free);
