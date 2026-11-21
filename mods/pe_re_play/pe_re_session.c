/* A play session in an imported game. See pe_re_session.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pe_imp_rdt.h"
#include "pe_re_session.h"
#include "pe_re_zone.h"

/* First rooms: RE2 and RE3 open in the street (room 1-00); RE1 in the mansion's main hall (1-06).
 * RE1 has a ground-floor preview using its static collision records. */
static const PeReGame k_games[PE_RE_GAMES] = {
    {"re1", "Resident Evil", 0, 0x06, 1},
    {"re2", "Resident Evil 2", 0, 0x00, 1},
    {"re3", "Resident Evil 3", 0, 0x00, 0},
};

const PeReGame *pe_re_game(int i) { return i >= 0 && i < PE_RE_GAMES ? &k_games[i] : NULL; }
const PeReGame *pe_re_game_named(const char *game) {
    for (int i = 0; game && i < PE_RE_GAMES; i++) if (!strcmp(k_games[i].game, game)) return &k_games[i];
    return NULL;
}

void pe_re_logic_id(const char *game, uint32_t stage, uint32_t room, uint32_t player, char *out, size_t n) {
    if (!strcmp(game, "re3")) snprintf(out, n, "re3:logic/%x%02x", (unsigned)stage + 1, (unsigned)room);
    else snprintf(out, n, "%s:logic/%x%02x-%u", game, (unsigned)stage + 1, (unsigned)room, (unsigned)player);
}

void pe_re_room_id(const char *game, uint32_t stage, uint32_t room, char *out, size_t n) {
    snprintf(out, n, "%s:room/%x%02x", game, (unsigned)stage + 1, (unsigned)room);
}

int pe_re_imported(const char *store, const PeReGame *g) {
    char logic[PE_IMP_ID_MAX], alternate_logic[PE_IMP_ID_MAX], room[PE_IMP_ID_MAX];
    PeImpAsset *a = NULL;
    int found = 0;
    if (!store || !g) return 0;
    pe_re_logic_id(g->game, g->stage, g->room, 0, logic, sizeof logic);
    pe_re_logic_id(g->game, g->stage, g->room, 1, alternate_logic, sizeof alternate_logic);
    pe_re_room_id(g->game, g->stage, g->room, room, sizeof room);
    const int n = pe_imp_catalog_assets(store, g->game, &a);
    for (int i = 0; i < n; i++) {
        if (!strcmp(a[i].id, logic) || !strcmp(a[i].id, alternate_logic)) found |= 1;
        if (!strcmp(a[i].id, room)) found |= 2;
    }
    free(a);
    return found == 3;
}

/* ---- zones ---- */
int pe_re_in_quad(const int16_t x[4], const int16_t z[4], double px, double pz) {
    int inside = 0;
    for (int i = 0, j = 3; i < 4; j = i++)               /* even-odd: any four-sided zone */
        if ((z[i] > pz) != (z[j] > pz) && px < (double)(x[j] - x[i]) * (pz - z[i]) / (double)(z[j] - z[i]) + x[i])
            inside = !inside;
    return inside;
}

/* Switch zones come grouped by the camera they leave. Each group opens with that camera's own area,
 * not a switch: RE2 and RE3 store it as the group's first record ("from 1 to 0" in room 1-00, a
 * zone covering cameras 0 and 1, which taken as a switch flips 0 and 1 for ever); RE1 flags it 1. */
int pe_re_camera_at(const PeImpRoomLogic *l, int camera, double x, double z) {
    uint32_t n = 0;
    int group = -1;
    if (!l || !pe_imp_logic_section(l, PE_IMP_LOGIC_CAMZ, &n, NULL)) return camera;
    for (uint32_t i = 0; i < n; i++) {
        PeImpCameraZone c;
        if (pe_imp_logic_camera_zone(l, i, &c)) continue;
        const int own = l->game == PE_IMP_RE1 ? c.flag == 1 : c.from != group;
        group = c.from;
        if (own || c.from != camera || c.to == camera) continue;
        if (pe_re_in_quad(c.x, c.z, x, z)) return c.to;
    }
    return camera;
}

int pe_re_door_at(const PeImpRoomLogic *l, const PeReScdRun *run, double x, double z, PeImpDoor *door) {
    uint32_t n = 0;
    if (!l || !pe_imp_logic_section(l, PE_IMP_LOGIC_DOOR, &n, NULL)) return -1;
    for (uint32_t i = 0; i < n; i++) {
        PeImpDoor d;
        if (pe_imp_logic_door(l, i, &d)) continue;
        if (run && (!pe_re_scd_live(run, d.at) || pe_re_zone_kind(run, d.zone.id, d.zone.sce, NULL) != d.zone.sce)) continue;
        if (pe_re_in_quad(d.zone.x, d.zone.z, x, z)) { if (door) *door = d; return (int)i; }
    }
    return -1;
}

int pe_re_door_is_stairway(const PeImpRoomLogic *room, const PeImpDoor *door) {
    return room && room->game == PE_IMP_RE1 && door->animation >= 0x16 && door->animation <= 0x19;
}

int pe_re1_examine_kind(const PeImpTrigger *trigger) {
    if ((trigger->data[0] & 0x81) != 0x81) return PE_RE_ZONE_NOTHING;
    switch (trigger->zone.sce) {
    case 0x02: return trigger->data[1] & 0x80 ? PE_RE_ZONE_MESSAGE : PE_RE_ZONE_NOTHING;
    case 0x08: return PE_RE_ZONE_ITEMBOX;
    case 0x10: return PE_RE_ZONE_SAVE;
    default: return PE_RE_ZONE_NOTHING;
    }
}

int pe_re_free_spot(const PeRe2RoomCollision *c, int floor, double *x, double *z, int radius, int reach) {
    if (!pe_imp_re2_blocked_on(c, (unsigned)floor, *x, *z, radius)) return 1;
    for (int r = 50; r <= reach; r += 50)                  /* rings of 16 points, nearest ring first */
        for (int k = 0; k < 16; k++) {
            static const double dir[16][2] = {{1, 0}, {.924, .383}, {.707, .707}, {.383, .924}, {0, 1}, {-.383, .924},
                                              {-.707, .707}, {-.924, .383}, {-1, 0}, {-.924, -.383}, {-.707, -.707},
                                              {-.383, -.924}, {0, -1}, {.383, -.924}, {.707, -.707}, {.924, -.383}};
            const double nx = *x + r * dir[k][0], nz = *z + r * dir[k][1];
            if (!pe_imp_re2_blocked_on(c, (unsigned)floor, nx, nz, radius)) { *x = nx, *z = nz; return 1; }
        }
    return 0;
}

/* ---- the session ---- */
static int has_item(void *ctx, uint16_t item) { return pe_re_has(&((PeReSession *)ctx)->progress, item); }
static int talking(void *ctx) { const PeReSession *s = ctx; return s->talking && s->talking(); }
static void lose_item(void *ctx, uint16_t item, uint16_t count) { pe_re_lose(&((PeReSession *)ctx)->progress, item, count); }
static void give_item(void *ctx, uint16_t item, uint16_t count) { pe_re_give(&((PeReSession *)ctx)->progress, item, count); }
static PeReScdHost host_of(PeReSession *s) {
    PeReScdHost h = {
        .has = has_item, .talking = talking, .key = s->key, .key_trg = s->key_trg,
        .ctx = s, .lose = lose_item, .give = give_item
    };
    return h;
}

static int load(PeReSession *s, const char *store, uint32_t stage, uint32_t room, char *why, size_t n) {
    char id[PE_IMP_ID_MAX];
    PeImpRoomLogic *l = NULL;
    pe_re_logic_id(s->game->game, stage, room, s->player, id, sizeof id);
    if (pe_imp_catalog_roomlogic(store, id, &l, why, n)) {
        /* RE1 stores shared rooms in ROOMxxx0.RDT. A Jill-specific room wins whenever present. */
        if (s->game != pe_re_game_named("re1") || s->player != 1) return -1;
        pe_re_logic_id(s->game->game, stage, room, 0, id, sizeof id);
        if (pe_imp_catalog_roomlogic(store, id, &l, NULL, 0)) return -1;
        l->player = s->player;
        if (why && n) *why = 0;
    }
    free(s->logic);
    s->logic = l, s->stage = stage, s->room = room;
    const PeReScdHost host = host_of(s);
    memset(s->progress.flags.bits[PE_RE_FG_USE], 0, sizeof s->progress.flags.bits[PE_RE_FG_USE]);   /* Sce_work_clr */
    pe_re_scd_run(l, &s->progress.flags, &host, &s->run);   /* what its scripts set up */
    s->auto_message = -1;
    return 0;
}

int pe_re_session_start(PeReSession *s, const char *store, const PeReGame *g, uint32_t player, char *why, size_t n) {
    memset(s, 0, sizeof *s);
    if (!g) { snprintf(why, n, "no such game"); return -1; }
    s->game = g, s->player = player, s->store = store;
    pe_re_flag_set(&s->progress.flags, PE_RE_FG_STATUS, 0, player != 0);   /* who's playing (pe_re_scd.h) */
    if (load(s, store, g->stage, g->room, why, n)) { s->game = NULL; return -1; }
    char items[PE_IMP_ID_MAX];
    snprintf(items, sizeof items, "%s:items/all", g->game);
    if (pe_imp_catalog_itemtable(store, items, &s->items, NULL, 0)) s->items = NULL;   /* names fall back to numbers */
    return 0;
}

int pe_re_session_enter(PeReSession *s, const char *store, uint32_t stage, uint32_t room, int camera, double x,
                        double z, int facing, char *why, size_t n) {
    if (!s->game) { snprintf(why, n, "no session"); return -1; }
    if (load(s, store, stage, room, why, n)) return -1;
    s->camera = camera;
    pe_re_session_place(s, x, z, facing);
    return 0;
}

void pe_re_door_sound_id(const char *game, const PeImpDoor *d, unsigned wave, char *out, size_t n) {
    snprintf(out, n, "%s:sound/door%02x/wave%02u", game, d->texture < 55 ? (unsigned)d->texture : 0u, wave);
}

int pe_re_floor_of(int y) {
    const int f = (y - PE_RE2_FLOOR_HEIGHT / 2) / -PE_RE2_FLOOR_HEIGHT;
    return f < 0 ? 0 : f > 31 ? 31 : f;
}

int pe_re_facing_of(int game_dir) { return (game_dir - 1024) & 4095; }

void pe_re_session_place(PeReSession *s, double x, double z, int facing) {
    s->x = x, s->z = z, s->facing = facing & 4095;
}

enum { CREST_PASSAGE_ROOM = 0x1A, CREST_GATE_ROOM = 0x1B, CREST_GATE_LOCK = 0x97 };
enum { COURTYARD_STAGE = 2, POOL_ROOM = 1, WATERFALL_ROOM = 2 };
enum { DORM_003_ROOM = 0x0A, PLANT_ROOM = 0x0C, BOOKSHELF_DOOR = 2, BOOKSHELF_TRIGGER = 7 };
enum { DORM_002_ROOM = 6, BASEMENT_ROOM = 0x0d, AQUARIUM_ROOM = 0x0e, SUPPLY_CLOSET = 0x10, CONTROL_ROOM = 0x11 };

int pe_re1_is_pool(const PeImpRoomLogic *room) {
    return room && room->game == PE_IMP_RE1 && room->stage == COURTYARD_STAGE && room->room == POOL_ROOM;
}

int pe_re1_is_pool_ladder(const PeImpRoomLogic *room, const PeImpTrigger *trigger) {
    return pe_re1_is_pool(room) && trigger->zone.sce == 0x0c && trigger->data[0] == 0x81 &&
           (trigger->zone.id == 2 || trigger->zone.id == 3);
}

void pe_re1_pool_collision(PeRe2RoomCollision *collision) {
    static const PeRe2Collider bounds[] = {
        /* The static walls leave a gap at either ladder. Close the gap on the bank. */
        {13176, 11736, 31014, 24624, 0, 0, 1},
        /* Below the bank, wall off the narrow passage between the two ladders. */
        {-32768, -32768, 21150, 32767, 0, 0, 1u << PE_RE1_POOL_FLOOR},
        {23616, -32768, 32767, 32767, 0, 0, 1u << PE_RE1_POOL_FLOOR},
        {21150, -32768, 23616, 10710, 0, 0, 1u << PE_RE1_POOL_FLOOR},
        {21150, 25668, 23616, 32767, 0, 0, 1u << PE_RE1_POOL_FLOOR},
    };
    const unsigned count = sizeof bounds / sizeof *bounds;
    if (collision->count <= PE_RE2_COLLIDER_MAX - count) {
        memcpy(collision->entries + collision->count, bounds, sizeof bounds);
        collision->count += count;
    }
}

static int re1_use_done(const PeReSession *s, unsigned flag) {
    return pe_re_flag(&s->progress.flags, PE_RE1_FG_PREVIEW, flag);
}

static unsigned re1_empty_crest_slots(const PeReSession *s) {
    return 4u - (unsigned)(re1_use_done(s, PE_RE1_SUN_PLACED) + re1_use_done(s, PE_RE1_MOON_PLACED) +
                           re1_use_done(s, PE_RE1_STAR_PLACED) + re1_use_done(s, PE_RE1_WIND_PLACED));
}

static unsigned re1_bridge_crates(const PeReSession *s) {
    return (unsigned)(re1_use_done(s, PE_RE1_BRIDGE_COUNT_LOW) + 2 * re1_use_done(s, PE_RE1_BRIDGE_COUNT_HIGH));
}

/* Through door `d`: locked says why; else the next room, placed as the door says. */
static PeReChange take_door(PeReSession *s, const char *store, const PeImpDoor *d, char *why, size_t n) {
    const unsigned lock_byte = d->lock_id >> 3;
    const uint8_t previous_locks = s->progress.unlocked[lock_byte];
    s->said[0] = 0;
    /* The RE1 room script gates this exit with the crest panel, separately from key locks. */
    if (s->logic->game == PE_IMP_RE1 && s->stage == 0 && s->room == CREST_PASSAGE_ROOM &&
        d->room == CREST_GATE_ROOM && d->lock_id == CREST_GATE_LOCK && re1_empty_crest_slots(s)) {
        snprintf(s->said, sizeof s->said, "The gate is locked. Fit the four crests into the panel.");
        return PE_RE_SAID;
    }
    if (pe_re1_is_pool(s->logic) &&
        d->stage == PE_RE_STAGE_HERE && d->room == WATERFALL_ROOM && !re1_use_done(s, PE_RE1_POOL_DRAINED)) {
        snprintf(s->said, sizeof s->said, "The pool is full. The crank mechanism might lower the water.");
        return PE_RE_SAID;
    }
    if (s->logic->game == PE_IMP_RE1 && s->stage == PE_RE1_GUARDHOUSE_STAGE && s->room == DORM_003_ROOM &&
        d->room == PLANT_ROOM && d->zone.id == BOOKSHELF_DOOR && !re1_use_done(s, PE_RE1_RED_BOOK_PLACED)) {
        snprintf(s->said, sizeof s->said, "The bookshelf blocks the passage.");
        return PE_RE_SAID;
    }
    if (s->logic->game == PE_IMP_RE1 && s->stage == PE_RE1_GUARDHOUSE_STAGE) {
        if (s->room == DORM_002_ROOM && d->room == BASEMENT_ROOM && !re1_use_done(s, PE_RE1_DORM_STAIRS_OPEN)) {
            snprintf(s->said, sizeof s->said, "The shelves cover the stairs.");
            return PE_RE_SAID;
        }
        if (s->room == BASEMENT_ROOM && d->room == AQUARIUM_ROOM && re1_bridge_crates(s) != 3) {
            snprintf(s->said, sizeof s->said, "Push the crates into the water to make a bridge.");
            return PE_RE_SAID;
        }
        if (s->room == AQUARIUM_ROOM && d->room == SUPPLY_CLOSET && !re1_use_done(s, PE_RE1_AQUARIUM_DRAINED)) {
            snprintf(s->said, sizeof s->said, "The water pressure holds the door shut. Drain the tank from the control room.");
            return PE_RE_SAID;
        }
    }
    /* In RE1, key type zero permits passage even when the source door carries a lock ID. */
    if ((s->logic->game != PE_IMP_RE1 || d->lock_type) &&
        pe_re_door_try(&s->progress, d, s->logic, s->items, s->said, sizeof s->said) == PE_RE_DOOR_LOCKED) return PE_RE_SAID;
    const uint32_t stage = d->stage == PE_RE_STAGE_HERE ? s->stage : d->stage;
    if (!pe_re_session_enter(s, store, stage, d->room, d->camera, d->next_x, d->next_z, pe_re_facing_of(d->next_dir), why, n)) {
        s->floor = pe_re_floor_of(d->next_y);
        s->door = *d;
        s->doors_taken++;
        return PE_RE_ROOM;
    }
    s->progress.unlocked[lock_byte] = previous_locks;
    s->said[0] = 0;
    return PE_RE_STAY;
}

static PeReChange say_message(PeReSession *s, const uint8_t data[6]) {
    const uint32_t m = (uint32_t)data[0] | (uint32_t)data[1] << 8;
    return pe_re_message(s->logic, m, s->said, sizeof s->said) ? PE_RE_STAY : PE_RE_SAID;
}

/* Sce_Event: Event_exec(data word 0, data byte 3). The game stops the scheduler's zone checks while
 * an event plays (Stop_flg); that stop isn't kept here, so a zone set off every frame doesn't
 * restart an event that's still running (`again` 0). */
static void start_event(PeReSession *s, const uint8_t data[6], int again) {
    const unsigned level = (unsigned)data[0] | (unsigned)data[1] << 8, proc = data[3];
    if (!again && (level < PE_RE_TASKS ? s->run.task[level].status && s->run.task[level].proc == (int)proc
                                       : pe_re_scd_running(&s->run, proc)))
        return;
    pe_re_scd_exec(s->logic, &s->run, level, proc);
    s->events++;
}

/* What a zone does, by its kind (SCE_AT.C's handlers). */
static PeReChange zone_act(PeReSession *s, const char *store, const PeReZoneHit *h, int examine, char *why, size_t n) {
    s->zone = *h;
    switch (h->sce) {
    case PE_RE_ZONE_DOOR: {
        PeImpDoor d;
        if (h->from != PE_RE_FROM_DOOR || pe_imp_logic_door(s->logic, h->index, &d)) return PE_RE_STAY;
        return take_door(s, store, &d, why, n);
    }
    case PE_RE_ZONE_ITEM: {
        PeImpPickup pk;
        if (h->from != PE_RE_FROM_PICKUP || pe_imp_logic_pickup(s->logic, h->index, &pk)) return PE_RE_STAY;
        pe_re_take(&s->progress, s->logic, &pk, s->items, s->said, sizeof s->said);
        return PE_RE_SAID;
    }
    case PE_RE_ZONE_MESSAGE: return say_message(s, h->data);
    case PE_RE_ZONE_EVENT: start_event(s, h->data, examine); return PE_RE_STAY;
    case PE_RE_ZONE_FLAG:                                  /* Sce_Flg_chg: group, bit, on/off */
        pe_re_flag_set(&s->progress.flags, (unsigned)h->data[0] | (unsigned)h->data[1] << 8,
                       ((unsigned)h->data[2] | (unsigned)h->data[3] << 8) & 0xFF, h->data[4] | h->data[5]);
        return PE_RE_STAY;
    case PE_RE_ZONE_SAVE: case PE_RE_ZONE_ITEMBOX: case PE_RE_ZONE_STATUS:   /* for the mod: a typewriter,
                                                       an item box, the pocket to use something */
        return PE_RE_ZONE;
    default: return PE_RE_STAY;                            /* water, damage, drawers, ...: not yet */
    }
}

/* Item interactions use the examine zones and script triggers imported from each RDT. */
typedef struct {
    unsigned stage, room, zone, kind, item, done;
    int requires;
    ReHostPose pose;
    const char *hint, *result;
    int reusable;
} Re1ItemUse;

static const Re1ItemUse k_re1_item_uses[] = {
    {0, PE_RE1_TERRARIUM_ROOM, 1, 2, PE_RE1_CHEMICAL, PE_RE1_TERRARIUM_CLEARED, -1, RE_POSE_REACH,
     "The pump feeds water to the vines. Its lid is open.", "The chemical killed the vines.", 0},
    {0, PE_RE1_PIANO_ROOM, 2, 2, PE_RE1_MUSIC_NOTES, PE_RE1_PIANO_PLAYED, -1, RE_POSE_PIANO,
     "A piano. Sheet music might help.", "The music opened a hidden alcove.", 0},
    {0, PE_RE1_PIANO_ROOM, 10, 2, PE_RE1_WOOD_EMBLEM, PE_RE1_PIANO_EMBLEM_PLACED, PE_RE1_PIANO_PLAYED, RE_POSE_REACH,
     "The pedestal takes an emblem.", "The Wooden Emblem frees the Gold Emblem.", 0},
    {0, PE_RE1_DINING_ROOM, 13, 9, PE_RE1_GOLD_EMBLEM, PE_RE1_CLOCK_OPEN, -1, RE_POSE_REACH,
     "An emblem fits in the recess above the fireplace.", "The clock moved, revealing the Shield Key.", 0},
    {0, CREST_PASSAGE_ROOM, 3, 7, PE_RE1_SUN_CREST, PE_RE1_SUN_PLACED, -1, RE_POSE_REACH,
     NULL, "The Sun Crest fits into the panel.", 0},
    {0, CREST_PASSAGE_ROOM, 4, 7, PE_RE1_MOON_CREST, PE_RE1_MOON_PLACED, -1, RE_POSE_REACH,
     NULL, "The Moon Crest fits into the panel.", 0},
    {0, CREST_PASSAGE_ROOM, 5, 7, PE_RE1_STAR_CREST, PE_RE1_STAR_PLACED, -1, RE_POSE_REACH,
     NULL, "The Star Crest fits into the panel.", 0},
    {0, CREST_PASSAGE_ROOM, 6, 7, PE_RE1_WIND_CREST, PE_RE1_WIND_PLACED, -1, RE_POSE_REACH,
     NULL, "The Wind Crest fits into the panel.", 0},
    {COURTYARD_STAGE, POOL_ROOM, 4, 7, PE_RE1_SQUARE_CRANK, PE_RE1_POOL_DRAINED, -1, RE_POSE_REACH,
     "There is a square socket in the pool mechanism.", "The pool drained. The passage across is clear.", 1},
    {PE_RE1_GUARDHOUSE_STAGE, DORM_003_ROOM, 5, 9, PE_RE1_RED_BOOK, PE_RE1_RED_BOOK_PLACED, -1, RE_POSE_REACH,
     "One book is missing from the shelf.", "The shelf slid aside, revealing a passage.", 0},
};

static const Re1ItemUse *re1_item_use(const PeReSession *s, const PeImpTrigger *t) {
    for (size_t i = 0; i < sizeof k_re1_item_uses / sizeof *k_re1_item_uses; i++) {
        const Re1ItemUse *use = &k_re1_item_uses[i];
        if (s->stage == use->stage && s->room == use->room && t->zone.id == use->zone && t->zone.sce == use->kind &&
            (use->requires < 0 || re1_use_done(s, (unsigned)use->requires))) return use;
    }
    return NULL;
}

uint16_t pe_re1_item_for_trigger(const PeReSession *s, const PeImpTrigger *trigger) {
    const Re1ItemUse *use = re1_item_use(s, trigger);
    return use && !re1_use_done(s, use->done) ? (uint16_t)use->item : 0;
}

int pe_re1_piano_cuts(const PeImpRoomLogic *room, uint8_t cameras[PE_RE1_PIANO_CUTS]) {
    if (!room || room->game != PE_IMP_RE1 || room->stage != 0 || room->room != PE_RE1_PIANO_ROOM ||
        room->player != 1) return 0;
    PeRe1EventPlan plan;
    if (!pe_re1_event_plan(room, 6, &plan) || plan.count != PE_RE1_PIANO_CUTS) return 0;
    for (unsigned i = 0; i < plan.count; i++) cameras[i] = plan.cuts[i].camera;
    return 1;
}

static int re1_signed_word(const uint8_t *p) {
    const unsigned word = (unsigned)p[0] | (unsigned)p[1] << 8;
    return (int)word - (word >= 0x8000 ? 0x10000 : 0);
}

static void read_room_prop(ReHostRoomProp *prop, const uint8_t *ins, const char *asset_id) {
    snprintf(prop->asset_id, sizeof prop->asset_id, "%s", asset_id);
    prop->x = re1_signed_word(ins + 4);
    prop->y = re1_signed_word(ins + 6);
    prop->z = re1_signed_word(ins + 8);
    prop->yaw = re1_signed_word(ins + 10) & 4095;
}

static void read_piano_panel(void *ctx, uint32_t proc, uint32_t at, const uint8_t *ins, uint32_t size) {
    (void)proc;
    (void)at;
    /* OM_SET object 0, sliding panel type 0x35. Its model is section 2, entry 0. */
    if (size != 28 || ins[0] != 0x1f || ins[1] != 0 || ins[2] != 0x35) return;
    read_room_prop(ctx, ins, "re1:object/10f-0");
}

int pe_re1_piano_panel(const PeImpRoomLogic *room, ReHostRoomProp *panel) {
    if (!room || room->game != PE_IMP_RE1 || room->stage != 0 || room->room != PE_RE1_PIANO_ROOM ||
        room->player != 1) return 0;
    uint32_t bytes = 0;
    const uint8_t *rdt = pe_imp_logic_section(room, PE_IMP_LOGIC_RDT0, NULL, &bytes);
    if (!rdt) return 0;
    memset(panel, 0, sizeof *panel);
    pe_imp_scd_walk(PE_IMP_RE1, rdt, bytes, read_piano_panel, panel, NULL);
    return panel->asset_id[0] != 0;
}

typedef struct {
    const uint8_t *rdt;
    const char *asset;
    uint8_t object, type, bank, flag;
    ReHostRoomProp clear, set;
} Re1SlidingProp;

static void read_sliding_prop(void *ctx, uint32_t proc, uint32_t at, const uint8_t *ins, uint32_t size) {
    (void)proc;
    Re1SlidingProp *prop = ctx;
    if (size != 28 || ins[0] != 0x1f || ins[1] != prop->object || ins[2] != prop->type || at < 4) return;
    const uint8_t clear_check[] = {0x04, prop->bank, prop->flag, 0x00};
    static const uint8_t set_else[] = {0x02, 0x1e};
    if (!memcmp(prop->rdt + at - sizeof clear_check, clear_check, sizeof clear_check))
        read_room_prop(&prop->clear, ins, prop->asset);
    else if (prop->clear.asset_id[0] && !memcmp(prop->rdt + at - sizeof set_else, set_else, sizeof set_else))
        read_room_prop(&prop->set, ins, prop->asset);
}

static int sliding_prop(const PeImpRoomLogic *room, Re1SlidingProp *prop, ReHostRoomProp *clear, ReHostRoomProp *set) {
    uint32_t bytes = 0;
    prop->rdt = pe_imp_logic_section(room, PE_IMP_LOGIC_RDT0, NULL, &bytes);
    if (!prop->rdt) return 0;
    pe_imp_scd_walk(PE_IMP_RE1, prop->rdt, bytes, read_sliding_prop, prop, NULL);
    if (!prop->clear.asset_id[0] || !prop->set.asset_id[0]) return 0;
    *clear = prop->clear;
    *set = prop->set;
    return 1;
}

int pe_re1_dining_clock(const PeImpRoomLogic *room, ReHostRoomProp *closed, ReHostRoomProp *open) {
    if (!room || room->game != PE_IMP_RE1 || room->stage != 0 || room->room != PE_RE1_DINING_ROOM ||
        room->player != 1) return 0;
    Re1SlidingProp clock = {.asset = "re1:object/105-2", .object = 2, .type = 1, .bank = 1, .flag = 0xab};
    return sliding_prop(room, &clock, closed, open);
}

int pe_re1_bookshelf(const PeImpRoomLogic *room, ReHostRoomProp *closed, ReHostRoomProp *open) {
    if (!room || room->game != PE_IMP_RE1 || room->stage != PE_RE1_GUARDHOUSE_STAGE || room->room != DORM_003_ROOM)
        return 0;
    Re1SlidingProp shelf = {.asset = "re1:object/40a-0", .object = 0, .type = 0x31, .bank = 0, .flag = 0x25};
    /* The Red Book sequence clears flag 0:25 when it moves the shelf off the door. */
    return sliding_prop(room, &shelf, open, closed);
}

static void read_pool_water(void *ctx, uint32_t proc, uint32_t at, const uint8_t *ins, uint32_t size) {
    (void)proc;
    (void)at;
    /* OM_SET object 1, type 0x0F. The main water mesh uses translucent textured faces. */
    if (size != 28 || ins[0] != 0x1f || ins[1] != 1 || ins[2] != 0x0f) return;
    ReHostRoomProp *surface = ctx;
    read_room_prop(surface, ins, "re1:object/301-1");
    surface->translucent = 1;
}

int pe_re1_pool_water(const PeImpRoomLogic *room, ReHostRoomProp *surface) {
    if (!pe_re1_is_pool(room)) return 0;
    uint32_t bytes = 0;
    const uint8_t *rdt = pe_imp_logic_section(room, PE_IMP_LOGIC_RDT0, NULL, &bytes);
    if (!rdt) return 0;
    memset(surface, 0, sizeof *surface);
    pe_imp_scd_walk(PE_IMP_RE1, rdt, bytes, read_pool_water, surface, NULL);
    return surface->asset_id[0] != 0;
}

static PeReChange re1_pool_climb(PeReSession *s, double x, double z) {
    uint32_t count = 0;
    pe_imp_logic_section(s->logic, PE_IMP_LOGIC_TRIG, &count, NULL);
    double front_x, front_z;
    pe_re_front(x, z, s->facing, &front_x, &front_z);
    for (uint32_t i = 0; i < count; i++) {
        PeImpTrigger t;
        if (pe_imp_logic_trigger(s->logic, i, &t) || !pe_re1_is_pool_ladder(s->logic, &t) ||
            !pe_re_scd_live(&s->run, t.at)) continue;
        if (!pe_re_in_quad(t.zone.x, t.zone.z, x, z) && !pe_re_in_quad(t.zone.x, t.zone.z, front_x, front_z))
            continue;
        if (!re1_use_done(s, PE_RE1_POOL_DRAINED)) {
            snprintf(s->said, sizeof s->said, "The ladder leads down into the water. Drain the pool first.");
            return PE_RE_SAID;
        }
        double ladder_x = 0, ladder_z = 0;
        for (int corner = 0; corner < 4; corner++) {
            ladder_x += t.zone.x[corner] / 4.0;
            ladder_z += t.zone.z[corner] / 4.0;
        }
        const int far = t.zone.id == 3;
        const int descending = s->floor == 0;
        const int bank = far ? 1 : -1;
        s->floor = descending ? PE_RE1_POOL_FLOOR : 0;
        s->camera = far ? 5 : 1;
        pe_re_session_place(s, ladder_x, ladder_z + bank * (descending ? -780 : 850),
                            descending == far ? 0 : 2048);
        return PE_RE_CLIMB;
    }
    return PE_RE_STAY;
}

/* The room script exposes door 2 at the sliding shelf. Its original DOOR record
 * is parked at the origin, so use the shelf's examine zone to reach that door. */
static PeReChange re1_bookshelf_door(PeReSession *s, const char *store, char *why, size_t n) {
    uint32_t count = 0;
    pe_imp_logic_section(s->logic, PE_IMP_LOGIC_DOOR, &count, NULL);
    for (uint32_t i = 0; i < count; i++) {
        PeImpDoor door;
        if (!pe_imp_logic_door(s->logic, i, &door) && door.zone.id == BOOKSHELF_DOOR && door.room == PLANT_ROOM &&
            pe_re_scd_live(&s->run, door.at))
            return take_door(s, store, &door, why, n);
    }
    return PE_RE_STAY;
}

static int re1_basement_stairs(const PeImpRoomLogic *room, PeImpDoor *door) {
    PeRe1EventPlan event;
    if (!pe_re1_event_plan(room, 8, &event) || !event.door_setup) return 0;
    const uint8_t *op = event.door_setup;
    if (op[1] != 2 || op[15] != BASEMENT_ROOM) return 0;
    const int x = re1_signed_word(op + 2), z = re1_signed_word(op + 4);
    const int right = x + (op[6] | op[7] << 8), bottom = z + (op[8] | op[9] << 8);
    if (right <= x || bottom <= z || right > 32767 || bottom > 32767) return 0;
    *door = (PeImpDoor){
        .zone = {.id = 2, .sce = op[10], .x = {x, right, right, x}, .z = {z, z, bottom, bottom}},
        .stage = PE_RE_STAGE_HERE, .room = BASEMENT_ROOM, .animation = op[12],
        .next_x = re1_signed_word(op + 16), .next_y = re1_signed_word(op + 18),
        .next_z = re1_signed_word(op + 20), .next_dir = re1_signed_word(op + 22),
        .lock_id = op[14], .lock_type = op[24], .free = op[25]
    };
    return 1;
}

typedef struct { unsigned object; int found, x, z; } Re1CrateStart;

static void read_crate_start(void *ctx, uint32_t proc, uint32_t at, const uint8_t *op, uint32_t width) {
    (void)proc;
    (void)at;
    Re1CrateStart *crate = ctx;
    if (crate->found || width != 28 || op[0] != 0x1f || op[1] != crate->object || op[2] != 0x11) return;
    crate->x = re1_signed_word(op + 4);
    crate->z = re1_signed_word(op + 8);
    crate->found = 1;
}

static PeReChange re1_guardhouse_examine(PeReSession *s, const char *store, double x, double z,
                                        double front_x, double front_z, char *why, size_t n) {
    if (s->stage != PE_RE1_GUARDHOUSE_STAGE) return PE_RE_STAY;
    if (s->room == DORM_002_ROOM) {
        PeImpDoor stairs;
        if (!re1_basement_stairs(s->logic, &stairs)) return PE_RE_STAY;
        if (re1_use_done(s, PE_RE1_DORM_STAIRS_OPEN)) {
            if (pe_re_in_quad(stairs.zone.x, stairs.zone.z, x, z) ||
                pe_re_in_quad(stairs.zone.x, stairs.zone.z, front_x, front_z))
                return take_door(s, store, &stairs, why, n);
        } else if ((x >= 9900 && x <= 13000 && z >= 8000 && z <= 9150) ||
                   (front_x >= 9900 && front_x <= 13000 && front_z >= 8000 && front_z <= 9150)) {
            pe_re_flag_set(&s->progress.flags, PE_RE1_FG_PREVIEW, PE_RE1_DORM_STAIRS_OPEN, 1);
            snprintf(s->said, sizeof s->said, "You move the shelves and uncover a stairway.");
            return PE_RE_SAID;
        }
    } else if (s->room == BASEMENT_ROOM) {
        /* The three push events move objects 3, 1, and 2 in this order. */
        static const unsigned order[] = {3, 1, 2};
        const unsigned placed = re1_bridge_crates(s);
        if (placed >= sizeof order / sizeof *order) return PE_RE_STAY;
        Re1CrateStart crate = {.object = order[placed]};
        uint32_t bytes = 0;
        const uint8_t *rdt = pe_imp_logic_section(s->logic, PE_IMP_LOGIC_RDT0, NULL, &bytes);
        if (rdt) pe_imp_scd_walk(PE_IMP_RE1, rdt, bytes, read_crate_start, &crate, NULL);
        if (crate.found && front_x >= crate.x - 950 && front_x <= crate.x + 950 &&
            front_z >= crate.z - 950 && front_z <= crate.z + 950) {
            const unsigned next = placed + 1;
            pe_re_flag_set(&s->progress.flags, PE_RE1_FG_PREVIEW, PE_RE1_BRIDGE_COUNT_LOW, next & 1);
            pe_re_flag_set(&s->progress.flags, PE_RE1_FG_PREVIEW, PE_RE1_BRIDGE_COUNT_HIGH, next >> 1);
            static const char *const response[] = {
                "You push the first crate into the water. Move the next crate along this wall.",
                "You push the second crate into the water. The last one is by the opposite wall.",
                "The three crates form a bridge. You can reach the far door."
            };
            snprintf(s->said, sizeof s->said, "%s", response[placed]);
            return PE_RE_SAID;
        }
    }
    return PE_RE_STAY;
}

/* RE1 AOT_SET keeps its activation and message selector in the payload. */
static PeReChange re1_examine(PeReSession *s, const char *store, double x, double z, char *why, size_t n) {
    uint32_t count = 0;
    pe_imp_logic_section(s->logic, PE_IMP_LOGIC_TRIG, &count, NULL);
    double front_x, front_z;
    pe_re_front(x, z, s->facing, &front_x, &front_z);
    const PeReChange guardhouse = re1_guardhouse_examine(s, store, x, z, front_x, front_z, why, n);
    if (guardhouse != PE_RE_STAY || (why && n && *why)) return guardhouse;
    for (uint32_t i = 0; i < count; i++) {
        PeImpTrigger trigger;
        if (pe_imp_logic_trigger(s->logic, i, &trigger) || !pe_re_scd_live(&s->run, trigger.at)) continue;
        if (!pe_re_in_quad(trigger.zone.x, trigger.zone.z, x, z) &&
            !pe_re_in_quad(trigger.zone.x, trigger.zone.z, front_x, front_z)) continue;
        if (s->stage == PE_RE1_GUARDHOUSE_STAGE && s->room == DORM_003_ROOM &&
            trigger.zone.id == BOOKSHELF_TRIGGER && trigger.zone.sce == 9)
            return re1_bookshelf_door(s, store, why, n);
        if (s->stage == PE_RE1_GUARDHOUSE_STAGE && trigger.data[0] == 0x81) {
            if (s->room == PE_RE1_DORM_001_BATHROOM && trigger.zone.id == 3 && trigger.zone.sce == 0) {
                if (re1_use_done(s, PE_RE1_DORM_TUB_DRAINED)) continue;
                pe_re_flag_set(&s->progress.flags, PE_RE1_FG_PREVIEW, PE_RE1_DORM_TUB_DRAINED, 1);
                snprintf(s->said, sizeof s->said, "You drain the tub and spot a key.");
                return PE_RE_SAID;
            }
            if (s->room == CONTROL_ROOM && trigger.zone.id == 1 && trigger.zone.sce == 0) {
                if (re1_use_done(s, PE_RE1_AQUARIUM_DRAINED)) continue;
                pe_re_flag_set(&s->progress.flags, PE_RE1_FG_PREVIEW, PE_RE1_AQUARIUM_DRAINED, 1);
                snprintf(s->said, sizeof s->said, "You operate the drain controls. The water in the tank recedes.");
                return PE_RE_SAID;
            }
        }
        const Re1ItemUse *use = re1_item_use(s, &trigger);
        if (use) {
            if (use->room == CREST_PASSAGE_ROOM) {
                const unsigned empty = re1_empty_crest_slots(s);
                if (empty)
                    snprintf(s->said, sizeof s->said, "The crest panel has %u empty %s.", empty, empty == 1 ? "slot" : "slots");
                else
                    snprintf(s->said, sizeof s->said, "All four crests are in place. The gate is unlocked.");
            } else {
                snprintf(s->said, sizeof s->said, "%s", re1_use_done(s, use->done) ? use->result : use->hint);
            }
            return PE_RE_SAID;
        }
        const int kind = pe_re1_examine_kind(&trigger);
        if (!kind) continue;
        if (kind == PE_RE_ZONE_MESSAGE) {
            const unsigned index = trigger.data[1] & 0x7f;
            if (pe_re_message(s->logic, index, s->said, sizeof s->said)) continue;
            return PE_RE_SAID;
        }
        s->zone = (PeReZoneHit){.id = trigger.zone.id, .sce = (uint8_t)kind,
                               .from = PE_RE_FROM_TRIGGER, .index = i};
        return PE_RE_ZONE;
    }
    return PE_RE_STAY;
}

PeReChange pe_re_session_step(PeReSession *s, const char *store, double x, double z, int action, char *why, size_t n) {
    PeImpDoor d;
    PeImpPickup pk;
    PeReZoneHit hits[16];
    if (!s->game || !s->logic) return PE_RE_STAY;
    s->x = x, s->z = z;
    if (action && pe_re1_is_pool(s->logic)) {
        s->said[0] = 0;
        const PeReChange climb = re1_pool_climb(s, x, z);
        if (climb != PE_RE_STAY) return climb;
        if (s->floor == PE_RE1_POOL_FLOOR) action = 0;   /* Only ladders are reachable down here. */
    }
    if (action) {
        s->said[0] = 0;
        /* an examine: the game's own check (the point in front, or underfoot, by the zone's bits) */
        if (pe_re_zone_check(s->logic, &s->run, &s->progress, x, z, s->facing, s->floor, 1, s->progress.flags.work,
                             hits, 1) == 1) {
            const PeReChange c = zone_act(s, store, &hits[0], 1, why, n);
            if (c != PE_RE_STAY || s->said[0] || hits[0].sce == PE_RE_ZONE_EVENT || hits[0].sce == PE_RE_ZONE_FLAG)
                return c;
        }
        /* else, more forgiving than the game: a door, pickup or message zone she stands in */
        if (pe_re_door_at(s->logic, &s->run, x, z, &d) >= 0) return take_door(s, store, &d, why, n);
        double front_x = x, front_z = z;
        if (s->logic->game == PE_IMP_RE1) pe_re_front(x, z, s->facing, &front_x, &front_z);
        int pickup = pe_re_pickup_at(s->logic, &s->run, &s->progress, x, z, &pk);
        if (pickup < 0 && s->logic->game == PE_IMP_RE1)
            pickup = pe_re_pickup_at(s->logic, &s->run, &s->progress, front_x, front_z, &pk);
        if (pickup >= 0) {
            pe_re_take(&s->progress, s->logic, &pk, s->items, s->said, sizeof s->said);
            return PE_RE_SAID;
        }
        if (s->logic->game == PE_IMP_RE1) {
            /* Some thresholds sit inside the static wall. Check the point in
             * front so those doors can be opened from the walkable side. */
            if (pe_re_door_at(s->logic, &s->run, front_x, front_z, &d) >= 0)
                return take_door(s, store, &d, why, n);
            const PeReChange change = re1_examine(s, store, x, z, why, n);
            if (change != PE_RE_STAY) return change;
        } else {
            const int message = pe_re_message_at(s->logic, &s->run, x, z);
            if (message >= 0 && !pe_re_message(s->logic, (uint32_t)message, s->said, sizeof s->said)) return PE_RE_SAID;
        }
    }
    /* every frame: the zones that don't wait for an examine */
    const int nh = pe_re_zone_check(s->logic, &s->run, &s->progress, x, z, s->facing, s->floor, 0,
                                    s->progress.flags.work, hits, 16);
    int message_zone = -1;
    for (int i = 0; i < nh; i++) {
        if (hits[i].sce == PE_RE_ZONE_MESSAGE) {           /* once each time she walks into it */
            message_zone = hits[i].id;
            if (s->auto_message == hits[i].id || (s->talking && s->talking())) continue;
            s->auto_message = hits[i].id;
            if (say_message(s, hits[i].data) == PE_RE_SAID) return PE_RE_SAID;
            continue;
        }
        if (hits[i].sce == PE_RE_ZONE_ITEM || hits[i].sce == PE_RE_ZONE_SAVE || hits[i].sce == PE_RE_ZONE_ITEMBOX ||
            hits[i].sce == PE_RE_ZONE_STATUS)
            continue;                                      /* those only ever answer an examine */
        const PeReChange c = zone_act(s, store, &hits[i], 0, why, n);
        if (c != PE_RE_STAY) return c;
    }
    if (message_zone < 0) s->auto_message = -1;
    if (s->run.ran && s->run.camera_lock) return PE_RE_STAY;   /* the scripts hold the camera */
    const int camera = pe_re_camera_at(s->logic, s->camera, x, z);
    if (camera == s->camera) return PE_RE_STAY;
    s->camera = camera;
    return PE_RE_CAMERA;
}

PeReChange pe_re_session_tick(PeReSession *s) {
    if (!s->game || !s->logic) return PE_RE_STAY;
    const PeReScdHost host = host_of(s);
    s->run.camera = s->camera;
    pe_re_scd_tick(s->logic, &s->progress.flags, &host, &s->run);
    for (int i = 0; i < s->run.naot_on; i++) {             /* aot_on: a zone set off by its script */
        PeReZoneHit h;
        if (pe_re_zone_by_id(s->logic, &s->run, &s->progress, s->run.aot_on[i], &h)) continue;
        char why[8];
        const PeReChange c = zone_act(s, s->store, &h, 1, why, sizeof why);
        if (c == PE_RE_SAID || c == PE_RE_ZONE) { s->run.naot_on = 0; return c; }
    }
    s->run.naot_on = 0;
    if (s->run.ran && s->run.camera != s->camera) {        /* a script cut to another camera */
        s->camera = s->run.camera;
        return PE_RE_CAMERA;
    }
    const int m = s->run.message;
    s->run.message = -1;
    if (m >= 0 && !pe_re_message(s->logic, (uint32_t)m, s->said, sizeof s->said)) return PE_RE_SAID;
    return PE_RE_STAY;
}

static const Re1ItemUse *re1_item_use_at(const PeReSession *s, uint16_t item) {
    if (!s->logic || s->logic->game != PE_IMP_RE1) return NULL;
    uint32_t count = 0;
    pe_imp_logic_section(s->logic, PE_IMP_LOGIC_TRIG, &count, NULL);
    double fx, fz;
    pe_re_front(s->x, s->z, s->facing, &fx, &fz);
    for (uint32_t i = 0; i < count; i++) {
        PeImpTrigger t;
        if (pe_imp_logic_trigger(s->logic, i, &t) || !pe_re_scd_live(&s->run, t.at)) continue;
        const Re1ItemUse *use = re1_item_use(s, &t);
        if (use && use->item == item && !re1_use_done(s, use->done) &&
            (pe_re_in_quad(t.zone.x, t.zone.z, s->x, s->z) || pe_re_in_quad(t.zone.x, t.zone.z, fx, fz))) return use;
    }
    return NULL;
}

PeReChange pe_re_session_use(PeReSession *s, uint16_t item) {
    char name[48];
    s->use_pose = RE_POSE_IDLE;
    s->said[0] = 0;
    if (!s->game || !pe_re_has(&s->progress, item)) return PE_RE_STAY;
    const Re1ItemUse *use = re1_item_use_at(s, item);
    if (use) {
        if (!use->reusable) pe_re_lose(&s->progress, item, 1);
        pe_re_flag_set(&s->progress.flags, PE_RE1_FG_PREVIEW, use->done, 1);
        s->uses++;
        s->use_pose = use->pose;
        if (use->room == CREST_PASSAGE_ROOM && !re1_empty_crest_slots(s))
            snprintf(s->said, sizeof s->said, "%s The gate is unlocked.", use->result);
        else
            snprintf(s->said, sizeof s->said, "%s", use->result);
        return PE_RE_SAID;
    }
    if (!pe_re_flag(&s->progress.flags, PE_RE_FG_USE, item)) {   /* the status screen's "can't use it here" */
        pe_re_item_name(s->items, item, name, sizeof name);
        snprintf(s->said, sizeof s->said, "You can't use the %s here.", name);
        return PE_RE_SAID;
    }
    s->progress.flags.work[PE_RE_WORK_USE_ID] = (int16_t)item;  /* the room's script takes it from here */
    s->uses++;
    return PE_RE_STAY;
}

static unsigned enemy_group(const PeReSession *s) {        /* Sce_em_set's flag set */
    return s->stage >= 3 || pe_re_flag(&s->progress.flags, PE_RE_FG_STATUS, 6) ? PE_RE_FG_ENEMY + 1 : PE_RE_FG_ENEMY;
}

int pe_re_session_enemies(const PeReSession *s, PeReEnemyPlacement *out, int max) {
    uint32_t size = 0;
    int n = 0;
    if (!s->logic || !s->run.ran) return 0;
    const uint8_t *rdt = pe_imp_logic_section(s->logic, PE_IMP_LOGIC_RDT0, NULL, &size);
    for (int i = 0; rdt && i < s->run.nenemy && n < max; i++) {
        const uint32_t at = s->run.enemy[i];
        if (at + 22 > size) continue;
        const uint8_t *e = rdt + at;                        /* SCE_EM_SET_WORK, 22 bytes */
        if (e[9] != 0xFF && pe_re_flag(&s->progress.flags, enemy_group(s), e[9])) continue;   /* killed */
        PeReEnemyPlacement *p = &out[n++];
        p->slot = e[2], p->kind = e[3], p->behaviour = (uint16_t)(e[4] | e[5] << 8);
        p->floor = e[6], p->sound = e[7], p->model = e[8], p->killed_flag = e[9];
        p->x = (int16_t)(e[10] | e[11] << 8), p->y = (int16_t)(e[12] | e[13] << 8);
        p->z = (int16_t)(e[14] | e[15] << 8), p->dir = (int16_t)(e[16] | e[17] << 8);
        p->motion = (int16_t)(e[18] | e[19] << 8), p->ctr = (int16_t)(e[20] | e[21] << 8);
    }
    return n;
}

int pe_re_session_defeated(PeReSession *s, uint8_t killed_flag) {
    if (!s->game || killed_flag == 0xFF) return -1;
    pe_re_flag_set(&s->progress.flags, enemy_group(s), killed_flag, 1);
    return 0;
}

int pe_re_enemy_lists_equal(const PeReEnemyPlacement *a, const PeReEnemyPlacement *b, int count) {
    for (int i = 0; i < count; i++, a++, b++) {
        if (a->slot != b->slot || a->kind != b->kind || a->behaviour != b->behaviour ||
            a->floor != b->floor || a->sound != b->sound || a->model != b->model ||
            a->killed_flag != b->killed_flag || a->x != b->x || a->y != b->y ||
            a->z != b->z || a->dir != b->dir || a->motion != b->motion || a->ctr != b->ctr)
            return 0;
    }
    return 1;
}

void pe_re_session_end(PeReSession *s) {
    free(s->logic);
    free(s->items);
    memset(s, 0, sizeof *s);
}
