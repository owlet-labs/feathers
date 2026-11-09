# Upstream issue draft for visage: destroying a window-paired layer deadlocks the caller (Vulkan)

**Filed:** https://github.com/VitalAudio/visage/pull/98

**Repo:** github.com/VitalAudio/visage
**We have a fix:** one commit, four lines and a guard. Offer it as a PR rather than a bug report.
**Our branch:** `owlet-labs/visage @ fix/layer-destroy-deadlock` (018cd86)

---

## Summary

Showing a `ApplicationWindow` a second time — or destroying one and creating another in the same
process — deadlocks the calling thread on the Vulkan backend. In a plugin this is the host's main
thread, so the editor never reopens, the DAW hangs when it next needs that thread (saving the
project, for us), and the host eventually reports the plugin as not responding.

## Reproduction

A CLAP plugin editor, opened and closed and opened again. `ApplicationWindow::show(parent)` begins
with `removeFromWindow()` and then builds a new window, so the second open is enough:

1. `gui->create` / `set_parent` → editor appears, draws normally
2. `gui->destroy`
3. `gui->create` / `set_parent` → **never returns**

Also reachable without reopening: destroy the object that owns the `ApplicationWindow` and create
another one (for us, a second plugin instance after the first is removed).

## Where it blocks

`eu-stack` on the wedged process:

```
bgfx::vk::CommandQueueVK::consume()
bgfx::vk::SwapChainVK::destroy()
bgfx::vk::FrameBufferVK::destroy()
bgfx::vk::RendererContextVK::createFrameBuffer(...)     <- the NEW window's frame buffer
bgfx::Context::rendererExecCommands(...)
bgfx::Context::frame(bool)
visage::Canvas::submit(int)
visage::ApplicationEditor::addToWindow(visage::Window*)
visage::ApplicationWindow::show(void*)
```

and on another thread, the other half:

```
__poll
xcb_wait_for_special_event                              <- Mesa's WSI present thread
```

## The mechanism

`bgfx::destroy` only QUEUES the destruction; the backend performs it inside a later `bgfx::frame()`.

`Layer::removeFromWindow()` (visage_graphics/layer.h) queues the frame buffer's destroy and returns:

```cpp
void removeFromWindow() {
  window_handle_ = nullptr;
  destroyFrameBuffer();     // bgfx::destroy(handle) — deferred
}
```

Nothing submits a frame between that and the window itself being torn down. So the deferred
`FrameBufferVK::destroy()` runs much later — inside the NEXT `createFrameBuffer` — by which time its
surface is gone. `SwapChainVK::destroy()` then waits in `CommandQueueVK::consume()` for a queue that
can never drain, because Mesa's WSI thread is waiting for a Present completion from a window that no
longer exists.

The order is wrong rather than the operation.

## The fix we are running

Submit the frames that execute the deferred destroy **while the window still exists**:

```cpp
void Layer::removeFromWindow() {
  // Only when there was something paired: show() calls this before building its FIRST window too,
  // when bgfx::init has not run yet (it happens later in addToWindow), and submitting a frame there
  // crashes outright.
  const bool had_frame_buffer = frame_buffer_data_ && bgfx::isValid(frame_buffer_data_->handle);
  window_handle_ = nullptr;
  destroyFrameBuffer();
  if (!had_frame_buffer)
    return;

  bgfx::frame();
  bgfx::frame();
}
```

Two frames because that is what it takes for a deferred command to be executed and retired, and it
is the idiom `post_effects.cpp`'s `destroyFrameBuffers()` already uses after destroying its own
frame buffers — which is part of why we think this is a missing call rather than a design choice.

The guard is not hypothetical: without it the fix crashes on the very first open, because `show()`
calls `removeFromWindow` before there is any renderer at all.

## Verified

In a CLAP host harness that opens, closes and reopens the editor: three cycles clean where every
previous attempt hung on the second, and four cycles clean with the owning object destroyed and
re-created between rounds. Linux/X11, Mesa RADV, `VISAGE_BACKGROUND_GRAPHICS_THREAD=1`.

## Note for maintainers

We could not find a supported way to avoid this from the application side. `Renderer::initialize`
latches `initialized_` and nothing resets it, `bgfx::shutdown` is called nowhere in the tree, and
`ApplicationWindow::show()` always destroys and rebuilds. Our own interim workaround was to keep one
window for the life of the process and reparent it between host containers with plain X11 — which
works, but cannot cover the object being destroyed, and is not something an application should have
to invent.
