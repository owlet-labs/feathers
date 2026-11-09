# Upstream issue draft for visage: `ApplicationWindow::close()` treats an empty callback list as a veto

**Filed:** https://github.com/VitalAudio/visage/issues/100

**Repo:** github.com/VitalAudio/visage
**Size:** one line, and the two call sites already disagree with each other.

---

## Summary

`ApplicationWindow::close()` and `hide()` do nothing at all unless the application has set an
`onCloseRequested` handler. An app that never sets one — which is the default, and which nothing
documents as required — gets a silent no-op from both.

## Where

`visage_app/application_window.cpp`:

```cpp
void ApplicationWindow::hide() {
  if (window_ && onCloseRequested().callback())
    return window_->hide();
}

void ApplicationWindow::close() {
  if (window_ && onCloseRequested().callback()) {
    removeFromWindow();
    window_ = nullptr;
  }
}
```

`CallbackList::callback()` (visage_utils/events.h) returns `defaultResult<R>()` when the list is
empty, and for `bool` that is a default-constructed `false`:

```cpp
template<typename... Args>
auto callback(Args&&... args) const {
  if (callbacks_.empty())
    return defaultResult<decltype(std::declval<std::function<T>>()(args...))>();
  ...
}
```

So "nobody has an opinion" is read as "somebody vetoed it".

## The inconsistency

visage's own window-manager close path checks emptiness FIRST and treats it as permission —
`visage_app/window_event_handler.cpp:102`:

```cpp
if (editor_->onCloseRequested().isEmpty())
  return true;
return editor_->onCloseRequested().callback();
```

So closing via the window manager is allowed when no handler is set, while closing
programmatically is refused. The two paths disagree about the same empty list.

## Why it matters

We spent a while with a teardown that we believed ran in the order its own comment described. It did
not: our `app->close()` had been a silent no-op since the first commit that used it, so widgets were
being destroyed while still attached to a live window and the window's destructor then walked them.
Nothing crashed on our platform, which is exactly what made it durable.

## Suggested fix

Use the same rule in both places — empty means allow:

```cpp
void ApplicationWindow::close() {
  if (window_ && (onCloseRequested().isEmpty() || onCloseRequested().callback())) {
    removeFromWindow();
    window_ = nullptr;
  }
}
```

If the intended semantics really are "no handler means do not close", then it wants a doc comment on
`onCloseRequested()` saying so, and `window_event_handler.cpp` wants correcting to match.
