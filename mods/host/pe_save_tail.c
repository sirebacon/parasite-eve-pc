/* The save's free tail carried forward. See pe_save_tail.h. */
#include <string.h>

#include "pe_mod_host.h"
#include "pe_save_tail.h"

#define PE_INV_CMD_START_NEW_GAME 1117
#define PE_PRIORITY_HOST_FIRST    (-1000)   /* before every mod, PE_PRIORITY_EARLY included */

static uint8_t s_tail[PE_SAVE_TAIL_SIZE];
static int s_have;

const uint8_t *pe_save_tail_carried(void) { return s_have ? s_tail : NULL; }

void pe_save_tail_loaded(const uint8_t *block) {
    memcpy(s_tail, block + PE_SAVE_TAIL_OFFSET, sizeof s_tail);
    s_have = 1;
}

void pe_save_tail_new_game(void) {
    memset(s_tail, 0, sizeof s_tail);
    s_have = 0;
}

void pe_save_tail_prepare(uint8_t *block) {
    if (s_have) memcpy(block + PE_SAVE_TAIL_OFFSET, s_tail, sizeof s_tail);
}

static int pe_on_read(void *ctx, void *user) {
    (void)user;
    if (ctx) pe_save_tail_loaded((const uint8_t *)ctx);
    return 0;
}

static int pe_on_write(void *ctx, void *user) {
    (void)user;
    if (ctx) pe_save_tail_prepare((uint8_t *)ctx);
    return 0;
}

static int pe_on_command(void *ctx, void *user) {
    (void)user;
    if (ctx && ((const PeHookInvCommand *)ctx)->command == PE_INV_CMD_START_NEW_GAME) pe_save_tail_new_game();
    return 0;
}

/* After the mods' own writes: remember what this save holds, so saving twice keeps the latest. */
static int pe_on_written(void *ctx, void *user) {
    (void)user;
    if (ctx) pe_save_tail_loaded((const uint8_t *)ctx);
    return 0;
}

void pe_save_tail_install(void) {
    pe_hook_add(PE_HOOK_SAVE_READ, pe_on_read, NULL, PE_PRIORITY_HOST_FIRST, "host");
    pe_hook_add(PE_HOOK_SAVE_WRITE, pe_on_write, NULL, PE_PRIORITY_HOST_FIRST, "host");
    pe_hook_add(PE_HOOK_SAVE_WRITE, pe_on_written, NULL, 1000, "host");
    pe_hook_add(PE_HOOK_INV_COMMAND, pe_on_command, NULL, PE_PRIORITY_HOST_FIRST, "host");
}
