#include "common.h"
#include "pe1/battle_runtime.h"
#include "pe1/field_movement.h"
#include "pe1/game_state.h"
#include "pe1/geom_state.h"
#include "pe1/gte.h"
#include "pe1/gte_types.h"
#include "pe1/pad_state.h"
#include "pe1/projection_slot.h"
#include "pe1/render_camera.h"
#include "pe1/render_lighting.h"
#include "pe1/render_object.h"
#include "pe1/render_prim.h"
#include "pe1/render_tint.h"

#include <math.h>

/* Geometry animation control: reset, update and script setup commands. */

int Render_ResetAllEntries(void) {
    GeomState *state;
    GeomStateAddress base, controls, slots;
    GeomEntry *entries;
    unsigned int i;
    unsigned int count;

    state = g_GeomState;
    base.state = g_GeomState;
    controls.word = base.word + state->ctrl_offset;
    base.word += state->entry_offset;
    entries = base.entry;
    count = state->entry_count;
    for (i = 0; i < count; i++) {
        GeomCtrlEntry *control = &controls.ctrl[i];
        u8 group = control->position.parts.groupFraction;

        slots.ctrl = control;
        slots.word += control->slot_offset;
        control->step = 0x100;
        control->elapsed = 0;
        control->head.b.flags = 1;
        control->position.packed = group;
        entries[*slots.bytes].head.b.flags |= 2;
    }
    return 0;
}

/* Advances the geometry animations of the selected group. */
int Render_StepGroupAnimations(void)
{
    GeomStateAddress state, controls, entries;
    unsigned int i, count;

    if (!(g_GameStateFlags & 0x104) &&
        (g_GameState.flags & 0xC00000) != 0x800000) {
        state.state = g_GeomState;
        controls.word = state.word + state.state->ctrl_offset;
        entries.word = state.word + state.state->entry_offset;
        count = state.state->entry_count;
        for (i = 0; i < count; i++) {
            GeomCtrlEntry *control = &controls.ctrl[i];
            int position = control->position.bits.value;

            if ((control->head.b.flags & 2) &&
                (control->head.b.flags & 0x14) &&
                control->position.bits.group == g_GeomScroll.state.position.groupSelect) {
                unsigned int j;
                GeomStateAddress slotTable;
                GeomAnimationSlot *slots;
                unsigned int frames;
                GeomAnimationSlot *slot;

                slotTable.ctrl = control;
                slotTable.word += control->slot_offset;
                slots = slotTable.slots;
                frames = control->head.packed >> 8;
                for (j = 0; j < frames; j++)
                    entries.entry[slots[j].entry].head.b.flags &= ~2;
                /* Subtraction retains retail's address-add operand order. */
                slot = slots - (-(position >> 8));
                entries.entry[slot->entry].head.b.flags |= 2;
                if (slot->duration < 0) {
                    slot->duration = 0;
                    control->elapsed = 0;
                    control->head.b.flags &= ~4;
                    return 0;
                }
                {
                    unsigned short elapsed = control->elapsed + 1;
                    control->elapsed = elapsed;
                    if (elapsed >= slot->duration) {
                        int next;
                        unsigned int length;

                        control->elapsed = 0;
                        control->position.bits.value = position + control->step;
                        next = control->position.bits.value;
                        length = control->head.packed >> 8;
                        if ((next >> 8) >= (int)length) {
                            if (control->head.b.flags & 0x20)
                                control->position.bits.value = next % (int)(length << 8);
                            else
                                control->position.bits.value = 0;
                            control->head.b.flags &= ~4;
                        } else if ((next >> 8) < 0) {
                            if (control->head.b.flags & 0x20)
                                control->position.bits.value = (int)(length << 8) -
                                    (-next % (int)(length << 8));
                            else
                                control->position.bits.value = (length - 1) << 8;
                            control->head.b.flags &= ~4;
                        }
                    }
                }
            }
        }
    }
    return 0;
}

/* Contiguous commands update the shared geometry animation control table. */

#define GEOM_CTRL_ENTRY(address, base, index) \
    (GEOM_STATE_OFFSET(address, base, ctrl_offset, (index) << 4), (address).ctrl)

int Render_SetEntryLimit(int arg0, int arg1)
{
  GeomStateAddress address, base;
  GeomCtrlEntry *entry;
  u32 positionWord;
  u32 flags;
  u32 hi;
  int ret;
  entry = GEOM_CTRL_ENTRY(address, base, arg0);
  positionWord = entry->position.packed;
  positionWord &= 0xFF;
  ret = entry->head.b.flags;
  flags = ret;
  hi = (u32)arg1 << 16;
  entry->elapsed = 0;
  positionWord |= hi;
  flags |= 2;
  entry->head.b.flags = flags;
  ret = 0;
  entry->position.packed = positionWord;
  return ret;
}

int Render_SetFlagSlot(int arg0, int arg1) {
    GeomStateAddress table, base;
    GeomCtrlEntry *slot;

    GEOM_STATE_OFFSET(table, base, ctrl_offset, 0);
    slot = table.ctrl + arg0;
    if (arg1 != 0) {
        slot->head.b.flags |= 6;
    } else {
        slot->head.b.flags &= 0xF9;
    }

    return 0;
}

s16 Render_GetEntryField6(int index) {
    GeomStateAddress address, base;

    return GEOM_CTRL_ENTRY(address, base, index)->position.parts.frame;
}

int Render_SetEntryField8(int index, unsigned int value) {
    GeomStateAddress address, base;

    GEOM_CTRL_ENTRY(address, base, index)->step = value >> 8;
    return 0;
}

int Render_FillEntrySlotValues(int index, u8 value)
{
    GeomStateAddress address, base, slots;
    GeomCtrlEntry *entry;
    int count;
    int i;

    entry = GEOM_CTRL_ENTRY(address, base, index);
    slots.ctrl = entry;
    slots.word += entry->slot_offset;
    count = entry->head.packed >> 8;
    for (i = 0; i < count; i++) {
        slots.slots[i].duration = value;
    }
    return 0;
}

int Render_SetEntrySlotValue(int index, int slot, u8 value) {
    GeomStateAddress address, base;
    GeomCtrlEntry *entry;
    int ret;

    entry = GEOM_CTRL_ENTRY(address, base, index);
    address.word += entry->slot_offset;
    ret = 0;
    address.slots[slot].duration = value;
    return ret;
}

int Render_SetEntryFlags(int index, int bits) {
    GeomStateAddress address, base;

    GEOM_CTRL_ENTRY(address, base, index)->head.b.flags |= bits & 0x30;
    return 0;
}

/* Set the animation start frame, stop slot and direction. */
int Menu_SetSlotEntry(int index, unsigned int limit, unsigned int slot)
{
    GeomStateAddress table, base;
    GeomCtrlEntry *entry;
    u32 positionWord;
    u32 flags;
    u32 hi;
    GeomAnimationSlot *slots;
    s32 offset;
    int neg;
    s16 f8;

    GEOM_STATE_OFFSET(table, base, ctrl_offset, index << 4);
    entry = table.ctrl;
    positionWord = entry->position.packed;
    positionWord &= 0xFF;
    offset = entry->slot_offset;
    hi = limit << 16;

        entry->elapsed = 0;
        positionWord |= hi;
        {
            flags = entry->head.b.flags;
            table.word += offset;
            slots = table.slots;
            entry->position.packed = positionWord;
        }
        neg = -1;
        flags |= 6;
        entry->head.b.flags = flags;
        slots[slot].duration = neg;
        f8 = entry->step;
        if ((f8 > 0 && slot < limit) || (f8 < 0 && limit < slot)) {
            entry->step = -(u16)entry->step;
        }
        return 0;

}

void SetGeomScreen(int h);

int Render_LoadGeomState(int index) {
    GeomStateAddress table, base;
    CameraViewport *entry;
    u16 *dst;
    int value;

    GEOM_STATE_OFFSET(table, base, entry_offset_1C, index * 52);
    entry = table.viewport;
    *g_GeomScroll.state.position.projectionDistance = entry->prefix.gpu.geom_screen;
    SetGeomScreen(entry->prefix.gpu.geom_screen);

    dst = (u16 *)g_GeomScroll.words.viewMatrix;
    dst[0] = entry->prefix.gpu.half02;
    dst = (u16 *)g_GeomScroll.words.viewMatrix;
    dst[1] = entry->prefix.gpu.half04;
    dst = (u16 *)g_GeomScroll.words.viewMatrix;
    dst[2] = entry->prefix.gpu.half06;
    dst = (u16 *)g_GeomScroll.words.viewMatrix;
    dst[3] = entry->prefix.gpu.half08;
    dst = (u16 *)g_GeomScroll.words.viewMatrix;
    dst[4] = entry->prefix.gpu.half0A;
    dst = (u16 *)g_GeomScroll.words.viewMatrix;
    dst[5] = entry->prefix.gpu.half0C;
    dst = (u16 *)g_GeomScroll.words.viewMatrix;
    dst[6] = entry->prefix.gpu.half0E;
    dst = (u16 *)g_GeomScroll.words.viewMatrix;
    dst[7] = entry->prefix.gpu.half10;
    dst = (u16 *)g_GeomScroll.words.viewMatrix;
    dst[8] = entry->prefix.gpu.half12;
    dst = (u16 *)g_GeomScroll.words.viewMatrix;
    ((u32 *)dst)[5] = entry->prefix.gpu.word14;
    dst = (u16 *)g_GeomScroll.words.viewMatrix;
    ((u32 *)dst)[6] = entry->prefix.gpu.word18;
    dst = (u16 *)g_GeomScroll.words.viewMatrix;
    ((u32 *)dst)[7] = entry->prefix.gpu.word1C;

    g_GeomScroll.state.position.groupSelect = index;
    value = g_GeomScroll.state.flags;
    g_GeomScroll.state.flags = value | 0x80;
    return 0;
}

int CdRom_SetSeekPos(unsigned int arg0) {
    unsigned short old0;
    unsigned short old1;
    unsigned short old2;

    old0 = (u16)g_ScreenTint.targetR;
    old1 = g_ScreenTint.targetG;
    old2 = (u16)g_ScreenTint.targetB;

    g_ScreenTint.targetR = 0xFF;
    g_ScreenTint.targetG = 0xFF;
    g_ScreenTint.targetB = 0xFF;
    g_ScreenTint.fadeMode = 2;
    g_ScreenTint.blendMode = 2;
    g_ScreenTint.duration = arg0;
    g_ScreenTint.frame = 0;
    g_ScreenTint.startR = old0;
    g_ScreenTint.startG = old1;
    g_ScreenTint.startB = old2;

    return 0;
}

int CdRom_SetScreenPos(int arg0, int arg1, int arg2, int arg3, unsigned short arg4) {
    unsigned short old0;
    unsigned short old1;
    unsigned short old2;
    int mode;

    old0 = (u16)g_ScreenTint.targetR;
    old1 = g_ScreenTint.targetG;
    old2 = (u16)g_ScreenTint.targetB;

    g_ScreenTint.targetR = arg1;
    g_ScreenTint.targetG = arg2;
    g_ScreenTint.targetB = arg3;
    g_ScreenTint.fadeMode = 2;
    g_ScreenTint.duration = arg0;
    g_ScreenTint.frame = 0;
    g_ScreenTint.startR = old0;
    mode = arg4;
    g_ScreenTint.startG = old1;
    g_ScreenTint.startB = old2;

    if (mode >= 4) {
        g_ScreenTint.blendMode = 1;
        return 0;
    }
    if (mode < 0) {
        g_ScreenTint.blendMode = 1;
        return 0;
    }

    g_ScreenTint.blendMode = arg4;
    return 0;
}

int Render_SetFadeColour(unsigned int arg0) {
    unsigned short old0;
    unsigned short old1;
    unsigned short old2;

    old0 = (u16)g_ScreenTint.targetR;
    old1 = g_ScreenTint.targetG;
    old2 = (u16)g_ScreenTint.targetB;

    g_ScreenTint.fadeMode = 6;
    g_ScreenTint.targetR = 0;
    g_ScreenTint.targetG = 0;
    g_ScreenTint.targetB = 0;
    g_ScreenTint.duration = arg0;
    g_ScreenTint.frame = 0;
    g_ScreenTint.startR = old0;
    g_ScreenTint.startG = old1;
    g_ScreenTint.startB = old2;

    return 0;
}

/* Build the camera basis matrix from the camera yaw (plus the player's
 * facing quadrant while aiming): forward = R * Z, right = Y x forward,
 * up = forward x right. */
int Render_DrawSprite(void)
{
    GteMatrixStorage rotation;
    GteShortVector axis;
    GteVector up, right, newUp, forward;
    int angle, cosine, sine;
    if ((g_PadPackets[0].header & 0xF000) == 0x7000)
        angle = g_GeomScroll.state.position.baseAngleAnalog;
    else
        angle = g_GeomScroll.state.position.baseAngleDigital;
    {
        int masked = angle & 4095;
        if (D_8009D254 && (D_8009D2E8 & 16)) {
            angle += 2048;
            angle += (((unsigned int)((Combatant *)D_8009D254->core)->stateFlags) >> 7) & 0xC00;
            masked = angle & 4095;
        }
        cosine = rcos(masked);
        sine = rsin(masked);
    }
    rotation.matrix.m[0][2] = sine;
    rotation.matrix.m[2][0] = -sine;
    rotation.matrix.m[1][1] = 4096;
    axis.z = 4096;
    rotation.matrix.m[0][0] = cosine;
    rotation.matrix.m[2][2] = cosine;
    rotation.matrix.t[0] = rotation.matrix.t[1] = rotation.matrix.t[2] = 0;
    rotation.matrix.m[0][1] = rotation.matrix.m[1][0] = rotation.matrix.m[1][2] = rotation.matrix.m[2][1] = 0;
    axis.x = 0;
    axis.y = 0;
    gte_ldrotmatrix(&rotation);
    gte_ldtransmatrix(&rotation);
    gte_ldv0(&axis);
    gte_rt();
    gte_stlvnl(&forward);
    up.y = 4096;
    up.x = 0;
    up.z = 0;
    gte_ldopv1_psyq(&up);
    gte_ldopv2(&forward);
    gte_op12_psyq();
    gte_stlvnl(&right);
    gte_ldopv1_psyq(&forward);
    gte_ldopv2(&right);
    gte_op12_psyq();
    gte_stlvnl(&newUp);
    g_GeomScroll.state.position.cameraBasis.t[0] = 0;
    g_GeomScroll.state.position.cameraBasis.t[1] = 0;
    g_GeomScroll.state.position.cameraBasis.t[2] = 0;
    g_GeomScroll.state.position.cameraBasis.m[0][0] = right.x;
    g_GeomScroll.state.position.cameraBasis.m[0][1] = newUp.x;
    g_GeomScroll.state.position.cameraBasis.m[0][2] = forward.x;
    g_GeomScroll.state.position.cameraBasis.m[1][0] = right.y;
    g_GeomScroll.state.position.cameraBasis.m[1][1] = newUp.y;
    g_GeomScroll.state.position.cameraBasis.m[1][2] = forward.y;
    g_GeomScroll.state.position.cameraBasis.m[2][0] = right.z;
    g_GeomScroll.state.position.cameraBasis.m[2][1] = newUp.z;
    g_GeomScroll.state.position.cameraBasis.m[2][2] = forward.z;
    return 0;
}


GeomScrollRecord g_GeomScroll;

/* Resets the view state at the start of a room draw: render flags, the
 * scroll and camera words, the light colour, the mesh group and depth, and
 * points the view at the given matrix and projection distance. */
int Render_InitViewState(void *matrix, void *screen) {
    g_GeomScroll.words.flags = 0x70;
    g_GeomScroll.state.position.meshTintLevel = 0x60;
    g_GeomScroll.state.position.focusHeight = 0x180;
    g_RoomLightTint.b = 0xFF;
    g_RoomLightTint.g = 0xFF;
    g_RoomLightTint.r = 0xFF;
    g_GeomScroll.words.position = 0;
    g_GeomScroll.words.saved = 0;
    g_GeomScroll.words.screenOffset = 0;
    g_GeomScroll.words.start = 0;
    g_GeomScroll.words.target = 0;
    g_GeomScroll.words.timer = 0;
    g_GeomScroll.words.viewMatrix = matrix;
    g_GeomScroll.words.projectionDistance = screen;
    g_GeomScroll.words.savedBoundsX = 0;
    g_GeomScroll.words.savedBoundsY = 0;
    g_GeomScroll.words.cameraOffset = 0;
    g_GeomScroll.state.position.groupSelect = 0;
    g_GeomScroll.state.position.baseAngleAnalog = 0;
    g_GeomScroll.state.position.baseAngleDigital = 0;
    g_GeomScroll.state.position.otZ = 0;
    g_GeomScroll.words.origin = 0;
    return 0;
}

/* Clamps the camera position into the selected group's viewport. */
s32 Render_ClampCameraPosition(s32 x, s32 y)
{
    u16 cameraX = x;

    if (g_GeomScroll.state.flags & 0x40) {
        GeomStateAddress address, base;
        CameraViewport *view;

        GEOM_STATE_OFFSET(address, base, entry_offset_1C, 0);
        view = &address.viewport[g_GeomScroll.state.position.groupSelect];
        if ((s16)cameraX < view->minX)
            g_GeomScroll.state.position.x = view->minX;
        else if (view->maxX < (s16)cameraX)
            g_GeomScroll.state.position.x = view->maxX;
        else
            g_GeomScroll.state.position.x = x;
        if ((s16)y < view->minY) {
            g_GeomScroll.state.position.y = view->minY;
            return 0;
        }
        if (view->maxY < (s16)y) {
            g_GeomScroll.state.position.y = view->maxY;
            return 0;
        }
        g_GeomScroll.state.position.y = y;
    }
    return 0;
}
/* Saves the selected viewport's bounds and opens them to the full range. */
int Render_SaveAndOpenBounds(void) {
    GeomStateAddress address, base;
    CameraViewport *view;

    GEOM_STATE_OFFSET(address, base, entry_offset_1C, 0);
    view = &address.viewport[g_GeomScroll.state.position.groupSelect];
    g_GeomScroll.state.position.boundsMinX = view->minX;
    g_GeomScroll.state.position.boundsMaxX = view->maxX;
    g_GeomScroll.state.position.boundsMinY = view->minY;
    g_GeomScroll.state.position.boundsMaxY = view->maxY;
    view->minX = -0x8000;
    view->maxX = 0x7FFF;
    view->minY = -0x8000;
    view->maxY = 0x7FFF;
    return 0;
}

/* Restores the bounds saved by Render_SaveAndOpenBounds. */
int Render_RestoreBounds(void) {
    GeomStateAddress address, base;
    CameraViewport *view;

    GEOM_STATE_OFFSET(address, base, entry_offset_1C, 0);
    view = &address.viewport[g_GeomScroll.state.position.groupSelect];
    view->minX = g_GeomScroll.state.position.boundsMinX;
    view->maxX = g_GeomScroll.state.position.boundsMaxX;
    view->minY = g_GeomScroll.state.position.boundsMinY;
    view->maxY = g_GeomScroll.state.position.boundsMaxY;
    return 0;
}

/* Widescreen: the camera follows the point projected to whole pixels
 * and eases by halves, dropping the fraction at both steps, so a slow pan
 * moves in uneven whole-pixel steps (2, 2, 1, ...). The same steps are kept
 * here without rounding, and the field is drawn shifted by the part the game
 * dropped (Render_GetCameraSubpixel); the game's own camera is unchanged. */
static struct {
    double offset_x, offset_y; /* cameraOffsetX/Y without rounding */
    double target_x, target_y; /* camera position without rounding, clamped */
    s16 game_x, game_y;        /* the game's cameraOffsetX/Y offset_x/y follow */
    s16 position_x, position_y; /* the game's camera position this frame */
    u16 screen_x, screen_y;    /* the game's GTE screen offset this frame */
    u8 tracking;
    u8 fresh;
} s_ExactCamera;

/* RTPS of the full 16.16 point (the game drops the fraction first). */
static int Render_ProjectExact(const GteMatrix *matrix, const s32 *position, s32 distance,
                               double *x, double *y) {
    double point[3];
    double view[3];
    int i;

    for (i = 0; i < 3; ++i)
        point[i] = position[i] / 65536.0;
    for (i = 0; i < 3; ++i)
        view[i] = matrix->t[i] + (matrix->m[i][0] * point[0] + matrix->m[i][1] * point[1] +
                                  matrix->m[i][2] * point[2]) / 4096.0;
    if (view[2] < 1.0)
        return 0;
    *x = 160.0 + view[0] * distance / view[2];
    *y = 112.0 + view[1] * distance / view[2];
    return 1;
}

static double Render_ClampExact(double value, int low, int high) {
    return value < low ? low : (value > high ? high : value);
}

int Render_GetCameraSubpixel(float *dx, float *dy, int *camera_x) {
    const GeomScrollCoordinates *position = &g_GeomScroll.state.position;
    int fresh = s_ExactCamera.fresh;

    *dx = *dy = 0.0f;
    *camera_x = position->x;
    s_ExactCamera.fresh = 0;
    if (!fresh || position->x != s_ExactCamera.position_x ||
        position->y != s_ExactCamera.position_y ||
        position->screenOffsetX != s_ExactCamera.screen_x ||
        position->screenOffsetY != s_ExactCamera.screen_y)
        return 0;
    *dx = (float)Render_ClampExact(s_ExactCamera.position_x - s_ExactCamera.target_x, -2, 2);
    *dy = (float)Render_ClampExact(s_ExactCamera.position_y - s_ExactCamera.target_y, -2, 2);
    return 1;
}

/* Follow the projected world position within the selected viewport. */

int Render_SetViewport(const s32 *position) {
    struct {
        s16 point[4];
        s16 projected[2];
        unsigned reserved[10];
    } local = {0};
    GeomScrollState *camera = &g_GeomScroll.state;
    GeomStateAddress geometry, viewAddress;
    CameraViewport *view;
    unsigned int cx;
    GeomStateAddress offset_geometry;
    int flags = camera->flags;
    int screen_x;
    int screen_y;
    int sum_x;
    int sum_y;
    int view_x;
    int view_y;
    int half_width;
    int half_height;
    int axisValue;
    int viewportValue;
    unsigned raw_width;
    int target_x;
    int target_y;
    int y_bound;
    int selected_x;

    if (flags & 7)
        return -23;
    cx = 160;
    if (!(flags & 64))
        return -24;
    {
        int value = position[0] >> 16;

        local.point[0] = value;
    }
    {
        int value = position[1] >> 16;

        local.point[1] = value;
    }
    {
        int value = position[2] >> 16;

        local.point[2] = value;
    }
    {
        unsigned int cy = 112;
        gte_SetGeomOffset(cx, cy);
    }
    {
        u32 **address = &camera->position.matrixWords;
        {
            const GteMatrixWords *words;
            words = (const GteMatrixWords *)*address;
            gte_ldrotmatrix(words);
            gte_ldtransmatrix(words);
        }
    }
    {
        int distance = *g_GeomScroll.state.position.projectionDistance;
        gte_ctc2_26(distance);
    }
    gte_ldv0(local.point);
    gte_rtps();
    gte_stsxy2(local.projected);
    screen_x = local.projected[0];
    screen_y = local.projected[1];
    {
        double exact_x = screen_x;
        double exact_y = screen_y;

        /* Far from the GTE's result (clamped or behind the camera): keep it. */
        if (!Render_ProjectExact((const GteMatrix *)camera->position.matrixWords, position,
                                 *camera->position.projectionDistance, &exact_x, &exact_y) ||
            fabs(exact_x - screen_x) > 2.0 || fabs(exact_y - screen_y) > 2.0) {
            exact_x = screen_x;
            exact_y = screen_y;
        }
        if (camera->flags & 128) {
            s_ExactCamera.offset_x = exact_x;
            s_ExactCamera.offset_y = exact_y;
        } else if (!s_ExactCamera.tracking ||
                   camera->position.cameraOffsetX != s_ExactCamera.game_x ||
                   camera->position.cameraOffsetY != s_ExactCamera.game_y) {
            /* something else moved the camera: start from the game's */
            s_ExactCamera.offset_x = camera->position.cameraOffsetX;
            s_ExactCamera.offset_y = camera->position.cameraOffsetY;
        }
        s_ExactCamera.offset_x = (s_ExactCamera.offset_x + exact_x) * 0.5;
        s_ExactCamera.offset_y = (s_ExactCamera.offset_y + exact_y) * 0.5;
    }
    if (camera->flags & 128) {
        g_GeomScroll.state.position.cameraOffsetX = screen_x;
        g_GeomScroll.state.position.cameraOffsetY = screen_y;
    }
    geometry.state = g_GeomState;
    sum_x = g_GeomScroll.state.position.cameraOffsetX + screen_x;
    screen_x = sum_x / 2;
    g_GeomScroll.state.position.cameraOffsetX = screen_x;
    sum_y = g_GeomScroll.state.position.cameraOffsetY + screen_y;
    sum_y += (unsigned)sum_y >> 31;
    view_x = 320 - screen_x;

    offset_geometry.state = g_GeomState;

    screen_y = sum_y >> 1;
    g_GeomScroll.state.position.cameraOffsetY = screen_y;
    viewAddress.word = geometry.word + offset_geometry.state->entry_offset_1C;
    view = viewAddress.viewport;
    view += g_GeomScroll.state.position.groupSelect;
    viewportValue = 224;

    raw_width = view->width;
    view_y = viewportValue - screen_y;
    raw_width <<= 16;
    viewportValue = (int)raw_width >> 16;
    viewportValue += raw_width >> 31;
    half_width = viewportValue >> 1;
    axisValue = half_width - 160;

    viewportValue = view->height;
    target_x = axisValue + screen_x;
    viewportValue = (unsigned)viewportValue << 16;
    axisValue = viewportValue >> 16;
    axisValue += (unsigned)viewportValue >> 31;
    half_height = axisValue >> 1;
    target_y = half_height + (screen_y - 112);
    viewportValue = view->minX;
    if (target_x < viewportValue ||
        (viewportValue = view->maxX, viewportValue < target_x)) {
        axisValue = viewportValue - 160;
        view_x = half_width - axisValue;
    }
    y_bound = view->minY;
    if (target_y < y_bound ||
        (y_bound = view->maxY, y_bound < target_y)) {
        axisValue = view->height;
        viewportValue = (s16)axisValue / 2;
        axisValue = y_bound - 112;
        view_y = viewportValue - axisValue;
    }
    g_GeomScroll.state.position.screenOffsetX = view_x;
    g_GeomScroll.state.position.screenOffsetY = view_y;
    s_ExactCamera.game_x = g_GeomScroll.state.position.cameraOffsetX;
    s_ExactCamera.game_y = g_GeomScroll.state.position.cameraOffsetY;
    s_ExactCamera.tracking = 1;
    s_ExactCamera.target_x = Render_ClampExact(half_width - 160 + s_ExactCamera.offset_x,
                                               view->minX, view->maxX);
    s_ExactCamera.target_y = Render_ClampExact(half_height - 112 + s_ExactCamera.offset_y,
                                               view->minY, view->maxY);
    s_ExactCamera.position_x = (s16)Render_ClampExact(target_x, view->minX, view->maxX);
    s_ExactCamera.position_y = (s16)Render_ClampExact(target_y, view->minY, view->maxY);
    s_ExactCamera.screen_x = (u16)view_x;
    s_ExactCamera.screen_y = (u16)view_y;
    s_ExactCamera.fresh = 1;
    selected_x = target_x;
    if (g_GeomScroll.state.flags & 64) {
        int signed_y;
        s16 chosen_bound;
        int upper_y;
        geometry.state = g_GeomState;
        viewAddress.word = geometry.word + geometry.state->entry_offset_1C;
        view = viewAddress.viewport;
        view += g_GeomScroll.state.position.groupSelect;
        viewportValue = (s16)target_x;
        axisValue = view->minX;
        chosen_bound = axisValue;
        if (viewportValue < axisValue) {
            signed_y = (unsigned)target_y << 16;
            g_GeomScroll.state.position.x = chosen_bound;
        } else {
            axisValue = view->maxX;
            chosen_bound = axisValue;
            if (axisValue < viewportValue) {
                signed_y = (unsigned)target_y << 16;
                g_GeomScroll.state.position.x = chosen_bound;
            } else {
                signed_y = (unsigned)target_y << 16;
                g_GeomScroll.state.position.x = selected_x;
            }
        }
        signed_y >>= 16;
        axisValue = view->minY;
        chosen_bound = axisValue;
        if (signed_y < axisValue) {
            g_GeomScroll.state.position.y = chosen_bound;
            return 0;
        }
        axisValue = view->maxY;
        upper_y = axisValue;
        if (axisValue < signed_y) {
            g_GeomScroll.state.position.y = upper_y;
            return 0;
        }
        g_GeomScroll.state.position.y = target_y;
    }
    return 0;
}
/* Moves the GTE screen offset to the current screen offset. */
int Render_SetGteScreenOffset(void) {
    gte_SetGeomOffset(g_GeomScroll.state.position.screenOffsetX,
                      g_GeomScroll.state.position.screenOffsetY);
    return 0;
}

/* Restores the GTE screen offset to the screen centre. */
int Render_ResetGteScreenOffset(void) {
    gte_SetGeomOffset(160, 112);
    return 0;
}

/* Starts a camera scroll from the current position to (x, y) over
 * `duration` frames; mode 8 selects the smoothed step. */
int Render_SetScrollMode(int x, int y, int duration, int mode) {
    s32 *flags = &g_GeomScroll.state.flags;
    int value = *flags;
    u32 position;
    int bits;

    if (!(value & 0x40))
        return -19;
    position = g_GeomScroll.words.position;
    g_GeomScroll.state.position.elapsed = 1;
    g_GeomScroll.state.position.targetX = x;
    g_GeomScroll.state.position.targetY = y;
    g_GeomScroll.state.position.duration = duration;
    g_GeomScroll.words.start = position;
    position = value & ~0xF;
    if (mode != 8)
        bits = position | 1;
    else
        bits = position | 9;
    *flags = bits;
    return 0;
}

/* Advance the camera transition, then center the selected viewport. */
int Render_StepCameraTransition(void)
{
    unsigned int flags = g_GeomScroll.state.flags;
    int mode, smooth;
    u16 dx, dy, duration;
    if (!(flags & 0x40))
        return -20;
    mode = flags & 7;
    smooth = flags & 8;
    if (mode != 1 && mode != 2 && mode != 3)
        return 0;
    {
        u16 targetX = g_GeomScroll.state.position.targetX, x = g_GeomScroll.state.position.startX;
        u16 targetY = g_GeomScroll.state.position.targetY, y = g_GeomScroll.state.position.startY;
        u16 time = g_GeomScroll.state.position.elapsed;
        duration = g_GeomScroll.state.position.duration;
        dx = targetX - x;
        dy = targetY - y;
        if (!smooth) {
            int movedX = (short)dx * (short)time / (short)duration;
            int movedY = (short)dy * (short)time / (short)duration;
            g_GeomScroll.state.position.x = x + movedX;
            g_GeomScroll.state.position.y = y + movedY;
        } else {
            int weight = rcos((short)time * 2048 / (short)duration + 2048) + 4096;
            int movedX = (short)dx * weight / 8192;
            int movedY = (short)dy * weight / 8192;
            g_GeomScroll.state.position.x = g_GeomScroll.state.position.startX + movedX;
            g_GeomScroll.state.position.y = g_GeomScroll.state.position.startY + movedY;
        }
    }
    {
        GeomStateAddress views, base;
        CameraViewport *view;
        GEOM_STATE_OFFSET(views, base, entry_offset_1C, 0);
        view = &views.viewport[g_GeomScroll.state.position.groupSelect];
        {
            unsigned int offset = 160 - (g_GeomScroll.state.position.x - (short)view->width / 2);
            g_GeomScroll.state.position.screenOffsetX = offset;
        }
        {
            unsigned int offset = 112 - (g_GeomScroll.state.position.y - (short)view->height / 2);
            g_GeomScroll.state.position.screenOffsetY = offset;
        }
    }
    if (mode == 1)
        g_GeomScroll.state.flags = (g_GeomScroll.state.flags & ~7) | 2;
    {
        u16 time = g_GeomScroll.state.position.elapsed + 1;
        g_GeomScroll.state.position.elapsed = time;
        if ((short)time > (short)duration) {
            unsigned int completed = g_GeomScroll.state.flags;
            if ((completed & 7) == 2)
                g_GeomScroll.state.flags = (completed & ~7) | 0x84;
            else
                g_GeomScroll.state.flags = (completed & ~7) | 0x80;
        }
    }
    return 0;
}

/* Project a world-space point and start a clamped camera transition. */
/* Starts a camera scroll towards the projection of a world position,
 * clamped to the selected viewport (duration -1 = 30 frames, mode -1 =
 * plain step). */
int Render_UpdateScrollPosition(void *positionArg, int duration, int mode)
{
    int *position = positionArg;
    GteShortVector vector = {0};
    s16 projected[2];
    GeomScrollState *camera = &g_GeomScroll.state;
    CameraViewport *view;
    int x, y;

    if (!(camera->flags & 64))
        return -21;
    g_GeomScroll.state.position.startX = g_GeomScroll.state.position.x;
    g_GeomScroll.state.position.startY = (u16)g_GeomScroll.state.position.y;
    vector.x = position[0] >> 16;
    vector.y = (position[1] >> 16) - (u16)g_GeomScroll.state.position.focusHeight;
    vector.z = position[2] >> 16;
    gte_SetGeomOffset(160, 112);
    {
        GteMatrix **slot = &g_GeomScroll.words.viewMatrix;
        GteMatrix *matrix;

        matrix = *slot;
        gte_ldrotmatrix(matrix);
        gte_ldtransmatrix(matrix);
    }
    gte_ldv0(&vector);
    gte_rtps();
    gte_stsxy2(projected);
    {
        GeomState *state = g_GeomState;
        GeomStateAddress table;

        table.state = g_GeomState;
        table.word += state->entry_offset_1C;
        view = &table.viewport[g_GeomScroll.state.position.groupSelect];
    }
    {
        u16 width = view->width,height = view->height;
        int halfX = (short)width/2-160,halfY = (short)height/2-112;
        x = halfX+projected[0];
        y = halfY+projected[1];
    }
    if (x < view->minX)
        x = view->minX;
    else if (x > view->maxX)
        x = view->maxX;
    if (y < view->minY)
        y = view->minY;
    else if (y > view->maxY)
        y = view->maxY;
    g_GeomScroll.state.position.targetX = x;
    g_GeomScroll.state.position.targetY = y;
    if (duration == -1)
        g_GeomScroll.state.position.duration = 30;
    else
        g_GeomScroll.state.position.duration = duration;
    g_GeomScroll.state.position.elapsed = 1;
    if (mode == -1)
        g_GeomScroll.state.flags = (g_GeomScroll.state.flags & ~7) | 3;
    else {
        unsigned int flags = g_GeomScroll.state.flags & ~15;
        unsigned int bits = mode | 3;
        g_GeomScroll.state.flags = flags | bits;
    }
    return 0;
}
