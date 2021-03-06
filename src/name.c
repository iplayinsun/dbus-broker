/*
 * Name Registry
 */

#include <c-macro.h>
#include <c-rbtree.h>
#include <c-ref.h>
#include <c-list.h>
#include <stdlib.h>
#include "dbus-protocol.h"
#include "driver.h"
#include "name.h"
#include "user.h"

void name_change_init(NameChange *change) {
        *change = (NameChange){};
}

void name_change_deinit(NameChange *change) {
        change->name = name_entry_unref(change->name);
        change->old_owner = NULL;
        change->new_owner = NULL;
}

/* new owner object linked into the owning peer */
static int name_owner_new(NameOwner **ownerp, Peer *peer, NameEntry *entry, CRBNode *parent, CRBNode **slot) {
        NameOwner *owner;

        if (peer->user->n_names < 1)
                return -EDQUOT;

        owner = calloc(1, sizeof(*owner));
        if (!owner)
                return -ENOMEM;

        peer->user->n_names --;

        owner->entry_link = (CList)C_LIST_INIT(owner->entry_link);
        owner->entry = name_entry_ref(entry);
        owner->peer = peer;
        c_rbtree_add(&peer->names, parent, slot, &owner->rb);

        *ownerp = owner;
        return 0;
}

/* unlink from peer and entry */
NameOwner *name_owner_free(NameOwner *owner) {
        if (!owner)
                return NULL;

        c_rbtree_remove(&owner->peer->names, &owner->rb);
        c_list_unlink(&owner->entry_link);
        name_entry_unref(owner->entry);

        owner->peer->user->n_names ++;

        free(owner);

        return NULL;
}

static int name_owner_compare(CRBTree *tree, void *k, CRBNode *rb) {
        NameOwner *owner = c_container_of(rb, NameOwner, rb);
        NameEntry *entry = k;

        if (owner->entry < entry)
                return -1;
        if (owner->entry > entry)
                return 1;

        return 0;
}

static int name_owner_get(NameOwner **ownerp, Peer *peer, NameEntry *entry) {
        CRBNode **slot, *parent;
        int r;

        slot = c_rbtree_find_slot(&peer->names, name_owner_compare, entry, &parent);
        if (!slot) {
                *ownerp = c_container_of(parent, NameOwner, rb);
        } else {
                r = name_owner_new(ownerp, peer, entry, parent, slot);
                if (r < 0)
                        return r;
        }

        return 0;
}

bool name_owner_is_primary(NameOwner *owner) {
        return (c_list_first(&owner->entry->owners) == &owner->entry_link);
}

static void name_owner_update(NameOwner *owner, uint32_t flags, NameChange *change, uint32_t *replyp) {
        NameEntry *entry = owner->entry;
        NameOwner *head;

        assert(!change->name);
        assert(!change->old_owner);
        assert(!change->new_owner);

        head = c_container_of(c_list_first(&entry->owners), NameOwner, entry_link);
        if (!head) {
                /* there is no primary owner */
                change->name = name_entry_ref(entry);
                change->new_owner = owner->peer;
                change->old_owner = NULL;

                /* @owner cannot already be linked */
                c_list_link_front(&entry->owners, &owner->entry_link);
                owner->flags = flags;
                *replyp = DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER;
        } else if (head == owner) {
                /* we are already the primary owner */
                owner->flags = flags;
                *replyp = DBUS_REQUEST_NAME_REPLY_ALREADY_OWNER;
        } else if ((flags & DBUS_NAME_FLAG_REPLACE_EXISTING) &&
                   (head->flags & DBUS_NAME_FLAG_ALLOW_REPLACEMENT)) {
                /* we replace the primary owner */
                change->name = name_entry_ref(entry);
                change->old_owner = head->peer;
                change->new_owner = owner->peer;

                if (head->flags & DBUS_NAME_FLAG_DO_NOT_QUEUE)
                        /* the previous primary owner is dropped */
                        name_owner_free(head);

                c_list_unlink(&owner->entry_link);
                c_list_link_front(&entry->owners, &owner->entry_link);
                owner->flags = flags;
                *replyp = DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER;
        } else if (!(flags & DBUS_NAME_FLAG_DO_NOT_QUEUE)) {
                /* we are appended to the queue */
                if (!c_list_is_linked(&owner->entry_link)) {
                        c_list_link_tail(&entry->owners, &owner->entry_link);
                }
                owner->flags = flags;
                *replyp = DBUS_REQUEST_NAME_REPLY_IN_QUEUE;
        } else {
                /* we are dropped */
                name_owner_free(owner);
                *replyp = DBUS_REQUEST_NAME_REPLY_EXISTS;
        }
}

static void name_owner_release(NameOwner *owner, NameChange *change) {
        assert(!change->name);
        assert(!change->old_owner);
        assert(!change->new_owner);

        if (name_owner_is_primary(owner)) {
                Peer *new_owner;

                if (c_list_last(&owner->entry->owners) != &owner->entry_link) {
                        NameOwner *next = c_list_entry(owner->entry_link.next, NameOwner, entry_link);
                        new_owner = next->peer;
                } else {
                        new_owner = NULL;
                }

                change->name = name_entry_ref(owner->entry);
                change->old_owner = owner->peer;
                change->new_owner = new_owner;
        }

        name_owner_free(owner);
}

/* new name entry linked into the registry */
static int name_entry_new(NameEntry **entryp, NameRegistry *registry, const char *name, CRBNode *parent, CRBNode **slot) {
        NameEntry *entry;
        size_t n_name;

        n_name = strlen(name) + 1;

        entry = malloc(sizeof(*entry) + n_name);
        if (!entry)
                return -ENOMEM;

        entry->n_refs = C_REF_INIT;
        entry->registry = registry;
        match_registry_init(&entry->matches);
        c_rbtree_add(&registry->entries, parent, slot, &entry->rb);
        entry->owners = (CList)C_LIST_INIT(entry->owners);
        memcpy((char*)entry->name, name, n_name);

        *entryp = entry;
        return 0;
}

void name_entry_free(_Atomic unsigned long *n_refs, void *userpointer) {
        NameEntry *entry = c_container_of(n_refs, NameEntry, n_refs);

        assert(c_list_is_empty(&entry->owners));

        c_rbtree_remove(&entry->registry->entries, &entry->rb);

        match_registry_deinit(&entry->matches);

        free(entry);
}

static int name_entry_compare(CRBTree *tree, void *k, CRBNode *rb) {
        NameEntry *entry = c_container_of(rb, NameEntry, rb);
        char *name = k;

        return strcmp(entry->name, name);
}

int name_entry_get(NameEntry **entryp, NameRegistry *registry, const char *name) {
        CRBNode **slot, *parent;
        int r;

        slot = c_rbtree_find_slot(&registry->entries, name_entry_compare, name, &parent);
        if (!slot) {
                *entryp = name_entry_ref(c_container_of(parent, NameEntry, rb));
        } else {
                r = name_entry_new(entryp, registry, name, parent, slot);
                if (r < 0)
                        return r;
        }

        return 0;
}

void name_registry_init(NameRegistry *registry) {
        *registry = (NameRegistry) {};
}

void name_registry_deinit(NameRegistry *registry) {
        assert(!registry->entries.root);
}

int name_registry_request_name(NameRegistry *registry, Peer *peer, const char *name, uint32_t flags, NameChange *change, uint32_t *replyp) {
        _c_cleanup_(name_entry_unrefp) NameEntry *entry = NULL;
        NameOwner *owner;
        int r;

        r = name_entry_get(&entry, registry, name);
        if (r < 0)
                return r;

        r = name_owner_get(&owner, peer, entry);
        if (r < 0)
                return r;

        name_owner_update(owner, flags, change, replyp);

        return 0;
}

NameEntry *name_registry_find_entry(NameRegistry *registry, const char *name) {
        return c_rbtree_find_entry(&registry->entries, name_entry_compare, name, NameEntry, rb);
}

void name_registry_release_name(NameRegistry *registry, Peer *peer, const char *name, NameChange *change, uint32_t *replyp) {
        NameEntry *entry;
        NameOwner *owner;

        entry = name_registry_find_entry(registry, name);
        if (!entry) {
                *replyp = DBUS_RELEASE_NAME_REPLY_NON_EXISTENT;
                return;
        }

        owner = c_rbtree_find_entry(&peer->names, name_owner_compare, entry, NameOwner, rb);
        if (!owner) {
                *replyp = DBUS_RELEASE_NAME_REPLY_NOT_OWNER;
                return;
        }

        name_owner_release(owner, change);

        *replyp = DBUS_RELEASE_NAME_REPLY_RELEASED;
}

Peer *name_registry_resolve_name(NameRegistry *registry, const char *name) {
        NameEntry *entry;
        NameOwner *owner;

        entry = c_rbtree_find_entry(&registry->entries, name_entry_compare, name, NameEntry, rb);
        if (!entry)
                return NULL;

        owner = c_list_first_entry(&entry->owners, NameOwner, entry_link);

        return owner->peer;
}
