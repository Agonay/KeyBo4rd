#pragma once

// Rainy-street idle animation for the 1.54" E-Ink panel (partial refreshes only,
// see EinkAnimation.cpp). Driven one frame per call from renderIdleScreen().
void einkAnimTick();        // starts after 5 s idle, draws the next frame when due
void einkAnimStop();        // deep-sleeps the panel; the frame stays and resumes with partials
void einkAnimInvalidate();  // something else drew on the panel: next start needs a full refresh
void einkAnimBootFrame();   // draw the scene at boot instead of the boot image
bool einkAnimRunning();
