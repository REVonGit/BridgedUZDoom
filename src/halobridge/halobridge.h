// halobridge.h - engine hooks for the HaloDoom bridge (see hdb_native.cpp).
// All of them are no-ops unless UZDoom was started with -hdbridge.
#pragma once

bool HDB_Init();          // d_main.cpp, D_DoomMain, after command-line commands run
void HDB_PumpInput();     // end of I_StartTic() (win32 and posix/sdl)
void HDB_CaptureFrame();
bool HDB_Active();         // attached to Halo (-hdbridge and the memory found)  // d_main.cpp, D_DoomLoop, right after D_Display()
