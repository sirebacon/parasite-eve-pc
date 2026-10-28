#ifndef PE_SAVE_TAIL_H
#define PE_SAVE_TAIL_H

/* The save's free tail carried forward (a host feature, installed always).
 *
 * Mods keep per-save data in the card block's free tail, 0x1C90..0x1FFF. The game builds every save
 * into a cleared buffer (Save_BuildCardFile, bzero), so a block whose mod isn't running at that save
 * -- turned off in Manage mods, failed to start, not in this build, or All mods off -- would be lost
 * for good. So the host keeps the tail of the save that was loaded (PE_HOOK_SAVE_READ, before any
 * mod) and puts it back into the buffer before the mods write (PE_HOOK_SAVE_WRITE, before any mod):
 * running mods overwrite their own blocks, every other block carries forward unchanged. New Game
 * clears the copy, so blocks never leak from one playthrough into another.
 *
 * The rule for mods: always write (or zero) your own block when saving, never skip it, so a stale copy
 * can't outlive a real change. Mods using the host's save chunks (pe_save.h) get this for free. */

#include <stddef.h>
#include <stdint.h>

#define PE_SAVE_TAIL_OFFSET 0x1C90
#define PE_SAVE_TAIL_SIZE   (0x2000 - PE_SAVE_TAIL_OFFSET)

void pe_save_tail_install(void);
/* The carried tail (PE_SAVE_TAIL_SIZE bytes), or NULL when there is none (new game, no save loaded). */
const uint8_t *pe_save_tail_carried(void);
/* For the host's save chunks and tests: what a save just loaded / New Game would do. */
void pe_save_tail_loaded(const uint8_t *block);
void pe_save_tail_new_game(void);
void pe_save_tail_prepare(uint8_t *block);

#endif
