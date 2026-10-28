/* Mod host: starts the mods, owns the hook table, services and events.
 *
 * The game calls pe_hook_dispatch() from its PE_HOOK lines (pe_hooks.h).
 * PE_HOOK_INIT starts the host: settings are read, mods start in dependency
 * order, and the Mods panel is installed for interactive runs. Hooks before
 * that do nothing. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pe_game.h"
#include "pe_main_menu.h"
#include "pe_mod_host.h"
#include "pe_mod_menu.h"
#include "pe_pages.h"
#include "pe_present.h"
#include "pe_save_tail.h"

extern const PeMod *const g_pe_mods[]; /* generated: pe_mod_list.c */

enum { PE_MAX_HANDLERS = 256, PE_MAX_EVENTS = 64, PE_MAX_SERVICES = 32, PE_MAX_MODS = 32 };

typedef struct {
    int id, priority, order;
    PeHookFn fn;
    void *user;
    const char *mod_id;
    int active;      /* its mod is active (set when the mods have started) */
} PeHandler;

typedef struct {
    const char *name;
    int version;
    const void *api;
    const char *mod_id;
} PeService;

static PeHandler s_handlers[PE_MAX_HANDLERS];
static int s_handler_count, s_handler_order;
static const char *s_events[PE_MAX_EVENTS];
static int s_event_count;
static PeService s_services[PE_MAX_SERVICES];
static int s_service_count;
static const PeMod *s_running[PE_MAX_MODS];
static int s_running_count;
static int s_started, s_interactive;
static char s_prefs_dir[1024];
static FILE *s_log;

/* ---- files and log ---- */

int pe_mod_path(char *out, size_t size, const char *file) {
    int n = snprintf(out, size, "%s%s", s_prefs_dir, file);
    return n > 0 && (size_t)n < size ? 0 : -1;
}

void pe_mod_log(const char *mod_id, const char *fmt, ...) {
    char line[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    fprintf(stderr, "[mods] %s: %s\n", mod_id ? mod_id : "host", line);
    if (s_log) {
        fprintf(s_log, "%s: %s\n", mod_id ? mod_id : "host", line);
        fflush(s_log);
    }
}

void pe_log(const char *tag, const char *fmt, ...) {
    static FILE *play;
    static int tried, event = -1;
    char path[1100], text[1100];
    va_list args;
    PeLogLine line;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    if (!play && !tried) {
        tried = 1;
        if (pe_mod_path(path, sizeof(path), "pe_play.log") == 0) play = fopen(path, "a");
    }
    if (play) {
        fprintf(play, "%s %s\n", tag, text);
        fflush(play);
    }
    if (event < 0) event = pe_event_id(PE_EVENT_LOG_LINE);
    line.tag = tag;
    line.text = text;
    pe_event_emit(event, &line);                 /* e.g. the detailed play log copies it */
}

int pe_mod_interactive(void) { return s_interactive; }

/* ---- hooks and events ---- */

int pe_hook_add(int id, PeHookFn fn, void *user, int priority, const char *mod_id) {
    int i;
    if (!fn || id < 0 || id >= PE_HOOK_COUNT + s_event_count || s_handler_count >= PE_MAX_HANDLERS)
        return -1;
    /* Keep the table sorted by (id, priority, registration order). */
    for (i = s_handler_count; i > 0; i--) {
        PeHandler *prev = &s_handlers[i - 1];
        if (prev->id < id || (prev->id == id && prev->priority <= priority)) break;
        s_handlers[i] = *prev;
    }
    s_handlers[i] = (PeHandler){id, priority, s_handler_order++, fn, user, mod_id, 1};
    s_handler_count++;
    return 0;
}

int pe_hook_dispatch(int id, void *ctx);

int pe_event_id(const char *name) {
    int i;
    for (i = 0; i < s_event_count; i++)
        if (strcmp(s_events[i], name) == 0) return PE_HOOK_COUNT + i;
    if (s_event_count >= PE_MAX_EVENTS) return -1;
    s_events[s_event_count] = name;
    return PE_HOOK_COUNT + s_event_count++;
}

int pe_event_emit(int id, void *ctx) {
    return id >= PE_HOOK_COUNT ? pe_hook_dispatch(id, ctx) : -1;
}

/* ---- services ---- */

int pe_service_publish(const char *name, int version, const void *api, const char *mod_id) {
    if (pe_service_get(name, version) || s_service_count >= PE_MAX_SERVICES) return -1;
    s_services[s_service_count++] = (PeService){name, version, api, mod_id};
    pe_mod_log(mod_id, "service %s v%d", name, version);
    return 0;
}

/* An inactive mod's services aren't offered: its code must not run through another mod. */
const void *pe_service_get(const char *name, int version) {
    int i;
    for (i = 0; i < s_service_count; i++)
        if (s_services[i].version == version && strcmp(s_services[i].name, name) == 0)
            return pe_mod_active(s_services[i].mod_id) ? s_services[i].api : NULL;
    return NULL;
}

/* ---- start-up ----
 * Every mod's init runs, so its F1 pages are there to browse and set up; but only ACTIVE mods
 * touch the game: the handlers, per-frame ticks, services and main-menu rows of an inactive mod
 * (off in Manage mods, All mods off, or a mod it needs inactive) are skipped. Mods' inits only
 * register, so the game plays as the original with every mod inactive. */
static const char *s_inited_ids[PE_MAX_MODS];
static int s_inited_count;
static const char *s_current_mod;          /* the mod whose init is running */

int pe_mod_is_running(const char *mod_id) {
    int i;
    if (!mod_id) return 0;
    for (i = 0; i < s_running_count; i++)
        if (strcmp(s_running[i]->id, mod_id) == 0) return 1;
    return 0;
}

const char *pe_mod_current(void) { return s_current_mod; }

/* The host's own (NULL, "host", "menu") are always active; a mod while it is starting too. */
int pe_mod_active(const char *mod_id) {
    int i;
    if (!mod_id || !s_started) return 1;
    for (i = 0; i < s_inited_count; i++)
        if (strcmp(s_inited_ids[i], mod_id) == 0) return pe_mod_is_running(mod_id);
    return 1;
}

static int pe_inited(const char *mod_id) {
    int i;
    for (i = 0; i < s_inited_count; i++) if (strcmp(s_inited_ids[i], mod_id) == 0) return 1;
    return 0;
}

static int pe_requires_inited(const PeMod *mod) {
    const char *const *req;
    for (req = mod->requires; req && *req; req++)
        if (!pe_inited(*req)) return 0;
    return 1;
}

static int pe_requires_met(const PeMod *mod) {
    const char *const *req;
    for (req = mod->requires; req && *req; req++)
        if (!pe_mod_is_running(*req)) return 0;
    return 1;
}

static void pe_shutdown_mods(void) {
    while (s_running_count > 0) {
        const PeMod *mod = s_running[--s_running_count];
        if (mod->shutdown) mod->shutdown();
    }
    if (s_log) fclose(s_log);
    s_log = NULL;
}

/* ---- Manage mods (F1): every built-in mod, on or off from the next start ----
 * "All mods" first, OFF by default: until a player turns it on, no mod starts and the game plays
 * as the original (only F1 and this page are there, to turn it on). Then a toggle per mod. All are
 * saved in pe_mods.ini like any row and read before the mods start, so a mod turned off doesn't
 * start at all (nor do the mods that need it). */
static int s_all_on;
static int s_mod_on[PE_MAX_MODS];
static char s_mod_note[PE_MAX_MODS][160];

static void pe_manage_register(int mod_count) {
    int i;
    pe_menu_top_page(PE_MENU_MANAGE, "Turn mods on or off. Changes apply the next time the game starts.", 100000);
    pe_menu_begin(PE_MENU_MANAGE);
    s_all_on = 0;
    pe_menu_register_toggle(0, "All mods", "Off: no mod starts and the game plays as the original. "
                            "Applies the next time the game starts.", &s_all_on, NULL);
    for (i = 0; i < mod_count; i++) {
        s_mod_on[i] = 1;
        pe_menu_register_toggle(0, g_pe_mods[i]->name, s_mod_note[i], &s_mod_on[i], NULL);
    }
    pe_menu_begin(0);
}

/* Each row's footer: what the mod is, what it needs, and how it is now. */
static void pe_manage_notes(int mod_count, const int *state) {
    static const char *const states[] = {
        "Running.", "Off: starts with the game again once turned on.", "Not running: a mod it needs is off or missing.",
        "Not running: it failed to start (see pe_mods.log).", "Not running: All mods is off.",
    };
    int i;
    for (i = 0; i < mod_count; i++) {
        const PeMod *mod = g_pe_mods[i];
        const char *const *req;
        size_t n = (size_t)snprintf(s_mod_note[i], sizeof s_mod_note[0], "%s %s. %s", mod->id, mod->version, states[state[i]]);
        for (req = mod->requires; req && *req && n < sizeof s_mod_note[0]; req++)
            n += (size_t)snprintf(s_mod_note[i] + n, sizeof s_mod_note[0] - n, "%s %s", req == mod->requires ? " Needs" : ",", *req);
    }
}

enum { PE_MOD_RUNNING, PE_MOD_OFF, PE_MOD_NEEDS, PE_MOD_FAILED, PE_MOD_ALL_OFF };

static void pe_start_mods(void) {
    int mod_count = 0, tried[PE_MAX_MODS] = {0}, state[PE_MAX_MODS] = {0}, progress = 1, i;

    while (g_pe_mods[mod_count] && mod_count < PE_MAX_MODS) mod_count++;
    pe_manage_register(mod_count);
    if (!s_all_on) pe_mod_log(NULL, "All mods off (Manage mods): pages browsable, the game plays as the original");
    /* Init every mod once the mods it needs have, in that order; active only if on and every mod
     * it needs is active. */
    while (progress) {
        progress = 0;
        for (i = 0; i < mod_count; i++) {
            const PeMod *mod = g_pe_mods[i];
            int ok;
            if (tried[i] || !pe_requires_inited(mod)) continue;
            tried[i] = progress = 1;
            s_current_mod = mod->id;
            ok = !mod->init || mod->init() == 0;
            s_current_mod = NULL;
            if (!ok) { state[i] = PE_MOD_FAILED; pe_mod_log(mod->id, "init failed, mod off"); continue; }
            s_inited_ids[s_inited_count++] = mod->id;
            if (!s_all_on) state[i] = PE_MOD_ALL_OFF;
            else if (!s_mod_on[i]) state[i] = PE_MOD_OFF;
            else if (!pe_requires_met(mod)) state[i] = PE_MOD_NEEDS;
            else {
                s_running[s_running_count++] = mod;
                pe_mod_log(mod->id, "started %s %s", mod->name, mod->version);
                continue;
            }
            pe_mod_log(mod->id, "inactive (%s): pages only", state[i] == PE_MOD_NEEDS ? "a mod it needs is off" : "off");
        }
    }
    for (i = 0; i < mod_count; i++)
        if (!tried[i]) {
            state[i] = PE_MOD_NEEDS;
            pe_mod_log(g_pe_mods[i]->id, "missing a required mod, mod off");
        }
    pe_manage_notes(mod_count, state);
    atexit(pe_shutdown_mods);
}

static void pe_host_start(const PeHookInit *init) {
    const char *slash;
    char log_path[1100];

    s_interactive = init && init->interactive;
    if (init && init->prefs_file && *init->prefs_file) {
        const char *a = strrchr(init->prefs_file, '/'), *b = strrchr(init->prefs_file, '\\');
        slash = a > b ? a : b;
        if (slash && (size_t)(slash - init->prefs_file + 1) < sizeof(s_prefs_dir))
            snprintf(s_prefs_dir, sizeof(s_prefs_dir), "%.*s",
                     (int)(slash - init->prefs_file + 1), init->prefs_file);
    }
    if (pe_mod_path(log_path, sizeof(log_path), "pe_mods.log") == 0)
        s_log = fopen(log_path, "w");
    pe_mod_log(NULL, "mod host started (mod API %d, %s)", PE_MOD_API_VERSION, s_interactive ? "interactive" : "headless");
    pe_menu_load_settings();
    pe_pages_describe();
    pe_start_mods();
    if (s_all_on) {                              /* off: nothing but F1 touches the game */
        pe_main_menu_init();
        pe_equip_sync_init();
        pe_present_install();
    }
    pe_save_tail_install();                      /* always: a save made with mods off keeps their data too */
    if (s_interactive) pe_panel_install();
    s_started = 1;
    {                                            /* inactive mods' handlers stay registered, skipped */
        int i;
        for (i = 0; i < s_handler_count; i++) s_handlers[i].active = pe_mod_active(s_handlers[i].mod_id);
    }
}

int pe_hook_dispatch(int id, void *ctx) {
    int i, handled = 0;
    if (id == PE_HOOK_INIT && !s_started) pe_host_start((const PeHookInit *)ctx);
    if (!s_started) return 0;
    for (i = 0; i < s_handler_count; i++) {
        if (s_handlers[i].id < id) continue;
        if (s_handlers[i].id > id) break;
        if (s_handlers[i].active && s_handlers[i].fn(ctx, s_handlers[i].user)) handled = 1;
    }
    return handled;
}
