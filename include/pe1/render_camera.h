#ifndef PE1_RENDER_CAMERA_H
#define PE1_RENDER_CAMERA_H

#include "pe1/gte_types.h"

int Render_DrawSprite(void);
int Render_SetGteScreenOffset(void);
int Render_ResetGteScreenOffset(void);

/* Camera initialization scratch: matrix words followed by projection distance. */
extern u32 g_EntityRenderScratch[9];
int Render_SetViewport(const s32 *position);
/* Widescreen: where the field belongs past where the frame Render_SetViewport
 * just placed draws it (game pixels), and the camera's whole-pixel x it is
 * drawn for; dx, dy 0 and returns 0 when it does not apply. */
int Render_GetCameraSubpixel(float *dx, float *dy, int *camera_x);
int Render_SetScrollMode(int x, int y, int duration, int mode);
int Render_SaveAndOpenBounds(void);
int Render_RestoreBounds(void);
s32 Render_ClampCameraPosition(s32 x, s32 y);
int Render_InitViewState(void *matrix, void *screen);
int Render_DrawMeshList(void *buffer, void **end);
int Render_ResetAllEntries(void);
void SetGeomScreen(int distance);
int Render_PrepareFrame(void);

int CdRom_SetSeekPos(unsigned int arg0);
int Render_LoadGeomState(int index);
int Render_SetFadeColour(unsigned int arg0);
int CdRom_SetScreenPos(int arg0, int arg1, int arg2, int arg3, unsigned short arg4);

#endif
