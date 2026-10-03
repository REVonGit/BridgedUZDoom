/*
 * hdb_protocol.h - HaloDoom Bridge shared-memory protocol.
 *
 * The one contract between the Halo CE side (host: world, AI, rendering)
 * and the UZDoom side (guest: Halo Doom Evolved player logic, weapons, HUD).
 * Modeled on SkyCraft's protocol/skycraft_protocol.h approach.
 *
 * Conventions
 *   - Every position/velocity in this file is in HALO WORLD UNITS (WU), z-up.
 *     Only the UZDoom native module converts to/from Doom map units.
 *   - Angles are radians, Halo convention: yaw 0 = +x, counter-clockwise;
 *     pitch positive = looking UP. (Doom's pitch is positive = down; the
 *     UZDoom native flips it.)
 *   - Velocities are WU per Halo tick (30 Hz). Move samples are WU per
 *     Doom tic (35 Hz); Halo sums whatever arrived during its tick.
 *   - Single-producer/single-consumer everywhere. x86 TSO + compiler fences.
 *   - Fixed-width fields and no pointers: the 32-bit Halo port and 64-bit
 *     UZDoom see the same layout.
 *
 * Bump HDB_PROTOCOL_VERSION on ANY layout change; tools/fake_halo.py mirrors
 * this layout with ctypes and tools/check_layout.c verifies the two agree.
 */
#ifndef HDB_PROTOCOL_H
#define HDB_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define HDB_PROTOCOL_VERSION 1u
#define HDB_MAGIC            0x31424448u /* "HDB1" */
#define HDB_SHM_NAME_A       "Local\\HaloDoomBridge.v1"
#define HDB_SHM_NAME_W       L"Local\\HaloDoomBridge.v1"
#define HDB_SHM_NAME_POSIX   "/HaloDoomBridge.v1"   /* shm_open name; /dev/shm/HaloDoomBridge.v1 on Linux */

#define HDB_MAX_PROXIES      256
#define HDB_INPUT_RING       1024
#define HDB_MOVE_RING        64
#define HDB_DAMAGE_RING      256
#define HDB_RAY_RING         256
#define HDB_EVENT_RING       64
#define HDB_OVERLAY_BUFFERS  3
#define HDB_OVERLAY_MAX_W    2560
#define HDB_OVERLAY_MAX_H    1440
#define HDB_NONE             0xFFFFFFFFu

#pragma pack(push, 4)

typedef struct { float x, y, z; } hdb_vec3;

/* ------------------------------------------------------------------ */
/* Halo -> Doom state (seqlocked, written once per Halo tick)          */
/* ------------------------------------------------------------------ */

enum hdb_halo_flags {
    HDB_HS_IN_GAME      = 1u << 0,  /* a map is loaded and the player exists        */
    HDB_HS_PAUSED       = 1u << 1,  /* Halo menu open: Doom should not tick input   */
    HDB_HS_CINEMATIC    = 1u << 2,  /* cutscene: Doom freezes, overlay hidden       */
    HDB_HS_LOADING      = 1u << 3,
    HDB_HS_PLAYER_DEAD  = 1u << 4,
    HDB_HS_HALO_CONTROL = 1u << 5,  /* vehicle/turret: Halo drives, Doom idles      */
    HDB_HS_GROUNDED     = 1u << 6,
    HDB_HS_IN_WATER     = 1u << 7,
    HDB_HS_DRIVING      = 1u << 8   /* Doom currently drives the player biped        */
};

enum hdb_proxy_flags {
    HDB_PF_ALIVE   = 1u << 0,
    HDB_PF_ENEMY   = 1u << 1,       /* hostile to the player's team */
    HDB_PF_VEHICLE = 1u << 2,
    HDB_PF_ITEM    = 1u << 3        /* a weapon or equipment lying in reach, not a unit:
                                       kind_hash its tag, health_frac its rounds (or 1) */
};

typedef struct {
    uint32_t entity_id;   /* Halo object handle; stable while the object lives */
    uint32_t flags;       /* hdb_proxy_flags */
    hdb_vec3 pos;         /* feet */
    hdb_vec3 vel;
    float    radius;
    float    height;
    float    yaw;
    uint32_t team;
    uint32_t kind_hash;   /* FNV-1a of the unit tag path; Doom maps it to blood/sounds */
    float    health_frac; /* 0..1, body+shield combined, for HaloDoom HUD target info */
} hdb_proxy;

typedef struct {
    volatile uint32_t seq;      /* seqlock: odd while writing */
    uint32_t halo_tick;
    uint32_t flags;             /* hdb_halo_flags */
    uint32_t map_id;            /* increments on every map load / BSP switch */
    char     map_name[64];
    hdb_vec3 world_origin;      /* per-map recentering point (WU)            */
    hdb_vec3 player_pos;        /* feet */
    hdb_vec3 player_vel;
    float    ground_z;          /* ground directly under the player (WU)     */
    float    fov_h;             /* horizontal FOV of the picture actually drawn
                                   (view_w x view_h), radians; UZDoom's native
                                   converts to its 4:3-based FOV              */
    uint32_t view_w, view_h;    /* Halo window drawable size; Doom renders to match */
    uint32_t proxy_count;
    hdb_proxy proxies[HDB_MAX_PROXIES];
} hdb_halo_state;

/* ------------------------------------------------------------------ */
/* Doom -> Halo state (seqlocked, written once per Doom tic)           */
/* ------------------------------------------------------------------ */

enum hdb_doom_flags {
    HDB_DS_READY       = 1u << 0,   /* bridge pk3 is running on HDBVOID */
    HDB_DS_PLAYER_DEAD = 1u << 1,
    HDB_DS_CROUCHING   = 1u << 2,
    HDB_DS_MENU        = 1u << 3    /* UZDoom's menu or console is open: Halo's clock
                                       stops, and every key but F11/F12 goes to Doom */
};

typedef struct {
    volatile uint32_t seq;
    uint32_t doom_tick;
    uint32_t flags;             /* hdb_doom_flags */
    float    yaw, pitch;        /* Halo convention (see header comment) */
    float    health, armor;     /* informational only */
} hdb_doom_state;

/* ------------------------------------------------------------------ */
/* Ring payloads                                                       */
/* ------------------------------------------------------------------ */

enum hdb_input_type {
    HDB_IN_KEY_DOWN  = 1,       /* code = scancode, | 0x80 for E0-extended keys (DIK style) */
    HDB_IN_KEY_UP    = 2,
    HDB_IN_MOUSE     = 3,       /* dx, dy raw counts, +y = moved down (raw-input convention) */
    HDB_IN_BTN_DOWN  = 4,       /* code = button index 0..7 */
    HDB_IN_BTN_UP    = 5,
    HDB_IN_WHEEL     = 6,       /* dy = notches, + = up */
    HDB_IN_RELEASE_ALL = 7      /* sent when control moves back to Halo */
};

typedef struct { uint16_t type; uint16_t code; int32_t dx; int32_t dy; } hdb_input;

typedef struct { uint32_t doom_tick; hdb_vec3 delta; } hdb_move;   /* WU moved in one Doom tic */

enum hdb_damage_flags {
    HDB_DF_NEEDS_LOS  = 1u << 0,  /* Halo must confirm origin->target isn't blocked by BSP */
    HDB_DF_EXPLOSION  = 1u << 1,
    HDB_DF_MELEE      = 1u << 2
};

typedef struct {
    uint32_t target_id;
    float    amount;              /* Doom damage points, Halo side scales */
    uint32_t dtype_hash;          /* FNV-1a of the Doom damage type name, lowercased */
    uint32_t flags;               /* hdb_damage_flags */
    hdb_vec3 origin;
    hdb_vec3 dir;
} hdb_damage;

typedef struct { uint32_t req_id; uint32_t flags; hdb_vec3 from; hdb_vec3 to; } hdb_ray_req;

typedef struct {
    uint32_t req_id;
    uint32_t hit;                 /* 0 = clear */
    uint32_t hit_entity;          /* HDB_NONE for BSP hits */
    hdb_vec3 point;
    hdb_vec3 normal;
} hdb_ray_result;

enum hdb_event_type {
    /* Halo -> Doom */
    HDB_EV_PLAYER_DAMAGED   = 1,  /* amount, source, dtype_hash */
    HDB_EV_CHECKPOINT_SAVED = 2,
    HDB_EV_REVERTED         = 3,
    HDB_EV_MAP_LOADED       = 4,
    HDB_EV_PLAYER_KILLED    = 5,  /* the player's unit died in Halo: Doom's dies too */
    HDB_EV_PLAYER_VITALITY  = 6,  /* source.x shields, source.y body, as fractions of the
                                     unit's maxima (shields above 1 overshielded): Halo
                                     keeps them; Halo Doom shows them */
    HDB_EV_LOADOUT_BEGIN    = 7,  /* a level starts: Doom's weapons and grenades go... */
    HDB_EV_LOADOUT_ITEM     = 8,  /* ...for these: dtype_hash the tag, amount the reserve
                                     rounds (or grenade count), source = (rounds loaded,
                                     charge 0..1, 1 if in hand) */
    HDB_EV_LOADOUT_END      = 9,
    /* Doom -> Halo */
    HDB_EV_DOOM_PLAYER_DIED = 100,
    HDB_EV_DOOM_TOOK_ITEM   = 101 /* dtype_hash = the item's entity_id: Halo removes it */
};

typedef struct { uint32_t type; float amount; uint32_t dtype_hash; hdb_vec3 source; } hdb_event;

#define HDB_DEFINE_RING(name, T, N) \
    typedef struct { volatile uint32_t head; volatile uint32_t tail; T items[N]; } name

HDB_DEFINE_RING(hdb_input_ring,  hdb_input,      HDB_INPUT_RING);
HDB_DEFINE_RING(hdb_move_ring,   hdb_move,       HDB_MOVE_RING);
HDB_DEFINE_RING(hdb_damage_ring, hdb_damage,     HDB_DAMAGE_RING);
HDB_DEFINE_RING(hdb_rayreq_ring, hdb_ray_req,    HDB_RAY_RING);
HDB_DEFINE_RING(hdb_rayres_ring, hdb_ray_result, HDB_RAY_RING);
HDB_DEFINE_RING(hdb_event_ring,  hdb_event,      HDB_EVENT_RING);

/* ------------------------------------------------------------------ */
/* Overlay: Doom's first-person weapon + HUD, BGRA straight alpha.     */
/* Triple buffered: writer never touches `front` or `reading`.         */
/* ------------------------------------------------------------------ */

typedef struct {
    volatile uint32_t front;      /* last completed buffer, HDB_NONE before first frame */
    volatile uint32_t reading;    /* buffer Halo is copying, HDB_NONE when idle */
    volatile uint32_t frame_id;
    uint32_t width[HDB_OVERLAY_BUFFERS];
    uint32_t height[HDB_OVERLAY_BUFFERS];
    uint8_t  pixels[HDB_OVERLAY_BUFFERS][HDB_OVERLAY_MAX_W * HDB_OVERLAY_MAX_H * 4];
} hdb_overlay;

/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t size;                 /* sizeof(hdb_shared) as seen by the creator */
    volatile uint32_t halo_pid;
    volatile uint32_t doom_pid;
    volatile uint32_t halo_heartbeat;
    volatile uint32_t doom_heartbeat;
    float    doom_units_per_wu;    /* scale, chosen by Halo side from hdbridge.ini */
    uint32_t overlay_key_rgb;      /* 0x00RRGGBB chroma key Doom renders the void with */
    uint32_t overlay_key_tolerance;

    hdb_halo_state  halo;
    hdb_doom_state  doom;

    /* Halo -> Doom */
    hdb_input_ring  input;
    hdb_event_ring  halo_events;
    hdb_rayres_ring ray_results;

    /* Doom -> Halo */
    hdb_move_ring   moves;
    hdb_damage_ring damage;
    hdb_rayreq_ring ray_requests;
    hdb_event_ring  doom_events;

    hdb_overlay     overlay;
} hdb_shared;

#pragma pack(pop)

/* ------------------------------------------------------------------ */
/* Helpers shared by both sides. The layout above uses only 4-byte    */
/* fields with pack(4), so it is identical under the game's MSVC-like */
/* ABI (-malign-double) and the host ABI of posix_ and win32_ units.   */
/* ------------------------------------------------------------------ */

static inline uint32_t hdb_fnv1a_lower(const char* s) {
    uint32_t h = 2166136261u;
    for (; s && *s; ++s) {
        unsigned char c = (unsigned char)*s;
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
        h = (h ^ c) * 16777619u;
    }
    return h;
}

#ifdef __cplusplus
#include <atomic>
#include <cstring>

namespace hdb {

inline void fence_rel() { std::atomic_thread_fence(std::memory_order_release); }
inline void fence_acq() { std::atomic_thread_fence(std::memory_order_acquire); }

template <class Block, class Fn>
inline void seq_write(Block& b, Fn&& fill) {
    b.seq = b.seq + 1;          /* odd: write in progress */
    fence_rel();
    fill(b);
    fence_rel();
    b.seq = b.seq + 1;          /* even: stable */
}

/* Copies a consistent snapshot of `src` into `out`. False if the writer kept us out. */
template <class Block>
inline bool seq_read(const Block& src, Block& out, int tries = 64) {
    for (int i = 0; i < tries; ++i) {
        uint32_t s1 = src.seq;
        if (s1 & 1u) continue;
        fence_acq();
        std::memcpy(&out, (const void*)&src, sizeof(Block));
        fence_acq();
        if (src.seq == s1) return true;
    }
    return false;
}

template <class Ring, class T>
inline bool ring_push(Ring& r, const T& v) {
    constexpr uint32_t N = sizeof(r.items) / sizeof(r.items[0]);
    uint32_t h = r.head;
    if (h - r.tail >= N) return false;          /* full: drop newest */
    r.items[h % N] = v;
    fence_rel();
    r.head = h + 1;
    return true;
}

template <class Ring, class T>
inline bool ring_pop(Ring& r, T& out) {
    constexpr uint32_t N = sizeof(r.items) / sizeof(r.items[0]);
    uint32_t t = r.tail;
    if (t == r.head) return false;
    fence_acq();
    out = r.items[t % N];
    fence_rel();
    r.tail = t + 1;
    return true;
}

inline uint32_t fnv1a_lower(const char* s) { return hdb_fnv1a_lower(s); }

} // namespace hdb

#else /* C: the halo-ce-universal side */

#if defined(__clang__) || defined(__GNUC__)
#define HDB_FENCE_REL() __atomic_thread_fence(__ATOMIC_RELEASE)
#define HDB_FENCE_ACQ() __atomic_thread_fence(__ATOMIC_ACQUIRE)
#else
#include <intrin.h>
#define HDB_FENCE_REL() _ReadWriteBarrier()
#define HDB_FENCE_ACQ() _ReadWriteBarrier()
#endif

#define HDB_RING_CAP(r) ((uint32_t)(sizeof((r).items) / sizeof((r).items[0])))

/* ok = 1 if pushed, 0 if the ring was full (newest dropped) */
#define HDB_RING_PUSH(r, v, ok) do {                                   \
        uint32_t h_ = (r).head;                                        \
        if (h_ - (r).tail >= HDB_RING_CAP(r)) { (ok) = 0; break; }     \
        (r).items[h_ % HDB_RING_CAP(r)] = (v);                         \
        HDB_FENCE_REL();                                               \
        (r).head = h_ + 1;                                             \
        (ok) = 1;                                                      \
    } while (0)

/* ok = 1 if an item was popped into out */
#define HDB_RING_POP(r, out, ok) do {                                  \
        uint32_t t_ = (r).tail;                                        \
        if (t_ == (r).head) { (ok) = 0; break; }                       \
        HDB_FENCE_ACQ();                                               \
        (out) = (r).items[t_ % HDB_RING_CAP(r)];                       \
        HDB_FENCE_REL();                                               \
        (r).tail = t_ + 1;                                             \
        (ok) = 1;                                                      \
    } while (0)

#define HDB_SEQ_BEGIN(b) do { (b).seq = (b).seq + 1; HDB_FENCE_REL(); } while (0)
#define HDB_SEQ_END(b)   do { HDB_FENCE_REL(); (b).seq = (b).seq + 1; } while (0)

static inline int hdb_seq_read(const volatile void* src, void* out, size_t size, const volatile uint32_t* seq) {
    int i;
    for (i = 0; i < 64; ++i) {
        uint32_t s1 = *seq;
        if (s1 & 1u) continue;
        HDB_FENCE_ACQ();
        memcpy(out, (const void*)src, size);
        HDB_FENCE_ACQ();
        if (*seq == s1) return 1;
    }
    return 0;
}

#endif /* __cplusplus */

#endif /* HDB_PROTOCOL_H */
