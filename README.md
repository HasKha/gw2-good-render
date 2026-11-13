# GW2 Good Render

A [Nexus](https://raidcore.gg/Nexus) addon for Guild Wars 2 that lets other addons draw *inside* the game's frame.

Nexus's render callback runs after the game has drawn everything. What an addon draws there covers the game's windows. 
GW2 Good Render follows the game's frame instead, and calls the addons subscribed to it at six moments along the way. 
An addon that draws at `MOMENT_BEFORE_WINDOWS` shows over the HUD and under every window. 
An addon can also draw into the world before post-processing, with the scene's depth available.

On its own it draws nothing. An addon that supports it draws at its moments while it is loaded, and draws as before
when it is not. It can be loaded or unloaded at any time. Addons that use it switch at the next frame.

## Installing

Download `GoodRender.dll` from [Releases](https://github.com/HasKha/gw2-good-render/releases) into your Nexus
`addons` folder, then load it from Nexus's addon list.

## For addon developers

The whole contract is one header, [`include/good_render.h`](include/good_render.h). Add it as a dependency, then add `gw2-good-render/include` to your include path.
What each field and call does is written beside it in the header.

### Subscribing

You must subscribe at load and unsubscribe at unload. If you don't unsubscribe, the service may call bad memory and cause a crash.

The service (goodrender.dll) does not need to be loaded first. Addons can be loaded in any order.
Once the service is loaded, it will start firing callbacks at existing and new subscribers.

```cpp
#include "good_render.h"

good_render::Link GoodRender;
good_render::Slot* GoodRenderSlot = nullptr;

void AddonLoad(AddonAPI* api) {
    GoodRender = good_render::open();
    if (GoodRender.block != nullptr) {
        GoodRenderSlot = good_render::subscribe(*GoodRender.block, OnMoment, 0);
    }
}

void AddonUnload() {
    if (GoodRenderSlot != nullptr) {
        good_render::unsubscribe(*GoodRenderSlot);
        GoodRenderSlot = nullptr;
    }
    good_render::close(GoodRender);
}
```

- **`open()`** returns no block only when Windows could not make the mapping. Treat that as the service not being
  there.
- **The order** decides who draws over whom. Subscribers are called in ascending order, so the highest draws last.
  Use `0` unless you have a reason not to. Community Challenge Mode uses `1000`, so that its mechanics read over
  everything else.
- **`unsubscribe()`** returns once the callback cannot be called again, so the module can be freed right after. Never
  call it from inside the callback: it would wait for its own call.

### In the callback

The callback is called at every moment of every frame, so return early from the moments you don't use.

```cpp
void OnMoment(const good_render::Moment* moment) {
    if (moment->moment != good_render::MOMENT_BEFORE_WINDOWS) {
        return;
    }
    // draw
}
```

- It runs on the game's render thread, in the middle of the game's frame, with the game's targets bound on
  `moment->context`. **Put back whatever you change on the context**: the game's next draw expects its own state.
- Never write a target's alpha. In the world, the engine uses it as a per-pixel tag.
- Everything in `Moment` is valid only for the call.

### Drawing ImGui

`moment->draw_list` is an empty ImGui draw list, ready to draw into like Nexus's background list. The service draws it
once your callback returns.

```cpp
bool DrawnInMoment = false;

void OnMoment(const good_render::Moment* moment) {
    if (moment->moment != good_render::MOMENT_BEFORE_WINDOWS) {
        return;
    }
    DrawMyThings(moment->draw_list);
    DrawnInMoment = true;
}
```

Positions are in the window's pixels, as in any ImGui list, at every moment. Clip rectangles and user callbacks are not
honoured.

### Drawing in the world

A draw list is flat: it draws over everything, so a shape behind a wall shows through it. At the world's moments,
`moment->world_quad` and `moment->world_billboard` draw ImGui's textured quads at points in the world instead, in
MumbleLink's metres. Each pixel is hidden wherever the game drew something solid in front of it, with no depth to
handle on your side.

```cpp
void OnMoment(const good_render::Moment* moment) {
    if (moment->moment != good_render::MOMENT_WORLD_FINISHED || !moment->world_quad) {
        return;
    }
    // A strip on the ground from a to b: the four corners go round the quad, uvs[i] at corners[i].
    const float corners[4][3] = {{a.x - side.x, a.y, a.z - side.z}, {b.x - side.x, b.y, b.z - side.z},
                                 {b.x + side.x, b.y, b.z + side.z}, {a.x + side.x, a.y, a.z + side.z}};
    const float uvs[4][2] = {{0, 0}, {0, 1}, {1, 1}, {1, 0}};
    moment->world_quad(corners, uvs, stripTexture, IM_COL32_WHITE);

    // An icon facing the camera, one metre across.
    moment->world_billboard(iconPosition, 0.5f, 0.5f, iconTexture, IM_COL32_WHITE);

    // A label beside it, flat over the world, as before.
    moment->draw_list->AddText(labelPosition, IM_COL32_WHITE, "Label");
    DrawnInMoment = true;
}
```

The texture is what `AddImageQuad` takes, or null for a plain colour. The quads are drawn after your callback returns,
in the order you called, and before `draw_list`. The two calls are null outside the world (`MOMENT_BEFORE_WINDOWS`
on) and while there is no camera, so draw on `draw_list` then, or fall back to Nexus.

### Falling back to Nexus

Keep drawing in Nexus's render callback, but only on frames the service did not draw:

```cpp
void AddonRender() {
    if (DrawnInMoment) {
        DrawnInMoment = false;
        return;
    }
    DrawMyThings(ImGui::GetBackgroundDrawList());
}
```

All of a frame's moments come before its Present, so any frame the service did not reach still draws through Nexus,
with no gap: a loading screen, the full map, or the service not loaded at all.

### The camera

`moment->camera` is the camera the frame was drawn with, in MumbleLink's space (metres, y up). It is null when there is
no camera: before MumbleLink is written, and while the full map is open.

Use it instead of reading MumbleLink during the moment. The game draws a frame behind the game thread that writes
MumbleLink, so a camera read then is one frame ahead of the world it would draw over, and shapes on the ground slip
while zooming.

To place an ImGui point over the world:

```cpp
bool ToScreen(const good_render::Camera& camera, const glm::vec3& world, ImVec2& screen) {
    const glm::vec4 clip = glm::make_mat4(camera.view_projection) * glm::vec4(world, 1.f);
    if (clip.w <= 0.f) {
        return false;
    }
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    screen = ImVec2((clip.x / clip.w * 0.5f + 0.5f) * display.x, (0.5f - clip.y / clip.w * 0.5f) * display.y);
    return true;
}
```

The matrices are column-major. `glm::make_mat4` and DirectXMath's `XMLoadFloat4x4` both take them as they are. The projection uses the scene's aspect, not
the window's. Below the monitor's resolution the two differ, and the scene is stretched over the window. The
projection's near and far planes are not the game's. To test against the game's depth, compare view depths:
`clip.w` against `linear_depth * moment->z_far`.

### The moments

In frame order. The header says what each one is drawn over and under.

| Moment | Use it for |
|---|---|
| `MOMENT_AFTER_OPAQUE` | geometry the game's ground telegraphs should paint over |
| `MOMENT_AFTER_DECALS` | shapes on the ground, under water, particles and effects |
| `MOMENT_SURFACE_END` | anything in the world that should go through post-processing |
| `MOMENT_WORLD_FINISHED` | icons and shapes in the world, through `world_quad`: the finished world, before any UI |
| `MOMENT_BEFORE_WINDOWS` | an overlay: over names and the HUD, under every window and the tooltip |
| `MOMENT_UI_FINISHED` | over the game's whole UI, still under Nexus's and every addon's windows |

Up to `MOMENT_WORLD_FINISHED`, what is bound is the scene. Resolution scaling can make it smaller than the window, and
`pixels_per_window_pixel` says by how much. `mask` and `linear_depth` are the scene's at every moment. In the game's UI
they are a different size from what is bound, so sample them by the screen's fraction there, not by pixel.

### How it works

[docs/how_it_works.md](docs/how_it_works.md) covers the shared block, and why it is not a Nexus event.

## Versions

Releases are tagged `vMajor.Minor.Build`, the form Nexus's updater reads.

Every addon shares one block, whatever header it was built against. Each subscriber's slot carries the
`good_render::MOMENT_VERSION` of its header, and the service calls the versions it can hand its `Moment` to: its own,
and older ones down to the oldest it still supports. `Moment` only grows at its end, so an older subscriber reads only
the fields it knows.

- **A subscriber the service does not serve is not called**, newer than the service or older than what it supports,
  and draws through Nexus as it would without the service. `good_render::served(*block)` says whether yours is called
  right now, for a message to your users.
- **An older header is deprecated** once a newer one is out: build against the latest, since an older one stops being
  served when `Moment` changes in a way it cannot follow.
