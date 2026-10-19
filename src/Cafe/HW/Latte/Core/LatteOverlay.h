#pragma once

void LatteOverlay_init();
void LatteOverlay_render(bool pad_view);
#if defined(CEMU_UWP)
void LatteOverlay_renderTouchCursor();
#endif
void LatteOverlay_updateStats(double fps, sint32 drawcalls, sint32 fastDrawcalls);

void LatteOverlay_pushNotification(const std::string& text, sint32 duration);