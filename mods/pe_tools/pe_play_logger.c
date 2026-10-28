/*
 * Detailed play log: F1 > Debug > Play log (the psxrecomp build's pe_loot_logger.c). Records play to
 * pe_play_log.csv beside the preferences, for balancing against real runs and chasing bugs.
 *
 * Columns (as the recomp's, for the bridge's analysis tools):
 *   kind, battle, day, map, scene, play_s, level, exp, hp, max_hp, junk, wayne, detail
 * Every row carries the context: the battle number while in battle, the story day, the map number
 * and scene name (g_SceneDispatchToken in base 32), play time in seconds, Aya's level, total EXP,
 * HP / max HP, Junk (the economy's `junk` service, if any) and Wayne's count. `detail` is
 * key=value pairs.
 *
 * Rows written here:
 *   session     one per launch: the local date and time.
 *   enemy       one per enemy as its battle slot frees: identity, start HP, defense, EXP, BP base /
 *               factor / hits, drop, steal, resistances, poison damage, charge step, boss, died.
 *   battle      one per battle: frames and seconds, EXP and BP, outcome (win / escape / loss), HP
 *               before -> after, gun, armor, boss, rewards, the combat summary and the snapshot.
 *   combat      the summary on battle rows: turns, actions by id, PE start -> end / spent /
 *               regained, casts by name, dealt / taken / healed, lowest HP, kills, first kill
 *               (seconds), shots, reloads, weapon swaps, bag items used.
 *   turn, hit, hurt, heal, spell, reload, kill, weapon   with "Combat detail" on: a row per event.
 *   pickup      a change in Aya's count of Junk, Tools, kits, trading cards or Mod Permits in field.
 *   steal / add an Inv_AddItem (a steal when PE_HOOK_STEAL announced one), with free bag slots.
 *   invcmd      a script inventory command; itemcheck a script asking how many of an item she has.
 *   wayne       Wayne's count moving (a hand-in, a craft), once it settles: from -> to.
 *   levelup, newgame, cardload, save (bag use, gear, bonus points), room (from, and the snapshot).
 *   store / withdraw   an item moved between the bag and Wayne's storage.
 * The snapshot: bag and storage items below 0x100 as id:count, the three ammo pools and the gun's
 * loaded rounds, so what was used shows as the difference between two snapshots.
 * Every other mod's pe_log row (loot, purchases, installs, Combine, settings changed ...) is copied
 * in as well, through the host's PE_EVENT_LOG_LINE.
 *
 * Freeze trace (F1 > Debug > Play log > Freeze trace), for hangs that leave no other row:
 *   speed       the turbo speed changing: from -> to (1 is off).
 *   phase       where the game is moving between field, battle and transition (drawn frames with
 *               neither a field nor a battle frame: a door, a load, a battle intro); scene marks
 *               PE_HOOK_SCENE_START. Each carries the frame counters and the speed.
 *   beat        once a second from its own thread, so it keeps writing while the game thread is
 *               stuck: the counters, the phase, the speed and how long since the last drawn frame.
 *               Counters that stop moving between beats show where it hung; no beats at all means
 *               the whole process stopped.
 *
 * The file starts over (the old one renamed with the time) when its header isn't this version's.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <SDL3/SDL.h>
#include <psyz/video.h>

#include "pe_game.h"
#include "pe_mod_host.h"
#include "pe_mod_menu.h"
#include "pe_pages.h"
#include "pe_services.h"
#include "pe1/asset_table.h"
#include "pe1/battle_runtime.h"
#include "pe1/battle_start.h"
#include "pe1/game_state.h"
#include "pe1/game_timers.h"
#include "pe1/inventory.h"
#include "pe1/inventory_state.h"
#include "pe1/scene_dispatch.h"

#define PE_FILE           "pe_play_log.csv"
#define PE_HEADER         "kind,battle,day,map,scene,play_s,level,exp,hp,max_hp,junk,wayne,detail"
#define PE_BAG            50
#define PE_STORAGE        100
#define PE_ENEMY_SLOTS    7
#define PE_GAUGE_FULL     9000u
#define PE_ESCAPE_PHASE   4
#define PE_MIN_FRAMES     3u       /* shorter: the blip after a load */
#define PE_WAYNE_SETTLE   90u

static int s_logging = 1, s_detail;
static FILE *s_log;
static int s_session;
static unsigned s_frame, s_battle_no, s_battle_frames;
static int s_in_battle, s_escape, s_boss, s_hp_start;
static int s_pending;              /* a load, reported on the next frame with its context */
enum { PE_PENDING_NONE, PE_PENDING_NEWGAME, PE_PENDING_CARD };
static int s_level_seen = -1;
static int s_steal_next;
static char s_rewards[160];
static int s_wayne_valid, s_wayne_moving;
static unsigned s_wayne_prev, s_wayne_from, s_wayne_changed;
static int s_room_token, s_room_valid;
static s16 s_prev_bag[PE_BAG], s_prev_store[PE_STORAGE];
static int s_lists_valid;

/* Freeze trace: counters written on the game thread, read by the beat thread. */
static int s_trace;
static SDL_Mutex *s_file_lock;     /* rows come from the game thread and the beat thread */
static SDL_Thread *s_beat_thread;
static SDL_AtomicInt s_beat_stop;
static volatile unsigned s_drawn, s_field_frames, s_battle_ticks, s_speed = 1;
static volatile Uint64 s_drawn_ms;
static volatile const char *s_phase = "start";
static volatile int s_last_map = -1;
static char s_last_scene[8];
static unsigned s_seen_field, s_seen_battle;


/* Item ids whose count is watched for field pickups: Junk, Tools, kits, Mod Permit, trading cards. */
static const u16 s_watch[] = {
    0x0B, 0x0C, 0x14, 0x15, 0x16, 0x17, 0x35, 0x36, 0x37,
    0xDD, 0xDE, 0xDF, 0xE0, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xEB,
};
#define PE_WATCH (sizeof s_watch / sizeof s_watch[0])
static int s_counts[PE_WATCH], s_counts_valid;

/* ---- the file ---- */
static int pe_open(void) {
    char path[1100], first[160] = "", old[1200];
    FILE *f;
    if (s_log) return 1;
    if (pe_mod_path(path, sizeof path, PE_FILE) != 0) return 0;
    if ((f = fopen(path, "r"))) {                  /* another version's columns: start over */
        const int same = fgets(first, sizeof first, f) && strncmp(first, PE_HEADER, strlen(PE_HEADER)) == 0;
        fclose(f);
        if (!same) {
            snprintf(old, sizeof old, "%.*s.%ld.csv", (int)(strlen(path) - 4), path, (long)time(NULL));
            rename(path, old);
        }
    }
    if (!(s_log = fopen(path, "a"))) return 0;
    fseek(s_log, 0, SEEK_END);
    if (ftell(s_log) == 0) fprintf(s_log, "%s\n", PE_HEADER);
    return 1;
}

static int pe_scene_name(int token, char *out) {
    int i;
    for (i = 0; i < 6; ++i) {
        char c = (char)g_Base32CharTable[((unsigned)token >> ((5 - i) * 5 + 2)) & 0x1Fu];
        if (c >= 'a' && c <= 'z') c = (char)(c - 0x20);
        out[i] = c >= 0x20 && c < 0x7F && c != ',' ? c : '?';
    }
    out[6] = 0;
    if (out[2] < '0' || out[2] > '9' || out[3] < '0' || out[3] > '9' || out[4] < '0' || out[4] > '9') return -1;
    return (out[2] - '0') * 100 + (out[3] - '0') * 10 + (out[4] - '0');
}

static unsigned pe_junk(void) {
    const PeJunkApi *j = pe_service_get("junk", 1);
    return j ? (unsigned)j->junk() : 0u;
}

static void pe_row(const char *kind, const char *fmt, ...) {
    char detail[1100], battle[12] = "", scene[8];
    va_list ap;
    char *c;
    int session;
    if (!s_logging) return;
    SDL_LockMutex(s_file_lock);
    if (!pe_open()) { SDL_UnlockMutex(s_file_lock); return; }
    session = !s_session;
    s_session = 1;
    if (session) {                                 /* the first row of a launch */
        char stamp[32];
        const time_t now = time(NULL);
        const struct tm *local = localtime(&now);
        if (!local || !strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", local)) snprintf(stamp, sizeof stamp, "unknown");
        pe_row("session", "start=%s", stamp);
    }
    va_start(ap, fmt);
    vsnprintf(detail, sizeof detail, fmt, ap);
    va_end(ap);
    for (c = detail; *c; ++c)                      /* one CSV field */
        if (*c == ',') *c = ';';
        else if (*c == '"') *c = '\'';
        else if (*c == '\n' || *c == '\r') *c = ' ';
    if (s_in_battle) snprintf(battle, sizeof battle, "%u", s_battle_no);
    fprintf(s_log, "%s,%s,", kind, battle);
    if (pe_save_loaded()) fprintf(s_log, "%d,", (int)g_GameState.current_story_day);
    else fprintf(s_log, ",");
    s_last_map = pe_scene_name(g_SceneDispatchToken, scene);
    memcpy(s_last_scene, scene, sizeof s_last_scene);
    fprintf(s_log, "%d,%s,", s_last_map, scene);
    if (pe_save_loaded())
        fprintf(s_log, "%u,%u,%u,%u,%u,%u,%u,", (unsigned)(g_PlayTimeFrameCounter / 60u), g_AyaSaveState.level + 1u,
                (unsigned)g_AyaSaveState.total_exp, g_AyaSaveState.current_hp, g_AyaSaveState.max_hp, pe_junk(),
                pe_wayne_count());
    else fprintf(s_log, ",,,,,,,");
    fprintf(s_log, "%s\n", detail);
    fflush(s_log);
    SDL_UnlockMutex(s_file_lock);
}

/* Every other mod's pe_log row, copied in. */
static int pe_on_log_line(void *ctx, void *user) {
    const PeLogLine *line = ctx;
    (void)user;
    if (line && line->tag && line->text) pe_row(line->tag, "%s", line->text);
    return 0;
}

/* ---- snapshots ---- */
static unsigned pe_item_of(s16 v) {                /* gear (0x100 + record) by its item id */
    return v >= 0x100 && v < 0x180 ? g_InventoryRuntime.equipment[v - 0x100].itemId : (unsigned)(u16)v;
}

static void pe_counts(const s16 *list, int n, u8 *out) {
    int i;
    memset(out, 0, 256);
    for (i = 0; i < n; ++i) if (list[i] > 0 && list[i] < 0x100 && out[list[i]] < 255) ++out[list[i]];
}

static void pe_snapshot(char *out, size_t size) {
    u8 bag[256], store[256];
    const ItemDataRecord *gun = pe_equipped(0);
    size_t n = 0;
    int v;
    pe_counts(g_AyaSaveState.inventory_items, PE_BAG, bag);
    pe_counts(g_WayneStorageItems, PE_STORAGE, store);
    n += (size_t)snprintf(out + n, size - n, "bag=");
    for (v = 1; v < 256 && n < size; ++v) if (bag[v]) n += (size_t)snprintf(out + n, size - n, "%02X:%u/", v, bag[v]);
    if (n < size) n += (size_t)snprintf(out + n, size - n, " st=");
    for (v = 1; v < 256 && n < size; ++v) if (store[v]) n += (size_t)snprintf(out + n, size - n, "%02X:%u/", v, store[v]);
    if (n < size)
        snprintf(out + n, size - n, " ammo=%u/%u/%u loaded=%u", g_KeyItemRecords[0].ammo, g_KeyItemRecords[1].ammo,
                 g_KeyItemRecords[2].ammo, gun ? gun->ammo : 0u);
}

static int pe_bag_free(void) {
    int n = 0, i, limit = Inv_GetAyaSlotLimit();
    if (limit > PE_BAG) limit = PE_BAG;
    for (i = 0; i < limit; ++i) n += g_AyaSaveState.inventory_items[i] == 0;
    return n;
}

static unsigned pe_gear_id(int armor) { const ItemDataRecord *r = pe_equipped(armor); return r ? r->itemId : 0u; }

/* ---- combat ---- */
static struct {
    int valid;
    unsigned turns, casts[20], act_id[8], act_n[8];
    int pe_start, pe_last, pe_spent, pe_gained, hp_last, hp_min;
    unsigned dealt, taken, healed, kills, first_kill, shots, reloads, wswaps, gauge_last;
    const ItemDataRecord *gun;
    int loaded_last;
    u8 bag_start[256];
} s_cb;

static struct {
    int tracking;
    u8 entity_id;
    int start_hp, last_hp, boss, poison;
    unsigned identity, exp, defense, bp_base, bp_factor, bp_hits, drop, steal, charge;
    u32 status2;
} s_enemy[PE_ENEMY_SLOTS];

static unsigned pe_secs(void) { return (s_battle_frames * 10u / 599u + 5u) / 10u; }

static void pe_combat_start(void) {
    memset(&s_cb, 0, sizeof s_cb);
    pe_counts(g_AyaSaveState.inventory_items, PE_BAG, s_cb.bag_start);
}

static void pe_combat_tick(void) {
    const Combatant *core = pe_aya_core();
    const ItemDataRecord *gun = pe_equipped(0);
    int hp, pe, loaded;
    unsigned gauge;
    if (!core) return;
    hp = core->curHP;
    pe = core->currentPe >> 16;
    gauge = core->hpAlive;
    loaded = gun ? gun->ammo : 0;
    if (!s_cb.valid) {
        s_cb.valid = 1;
        s_cb.pe_start = s_cb.pe_last = pe;
        s_cb.hp_last = s_cb.hp_min = hp;
        s_cb.gauge_last = gauge;
        s_cb.gun = gun;
        s_cb.loaded_last = loaded;
        return;
    }
    if (hp < s_cb.hp_last) {
        s_cb.taken += (unsigned)(s_cb.hp_last - hp);
        if (s_detail) pe_row("hurt", "dmg=%d t=%u", s_cb.hp_last - hp, pe_secs());
    } else if (hp > s_cb.hp_last) {
        s_cb.healed += (unsigned)(hp - s_cb.hp_last);
        if (s_detail) pe_row("heal", "hp=%d t=%u", hp - s_cb.hp_last, pe_secs());
    }
    if (hp < s_cb.hp_min) s_cb.hp_min = hp;
    s_cb.hp_last = hp;
    if (pe < s_cb.pe_last) s_cb.pe_spent += s_cb.pe_last - pe;
    else s_cb.pe_gained += pe - s_cb.pe_last;
    s_cb.pe_last = pe;
    if (s_cb.gauge_last >= PE_GAUGE_FULL && gauge < s_cb.gauge_last) {   /* a turn */
        const unsigned id = core->action ? (u16)core->action->actionCode.actionId : 0xFFFFu;
        int k = 0;
        ++s_cb.turns;
        while (k < 8 && s_cb.act_n[k] && s_cb.act_id[k] != id) ++k;
        if (k < 8) { s_cb.act_id[k] = id; ++s_cb.act_n[k]; }
        if (s_detail) pe_row("turn", "n=%u act=%u t=%u pe=%d loaded=%d", s_cb.turns, id, pe_secs(), pe, loaded);
    }
    s_cb.gauge_last = gauge;
    if (gun != s_cb.gun) {
        ++s_cb.wswaps;
        if (s_detail) pe_row("weapon", "gun=0x%02X t=%u", pe_gear_id(0), pe_secs());
        s_cb.gun = gun;
    } else if (loaded < s_cb.loaded_last) {
        s_cb.shots += (unsigned)(s_cb.loaded_last - loaded);
    } else if (loaded > s_cb.loaded_last) {
        ++s_cb.reloads;
        if (s_detail) pe_row("reload", "loaded=%d t=%u", loaded, pe_secs());
    }
    s_cb.loaded_last = loaded;
}

static void pe_enemy_hp(int slot, int before, int after) {
    const int a = after < 0 ? 0 : after;
    if (before <= 0 || a >= before) return;
    s_cb.dealt += (unsigned)(before - a);
    if (s_detail) pe_row("hit", "slot=%d dmg=%d left=%d t=%u", slot, before - a, a, pe_secs());
    if (a == 0) {
        if (!s_cb.kills) s_cb.first_kill = pe_secs();
        ++s_cb.kills;
        if (s_detail) pe_row("kill", "slot=%d t=%u", slot, pe_secs());
    }
}

static void pe_log_enemy(int s, int died) {
    const u32 s2 = s_enemy[s].status2;
    if (!s_enemy[s].identity) return;              /* an empty slot, not a real enemy */
    /* Resist bits (2 each): 0 neutral / 50%, 1 resists / immune, 2 weak / always. */
    pe_row("enemy", "slot=%d id=0x%04X hp=%d def=%u exp=%u bp=%u/%u/%u drop=0x%02X steal=0x%02X boss=%d died=%d"
           " heat=%u frost=%u cyanide=%u acid=%u tranq=%u steal_r=%u poison=%d charge=%u",
           s, s_enemy[s].identity, s_enemy[s].start_hp, s_enemy[s].defense, s_enemy[s].exp, s_enemy[s].bp_base,
           s_enemy[s].bp_factor, s_enemy[s].bp_hits, s_enemy[s].drop, s_enemy[s].steal, s_enemy[s].boss, died,
           (unsigned)(s2 >> 14) & 3u, (unsigned)(s2 >> 16) & 3u, (unsigned)s2 & 3u, (unsigned)(s2 >> 2) & 3u,
           (unsigned)(s2 >> 4) & 3u, (unsigned)(s2 >> 12) & 3u, s_enemy[s].poison, s_enemy[s].charge);
}

static void pe_track_enemies(void) {
    int s;
    for (s = 0; s < PE_ENEMY_SLOTS; ++s) {
        const EnemyCombatant *core = &g_BattleEnemySlots[s].combatant;
        const int active = g_BattleEnemySlots[s].active != 0;
        const u8 eid = core->lowAndEntityId.bytes.entityId;
        int hp;
        if (s_enemy[s].tracking && (!active || eid != s_enemy[s].entity_id)) {
            pe_log_enemy(s, s_enemy[s].last_hp <= 0);
            s_enemy[s].tracking = 0;
        }
        if (!active) continue;
        hp = core->hpAlive;
        if (!s_enemy[s].tracking) {
            s_enemy[s].tracking = 1;
            s_enemy[s].entity_id = eid;
            s_enemy[s].start_hp = s_enemy[s].last_hp = hp;   /* a new enemy: no damage yet */
        }
        if (hp > s_enemy[s].start_hp) s_enemy[s].start_hp = hp;   /* scripts may set HP after spawn */
        if (s_enemy[s].identity) pe_enemy_hp(s, s_enemy[s].last_hp, hp);
        s_enemy[s].last_hp = hp;
        s_enemy[s].identity = (u16)core->field04.fieldId04;
        s_enemy[s].exp = core->hpMirror;
        s_enemy[s].defense = core->defense;
        s_enemy[s].bp_base = core->rewardBase;
        s_enemy[s].bp_factor = core->rewardFactorA;
        s_enemy[s].bp_hits = core->rewardFactorB;
        s_enemy[s].drop = core->rewardSlotId;
        s_enemy[s].steal = core->stealItemId;
        s_enemy[s].status2 = core->statusFlags2;
        s_enemy[s].boss = (core->statusFlags2 & 0x40000u) != 0;
        s_enemy[s].poison = core->periodicDamage;
        s_enemy[s].charge = core->chargeStep;
        if (s_enemy[s].identity && s_enemy[s].boss) s_boss = 1;
    }
}

static void pe_battle_end(void) {
    char combat[460], snap[400];
    size_t n = 0;
    u8 now[256];
    int k, v;
    const int hp = g_AyaSaveState.current_hp;
    if (s_battle_frames < PE_MIN_FRAMES) return;
    n += (size_t)snprintf(combat + n, sizeof combat - n, "turns=%u acts=", s_cb.turns);
    for (k = 0; k < 8 && s_cb.act_n[k] && n < sizeof combat; ++k)
        n += (size_t)snprintf(combat + n, sizeof combat - n, "%u:%u/", s_cb.act_id[k], s_cb.act_n[k]);
    if (n < sizeof combat)
        n += (size_t)snprintf(combat + n, sizeof combat - n, " pe=%d->%d spent=%d regained=%d casts=", s_cb.pe_start,
                              s_cb.pe_last, s_cb.pe_spent, s_cb.pe_gained);
    for (k = 0; k < 20 && n < sizeof combat; ++k)
        if (s_cb.casts[k]) n += (size_t)snprintf(combat + n, sizeof combat - n, "%s:%u/", pe_power_tag(k), s_cb.casts[k]);
    if (n < sizeof combat)
        n += (size_t)snprintf(combat + n, sizeof combat - n,
                              " dealt=%u taken=%u healed=%u hp_min=%d kills=%u first_kill=%u shots=%u reloads=%u wswaps=%u used=",
                              s_cb.dealt, s_cb.taken, s_cb.healed, s_cb.hp_min, s_cb.kills, s_cb.first_kill, s_cb.shots,
                              s_cb.reloads, s_cb.wswaps);
    pe_counts(g_AyaSaveState.inventory_items, PE_BAG, now);
    for (v = 1; v < 256 && n < sizeof combat; ++v)
        if (s_cb.bag_start[v] > now[v]) n += (size_t)snprintf(combat + n, sizeof combat - n, "%02X:%u/", v, s_cb.bag_start[v] - now[v]);
    pe_snapshot(snap, sizeof snap);
    pe_row("battle", "frames=%u secs=%u exp=%u bp=%u outcome=%s hp=%d->%d gun=0x%02X armor=0x%02X boss=%d rewards=%s %s %s",
           s_battle_frames, pe_secs(), (unsigned)g_BattleExpReward, (unsigned)g_BattlePeBonusReward,
           hp <= 0 ? "loss" : s_escape ? "escape" : "win", s_hp_start, hp, pe_gear_id(0), pe_gear_id(1), s_boss,
           s_rewards[0] ? s_rewards : "-", combat, snap);
    s_rewards[0] = 0;
}

/* ---- field tracking ---- */
static int pe_count_item(u16 id) {
    int n = 0, i;
    for (i = 0; i < PE_BAG; ++i) n += (u16)g_AyaSaveState.inventory_items[i] == id;
    return n;
}

static void pe_track_wayne(void) {
    const unsigned now = pe_wayne_count();
    if (!s_wayne_valid) { s_wayne_valid = 1; s_wayne_prev = now; return; }
    if (now != s_wayne_prev) {
        if (!s_wayne_moving) { s_wayne_moving = 1; s_wayne_from = s_wayne_prev; }
        s_wayne_changed = s_frame;
        s_wayne_prev = now;
    }
    if (s_wayne_moving && s_frame - s_wayne_changed > PE_WAYNE_SETTLE) {
        s_wayne_moving = 0;
        if (now != s_wayne_from) pe_row("wayne", "from=%u to=%u", s_wayne_from, now);
    }
}

static void pe_track_room(void) {
    if (s_room_valid && g_SceneDispatchToken != s_room_token) {
        char from[8], snap[400];
        const int map = pe_scene_name(s_room_token, from);
        pe_snapshot(snap, sizeof snap);
        pe_row("room", "from=%d/%s %s", map, from, snap);
    }
    s_room_token = g_SceneDispatchToken;
    s_room_valid = 1;
}

/* Items moved between the bag and Wayne's storage. */
static void pe_track_storage(void) {
    const s16 *bag = g_AyaSaveState.inventory_items, *store = g_WayneStorageItems;
    if (s_lists_valid && (memcmp(bag, s_prev_bag, sizeof s_prev_bag) || memcmp(store, s_prev_store, sizeof s_prev_store))) {
        s16 bag_out[16], bag_in[16], st_out[16], st_in[16];
        int nbo = 0, nbi = 0, nso = 0, nsi = 0, a, b, i;
        for (i = 0; i < PE_BAG; ++i)
            if (bag[i] != s_prev_bag[i]) {
                if (s_prev_bag[i] && nbo < 16) bag_out[nbo++] = s_prev_bag[i];
                if (bag[i] && nbi < 16) bag_in[nbi++] = bag[i];
            }
        for (i = 0; i < PE_STORAGE; ++i)
            if (store[i] != s_prev_store[i]) {
                if (s_prev_store[i] && nso < 16) st_out[nso++] = s_prev_store[i];
                if (store[i] && nsi < 16) st_in[nsi++] = store[i];
            }
        for (a = 0; a < nbo; ++a)
            for (b = 0; b < nsi; ++b)
                if (bag_out[a] == st_in[b]) {
                    pe_row("store", "item=0x%02X%s", pe_item_of(bag_out[a]), bag_out[a] >= 0x100 ? " gear" : "");
                    st_in[b] = 0;
                    break;
                }
        for (a = 0; a < nso; ++a)
            for (b = 0; b < nbi; ++b)
                if (st_out[a] == bag_in[b]) {
                    pe_row("withdraw", "item=0x%02X%s", pe_item_of(st_out[a]), st_out[a] >= 0x100 ? " gear" : "");
                    bag_in[b] = 0;
                    break;
                }
    }
    memcpy(s_prev_bag, bag, sizeof s_prev_bag);
    memcpy(s_prev_store, store, sizeof s_prev_store);
    s_lists_valid = 1;
}

/* ---- freeze trace ---- */
static void pe_trace_row(const char *kind, const char *what) {
    pe_row(kind, "%s drawn=%u field=%u battle=%u speed=%u", what, s_drawn, s_field_frames, s_battle_ticks, s_speed);
}

static void pe_trace_speed(void) {
    const unsigned now = Psyz_VideoGetSpeed();
    if (now == s_speed) return;
    if (s_trace) {
        char what[32];
        snprintf(what, sizeof what, "from=%u to=%u", s_speed, now);
        s_speed = now;
        pe_trace_row("speed", what);
    }
    s_speed = now;
}

static void pe_trace_phase(const char *phase) {
    if (s_phase == phase) return;
    s_phase = phase;
    if (s_trace) pe_trace_row("phase", phase);
}

/* Each drawn frame: field, battle, or neither since the last one (a door, a load, a battle intro). */
static int pe_on_frame_drawn(void *ctx, void *user) {
    (void)ctx;
    (void)user;
    ++s_drawn;
    s_drawn_ms = SDL_GetTicks();
    pe_trace_speed();
    if (s_battle_ticks != s_seen_battle) pe_trace_phase("battle");
    else if (s_field_frames != s_seen_field) pe_trace_phase("field");
    else pe_trace_phase("transition");
    s_seen_battle = s_battle_ticks;
    s_seen_field = s_field_frames;
    return 0;
}

static int pe_on_scene_start(void *ctx, void *user) {
    (void)ctx;
    (void)user;
    pe_trace_speed();
    if (s_trace) pe_trace_row("phase", "scene_start");
    return 0;
}

/* Its own row, not pe_row: game state may be mid-change on the game thread, so it carries the
 * map and scene of the game thread's last row. */
static int SDLCALL pe_beat_main(void *user) {
    (void)user;
    while (!SDL_GetAtomicInt(&s_beat_stop)) {
        SDL_Delay(1000);
        if (!s_logging || !s_trace) continue;
        SDL_LockMutex(s_file_lock);
        if (s_log) {
            const Uint64 since = s_drawn_ms ? SDL_GetTicks() - s_drawn_ms : 0;
            fprintf(s_log, "beat,,,%d,%s,,,,,,,,phase=%s drawn=%u field=%u battle=%u speed=%u since_drawn_ms=%llu\n",
                    s_last_map, s_last_scene, (const char *)s_phase, s_drawn, s_field_frames, s_battle_ticks, s_speed,
                    (unsigned long long)since);
            fflush(s_log);
        }
        SDL_UnlockMutex(s_file_lock);
    }
    return 0;
}

/* ---- every frame (field and battle) ---- */
static void pe_tick(void) {
    unsigned i;
    int level;
    ++s_frame;
    if (!s_logging) {
        SDL_LockMutex(s_file_lock);
        if (s_log) { fclose(s_log); s_log = NULL; }
        SDL_UnlockMutex(s_file_lock);
        s_counts_valid = 0;
        s_pending = PE_PENDING_NONE;
        return;
    }
    if (!pe_game_loaded()) return;
    if (s_pending) {                               /* the loaded game's context, now */
        pe_row(s_pending == PE_PENDING_NEWGAME ? "newgame" : "cardload", "%s", "");
        s_pending = PE_PENDING_NONE;
        s_counts_valid = s_wayne_valid = s_wayne_moving = s_room_valid = s_lists_valid = 0;
        s_level_seen = -1;
        s_in_battle = 0;
        memset(s_enemy, 0, sizeof s_enemy);
    }
    level = g_AyaSaveState.level + 1;
    if (s_level_seen > 0 && level > s_level_seen) pe_row("levelup", "to=%d", level);
    s_level_seen = level;
    if (pe_in_battle()) {
        if (!s_in_battle) {
            s_in_battle = 1;
            ++s_battle_no;
            s_battle_frames = 0;
            s_escape = s_boss = 0;
            s_hp_start = g_AyaSaveState.current_hp;
            pe_combat_start();
        }
        pe_combat_tick();
        ++s_battle_frames;
        if (g_BattleModeState == PE_ESCAPE_PHASE) s_escape = 1;
        pe_track_enemies();
        s_counts_valid = s_lists_valid = 0;        /* battle rewards aren't field pickups */
        return;
    }
    if (s_in_battle) {
        int s;
        for (s = 0; s < PE_ENEMY_SLOTS; ++s)
            if (s_enemy[s].tracking) { pe_log_enemy(s, s_enemy[s].last_hp <= 0); s_enemy[s].tracking = 0; }
        pe_battle_end();
        s_in_battle = 0;
    }
    pe_track_wayne();
    pe_track_room();
    pe_track_storage();
    for (i = 0; i < PE_WATCH; ++i) {               /* field pickups */
        const int now = pe_count_item(s_watch[i]);
        if (s_counts_valid && now != s_counts[i]) pe_row("pickup", "item=0x%02X delta=%d", s_watch[i], now - s_counts[i]);
        s_counts[i] = now;
    }
    s_counts_valid = 1;
}

/* Once a frame: the field frame outside battle, the battle update in it. */
static int pe_on_frame(void *ctx, void *user) { (void)ctx; (void)user; ++s_field_frames; if (!pe_in_battle()) pe_tick(); return 0; }
static int pe_on_battle(void *ctx, void *user) { (void)ctx; (void)user; ++s_battle_ticks; if (pe_in_battle()) pe_tick(); return 0; }

/* ---- hooked moments ---- */
static int pe_on_steal(void *ctx, void *user) { (void)ctx; (void)user; s_steal_next = 1; return 0; }

static int pe_on_item_add(void *ctx, void *user) {
    const s32 id = *(const s32 *)ctx;
    (void)user;
    if (pe_save_loaded()) pe_row(s_steal_next ? "steal" : "add", "item=0x%02X bagfree=%d", (unsigned)(id & 0xFFFF), pe_bag_free());
    s_steal_next = 0;
    return 0;
}

static int pe_on_inv_command(void *ctx, void *user) {
    const PeHookInvCommand *c = ctx;
    (void)user;
    if (pe_save_loaded()) pe_row("invcmd", "cmd=%d value=0x%X", c->command, (unsigned)c->value);
    return 0;
}

/* A new game of any kind: its row is written once the game is running. */
static int pe_on_new_game(void *ctx, void *user) {
    (void)ctx; (void)user;
    s_pending = PE_PENDING_NEWGAME;
    return 0;
}

/* Script_CountItemsByValue ({&item, &result}): the item, and how many the active list holds. */
static int pe_on_item_count(void *ctx, void *user) {
    int **args = ctx;
    unsigned item;
    int count = 0, i, limit = g_InvSlotLimit;
    (void)user;
    if (!pe_save_loaded() || !args || !args[0]) return 0;
    item = (unsigned)*args[0] & 0xFFFFu;
    if (limit > PE_STORAGE) limit = PE_STORAGE;
    for (i = 0; g_InvItemPtr && i < limit; ++i) count += pe_item_of(g_InvItemPtr[i]) == item;
    pe_row("itemcheck", "item=0x%02X count=%d list=%s", item, count,
           g_InvItemPtr == g_AyaSaveState.inventory_items ? "bag" : g_InvItemPtr == g_WayneStorageItems ? "storage" : "other");
    return 0;
}

static int pe_on_spell(void *ctx, void *user) {
    const PeHookSpell *spell = ctx;
    (void)user;
    if (!s_in_battle || spell->spell >= 20) return 0;
    ++s_cb.casts[spell->spell];
    if (s_detail) pe_row("spell", "tech=%s t=%u", pe_power_tag(spell->spell), pe_secs());
    return 0;
}

static int pe_on_rewards(void *ctx, void *user) {
    const PeHookRewards *r = ctx;
    size_t n = 0;
    int i;
    (void)user;
    s_rewards[0] = 0;
    for (i = 0; r->list && i < 10 && r->list[i].id && n < sizeof s_rewards; ++i) {
        n += (size_t)snprintf(s_rewards + n, sizeof s_rewards - n, "%s0x%02X", n ? " " : "", (unsigned)r->list[i].id);
        if (r->list[i].extra >= 0 && n < sizeof s_rewards)
            n += (size_t)snprintf(s_rewards + n, sizeof s_rewards - n, "x%d", (int)r->list[i].extra);
    }
    return 0;
}

static void pe_gear(char *out, size_t size, int armor) {
    const ItemDataRecord *rec = pe_equipped(armor);
    size_t n = strlen(out);
    int s, slots;
    if (!rec) { snprintf(out + n, size - n, " %s=none", armor ? "armor" : "gun"); return; }
    n += (size_t)snprintf(out + n, size - n, " %s=0x%02X+%d/%d/%d mods=", armor ? "armor" : "gun", rec->itemId,
                          rec->bonusStats[0], rec->bonusStats[1], rec->bonusStats[2]);
    slots = rec->tailCount > 10 ? 10 : rec->tailCount;
    for (s = 0; s < slots && n < size; ++s) n += (size_t)snprintf(out + n, size - n, "%s%d", s ? "/" : "", rec->tailData[s] & 0x1F);
    if (slots <= 0 && n < size) snprintf(out + n, size - n, "-");
}

static int pe_on_save_write(void *ctx, void *user) {
    char line[220];
    int used = 0, i, limit = Inv_GetAyaSlotLimit();
    (void)ctx;
    (void)user;
    if (!pe_save_loaded()) return 0;
    if (limit > PE_BAG) limit = PE_BAG;
    for (i = 0; i < limit; ++i) used += g_AyaSaveState.inventory_items[i] != 0;
    snprintf(line, sizeof line, "bag=%d/%d bp=%u", used, limit, (unsigned)g_AyaSaveState.bonus_points);
    pe_gear(line, sizeof line, 0);
    pe_gear(line, sizeof line, 1);
    pe_row("save", "%s", line);
    return 0;
}

static int pe_on_save_read(void *ctx, void *user) { (void)ctx; (void)user; s_pending = PE_PENDING_CARD; return 0; }

void pe_play_logger_shutdown(void) {
    if (s_beat_thread) {
        SDL_SetAtomicInt(&s_beat_stop, 1);
        SDL_WaitThread(s_beat_thread, NULL);
        s_beat_thread = NULL;
    }
    SDL_LockMutex(s_file_lock);
    if (s_log) fclose(s_log);
    s_log = NULL;
    SDL_UnlockMutex(s_file_lock);
}

void pe_play_logger_register(const char *mod_id) {
    pe_menu_register_toggle(PE_PAGE_DEBUG_PLAY_LOG, "Play log", "Log play to pe_play_log.csv beside the settings.", &s_logging, NULL);
    pe_menu_register_toggle(PE_PAGE_DEBUG_PLAY_LOG, "Combat detail", "Play log: a row per hit, turn, spell.", &s_detail, NULL);
    pe_menu_register_toggle(PE_PAGE_DEBUG_PLAY_LOG, "Freeze trace", "Play log: turbo, transition phases and a beat a second.", &s_trace, NULL);
    s_file_lock = SDL_CreateMutex();
    s_beat_thread = SDL_CreateThread(pe_beat_main, "pe_play_beat", NULL);
    pe_hook_add(PE_HOOK_FRAME_DRAWN, pe_on_frame_drawn, NULL, PE_PRIORITY_LATE, mod_id);
    pe_hook_add(PE_HOOK_SCENE_START, pe_on_scene_start, NULL, PE_PRIORITY_LATE, mod_id);
    pe_hook_add(PE_HOOK_FRAME, pe_on_frame, NULL, PE_PRIORITY_LATE, mod_id);
    pe_hook_add(PE_HOOK_BATTLE_UPDATE, pe_on_battle, NULL, PE_PRIORITY_LATE, mod_id);
    pe_hook_add(PE_HOOK_STEAL, pe_on_steal, NULL, PE_PRIORITY_EARLY, mod_id);
    pe_hook_add(PE_HOOK_ITEM_ADD, pe_on_item_add, NULL, PE_PRIORITY_EARLY, mod_id);   /* before a mod handles it */
    pe_hook_add(PE_HOOK_INV_COMMAND, pe_on_inv_command, NULL, PE_PRIORITY_EARLY, mod_id);
    pe_hook_add(PE_HOOK_NEW_GAME, pe_on_new_game, NULL, PE_PRIORITY_EARLY, mod_id);
    pe_hook_add(PE_HOOK_ITEM_COUNT, pe_on_item_count, NULL, PE_PRIORITY_EARLY, mod_id);
    pe_hook_add(PE_HOOK_SPELL, pe_on_spell, NULL, PE_PRIORITY_NORMAL, mod_id);
    pe_hook_add(PE_HOOK_BATTLE_REWARDS, pe_on_rewards, NULL, PE_PRIORITY_LATE, mod_id);
    pe_hook_add(PE_HOOK_SAVE_WRITE, pe_on_save_write, NULL, PE_PRIORITY_LATE, mod_id);
    pe_hook_add(PE_HOOK_SAVE_READ, pe_on_save_read, NULL, PE_PRIORITY_NORMAL, mod_id);
    pe_hook_add(pe_event_id(PE_EVENT_LOG_LINE), pe_on_log_line, NULL, PE_PRIORITY_NORMAL, mod_id);
}
