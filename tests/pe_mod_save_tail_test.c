/* The save's free tail carried forward (gap 8): a mod that isn't running when the game saves keeps its
 * block; running mods overwrite only their own; New Game clears the copy; saving twice keeps the
 * latest; and the host's handlers run before every mod's (and its record after). The game's own save
 * path is modelled as it is: a cleared 8 KB buffer, then PE_HOOK_SAVE_WRITE. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "pe_mod_host.h"
#include "pe_save_tail.h"

typedef struct { int id, priority; PeHookFn fn; } Handler;
static Handler s_h[16];
static int s_n;

int pe_hook_add(int id, PeHookFn fn, void *user, int priority, const char *mod_id) {
    (void)user; (void)mod_id;
    s_h[s_n++] = (Handler){id, priority, fn};
    return 0;
}

/* Two stand-in mods: "keys" at +0x200 and "junk" at +0x000, each always writing its own block. */
static int s_junk_on = 1, s_keys_on = 1;
static unsigned char s_junk = 0x11, s_keys = 0x22;
static int junk_write(void *ctx, void *u) { (void)u; if (s_junk_on) memset((unsigned char *)ctx + 0x1C90, s_junk, 16); return 0; }
static int keys_write(void *ctx, void *u) { (void)u; if (s_keys_on) memset((unsigned char *)ctx + 0x1C90 + 0x200, s_keys, 16); return 0; }

/* Dispatch in priority order, as the host does (mods at PE_PRIORITY_NORMAL). */
static void dispatch(int id, void *ctx) {
    int p, i;
    for (p = -2000; p <= 2000; ++p) {
        for (i = 0; i < s_n; ++i) if (s_h[i].id == id && s_h[i].priority == p) s_h[i].fn(ctx, NULL);
        if (p == PE_PRIORITY_NORMAL && id == PE_HOOK_SAVE_WRITE) { junk_write(ctx, NULL); keys_write(ctx, NULL); }
    }
}

static unsigned char s_card[0x2000];
static void game_save(void) { memset(s_card, 0, sizeof s_card); dispatch(PE_HOOK_SAVE_WRITE, s_card); }   /* Save_BuildCardFile */
static int block_is(int off, unsigned char v) {
    int i;
    for (i = 0; i < 16; ++i) if (s_card[0x1C90 + off + i] != v) return 0;
    return 1;
}

int main(void) {
    unsigned char loaded[0x2000];
    PeHookInvCommand new_game = {1117, 0, 0};
    int i;
    pe_save_tail_install();
    for (i = 0; i < s_n; ++i)                                  /* host first on read/write/command */
        if (s_h[i].id != PE_HOOK_SAVE_WRITE || s_h[i].priority < 0) assert(s_h[i].priority < PE_PRIORITY_EARLY);

    game_save();                                               /* no save loaded: both write, nothing carried */
    assert(block_is(0x000, 0x11) && block_is(0x200, 0x22) && pe_save_tail_carried());

    memset(loaded, 0, sizeof loaded);                          /* load a save holding both blocks */
    memset(loaded + 0x1C90, 0xAA, 16);
    memset(loaded + 0x1C90 + 0x200, 0xBB, 16);
    memset(loaded + 0x1C90 + 0x300, 0xCC, 16);                 /* and a block of a mod not in this build */
    dispatch(PE_HOOK_SAVE_READ, loaded);

    s_junk_on = 0;                                             /* the economy is off for this save */
    game_save();
    assert(block_is(0x000, 0xAA));                             /* its block carried forward, not lost */
    assert(block_is(0x200, 0x22));                             /* the running mod wrote its own */
    assert(block_is(0x300, 0xCC));                             /* the unknown mod's block survives too */

    s_keys = 0x33;                                             /* saving again keeps the latest */
    game_save();
    assert(block_is(0x000, 0xAA) && block_is(0x200, 0x33) && block_is(0x300, 0xCC));

    s_junk_on = 1;                                             /* turned back on: it writes its own again */
    game_save();
    assert(block_is(0x000, 0x11));

    dispatch(PE_HOOK_INV_COMMAND, &new_game);                  /* New Game: nothing carries into it */
    assert(!pe_save_tail_carried());
    s_junk_on = 0;
    game_save();
    assert(block_is(0x000, 0x00) && block_is(0x300, 0x00) && block_is(0x200, 0x33));
    puts("Save tail checks passed: mods off keep their blocks, unknown blocks survive, latest kept, New Game clears.");
    return 0;
}
