/* The play session (mods/pe_re_play/pe_re_session.c), without the game: zones, camera switches and
 * doors on a made-up room; then, with PE_TEST_RE2_DISC, RE2 imported and walked door by door from
 * the street: every door must lead to an imported room that has the camera it names, and should
 * put the player on free floor. PE_TEST_RE1_DISC exercises Jill's mansion hall and an exit. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pe_imp.h"
#include "pe_imp_internal.h"
#include "pe_imp_logic.h"
#include "pe_imp_rdt.h"
#include "pe_imp_room.h"
#include "pe_re_save.h"
#include "pe_re_session.h"
#include "pe_src_re1.h"
#include "pe_src_re2.h"

int re_headless_smoke(const char *store, unsigned scenario);

static int s_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); s_fail = 1; } } while (0)

static void rect(int16_t x[4], int16_t z[4], int x0, int z0, int x1, int z1) {
    x[0] = (int16_t)x0, z[0] = (int16_t)z0, x[1] = (int16_t)x1, z[1] = (int16_t)z0;
    x[2] = (int16_t)x1, z[2] = (int16_t)z1, x[3] = (int16_t)x0, z[3] = (int16_t)z1;
}

#define ROOMS_MAX 512
static const char *s_store = "pe-re-session-test/store";

static void re1_check_branches(void) {
    /* IF Jill, define the first trigger; ELSE, define the alternate trigger. */
    uint8_t rdt[196] = {
        [0x60] = 148, [148] = 48,
        [150] = 1, [151] = 24,
        [152] = 4, [155] = 1,
        [156] = 0x0d,
        [174] = 2, [175] = 20,
        [176] = 0x0d
    };
    PeImpTrigger triggers[] = {
        {.zone.id = 1, .zone.sce = 0x10, .data = {0x81}, .at = 156},
        {.zone.id = 2, .zone.sce = 0x08, .data = {0x81}, .at = 176}
    };
    for (unsigned i = 0; i < 2; i++)
        rect(triggers[i].zone.x, triggers[i].zone.z, 0, 0, 1000, 1000);

    for (unsigned player = 0; player < 2; player++) {
        PeImpLogicBuilder *builder = pe_imp_logic_begin(PE_IMP_RE1, player, 0, 0);
        CHECK(builder && !pe_imp_logic_add(builder, PE_IMP_LOGIC_RDT0, 0, rdt, sizeof rdt));
        CHECK(!pe_imp_logic_add_trigger(builder, triggers, 2));
        CHECK(!pe_imp_logic_add_origins(builder, NULL, 0, triggers, 2, NULL, 0));
        PeReSession session = {.game = pe_re_game_named("re1"), .logic = pe_imp_logic_finish(builder)};
        pe_re_scd_run(session.logic, NULL, NULL, &session.run);
        CHECK(pe_re_scd_live(&session.run, 156) == (player == 1));
        CHECK(pe_re_scd_live(&session.run, 176) == (player == 0));
        char why[80];
        CHECK(pe_re_session_step(&session, s_store, 500, 500, 1, why, sizeof why) == PE_RE_ZONE);
        CHECK(session.zone.sce == (player ? PE_RE_ZONE_SAVE : PE_RE_ZONE_ITEMBOX));
        pe_re_session_end(&session);
    }

    rdt[153] = 4;                    /* unimplemented flag bank: keep both paths */
    PeImpLogicBuilder *builder = pe_imp_logic_begin(PE_IMP_RE1, 1, 0, 0);
    CHECK(builder && !pe_imp_logic_add(builder, PE_IMP_LOGIC_RDT0, 0, rdt, sizeof rdt));
    PeImpRoomLogic *logic = pe_imp_logic_finish(builder);
    PeReScdRun run;
    pe_re_scd_run(logic, NULL, NULL, &run);
    CHECK(pe_re_scd_live(&run, 156) && pe_re_scd_live(&run, 176));
    free(logic);
}

/* Steps on the imported door's action zone without changing rooms through the debug API. */
static int re1_route_door(PeReSession *s, const char *store, unsigned target, PeReChange expected) {
    uint32_t count = 0;
    pe_imp_logic_section(s->logic, PE_IMP_LOGIC_DOOR, &count, NULL);
    for (uint32_t i = 0; i < count; i++) {
        PeImpDoor door;
        if (pe_imp_logic_door(s->logic, i, &door) || !pe_re_scd_live(&s->run, door.at)) continue;
        const unsigned stage = door.stage == PE_RE_STAGE_HERE ? s->stage : door.stage;
        if (((stage + 1) << 8 | door.room) != target) continue;
        double x = 0, z = 0;
        for (int k = 0; k < 4; k++) x += door.zone.x[k] / 4.0, z += door.zone.z[k] / 4.0;
        char why[200] = "";
        const PeReChange change = pe_re_session_step(s, store, x, z, 1, why, sizeof why);
        if (change == expected && expected != PE_RE_ROOM) return 1;
        if (change == PE_RE_ROOM && expected == PE_RE_ROOM && s->stage == stage && s->room == door.room) {
            char id[PE_IMP_ID_MAX];
            pe_re_room_id("re1", s->stage, s->room, id, sizeof id);
            PeImpImageSet *set = NULL;
            PeImpRoomView view;
            if (!pe_imp_catalog_imageset(store, id, &set, why, sizeof why)) {
                const int ok = !pe_imp_room_view(set, (uint32_t)s->camera, &view);
                free(set);
                if (ok) return 1;
                snprintf(why, sizeof why, "missing destination camera %d", s->camera);
            }
        }
        printf("RE1 door to %03x: change %d, room %x%02x, %s %s\n",
               target, change, (unsigned)s->stage + 1, (unsigned)s->room, s->said, why);
        return 0;
    }
    printf("RE1 door to %03x missing from %x%02x\n", target, (unsigned)s->stage + 1, (unsigned)s->room);
    return 0;
}

static int re1_route_walk(PeReSession *s, const char *store, const unsigned *rooms, size_t count) {
    for (size_t i = 0; i < count; i++)
        if (!re1_route_door(s, store, rooms[i], PE_RE_ROOM)) return 0;
    return 1;
}

static int re1_armor_route(PeReSession *s, const char *store) {
    static const unsigned to_chemical[] = {0x203, 0x204, 0x206, 0x207, 0x10b, 0x118};
    static const unsigned to_pump[] = {0x10b, 0x207, 0x204, 0x203, 0x202, 0x201, 0x101, 0x103, 0x10c};
    static const unsigned to_hall[] = {0x103, 0x104, 0x105, 0x106};
    char why[200] = "";
    if (!re1_route_door(s, store, 0x111, PE_RE_SAID) || !strstr(s->said, "needs the Armor Key")) return 0;
    if (!re1_route_walk(s, store, to_chemical, sizeof to_chemical / sizeof *to_chemical)) return 0;
    if (pe_re_session_step(s, store, 3160, 3369, 1, why, sizeof why) != PE_RE_SAID ||
        !pe_re_has(&s->progress, PE_RE1_CHEMICAL)) return 0;
    if (!re1_route_walk(s, store, to_pump, sizeof to_pump / sizeof *to_pump)) return 0;

    CHECK(pe_re_pickup_at(s->logic, &s->run, &s->progress, 5400, 5500, NULL) < 0);
    pe_re_session_step(s, store, 5400, 5500, 0, why, sizeof why);
    CHECK(pe_re_session_use(s, PE_RE1_CHEMICAL) == PE_RE_SAID && pe_re_has(&s->progress, PE_RE1_CHEMICAL));
    CHECK(pe_re_session_step(s, store, 16200, 5400, 1, why, sizeof why) == PE_RE_SAID && strstr(s->said, "pump"));
    CHECK(pe_re_session_use(s, PE_RE1_CHEMICAL) == PE_RE_SAID && !pe_re_has(&s->progress, PE_RE1_CHEMICAL));
    if (pe_re_session_step(s, store, 5400, 5500, 1, why, sizeof why) != PE_RE_SAID ||
        !pe_re_has(&s->progress, PE_RE1_ARMOR_KEY)) return 0;
    if (!re1_route_walk(s, store, to_hall, sizeof to_hall / sizeof *to_hall) ||
        !re1_route_door(s, store, 0x111, PE_RE_ROOM) || !strstr(s->said, "Used the Armor Key") ||
        s->logic->player != 1 || !re1_route_door(s, store, 0x106, PE_RE_ROOM)) return 0;

    CHECK(pe_re_session_step(s, store, 14000, 17000, 1, why, sizeof why) == PE_RE_SAID);
    CHECK(pe_re_session_step(s, store, 14000, 17000, 1, why, sizeof why) == PE_RE_ZONE && s->zone.sce == PE_RE_ZONE_SAVE);
    uint8_t bytes[PE_RE_SAVE_SIZE];
    PeReSaved saved;
    PeReSession resumed;
    pe_re_save_encode(s, 1, bytes);
    if (pe_re_save_decode(bytes, &saved) || pe_re_session_resume(&resumed, store, &saved, why, sizeof why)) return 0;
    CHECK(resumed.room == 6 && resumed.player == 1 && pe_re_has(&resumed.progress, PE_RE1_ARMOR_KEY));
    CHECK(pe_re_pickup_at(resumed.logic, &resumed.run, &resumed.progress, 14000, 17000, NULL) < 0);
    CHECK(re1_route_door(&resumed, store, 0x111, PE_RE_ROOM) && !resumed.said[0]);
    static const unsigned back_to_pump[] = {0x106, 0x105, 0x104, 0x103, 0x10c};
    if (re1_route_walk(&resumed, store, back_to_pump, sizeof back_to_pump / sizeof *back_to_pump)) {
        CHECK(pe_re_pickup_at(resumed.logic, &resumed.run, &resumed.progress, 5400, 5500, NULL) < 0);
        CHECK(pe_re_flag(&resumed.progress.flags, PE_RE1_FG_PREVIEW, PE_RE1_TERRARIUM_CLEARED));
    } else CHECK(0);
    pe_re_session_end(&resumed);
    printf("RE1 Jill: mansion hall to Chemical, Terrarium, Armor Key, keyed door and save reload\n");
    return 1;
}

static int re1_shield_route(PeReSession *s, const char *store) {
    static const unsigned to_piano[] = {0x104, 0x10f};
    static const unsigned to_dining[] = {0x104, 0x105};
    static const unsigned to_attic[] = {0x106, 0x203, 0x204, 0x20d, 0x20e};
    char why[200] = "";
    if (!re1_route_door(s, store, 0x105, PE_RE_ROOM)) return 0;
    CHECK(pe_re_pickup_at(s->logic, &s->run, &s->progress, 17900, 15000, NULL) < 0);
    CHECK(pe_re_session_step(s, store, 2900, 8100, 1, why, sizeof why) == PE_RE_SAID &&
          pe_re_has(&s->progress, PE_RE1_WOOD_EMBLEM));

    if (!re1_route_walk(s, store, to_piano, sizeof to_piano / sizeof *to_piano)) return 0;
    CHECK(strstr(s->said, "Lock Pick"));
    CHECK(pe_re_pickup_at(s->logic, &s->run, &s->progress, 8850, 19300, NULL) < 0);
    pe_re_session_place(s, 8850, 19300, 0);
    CHECK(pe_re_session_use(s, PE_RE1_WOOD_EMBLEM) == PE_RE_SAID && pe_re_has(&s->progress, PE_RE1_WOOD_EMBLEM));
    CHECK(pe_re_session_step(s, store, 17400, 12000, 1, why, sizeof why) == PE_RE_SAID &&
          pe_re_has(&s->progress, PE_RE1_MUSIC_NOTES));
    CHECK(pe_re_session_use(s, PE_RE1_MUSIC_NOTES) == PE_RE_SAID && pe_re_has(&s->progress, PE_RE1_MUSIC_NOTES));
    pe_re_session_place(s, 10500, 8050, 0);
    CHECK(pe_re_session_use(s, PE_RE1_MUSIC_NOTES) == PE_RE_SAID && !pe_re_has(&s->progress, PE_RE1_MUSIC_NOTES));
    CHECK(pe_re_pickup_at(s->logic, &s->run, &s->progress, 8850, 19300, NULL) < 0);
    pe_re_session_place(s, 8850, 19300, 0);
    CHECK(pe_re_session_use(s, PE_RE1_WOOD_EMBLEM) == PE_RE_SAID && !pe_re_has(&s->progress, PE_RE1_WOOD_EMBLEM));
    CHECK(pe_re_session_step(s, store, 8850, 19300, 1, why, sizeof why) == PE_RE_SAID &&
          pe_re_has(&s->progress, PE_RE1_GOLD_EMBLEM));

    if (!re1_route_walk(s, store, to_dining, sizeof to_dining / sizeof *to_dining)) return 0;
    pe_re_session_place(s, 2900, 8100, 0);
    CHECK(pe_re_session_use(s, PE_RE1_GOLD_EMBLEM) == PE_RE_SAID && !pe_re_has(&s->progress, PE_RE1_GOLD_EMBLEM));
    CHECK(pe_re_session_step(s, store, 17900, 15000, 1, why, sizeof why) == PE_RE_SAID &&
          pe_re_has(&s->progress, PE_RE1_SHIELD_KEY));
    if (!re1_route_walk(s, store, to_attic, sizeof to_attic / sizeof *to_attic) ||
        !re1_route_door(s, store, 0x210, PE_RE_ROOM)) return 0;
    CHECK(strstr(s->said, "Shield Key") || strstr(s->said, "Sheild Key"));

    uint8_t bytes[PE_RE_SAVE_SIZE];
    PeReSaved saved;
    pe_re_save_encode(s, 1, bytes);
    CHECK(!pe_re_save_decode(bytes, &saved) && pe_re_has(&saved.progress, PE_RE1_SHIELD_KEY) &&
          pe_re_flag(&saved.progress.flags, PE_RE1_FG_PREVIEW, PE_RE1_CLOCK_OPEN) &&
          pe_re_flag(&saved.progress.flags, PE_RE1_FG_PREVIEW, PE_RE1_PIANO_EMBLEM_PLACED));
    PeReSession resumed;
    if (pe_re_session_resume(&resumed, store, &saved, why, sizeof why)) return 0;
    CHECK(re1_route_door(&resumed, store, 0x20e, PE_RE_ROOM) &&
          re1_route_door(&resumed, store, 0x210, PE_RE_ROOM) && !resumed.said[0]);
    pe_re_session_end(&resumed);
    printf("RE1 Jill: Lock Pick, Piano Bar, emblem exchange, Shield Key and attic door reload\n");
    return 1;
}

/* Jill's mansion hall exercises the RE1 importer through the portable session. The same
 * background is shared with Chris, but the door and collision records are Jill's variant. */
static void re1_mansion(const char *cue) {
    static const char store[] = "re1-jill-session-test/store";
    PeImpScan scan;
    pe_imp_registry_reset();
    if (pe_imp_add_reader(&g_re1_rooms_ps1) || pe_imp_add_reader(&g_re1_logic_ps1) ||
        pe_imp_add_reader(&g_re1_items_ps1) ||
        pe_imp_scan(store, cue, NULL, &scan)) {
        CHECK(0 && "RE1 mansion import failed");
        return;
    }

    const PeReGame *game = pe_re_game_named("re1");
    PeReSession session;
    char why[200] = "";
    PeImpRoomLogic *chris = NULL;
    CHECK(!pe_imp_catalog_roomlogic(store, "re1:logic/106-0", &chris, NULL, 0));
    free(chris);
    CHECK(pe_re_imported(store, game));
    if (pe_re_session_start(&session, store, game, 1, why, sizeof why)) {
        printf("RE1 mansion start: %s\n", why);
        CHECK(0);
        return;
    }
    CHECK(session.logic->game == PE_IMP_RE1 && session.logic->player == 1);
    CHECK(session.stage == 0 && session.room == 6 && session.camera == 0);
    uint32_t bytes = 0;
    const uint8_t *raw = pe_imp_logic_section(session.logic, PE_IMP_LOGIC_COLL, NULL, &bytes);
    PeRe2RoomCollision walls;
    CHECK(!pe_imp_re1_collision(raw, bytes, &walls));
    CHECK(walls.count > 20 && !pe_imp_re2_blocked_on(&walls, 0, 12000, 22000, 280));
    CHECK(pe_imp_re2_blocked_on(&walls, 0, 19500, 22000, 280));
    double walk_x = 12000, walk_z = 22000;
    pe_imp_re2_slide_on(&walls, 0, &walk_x, &walk_z, 8000, 0, 280);
    CHECK(walk_x > 12000 && walk_x < 14500 && walk_z == 22000);
    CHECK(bytes >= 12 && pe_imp_re1_collision(raw, bytes - 12, &walls) < 0 && walls.count == 0);

    PeImpImageSet *set = NULL;
    CHECK(!pe_imp_catalog_imageset(store, "re1:room/106", &set, why, sizeof why));
    if (set) {
        PeImpRoomView view;
        CHECK(!pe_imp_room_view(set, 0, &view) && view.game == PE_IMP_RE1 && view.slot == 0);
        CHECK(pe_imp_room_projection(&view) == 230);
        free(set);
    }

    uint32_t cuts = 0, doors = 0;
    pe_imp_logic_section(session.logic, PE_IMP_LOGIC_CAMZ, &cuts, NULL);
    pe_imp_logic_section(session.logic, PE_IMP_LOGIC_DOOR, &doors, NULL);
    int switched = 0;
    for (uint32_t i = 0; i < cuts && !switched; i++) {
        PeImpCameraZone zone;
        if (pe_imp_logic_camera_zone(session.logic, i, &zone) || zone.flag == 1 || zone.from == zone.to)
            continue;
        double x = 0, z = 0;
        for (int c = 0; c < 4; c++) {
            x += zone.x[c] / 4.0;
            z += zone.z[c] / 4.0;
        }
        session.camera = zone.from;
        switched = pe_re_session_step(&session, store, x, z, 0, why, sizeof why) == PE_RE_CAMERA;
        if (switched) CHECK(session.camera == zone.to);
    }
    CHECK(switched);

    int entered = 0;
    for (uint32_t i = 0; i < doors && !entered; i++) {
        PeImpDoor door;
        if (pe_imp_logic_door(session.logic, i, &door) || door.room != 5 || door.lock_id || door.lock_type) continue;
        double x = 0, z = 0;
        for (int c = 0; c < 4; c++) {
            x += door.zone.x[c] / 4.0;
            z += door.zone.z[c] / 4.0;
        }
        entered = pe_re_session_step(&session, store, x, z, 1, why, sizeof why) == PE_RE_ROOM;
        if (entered) {
            CHECK(session.logic->player == 1 && session.doors_taken == 1);
            CHECK(session.room == session.door.room && session.room == 5);
            raw = pe_imp_logic_section(session.logic, PE_IMP_LOGIC_COLL, NULL, &bytes);
            CHECK(!pe_imp_re1_collision(raw, bytes, &walls));
            CHECK(!pe_imp_re2_blocked_on(&walls, 0, session.x, session.z, 280));
            char id[PE_IMP_ID_MAX];
            pe_re_room_id("re1", session.stage, session.room, id, sizeof id);
            set = NULL;
            CHECK(!pe_imp_catalog_imageset(store, id, &set, why, sizeof why));
            if (set) {
                CHECK(session.camera >= 0 && (uint32_t)session.camera < set->count);
                free(set);
            }
            printf("RE1 Jill: mansion hall to %s, camera %d\n", id, session.camera);
        }
    }
    if (!entered) printf("RE1 mansion door: %u records; %s\n", doors, why);
    CHECK(entered);
    if (session.stage == 0 && session.room == 5) {
        CHECK(pe_re_session_step(&session, store, 12500, 3300, 0, why, sizeof why) != PE_RE_SAID);
        CHECK(pe_re_session_step(&session, store, 12500, 3300, 1, why, sizeof why) == PE_RE_SAID);
        CHECK(strstr(session.said, "nothing inside") ||
              !strcmp(session.said, "This message uses an unsupported text encoding."));
        printf("RE1 Jill examine: %s\n", session.said);
    }
    CHECK(!pe_re_session_enter(&session, store, 0, 6, 0, 14000, 17000, 0, why, sizeof why));
    PeImpPickup ribbon;
    CHECK(pe_re_pickup_at(session.logic, &session.run, &session.progress, 14000, 17000, &ribbon) >= 0 &&
          ribbon.item == PE_RE1_INK_RIBBON && ribbon.flag == 0x34);
    CHECK(pe_re_session_step(&session, store, 14000, 17000, 1, why, sizeof why) == PE_RE_SAID);
    CHECK(pe_re_has(&session.progress, PE_RE1_INK_RIBBON));
    CHECK(pe_re_session_step(&session, store, 14000, 17000, 1, why, sizeof why) == PE_RE_ZONE);
    CHECK(session.zone.sce == PE_RE_ZONE_SAVE);
    CHECK(pe_re_session_step(&session, store, 17050, 17200, 1, why, sizeof why) == PE_RE_ROOM);
    CHECK(session.stage == 1 && session.room == 3 && pe_re_door_is_stairway(session.logic, &session.door));
    const uint8_t *upper = pe_imp_logic_section(session.logic, PE_IMP_LOGIC_COLL, NULL, &bytes);
    CHECK(!pe_imp_re1_collision(upper, bytes, &walls));
    CHECK(!pe_imp_re2_blocked_on(&walls, 0, session.x, session.z, 280));
    CHECK(pe_re_session_step(&session, store, 17000, 22500, 1, why, sizeof why) == PE_RE_ROOM);
    CHECK(session.stage == 0 && session.room == 6 && pe_re_door_is_stairway(session.logic, &session.door));
    CHECK(!pe_re_session_enter(&session, store, 0, 0, 0, 7200, 2800, 0, why, sizeof why));
    CHECK(pe_re_session_step(&session, store, 7200, 2800, 1, why, sizeof why) == PE_RE_ZONE);
    CHECK(session.zone.sce == PE_RE_ZONE_ITEMBOX);
    printf("RE1 Jill: examine, typewriter, item box, and two-way mansion stairs\n");
    pe_re_session_end(&session);

    if (!pe_re_session_start(&session, store, game, 1, why, sizeof why)) {
        const int armor_route = re1_armor_route(&session, store);
        CHECK(armor_route);
        if (armor_route) CHECK(re1_shield_route(&session, store));
        pe_re_session_end(&session);
    } else CHECK(0);

    /* Jill's room 40F registers two triggers under the character flag check. */
    PeImpRoomLogic *variant = NULL;
    CHECK(!pe_imp_catalog_roomlogic(store, "re1:logic/40f-1", &variant, why, sizeof why));
    if (variant) {
        uint32_t count = 0;
        unsigned checked = 0;
        pe_imp_logic_section(variant, PE_IMP_LOGIC_TRIG, &count, NULL);
        PeReScdRun jill, chris;
        pe_re_scd_run(variant, NULL, NULL, &jill);
        variant->player = 0;
        pe_re_scd_run(variant, NULL, NULL, &chris);
        for (uint32_t i = 0; i < count; i++) {
            PeImpTrigger trigger;
            if (pe_imp_logic_trigger(variant, i, &trigger) || trigger.zone.sce != 7 ||
                (trigger.zone.id != 2 && trigger.zone.id != 3)) continue;
            CHECK(pe_re_scd_live(&jill, trigger.at) && !pe_re_scd_live(&chris, trigger.at));
            checked++;
        }
        CHECK(checked == 2);
        free(variant);
    }
}

static void re1_check_text(void) {
    const uint8_t messages[] = {
        6, 0, 10, 0, 15, 0,
        0x1d, 0x79, 0x01, 0x00,             /* A. */
        0x1d, 0xf9, 0xa2, 0x01, 0x00,       /* extended encoding after an English glyph */
        0x04, 0xf9, 0x01, 0x00              /* cue, no display text */
    };
    uint8_t rdt[148 + sizeof messages] = {[0x74] = 148};
    memcpy(rdt + 148, messages, sizeof messages);
    PeImpRoomLogic *logic = pe_imp_rdt_logic(PE_IMP_RE1, 1, 0, 6, rdt, sizeof rdt, NULL, 0);
    CHECK(logic);
    if (!logic) return;

    uint32_t bytes;
    const uint8_t *raw = pe_imp_logic_section(logic, PE_IMP_LOGIC_MSG0, NULL, &bytes);
    CHECK(raw && bytes == sizeof messages && !memcmp(raw, messages, bytes));
    char say[80];
    CHECK(!pe_re_message(logic, 0, say, sizeof say) && !strcmp(say, "A."));
    CHECK(!pe_re_message(logic, 1, say, sizeof say) &&
          !strcmp(say, "This message uses an unsupported text encoding."));
    CHECK(pe_re_message(logic, 2, say, sizeof say));
    free(logic);
}

static void re1_check_pickups(void) {
    uint8_t rdt[202] = {
        [0x60] = 148, [148] = 54,
        [150] = 0x18, [151] = 1, [156] = 100, [158] = 100, [160] = 47, [161] = 3,
        [176] = 0x18, [177] = 2, [178] = 200, [182] = 100, [184] = 100,
        [186] = 53, [187] = 1, [198] = 0x34
    };
    PeImpRoomLogic *logic = pe_imp_rdt_logic(PE_IMP_RE1, 1, 0, 6, rdt, sizeof rdt, NULL, 0);
    CHECK(logic);
    if (!logic) return;

    PeReProgress progress = {0};
    PeImpPickup first, second;
    char say[80];
    CHECK(!pe_imp_logic_pickup(logic, 0, &first) && first.flag == 0);
    CHECK(!pe_imp_logic_pickup(logic, 1, &second) && second.flag == 0x34);
    pe_re_flag_set(&progress.flags, PE_RE_FG_ITEM, 0, 1);   /* ambiguous bit from an older save */
    CHECK(!pe_re_pickup_gone(logic, NULL, &progress, &first));
    CHECK(!pe_re_take(&progress, logic, &first, NULL, say, sizeof say));
    CHECK(pe_re_pickup_at(logic, NULL, &progress, 50, 50, NULL) < 0);
    CHECK(pe_re_pickup_at(logic, NULL, &progress, 250, 50, &second) == 1);
    CHECK(!pe_re_take(&progress, logic, &second, NULL, say, sizeof say));
    CHECK(pe_re_pickup_at(logic, NULL, &progress, 250, 50, NULL) < 0);

    progress.ntaken = 0;   /* the save retains flags rather than the in-memory pickup list */
    CHECK(pe_re_pickup_gone(logic, NULL, &progress, &first));
    CHECK(pe_re_pickup_gone(logic, NULL, &progress, &second));
    pe_re_flag_set(&progress.flags, PE_RE_FG_ITEM, second.flag, 0);
    progress.taken[0] = 6u << 16;   /* room-only entry from an earlier preview save */
    progress.ntaken = 1;
    CHECK(pe_re_pickup_gone(logic, NULL, &progress, &second));

    PeImpLogicBuilder *legacy = pe_imp_logic_begin(PE_IMP_RE1, 1, 0, 6);
    second.flag = 0;
    CHECK(legacy && !pe_imp_logic_add_pickup(legacy, &second, 1) &&
          !pe_imp_logic_add_origins(legacy, NULL, 0, NULL, 0, &second, 1) &&
          !pe_imp_logic_add(legacy, PE_IMP_LOGIC_RDT0, 0, rdt, sizeof rdt));
    PeImpRoomLogic *old_logic = pe_imp_logic_finish(legacy);
    CHECK(old_logic && !pe_imp_logic_pickup(old_logic, 0, &second) && second.flag == 0x34);
    free(old_logic);
    free(logic);
}

int main(void) {
    re1_check_branches();
    re1_check_text();
    re1_check_pickups();
    char why[200] = "";

    /* A short RE1 wall can straddle the signed 16-bit boundary. It must
     * block the edge and leave the middle of the room clear. */
    {
        const uint8_t sca[36] = {
            [4] = 1,
            [24] = 0xA0, [25] = 0x86,  /* x = 34464 modulo 65536 */
            [26] = 0xE8, [27] = 0x03,  /* z = 1000 */
            [28] = 0x00, [29] = 0x7D,  /* x = 32000 */
            [32] = 1
        };
        PeRe2RoomCollision collision;
        CHECK(!pe_imp_re1_collision(sca, sizeof sca, &collision) && collision.count == 1);
        CHECK(!pe_imp_re2_blocked_on(&collision, 0, 16000, 500, 280));
        CHECK(pe_imp_re2_blocked_on(&collision, 0, -31500, 500, 280));
    }

    /* A script can replace or move a placement without changing the list length. */
    {
        const PeReEnemyPlacement original[] = {{.slot = 1, .kind = 3, .killed_flag = 8, .x = 20}};
        PeReEnemyPlacement updated[] = {original[0]};
        CHECK(pe_re_enemy_lists_equal(original, updated, 1));
        updated[0].x = 21;
        CHECK(!pe_re_enemy_lists_equal(original, updated, 1));
        updated[0] = original[0];
        updated[0].kind = 4;
        CHECK(!pe_re_enemy_lists_equal(original, updated, 1));
        CHECK(pe_re_enemy_lists_equal(original, updated, 0));
    }

    /* zones: a square and a slanted four-corner zone */
    int16_t x[4], z[4];
    rect(x, z, 0, 0, 1000, 1000);
    CHECK(pe_re_in_quad(x, z, 500, 500) && !pe_re_in_quad(x, z, 1500, 500) && !pe_re_in_quad(x, z, 500, -1));
    const int16_t sx[4] = {0, 1000, 1500, 500}, sz[4] = {0, 0, 1000, 1000};
    CHECK(pe_re_in_quad(sx, sz, 1200, 800) && !pe_re_in_quad(sx, sz, 100, 800));

    /* the game's zone check (pe_re_zone.h) on made-up zones */
    {
        double fx, fz;
        pe_re_front(0, 0, 0, &fx, &fz);
        CHECK(fx > -1 && fx < 1 && fz > -621 && fz < -619);    /* facing 0: -z */
        pe_re_front(0, 0, 1024, &fx, &fz);
        CHECK(fx > -621 && fx < -619 && fz > -1 && fz < 1);    /* 1024: -x */
        CHECK(pe_re_scd_keys(0x0040) == (1u << 6 | 1u << 7 | 1u << 12));   /* Cross */
        CHECK(pe_re_scd_keys(0x1000) == (1u << 0 | 1u << 4) && pe_re_scd_keys(0x0020) == 1u << 14);
        PeImpTrigger tz[4];
        memset(tz, 0, sizeof tz);
        tz[0].zone.id = 3, tz[0].zone.sce = PE_RE_ZONE_MESSAGE, tz[0].zone.sat = 0x31;      /* examine, in front */
        rect(tz[0].zone.x, tz[0].zone.z, 0, 0, 1000, 1000);
        tz[0].data[0] = 7;
        tz[1].zone.id = 1, tz[1].zone.sce = PE_RE_ZONE_NOTHING, tz[1].zone.sat = 0x31;      /* kind 0: only F_atari */
        rect(tz[1].zone.x, tz[1].zone.z, 0, 0, 1000, 1000);
        tz[2].zone.id = 4, tz[2].zone.sce = PE_RE_ZONE_EVENT, tz[2].zone.sat = 0x41;        /* walk-in, underfoot */
        rect(tz[2].zone.x, tz[2].zone.z, 0, 1000, 1000, 2000);
        tz[2].data[0] = 10, tz[2].data[3] = 2;
        tz[3].zone.id = 5, tz[3].zone.sce = PE_RE_ZONE_FLAG, tz[3].zone.sat = 0x41, tz[3].zone.floor = 1;   /* upstairs */
        rect(tz[3].zone.x, tz[3].zone.z, 0, 1000, 1000, 2000);
        PeImpLogicBuilder *zb = pe_imp_logic_begin(PE_IMP_RE2, 0, 0, 0);
        CHECK(zb && !pe_imp_logic_add_trigger(zb, tz, 4));
        PeImpRoomLogic *zl = pe_imp_logic_finish(zb);
        CHECK(zl != NULL);
        if (zl) {
            PeReProgress zp;
            PeReZoneHit hit[4];
            memset(&zp, 0, sizeof zp);
            int16_t *w = zp.flags.work;
            w[0] = w[1] = -1;
            /* standing in it facing away: the front point is outside; facing it from 620 off: a hit */
            CHECK(pe_re_zone_check(zl, NULL, &zp, 500, 500, 2048, 0, 1, w, hit, 1) == 0 && w[0] == -1);
            CHECK(pe_re_zone_check(zl, NULL, &zp, 500, 1100, 0, 0, 1, w, hit, 1) == 1 && hit[0].id == 3 &&
                  hit[0].data[0] == 7 && w[0] == 3);           /* zone 1 (kind 0) hit first, looked past */
            CHECK(pe_re_zone_check(zl, NULL, &zp, 500, 1100, 0, 0, 0, w, hit, 4) == 1 && hit[0].id == 4 && w[1] == 4);
            CHECK(pe_re_zone_check(zl, NULL, &zp, 500, 1500, 0, 1, 0, w, hit, 4) == 1 && hit[0].id == 5);   /* floor 1 */
            CHECK(pe_re_zone_check(zl, NULL, &zp, 500, 5000, 0, 0, 0, w, hit, 4) == 0);
            free(zl);
        }
    }

    /* the pocket (eight slots), losing items, the item box, using an item */
    {
        static PeReProgress ip;
        memset(&ip, 0, sizeof ip);
        for (uint16_t it = 1; it <= PE_RE_POCKET_SLOTS; it++) CHECK(!pe_re_give(&ip, it, 1));
        CHECK(pe_re_give(&ip, 99, 1) < 0 && !pe_re_give(&ip, 3, 5) && ip.held == PE_RE_POCKET_SLOTS);   /* full; stacks */
        CHECK(!pe_re_lose(&ip, 3, 2) && pe_re_has(&ip, 3) && ip.pocket[2].count == 4);
        CHECK(!pe_re_lose(&ip, 3, 0) && !pe_re_has(&ip, 3) && ip.held == 7 && ip.pocket[2].item == 4);   /* moved up */
        CHECK(pe_re_lose(&ip, 3, 0) < 0);
        CHECK(!pe_re_box_put(&ip, 0) && ip.boxed == 1 && ip.box[0].item == 1 && !pe_re_has(&ip, 1) && ip.held == 6);
        CHECK(!pe_re_give(&ip, 1, 2) && !pe_re_box_put(&ip, ip.held - 1) && ip.boxed == 1 && ip.box[0].count == 3);
        CHECK(!pe_re_box_take(&ip, 0) && ip.boxed == 0 && pe_re_has(&ip, 1) && pe_re_box_take(&ip, 0) < 0);
        while (ip.held < PE_RE_POCKET_SLOTS) pe_re_give(&ip, (uint16_t)(50 + ip.held), 1);
        ip.box[0].item = 77, ip.box[0].count = 1, ip.boxed = 1;
        CHECK(pe_re_box_take(&ip, 0) < 0 && ip.boxed == 1);   /* pocket full: stays in the box */
        static PeReSession us;
        memset(&us, 0, sizeof us);
        us.game = pe_re_game_named("re2");
        pe_re_give(&us.progress, 0x40, 1);
        us.progress.flags.work[PE_RE_WORK_USE_ID] = -1;
        CHECK(pe_re_session_use(&us, 0x40) == PE_RE_SAID && strstr(us.said, "can't use") &&
              us.progress.flags.work[PE_RE_WORK_USE_ID] == -1);
        pe_re_flag_set(&us.progress.flags, PE_RE_FG_USE, 0x40, 1);
        CHECK(pe_re_session_use(&us, 0x40) == PE_RE_STAY && us.progress.flags.work[PE_RE_WORK_USE_ID] == 0x40);
        CHECK(pe_re_session_use(&us, 0x41) == PE_RE_STAY && us.uses == 1);   /* not held: nothing */
    }

    /* a made-up room: camera 0 -> 1 past x 2000, back past x 1800; a door at the far end */
    PeImpLogicBuilder *b = pe_imp_logic_begin(PE_IMP_RE2, 0, 0, 0);
    PeImpCameraZone cz[4];                  /* as RE2 stores them: each camera's own area first */
    memset(cz, 0, sizeof cz);
    cz[0].from = 0, cz[0].to = 0, rect(cz[0].x, cz[0].z, 0, 0, 2100, 1000);
    cz[1].from = 0, cz[1].to = 1, rect(cz[1].x, cz[1].z, 2000, 0, 4000, 1000);
    cz[2].from = 1, cz[2].to = 0, rect(cz[2].x, cz[2].z, 0, 0, 4000, 1000);   /* camera 1's area, not a switch */
    cz[3].from = 1, cz[3].to = 0, rect(cz[3].x, cz[3].z, 0, 0, 1800, 1000);
    PeImpDoor door;
    memset(&door, 0, sizeof door);
    rect(door.zone.x, door.zone.z, 3800, 0, 4000, 1000);
    door.stage = 14, door.room = 0xEE, door.camera = 2, door.next_x = 100, door.next_z = 200, door.next_dir = 1024;
    CHECK(b && !pe_imp_logic_add_camera_zone(b, cz, 4) && !pe_imp_logic_add_door(b, &door, 1));
    PeImpRoomLogic *l = pe_imp_logic_finish(b);
    CHECK(l != NULL);
    if (l) {
        CHECK(pe_re_camera_at(l, 0, 1000, 500) == 0 && pe_re_camera_at(l, 0, 2500, 500) == 1);
        CHECK(pe_re_camera_at(l, 1, 1900, 500) == 1 && pe_re_camera_at(l, 1, 1000, 500) == 0);   /* no flicker at the seam */
        CHECK(pe_re_camera_at(l, 1, 3000, 500) == 1);          /* in camera 1's own area: stays */
        PeImpDoor d;
        CHECK(pe_re_door_at(l, NULL, 3900, 500, &d) == 0 && d.room == 0xEE && pe_re_door_at(l, NULL, 3000, 500, NULL) < 0);
        PeReSession s;                                          /* steps on a session holding it */
        memset(&s, 0, sizeof s);
        s.game = pe_re_game_named("re2"), s.logic = l;
        CHECK(pe_re_session_step(&s, s_store, 1000, 500, 0, why, sizeof why) == PE_RE_STAY && s.camera == 0);
        CHECK(pe_re_session_step(&s, s_store, 2500, 500, 0, why, sizeof why) == PE_RE_CAMERA && s.camera == 1);
        CHECK(pe_re_session_step(&s, s_store, 3900, 500, 0, why, sizeof why) == PE_RE_STAY);   /* no Cross */
        CHECK(pe_re_session_step(&s, s_store, 3900, 500, 1, why, sizeof why) == PE_RE_STAY && *why);   /* room F-EE: no such room */
        printf("made-up door: %s\n", why);
        pe_re_session_end(&s);                                  /* frees l */
    }

    /* rules on made-up records: a keyed door, a bolted pair, a pickup, a message */
    PeReProgress pr;
    memset(&pr, 0, sizeof pr);
    PeImpDoor kd;
    char say[200];
    memset(&kd, 0, sizeof kd);
    kd.lock_id = 138, kd.lock_type = 88;
    CHECK(pe_re_door_try(&pr, &kd, NULL, NULL, say, sizeof say) == PE_RE_DOOR_LOCKED && !strcmp(say, "It's locked. It needs the item 88."));
    CHECK(!pe_re_give(&pr, 88, 1) && pe_re_has(&pr, 88));
    CHECK(pe_re_door_try(&pr, &kd, NULL, NULL, say, sizeof say) == PE_RE_DOOR_UNLOCKED && strstr(say, "Used the"));
    CHECK(pe_re_door_try(&pr, &kd, NULL, NULL, say, sizeof say) == PE_RE_DOOR_OPEN && !*say);   /* stays open */
    PeImpDoor far = kd, near = kd;                              /* one lock, two sides */
    far.lock_id = near.lock_id = 131, far.lock_type = PE_RE_LOCK_OTHER_SIDE, near.lock_type = PE_RE_LOCK_BOLT;
    CHECK(pe_re_door_try(&pr, &far, NULL, NULL, say, sizeof say) == PE_RE_DOOR_LOCKED && strstr(say, "other side"));
    CHECK(pe_re_door_try(&pr, &near, NULL, NULL, say, sizeof say) == PE_RE_DOOR_UNLOCKED);
    CHECK(pe_re_door_try(&pr, &far, NULL, NULL, say, sizeof say) == PE_RE_DOOR_OPEN);
    b = pe_imp_logic_begin(PE_IMP_RE2, 0, 0, 5);
    PeImpPickup pk[2];
    PeImpTrigger tg;
    memset(pk, 0, sizeof pk), memset(&tg, 0, sizeof tg);
    rect(pk[0].zone.x, pk[0].zone.z, 0, 0, 500, 500), pk[0].item = 88, pk[0].count = 1, pk[0].flag = 4;
    pk[1] = pk[0];                                              /* the same pickup on another branch */
    rect(tg.zone.x, tg.zone.z, 1000, 0, 1500, 500), tg.zone.sce = PE_RE_SCE_MESSAGE, tg.zone.sat = 0x31, tg.data[0] = 1;
    static const char text[] = "\nFirst.\0A paper,#\nnothing useful.\0";
    CHECK(b && !pe_imp_logic_add_pickup(b, pk, 2) && !pe_imp_logic_add_trigger(b, &tg, 1) &&
          !pe_imp_logic_add(b, PE_IMP_LOGIC_TEXT, 2, text, sizeof text - 1));
    l = pe_imp_logic_finish(b);
    CHECK(l != NULL);
    if (l) {
        PeReProgress q;
        PeImpPickup got;
        memset(&q, 0, sizeof q);
        CHECK(pe_re_pickup_at(l, NULL, &q, 100, 100, &got) == 0 && got.item == 88);
        CHECK(!pe_re_take(&q, l, &got, NULL, say, sizeof say) && !strcmp(say, "Picked up the item 88.") && pe_re_has(&q, 88));
        CHECK(pe_re_pickup_at(l, NULL, &q, 100, 100, NULL) < 0);     /* both copies gone: same flag */
        CHECK(pe_re_message_at(l, NULL, 1200, 100) == 1 && pe_re_message_at(l, NULL, 600, 100) < 0);
        CHECK(!pe_re_message(l, 1, say, sizeof say) && !strcmp(say, "A paper, nothing useful."));
        CHECK(!pe_re_message(l, 0, say, sizeof say) && !strcmp(say, "First.") && pe_re_message(l, 2, say, sizeof say));
        free(l);
    }

    /* ramps: a slope rising toward +x to floor 1; stairs rising toward -z from floor 1 to floor 2 */
    static PeRe2RoomCollision ramps;
    memset(&ramps, 0, sizeof ramps);
    ramps.count = 3;
    ramps.entries[0] = (PeRe2Collider){0, 0, 1000, 500, 11, (uint16_t)(1u << 6 | 0u << 4), 1};
    ramps.entries[1] = (PeRe2Collider){2000, 0, 2500, 1000, 12, (uint16_t)(2u << 6 | 3u << 4), 2};
    ramps.entries[2] = (PeRe2Collider){-500, 0, -400, 500, 0, 0, 2};   /* a wall on floor 1 only */
    double gy = 1;
    CHECK(pe_imp_re2_ground(&ramps, 0, 250, &gy) && gy == 0);
    CHECK(pe_imp_re2_ground(&ramps, 400, 250, &gy) && gy == -720 && pe_re_floor_of((int)gy) == 0);
    CHECK(pe_imp_re2_ground(&ramps, 500, 250, &gy) && gy == -900 && pe_re_floor_of((int)gy) == 1);   /* halfway: up, as the game rounds */
    CHECK(pe_imp_re2_ground(&ramps, 1000, 250, &gy) && gy == -1800 && pe_re_floor_of((int)gy) == 1);
    CHECK(pe_imp_re2_ground(&ramps, 2200, 1000, &gy) && gy == -1800);   /* stairs' bottom: their lowest floor */
    CHECK(pe_imp_re2_ground(&ramps, 2200, 0, &gy) && gy == -3600 && pe_re_floor_of((int)gy) == 2);
    CHECK(!pe_imp_re2_ground(&ramps, 1500, 250, &gy));
    CHECK(!pe_imp_re2_blocked_on(&ramps, 0, 500, 250, 50));           /* ramps never block */
    CHECK(pe_imp_re2_blocked_on(&ramps, 1, -450, 250, 50) && !pe_imp_re2_blocked_on(&ramps, 0, -450, 250, 50));

    char id[PE_IMP_ID_MAX];
    pe_re_logic_id("re2", 0, 0x0a, 1, id, sizeof id);
    CHECK(!strcmp(id, "re2:logic/10a-1"));
    pe_re_logic_id("re3", 1, 0x10, 0, id, sizeof id);
    CHECK(!strcmp(id, "re3:logic/210"));
    pe_re_room_id("re1", 0, 6, id, sizeof id);
    CHECK(!strcmp(id, "re1:room/106"));
    CHECK(pe_re_game_named("re2")->playable && pe_re_game_named("re1")->playable && !pe_re_game_named("dc1"));

    CHECK(!re_headless_smoke(NULL, 0));
    const char *re1 = getenv("PE_TEST_RE1_DISC");
    if (re1 && *re1) re1_mansion(re1);
    const char *re2 = getenv("PE_TEST_RE2_DISC");
    if (!re2 || !*re2) {
        printf("PE_TEST_RE2_DISC not set: the RE2 walk skipped\n");
        printf(s_fail ? "re session FAILED\n" : "re session passed\n");
        return s_fail;
    }
    PeImpScan sc;
    pe_imp_registry_reset();
    CHECK(!pe_imp_add_reader(&g_re2_logic_ps1) && !pe_imp_add_reader(&g_re2_rooms_ps1) && !pe_imp_add_reader(&g_re2_items_ps1) &&
          !pe_imp_add_reader(&g_re2_door_ps1));
    CHECK(!pe_imp_set_naming("re2", g_re2_naming));
    CHECK(!pe_imp_scan(s_store, re2, NULL, &sc));
    const PeReGame *g = pe_re_game_named("re2");
    CHECK(pe_re_imported(s_store, g) && !pe_re_imported(s_store, pe_re_game_named("re3")));

    PeReSession probe;
    const unsigned scenario = pe_re_session_start(&probe, s_store, g, 0, why, sizeof why) != 0;
    pe_re_session_end(&probe);
    CHECK(!re_headless_smoke(s_store, scenario));

    /* every room reachable through doors from the street, breadth first */
    static uint32_t queue[ROOMS_MAX];
    static uint8_t seen[16][256];
    int head = 0, tail = 0, doors = 0, missing = 0, no_camera = 0, blocked = 0, stuck = 0, facing_known = 0, facing_re = 0, facing_stage = 0;
    PeReSession s;
    CHECK(!pe_re_session_start(&s, s_store, g, scenario, why, sizeof why));
    pe_re_session_end(&s);
    queue[tail++] = g->stage << 8 | g->room, seen[g->stage][g->room] = 1;
    while (head < tail) {
        const uint32_t stage = queue[head] >> 8, room = queue[head] & 0xFF;
        head++;
        char lid[PE_IMP_ID_MAX];
        PeImpRoomLogic *rl = NULL;
        pe_re_logic_id("re2", stage, room, scenario, lid, sizeof lid);
        if (pe_imp_catalog_roomlogic(s_store, lid, &rl, NULL, 0)) continue;
        uint32_t n = 0;
        pe_imp_logic_section(rl, PE_IMP_LOGIC_DOOR, &n, NULL);
        for (uint32_t i = 0; i < n; i++) {
            PeImpDoor d;
            if (pe_imp_logic_door(rl, i, &d)) continue;
            doors++;
            const uint32_t ts = d.stage == PE_RE_STAGE_HERE ? stage : d.stage;
            if (ts >= 16) { missing++; continue; }
            PeReSession t;                                  /* take the door as the runtime does */
            memset(&t, 0, sizeof t);
            t.game = g, t.player = scenario;
            if (pe_re_session_enter(&t, s_store, ts, d.room, d.camera, d.next_x, d.next_z, d.next_dir, NULL, 0)) {
                missing++;
                continue;
            }
            char rid[PE_IMP_ID_MAX];
            PeImpImageSet *set = NULL;
            pe_re_room_id("re2", ts, d.room, rid, sizeof rid);
            if (pe_imp_catalog_imageset(s_store, rid, &set, NULL, 0) || d.camera >= set->count) no_camera++;
            uint32_t coll = 0;
            const uint8_t *data = pe_imp_logic_section(t.logic, PE_IMP_LOGIC_COLL, NULL, &coll);
            PeRe2RoomCollision c;
            if (data && !pe_imp_re2_collision_all(data, coll, &c)) {
                const int fl = pe_re_floor_of(d.next_y);
                double fx = d.next_x, fz = d.next_z;
                if (pe_imp_re2_blocked_on(&c, (unsigned)fl, fx, fz, 280)) blocked++;
                if (!pe_re_free_spot(&c, fl, &fx, &fz, 280, 2000)) {      /* as the stage places Aya */
                    stuck++;
                    if (getenv("PE_TEST_RE_STUCK"))
                        printf("  stuck: %x%02x door %u -> %x%02x at %d,%d (y %d), %u colliders\n", stage + 1, room, i,
                               ts + 1, d.room, d.next_x, d.next_z, d.next_y, c.count);
                }
            }
            {   /* which way she faces on landing: away from the door back, into the room */
                uint32_t nb = 0;
                double best = 1e18, vx = 0, vz = 0;
                pe_imp_logic_section(t.logic, PE_IMP_LOGIC_DOOR, &nb, NULL);
                for (uint32_t k = 0; k < nb; k++) {
                    PeImpDoor b;
                    if (pe_imp_logic_door(t.logic, k, &b) || b.room != room) continue;
                    double bx = 0, bz = 0;
                    for (int c4 = 0; c4 < 4; c4++) bx += b.zone.x[c4] / 4.0, bz += b.zone.z[c4] / 4.0;
                    const double dd = (bx - d.next_x) * (bx - d.next_x) + (bz - d.next_z) * (bz - d.next_z);
                    if (dd < best) best = dd, vx = d.next_x - bx, vz = d.next_z - bz;
                }
                if (best < 1e18 && (vx || vz)) {
                    const double a = (d.next_dir & 4095) * (acos(-1.0) / 2048.0);
                    facing_known++;
                    facing_re += vx * cos(a) - vz * sin(a) > 0;          /* RE2's Rot_vector: 0 faces +x */
                    const double y = pe_re_facing_of(d.next_dir) * (acos(-1.0) / 2048.0);
                    facing_stage += -vx * sin(y) - vz * cos(y) > 0;      /* as the session turns her: 0 faces -z */
                }
            }
            free(set);
            pe_re_session_end(&t);
            if (!seen[ts][d.room] && tail < ROOMS_MAX) seen[ts][d.room] = 1, queue[tail++] = ts << 8 | d.room;
        }
        free(rl);
    }
    /* every door's sound bank is in the store (as the importer files them) */
    {
        PeImpAsset *assets = NULL;
        const int na = pe_imp_catalog_assets(s_store, "re2", &assets);
        int with = 0, without = 0, waves[4] = {0};
        for (uint32_t st = 0; st < 7; st++)
            for (uint32_t rm = 0; rm < 64; rm++) {
                char lid[PE_IMP_ID_MAX];
                PeImpRoomLogic *rl = NULL;
                pe_re_logic_id("re2", st, rm, scenario, lid, sizeof lid);
                if (pe_imp_catalog_roomlogic(s_store, lid, &rl, NULL, 0)) continue;
                uint32_t n = 0;
                pe_imp_logic_section(rl, PE_IMP_LOGIC_DOOR, &n, NULL);
                for (uint32_t i = 0; i < n; i++) {
                    PeImpDoor d;
                    char sid[PE_IMP_ID_MAX];
                    int found = 0;
                    if (pe_imp_logic_door(rl, i, &d)) continue;
                    for (unsigned w = 1; w <= 4; w++) {
                        pe_re_door_sound_id("re2", &d, w, sid, sizeof sid);
                        for (int a = 0; a < na; a++) if (!strcmp(assets[a].id, sid)) { found = 1, waves[w - 1]++; break; }
                    }
                    found ? with++ : without++;
                }
                free(rl);
            }
        free(assets);
        printf("door sounds: %d doors with their bank, %d without; waves 1-4 present %d %d %d %d\n", with, without,
               waves[0], waves[1], waves[2], waves[3]);
        CHECK(with > 0 && without * 10 <= with);
    }
    printf("door landings facing into the room: %d of %d by RE2's angle, %d by the stage's\n", facing_re, facing_known,
           facing_stage);
    printf("RE2 from the street: %d rooms reached, %d doors, %d to rooms not imported, %d without their camera, "
           "%d onto blocked floor, %d with no free floor near\n", tail, doors, missing, no_camera, blocked, stuck);
    CHECK(tail >= 40 && doors >= 100);
    CHECK(facing_known > 100 && facing_stage == facing_re && facing_stage * 10 >= facing_known * 9);

    /* The detailed key, script and placement expectations below describe Leon A. */
    if (scenario) {
        CHECK(missing * 20 <= doors && no_camera == 0 && stuck * 20 <= doors);
        printf(s_fail ? "Claire room walk FAILED\n" : "Claire room walk passed\n");
        return s_fail;
    }

    /* RE2's own rules: 1-05 holds the Cabin Key and the door to 1-06 that needs it */
    PeReSession r;
    CHECK(!pe_re_session_start(&r, s_store, g, 0, why, sizeof why) && r.items);
    if (r.logic && !pe_re_session_enter(&r, s_store, 0, 0x05, 0, 0, 0, 0, why, sizeof why)) {
        PeImpPickup key;
        PeImpDoor cabin;
        int ki = -1, di = -1;
        uint32_t n = 0;
        pe_imp_logic_section(r.logic, PE_IMP_LOGIC_PICK, &n, NULL);
        for (uint32_t i = 0; i < n && ki < 0; i++) if (!pe_imp_logic_pickup(r.logic, i, &key) && key.item == 88) ki = (int)i;
        pe_imp_logic_section(r.logic, PE_IMP_LOGIC_DOOR, &n, NULL);
        for (uint32_t i = 0; i < n && di < 0; i++) if (!pe_imp_logic_door(r.logic, i, &cabin) && cabin.lock_type == 88) di = (int)i;
        CHECK(ki >= 0 && di >= 0);
        if (ki >= 0 && di >= 0) {
            double dx = 0, dz = 0, kx = 0, kz = 0;
            for (int k = 0; k < 4; k++) dx += cabin.zone.x[k] / 4.0, dz += cabin.zone.z[k] / 4.0, kx += key.zone.x[k] / 4.0, kz += key.zone.z[k] / 4.0;
            CHECK(pe_re_session_step(&r, s_store, dx, dz, 1, why, sizeof why) == PE_RE_SAID);
            printf("1-05 door: %s\n", r.said);
            CHECK(!strcmp(r.said, "It's locked. It needs the Cabin Key."));
            CHECK(pe_re_session_step(&r, s_store, kx, kz, 1, why, sizeof why) == PE_RE_SAID);
            printf("1-05 pickup: %s\n", r.said);
            CHECK(!strcmp(r.said, "Picked up the Cabin Key.") && pe_re_has(&r.progress, 88));
            CHECK(pe_re_session_step(&r, s_store, kx, kz, 1, why, sizeof why) != PE_RE_SAID || strcmp(r.said, "Picked up the Cabin Key."));
            CHECK(pe_re_session_step(&r, s_store, dx, dz, 1, why, sizeof why) == PE_RE_ROOM && r.room == 0x06);
            printf("1-05 door again: %s -> room %x-%02X\n", r.said, (unsigned)r.stage + 1, (unsigned)r.room);
            CHECK(!strcmp(r.said, "Used the Cabin Key."));
        }
    }
    if (!pe_re_session_enter(&r, s_store, 0, 0x01, 0, 0, 0, 0, why, sizeof why)) {   /* 1-01's papers */
        char m[200];
        CHECK(!pe_re_message(r.logic, 13, m, sizeof m) && !strcmp(m, "Paper dated Sep. 18th. It looks irrelevant."));
        CHECK(!pe_re_message(r.logic, 19, m, sizeof m) && !strcmp(m, "Gun cases and related paraphernalia."));
        CHECK(r.run.ran && r.run.nlive > 0);
    }
    /* 1-05 again: the Cabin Key was taken, so its FG_ITEM flag is set: the script still runs its
     * item_aot_set (RE2 doesn't guard them), and the flag keeps it from being placed, as the engine does */
    if (!pe_re_session_enter(&r, s_store, 0, 0x05, 0, 0, 0, 0, why, sizeof why)) {
        uint32_t n = 0;
        int placed = 0, found = 0;
        pe_imp_logic_section(r.logic, PE_IMP_LOGIC_PICK, &n, NULL);
        for (uint32_t i = 0; i < n; i++) {
            PeImpPickup pk, again;
            if (pe_imp_logic_pickup(r.logic, i, &pk) || pk.item != 88) continue;
            placed += pe_re_scd_live(&r.run, pk.at);
            double kx = 0, kz = 0;
            for (int k = 0; k < 4; k++) kx += pk.zone.x[k] / 4.0, kz += pk.zone.z[k] / 4.0;
            found += pe_re_pickup_at(r.logic, &r.run, &r.progress, kx, kz, &again) >= 0 && again.item == 88;
        }
        printf("1-05 after the key: its item_aot_set ran %d times, pickable %d\n", placed, found);
        CHECK(r.run.ran && placed >= 1 && !found && pe_re_flag(&r.progress.flags, PE_RE_FG_ITEM, 4));
    }
    /* saved and loaded: the bytes round-trip, and a resumed session is where it was, with what it had */
    if (r.game) {
        static uint8_t bytes[PE_RE_SAVE_SIZE], again[PE_RE_SAVE_SIZE];
        PeReSaved saved;
        PeReSession back;
        r.x = 1234, r.z = -5678, r.facing = 1024, r.camera = 0;
        r.progress.box[0].item = 0x40, r.progress.box[0].count = 2, r.progress.boxed = 1;   /* v2: the box, work */
        r.progress.flags.work[20] = 1234;
        pe_re_save_encode(&r, 1, bytes);
        {
            static uint8_t up[PE_RE_SAVE_SIZE];
            PeReSaved old;
            CHECK(!pe_re_save_upgrade(up, bytes, PE_RE_SAVE_SIZE_V1, 1) && !pe_re_save_decode(up, &old) &&
                  old.room == 0x05 && pe_re_has(&old.progress, 88) && !old.progress.boxed && !old.progress.flags.work[20]);
            CHECK(pe_re_save_upgrade(up, bytes, PE_RE_SAVE_SIZE_V1, 3) < 0);
        }
        CHECK(!pe_re_save_decode(bytes, &saved) && saved.game == g && saved.playing && saved.room == 0x05 &&
              saved.x == 1234 && saved.z == -5678 && saved.facing == 1024 && pe_re_has(&saved.progress, 88));
        CHECK(!pe_re_session_resume(&back, s_store, &saved, why, sizeof why) && back.room == 0x05 && back.x == 1234);
        CHECK(pe_re_has(&back.progress, 88) && pe_re_flag(&back.progress.flags, PE_RE_FG_ITEM, 4));
        CHECK(!memcmp(back.progress.unlocked, r.progress.unlocked, sizeof r.progress.unlocked));
        CHECK(back.progress.boxed == 1 && back.progress.box[0].item == 0x40 && back.progress.box[0].count == 2 &&
              back.progress.flags.work[20] == 1234);
        pe_re_save_encode(&back, 1, again);
        CHECK(!memcmp(bytes, again, sizeof bytes));               /* stable */
        PeImpDoor cabin;                                          /* the cabin door stays open */
        uint32_t n = 0;
        int di = -1;
        pe_imp_logic_section(back.logic, PE_IMP_LOGIC_DOOR, &n, NULL);
        for (uint32_t i = 0; i < n && di < 0; i++) if (!pe_imp_logic_door(back.logic, i, &cabin) && cabin.lock_type == 88) di = (int)i;
        if (di >= 0) {
            double dx = 0, dz = 0;
            for (int k = 0; k < 4; k++) dx += cabin.zone.x[k] / 4.0, dz += cabin.zone.z[k] / 4.0;
            CHECK(pe_re_session_step(&back, s_store, dx, dz, 1, why, sizeof why) == PE_RE_ROOM && back.room == 0x06 && !back.said[0]);
        }
        printf("saved %d bytes; resumed in %x-%02X with the Cabin Key, its door open\n", PE_RE_SAVE_SIZE,
               (unsigned)saved.stage + 1, (unsigned)saved.room);
        pe_re_session_end(&back);
        memset(bytes, 0, sizeof bytes);
        CHECK(!pe_re_save_decode(bytes, &saved) && !saved.game);  /* a save without a game */
        bytes[0] = 9;
        CHECK(pe_re_save_decode(bytes, &saved) < 0);
    }
    pe_re_session_end(&r);

    /* using an item where a room takes it, end to end: 1-08's valve spot. Standing facing it, the
     * every-frame zone check sets its Use_flg bit and F_atari; using the item sets Use_id; the next
     * frame's handler (main procedure 1) sees both and starts the room's event, which switches the
     * spot off (aot_reset). Away from it, the item can't be used. */
    {
        static PeReSession v;
        if (!pe_re_session_start(&v, s_store, pe_re_game_named("re2"), 0, why, sizeof why) &&
            !pe_re_session_enter(&v, s_store, 0, 0x08, 0, 0, 0, 0, why, sizeof why)) {
            uint32_t n = 0;
            PeImpTrigger spot;
            int have = 0;
            pe_imp_logic_section(v.logic, PE_IMP_LOGIC_TRIG, &n, NULL);
            for (uint32_t i = 0; i < n && !have; i++)
                if (!pe_imp_logic_trigger(v.logic, i, &spot) && pe_re_scd_live(&v.run, spot.at) &&
                    spot.zone.sce == PE_RE_ZONE_FLAG && (spot.data[0] | spot.data[1] << 8) == PE_RE_FG_USE)
                    have = 1;
            CHECK(have);
            if (have) {
                const uint16_t item = (uint16_t)(spot.data[2] | spot.data[3] << 8);
                double cx = 0, cz = 0;
                for (int c = 0; c < 4; c++) cx += spot.zone.x[c] / 4.0, cz += spot.zone.z[c] / 4.0;
                pe_re_give(&v.progress, item, 1);
                for (int f = 0; f < 30; f++) pe_re_session_tick(&v);     /* the room settles */
                v.facing = 0, v.floor = 0;
                pe_re_session_step(&v, s_store, cx, cz + 3000, 0, why, sizeof why);   /* far off */
                CHECK(pe_re_session_use(&v, item) == PE_RE_SAID);
                pe_re_session_tick(&v);
                pe_re_session_step(&v, s_store, cx, cz + PE_RE_FRONT_REACH, 0, why, sizeof why);   /* facing it */
                CHECK(v.progress.flags.work[PE_RE_WORK_F_ATARI] == spot.zone.id && pe_re_flag(&v.progress.flags, PE_RE_FG_USE, item));
                const int resets = v.run.nreset;
                CHECK(pe_re_session_use(&v, item) == PE_RE_STAY);
                int cuts = 0, held = 0;
                for (int f = 0; f < 5; f++) cuts += pe_re_session_tick(&v) == PE_RE_CAMERA;
                CHECK(v.run.nreset > resets && pe_re_zone_kind(&v.run, spot.zone.id, spot.zone.sce, NULL) == 0);
                CHECK(v.progress.flags.work[PE_RE_WORK_USE_ID] == -1);   /* cleared after the frame */
                for (int f = 0; f < 600; f++) {                  /* the event plays: its camera cuts */
                    cuts += pe_re_session_tick(&v) == PE_RE_CAMERA;
                    held += v.run.camera_lock;
                }
                CHECK(cuts >= 2 && held > 0);
                printf("1-08 item %u used at zone %u: %d zones switched by its event, %d camera cuts, last camera %d\n",
                       (unsigned)item, (unsigned)spot.zone.id, v.run.nreset - resets, cuts, v.camera);
            }
        } else {
            printf("1-08: %s\n", why);
            CHECK(0);
        }
        pe_re_session_end(&v);
    }

    /* enemies: the street's placements reported; one beaten stays gone when she comes back */
    {
        static PeReSession v;
        PeReEnemyPlacement e[PE_RE_ENEMIES_MAX];
        if (!pe_re_session_start(&v, s_store, pe_re_game_named("re2"), 0, why, sizeof why)) {
            for (int f = 0; f < 30; f++) pe_re_session_tick(&v);
            const int n = pe_re_session_enemies(&v, e, PE_RE_ENEMIES_MAX);
            int flagged = -1;
            for (int i = 0; i < n && flagged < 0; i++) if (e[i].killed_flag != 0xFF) flagged = i;
            printf("RE2 street: %d enemies placed (first kind 0x%02X at %d,%d)\n", n, n ? e[0].kind : 0, n ? e[0].x : 0,
                   n ? e[0].z : 0);
            CHECK(n > 0 && flagged >= 0);
            if (flagged >= 0) {
                const uint8_t flag = e[flagged].killed_flag;
                CHECK(!pe_re_session_defeated(&v, flag) && pe_re_session_enemies(&v, e, PE_RE_ENEMIES_MAX) == n - 1);
                CHECK(!pe_re_session_enter(&v, s_store, v.stage, v.room, 0, v.x, v.z, 0, why, sizeof why));
                for (int f = 0; f < 30; f++) pe_re_session_tick(&v);
                int back = 0;
                const int m = pe_re_session_enemies(&v, e, PE_RE_ENEMIES_MAX);
                for (int i = 0; i < m; i++) back |= e[i].killed_flag == flag;
                CHECK(m == n - 1 && !back);
                uint8_t saved[PE_RE_SAVE_SIZE];
                PeReSaved decoded;
                pe_re_save_encode(&v, 1, saved);
                const int decoded_ok = !pe_re_save_decode(saved, &decoded);
                CHECK(decoded_ok);
                pe_re_session_end(&v);
                const int resumed = decoded_ok ? pe_re_session_resume(&v, s_store, &decoded, why, sizeof why) : -1;
                CHECK(!resumed);
                if (!resumed) {
                    for (int f = 0; f < 30; f++) pe_re_session_tick(&v);
                    const int remaining = pe_re_session_enemies(&v, e, PE_RE_ENEMIES_MAX);
                    CHECK(remaining == n - 1);
                    for (int i = 0; i < remaining; i++) CHECK(e[i].killed_flag != flag);
                }
            }
            CHECK(pe_re_session_defeated(&v, 0xFF) < 0);
        }
        pe_re_session_end(&v);
    }

    /* a typewriter and an item box (2-08): examined from in front, they're handed to the mod */
    {
        static PeReSession v;
        if (!pe_re_session_start(&v, s_store, pe_re_game_named("re2"), 0, why, sizeof why) &&
            !pe_re_session_enter(&v, s_store, 1, 0x08, 0, 0, 0, 0, why, sizeof why)) {
            for (int f = 0; f < 30; f++) pe_re_session_tick(&v);
            const int want[2] = {PE_RE_ZONE_SAVE, PE_RE_ZONE_ITEMBOX};
            for (int w = 0; w < 2; w++) {
                uint32_t n = 0;
                PeImpTrigger tz;
                int have = 0;
                pe_imp_logic_section(v.logic, PE_IMP_LOGIC_TRIG, &n, NULL);
                for (uint32_t i = 0; i < n && !have; i++)
                    have = !pe_imp_logic_trigger(v.logic, i, &tz) && pe_re_scd_live(&v.run, tz.at) &&
                           pe_re_zone_kind(&v.run, tz.zone.id, tz.zone.sce, NULL) == want[w];
                CHECK(have);
                if (!have) continue;
                double cx = 0, cz = 0;
                for (int c = 0; c < 4; c++) cx += tz.zone.x[c] / 4.0, cz += tz.zone.z[c] / 4.0;
                v.facing = 0, v.floor = 0;
                pe_re_session_tick(&v);
                const PeReChange ch = pe_re_session_step(&v, s_store, cx, cz + PE_RE_FRONT_REACH, 1, why, sizeof why);
                printf("2-08 kind %d zone %u (sat %02x floor %02x super %02x): change %d, zone kind %u\n", want[w],
                       (unsigned)tz.zone.id, (unsigned)tz.zone.sat, (unsigned)tz.zone.floor, (unsigned)tz.zone.super,
                       (int)ch, (unsigned)v.zone.sce);
                CHECK(ch == PE_RE_ZONE && v.zone.sce == want[w]);
            }
        } else {
            printf("2-08: %s\n", why);
            CHECK(0);
        }
        pe_re_session_end(&v);
    }

    /* every room's scripts, as a new game (Leon A): what ran, what was skipped */
    PeReProgress fresh;
    int rooms = 0, ran = 0, live_doors = 0, all_doors = 0, live_items = 0, all_items = 0;
    long executed = 0, skipped = 0, stopped = 0;
    int placed = 0, enem_records = 0;
    int kinds[16] = {0}, manual = 0, events = 0, events_found = 0, events_started = 0, use_zones = 0, use_found = 0;
    for (uint32_t st = 0; st < 7; st++)
        for (uint32_t rm = 0; rm < 64; rm++) {
            char lid[PE_IMP_ID_MAX];
            PeImpRoomLogic *rl = NULL;
            pe_re_logic_id("re2", st, rm, 0, lid, sizeof lid);
            if (pe_imp_catalog_roomlogic(s_store, lid, &rl, NULL, 0)) continue;
            static PeReScdRun run;
            memset(&fresh, 0, sizeof fresh);
            pe_re_scd_run(rl, &fresh.flags, NULL, &run);
            for (int f = 0; f < 120; f++) pe_re_scd_tick(rl, &fresh.flags, NULL, &run);   /* four seconds of play */
            rooms++, ran += run.ran, executed += run.executed, skipped += run.skipped, stopped += run.stopped;
            placed += run.nenemy;
            { uint32_t ne = 0; pe_imp_logic_section(rl, PE_IMP_LOGIC_ENEM, &ne, NULL); enem_records += (int)ne; }
            if (run.messages && getenv("PE_TEST_RE_MESSAGES"))
                printf("  %x%02x: message_on %d times in 4 s, %d tasks running, %d over budget\n", st + 1, rm,
                       run.messages, run.waiting, run.stopped);
            uint32_t n = 0;
            pe_imp_logic_section(rl, PE_IMP_LOGIC_DOOR, &n, NULL);
            for (uint32_t i = 0; i < n; i++) {
                PeImpDoor d;
                if (!pe_imp_logic_door(rl, i, &d)) all_doors++, live_doors += pe_re_scd_live(&run, d.at);
            }
            pe_imp_logic_section(rl, PE_IMP_LOGIC_PICK, &n, NULL);
            for (uint32_t i = 0; i < n; i++) {
                PeImpPickup pk;
                if (!pe_imp_logic_pickup(rl, i, &pk)) all_items++, live_items += pe_re_scd_live(&run, pk.at);
            }
            /* the zones the scripts left: by kind; each event zone found where the game looks for it,
             * and its task starting */
            pe_imp_logic_section(rl, PE_IMP_LOGIC_TRIG, &n, NULL);
            for (uint32_t i = 0; i < n; i++) {
                PeImpTrigger tr;
                if (pe_imp_logic_trigger(rl, i, &tr) || !pe_re_scd_live(&run, tr.at)) continue;
                const int k = pe_re_zone_kind(&run, tr.zone.id, tr.zone.sce, tr.data);
                kinds[k & 15]++, manual += (tr.zone.sat & 0x10) != 0;
                if (k == PE_RE_ZONE_FLAG && (tr.data[0] | tr.data[1] << 8) == PE_RE_FG_USE && tr.data[4]) {
                    /* an item usable here: a flag-change zone that sets its Use_flg bit (SCE_AT.C Sce_Flg_chg
                     * writes through the same flag table as the scripts' set) while she stands in it */
                    double cx = 0, cz = 0;
                    PeReZoneHit hit[16];
                    for (int c = 0; c < 4; c++) cx += tr.zone.x[c] / 4.0, cz += tr.zone.z[c] / 4.0;
                    const int under = (tr.zone.sat & PE_RE_SAT_UNDER) != 0;
                    const int nh = pe_re_zone_check(rl, &run, &fresh, cx, under ? cz : cz + PE_RE_FRONT_REACH, 0,
                                                    tr.zone.floor & 0x80 ? 0 : tr.zone.floor, 0, NULL, hit, 16);
                    int found = 0;
                    for (int h = 0; h < nh; h++) found |= hit[h].id == tr.zone.id;
                    use_zones++, use_found += found;
                    if (getenv("PE_TEST_RE_EVENTS"))
                        printf("  %x%02x zone %u: item %u usable here, sat %02x%s\n", st + 1, rm, (unsigned)tr.zone.id,
                               (unsigned)(tr.data[2] | tr.data[3] << 8), (unsigned)tr.zone.sat, found ? "" : " NOT FOUND");
                }
                if (k != PE_RE_ZONE_EVENT || !(tr.zone.sat & 1) || (tr.zone.super & 0x80)) continue;
                events++;
                double cx = 0, cz = 0;
                for (int c = 0; c < 4; c++) cx += tr.zone.x[c] / 4.0, cz += tr.zone.z[c] / 4.0;
                const int under = (tr.zone.sat & PE_RE_SAT_UNDER) != 0;
                PeReZoneHit hit[16];
                int16_t work[4] = {-1, -1, -1, -1};
                const int floor = tr.zone.floor & 0x80 ? 0 : tr.zone.floor;
                const int nh = pe_re_zone_check(rl, &run, &fresh, cx, under ? cz : cz + PE_RE_FRONT_REACH, 0, floor,
                                                (tr.zone.sat & 0x10) != 0, work, hit, 16);
                int found = 0;
                for (int h = 0; h < nh; h++) found |= hit[h].id == tr.zone.id;
                found |= work[under ? 1 : 0] == tr.zone.id;   /* an examine stopped at an earlier zone */
                events_found += found;
                const unsigned level = (unsigned)tr.data[0] | (unsigned)tr.data[1] << 8, proc = tr.data[3];
                pe_re_scd_exec(rl, &run, level, proc);
                events_started += pe_re_scd_running(&run, proc);
                if (getenv("PE_TEST_RE_EVENTS"))
                    printf("  %x%02x zone %u: event level %u proc %u sat %02x floor %02x%s%s\n", st + 1, rm,
                           (unsigned)tr.zone.id, level, proc, (unsigned)tr.zone.sat, (unsigned)tr.zone.floor,
                           found ? "" : " NOT FOUND", pe_re_scd_running(&run, proc) ? "" : " not started");
            }
            free(rl);
        }
    printf("RE2 zones, new game: %d message, %d event, %d flag, %d save, %d item box, %d other; %d of them examined; "
           "event zones found %d of %d, started %d\n", kinds[PE_RE_ZONE_MESSAGE], kinds[PE_RE_ZONE_EVENT],
           kinds[PE_RE_ZONE_FLAG], kinds[PE_RE_ZONE_SAVE], kinds[PE_RE_ZONE_ITEMBOX],
           kinds[0] + kinds[3] + kinds[7] + kinds[8] + kinds[11] + kinds[12] + kinds[13] + kinds[14], manual,
           events_found, events, events_started);
    CHECK(events > 0 && events_found * 10 >= events * 9 && events_started * 10 >= events * 9);
    CHECK(kinds[PE_RE_ZONE_SAVE] > 0 && kinds[PE_RE_ZONE_ITEMBOX] > 0);
    printf("RE2 enemies, new game: %d placements run of %d in the rooms' scripts\n", placed, enem_records);
    CHECK(placed > 50 && placed <= enem_records);
    printf("RE2 item use, new game: %d zones make an item usable, %d found where she stands; %d pocket zones\n",
           use_zones, use_found, kinds[PE_RE_ZONE_STATUS]);
    CHECK(use_zones > 5 && use_found * 10 >= use_zones * 9);
    printf("RE2 scripts, new game: %d rooms, %d ran; %ld instructions run, %ld skipped, %ld tasks over the frame budget "
           "(4 s of play); doors set up %d of %d, pickups %d of %d\n", rooms, ran, executed, skipped, stopped,
           live_doors, all_doors, live_items, all_items);
    CHECK(rooms >= 100 && ran == rooms && live_doors * 10 >= all_doors * 7 && live_items > 0 && live_items < all_items);
    /* Landings are checked against their own floor's collision (1,800 units a floor). A few still
     * have no free floor near (stairs and slopes count as solid for now; the stage walks Aya freely
     * until she's clear): 7 of 263 on disc 1, was 40 before floors. Guard the count. */
    CHECK(missing * 20 <= doors && no_camera == 0 && stuck * 20 <= doors);

    printf(s_fail ? "re session FAILED\n" : "re session passed\n");
    return s_fail;
}
