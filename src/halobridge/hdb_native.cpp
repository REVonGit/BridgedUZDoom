// hdb_native.cpp - UZDoom side of the HaloDoom bridge.
//
// ZScript can't open shared memory, so this small native module does it and
// exposes a `HaloBridge` struct to ZScript; the gameplay logic itself lives in
// HaloDoomBridge.pk3.
//
// Active only when UZDoom is started with -hdbridge. Without it every native
// reports "inactive" and UZDoom behaves exactly as stock.
//
// Hooks (one line each):
//   d_main.cpp   D_DoomMain, after command-line commands run:  HDB_Init();
//   d_main.cpp   D_DoomLoop, right after D_Display():          HDB_CaptureFrame();
//                (which draws the frame a second time: see there)
//   win32/i_input.cpp and posix/sdl/i_input.cpp,
//                end of I_StartTic():                          HDB_PumpInput();
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#ifndef __APPLE__
#include <SDL2/SDL.h>      // the SDL backend; macOS uses its own Cocoa one
#endif
#endif
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <thread>
#include <unordered_set>

#include "hdb_protocol.h"
#include "halobridge.h"
#include "vm.h"
#include "d_eventbase.h"
#include "keydef.h"
#include "c_dispatch.h"
#include "c_cvars.h"
#include "v_video.h"
#include "m_argv.h"
#include "m_png.h"
#include "printf.h"
#include "g_game.h"
#include "cmdlib.h"
#include "d_main.h"
#include "texturemanager.h"
#include "m_joy.h"
#include "menustate.h"
#include "d_gui.h"
#include "c_console.h"
#include "filesystem.h"
FString G_BuildSaveName(const char* prefix);   // common/menu/savegamemanager.cpp

FARG_CUSTOM(hdbridge, "-hdbridge", "Other", true,
	"Attach to a running Halo CE (HaloDoom bridge).", "",
	"UZDoom runs hidden and drives the Halo player through shared memory.");
FARG_CUSTOM(hdbridge_visible, "-hdbridge-visible", "Other", true,
	"Keep the window visible while attached to Halo (debugging).", "", "");
FARG_CUSTOM(hdbridge_flip, "-hdbridge-flip", "Other", true,
	"Flip the captured overlay vertically.", "", "");

EXTERN_CVAR(Bool, vid_activeinbackground)
extern bool GUICapture;   // the platform's input code: a menu or the console has the keyboard

namespace {

#ifdef _WIN32
HANDLE          g_map = nullptr;
#endif
hdb_shared*     g_shm = nullptr;
bool            g_visible = false;
bool            g_flip = false;
int             g_hide_tries = 0;
hdb_halo_state  g_halo{};          // per-tic snapshot
bool            g_halo_valid = false;
uint32_t        g_ray_seq = 1;
std::unordered_set<int> g_keys_down;
uint32_t        g_menu_flag = 0;    // HDB_DS_MENU while UZDoom's menu or console is open

constexpr double kPi = 3.14159265358979323846;

double Scale() { return g_shm && g_shm->doom_units_per_wu > 0 ? g_shm->doom_units_per_wu : 80.0; }

DVector3 ToDoomPos(const hdb_vec3& p) {
	const hdb_vec3& o = g_halo.world_origin;
	const double s = Scale();
	return DVector3((p.x - o.x) * s, (p.y - o.y) * s, (p.z - o.z) * s);
}
hdb_vec3 ToHaloPos(double x, double y, double z) {
	const hdb_vec3& o = g_halo.world_origin;
	const double s = Scale();
	return { float(x / s + o.x), float(y / s + o.y), float(z / s + o.z) };
}
hdb_vec3 ToHaloDelta(double x, double y, double z) {
	const double s = Scale();
	return { float(x / s), float(y / s), float(z / s) };
}

uint32_t OwnPid() {
#ifdef _WIN32
	return (uint32_t)GetCurrentProcessId();
#else
	return (uint32_t)getpid();
#endif
}

// Halo creates the memory; this side only opens it.
bool OpenShared() {
#ifdef _WIN32
	g_map = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, HDB_SHM_NAME_A);
	if (!g_map) return false;
	g_shm = (hdb_shared*)MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(hdb_shared));
	if (!g_shm) { CloseHandle(g_map); g_map = nullptr; return false; }
#else
	int fd = shm_open(HDB_SHM_NAME_POSIX, O_RDWR, 0);
	if (fd < 0) return false;
	void* p = mmap(nullptr, sizeof(hdb_shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (p == MAP_FAILED) return false;
	g_shm = (hdb_shared*)p;
#endif
	return true;
}

void CloseShared() {
#ifdef _WIN32
	if (g_shm) UnmapViewOfFile(g_shm);
	if (g_map) CloseHandle(g_map);
	g_map = nullptr;
#else
	if (g_shm) munmap(g_shm, sizeof(hdb_shared));
#endif
	g_shm = nullptr;
}

// ---- window hiding -------------------------------------------------------
// A hidden window never has focus, so the OS input path stays silent and all
// input arrives through the bridge ring instead.

#ifdef _WIN32
BOOL CALLBACK HideOwnWindow(HWND hwnd, LPARAM) {
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	if (pid == GetCurrentProcessId() && IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_HIDE);
	return TRUE;
}
void HideOwnWindows() { EnumWindows(HideOwnWindow, 0); }
#elif defined(__APPLE__)
void HideOwnWindows() {}   // not a bridge platform (Halo's port runs on Windows and Linux)
#else
void HideOwnWindows() {
	// UZDoom creates one SDL window; window IDs start at 1.
	for (Uint32 id = 1; id < 8; ++id)
		if (SDL_Window* w = SDL_GetWindowFromID(id))
			if (SDL_GetWindowFlags(w) & SDL_WINDOW_SHOWN) SDL_HideWindow(w);
}
#endif

// ---- input ---------------------------------------------------------------

// A key as a menu or the console takes it (as the platform's input code
// posts it while GUICapture is on): the menu key or character, with the
// modifiers held, and the character typed. US layout.
struct GuiKey { int8_t dik; int gk; char plain, shifted; };
const GuiKey kGuiKeys[] = {
	{0x01, GK_ESCAPE, 0, 0}, {0x0E, GK_BACKSPACE, 0, 0}, {0x0F, GK_TAB, 0, 0}, {0x1C, GK_RETURN, 0, 0},
	{0x3B, GK_F1, 0, 0}, {0x3C, GK_F2, 0, 0}, {0x3D, GK_F3, 0, 0}, {0x3E, GK_F4, 0, 0}, {0x3F, GK_F5, 0, 0},
	{0x40, GK_F6, 0, 0}, {0x41, GK_F7, 0, 0}, {0x42, GK_F8, 0, 0}, {0x43, GK_F9, 0, 0}, {0x44, GK_F10, 0, 0},
	{0x57, GK_F11, 0, 0}, {0x58, GK_F12, 0, 0},
	{0x02, 0, '1', '!'}, {0x03, 0, '2', '@'}, {0x04, 0, '3', '#'}, {0x05, 0, '4', '$'}, {0x06, 0, '5', '%'},
	{0x07, 0, '6', '^'}, {0x08, 0, '7', '&'}, {0x09, 0, '8', '*'}, {0x0A, 0, '9', '('}, {0x0B, 0, '0', ')'},
	{0x0C, 0, '-', '_'}, {0x0D, 0, '=', '+'}, {0x1A, 0, '[', '{'}, {0x1B, 0, ']', '}'}, {0x27, 0, ';', ':'},
	{0x28, 0, '\'', '"'}, {0x29, 0, '`', '~'}, {0x2B, 0, '\\', '|'}, {0x33, 0, ',', '<'}, {0x34, 0, '.', '>'},
	{0x35, 0, '/', '?'}, {0x39, 0, ' ', ' '},
	{0x10, 0, 'q', 'Q'}, {0x11, 0, 'w', 'W'}, {0x12, 0, 'e', 'E'}, {0x13, 0, 'r', 'R'}, {0x14, 0, 't', 'T'},
	{0x15, 0, 'y', 'Y'}, {0x16, 0, 'u', 'U'}, {0x17, 0, 'i', 'I'}, {0x18, 0, 'o', 'O'}, {0x19, 0, 'p', 'P'},
	{0x1E, 0, 'a', 'A'}, {0x1F, 0, 's', 'S'}, {0x20, 0, 'd', 'D'}, {0x21, 0, 'f', 'F'}, {0x22, 0, 'g', 'G'},
	{0x23, 0, 'h', 'H'}, {0x24, 0, 'j', 'J'}, {0x25, 0, 'k', 'K'}, {0x26, 0, 'l', 'L'},
	{0x2C, 0, 'z', 'Z'}, {0x2D, 0, 'x', 'X'}, {0x2E, 0, 'c', 'C'}, {0x2F, 0, 'v', 'V'}, {0x30, 0, 'b', 'B'},
	{0x31, 0, 'n', 'N'}, {0x32, 0, 'm', 'M'},
};
const struct { int dik, gk; } kGuiExtended[] = {
	{0x9C, GK_RETURN}, {0xC8, GK_UP}, {0xD0, GK_DOWN}, {0xCB, GK_LEFT}, {0xCD, GK_RIGHT},
	{0xC7, GK_HOME}, {0xCF, GK_END}, {0xC9, GK_PGUP}, {0xD1, GK_PGDN}, {0xD3, GK_DEL},
};

bool Held(int key) { return g_keys_down.count(key) != 0; }

void PostGuiKey(int key, bool down) {
	int gk = 0;
	char ch = 0;
	const bool shift = Held(0x2A) || Held(0x36);
	for (auto& k : kGuiKeys)
		if ((uint8_t)k.dik == key) { gk = k.gk; ch = shift ? k.shifted : k.plain; break; }
	for (auto& k : kGuiExtended)
		if (k.dik == key) gk = k.gk;
	if (!gk && !ch) return;   // modifiers and the rest: only their state matters

	event_t ev = {};
	ev.type = EV_GUI_Event;
	ev.subtype = down ? EV_GUI_KeyDown : EV_GUI_KeyUp;
	ev.data1 = (int16_t)(gk ? gk : toupper((unsigned char)ch));
	ev.data3 = (shift ? GKM_SHIFT : 0) | ((Held(0x1D) || Held(0x9D)) ? GKM_CTRL : 0) |
		((Held(0x38) || Held(0xB8)) ? GKM_ALT : 0);
	D_PostEvent(&ev);
	if (down && ch) {
		event_t typed = {};
		typed.type = EV_GUI_Event;
		typed.subtype = EV_GUI_Char;
		typed.data1 = (int16_t)(unsigned char)ch;
		D_PostEvent(&typed);
	}
}

void PostKey(int key, bool down) {
	if (key < KEY_MOUSE1 && GUICapture) {
		if (down) g_keys_down.insert(key); else g_keys_down.erase(key);
		PostGuiKey(key, down);
		return;
	}
	event_t ev = {};
	ev.type = down ? EV_KeyDown : EV_KeyUp;
	ev.data1 = (int16_t)key;
	D_PostEvent(&ev);
	if (down) g_keys_down.insert(key); else g_keys_down.erase(key);
}

void Shutdown() {
	if (g_shm) g_shm->doom_pid = 0;
	CloseShared();
}

} // namespace

// =========================================================================
// Engine hooks
// =========================================================================

bool HDB_Init() {
	if (!Args->CheckParm(FArg_hdbridge)) return false;
	g_visible = Args->CheckParm(FArg_hdbridge_visible) != 0;
	g_flip = Args->CheckParm(FArg_hdbridge_flip) != 0;

	// Halo creates the memory; give it a few seconds if we were started by hand.
	bool opened = false;
	for (int i = 0; i < 50 && !opened; ++i) {
		opened = OpenShared();
		if (!opened) std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	if (!opened) { Printf("HaloDoomBridge: no Halo bridge memory found, running standalone\n"); return false; }

	if (g_shm->magic != HDB_MAGIC || g_shm->version != HDB_PROTOCOL_VERSION ||
		g_shm->size != sizeof(hdb_shared)) {
		Printf("HaloDoomBridge: protocol mismatch (Halo v%u, UZDoom v%u)\n",
			g_shm->version, HDB_PROTOCOL_VERSION);
		CloseShared();
		return false;
	}
	g_shm->doom_pid = OwnPid();
	g_hide_tries = g_visible ? 0 : 200;
	atexit(Shutdown);

	// D_Display skips drawing when the window isn't active, and a hidden
	// window never is. The overlay is chroma-keyed, so anything that tints or
	// smears the void colour must be off too. Halo launches UZDoom with its own
	// -config file, so none of this touches the player's normal settings.
	vid_activeinbackground = true;
	AddCommandString("freelook 1; i_pauseinbackground 0; i_soundinbackground 1; "
		"gl_bloom 0; gl_tonemap 0; gl_ssao 0; gl_fxaa 0; gl_lens 0; "
		"vid_contrast 1; vid_saturation 1; vid_gamma 1; use_joystick 1; "
		"vid_scalemode 0; vid_scalefactor 1; "
		// each frame is drawn twice (HDB_CaptureFrame): 120 drawings, 60 frames
		"vid_vsync 0; vid_maxfps 120");
	// hdbridge.cfg in HaloDoomBridge.pk3: the controls and settings bridge
	// play needs (Halo Doom sets no default keys; the gametype), every start
	{
		int lump = fileSystem.CheckNumForFullName("hdbridge.cfg");
		if (lump >= 0) {
			auto data = fileSystem.ReadFile(lump);
			FString text(data.string(), data.size());
			int lines = 0;
			for (auto& line : text.Split("\n")) {
				line.StripLeftRight();
				if (line.IsEmpty() || line[0] == '#' || line.IndexOf("//") == 0) continue;
				AddCommandString(line.GetChars());
				lines++;
			}
			Printf("HaloDoomBridge: ran hdbridge.cfg (%d lines)\n", lines);
		}
	}
	Printf("HaloDoomBridge: attached to Halo (pid %u)\n", g_shm->halo_pid);
	return true;
}

// The window stays hidden, so it is never the active one: controllers that
// can (XInput pads, on Windows) are read in the background, where Halo leaves
// them to Halo Doom while Doom drives. Again every two seconds, for pads
// plugged in later.
static void ControllersInBackground() {
	TArray<IJoystickConfig*> sticks;
	I_GetJoysticks(sticks);
	for (auto stick : sticks)
		if (stick->AllowsEnabledInBackground() && !stick->GetEnabledInBackground())
			stick->SetEnabledInBackground(true);
}

// End of I_StartTic (both backends): runs before events become ticcmds.
void HDB_PumpInput() {
	if (!g_shm) return;
	if (g_hide_tries > 0) { --g_hide_tries; HideOwnWindows(); }
	static int pad_check = 0;
	if (pad_check-- <= 0) { ControllersInBackground(); pad_check = 70; }

	// Alive, every tic, even while a menu has the game paused (the game's
	// own tick, which publishes the rest, stops then)
	g_shm->doom_heartbeat = g_shm->doom_heartbeat + 1;

	// UZDoom's menu or console: Halo stops its clock and sends every key here
	const uint32_t menu = (menuactive != MENU_Off || ConsoleState == c_down || ConsoleState == c_falling)
		? (uint32_t)HDB_DS_MENU : 0u;
	if (menu != g_menu_flag) {
		g_menu_flag = menu;
		hdb::seq_write(g_shm->doom, [&](hdb_doom_state& d) {
			d.flags = (d.flags & ~(uint32_t)HDB_DS_MENU) | menu;
		});
	}

	// Halo reinitialises the memory when it restarts while we keep running:
	// re-announce ourselves.
	if (g_shm->magic == HDB_MAGIC && g_shm->doom_pid == 0) g_shm->doom_pid = OwnPid();

	hdb_input in;
	while (hdb::ring_pop(g_shm->input, in)) {
		switch (in.type) {
		case HDB_IN_KEY_DOWN: PostKey(in.code, true); break;
		case HDB_IN_KEY_UP:   PostKey(in.code, false); break;
		case HDB_IN_MOUSE:    PostMouseMove(in.dx, in.dy); break;   // raw counts, +y = down, as the raw-input path
		case HDB_IN_BTN_DOWN: PostKey(KEY_MOUSE1 + in.code, true); break;
		case HDB_IN_BTN_UP:   PostKey(KEY_MOUSE1 + in.code, false); break;
		case HDB_IN_WHEEL: {
			int k = in.dy > 0 ? KEY_MWHEELUP : KEY_MWHEELDOWN;
			for (int n = in.dy > 0 ? in.dy : -in.dy; n > 0; --n) { PostKey(k, true); PostKey(k, false); }
			break;
		}
		case HDB_IN_RELEASE_ALL: {
			std::unordered_set<int> held = g_keys_down;
			for (int k : held) PostKey(k, false);
			break;
		}
		}
	}
}

// Right after D_Display(): the same point UZDoom's own screenshots read the
// finished frame from, on both the OpenGL and Vulkan backends.
//
// The overlay needs to know how see-through every pixel is: Halo Doom's HUD
// has translucent panels, its weapon sprites soft edges, its effects glow.
// A colour key can't tell those apart from the void, and they come out tinted
// with it. So the frame is drawn twice, the void (texture HDBKEY) black for
// the first and white for the second: a pixel's opacity is how little it
// changed, and its own colour what it is over black divided by that. The
// void is empty, so the second drawing is cheap. Time stays the frame's own
// for both (I_SetFrameTime runs once per loop), so they match exactly.
//
// A HaloDoomBridge.pk3 without the black and white textures (HDBKEYB,
// HDBKEYW) gets the old colour key instead.

namespace {

int         g_void_mode = 0;        // 0 not looked yet, 1 black and white, -1 colour key
FTextureID  g_void_tex, g_black_tex, g_white_tex;

struct Frame { TArray<uint8_t> buf; int pitch = 0, bpp = 0; uint32_t w = 0, h = 0; };

bool ReadFrame(Frame& f) {
	ESSType type = SS_RGB;
	float gamma = 1.f;
	f.buf = screen->GetScreenshotBuffer(f.pitch, type, gamma);
	if (f.buf.Size() == 0 || f.pitch <= 0) return false;
	if (type != SS_RGB && type != SS_BGRA) return false;
	f.bpp = type == SS_BGRA ? 4 : 3;
	f.w = uint32_t(f.pitch / f.bpp);
	f.h = uint32_t(f.buf.Size() / f.pitch);
	return f.w > 0 && f.h > 0;
}

inline void Rgb(const Frame& f, uint32_t x, uint32_t y, int& r, int& g, int& b) {
	const uint8_t* s = f.buf.Data() + (g_flip ? (f.h - 1 - y) : y) * f.pitch + x * f.bpp;
	r = f.bpp == 4 ? s[2] : s[0]; g = s[1]; b = f.bpp == 4 ? s[0] : s[2];
}

void LookUpVoid() {
	g_void_tex  = TexMan.CheckForTexture("HDBKEY",  ETextureType::Any);
	g_black_tex = TexMan.CheckForTexture("HDBKEYB", ETextureType::Any);
	g_white_tex = TexMan.CheckForTexture("HDBKEYW", ETextureType::Any);
	g_void_mode = g_void_tex.isValid() && g_black_tex.isValid() && g_white_tex.isValid() ? 1 : -1;
	if (g_void_mode == 1) {
		TexMan.SetTranslation(g_void_tex, g_black_tex);   // from the next frame on
		Printf("HaloDoomBridge: overlay with true transparency\n");
	} else {
		Printf("HaloDoomBridge: HaloDoomBridge.pk3 has no HDBKEYB/HDBKEYW: overlay by colour key\n");
	}
}

} // namespace

void HDB_CaptureFrame() {
	if (!g_shm || !screen) return;
	hdb_overlay& o = g_shm->overlay;

	if (g_void_mode == 0) { LookUpVoid(); if (g_void_mode == 1) return; }

	Frame first, second;
	if (!ReadFrame(first)) return;
	if (g_void_mode == 1) {
		// this frame was drawn over black; again over white
		TexMan.SetTranslation(g_void_tex, g_white_tex);
		D_Display();
		TexMan.SetTranslation(g_void_tex, g_black_tex);
		if (!ReadFrame(second) || second.w != first.w || second.h != first.h) return;
	}

	uint32_t w = first.w, h = first.h;
	if (w > HDB_OVERLAY_MAX_W) w = HDB_OVERLAY_MAX_W;
	if (h > HDB_OVERLAY_MAX_H) h = HDB_OVERLAY_MAX_H;

	// Pick a buffer Halo isn't showing or copying.
	uint32_t dst = 0;
	for (; dst < HDB_OVERLAY_BUFFERS; ++dst)
		if (dst != o.front && dst != o.reading) break;
	if (dst >= HDB_OVERLAY_BUFFERS) return;

	uint8_t* out = o.pixels[dst];
	if (g_void_mode == 1) {
		for (uint32_t y = 0; y < h; ++y) {
			uint8_t* p = out + y * w * 4;
			for (uint32_t x = 0; x < w; ++x, p += 4) {
				int br, bg, bb, wr, wg, wb;
				Rgb(first, x, y, br, bg, bb);
				Rgb(second, x, y, wr, wg, wb);
				// opacity per channel; additive light (which turns white
				// white) shows as its brightest channel
				int a = 255 - (wr - br);
				a = std::max(a, 255 - (wg - bg));
				a = std::max(a, 255 - (wb - bb));
				if (a > 255) a = 255;
				if (a <= 2) { p[0] = p[1] = p[2] = p[3] = 0; continue; }
				p[0] = (uint8_t)std::min(255, (bb * 255 + a / 2) / a);
				p[1] = (uint8_t)std::min(255, (bg * 255 + a / 2) / a);
				p[2] = (uint8_t)std::min(255, (br * 255 + a / 2) / a);
				p[3] = (uint8_t)a;
			}
		}
	} else {
		const uint32_t key = g_shm->overlay_key_rgb;
		const int kr = (key >> 16) & 0xFF, kg = (key >> 8) & 0xFF, kb = key & 0xFF;
		const int tol = (int)g_shm->overlay_key_tolerance;
		for (uint32_t y = 0; y < h; ++y) {
			uint8_t* p = out + y * w * 4;
			for (uint32_t x = 0; x < w; ++x, p += 4) {
				int r, g, b;
				Rgb(first, x, y, r, g, b);
				const int d = abs(r - kr) + abs(g - kg) + abs(b - kb);
				if (d <= tol) { p[0] = p[1] = p[2] = p[3] = 0; }
				else          { p[0] = (uint8_t)b; p[1] = (uint8_t)g; p[2] = (uint8_t)r; p[3] = 255; }
			}
		}
	}
	o.width[dst] = w;
	o.height[dst] = h;
	hdb::fence_rel();
	o.front = dst;
	o.frame_id = o.frame_id + 1;
}

// =========================================================================
// ZScript natives: struct HaloBridge (wadsrc/static/zscript/halobridge.zs)
// =========================================================================

DEFINE_ACTION_FUNCTION(_HaloBridge, IsActive)
{
	PARAM_PROLOGUE;
	ACTION_RETURN_BOOL(g_shm != nullptr);
}

// flags, mapId, haloTick, playerPos, playerVel (doom units per doom tic), groundZ, fovDeg
DEFINE_ACTION_FUNCTION(_HaloBridge, GetState)
{
	PARAM_PROLOGUE;
	g_halo_valid = g_shm && hdb::seq_read(g_shm->halo, g_halo);
	const double s = Scale(), perTic = 30.0 / 35.0;
	const hdb_vec3 v = g_halo.player_vel;
	if (numret > 0) ret[0].SetInt(g_halo_valid ? (int)g_halo.flags : 0);
	if (numret > 1) ret[1].SetInt((int)g_halo.map_id);
	if (numret > 2) ret[2].SetInt((int)g_halo.halo_tick);
	if (numret > 3) ret[3].SetVector(ToDoomPos(g_halo.player_pos));
	if (numret > 4) ret[4].SetVector(DVector3(v.x * s * perTic, v.y * s * perTic, v.z * s * perTic));
	if (numret > 5) ret[5].SetFloat((g_halo.ground_z - g_halo.world_origin.z) * s);
	if (numret > 6) {
		// Halo's FOV is horizontal at the picture's real aspect. UZDoom's is
		// horizontal at 4:3 and widens from there, so go through vertical FOV.
		const double aspect = g_halo.view_h ? double(g_halo.view_w) / g_halo.view_h : 16.0 / 9.0;
		const double vfov = 2.0 * atan(tan(g_halo.fov_h * 0.5) / aspect);
		const double doomFov = 2.0 * atan(tan(vfov * 0.5) * (4.0 / 3.0));
		ret[6].SetFloat(g_halo.fov_h > 0 ? doomFov * 180.0 / kPi : 0.0);
	}
	return numret < 7 ? numret : 7;
}

DEFINE_ACTION_FUNCTION(_HaloBridge, ProxyCount)
{
	PARAM_PROLOGUE;
	ACTION_RETURN_INT(g_halo_valid ? (int)g_halo.proxy_count : 0);
}

// id, flags, pos, radius, height, angleDeg, kindHash, healthFrac
DEFINE_ACTION_FUNCTION(_HaloBridge, GetProxy)
{
	PARAM_PROLOGUE;
	PARAM_INT(i);
	hdb_proxy p{};
	if (g_halo_valid && i >= 0 && (uint32_t)i < g_halo.proxy_count) p = g_halo.proxies[i];
	const double s = Scale();
	if (numret > 0) ret[0].SetInt((int)p.entity_id);
	if (numret > 1) ret[1].SetInt((int)p.flags);
	if (numret > 2) ret[2].SetVector(ToDoomPos(p.pos));
	if (numret > 3) ret[3].SetFloat(p.radius * s);
	if (numret > 4) ret[4].SetFloat(p.height * s);
	if (numret > 5) ret[5].SetFloat(p.yaw * 180.0 / kPi);
	if (numret > 6) ret[6].SetInt((int)p.kind_hash);
	if (numret > 7) ret[7].SetFloat(p.health_frac);
	return numret < 8 ? numret : 8;
}

DEFINE_ACTION_FUNCTION(_HaloBridge, PushMove)
{
	PARAM_PROLOGUE;
	PARAM_INT(tick);
	PARAM_FLOAT(dx); PARAM_FLOAT(dy); PARAM_FLOAT(dz);
	if (g_shm) {
		hdb_move m{ (uint32_t)tick, ToHaloDelta(dx, dy, dz) };
		hdb::ring_push(g_shm->moves, m);
	}
	return 0;
}

// Doom angle/pitch in degrees (Doom convention: pitch + = down).
DEFINE_ACTION_FUNCTION(_HaloBridge, PublishDoom)
{
	PARAM_PROLOGUE;
	PARAM_INT(tick);
	PARAM_INT(flags);
	PARAM_FLOAT(angle);
	PARAM_FLOAT(pitch);
	PARAM_FLOAT(health);
	PARAM_FLOAT(armor);
	if (g_shm) {
		hdb::seq_write(g_shm->doom, [&](hdb_doom_state& d) {
			d.doom_tick = (uint32_t)tick;
			d.flags = (uint32_t)flags | g_menu_flag;
			d.yaw = float(angle * kPi / 180.0);
			d.pitch = float(-pitch * kPi / 180.0);
			d.health = float(health);
			d.armor = float(armor);
		});
	}
	return 0;
}

DEFINE_ACTION_FUNCTION(_HaloBridge, PushDamage)
{
	PARAM_PROLOGUE;
	PARAM_INT(target);
	PARAM_FLOAT(amount);
	PARAM_INT(dtype);
	PARAM_FLOAT(ox); PARAM_FLOAT(oy); PARAM_FLOAT(oz);
	PARAM_FLOAT(dx); PARAM_FLOAT(dy); PARAM_FLOAT(dz);
	PARAM_INT(flags);
	if (g_shm) {
		hdb_damage d{};
		d.target_id = (uint32_t)target;
		d.amount = float(amount);
		d.dtype_hash = (uint32_t)dtype;
		d.flags = (uint32_t)flags;
		d.origin = ToHaloPos(ox, oy, oz);
		d.dir = { float(dx), float(dy), float(dz) };     // unit vector, frame-independent
		hdb::ring_push(g_shm->damage, d);
	}
	return 0;
}

// type (0 = none), amount, dtypeHash, source (doom units)
DEFINE_ACTION_FUNCTION(_HaloBridge, PopHaloEvent)
{
	PARAM_PROLOGUE;
	hdb_event ev{};
	if (!g_shm || !hdb::ring_pop(g_shm->halo_events, ev)) ev.type = 0;
	if (numret > 0) ret[0].SetInt((int)ev.type);
	if (numret > 1) ret[1].SetFloat(ev.amount);
	if (numret > 2) ret[2].SetInt((int)ev.dtype_hash);
	if (numret > 3) ret[3].SetVector(ToDoomPos(ev.source));
	// the same three numbers as sent, for events whose "source" isn't a place
	if (numret > 4) ret[4].SetVector(DVector3(ev.source.x, ev.source.y, ev.source.z));
	return numret < 5 ? numret : 5;
}

DEFINE_ACTION_FUNCTION(_HaloBridge, PushDoomEvent)
{
	PARAM_PROLOGUE;
	PARAM_INT(type);
	PARAM_INT(arg);
	if (g_shm) {
		hdb_event ev{ (uint32_t)type, 0.f, (uint32_t)arg, {} };
		hdb::ring_push(g_shm->doom_events, ev);
	}
	return 0;
}

// Returns a request id, 0 if the ring is full.
DEFINE_ACTION_FUNCTION(_HaloBridge, RequestRay)
{
	PARAM_PROLOGUE;
	PARAM_FLOAT(fx); PARAM_FLOAT(fy); PARAM_FLOAT(fz);
	PARAM_FLOAT(tx); PARAM_FLOAT(ty); PARAM_FLOAT(tz);
	PARAM_BOOL(objects);
	int id = 0;
	if (g_shm) {
		if (++g_ray_seq == 0) g_ray_seq = 1;
		hdb_ray_req r{ g_ray_seq, objects ? 1u : 0u, ToHaloPos(fx, fy, fz), ToHaloPos(tx, ty, tz) };
		if (hdb::ring_push(g_shm->ray_requests, r)) id = (int)g_ray_seq;
	}
	ACTION_RETURN_INT(id);
}

// reqId (0 = none), hit, entity, point, normal
DEFINE_ACTION_FUNCTION(_HaloBridge, PopRayResult)
{
	PARAM_PROLOGUE;
	hdb_ray_result r{};
	if (!g_shm || !hdb::ring_pop(g_shm->ray_results, r)) r.req_id = 0;
	if (numret > 0) ret[0].SetInt((int)r.req_id);
	if (numret > 1) ret[1].SetInt((int)r.hit);
	if (numret > 2) ret[2].SetInt((int)r.hit_entity);
	if (numret > 3) ret[3].SetVector(ToDoomPos(r.point));
	if (numret > 4) ret[4].SetVector(DVector3(r.normal.x, r.normal.y, r.normal.z));
	return numret < 5 ? numret : 5;
}

DEFINE_ACTION_FUNCTION(_HaloBridge, HashName)
{
	PARAM_PROLOGUE;
	PARAM_STRING(s);
	ACTION_RETURN_INT((int)hdb::fnv1a_lower(s.GetChars()));
}

// Checkpoints mirror Halo's: Doom saves when Halo saves, loads when Halo reverts.
// Called straight into the game (not through the console), because save and
// load are unsafe console commands that may be refused inside a script call.
// Halo's checkpoints: a Doom save beside each, loaded when Halo reverts, so
// Halo Doom's weapons, ammo, shields and health go back with the world.
DEFINE_ACTION_FUNCTION(_HaloBridge, SaveCheckpoint)
{
	PARAM_PROLOGUE;
	if (g_shm) {
		FString name = G_BuildSaveName("hdb_checkpoint");
		Printf("HaloDoomBridge: Halo checkpoint: saving %s\n", name.GetChars());
		G_SaveGame(name.GetChars(), "Halo checkpoint");
	}
	return 0;
}

// false if there is no save to load (the caller brings the player back instead)
DEFINE_ACTION_FUNCTION(_HaloBridge, LoadCheckpoint)
{
	PARAM_PROLOGUE;
	if (!g_shm) ACTION_RETURN_BOOL(false);
	FString name = G_BuildSaveName("hdb_checkpoint");
	if (!FileExists(name.GetChars())) {
		Printf("HaloDoomBridge: Halo reverted, but there is no %s\n", name.GetChars());
		ACTION_RETURN_BOOL(false);
	}
	Printf("HaloDoomBridge: Halo reverted: loading %s\n", name.GetChars());
	G_LoadGame(name.GetChars(), true);
	ACTION_RETURN_BOOL(true);
}
