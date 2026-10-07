/* interface_registry.c — portable host implementation of the SDK plugin-to-plugin interface
 * registry (roth_interfaces_v1): a small fixed table of (name, owner, version, pointer).
 *
 * The host stores names and pointers and never follows a pointer. Ownership is the publishing
 * plugin's own api table address, the same identity the present channel uses.
 *
 * Writers (publish / withdraw / release) run on MAIN or GAME context, which are one thread at a
 * time by the loader's lifecycle. The timer tick can interrupt that thread, so every update is
 * ordered to leave the table readable at any instant: a new entry is filled completely before the
 * count that exposes it advances, and an entry's pointer is the last field stored when it becomes
 * live and the first cleared when it stops being live. A lookup that lands in the middle of an
 * update therefore sees either the old state or the new one.
 */
#include "interface_registry.h"
#include "roth_sdk.h"

#include <stdint.h>
#include <string.h>

#define IFACE_MAX      64
#define IFACE_NAME_MAX 64   /* including the terminator */

struct iface_entry {
    char name[IFACE_NAME_MAX];   /* written once, when the entry is first claimed */
    uintptr_t owner;             /* publishing plugin's api table address; 0 = withdrawn */
    uint32_t version;
    const void *iface;           /* NULL = nothing published under this name right now */
};

static struct iface_entry g_iface[IFACE_MAX];
static unsigned g_iface_count;

/* 1..63 characters with a '.' somewhere, like a plugin id. */
static int name_valid(const char *name)
{
    size_t n;
    if (!name)
        return 0;
    n = strnlen(name, IFACE_NAME_MAX);
    return n > 0 && n < IFACE_NAME_MAX && memchr(name, '.', n) != NULL;
}

static struct iface_entry *find(const char *name)
{
    unsigned count = __atomic_load_n(&g_iface_count, __ATOMIC_ACQUIRE);
    for (unsigned i = 0; i < count; i++)
        if (strcmp(g_iface[i].name, name) == 0)
            return &g_iface[i];
    return NULL;
}

int32_t roth_interfaces_publish(const struct roth_api_v1 *api, const char *name,
                                uint32_t version, const void *iface)
{
    struct iface_entry *e;
    uintptr_t owner;

    if (!api || !iface || !name_valid(name))
        return ROTH_ERR_INVALID;
    e = find(name);
    if (e) {
        owner = __atomic_load_n(&e->owner, __ATOMIC_ACQUIRE);
        if (owner != 0 && owner != (uintptr_t)api)
            return ROTH_ERR_NOT_OWNER;
        /* Take the pointer down while the version under it changes. */
        __atomic_store_n(&e->iface, (const void *)0, __ATOMIC_RELEASE);
        __atomic_store_n(&e->owner, (uintptr_t)api, __ATOMIC_RELEASE);
        __atomic_store_n(&e->version, version, __ATOMIC_RELEASE);
        __atomic_store_n(&e->iface, iface, __ATOMIC_RELEASE);
        return ROTH_OK;
    }
    if (g_iface_count >= IFACE_MAX)
        return ROTH_ERR_NOMEM;
    e = &g_iface[g_iface_count];
    memset(e, 0, sizeof *e);
    strcpy(e->name, name);   /* name_valid bounded it */
    e->owner = (uintptr_t)api;
    e->version = version;
    e->iface = iface;
    __atomic_store_n(&g_iface_count, g_iface_count + 1u, __ATOMIC_RELEASE);
    return ROTH_OK;
}

const void *roth_interfaces_lookup(const struct roth_api_v1 *api, const char *name,
                                   uint32_t *version)
{
    struct iface_entry *e;
    const void *iface;

    (void)api;
    if (!name)
        return NULL;
    e = find(name);
    if (!e)
        return NULL;
    iface = __atomic_load_n(&e->iface, __ATOMIC_ACQUIRE);
    if (iface && version)
        *version = __atomic_load_n(&e->version, __ATOMIC_ACQUIRE);
    return iface;
}

int32_t roth_interfaces_withdraw(const struct roth_api_v1 *api, const char *name)
{
    struct iface_entry *e;
    uintptr_t owner;

    if (!api || !name)
        return ROTH_ERR_INVALID;
    e = find(name);
    if (!e)
        return ROTH_ERR_INVALID;
    owner = __atomic_load_n(&e->owner, __ATOMIC_ACQUIRE);
    if (owner == 0)
        return ROTH_ERR_INVALID;
    if (owner != (uintptr_t)api)
        return ROTH_ERR_NOT_OWNER;
    __atomic_store_n(&e->iface, (const void *)0, __ATOMIC_RELEASE);
    __atomic_store_n(&e->owner, (uintptr_t)0, __ATOMIC_RELEASE);
    return ROTH_OK;
}

void roth_interfaces_release_owner(const struct roth_api_v1 *api)
{
    if (!api)
        return;
    for (unsigned i = 0; i < g_iface_count; i++) {
        if (__atomic_load_n(&g_iface[i].owner, __ATOMIC_ACQUIRE) != (uintptr_t)api)
            continue;
        __atomic_store_n(&g_iface[i].iface, (const void *)0, __ATOMIC_RELEASE);
        __atomic_store_n(&g_iface[i].owner, (uintptr_t)0, __ATOMIC_RELEASE);
    }
}
