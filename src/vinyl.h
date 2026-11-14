#pragma once

#include <d2d1.h>

/* Registers the vinyl effect with `factory` and starts compiling its shader on a worker thread. The first
   VinylDraw waits for the compiler if needed; if compiling or registering fails, VinylDraw returns false */
void VinylInit(ID2D1Factory* factory);

/* Draws the vinyl of a record centered at (cx, cy), in DIPs. The label area, `label` times the radius,
   stays empty for the caller to fill. `color` is the vinyl's 0xRRGGBB. Returns false without the effect */
bool VinylDraw(ID2D1RenderTarget* rt, float cx, float cy, float radius, float label,
               float angleDeg, unsigned seed, UINT32 color, float opacity);

/* Releases the effect, which belongs to the render target it was first drawn on */
void VinylRelease();
