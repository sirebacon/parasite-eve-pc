/* Bug recorder: a black box for bugs nobody can reproduce.
 *
 * While on, it keeps the last minutes of play in memory and, when F8 is pressed (or F1 > Bug
 * recorder > Save a bug report now), writes them to a bug report folder beside the preferences:
 *   bug-reports/<date-time>/recording.jsonl   one JSON object per line, oldest first:
 *       state     once a second: room, scene, story day, battle / field, Aya's HP / PE / level /
 *                 position, the enemy slots, the frame counter
 *       keyframe  every minute (and at the start): the whole of each state block below, as hex
 *       diff      once a second, and at every room change, battle start / end and save: the bytes
 *                 of each block that changed since the last one (rng only in keyframes: it changes
 *                 every frame)
 *       pad       the controller packet (buttons and both sticks) whenever it changes
 *       events    scene, newgame, battle_start / battle_end, hit, hit_end, hp_loss, spell, rewards,
 *                 invcmd, item_add / item_remove / item_count / item_use, steal, tool_transfer,
 *                 storage_transfer, menu_action, script_random, text_page, equip_guard, save_write,
 *                 save_read (with the whole card block), and every mod's play log row ("log")
 *   The state blocks: save (the game's 0x12E4-byte runtime save state: Aya, bag, storage, map),
 *   vars (the 0x800-byte script variable bank: story flags), game (the game state block), aya (her
 *   combatant record), enemies (the seven battle enemy slots), player (her field / battle actor),
 *   rng (the per-frame generator words) and misc (frame counter, scene token, state flags, move lock).
 *   tools/bugrec/decode.py rebuilds the blocks from a recording and prints them with names.
 *
 * Replays: at launch the recorder copies the settings, the mods' settings, quick saves and memory
 * cards to bug-recorder/session/start, then writes every change of the controller packet to
 * bug-recorder/session/tape.txt and a hash of the state blocks once a second to hashes.txt. A
 * report carries that session. tools/bugrec/replay.py starts the game headless from those files with
 * PE_BUGREC_REPLAY=<session folder>: the recorder plays the tape back into the controller and
 * compares the hashes, printing where the replay first differs from the recorded play.
 * The session of a launch that never shut down (a crash, a hang killed) is kept as
 * bug-recorder/session-previous, so a crash can still be replayed.
 *
 * Headless runs record only when PE_BUGREC=1, PE_BUGREC_DUMP_AT=<vblank> (writes a report at that
 * VBlank) or PE_BUGREC_REPLAY is set, so tests don't write into the user's folders.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "common.h"
#include "pe_game.h"
#include "pe_mod_host.h"
#include "pe_mod_menu.h"
#include "pe1/battle.h"
#include "pe1/battle_runtime.h"
#include "pe1/battle_saved_state.h"
#include "pe1/boot.h"
#include "pe1/game_state.h"
#include "pe1/pad_state.h"
#include "pe1/save_blob.h"
#include "pe1/scene_dispatch.h"
#include "pe1/script_queue.h"

#define MOD_ID "pe_bugrec"
#define PE_PAGE_BUGREC "Bug recorder"
#define RING_BYTES (40u << 20)
#define LINE_MAX_BYTES (96u << 10)
#define TAPE_VERSION 1
#define HASH_EVERY 60u      /* VBlanks between state hashes and diffs */
#define KEY_EVERY 3600u     /* VBlanks between keyframes */
#define CARD_BYTES 0x2000u

/* ---- settings ---- */
static int s_on = 1, s_minutes = 15, s_hotkey = 1;
static int s_reports;

/* ---- run state ---- */
static int s_active;             /* recording this launch */
static int s_wanted, s_started;  /* recording asked for; the first frame has decided */
static char s_replay_dir[1024];
static void bugrec_start(void);
static int s_interactive;
static char s_session[1024];     /* bug-recorder/session */
static FILE *s_tape, *s_hashes;
static int s_dump_requested;
static u32 s_dump_at;            /* PE_BUGREC_DUMP_AT */
static int s_key_down;

/* ---- the ring ---- */
static char *s_ring;
static size_t s_ring_head;
static int s_ring_wrapped;
static char s_line[LINE_MAX_BYTES];

/* ---- state blocks ---- */
typedef struct Misc {
    u32 frame_counter;
    s32 scene_token;
    u32 state_flags;
    s32 move_lock;
} Misc;

typedef struct Block {
    const char *name;
    int hashed;          /* no native pointers inside: the same in a replay */
    size_t size;
    unsigned char *last; /* as last written */
    unsigned char *now;
} Block;

enum { B_SAVE, B_VARS, B_GAME, B_AYA, B_ENEMIES, B_PLAYER, B_RNG, B_MISC, B_COUNT };
static Block s_blocks[B_COUNT] = {
    {"save", 1, 0x12E4, 0, 0},
    {"vars", 1, 0x800, 0, 0},
    {"game", 0, sizeof(Pe1GameState), 0, 0},
    {"aya", 1, sizeof(((AyaBattleState *)0)->words), 0, 0},
    {"enemies", 0, sizeof(BattleEnemySlot) * 7, 0, 0},
    {"player", 0, sizeof(BattleEntity), 0, 0},
    {"rng", 1, sizeof(u32) * 33, 0, 0},
    {"misc", 1, sizeof(Misc), 0, 0},
};
static u32 s_next_sample, s_next_key;
static int s_have_key;
static int s_last_battle = -1;
static u8 s_last_pad[8];
static int s_have_pad;

/* ---- replay ---- */
typedef struct TapeRow { u32 vblank; u8 pad[8]; } TapeRow;
typedef struct HashRow { u32 vblank; u32 h[B_COUNT]; } HashRow;
static TapeRow *s_tape_rows;
static size_t s_tape_count, s_tape_next;
static HashRow *s_hash_rows;
static size_t s_hash_count, s_hash_next, s_hash_matched;
static int s_replay, s_diverged;
static u8 s_replay_pad[8];

static u32 vblank(void) { return g_BootHostVBlankCount; }

/* ---------------------------------------------------------------- ring */

static void ring_put(const char *text, size_t n) {
    if (!s_ring) return;
    while (n) {
        size_t room = RING_BYTES - s_ring_head, take = n < room ? n : room;
        memcpy(s_ring + s_ring_head, text, take);
        s_ring_head += take;
        text += take;
        n -= take;
        if (s_ring_head == RING_BYTES) { s_ring_head = 0; s_ring_wrapped = 1; }
    }
}

/* One line: {"v":<vblank>,"t":"<tag>"<rest>}\n. rest starts with a comma, or is empty. */
static void row(const char *tag, const char *fmt, ...) {
    if (!s_active) return;
    int n = snprintf(s_line, sizeof s_line, "{\"v\":%u,\"t\":\"%s\"", (unsigned)vblank(), tag);
    if (n < 0 || (size_t)n >= sizeof s_line) return;
    if (fmt && *fmt) {
        va_list ap;
        va_start(ap, fmt);
        int m = vsnprintf(s_line + n, sizeof s_line - (size_t)n, fmt, ap);
        va_end(ap);
        if (m < 0) return;
        n += m;
        if ((size_t)n >= sizeof s_line - 3) n = (int)sizeof s_line - 3;
    }
    s_line[n++] = '}';
    s_line[n++] = '\n';
    ring_put(s_line, (size_t)n);
}

/* JSON string body: quotes, backslashes and control bytes escaped; at most out_size - 1 bytes. */
static void json_text(char *out, size_t out_size, const char *in) {
    size_t o = 0;
    for (; in && *in && o + 7 < out_size; ++in) {
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c < 0x20 || c >= 0x7F) o += (size_t)snprintf(out + o, out_size - o, "\\u%04x", c);
        else out[o++] = (char)c;
    }
    out[o] = 0;
}

static void hex(char *out, const unsigned char *bytes, size_t n) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; ++i) {
        out[i * 2] = digits[bytes[i] >> 4];
        out[i * 2 + 1] = digits[bytes[i] & 15];
    }
    out[n * 2] = 0;
}

/* ---------------------------------------------------------------- state blocks */

static void blocks_read(void) {
    memcpy(s_blocks[B_SAVE].now, &g_SaveRuntimeState, s_blocks[B_SAVE].size);
    memcpy(s_blocks[B_VARS].now, g_ScriptVariableBank, s_blocks[B_VARS].size);
    memcpy(s_blocks[B_GAME].now, &g_GameState, s_blocks[B_GAME].size);
    memcpy(s_blocks[B_AYA].now, g_AyaBattleState.words, s_blocks[B_AYA].size);
    memcpy(s_blocks[B_ENEMIES].now, g_BattleEnemySlots, s_blocks[B_ENEMIES].size);
    BattleEntity *player = pe_player();
    if (player) memcpy(s_blocks[B_PLAYER].now, player, s_blocks[B_PLAYER].size);
    else memset(s_blocks[B_PLAYER].now, 0, s_blocks[B_PLAYER].size);
    memcpy(s_blocks[B_RNG].now, g_DrawPrimGeneratorWords, s_blocks[B_RNG].size);
    Misc misc = {g_FrameRngCounter, g_SceneDispatchToken, (u32)g_GameStateFlags, (s32)g_FieldMoveLock};
    memcpy(s_blocks[B_MISC].now, &misc, sizeof misc);
}

static u32 fnv(const unsigned char *p, size_t n) {
    u32 h = 2166136261u;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 16777619u;
    return h;
}

static void keyframe(const char *why) {
    static char buf[LINE_MAX_BYTES - 256];
    row("keyframe", ",\"why\":\"%s\"", why);
    for (int b = 0; b < B_COUNT; ++b) {
        Block *k = &s_blocks[b];
        if (k->size * 2 + 1 > sizeof buf) continue;
        hex(buf, k->now, k->size);
        row("block", ",\"r\":\"%s\",\"size\":%u,\"x\":\"%s\"", k->name, (unsigned)k->size, buf);
        memcpy(k->last, k->now, k->size);
    }
    s_have_key = 1;
}

/* Changed byte runs (gaps under 8 bytes joined) as [[offset,"hex"],...]. */
static void diff_blocks(const char *why) {
    static char buf[LINE_MAX_BYTES - 256];
    for (int b = 0; b < B_COUNT; ++b) {
        Block *k = &s_blocks[b];
        /* The generator words change every frame: keyframes and hashes only, not every second. */
        if (b == B_RNG) continue;
        size_t o = 0, i = 0;
        int any = 0, full = 0;
        while (i < k->size) {
            if (k->now[i] == k->last[i]) { ++i; continue; }
            size_t start = i, end = i + 1, gap = 0;
            for (size_t j = i + 1; j < k->size && gap < 8; ++j) {
                if (k->now[j] != k->last[j]) { end = j + 1; gap = 0; }
                else ++gap;
            }
            size_t need = 24 + (end - start) * 2;
            if (o + need >= sizeof buf) { full = 1; break; }
            o += (size_t)snprintf(buf + o, sizeof buf - o, "%s[%u,\"", any ? "," : "", (unsigned)start);
            hex(buf + o, k->now + start, end - start);
            o += (end - start) * 2;
            o += (size_t)snprintf(buf + o, sizeof buf - o, "\"]");
            any = 1;
            i = end;
        }
        if (full) {
            if (k->size * 2 + 1 < sizeof buf) {
                hex(buf, k->now, k->size);
                row("block", ",\"r\":\"%s\",\"size\":%u,\"why\":\"%s\",\"x\":\"%s\"", k->name,
                    (unsigned)k->size, why, buf);
            }
        } else if (any) {
            row("diff", ",\"r\":\"%s\",\"why\":\"%s\",\"d\":[%s]", k->name, why, buf);
        }
        memcpy(k->last, k->now, k->size);
    }
}

static void state_row(void) {
    char enemies[7 * 40] = "";
    size_t o = 0;
    for (int i = 0; i < 7; ++i) {
        const BattleEnemySlot *slot = &g_BattleEnemySlots[i];
        if (!slot->active) continue;
        o += (size_t)snprintf(enemies + o, sizeof enemies - o, "%s[%d,%d,%d]", o ? "," : "", i,
                              (int)slot->combatant.hpAlive, (int)slot->combatant.maxHp);
    }
    Combatant *aya = pe_aya_core();
    BattleEntity *player = pe_player();
    row("state",
        ",\"map\":%d,\"saved_map\":%d,\"scene\":\"%08x\",\"day\":%d,\"battle\":%d,\"free\":%d,"
        "\"flags\":\"%08x\",\"hp\":%d,\"max_hp\":%d,\"pos\":[%d,%d,%d],\"rot\":%d,"
        "\"enemies\":[%s],\"frame\":%u,\"wayne\":%u",
        pe_room_number(), (int)g_SaveRuntimeState.currentMapNumber, (unsigned)g_SceneDispatchToken,
        (int)g_GameState.current_story_day, pe_in_battle(), pe_free_field(), (unsigned)g_GameStateFlags,
        aya ? (int)aya->curHP : -1, aya ? (int)aya->maxHP : -1,
        player ? (int)player->posX.parts.integer : 0, player ? (int)player->posY.parts.integer : 0,
        player ? (int)player->posZ.parts.integer : 0, player ? (int)player->facingAngle : 0, enemies,
        (unsigned)g_FrameRngCounter, pe_wayne_count());
}

/* A sample: blocks read, a keyframe or a diff, the state row. */
static void sample(const char *why) {
    if (!s_active || !pe_save_loaded()) return;
    blocks_read();
    if (!s_have_key || vblank() >= s_next_key) {
        keyframe(why);
        s_next_key = vblank() + KEY_EVERY;
    } else {
        diff_blocks(why);
    }
    state_row();
}

/* ---------------------------------------------------------------- files */

static int path_join(char *out, size_t size, const char *dir, const char *name) {
    int n = snprintf(out, size, "%s/%s", dir, name);
    return n > 0 && (size_t)n < size;
}

/* C file I/O, not SDL_LoadFile: on Windows that can't open a file still open for writing (the
 * tape, the hashes, pe_mods.log). tail_max: only the file's last bytes. */
static int copy_file(const char *from, const char *to, size_t tail_max) {
    FILE *in = fopen(from, "rb");
    if (!in) return 0;
    if (tail_max && !fseek(in, 0, SEEK_END)) {
        long size = ftell(in);
        fseek(in, size > (long)tail_max ? size - (long)tail_max : 0, SEEK_SET);
    }
    FILE *out = fopen(to, "wb");
    if (!out) { fclose(in); return 0; }
    static char buf[64 << 10];
    size_t n;
    int ok = 1;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) ok &= fwrite(buf, 1, n, out) == n;
    fclose(in);
    return (fclose(out) == 0) & ok;
}

static int remove_tree(const char *path);
static SDL_EnumerationResult remove_entry(void *user, const char *dir, const char *name) {
    (void)user;
    char path[1024];
    if (snprintf(path, sizeof path, "%s%s", dir, name) < (int)sizeof path) remove_tree(path);
    return SDL_ENUM_CONTINUE;
}
static int remove_tree(const char *path) {
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(path, &info)) return 1;
    if (info.type == SDL_PATHTYPE_DIRECTORY) SDL_EnumerateDirectory(path, remove_entry, NULL);
    return SDL_RemovePath(path);
}

typedef struct CopyJob { const char *to; int depth; } CopyJob;
static int copy_tree(const char *from, const char *to, int depth);
static SDL_EnumerationResult copy_entry(void *user, const char *dir, const char *name) {
    CopyJob *job = user;
    char from[1024], to[1024];
    if (snprintf(from, sizeof from, "%s%s", dir, name) >= (int)sizeof from) return SDL_ENUM_CONTINUE;
    if (!path_join(to, sizeof to, job->to, name)) return SDL_ENUM_CONTINUE;
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(from, &info)) return SDL_ENUM_CONTINUE;
    if (info.type == SDL_PATHTYPE_DIRECTORY) {
        if (job->depth > 0) copy_tree(from, to, job->depth - 1);
    } else if (info.size <= (16u << 20)) {
        copy_file(from, to, 0);
    }
    return SDL_ENUM_CONTINUE;
}
static int copy_tree(const char *from, const char *to, int depth) {
    CopyJob job = {to, depth};
    SDL_CreateDirectory(to);
    return SDL_EnumerateDirectory(from, copy_entry, &job);
}

/* The settings a replay needs: top-level settings, mods' settings and quick saves, and the cards. */
static SDL_EnumerationResult start_entry(void *user, const char *dir, const char *name) {
    const char *to = user;
    const char *dot = strrchr(name, '.');
    static const char *const kinds[] = {".toml", ".ini", ".pec", ".mcd", ".dat", ".txt"};
    char from[1024], target[1024];
    if (!dot || snprintf(from, sizeof from, "%s%s", dir, name) >= (int)sizeof from) return SDL_ENUM_CONTINUE;
    if (!strncmp(name, "randomizer-", 11)) return SDL_ENUM_CONTINUE; /* spoiler logs */
    for (size_t i = 0; i < sizeof kinds / sizeof *kinds; ++i) {
        if (SDL_strcasecmp(dot, kinds[i])) continue;
        SDL_PathInfo info;
        if (SDL_GetPathInfo(from, &info) && info.type == SDL_PATHTYPE_FILE && info.size <= (1u << 20) &&
            path_join(target, sizeof target, to, name))
            copy_file(from, target, 0);
    }
    return SDL_ENUM_CONTINUE;
}

/* Local date and time; file_name: 20261117-153000, else 2026-11-17 15:30:00. */
static void local_stamp(char *out, size_t size, int file_name) {
    SDL_Time now = 0;
    SDL_DateTime t;
    if (!SDL_GetCurrentTime(&now) || !SDL_TimeToDateTime(now, &t, true)) SDL_zero(t);
    snprintf(out, size, file_name ? "%04d%02d%02d-%02d%02d%02d" : "%04d-%02d-%02d %02d:%02d:%02d", t.year, t.month,
             t.day, t.hour, t.minute, t.second);
}

static void session_start(void) {
    char root[1024], previous[1024], start[1024], path[1024], config[1024];
    if (pe_mod_path(root, sizeof root, "bug-recorder") || pe_mod_path(config, sizeof config, "")) return;
    SDL_CreateDirectory(root);
    path_join(s_session, sizeof s_session, root, "session");
    path_join(previous, sizeof previous, root, "session-previous");
    /* A session that never shut down (no "ended" file) is kept for its replay. */
    if (path_join(path, sizeof path, s_session, "ended") && !SDL_GetPathInfo(path, NULL) &&
        SDL_GetPathInfo(s_session, NULL)) {
        remove_tree(previous);
        SDL_RenamePath(s_session, previous);
    }
    remove_tree(s_session);
    SDL_CreateDirectory(s_session);
    path_join(start, sizeof start, s_session, "start");
    SDL_CreateDirectory(start);
    SDL_EnumerateDirectory(config, start_entry, start);
    char cards_from[1024], cards_to[1024];
    if (path_join(cards_from, sizeof cards_from, config, "memory-cards") &&
        path_join(cards_to, sizeof cards_to, start, "memory-cards"))
        copy_tree(cards_from, cards_to, 1);

    char when[64];
    local_stamp(when, sizeof when, 0);
    if (path_join(path, sizeof path, s_session, "session.json")) {
        FILE *f = fopen(path, "w");
        if (f) {
            fprintf(f, "{\"tape_version\":%d,\"started\":\"%s\",\"interactive\":%d,\"platform\":\"%s\","
                       "\"hash_every\":%u,\"blocks\":[", TAPE_VERSION, when, s_interactive,
                    SDL_GetPlatform(), HASH_EVERY);
            for (int b = 0; b < B_COUNT; ++b)
                fprintf(f, "%s{\"name\":\"%s\",\"size\":%u,\"hashed\":%d}", b ? "," : "", s_blocks[b].name,
                        (unsigned)s_blocks[b].size, s_blocks[b].hashed);
            fprintf(f, "]}\n");
            fclose(f);
        }
    }
    if (path_join(path, sizeof path, s_session, "tape.txt")) {
        s_tape = fopen(path, "w");
        if (s_tape) fprintf(s_tape, "# vblank header buttons rx ry lx ly (hex: the controller packet, port 1)\n");
    }
    if (path_join(path, sizeof path, s_session, "hashes.txt")) {
        s_hashes = fopen(path, "w");
        if (s_hashes) {
            fprintf(s_hashes, "# vblank");
            for (int b = 0; b < B_COUNT; ++b) if (s_blocks[b].hashed) fprintf(s_hashes, " %s", s_blocks[b].name);
            fprintf(s_hashes, "\n");
        }
    }
}

static void session_end(void) {
    char path[1024];
    if (s_tape) { fclose(s_tape); s_tape = NULL; }
    if (s_hashes) { fclose(s_hashes); s_hashes = NULL; }
    if (*s_session && path_join(path, sizeof path, s_session, "ended")) SDL_SaveFile(path, "", 0);
}

/* The ring from the keyframe at or before the cut-off (minutes kept) to now. */
static int write_recording(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    size_t total = s_ring_wrapped ? RING_BYTES : s_ring_head;
    char *flat = malloc(total + 1);
    if (!flat) { fclose(f); return 0; }
    if (s_ring_wrapped) {
        memcpy(flat, s_ring + s_ring_head, RING_BYTES - s_ring_head);
        memcpy(flat + RING_BYTES - s_ring_head, s_ring, s_ring_head);
    } else {
        memcpy(flat, s_ring, total);
    }
    flat[total] = 0;
    char *p = flat;
    if (s_ring_wrapped) { char *nl = strchr(p, '\n'); p = nl ? nl + 1 : flat + total; }
    u32 now = vblank(), span = (u32)s_minutes * 60u * 60u;
    u32 cutoff = now > span ? now - span : 0;
    char *from = NULL;
    for (char *q = p; q && *q;) {
        unsigned v = 0;
        if (!strncmp(q, "{\"v\":", 5) && sscanf(q + 5, "%u", &v) == 1 && strstr(q, "\"t\":\"keyframe\"") &&
            strstr(q, "\"t\":\"keyframe\"") < strchr(q, '\n')) {
            if (!from || v <= cutoff) from = q;
            if (v > cutoff) break;
        }
        char *nl = strchr(q, '\n');
        q = nl ? nl + 1 : NULL;
    }
    if (!from) from = p;
    fwrite(from, 1, (size_t)(flat + total - from), f);
    free(flat);
    return fclose(f) == 0;
}

static const char kReadme[] =
    "Parasite Eve bug report (Bug recorder mod)\n"
    "\n"
    "note.txt          what went wrong: write it there (what you did, what you expected, what happened)\n"
    "recording.jsonl   the last minutes of play, one JSON object per line (see mods/pe_bugrec/pe_bugrec.c)\n"
    "session/          this launch from its start: start/ (settings, mods' settings, quick saves, memory\n"
    "                  cards as they were at launch), tape.txt (every controller change), hashes.txt\n"
    "pe_mods.log, pe_play.log   the ends of the mod and play logs\n"
    "\n"
    "Read it:   python tools/bugrec/decode.py <this folder>            (timeline with named fields)\n"
    "           python tools/bugrec/decode.py <this folder> --at VBLANK  (every state block at that moment)\n"
    "Replay it: python tools/bugrec/replay.py <this folder> --exe <parasite-eve-pc> --disc <disc 1 cue>\n";

static void dump_report(const char *why) {
    char root[1024], dir[1024], path[1024], stamp[64], src[1024];
    if (!s_active || pe_mod_path(root, sizeof root, "bug-reports")) return;
    SDL_CreateDirectory(root);
    local_stamp(stamp, sizeof stamp, 1);
    if (!path_join(dir, sizeof dir, root, stamp)) return;
    for (int i = 2; SDL_GetPathInfo(dir, NULL) && i < 100; ++i) {
        char alt[96];
        snprintf(alt, sizeof alt, "%s-%d", stamp, i);
        path_join(dir, sizeof dir, root, alt);
    }
    SDL_CreateDirectory(dir);

    sample("bug");
    row("bug", ",\"why\":\"%s\"", why);
    if (s_tape) fflush(s_tape);
    if (s_hashes) fflush(s_hashes);

    int ok = path_join(path, sizeof path, dir, "recording.jsonl") && write_recording(path);
    if (path_join(path, sizeof path, dir, "README.txt")) SDL_SaveFile(path, kReadme, sizeof kReadme - 1);
    if (path_join(path, sizeof path, dir, "note.txt")) {
        char note[256];
        int n = snprintf(note, sizeof note, "Saved at VBlank %u (%s). What happened:\n\n", (unsigned)vblank(), why);
        SDL_SaveFile(path, note, (size_t)n);
    }
    if (path_join(path, sizeof path, dir, "session")) copy_tree(s_session, path, 2);
    static const char *const logs[] = {"pe_mods.log", "pe_play.log"};
    for (int i = 0; i < 2; ++i)
        if (!pe_mod_path(src, sizeof src, logs[i]) && path_join(path, sizeof path, dir, logs[i]))
            copy_file(src, path, 512u << 10);

    ++s_reports;
    pe_mod_log(MOD_ID, "bug report %s: %s", ok ? "saved" : "FAILED", dir);
    printf("bugrec: report %s %s\n", ok ? "saved" : "failed", dir);
    fflush(stdout);
    if (s_interactive) {
        char msg[160];
        snprintf(msg, sizeof msg, ok ? "Bug report saved (bug-reports/%s)" : "Bug report failed (%s)", stamp);
        pe_toast(msg);
    }
}

/* ---------------------------------------------------------------- replay */

static int replay_load(const char *dir) {
    char path[1024], line[512];
    if (!path_join(path, sizeof path, dir, "tape.txt")) return 0;
    FILE *f = fopen(path, "r");
    if (!f) { printf("bugrec: no tape in %s\n", dir); return 0; }
    size_t cap = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned v, h, b, rx, ry, lx, ly;
        if (line[0] == '#' || sscanf(line, "%u %x %x %x %x %x %x", &v, &h, &b, &rx, &ry, &lx, &ly) != 7) continue;
        if (s_tape_count == cap) {
            cap = cap ? cap * 2 : 4096;
            TapeRow *grown = realloc(s_tape_rows, cap * sizeof *grown);
            if (!grown) break;
            s_tape_rows = grown;
        }
        TapeRow *r = &s_tape_rows[s_tape_count++];
        r->vblank = v;
        r->pad[0] = (u8)h; r->pad[1] = (u8)(h >> 8); r->pad[2] = (u8)b; r->pad[3] = (u8)(b >> 8);
        r->pad[4] = (u8)rx; r->pad[5] = (u8)ry; r->pad[6] = (u8)lx; r->pad[7] = (u8)ly;
    }
    fclose(f);
    if (path_join(path, sizeof path, dir, "hashes.txt") && (f = fopen(path, "r"))) {
        cap = 0;
        while (fgets(line, sizeof line, f)) {
            HashRow r = {0};
            char *p = line, *end;
            if (line[0] == '#') continue;
            r.vblank = (u32)strtoul(p, &end, 10);
            if (end == p) continue;
            p = end;
            for (int b = 0; b < B_COUNT; ++b) {
                if (!s_blocks[b].hashed) continue;
                r.h[b] = (u32)strtoul(p, &end, 16);
                p = end;
            }
            if (s_hash_count == cap) {
                cap = cap ? cap * 2 : 4096;
                HashRow *grown = realloc(s_hash_rows, cap * sizeof *grown);
                if (!grown) break;
                s_hash_rows = grown;
            }
            s_hash_rows[s_hash_count++] = r;
        }
        fclose(f);
    }
    printf("bugrec: replaying %u controller changes, %u state checks from %s\n", (unsigned)s_tape_count,
           (unsigned)s_hash_count, dir);
    fflush(stdout);
    return 1;
}

static void replay_pad(void) {
    while (s_tape_next < s_tape_count && s_tape_rows[s_tape_next].vblank <= vblank())
        memcpy(s_replay_pad, s_tape_rows[s_tape_next++].pad, 8);
    if (!s_tape_next) return;
    PadPacket *pad = &g_PadPackets[0];
    pad->header = (u16)(s_replay_pad[0] | s_replay_pad[1] << 8);
    pad->buttons = (u16)(s_replay_pad[2] | s_replay_pad[3] << 8);
    pad->rightX = s_replay_pad[4]; pad->rightY = s_replay_pad[5];
    pad->leftX = s_replay_pad[6]; pad->leftY = s_replay_pad[7];
}

/* ---------------------------------------------------------------- per frame */

static void hash_check(void) {
    u32 h[B_COUNT] = {0};
    for (int b = 0; b < B_COUNT; ++b) if (s_blocks[b].hashed) h[b] = fnv(s_blocks[b].now, s_blocks[b].size);
    if (s_hashes) {
        fprintf(s_hashes, "%u", (unsigned)vblank());
        for (int b = 0; b < B_COUNT; ++b) if (s_blocks[b].hashed) fprintf(s_hashes, " %08x", (unsigned)h[b]);
        fprintf(s_hashes, "\n");
    }
    if (!s_replay || s_diverged) return;
    while (s_hash_next < s_hash_count && s_hash_rows[s_hash_next].vblank < vblank()) ++s_hash_next;
    if (s_hash_next >= s_hash_count || s_hash_rows[s_hash_next].vblank != vblank()) return;
    const HashRow *want = &s_hash_rows[s_hash_next++];
    char which[128] = "";
    size_t o = 0;
    for (int b = 0; b < B_COUNT; ++b)
        if (s_blocks[b].hashed && want->h[b] != h[b])
            o += (size_t)snprintf(which + o, sizeof which - o, "%s%s", o ? "," : "", s_blocks[b].name);
    if (o) {
        s_diverged = 1;
        printf("bugrec: replay DIVERGED at vblank %u in %s (after %u matching checks)\n", (unsigned)vblank(),
               which, (unsigned)s_hash_matched);
        row("replay_diverged", ",\"blocks\":\"%s\",\"matched\":%u", which, (unsigned)s_hash_matched);
    } else if (++s_hash_matched % 60 == 0 || s_hash_next == s_hash_count) {
        printf("bugrec: replay matches through vblank %u (%u checks%s)\n", (unsigned)vblank(),
               (unsigned)s_hash_matched, s_hash_next == s_hash_count ? ", all of them" : "");
    }
    fflush(stdout);
}

static void record_pad(void) {
    const PadPacket *pad = &g_PadPackets[0];
    u8 now[8] = {(u8)pad->header, (u8)(pad->header >> 8), (u8)pad->buttons, (u8)(pad->buttons >> 8),
                 pad->rightX, pad->rightY, pad->leftX, pad->leftY};
    if (s_have_pad && !memcmp(now, s_last_pad, 8)) return;
    memcpy(s_last_pad, now, 8);
    s_have_pad = 1;
    u16 header = (u16)(now[0] | now[1] << 8), buttons = (u16)(now[2] | now[3] << 8);
    if (s_tape)
        fprintf(s_tape, "%u %04x %04x %02x %02x %02x %02x\n", (unsigned)vblank(), header, buttons, now[4], now[5],
                now[6], now[7]);
    row("pad", ",\"header\":\"%04x\",\"held\":\"%04x\",\"rx\":%u,\"ry\":%u,\"lx\":%u,\"ly\":%u", header,
        (u16)~buttons, now[4], now[5], now[6], now[7]);
}

static int on_pad_read(void *ctx, void *user) {
    (void)ctx;
    (void)user;
    if (!s_started) bugrec_start();
    if (!s_active) return 0;
    if (s_replay) replay_pad();
    record_pad();

    if (s_interactive && s_hotkey) {
        const bool *keys = SDL_GetKeyboardState(NULL);
        int down = keys && keys[SDL_SCANCODE_F8];
        if (down && !s_key_down) s_dump_requested = 1;
        s_key_down = down;
    }

    int battle = pe_in_battle();
    if (s_last_battle >= 0 && battle != s_last_battle && pe_save_loaded()) {
        row(battle ? "battle_start" : "battle_end", ",\"map\":%d,\"hp\":%d", pe_room_number(),
            pe_aya_core() ? (int)pe_aya_core()->curHP : -1);
        sample(battle ? "battle_start" : "battle_end");
    }
    s_last_battle = battle;

    if (vblank() >= s_next_sample) {
        s_next_sample = vblank() + HASH_EVERY;
        if (pe_save_loaded()) {
            sample("tick");
            hash_check();
        }
        if (s_tape) fflush(s_tape);
        if (s_hashes) fflush(s_hashes);
    }

    if (s_dump_at && vblank() >= s_dump_at) { s_dump_at = 0; dump_report("PE_BUGREC_DUMP_AT"); }
    if (s_dump_requested) { s_dump_requested = 0; dump_report(s_interactive ? "F8" : "request"); }
    return 0;
}

/* ---------------------------------------------------------------- events */

static int on_scene(void *ctx, void *user) {
    (void)ctx; (void)user;
    row("scene", ",\"map\":%d,\"token\":\"%08x\",\"day\":%d", pe_room_number(), (unsigned)g_SceneDispatchToken,
        (int)g_GameState.current_story_day);
    sample("scene");
    return 0;
}
static int on_new_game(void *ctx, void *user) {
    (void)user;
    const PeHookNewGame *g = ctx;
    row("newgame", ",\"kind\":%d", g ? g->kind : 0);
    return 0;
}
static int enemy_index(const BattleEntity *e) {
    if (!e || !e->core) return -1;
    for (int i = 0; i < 7; ++i) if ((const void *)&g_BattleEnemySlots[i].combatant == e->core) return i;
    return -1;
}
static int on_hit(void *ctx, void *user) {
    (void)user;
    const BattleEntity *e = ctx;
    const EnemyCombatant *c = e ? e->core : NULL;
    row("hit", ",\"slot\":%d,\"hp\":%d", enemy_index(e), c ? (int)c->hpAlive : -1);
    return 0;
}
static int on_hit_end(void *ctx, void *user) {
    (void)user;
    const BattleEntity *e = ctx;
    const EnemyCombatant *c = e ? e->core : NULL;
    row("hit_end", ",\"slot\":%d,\"hp\":%d", enemy_index(e), c ? (int)c->hpAlive : -1);
    return 0;
}
static int on_hp_loss(void *ctx, void *user) {
    (void)user;
    const PeHookHpLoss *h = ctx;
    if (!h) return 0;
    BattleEntity *player = pe_player();
    row("hp_loss", ",\"aya\":%d,\"hp\":%d,\"loss\":%d,\"source\":%u",
        player && h->actor == (const void *)player->core, h->current_hp, h->loss ? *h->loss : 0, h->source);
    return 0;
}
static int on_spell(void *ctx, void *user) {
    (void)user;
    const PeHookSpell *s = ctx;
    if (s) row("spell", ",\"power\":%u,\"tag\":\"%s\",\"target_slot\":%d", s->spell,
               pe_power_tag((int)s->spell) ? pe_power_tag((int)s->spell) : "", enemy_index(s->target));
    return 0;
}
static int on_rewards(void *ctx, void *user) {
    (void)user;
    const PeHookRewards *r = ctx;
    if (r) row("rewards", ",\"exp\":%d", r->exp);
    return 0;
}
static int on_inv_command(void *ctx, void *user) {
    (void)user;
    const PeHookInvCommand *c = ctx;
    if (c) row("invcmd", ",\"command\":%d,\"value\":%d", c->command, c->value);
    return 0;
}
static int on_item_add(void *ctx, void *user) {
    (void)user;
    const s32 *id = ctx;
    row("item_add", ",\"id\":%d,\"field\":%d", id ? (int)*id : -1, !pe_in_battle());
    return 0;
}
static int on_item_remove(void *ctx, void *user) {
    (void)user;
    const PeHookItemRemove *r = ctx;
    if (r) row("item_remove", ",\"index\":%d", r->index);
    return 0;
}
static int on_item_count(void *ctx, void *user) {
    (void)user;
    int **args = ctx;
    if (args && args[0]) row("item_count", ",\"item\":%d", *args[0]);
    return 0;
}
static int on_item_use(void *ctx, void *user) {
    (void)user;
    const int *id = ctx;
    row("item_use", ",\"id\":%d", id ? *id : -1);
    return 0;
}
static int on_steal(void *ctx, void *user) {
    (void)ctx; (void)user;
    row("steal", "");
    return 0;
}
static int on_tool_transfer(void *ctx, void *user) {
    (void)user;
    const PeHookToolTransfer *t = ctx;
    if (t) row("tool_transfer", ",\"selection\":%u,\"modifier\":%d,\"flags\":%d,\"cost\":%d", t->selection,
               t->modifier, t->flags, t->cost);
    return 0;
}
static int on_storage_transfer(void *ctx, void *user) {
    (void)user;
    const PeHookStorageTransfer *t = ctx;
    if (t) row("storage_transfer", ",\"from_mode\":%d,\"from\":%d,\"to_mode\":%d,\"to\":%d", t->from_mode,
               t->from_index, t->to_mode, t->to_index);
    return 0;
}
static int on_menu_action(void *ctx, void *user) {
    (void)user;
    const PeHookMenuAction *m = ctx;
    if (m) row("menu_action", ",\"action\":%d", m->action);
    return 0;
}
static int on_script_random(void *ctx, void *user) {
    (void)user;
    const PeHookScriptRandom *r = ctx;
    if (r && r->start && r->end) row("script_random", ",\"start\":%d,\"end\":%d", *r->start, *r->end);
    return 0;
}
static int on_text_page(void *ctx, void *user) {
    (void)user;
    const PeHookTextPage *t = ctx;
    if (t) row("text_page", ",\"page\":%u", t->page);
    return 0;
}
static int on_equip_guard(void *ctx, void *user) {
    (void)user;
    const PeHookEquipmentGuard *g = ctx;
    if (g) row("equip_guard", ",\"kind\":%d,\"value\":%d,\"selection\":%u", g->kind, g->value, g->selection);
    return 0;
}
static void card_row(const char *tag, const u8 *block) {
    static char buf[CARD_BYTES * 2 + 1];
    if (!block) return;
    hex(buf, block, CARD_BYTES);
    row(tag, ",\"card\":\"%s\"", buf);
}
static int on_save_write(void *ctx, void *user) {
    (void)user;
    card_row("save_write", ctx);
    sample("save_write");
    return 0;
}
static int on_save_read(void *ctx, void *user) {
    (void)user;
    card_row("save_read", ctx);
    return 0;
}
static int on_log_line(void *ctx, void *user) {
    (void)user;
    const PeLogLine *l = ctx;
    static char tag[128], text[4096];
    if (!l) return 0;
    json_text(tag, sizeof tag, l->tag);
    json_text(text, sizeof text, l->text);
    row("log", ",\"tag\":\"%s\",\"text\":\"%s\"", tag, text);
    return 0;
}

/* ---------------------------------------------------------------- start / stop */

static void save_now(void) { s_dump_requested = 1; }
static void reports_text(char *out, unsigned size) {
    snprintf(out, size, s_active ? "%d this launch" : "recording off this launch", s_reports);
}

static int bugrec_init(void) {
    pe_menu_begin(PE_PAGE_BUGREC);
    pe_menu_top_page(PE_PAGE_BUGREC, "Keeps the last minutes of play; F8 saves them as a bug report.", 90);
    pe_menu_register_toggle(0, "Record", "Keep the last minutes of play in memory (from the next launch).",
                            &s_on, NULL);
    pe_menu_register_int(0, "Minutes kept", "How much play a bug report holds.", &s_minutes, 1, 30, 1);
    pe_menu_register_toggle(0, "F8 saves a report", "Press F8 the moment something goes wrong.", &s_hotkey,
                            NULL);
    pe_menu_register_action(0, "Save a bug report now",
                            "Writes bug-reports/<date-time> beside your settings: the recording, this "
                            "launch's controller tape and starting saves, and the logs.", save_now);
    pe_menu_register_info(0, "Bug reports", "Reports saved since launch.", reports_text);
    pe_menu_begin(0);

    s_interactive = pe_mod_interactive();
    const char *replay = SDL_getenv("PE_BUGREC_REPLAY");
    const char *dump_at = SDL_getenv("PE_BUGREC_DUMP_AT");
    const char *forced = SDL_getenv("PE_BUGREC");
    if (dump_at && *dump_at) s_dump_at = (u32)strtoul(dump_at, NULL, 10);
    if (s_interactive) s_wanted = 1; /* the Record row, read when the first frame starts recording */
    else s_wanted = (replay && *replay) || s_dump_at || (forced && !strcmp(forced, "1"));
    if (replay && *replay) SDL_strlcpy(s_replay_dir, replay, sizeof s_replay_dir);

    /* Handlers only run while the mod is on in Manage mods: the first frame starts recording. */
    pe_hook_add(PE_HOOK_PAD_READ, on_pad_read, NULL, PE_PRIORITY_EARLY, MOD_ID);
    pe_hook_add(PE_HOOK_SCENE_START, on_scene, NULL, PE_PRIORITY_LATE, MOD_ID);
    pe_hook_add(PE_HOOK_NEW_GAME, on_new_game, NULL, PE_PRIORITY_LATE, MOD_ID);
    pe_hook_add(PE_HOOK_ENEMY_HIT, on_hit, NULL, PE_PRIORITY_LATE, MOD_ID);
    pe_hook_add(PE_HOOK_ENEMY_HIT_END, on_hit_end, NULL, PE_PRIORITY_LATE, MOD_ID);
    pe_hook_add(PE_HOOK_HP_LOSS, on_hp_loss, NULL, PE_PRIORITY_LATE, MOD_ID);
    pe_hook_add(PE_HOOK_SPELL, on_spell, NULL, PE_PRIORITY_LATE, MOD_ID);
    pe_hook_add(PE_HOOK_BATTLE_REWARDS, on_rewards, NULL, PE_PRIORITY_LATE, MOD_ID);
    pe_hook_add(PE_HOOK_INV_COMMAND, on_inv_command, NULL, PE_PRIORITY_EARLY, MOD_ID);
    pe_hook_add(PE_HOOK_ITEM_ADD, on_item_add, NULL, PE_PRIORITY_EARLY, MOD_ID);
    pe_hook_add(PE_HOOK_ITEM_REMOVE, on_item_remove, NULL, PE_PRIORITY_EARLY, MOD_ID);
    pe_hook_add(PE_HOOK_ITEM_COUNT, on_item_count, NULL, PE_PRIORITY_EARLY, MOD_ID);
    pe_hook_add(PE_HOOK_ITEM_USE, on_item_use, NULL, PE_PRIORITY_LATE, MOD_ID);
    pe_hook_add(PE_HOOK_STEAL, on_steal, NULL, PE_PRIORITY_LATE, MOD_ID);
    pe_hook_add(PE_HOOK_TOOL_TRANSFER, on_tool_transfer, NULL, PE_PRIORITY_EARLY, MOD_ID);
    pe_hook_add(PE_HOOK_STORAGE_TRANSFER, on_storage_transfer, NULL, PE_PRIORITY_EARLY, MOD_ID);
    pe_hook_add(PE_HOOK_MAIN_MENU_ACTION, on_menu_action, NULL, PE_PRIORITY_EARLY, MOD_ID);
    pe_hook_add(PE_HOOK_SCRIPT_RANDOM, on_script_random, NULL, PE_PRIORITY_LATE, MOD_ID);
    pe_hook_add(PE_HOOK_TEXT_PAGE, on_text_page, NULL, PE_PRIORITY_LATE, MOD_ID);
    pe_hook_add(PE_HOOK_EQUIPMENT_GUARD, on_equip_guard, NULL, PE_PRIORITY_EARLY, MOD_ID);
    pe_hook_add(PE_HOOK_SAVE_WRITE, on_save_write, NULL, PE_PRIORITY_LATE, MOD_ID);
    pe_hook_add(PE_HOOK_SAVE_READ, on_save_read, NULL, PE_PRIORITY_EARLY, MOD_ID);
    pe_hook_add(pe_event_id(PE_EVENT_LOG_LINE), on_log_line, NULL, PE_PRIORITY_NORMAL, MOD_ID);
    return 0;
}

/* The first frame with the mod on: memory, the session folder, the replay tape. */
static void bugrec_start(void) {
    s_started = 1;
    if (!s_wanted || (s_interactive && !s_on)) return;
    s_ring = malloc(RING_BYTES);
    for (int b = 0; b < B_COUNT; ++b) {
        s_blocks[b].last = calloc(1, s_blocks[b].size);
        s_blocks[b].now = calloc(1, s_blocks[b].size);
        if (!s_blocks[b].last || !s_blocks[b].now) { free(s_ring); s_ring = NULL; }
    }
    if (!s_ring) { pe_mod_log(MOD_ID, "out of memory, not recording"); return; }
    if (*s_replay_dir) s_replay = replay_load(s_replay_dir);
    session_start();
    s_active = 1;
    pe_mod_log(MOD_ID, "recording%s; session %s", s_replay ? " (replay)" : "", s_session);
}

static void bugrec_shutdown(void) {
    if (!s_active) return;
    session_end();
    free(s_ring);
    s_ring = NULL;
    for (int b = 0; b < B_COUNT; ++b) { free(s_blocks[b].last); free(s_blocks[b].now); }
    free(s_tape_rows);
    free(s_hash_rows);
    s_active = 0;
}

const PeMod pe_mod_pe_bugrec = {MOD_ID, "Bug recorder", "1.0", NULL, bugrec_init, bugrec_shutdown};
