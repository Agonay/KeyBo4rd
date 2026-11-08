#pragma once

// "Storm" idle animation for the 1.54" E-Ink panel. Each act shows a different
// refresh technique (full, partial whole-screen, partial window, full-refresh
// flash). Driven one frame per call from renderIdleScreen().
void einkAnimTick();    // starts after 5 s idle, draws the next frame when due
void einkAnimStop();    // hibernates the panel, last frame stays on screen
void einkAnimReset();   // next start replays the full-refresh intro (mode just selected)
bool einkAnimRunning();
