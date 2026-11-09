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

// THE WINDOWS FILE DIALOG — IFileOpenDialog / IFileSaveDialog, FOS_PICKFOLDERS for a folder.
//
// **UNTESTED. WRITTEN AGAINST THE DOCUMENTED API AND NEVER COMPILED HERE** — CI builds Linux and macOS
// only. Treat the first Windows build as its first review.
//
// `IFileDialog::Show` is modal and runs its own message loop until the person answers, so it runs on a
// WORKER THREAD in a single-threaded apartment of its own (the dialog's COM objects never cross
// threads), owned by the plugin's window so it sits above it. The host's UI thread returns at once.
// A cancel from the message thread posts WM_CLOSE to the worker's windows — `PostMessage` is the one
// thing that is safe to aim at another thread's window. Workers are joined at static destruction, as on
// Linux, so an unloaded DLL is never woken into.

#if defined(_WIN32)

#include "plugin/ui/file_dialog.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shobjidl.h>

#include <atomic>
#include <list>
#include <string>
#include <thread>
#include <vector>

namespace feathers::ui {

namespace {

using Status = FileDialogResult::Status;

struct Control {
	std::atomic<DWORD> threadId { 0 };
	std::atomic<bool> cancelled { false };
	std::atomic<bool> finished { false };

	void cancel() {
		cancelled.store(true);
		const DWORD id = threadId.load();
		if (id == 0) {
			return; // not up yet: the worker checks `cancelled` before it shows anything
		}
		EnumThreadWindows(
		    id,
		    [](HWND hwnd, LPARAM) -> BOOL {
			    PostMessageW(hwnd, WM_CLOSE, 0, 0);
			    return TRUE;
		    },
		    0);
	}
};

std::wstring wide(const std::string& utf8) {
	if (utf8.empty()) {
		return {};
	}
	const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
	std::wstring out(static_cast<size_t>(n), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
	return out;
}

template <typename T>
void release(T*& p) {
	if (p != nullptr) {
		p->Release();
		p = nullptr;
	}
}

FileDialogResult show(const FileDialogOptions& options, const Control& control) {
	using Mode = FileDialogOptions::Mode;
	FileDialogResult result;
	result.status = Status::Failed;

	IFileDialog* dialog = nullptr;
	const HRESULT created = options.mode == Mode::SaveFile
	                            ? CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))
	                            : CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
	if (FAILED(created) || dialog == nullptr) {
		result.error = "Could not create the file dialog";
		return result;
	}

	FILEOPENDIALOGOPTIONS flags = 0;
	if (SUCCEEDED(dialog->GetOptions(&flags))) {
		flags |= FOS_FORCEFILESYSTEM;
		if (options.mode == Mode::OpenFolder) {
			flags |= FOS_PICKFOLDERS;
		}
		if (options.mode == Mode::SaveFile) {
			flags |= FOS_OVERWRITEPROMPT;
		} else {
			flags |= FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST;
		}
		dialog->SetOptions(flags);
	}
	if (!options.title.empty()) {
		dialog->SetTitle(wide(options.title).c_str());
	}

	// The filter strings must outlive SetFileTypes' copy — they are copied, but keep them anyway.
	std::vector<std::wstring> names;
	std::vector<std::wstring> specs;
	std::vector<COMDLG_FILTERSPEC> filterSpecs;
	if (options.mode != Mode::OpenFolder && !options.filters.empty()) {
		for (const FileFilter& f : options.filters) {
			names.push_back(wide(f.label));
			std::wstring spec;
			for (const std::string& ext : f.extensions) {
				if (!spec.empty()) {
					spec += L';';
				}
				spec += L"*." + wide(ext); // Windows matches case-insensitively already
			}
			specs.push_back(std::move(spec));
		}
		for (size_t i = 0; i < names.size(); ++i) {
			filterSpecs.push_back({ names[i].c_str(), specs[i].c_str() });
		}
		dialog->SetFileTypes(static_cast<UINT>(filterSpecs.size()), filterSpecs.data());
		if (options.mode == Mode::SaveFile && !options.filters.front().extensions.empty()) {
			dialog->SetDefaultExtension(wide(options.filters.front().extensions.front()).c_str());
		}
	}
	if (options.mode == Mode::SaveFile && !options.defaultName.empty()) {
		dialog->SetFileName(wide(options.defaultName).c_str());
	}
	if (!options.startDir.empty()) {
		IShellItem* folder = nullptr;
		if (SUCCEEDED(SHCreateItemFromParsingName(options.startDir.wstring().c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
			dialog->SetFolder(folder);
		}
		release(folder);
	}

	if (control.cancelled.load()) {
		release(dialog);
		result.status = Status::Cancelled;
		return result;
	}
	const HRESULT shown = dialog->Show(static_cast<HWND>(options.parent));
	if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
		result.status = Status::Cancelled;
	} else if (FAILED(shown)) {
		result.error = "The file dialog failed to open";
	} else {
		IShellItem* item = nullptr;
		PWSTR path = nullptr;
		if (SUCCEEDED(dialog->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
			result.status = Status::Chosen;
			result.path = std::filesystem::path(path);
			CoTaskMemFree(path);
		} else {
			result.error = "The chosen item is not a file on disk";
		}
		release(item);
	}
	release(dialog);
	return result;
}

class WinSession final : public FileDialogBackend::Session {
public:
	explicit WinSession(std::shared_ptr<Control> control) : control_(std::move(control)) {}
	void cancel() override { control_->cancel(); }

private:
	std::shared_ptr<Control> control_;
};

class WinBackend final : public FileDialogBackend {
public:
	~WinBackend() override {
		for (Worker& w : workers_) {
			w.control->cancel();
		}
		for (Worker& w : workers_) {
			if (w.thread.joinable()) {
				w.thread.join();
			}
		}
	}

	std::unique_ptr<Session> start(const FileDialogOptions& options, std::shared_ptr<FileDialogChannel> channel) override {
		for (auto it = workers_.begin(); it != workers_.end();) {
			if (it->control->finished.load()) {
				it->thread.join();
				it = workers_.erase(it);
			} else {
				++it;
			}
		}
		auto control = std::make_shared<Control>();
		Worker& worker = workers_.emplace_back();
		worker.control = control;
		worker.thread = std::thread([options, channel, control] {
			control->threadId.store(GetCurrentThreadId());
			const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
			if (FAILED(init)) {
				channel->complete({ Status::Failed, {}, "Could not start COM for the file dialog" });
			} else {
				channel->complete(show(options, *control));
				CoUninitialize();
			}
			control->finished.store(true);
		});
		return std::make_unique<WinSession>(std::move(control));
	}

private:
	struct Worker {
		std::shared_ptr<Control> control;
		std::thread thread;
	};
	std::list<Worker> workers_;
};

} // namespace

FileDialogBackend& platformFileDialogBackend() {
	static WinBackend backend;
	return backend;
}

} // namespace feathers::ui

#endif
