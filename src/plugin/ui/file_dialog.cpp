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

#include "plugin/ui/file_dialog.h"

#include <visage/app.h>

#include <utility>

namespace feathers::ui {

namespace {

/// The innermost `ScopedFileDialogBackend`. Message thread only, like every `FileDialog`.
FileDialogBackend* scopedBackend = nullptr;

} // namespace

void FileDialogChannel::complete(FileDialogResult result) {
	const std::lock_guard<std::mutex> lock(mutex_);
	if (completed_) {
		return;
	}
	completed_ = true;
	result_ = std::move(result);
}

std::optional<FileDialogResult> FileDialogChannel::take() {
	const std::lock_guard<std::mutex> lock(mutex_);
	std::optional<FileDialogResult> out = std::move(result_);
	result_.reset();
	return out;
}

ScopedFileDialogBackend::ScopedFileDialogBackend(FileDialogBackend& backend) : previous_(scopedBackend) {
	scopedBackend = &backend;
}

ScopedFileDialogBackend::~ScopedFileDialogBackend() { scopedBackend = previous_; }

void* nativeWindowOf(const visage::Frame* frame) {
	const visage::Frame* top = frame;
	while (top != nullptr && top->parent() != nullptr) {
		top = top->parent();
	}
	const auto* editor = dynamic_cast<const visage::ApplicationEditor*>(top);
	if (editor == nullptr || editor->window() == nullptr) {
		return nullptr;
	}
	return editor->window()->nativeHandle();
}

bool FileDialog::available() {
	return (scopedBackend != nullptr ? *scopedBackend : platformFileDialogBackend()).available();
}

FileDialog::FileDialog(FileDialogBackend* backend)
    : backend_(backend) {}

FileDialog::~FileDialog() {
	timer_.stopTimer();
	cancel();
}

bool FileDialog::open(const FileDialogOptions& options, Callback done) {
	if (isOpen()) {
		return false;
	}
	thread_ = std::this_thread::get_id();
	channel_ = std::make_shared<FileDialogChannel>();
	done_ = std::move(done);
	// THE BACKEND IS CHOSEN HERE, NOT AT CONSTRUCTION: a widget built before a case installs its fake
	// still opens the fake.
	FileDialogBackend& backend =
	    backend_ != nullptr ? *backend_ : (scopedBackend != nullptr ? *scopedBackend : platformFileDialogBackend());
	// A BACKEND THAT ANSWERS INSIDE `start` (a refusal, a fake) has only written the channel: the answer
	// still waits for `poll()`, so no caller is ever called back from inside its own `open()`.
	session_ = backend.start(options, channel_);
	timer_.startTimer(kPollMs);
	return true;
}

void FileDialog::cancel() {
	if (!isOpen()) {
		return;
	}
	channel_->abandon();
	if (session_ != nullptr) {
		session_->cancel();
	}
	session_.reset();
	channel_.reset();
	done_ = nullptr;
	timer_.stopTimer();
}

bool FileDialog::poll() {
	// THE MESSAGE THREAD OR NOTHING. A poll from anywhere else leaves the answer where it is for the
	// timer to deliver; a callback on a worker is the one outcome this class exists to rule out.
	if (!isOpen() || std::this_thread::get_id() != thread_) {
		return false;
	}
	std::optional<FileDialogResult> result = channel_->take();
	if (!result.has_value()) {
		return false;
	}
	// EVERYTHING OFF `this` FIRST: the callback may destroy this object or open the next dialog.
	Callback done = std::move(done_);
	done_ = nullptr;
	session_.reset();
	channel_.reset();
	timer_.stopTimer();
	if (done) {
		done(*result);
	}
	return true;
}

} // namespace feathers::ui
