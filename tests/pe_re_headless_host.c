/* A second host for the production RE play mod. It records presentation and lets the test
 * drive input and lifecycle events without the game, renderer or platform. */
#include <stdio.h>
#include <string.h>

#include "pe_extension_section.h"
#include "pe_mod_api.h"
#include "pe_mod_menu.h"
#include "pe_re_door.h"
#include "pe_re_save.h"
#include "pe_services.h"
#include "re_host.h"

extern const PeMod pe_mod_pe_re_play;

static struct {
    char room[PE_IMP_ID_MAX], text[200];
    double x, z;
    int facing, camera, floor, active, shade, curtain, hold, backdrop;
    unsigned rooms, placements, saves;
    int choice, busy, menu;
    uint16_t buttons;
    ReHostCallback frame, save_read, new_game, save_committed;
    PeMenuAction start, next_door, jump, typewriter, give_ribbon, next_use, give_use;
    ReHostPose gesture;
    unsigned gesture_frame;
    unsigned camera_count;
    int cameras[PE_RE1_PIANO_CUTS + 1];
    ReHostRoomProp prop;
    int *scenario, *stage, *room_number;
    const PeExtensionSection *progress;
    const PeRePlayApi *api;
    const char *store;
} s;

static void noop(void) {}
static void setting(int value) { (void)value; }
static int unavailable(void) { return 0; }
static int blocked(double x, double z) { (void)x; (void)z; return 0; }
static void init(const char *mod_id, const char *store) { (void)mod_id; (void)store; }
static void place(double x, double z) { s.x = x; s.z = z; }
static void face(int facing) { s.facing = facing; }
static int floor_now(void) { return s.floor; }
static double scale(void) { return 1; }
static int active(void) { return s.active; }
static int field_free(void) { return !s.menu && !s.busy; }
static int menu_active(void) { return s.menu; }
static void open_save(void) { s.menu = 1; s.saves++; }
static uint16_t buttons(void) { return s.buttons; }

static int show_room(const char *id, const PeImpRoomLogic *logic, int camera, int floor, int use_pose,
                     double x, double z, int facing, char *why, size_t n) {
    if (!logic) {
        snprintf(why, n, "Missing room logic");
        return -1;
    }
    snprintf(s.room, sizeof s.room, "%s", id);
    s.active = 1;
    s.rooms++;
    s.camera = camera;
    if (s.hold && s.gesture == RE_POSE_PIANO && s.camera_count < sizeof s.cameras / sizeof *s.cameras)
        s.cameras[s.camera_count++] = camera;
    s.floor = floor;
    place(use_pose ? x : 0, use_pose ? z : 0);
    face(use_pose ? facing : 0);
    return 0;
}

static void hide(void) { s.active = 0; }
static void fade(int shade, int curtain, int hold) { s.shade = shade; s.curtain = curtain; s.hold = hold; }
static void pose(double *x, double *z, int *facing) { *x = s.x; *z = s.z; *facing = s.facing; }
static void placements(const struct PeReEnemyPlacement *list, int count) {
    (void)list;
    s.placements = (unsigned)count;
}
static int say(const char *text) {
    if (s.busy) return 0;
    snprintf(s.text, sizeof s.text, "%s", text);
    s.busy = 1;
    return 1;
}
static int choose(const char *title, const char *const labels[], int count, int at) {
    return count > 0 && at >= 0 && at < count && labels[at] && say(title);
}
static int choice(void) { const int result = s.choice; s.choice = -1; return result; }
static int busy(void) { return s.busy; }
static void forget(void) { s.busy = 0; s.choice = -1; }
static void gesture(ReHostPose pose, unsigned frame, unsigned frames) {
    s.gesture = pose;
    s.gesture_frame = frames ? frame : 0;
}
static void prop(const ReHostRoomProp *placement) {
    s.prop = placement ? *placement : (ReHostRoomProp){0};
}
static int arm(void) { return s.backdrop = s.active; }
static void release(void) { s.backdrop = 0; }
static void bind(const char *mod_id, ReHostCallback frame, ReHostCallback save_read, ReHostCallback new_game) {
    (void)mod_id;
    s.frame = frame;
    s.save_read = save_read;
    s.new_game = new_game;
}

const ReHost *re_host(void) {
    static const ReHost host = {
        .struct_size = sizeof host, .init = init, .shutdown = noop, .show_room = show_room,
        .hide_room = hide, .room_active = active, .room_fade = fade, .player_pose = pose,
        .player_floor = floor_now, .place_player = place, .face_player = face, .position_blocked = blocked,
        .place_at_ramp = unavailable, .set_masks = setting, .set_player_size = setting, .player_scale = scale,
        .set_placement_markers = setting, .set_placement_meshes = setting, .set_placement_idle = setting,
        .set_placements = placements, .say = say, .choose = choose, .dialogue_choice = choice,
        .dialogue_busy = busy, .forget_dialogue = forget, .field_free = field_free, .held_buttons = buttons,
        .consume_native_interact = noop, .open_save_screen = open_save, .native_menu_active = menu_active,
        .arm_backdrop = arm, .release_backdrop = release, .bind_lifecycle = bind, .player_gesture = gesture,
        .set_room_prop = prop,
    };
    return &host;
}

/* The shared mod-host services the production mod uses. The registered callbacks and save bytes
 * let the test exercise the same paths as the game menu, encounter service and companion store. */
void pe_menu_begin(const char *page) { (void)page; }
int pe_menu_top_page(const char *page, const char *desc, int order) { (void)page; (void)desc; (void)order; return 0; }
int pe_menu_register_action(const char *sub, const char *label, const char *desc, PeMenuAction action) {
    (void)sub; (void)desc;
    if (!strcmp(label, "Start")) s.start = action;
    if (!strcmp(label, "Go to the next door")) s.next_door = action;
    if (!strcmp(label, "Go to that room")) s.jump = action;
    if (!strcmp(label, "Go to the next typewriter")) s.typewriter = action;
    if (!strcmp(label, "Give an Ink Ribbon")) s.give_ribbon = action;
    if (!strcmp(label, "Go to the next place to use an item")) s.next_use = action;
    if (!strcmp(label, "Give the item used there")) s.give_use = action;
    return 0;
}
int pe_menu_register_int(const char *sub, const char *label, const char *desc,
                         int *value, int min, int max, int step) {
    (void)sub; (void)desc; (void)min; (void)max; (void)step;
    if (!strcmp(label, "Scenario")) s.scenario = value;
    if (!strcmp(label, "Stage")) s.stage = value;
    if (!strcmp(label, "Room")) s.room_number = value;
    return 0;
}
int pe_menu_register_toggle(const char *sub, const char *label, const char *desc, int *value, PeMenuPerFrame fn) {
    (void)sub; (void)label; (void)desc; (void)value; (void)fn; return 0;
}
int pe_menu_register_info(const char *sub, const char *label, const char *desc, PeMenuText fn) {
    (void)sub; (void)label; (void)desc; (void)fn; return 0;
}
int pe_menu_register_dynamic_choice(const char *sub, const char *label, const char *desc, int *value,
                                    unsigned max, PeMenuChoiceCount count, PeMenuChoiceName name) {
    (void)sub; (void)label; (void)desc; (void)value; (void)max; (void)count; (void)name; return 0;
}
int pe_mod_path(char *out, size_t n, const char *file) { (void)file; return snprintf(out, n, "%s", s.store) >= (int)n; }
void pe_mod_log(const char *mod_id, const char *fmt, ...) { (void)mod_id; (void)fmt; }
void pe_toast(const char *text) { (void)text; }
int pe_event_id(const char *name) {
    if (!strcmp(name, PE_EVENT_SAVE_COMMITTED)) return 1;
    return !strcmp(name, "re_play.room") ? 2 : 3;
}
int pe_hook_add(int id, PeHookFn fn, void *user, int priority, const char *mod_id) {
    (void)user; (void)priority; (void)mod_id;
    if (id != 1) return -1;
    s.save_committed = fn;
    return 0;
}
int pe_event_emit(int id, void *ctx) { (void)id; (void)ctx; return 0; }
int pe_service_publish(const char *name, int version, const void *api, const char *mod_id) {
    (void)mod_id;
    if (strcmp(name, "re_play") || version != 1) return -1;
    s.api = api;
    return 0;
}
const void *pe_service_get(const char *name, int version) {
    return version == 1 && !strcmp(name, "re_play") ? s.api : NULL;
}
int pe_extension_register(const PeExtensionSection *section) { s.progress = section; return 0; }
PeRegResult pe_extension_enable(const PeExtensionSection *section) {
    (void)section;
    return PE_REG_OK;
}

static void frames(unsigned count, uint16_t held) {
    s.buttons = held;
    for (unsigned i = 0; i < count; i++) s.frame(NULL, NULL);
}

#define EXPECT(expr) do { if (!(expr)) { printf("Headless host failed: %s\n", #expr); return -1; } } while (0)

static int play(unsigned scenario) {
    PeReRoomEvent room;
    EXPECT(s.start && s.frame && s.save_read && s.new_game && s.scenario && s.api && s.progress);
    *s.scenario = (int)scenario;
    s.start();
    frames(30, 0);
    EXPECT(s.active && !strcmp(s.room, "re2:room/100") && !s.api->room(&room));
    int flag = -1;
    for (int i = 0; i < room.count && flag < 0; i++)
        if (room.enemies[i].killed_flag != 0xFF) flag = room.enemies[i].killed_flag;
    EXPECT(flag >= 0);
    const int before = room.count;
    EXPECT(s.api->pause_for_battle() == 1 && s.backdrop && !s.active);
    s.api->resume_after_battle();
    frames(2, 0);
    EXPECT(!s.backdrop && s.active && !s.api->room(&room));
    EXPECT(room.count == before);
    EXPECT(s.api->pause_for_battle() == 1 && s.backdrop && !s.active);
    EXPECT(s.api->defeated(0xFF) < 0);
    EXPECT(!s.api->defeated((uint8_t)flag));
    PeReSaved saved;
    EXPECT(!pe_re_save_decode(s.progress->data, &saved) && saved.playing && saved.player == scenario);
    EXPECT(pe_re_flag(&saved.progress.flags, PE_RE_FG_ENEMY, (unsigned)flag));
    s.api->resume_after_battle();
    frames(2, 0);
    EXPECT(!s.backdrop && s.active && !s.api->room(&room));
    EXPECT(room.count < before && s.placements == (unsigned)room.count);
    for (int i = 0; i < room.count; i++) EXPECT(room.enemies[i].killed_flag != flag);
    s.save_read(NULL, NULL);
    EXPECT(!s.active);
    frames(30, 0);
    EXPECT(s.active && !strcmp(s.room, "re2:room/100") && !s.api->room(&room));
    EXPECT(room.count < before);
    for (int i = 0; i < room.count; i++) EXPECT(room.enemies[i].killed_flag != flag);

    EXPECT(s.next_door);
    s.next_door();
    frames(1, RE_HOST_CROSS);
    EXPECT(s.hold);
    frames(75, 0);
    EXPECT(!s.hold && s.shade == 128 && strcmp(s.room, "re2:room/100"));
    if (scenario) return 0;

    EXPECT(s.jump && s.stage && s.room_number && s.give_ribbon && s.typewriter && s.save_committed);
    *s.stage = 2;
    *s.room_number = 8;
    s.jump();
    frames(30, 0);
    s.give_ribbon();
    s.typewriter();
    frames(1, RE_HOST_CROSS);
    EXPECT(s.busy && !strcmp(s.text, "Use an Ink Ribbon to save?"));
    s.busy = 0;
    s.choice = 0;
    frames(2, 0);
    EXPECT(s.menu && s.saves == 1);
    s.save_committed(NULL, NULL);
    s.menu = 0;
    frames(1, 0);
    EXPECT(!pe_re_save_decode(s.progress->data, &saved) && !pe_re_has(&saved.progress, PE_RE2_INK_RIBBON));
    return 0;
}

int re_headless_smoke(const char *store, unsigned scenario) {
    memset(&s, 0, sizeof s);
    EXPECT(re_host()->struct_size == sizeof(ReHost));
    if (!store) {
        const PeImpDoor door = {0};
        pe_re_door_begin("re2", &door);
        EXPECT(s.hold && s.shade == 128);
        unsigned swaps = 0;
        while (pe_re_door_busy()) {
            if (pe_re_door_step() == PE_RE_DOOR_SWAP) {
                EXPECT(s.curtain && s.hold && !s.shade);
                swaps++;
            }
        }
        EXPECT(swaps == 1 && !s.hold && !s.curtain && s.shade == 128);
        return 0;
    }
    s.store = store;
    EXPECT(!pe_mod_pe_re_play.init());
    const int result = play(scenario);
    pe_mod_pe_re_play.shutdown();
    return result;
}

static int play_gesture(void) {
    EXPECT(s.start && s.frame && s.scenario && s.stage && s.room_number && s.jump && s.next_use && s.give_use);
    *s.scenario = 1;
    s.start();
    EXPECT(s.active && !strcmp(s.room, "re1:room/106"));

    *s.stage = 1;
    *s.room_number = PE_RE1_PIANO_ROOM;
    s.jump();
    frames(1, 0);
    const int previous_camera = s.camera;
    EXPECT(!strcmp(s.prop.asset_id, "re1:object/10f-0") && s.prop.x == 9100 &&
           s.prop.z == 12576 && s.prop.y == 0 && s.prop.yaw == 1024);
    PeImpModel panel;
    EXPECT(!pe_imp_catalog_model(s.store, s.prop.asset_id, &panel, NULL, 0));
    EXPECT(panel.parts == 1);
    pe_imp_model_free(&panel);
    s.next_use();
    s.give_use();
    frames(1, RE_HOST_SQUARE);
    EXPECT(s.busy);
    s.busy = 0;
    s.choice = 0;
    frames(8, 0);
    EXPECT(s.hold && s.gesture == RE_POSE_PIANO && s.gesture_frame == 8 && !s.busy);
    frames(32, 0);
    EXPECT(s.hold && s.prop.y < 0 && s.prop.y > -5400);
    frames(30, RE_HOST_CROSS);
    EXPECT(!s.hold && s.gesture == RE_POSE_IDLE && s.busy && strstr(s.text, "hidden alcove"));
    EXPECT(s.prop.y == -5400);
    const int expected[] = {6, 1, 7, 1, previous_camera};
    EXPECT(s.camera_count == sizeof expected / sizeof *expected &&
           !memcmp(s.cameras, expected, sizeof expected));
    PeReSaved saved;
    EXPECT(!pe_re_save_decode(s.progress->data, &saved) && saved.camera == previous_camera &&
           pe_re_flag(&saved.progress.flags, PE_RE1_FG_PREVIEW, PE_RE1_PIANO_PLAYED));
    s.save_read(NULL, NULL);
    frames(1, 0);
    EXPECT(!strcmp(s.room, "re1:room/10f") && s.prop.y == -5400);

    *s.room_number = PE_RE1_DINING_ROOM;
    s.jump();
    frames(1, 0);
    EXPECT(!strcmp(s.prop.asset_id, "re1:object/105-2") && s.prop.x == 16000 &&
           s.prop.z == 14300 && s.prop.y == 0 && s.prop.yaw == 1024);
    PeImpModel clock;
    EXPECT(!pe_imp_catalog_model(s.store, s.prop.asset_id, &clock, NULL, 0));
    EXPECT(clock.parts == 1);
    pe_imp_model_free(&clock);
    s.next_use();
    s.give_use();
    const int dining_camera = s.camera;
    frames(1, RE_HOST_SQUARE);
    EXPECT(s.busy);
    s.busy = 0;
    s.choice = 0;
    frames(8, 0);
    EXPECT(s.hold && s.gesture == RE_POSE_REACH && s.prop.x == 16000);
    frames(16, 0);
    EXPECT(s.hold && s.camera == 5 && s.prop.x > 16000 && s.prop.x < 18000);
    frames(24, 0);
    EXPECT(!s.hold && s.camera == dining_camera && s.prop.x == 18000 &&
           s.busy && strstr(s.text, "revealing the Shield Key"));
    EXPECT(!pe_re_save_decode(s.progress->data, &saved) && saved.camera == dining_camera &&
           pe_re_flag(&saved.progress.flags, PE_RE1_FG_PREVIEW, PE_RE1_CLOCK_OPEN));
    s.save_read(NULL, NULL);
    frames(1, 0);
    EXPECT(!strcmp(s.room, "re1:room/105") && s.prop.x == 18000);

    *s.stage = 3;
    *s.room_number = 1;
    s.jump();
    frames(1, 0);
    EXPECT(!strcmp(s.prop.asset_id, "re1:object/301-1") && s.prop.translucent &&
           s.prop.x == 24180 && s.prop.z == 17544 && s.prop.y == 870);
    PeImpModel water;
    EXPECT(!pe_imp_catalog_model(s.store, s.prop.asset_id, &water, NULL, 0));
    EXPECT(water.parts == 1 && water.part[0].tris == 50);
    pe_imp_model_free(&water);
    s.next_use();
    s.give_use();
    const int pool_camera = s.camera;
    frames(1, RE_HOST_SQUARE);
    EXPECT(s.busy);
    s.busy = 0;
    s.choice = 0;
    frames(8, 0);
    EXPECT(s.hold && s.gesture == RE_POSE_REACH && s.prop.y == 870);
    frames(22, 0);
    EXPECT(s.hold && s.camera == 2 && s.prop.y > 870);
    frames(20, 0);
    EXPECT(s.hold && !s.prop.asset_id[0]);
    frames(8, 0);
    EXPECT(!s.hold && s.camera == pool_camera && !s.prop.asset_id[0] &&
           s.busy && strstr(s.text, "pool drained"));
    EXPECT(!pe_re_save_decode(s.progress->data, &saved) && saved.camera == pool_camera &&
           pe_re_flag(&saved.progress.flags, PE_RE1_FG_PREVIEW, PE_RE1_POOL_DRAINED) &&
           pe_re_has(&saved.progress, PE_RE1_SQUARE_CRANK));
    s.save_read(NULL, NULL);
    frames(1, 0);
    EXPECT(!strcmp(s.room, "re1:room/301") && !s.prop.asset_id[0]);

    place(22250, 11223);
    frames(10, RE_HOST_CROSS);
    EXPECT(s.hold && s.gesture == RE_POSE_CLIMB && s.floor == PE_RE1_POOL_FLOOR && s.camera == 1);
    frames(35, 0);
    EXPECT(!s.hold && !s.busy && s.gesture == RE_POSE_IDLE);
    EXPECT(!pe_re_save_decode(s.progress->data, &saved) && saved.floor == PE_RE1_POOL_FLOOR);
    s.save_read(NULL, NULL);
    frames(1, 0);
    EXPECT(s.floor == PE_RE1_POOL_FLOOR && !s.prop.asset_id[0]);
    place(22250, 25146);
    frames(10, RE_HOST_CROSS);
    EXPECT(s.hold && s.gesture == RE_POSE_CLIMB && s.floor == 0 && s.camera == 5);
    frames(35, 0);
    EXPECT(!s.hold && !s.busy && s.floor == 0 && s.gesture == RE_POSE_IDLE);

    *s.stage = PE_RE1_GUARDHOUSE_STAGE + 1;
    *s.room_number = PE_RE1_PLANT_ROOM;
    s.jump();
    frames(1, 0);
    PeReRoomEvent plant_room;
    EXPECT(!s.api->room(&plant_room) && !strcmp(plant_room.game, "re1") && plant_room.count == 1 &&
           plant_room.enemies[0].kind == PE_RE1_PLANT_KIND && plant_room.enemies[0].killed_flag == PE_RE1_PLANT_DEFEAT);
    EXPECT(s.api->pause_for_battle() == 1 && !s.active && s.api->room(&plant_room));
    EXPECT(s.api->defeated(0xff) < 0 && !s.api->defeated(PE_RE1_PLANT_DEFEAT));
    EXPECT(!pe_re_save_decode(s.progress->data, &saved) &&
           pe_re_flag(&saved.progress.flags, PE_RE_FG_ENEMY, PE_RE1_PLANT_DEFEAT));
    s.api->resume_after_battle();
    frames(2, 0);
    EXPECT(!s.api->room(&plant_room) && !plant_room.count);
    s.save_read(NULL, NULL);
    frames(1, 0);
    EXPECT(!s.api->room(&plant_room) && !plant_room.count);
    return 0;
}

int re_headless_re1_gesture(const char *store) {
    memset(&s, 0, sizeof s);
    s.store = store;
    EXPECT(!pe_mod_pe_re_play.init());
    const int rc = play_gesture();
    pe_mod_pe_re_play.shutdown();
    return rc;
}
