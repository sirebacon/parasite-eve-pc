#ifndef PE_INVENTORY_FEATURES_H
#define PE_INVENTORY_FEATURES_H
#include <stdint.h>
#include <string.h>
#include "pe_game.h"

/* Feature-private helpers. The PKEY and SORT blocks keep the recomp's exact disk
 * layout in the host's save window (host/pe_save.h), for builds without the host's
 * records; native pointers never enter a memory-card save. */
static inline int pe_inventory_limit(void) {
    int n = Inv_GetAyaSlotLimit();
    return n < 0 ? 0 : n > 50 ? 50 : n;
}
static inline uint32_t pe_inventory_checksum(const void *data, unsigned n, uint32_t seed) {
    const uint8_t *p = data;
    for (unsigned i = 0; i < n; ++i) seed = (seed << 5) + seed + p[i];
    return seed;
}
static inline uint32_t pe_inventory_word(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline void pe_inventory_put(void *p, uint32_t v) { memcpy(p, &v, 4); }
static inline void pe_inventory_save(void *card, unsigned at, const void *data, unsigned n,
                                     uint32_t magic, uint32_t seed) {
    uint8_t *p = (uint8_t *)card + at;
    pe_inventory_put(p, magic); pe_inventory_put(p + 4, 1); pe_inventory_put(p + 8, n);
    pe_inventory_put(p + 12, pe_inventory_checksum(data, n, seed)); memcpy(p + 16, data, n);
}
static inline int pe_inventory_load(const void *card, unsigned at, void *data, unsigned n,
                                    uint32_t magic, uint32_t seed) {
    const uint8_t *p = (const uint8_t *)card + at;
    if (pe_inventory_word(p) != magic || pe_inventory_word(p + 4) != 1 ||
        pe_inventory_word(p + 8) != n || pe_inventory_word(p + 12) != pe_inventory_checksum(p + 16, n, seed)) return 0;
    memcpy(data, p + 16, n); return 1;
}
void pe_key_items_register(const char *mod_id);
int pe_keys_enabled(void);
int pe_keys_count(void);
uint16_t pe_keys_item(int i);   /* 0 past the end */
void pe_key_menu_register(const char *mod_id);
void pe_bag_sort_register(const char *mod_id);
int pe_bag_sort_auto(void);     /* new items are sorted the remembered way */
void pe_inventory_menu_register(const char *mod_id);
void pe_inventory_menu_shutdown(void);
#endif
