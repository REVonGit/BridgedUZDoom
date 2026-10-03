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
//   win32/i_input.cpp and posix/sdl/i_input.cpp,
//                end of I_StartTic():                          HDB_PumpInput();
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <SDL2/SDL.h>
#endif
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
FString G_BuildSaveName(const char* prefix);   // common/menu/savegamemanager.cpp

FARG_CUSTOM(hdbridge, "-hdbridge", "Other", true,
	"Attach to a running Halo CE (HaloDoom bridge).", "",
	"UZDoom runs hidden and drives the Halo player through shared memory.");
FARG_CUSTOM(hdbridge_visible, "-hdbridge-visible", "Other", true,
	"Keep the window visible while attached to Halo (debugging).", "", "");
FARG_CUSTOM(hdbridge_flip, "-hdbridge-flip", "Other", true,
	"Flip the captured overlay vertically.", "", "");

EXTERN_CVAR(Bool, vid_activeinbackground)

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
#else
void HideOwnWindows() {
	// UZDoom creates one SDL window; window IDs start at 1.
	for (Uint32 id = 1; id < 8; ++id)
		if (SDL_Window* w = SDL_GetWindowFromID(id))
			if (SDL_GetWindowFlags(w) & SDL_WINDOW_SHOWN) SDL_HideWindow(w);
}
#endif

// ---- input ---------------------------------------------------------------

void PostKey(int key, bool down) {
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
		"vid_contrast 1; vid_saturation 1; vid_scalemode 0; vid_scalefactor 1; "
		"vid_vsync 0; vid_maxfps 60");
	Printf("HaloDoomBridge: attached to Halo (pid %u)\n", g_shm->halo_pid);
	return true;
}

// End of I_StartTic (both backends): runs before events become ticcmds.
void HDB_PumpInput() {
	if (!g_shm) return;
	if (g_hide_tries > 0) { --g_hide_tries; HideOwnWindows(); }

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
void HDB_CaptureFrame() {
	if (!g_shm || !screen) return;
	hdb_overlay& o = g_shm->overlay;

	int pitch = 0;
	ESSType type = SS_RGB;
	float gamma = 1.f;
	TArray<uint8_t> buf = screen->GetScreenshotBuffer(pitch, type, gamma);
	if (buf.Size() == 0 || pitch <= 0) return;
	const int bpp = type == SS_BGRA ? 4 : 3;
	if (type != SS_RGB && type != SS_BGRA) return;

	uint32_t w = uint32_t(pitch / bpp);
	uint32_t h = uint32_t(buf.Size() / pitch);
	if (w > HDB_OVERLAY_MAX_W) w = HDB_OVERLAY_MAX_W;
	if (h > HDB_OVERLAY_MAX_H) h = HDB_OVERLAY_MAX_H;

	// Pick a buffer Halo isn't showing or copying.
	uint32_t dst = 0;
	for (; dst < HDB_OVERLAY_BUFFERS; ++dst)
		if (dst != o.front && dst != o.reading) break;
	if (dst >= HDB_OVERLAY_BUFFERS) return;

	const uint32_t key = g_shm->overlay_key_rgb;
	const int kr = (key >> 16) & 0xFF, kg = (key >> 8) & 0xFF, kb = key & 0xFF;
	const int tol = (int)g_shm->overlay_key_tolerance;

	uint8_t* out = o.pixels[dst];
	for (uint32_t y = 0; y < h; ++y) {
		const uint8_t* row = buf.Data() + (g_flip ? (h - 1 - y) : y) * pitch;
		uint8_t* drow = out + y * w * 4;
		for (uint32_t x = 0; x < w; ++x) {
			const uint8_t* s = row + x * bpp;
			const int r = bpp == 4 ? s[2] : s[0], g = s[1], b = bpp == 4 ? s[0] : s[2];
			const int d = abs(r - kr) + abs(g - kg) + abs(b - kb);
			uint8_t* p = drow + x * 4;
			if (d <= tol) { p[0] = p[1] = p[2] = p[3] = 0; }
			else          { p[0] = (uint8_t)b; p[1] = (uint8_t)g; p[2] = (uint8_t)r; p[3] = 255; }
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
			d.flags = (uint32_t)flags;
			d.yaw = float(angle * kPi / 180.0);
			d.pitch = float(-pitch * kPi / 180.0);
			d.health = float(health);
			d.armor = float(armor);
		});
		g_shm->doom_heartbeat = g_shm->doom_heartbeat + 1;
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
	return numret < 4 ? numret : 4;
}

DEFINE_ACTION_FUNCTION(_HaloBridge, PushDoomEvent)
{
	PARAM_PROLOGUE;
	PARAM_INT(type);
	if (g_shm) {
		hdb_event ev{ (uint32_t)type, 0.f, 0, {} };
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
DEFINE_ACTION_FUNCTION(_HaloBridge, SaveCheckpoint)
{
	PARAM_PROLOGUE;
	if (g_shm) G_SaveGame(G_BuildSaveName("hdb_checkpoint").GetChars(), "Halo checkpoint");
	return 0;
}

DEFINE_ACTION_FUNCTION(_HaloBridge, LoadCheckpoint)
{
	PARAM_PROLOGUE;
	if (g_shm) G_LoadGame(G_BuildSaveName("hdb_checkpoint").GetChars(), true);
	return 0;
}
