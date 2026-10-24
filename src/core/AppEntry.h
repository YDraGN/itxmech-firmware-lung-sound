#pragma once
// Each build environment links exactly one implementation of these
// (app/App.cpp for production, debug/StreamApp.cpp for bench streaming).
void appSetup();
void appLoop();
