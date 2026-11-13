# How it works

## Why not a Nexus event

Nexus's `Events.Raise` calls every subscriber while it holds one recursive mutex, shared by every event of every addon
(`Host/Events/EvtApi.cpp` in Nexus). Drawing inside it would hold up any other thread that raises any event, for as long
as the drawing takes. That includes arcdps's combat events on the game's thread.

So the subscribers are a table in shared memory, and the service calls each one directly, with no lock between them.

## The block

- **The block** is a named file mapping, `GW2GoodRender.v1.<pid>`, made zeroed by the first side to open it. Every
  side opens it for itself, whichever loads first, and all zeroes is an empty block with no service, so nobody sets it
  up. Its layout never changes: `v1` is the layout, and a new one would be a new name.
  - It is not Nexus's DataLink. An addon's `DataLink.Share` makes the resource in Nexus's own memory, not a file
    mapping (`Core/DataLink/DlApi.cpp`, called with `aIsPublic` false), so only addons loaded by Nexus reach it. One
    side is not: Community Challenge Mode can also be loaded by arcdps, or as a `d3d11.dll` proxy, and coordinates
    through the same block.
  - That memory is also not zeroed (`new char[size]`), while the block depends on starting zeroed. `Share` returns
    the same pointer to whoever asks, first or not, so a side cannot tell whether it is the one to clear it.
- **A subscriber** claims a slot with an atomic compare-and-swap, and leaves an order, its header's
  `MOMENT_VERSION` and a callback in it, the callback last.
- **The service says which versions it calls**, in `oldest_served` and `newest_served`, and skips the slots of any
  other. Both are zero while no service is loaded.
- **The service counts each call into a slot before it reads the slot's callback.** So `unsubscribe()` can clear the
  callback, then wait for the count to drop to zero, and the subscriber can unload as soon as it returns.
- **The service never hands out a pointer into itself**, except `draw_list`, `world_quad` and `world_billboard`, which
  are valid only during a call. What they collected it draws before the call is counted out, so the textures they name
  may be the subscriber's own. The world's quads go through the same shaders as the list, with each pixel's view depth
  tested against `linear_depth`.
- **Once a frame, outside any moment, the service frees the slot of any subscriber whose module is gone** without
  unsubscribing, so it never calls into freed memory.

## One follower of the frame at a time

Following the frame means detouring the game's D3D11 calls to find where each moment falls, and two addons doing it at
once would each read the other's drawing as the game's frame moving. Community Challenge Mode follows the frame itself
while the service is not there, and hands it over when the service comes. The two agree on who follows through a
mapping of their own, outside this header: a subscriber has no part in it.
