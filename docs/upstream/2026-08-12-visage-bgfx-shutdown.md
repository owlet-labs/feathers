# Upstream note for visage: bgfx is initialised once and never shut down

**Filed:** https://github.com/VitalAudio/visage/issues/99

**Repo:** github.com/VitalAudio/visage
**Context:** vendored as a submodule in a CLAP plugin (Linux/X11), where the host
`dlopen`s the plugin, closes the editor, unloads the library and exits. bgfx selects the Vulkan
backend here (Mesa RADV); the same lifecycle question applies whichever backend is chosen.

## What we observed

`Renderer::initialize()` (visage_graphics/renderer.cpp:104) calls `bgfx::init()` and
`startRenderThread()`, guarded by `initialized_`:

```cpp
void Renderer::initialize(void* model_window, void* display) {
  if (initialized_)
    return;
  callback_handler_ = std::make_unique<GraphicsCallbackHandler>();
  initialized_ = true;
  startRenderThread();
  bgfx::Init bgfx_init;
  ...
  bgfx::init(bgfx_init);
```

There is no matching teardown. `bgfx::shutdown` does not appear anywhere in the library — the whole
tree has zero occurrences. `Renderer::~Renderer()` only calls `Thread::stop()` on its own render
thread; the bgfx context, and the GL context under it, live until the process ends.

With `VISAGE_BACKGROUND_GRAPHICS_THREAD=1` (the default on Linux) that leaves, at process exit:

- visage's own render thread, stopped only when the `Renderer` singleton is destroyed — which is at
  static destruction time, i.e. during the same unwind that is tearing everything else down;
- the graphics context, never released, and with it the driver's own threads. Here bgfx selects the
  Vulkan backend and Mesa's RADV driver spawns several: `WSI swapchain q`, `WSI swapchain e` and two
  `<app>:disk$0` threads, all alive in `/proc` for the whole life of the process.

ThreadSanitizer reports a data race between the main thread and one of those driver threads, on
every run, in a plugin host that opens the editor once and then exits:

```
WARNING: ThreadSanitizer: data race
  Write of size 8 at 0x721400005000 by main thread:
    #0 free
    #1 libvulkan_radeon.so+0x2865b5

  Previous write of size 8 at 0x721400005000 by thread T2:
    #0 malloc
    #1 libvulkan_radeon.so+0x286324

  Thread T2 '<app>:disk$0' (running) created by main thread at:
    #0 pthread_create
    #1 libvulkan_radeon.so+0x2ae0f3
    #2 bgfx::vk::rendererCreate(bgfx::Init const&)   renderer_vk.cpp:4631
    #3 bgfx::rendererCreate(bgfx::Init const&)       bgfx.cpp:2789
    ...
    #9 bgfx::init(bgfx::Init const&)                 bgfx.cpp:3584
   #10 visage::Renderer::initialize(void*, void*)    renderer.cpp:154
   #11 visage::ApplicationEditor::addToWindow(visage::Window*)
   #12 visage::ApplicationWindow::showWindow(bool)
```

The thread is still marked `running` when the main thread frees driver memory underneath it. Since
nothing ever calls `bgfx::shutdown()`, the Vulkan device is never destroyed and that thread is never
joined — it simply outlives whatever tears the process down.

For a normal application that exits when its window closes, none of this matters. For a plugin it
is different in kind: the host owns the process, opens and closes the editor repeatedly, and
eventually `dlclose`s the module while the rest of the host keeps running.

## Why it is awkward from the plugin side

There is no supported way to undo `initialize()`:

- `bgfx::shutdown()` is never called and there is no visage API that would call it;
- `Renderer` inherits `Thread`, so `Renderer::instance().stop()` compiles and would stop the render
  thread — but it is a one-way door. `initialize()` early-returns on `initialized_`, which nothing
  resets, so `startRenderThread()` never runs again and the next editor the host opens gets a dead
  renderer. Trading a shutdown-time problem for a broken second open is not a trade a plugin can
  make.

So a plugin can close its window correctly, destroy every frame it owns, and still have no way to
return the graphics stack to the state it was in before the editor was first opened.

## What we are not claiming

We chased an intermittent exit-time segfault in our own plugin and initially suspected this. It was
not the cause: the actual bug was ours (a static font cache outliving visage's `FontCache`, so
`Font::~Font` reached into a destroyed singleton). Fixing that took the crash rate from ~43% of runs
to ~5%. A residual exit-only crash remains, which we have not yet attributed, and this is one of the
remaining candidates — but we have no evidence pinning it here, and we would rather report the
lifecycle gap on its own merits than dress it up as a crash report.

## What would help

Any of, roughly in order of usefulness to a plugin:

1. A `Renderer::shutdown()` (or destructor path) that calls `bgfx::shutdown()` and clears
   `initialized_`, so the pair `initialize()`/`shutdown()` is symmetric and a later `initialize()`
   works.
2. Failing that, documenting that bgfx stays initialised for the life of the process, so plugin
   authors know the state they are leaving behind and can decide what to do about it.
3. A note on the intended lifetime of the `Renderer` singleton in a `dlopen`ed module, where
   "process exit" and "library unload" are not the same event and may be racing each other.

Happy to test a patch against the plugin case — repeated editor open/close plus library unload is
easy for us to reproduce and is the shape most plugin hosts exercise.
