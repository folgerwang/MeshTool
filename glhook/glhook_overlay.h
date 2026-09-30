#pragma once

#define NOGDI
#include <windows.h>
#undef NOGDI
#include <cstdint>

// Overlay rendered inside Google Earth's OpenGL context.
// Draws a small HUD in the corner showing MeshTool capture status.

void OverlayInit();
void OverlayRender(void* hdc);
void OverlayCheckHotkey();  // Check F12 for capture trigger
