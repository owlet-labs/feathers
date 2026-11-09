/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 *
 * --- Additional terms under GNU GPL version 3 section 7 ---
 * This file requires preservation of the above copyright notice and author attribution
 * in all copies or substantial portions of this file.
 */

#pragma once

#include <visage/ui.h>

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// THE OS FILE DIALOG — the desktop's own "open a file", "choose a folder" and "save as", asked for
// without waiting for the answer: the regular OS file browser.
//
// **ONE ASYNC ROAD ON EVERY PLATFORM.** `open()` returns at once; the answer arrives later, on the
// MESSAGE THREAD, through the callback given to `open()`:
//   - LINUX: the xdg-desktop-portal FileChooser over the session bus, spoken by hand
//     (`file_dialog_portal.h` — no libdbus, the static-link posture), on a worker thread. It is the road
//     that works inside Flatpak Bitwig. With no portal on the bus, `zenity` then `kdialog`, spawned
//     directly (no shell). With neither, the answer is `Failed` and says so.
//   - MACOS: NSOpenPanel / NSSavePanel on the main thread with a completion handler (`file_dialog_mac.mm`).
//   - WINDOWS: IFileOpenDialog / IFileSaveDialog on a worker thread's own COM apartment
//     (`file_dialog_win.cpp`). COMPILED AGAINST THE DOCUMENTED API AND NEVER RUN: CI builds Linux and
//     macOS only.
//
// **HOW THE ANSWER GETS BACK.** A backend never sees the `FileDialog`: it is handed a
// `FileDialogChannel`, shared between the two, and puts its one result there from whatever thread it
// is on. The `FileDialog` takes it out in `poll()`, on the message thread, and only then calls back —
// driven by its own `visage::EventTimer`, which visage services from the window's frame loop, the same
// loop the editor's heartbeat rides. So:
//   - A CALLBACK NEVER RUNS ON A WORKER THREAD, and never inside `open()`, even for a backend that
//     answers at once.
//   - A `FileDialog` DESTROYED WHILE ITS DIALOG IS UP (an editor closed, a panel dismissed) marks the
//     channel abandoned and asks the dialog to close. The worker still finishes into the channel, which
//     it co-owns, and nobody reads it: nothing is ever called on the dead object. The timer is a member,
//     so it dies with it — the reason a posted `runOnEventThread` closure is not used (`PresetBrowser`'s
//     `reopen_` makes the same argument).

namespace feathers::ui {

struct FileFilter {
	std::string label;                   ///< "Audio"
	std::vector<std::string> extensions; ///< without the dot, any case: { "wav", "flac" }
};

struct FileDialogOptions {
	enum class Mode { OpenFile, OpenFolder, SaveFile };
	Mode mode = Mode::OpenFile;
	std::string title;
	std::filesystem::path startDir; ///< empty: the platform's choice
	std::vector<FileFilter> filters; ///< empty: any file. Ignored for a folder
	std::string defaultName;         ///< SaveFile: the name the field starts with
	/// THE WINDOW TO BE ABOVE — X11 `Window`, `NSView*`, `HWND`, as visage's `nativeHandle()` gives it.
	/// Null: a free-standing dialog. `nativeWindowOf` finds it for a frame.
	void* parent = nullptr;
};

struct FileDialogResult {
	enum class Status { Chosen, Cancelled, Failed };
	Status status = Status::Failed;
	std::filesystem::path path; ///< Chosen only
	std::string error;          ///< Failed only: why, in words a notice can show

	[[nodiscard]] bool chosen() const noexcept { return status == Status::Chosen; }
};

/// **THE ONE OBJECT A BACKEND AND A `FileDialog` SHARE.** Held by `shared_ptr` on both sides, so a
/// worker that outlives its requester still has somewhere to write.
class FileDialogChannel {
public:
	/// ANY THREAD. The FIRST result wins; later ones are dropped (a cancel racing a choice).
	void complete(FileDialogResult result);
	/// ANY THREAD. True once the requester cancelled or was destroyed: a worker may stop early.
	[[nodiscard]] bool abandoned() const noexcept { return abandoned_.load(std::memory_order_acquire); }

private:
	friend class FileDialog;
	void abandon() noexcept { abandoned_.store(true, std::memory_order_release); }
	std::optional<FileDialogResult> take();

	mutable std::mutex mutex_;
	std::optional<FileDialogResult> result_;
	bool completed_ = false;
	std::atomic<bool> abandoned_ { false };
};

/// **WHAT ACTUALLY SHOWS A DIALOG.** One per platform (`platformFileDialogBackend`), and a fake in the
/// cases. MESSAGE THREAD for `start`, which must return promptly; the dialog then lives in the
/// returned session and completes `channel` exactly once, from any thread — or, once `cancel()` has
/// been called, may not complete it at all.
class FileDialogBackend {
public:
	class Session {
	public:
		virtual ~Session() = default;
		/// MESSAGE THREAD. Ask the dialog to go away. Best effort, and it may still complete.
		virtual void cancel() = 0;
	};
	virtual ~FileDialogBackend() = default;
	/// MESSAGE THREAD. Can a dialog be shown at all? macOS and Windows always can; Linux needs a portal on
	/// the session bus, or zenity or kdialog on PATH. Cheap after the first call (Linux keeps it 5 s).
	virtual bool available() { return true; }
	/// Null: nothing could be started, and `channel` has been completed with the reason.
	virtual std::unique_ptr<Session> start(const FileDialogOptions& options, std::shared_ptr<FileDialogChannel> channel) = 0;
};

/// This platform's backend, for the life of the process.
FileDialogBackend& platformFileDialogBackend();

/// **THE CASES' BACKEND, PROCESS-WIDE, WHILE IT LIVES.** Every `FileDialog` built without an explicit
/// backend uses the innermost one — so a case drives the preset browser's own dialogs without a
/// seam per widget, and no case ever puts a real dialog on somebody's desktop.
class ScopedFileDialogBackend {
public:
	explicit ScopedFileDialogBackend(FileDialogBackend& backend);
	~ScopedFileDialogBackend();
	ScopedFileDialogBackend(const ScopedFileDialogBackend&) = delete;
	ScopedFileDialogBackend& operator=(const ScopedFileDialogBackend&) = delete;

private:
	FileDialogBackend* previous_;
};

/// The native window a frame is shown in (for `FileDialogOptions::parent`), or null.
void* nativeWindowOf(const visage::Frame* frame);

/// **ONE DIALOG AT A TIME, OWNED BY WHOEVER ASKED.** Hold it as a member of the widget that opens it:
/// its lifetime IS the guarantee that the callback never reaches a dead widget.
class FileDialog {
public:
	using Callback = std::function<void(const FileDialogResult&)>;

	/// `backend` null: at each `open()`, the scoped test backend when one is installed, else the platform's.
	explicit FileDialog(FileDialogBackend* backend = nullptr);
	/// Cancels an open dialog. Its callback is never called.
	~FileDialog();
	FileDialog(const FileDialog&) = delete;
	FileDialog& operator=(const FileDialog&) = delete;

	/// **MESSAGE THREAD. CAN A PICKER BE SHOWN AT ALL?** — the backend `open()` would use now (the scoped
	/// test backend, else the platform's). False: keep a fallback UI, or disable the row that opens one.
	[[nodiscard]] static bool available();

	/// MESSAGE THREAD. Show a dialog; `done` is called ONCE, later, on this thread, from `poll()`.
	/// False — `done` never called — while another is still open from this object: a second click on
	/// "Import…" does not stack a second dialog.
	bool open(const FileDialogOptions& options, Callback done);
	/// MESSAGE THREAD. Close the dialog without an answer: the callback is dropped, not called.
	void cancel();
	[[nodiscard]] bool isOpen() const noexcept { return channel_ != nullptr; }

	/// **MESSAGE THREAD — THE ONE PLACE A CALLBACK RUNS.** Delivers a finished answer; true when it did.
	/// Called by this object's own timer; public so a case (or a tick) can drive it without an event loop.
	/// The callback may destroy this `FileDialog` or open another: nothing here touches `this` after it.
	bool poll();

	/// How often the timer looks, in ms.
	static constexpr int kPollMs = 30;

private:
	class Timer final : public visage::EventTimer {
	public:
		explicit Timer(FileDialog& owner) : owner_(owner) {}
		void timerCallback() override { (void)owner_.poll(); }

	private:
		FileDialog& owner_;
	};

	FileDialogBackend* backend_; ///< null: resolved at `open()`
	std::shared_ptr<FileDialogChannel> channel_;
	std::unique_ptr<FileDialogBackend::Session> session_;
	Callback done_;
	std::thread::id thread_;
	/// LAST, so it stops before anything it would read is gone.
	Timer timer_ { *this };
};

} // namespace feathers::ui
