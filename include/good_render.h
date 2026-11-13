#pragma once

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cwchar>

// GW2 Good Render: one addon follows the game's frame and hands its moments to every addon subscribed, so that each
// draws into the game's frame rather than over all of it -- under the game's windows, or into the world. **Take this
// header as it is**, from https://github.com/HasKha/gw2-good-render: the service and every subscriber must hold the
// same one. How to subscribe, and how it works: that repository's docs/.
//
// The subscribers are a table in a block of shared memory, a named file mapping of the process's, which every side
// opens for itself. A moment calls each subscriber straight from the service, on the game's render thread, with no
// lock between them: a subscriber claims a slot with an atomic swap, and the service counts each call into a slot, so
// unsubscribing waits out a call in progress.
//
// Not Nexus's DataLink, though it is also a named file mapping: `DataLink.Share` zeroes the memory even when it opens
// a mapping that already exists, which would wipe the subscribers of a block another side made first.

struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;
struct ImDrawList;

namespace good_render {

// In frame order.
enum MomentId : uint32_t {
    // Opaque geometry is done, the game's ground telegraphs are next.
    MOMENT_AFTER_OPAQUE = 0,
    // The game's ground telegraphs are done; water, particles and effects are next, and blend over what is drawn here.
    MOMENT_AFTER_DECALS = 1,
    // The surface pass is being left; what is drawn here still goes through post-processing.
    MOMENT_SURFACE_END = 2,
    // The world is finished, post-processing included, and the game's UI is next.
    MOMENT_WORLD_FINISHED = 3,
    // Inside the game's UI: over its names, nameplates, damage numbers and HUD, under the NPC dialog, every window and
    // the tooltip. Where an overlay on the world belongs. While the player hides the UI, at the UI's end instead.
    MOMENT_BEFORE_WINDOWS = 4,
    // The game's UI is done; Nexus's and every addon's windows are still to come.
    MOMENT_UI_FINISHED = 5,
};

// The camera the frame is drawn with, in MumbleLink's space: metres, x right, y up, z forward, left-handed.
//
// Not MumbleLink read during the moment: the game draws a frame behind its game thread, which writes MumbleLink, so a
// camera read then is a frame ahead of the world it would draw over, and shapes on the ground slip while zooming. This is
// MumbleLink as it was at the previous Present, which is what the frame was drawn with.
//
// The matrices are column-major, element [column * 4 + row], so clip = matrix * (x, y, z, 1). Both glm::make_mat4() and
// DirectXMath's XMLoadFloat4x4() take them as they are: DirectXMath's row vectors, v * M, read the same memory the same way.
struct Camera {
    float position[3];
    float front[3];  // unit length
    float up[3];     // unit length
    float fov;       // vertical, radians
    // The scene's width over its height, which the projection takes. Not the window's when the scene renders at another
    // shape: below the monitor's resolution, the scene is that resolution and the window the monitor's.
    float aspect;
    // World to view: lookAtLH, so view depth is view z, in metres.
    float view[16];
    // View to clip: perspectiveLH with D3D's 0..1 depth, between 0.1 m and 10 km; so clip w is view depth. Its planes
    // are not the game's: compare depths by view depth, as linear_depth holds it.
    float projection[16];
    float view_projection[16];
};

// What a moment hands each subscriber. Valid only for the call.
struct Moment {
    uint32_t moment;  // MomentId
    // What is bound, in its own pixels: the scene's size up to WORLD_FINISHED, which resolution scaling can make
    // smaller than the window, and the window's after.
    uint32_t width;
    uint32_t height;
    // The far plane linear_depth is divided by, in metres: view depth is linear_depth * z_far.
    float z_far;
    // Null with no camera: before MumbleLink is written, and while the full map is open, behind which the camera keeps
    // moving.
    const Camera* camera;
    // How many of the bound target's pixels one of the window's is: 1 in the game's UI.
    float pixels_per_window_pixel;
    // The game's immediate context, with the game's targets bound. **Whatever a subscriber changes on it, it puts
    // back**, as the game's next draw expects its own state. Never a target's alpha: in the world it is the engine's
    // per-pixel tag.
    ID3D11DeviceContext* context;
    // The scene's, at every moment, so in the game's UI another size than what is bound: sampled by the screen's
    // fraction there, not by pixel. Never bound as a target.
    //
    // The prepass colour, whose alpha * 255 is 0 on most of the world, 1 where the game puts no decals, 3 on foliage
    // and 5 on characters.
    ID3D11ShaderResourceView* mask;
    // Opaque geometry's view depth over z_far, an R32_FLOAT; sky reads 1.
    ID3D11ShaderResourceView* linear_depth;
    // An empty list for the subscriber to draw into, as into Nexus's background list: started as ImGui starts its own,
    // with the font atlas's texture and a full-screen clip rectangle, laid out in the window's pixels. The service draws
    // it into what is bound once the callback returns, scaled to the moment: each command textured by its TextureId, an
    // ID3D11ShaderResourceView*, colour only, with clip rectangles and user callbacks not honoured. The list is ImGui
    // 1.80's, Nexus's, which every Nexus addon shares, and grows through Nexus's allocator, as every Nexus addon's ImGui
    // does. Only during the call.
    ImDrawList* draw_list;

    // ImGui's textured quad, at points in the world rather than on the screen: MumbleLink's metres, as the camera's.
    // Hidden pixel by pixel behind whatever the game drew opaque in front of it -- a wall, a hill, a character --
    // which draw_list never is. Drawn after the callback returns, before draw_list, in the order called.
    //
    // `texture` is what ImGui's AddImageQuad() takes, an ID3D11ShaderResourceView*, or null for a plain colour;
    // `colour` is IM_COL32's. The corners go round the quad, either way, uvs[i] at corners[i]; null uvs are the whole
    // texture, (0,0) (1,0) (1,1) (0,1). Points that are not finite drop the quad. As draw_list's, the
    // texture must stay alive until the call has been drawn: until unsubscribe() returns.
    //
    // Null outside the world -- from BEFORE_WINDOWS on -- and with no camera: draw on draw_list then.
    void (*world_quad)(const float corners[4][3], const float uvs[4][2], void* texture, uint32_t colour);
    // world_quad's quad standing upright to the camera, as an icon in the world: the whole texture, `half_width` and
    // `half_height` metres from `centre` each way. Null when world_quad is.
    void (*world_billboard)(const float centre[3], float half_width, float half_height, void* texture, uint32_t colour);
};

using Callback = void (*)(const Moment* moment);

// Who follows the frame now. One follower at a time: two would each read the other's drawing as the game's frame.
enum Owner : uint32_t {
    OWNER_NONE = 0,
    OWNER_SERVICE = 1,
    // An addon that follows the frame itself while the service is not there, as Community Challenge Mode does. It
    // lets go when the service comes, and the service takes the frame from nobody.
    OWNER_ADDON = 2,
};

// More subscribers than there will be.
constexpr int MOST_SUBSCRIBERS = 32;

struct Slot {
    // Set by the subscriber that took the slot, before anything else of it; cleared after it has left.
    std::atomic<uint32_t> claimed;
    // Subscribers are called in ascending order, ties in slot order: what draws over the others draws last.
    int32_t order;
    std::atomic<Callback> callback;
    // Calls into the slot in progress, counted by the service before it reads the callback.
    std::atomic<uint32_t> calls;
};

struct Block {
    // HEADER once the block is made. Another value is another version's block, which nothing here reads.
    std::atomic<uint64_t> header;
    // 1 while the service is loaded and able to follow the frame.
    std::atomic<uint32_t> service;
    std::atomic<uint32_t> owner;  // Owner
    Slot slots[MOST_SUBSCRIBERS];
};

// "GDRENDER" with the version in the low byte. Raised whenever Block or Moment changes.
constexpr uint64_t HEADER = 0x4744'5245'4E44'4500ull | 2;

static_assert(std::atomic<uint64_t>::is_always_lock_free && std::atomic<uint32_t>::is_always_lock_free &&
                  std::atomic<Callback>::is_always_lock_free,
              "the block is shared between modules, so every atomic in it must be a plain word");

// ---- The block

struct Link {
    HANDLE mapping = nullptr;
    Block* block = nullptr;
};

// This process's block, made zeroed by whoever opens it first. Empty when the block is another version's, or the
// mapping could not be made.
inline Link open() {
    wchar_t name[64]{};
    swprintf_s(name, L"GW2GoodRender.%lu", GetCurrentProcessId());
    Link link;
    link.mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Block), name);
    if (link.mapping != nullptr) {
        link.block = (Block*)MapViewOfFile(link.mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Block));
    }
    uint64_t header = 0;
    if (link.block != nullptr && !link.block->header.compare_exchange_strong(header, HEADER) && header != HEADER) {
        UnmapViewOfFile(link.block);
        link.block = nullptr;
    }
    if (link.block == nullptr && link.mapping != nullptr) {
        CloseHandle(link.mapping);
        link.mapping = nullptr;
    }
    return link;
}

// The block is kept for as long as any side has it open.
inline void close(Link& link) {
    if (link.block != nullptr) {
        UnmapViewOfFile(link.block);
    }
    if (link.mapping != nullptr) {
        CloseHandle(link.mapping);
    }
    link = Link{};
}

// ---- Subscribing

// Takes a free slot for `callback`, called at every moment from then on. Null when every slot is taken.
inline Slot* subscribe(Block& block, Callback callback, int32_t order) {
    for (Slot& slot : block.slots) {
        uint32_t free = 0;
        if (slot.claimed.compare_exchange_strong(free, 1)) {
            slot.order = order;
            slot.callback.store(callback);
            return &slot;
        }
    }
    return nullptr;
}

// Takes `slot`'s callback out, then waits for a call into it in progress to return, up to a second: once this returns
// the callback is not called again, and may be unloaded. Never from inside the callback, whose own call it would wait
// for.
inline void unsubscribe(Slot& slot) {
    slot.callback.store(nullptr);
    for (int waited_ms = 0; waited_ms < 1000 && slot.calls.load() != 0; waited_ms += 5) {
        Sleep(5);
    }
    slot.claimed.store(0);
}

// ---- The service

// Calls every subscriber with `moment`, in order, and `after_each` after each call. A slot's call is counted before its
// callback is read, so unsubscribe() can wait it out. `after_each` runs inside the count, so whatever the call left for
// the service to draw, textures of the subscriber's included, is drawn before the subscriber can unload.
inline void callSubscribers(Block& block, const Moment& moment, void (*after_each)(const Moment&) = nullptr) {
    Slot* ordered[MOST_SUBSCRIBERS];
    int count = 0;
    for (Slot& slot : block.slots) {
        if (slot.claimed.load() != 0) {
            ordered[count++] = &slot;
        }
    }
    std::stable_sort(ordered, ordered + count, [](const Slot* a, const Slot* b) { return a->order < b->order; });
    for (int i = 0; i < count; ++i) {
        Slot& slot = *ordered[i];
        slot.calls.fetch_add(1);
        if (const Callback callback = slot.callback.load()) {
            callback(&moment);
            if (after_each != nullptr) {
                after_each(moment);
            }
        }
        slot.calls.fetch_sub(1);
    }
}

// Frees the slot of any subscriber whose module is gone without unsubscribing -- one that crashed in its unload, or
// has none -- so it is never called into freed memory. The service's, once a frame, outside a moment.
inline void dropUnloadedSubscribers(Block& block) {
    for (Slot& slot : block.slots) {
        HMODULE module = nullptr;
        const Callback callback = slot.callback.load();
        if (callback != nullptr &&
            !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)callback, &module)) {
            Callback expected = callback;
            if (slot.callback.compare_exchange_strong(expected, nullptr)) {
                slot.claimed.store(0);
            }
        }
    }
}

} // namespace good_render
