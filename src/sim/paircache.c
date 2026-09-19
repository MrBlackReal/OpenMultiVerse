/*
 * paircache.c — sparse symmetric pair -> cooldown map (see paircache.h).
 *
 * Open addressing with linear probing. Deletion is the awkward part of linear
 * probing: removing an entry can break the probe chain of a later one. Rather
 * than carry tombstones (which accumulate and force periodic rehashing anyway),
 * every removal path here rebuilds the table from its live entries. Removal
 * happens on sweep and on body recycling, both rare relative to lookups, and
 * the table is small enough that a rebuild is a few microseconds.
 */
#include "paircache.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint64_t key;      /* packed (lo,hi) + 1; 0 = empty slot  */
    double   expiry;   /* absolute time the cooldown ends      */
} PairEntry;

#define PC_MIN_CAP 64          /* power of two */
#define PC_MAX_CAP (1 << 24)   /* sanity bound; also lets the compiler size allocs */
#define PC_MAX_LOAD_NUM 7      /* grow above 7/10 load */
#define PC_MAX_LOAD_DEN 10

static PairEntry *s_tab = NULL;
static int s_cap  = 0;         /* always a power of two, or 0 */
static int s_used = 0;         /* occupied slots */

/* Pack an unordered pair into a non-zero key. Negative indices are rejected by
 * the callers below, so 32 bits each is ample and 0 stays free as "empty". */
static inline uint64_t pack(int a, int b)
{
    uint32_t lo = (uint32_t)(a < b ? a : b);
    uint32_t hi = (uint32_t)(a < b ? b : a);
    return (((uint64_t)lo << 32) | (uint64_t)hi) + 1u;
}

/* splitmix64 finalizer — cheap, and mixes the low bits that linear probing on a
 * power-of-two mask is sensitive to. Body indices are small and dense, so a
 * weak hash would cluster badly here. */
static inline uint64_t mix(uint64_t x)
{
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27; x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

/* Slot holding `key`, or the first empty slot if absent. Never returns -1: the
 * load factor keeps at least one slot free, so the probe always terminates. */
static int slot_of(uint64_t key)
{
    int mask = s_cap - 1;
    int i = (int)(mix(key) & (uint64_t)mask);
    for (;;) {
        if (s_tab[i].key == 0 || s_tab[i].key == key) return i;
        i = (i + 1) & mask;
    }
}

/* Reinsert `n` entries into a freshly sized table. */
static void rebuild(int new_cap, const PairEntry *src, int n)
{
    PairEntry *old = s_tab;
    s_tab = (PairEntry *)calloc((size_t)new_cap, sizeof(PairEntry));
    if (!s_tab) { s_tab = old; return; }   /* keep the old table on OOM */
    s_cap  = new_cap;
    s_used = 0;
    for (int i = 0; i < n; i++) {
        if (src[i].key == 0) continue;
        int j = slot_of(src[i].key);
        s_tab[j] = src[i];
        s_used++;
    }
    if (old != s_tab) free(old);
}

static int ensure_table(void)
{
    if (s_cap == 0) {
        s_tab = (PairEntry *)calloc(PC_MIN_CAP, sizeof(PairEntry));
        if (!s_tab) return 0;
        s_cap = PC_MIN_CAP;
        s_used = 0;
    }
    return 1;
}

void paircache_reset(void)
{
    free(s_tab);
    s_tab = NULL;
    s_cap = 0;
    s_used = 0;
}

double paircache_remaining(int a, int b, double now)
{
    if (a < 0 || b < 0 || a == b || s_cap == 0) return 0.0;
    int i = slot_of(pack(a, b));
    if (s_tab[i].key == 0) return 0.0;
    double left = s_tab[i].expiry - now;
    return left > 0.0 ? left : 0.0;
}

void paircache_arm(int a, int b, double now, double cooldown)
{
    if (a < 0 || b < 0 || a == b) return;
    if (!ensure_table()) return;

    uint64_t key = pack(a, b);
    int i = slot_of(key);
    if (s_tab[i].key == 0) {
        /* Growing before the insert keeps a free slot available for probing.
         * Copy the table first: rebuild() reinserts from the snapshot. */
        if (s_cap < PC_MAX_CAP &&
            (s_used + 1) * PC_MAX_LOAD_DEN >= s_cap * PC_MAX_LOAD_NUM) {
            PairEntry *snap = (PairEntry *)malloc((size_t)s_cap * sizeof(PairEntry));
            if (snap) {
                memcpy(snap, s_tab, (size_t)s_cap * sizeof(PairEntry));
                int n = s_cap;
                rebuild(s_cap * 2, snap, n);
                free(snap);
                i = slot_of(key);
            }
        }
        s_tab[i].key = key;
        s_used++;
    }
    s_tab[i].expiry = now + cooldown;
}

void paircache_sweep(double now)
{
    if (s_cap <= 0 || s_cap > PC_MAX_CAP || s_used == 0) return;

    int live = 0;
    for (int i = 0; i < s_cap; i++)
        if (s_tab[i].key != 0 && s_tab[i].expiry > now) live++;
    if (live == s_used) return;                 /* nothing expired */

    PairEntry *snap = (PairEntry *)malloc((size_t)s_cap * sizeof(PairEntry));
    if (!snap) return;                          /* stale entries are harmless */
    int n = 0;
    for (int i = 0; i < s_cap; i++)
        if (s_tab[i].key != 0 && s_tab[i].expiry > now) snap[n++] = s_tab[i];

    /* Shrink once the table is mostly empty, but never below the floor. */
    int cap = s_cap;
    while (cap > PC_MIN_CAP && n * 4 < cap) cap /= 2;
    rebuild(cap, snap, n);
    free(snap);
}

void paircache_forget_body(int idx)
{
    if (s_cap <= 0 || s_cap > PC_MAX_CAP || s_used == 0 || idx < 0) return;

    uint64_t want = (uint64_t)(uint32_t)idx;
    PairEntry *snap = (PairEntry *)malloc((size_t)s_cap * sizeof(PairEntry));
    if (!snap) return;
    int n = 0;
    for (int i = 0; i < s_cap; i++) {
        if (s_tab[i].key == 0) continue;
        uint64_t k = s_tab[i].key - 1u;
        if ((k >> 32) == want || (k & 0xffffffffULL) == want) continue;
        snap[n++] = s_tab[i];
    }
    if (n != s_used) rebuild(s_cap, snap, n);
    free(snap);
}

int paircache_live(void)     { return s_used; }
int paircache_capacity(void) { return s_cap; }
