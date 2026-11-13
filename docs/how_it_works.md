# How it works

## Why not a Nexus event

Nexus's `Events.Raise` calls every subscriber while it holds one recursive mutex, shared by every event of every addon
(`Host/Events/EvtApi.cpp` in Nexus). Drawing inside it would hold up any other thread that raises any event, for as long
as the drawing takes. That includes arcdps's combat events on the game's thread.

So the subscribers are a table in shared memory, and the service calls each one directly, with no lock between them.

## The block

- **The block** is a named file mapping, `GW2GoodRender.<pid>`, made zeroed by the first side to open it. Every side
  opens it for itself, whichever loads first.
  - It is not Nexus's DataLink, though that is a named file mapping too. `DataLink.Share` zeroes the memory even when it
    opens a mapping that already exists, which would wipe the subscribers of a block another side made first.
  - A plain mapping also works for an addon that is not loaded by Nexus.
- **A subscriber** claims a slot with an atomic compare-and-swap, and leaves a callback and an order in it.
- **The service counts each call into a slot before it reads the slot's callback.** So `unsubscribe()` can clear the
  callback, then wait for the count to drop to zero, and the subscriber can unload as soon as it returns.
- **The service never hands out a pointer into itself**, except `draw_list`, `world_quad` and `world_billboard`, which
  are valid only during a call. What they collected it draws before the call is counted out, so the textures they name
  may be the subscriber's own. The world's quads go through the same shaders as the list, with each pixel's view depth
  tested against `linear_depth`.
- **Once a frame, outside any moment, the service frees the slot of any subscriber whose module is gone** without
  unsubscribing, so it never calls into freed memory.

## One follower of the frame at a time

Following the frame means detouring the game's D3D11 calls to find where each moment falls. Two addons doing it at once
would each read the other's drawing as the game's frame moving. So the block says who follows the frame now, `owner`,
and whether the service is loaded and able to follow it, `service`.

An addon that also follows the frame on its own, as Community Challenge Mode does when the service is not loaded, keeps
to this protocol at every Present:

- **While `service` is 0**, it may take the frame: a compare-and-swap of `owner` from `OWNER_NONE` to `OWNER_ADDON`. If
  the swap fails, someone else has it, and the addon does not follow.
- **When `service` becomes 1**, it stops following, then sets `owner` back to `OWNER_NONE`. The service takes the frame
  from nobody at its own next Present. A frame or two draws nothing in between, and there are never two followers.
- **When the service unloads**, it stops following before it gives `owner` back. The addon takes the frame again at its
  next Present.

An addon that only subscribes never touches `owner` or `service`.
