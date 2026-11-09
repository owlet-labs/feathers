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

// THE LINUX FILE DIALOG: the portal conversation (`file_dialog_portal.h`) over a socket on a worker
// thread, then zenity, then kdialog. Everything it says and reads is built and parsed by the pure
// half; this file is only sockets, processes and the thread.
//
// **THE WORKER NEVER TOUCHES THE `FileDialog`** — only the channel and its own control block, both
// shared. A cancel (or a destroyed `FileDialog`) writes the control's pipe; the worker, which is only
// ever blocked in `poll()` on that pipe and its socket or its child's stdout, wakes, closes the portal
// request (or kills its own child, by pid) and ends.
//
// **AND IT IS JOINED BEFORE THE LIBRARY GOES.** A detached thread still blocked in `poll()` when the
// host unloads the plugin would wake into unmapped code. So the backend keeps every worker, reaps the
// finished ones as new ones start, and at static destruction — which a `dlclose` runs — cancels and
// joins the rest. Each ends promptly once cancelled: nothing it waits on is unbounded after the pipe.

#include "plugin/ui/file_dialog.h"
#include "plugin/ui/file_dialog_portal.h"

#if defined(__linux__)

#include "plugin/ui/file_dialog_bus.h"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <list>
#include <thread>

extern char** environ;

//------------------------------------------------------------------------------------------------//
// THE BUS                                                                                          //
//------------------------------------------------------------------------------------------------//

namespace feathers::ui::portal {

BusConnection::~BusConnection() {
	if (fd_ >= 0) {
		::close(fd_);
	}
}

bool BusConnection::connect(const BusAddress& address) {
	sockaddr_un sa {};
	sa.sun_family = AF_UNIX;
	const size_t room = sizeof(sa.sun_path) - (address.abstract ? 1 : 0) - 1;
	if (address.path.size() > room) {
		return false;
	}
	socklen_t length = 0;
	if (address.abstract) {
		// THE ABSTRACT NAMESPACE: a leading NUL, and the length counts the name exactly, no terminator.
		sa.sun_path[0] = '\0';
		std::memcpy(sa.sun_path + 1, address.path.data(), address.path.size());
		length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + address.path.size());
	} else {
		std::memcpy(sa.sun_path, address.path.data(), address.path.size());
		length = static_cast<socklen_t>(sizeof(sa));
	}
	if (fd_ >= 0) {
		::close(fd_);
	}
	fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd_ < 0) {
		return false;
	}
	if (::connect(fd_, reinterpret_cast<const sockaddr*>(&sa), length) != 0) {
		::close(fd_);
		fd_ = -1;
		return false;
	}
	return true;
}

bool BusConnection::write(const void* data, size_t size) {
	const auto* p = static_cast<const uint8_t*>(data);
	while (size > 0) {
		const ssize_t n = ::send(fd_, p, size, MSG_NOSIGNAL);
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n <= 0) {
			return false;
		}
		p += n;
		size -= static_cast<size_t>(n);
	}
	return true;
}

bool BusConnection::authenticate(int wakeFd, int timeoutMs) {
	const std::string auth = authExternal(static_cast<uint32_t>(::getuid()));
	if (!write(auth.data(), auth.size())) {
		return false;
	}
	std::string line;
	if (readLine(line, wakeFd, timeoutMs) != Wait::Got || line.rfind("OK ", 0) != 0) {
		return false;
	}
	static constexpr char kBegin[] = "BEGIN\r\n";
	return write(kBegin, sizeof kBegin - 1);
}

uint32_t BusConnection::send(dbus::Message message) {
	message.serial = ++serial_;
	const std::vector<uint8_t> bytes = dbus::encode(message);
	return write(bytes.data(), bytes.size()) ? message.serial : 0;
}

BusConnection::Wait BusConnection::next(dbus::Message& out, int wakeFd, int timeoutMs) {
	for (;;) {
		bool malformed = false;
		const size_t length = dbus::messageLength(in_.data(), in_.size(), &malformed);
		if (malformed) {
			return Wait::Closed;
		}
		if (length != 0 && in_.size() >= length) {
			const bool ok = dbus::decode(in_.data(), length, out);
			in_.erase(in_.begin(), in_.begin() + static_cast<std::ptrdiff_t>(length));
			return ok ? Wait::Got : Wait::Closed;
		}
		if (const Wait w = fill(wakeFd, timeoutMs); w != Wait::Got) {
			return w;
		}
	}
}

std::string BusConnection::hello(int wakeFd, int timeoutMs) {
	const uint32_t serial = send(portal::hello(0));
	if (serial == 0) {
		return {};
	}
	dbus::Message msg;
	for (;;) {
		if (next(msg, wakeFd, timeoutMs) != Wait::Got) {
			return {};
		}
		if (msg.replySerial == serial) {
			return parseString(msg).value_or(std::string {});
		}
	}
}

std::optional<dbus::Message> BusConnection::call(dbus::Message message, int wakeFd, int timeoutMs) {
	const uint32_t serial = send(std::move(message));
	if (serial == 0) {
		return std::nullopt;
	}
	dbus::Message msg;
	for (;;) {
		if (next(msg, wakeFd, timeoutMs) != Wait::Got) {
			return std::nullopt;
		}
		if (msg.replySerial == serial && (msg.type == dbus::Type::MethodReturn || msg.type == dbus::Type::Error)) {
			return msg;
		}
	}
}

BusConnection::Wait BusConnection::readLine(std::string& line, int wakeFd, int timeoutMs) {
	for (;;) {
		for (size_t i = 0; i + 1 < in_.size(); ++i) {
			if (in_[i] == '\r' && in_[i + 1] == '\n') {
				line.assign(in_.begin(), in_.begin() + static_cast<std::ptrdiff_t>(i));
				in_.erase(in_.begin(), in_.begin() + static_cast<std::ptrdiff_t>(i + 2));
				return Wait::Got;
			}
		}
		if (in_.size() > 4096) {
			return Wait::Closed;
		}
		if (const Wait w = fill(wakeFd, timeoutMs); w != Wait::Got) {
			return w;
		}
	}
}

BusConnection::Wait BusConnection::fill(int wakeFd, int timeoutMs) {
	pollfd fds[2] = { { fd_, POLLIN, 0 }, { wakeFd, POLLIN, 0 } };
	const int n = ::poll(fds, wakeFd >= 0 ? 2 : 1, timeoutMs);
	if (n < 0) {
		return errno == EINTR ? Wait::Got : Wait::Closed;
	}
	if (n == 0) {
		return Wait::TimedOut;
	}
	if (wakeFd >= 0 && (fds[1].revents & POLLIN) != 0) {
		return Wait::Cancelled;
	}
	uint8_t buf[4096];
	const ssize_t got = ::recv(fd_, buf, sizeof buf, 0);
	if (got < 0 && (errno == EINTR || errno == EAGAIN)) {
		return Wait::Got;
	}
	if (got <= 0) {
		return Wait::Closed;
	}
	in_.insert(in_.end(), buf, buf + got);
	return Wait::Got;
}

std::vector<BusAddress> sessionBusAddresses() {
	if (const char* env = std::getenv("DBUS_SESSION_BUS_ADDRESS"); env != nullptr && *env != '\0') {
		return parseBusAddress(env);
	}
	// NO ADDRESS IN THE ENVIRONMENT — systemd's per-user bus is at a fixed place when there is one.
	if (const char* runtime = std::getenv("XDG_RUNTIME_DIR"); runtime != nullptr && *runtime != '\0') {
		return { { false, std::string(runtime) + "/bus" } };
	}
	return {};
}

bool portalReachable(int timeoutMs) {
	BusConnection bus;
	bool connected = false;
	for (const BusAddress& address : sessionBusAddresses()) {
		if (bus.connect(address)) {
			connected = true;
			break;
		}
	}
	if (!connected || !bus.authenticate(-1, timeoutMs) || bus.hello(-1, timeoutMs).empty()) {
		return false;
	}
	if (const auto owned = bus.call(nameHasOwner(kPortalName, 0), -1, timeoutMs); owned.has_value()) {
		if (parseBool(*owned).value_or(false)) {
			return true;
		}
	}
	if (const auto names = bus.call(listActivatableNames(0), -1, timeoutMs); names.has_value()) {
		for (const std::string& name : parseStringArray(*names).value_or(std::vector<std::string> {})) {
			if (name == kPortalName) {
				return true;
			}
		}
	}
	return false;
}

} // namespace feathers::ui::portal

namespace feathers::ui {

namespace {

using Status = FileDialogResult::Status;
using portal::BusConnection;

FileDialogResult failed(std::string why) {
	FileDialogResult r;
	r.status = Status::Failed;
	r.error = std::move(why);
	return r;
}

/// **THE WORKER'S CONTROL BLOCK** — shared by the session (which cancels) and the worker (which waits).
/// A byte in the pipe is the cancel; it is never read, so every later wait sees it too.
struct Control {
	int pipe[2] = { -1, -1 };
	std::atomic<bool> finished { false };
	Control() {
		if (::pipe2(pipe, O_CLOEXEC | O_NONBLOCK) != 0) {
			pipe[0] = pipe[1] = -1;
		}
	}
	~Control() {
		for (const int fd : pipe) {
			if (fd >= 0) {
				::close(fd);
			}
		}
	}
	Control(const Control&) = delete;
	Control& operator=(const Control&) = delete;
	void cancel() const {
		const char one = 1;
		(void)!::write(pipe[1], &one, 1);
	}
	[[nodiscard]] int wakeFd() const { return pipe[0]; }
};

/// How long any step BEFORE the dialog is up may take. The dialog itself waits as long as the person does.
constexpr int kStepTimeoutMs = 10000;

std::string errorText(const dbus::Message& error) {
	std::string text = error.errorName;
	if (error.signature.rfind('s', 0) == 0) {
		dbus::Reader r(error);
		const std::string detail = r.string();
		if (r.ok() && !detail.empty()) {
			text += ": " + detail;
		}
	}
	return text;
}

/// **THE PORTAL.** False: there is no portal to ask (no bus, no service, no FileChooser), and the
/// fallbacks should try. True: the channel has its answer, or the request was cancelled.
bool runPortal(const FileDialogOptions& options, FileDialogChannel& channel, const Control& control) {
	BusConnection bus;
	bool connected = false;
	for (const portal::BusAddress& address : portal::sessionBusAddresses()) {
		if (bus.connect(address)) {
			connected = true;
			break;
		}
	}
	if (!connected || !bus.authenticate(control.wakeFd(), kStepTimeoutMs)) {
		return false;
	}
	const std::string unique = bus.hello(control.wakeFd(), kStepTimeoutMs);
	if (unique.empty()) {
		return false;
	}

	static std::atomic<uint32_t> tokens { 0 };
	const std::string token = "feathers" + std::to_string(++tokens);
	std::string handle = portal::requestPath(unique, token);
	(void)bus.send(portal::addMatch(portal::responseRule(handle), 0));
	const uint32_t callSerial =
	    bus.send(portal::fileChooserCall(options, portal::parentHandle(options.parent), token, 0));
	if (callSerial == 0) {
		return false;
	}

	dbus::Message msg;
	bool replied = false;
	for (;;) {
		switch (bus.next(msg, control.wakeFd(), replied ? -1 : kStepTimeoutMs)) {
		case BusConnection::Wait::Got: break;
		case BusConnection::Wait::Cancelled:
			(void)bus.send(portal::closeRequest(handle, 0));
			channel.complete({ Status::Cancelled, {}, {} });
			return true;
		case BusConnection::Wait::TimedOut:
			channel.complete(failed("The desktop portal did not answer"));
			return true;
		case BusConnection::Wait::Closed:
			channel.complete(failed("The session bus closed the connection"));
			return true;
		}
		if (msg.type == dbus::Type::Error && msg.replySerial == callSerial) {
			if (portal::isNoPortalError(msg.errorName)) {
				return false;
			}
			channel.complete(failed(errorText(msg)));
			return true;
		}
		if (msg.type == dbus::Type::MethodReturn && msg.replySerial == callSerial) {
			replied = true;
			// A PORTAL OLDER THAN handle_token names its own request path; listen there too.
			if (const auto path = portal::parseObjectPath(msg); path.has_value() && *path != handle) {
				handle = *path;
				(void)bus.send(portal::addMatch(portal::responseRule(handle), 0));
			}
			continue;
		}
		portal::Response response;
		if (msg.path != handle || !portal::parseResponse(msg, response)) {
			continue;
		}
		if (response.code == 1) {
			channel.complete({ Status::Cancelled, {}, {} });
		} else if (response.code != 0 || response.uris.empty()) {
			channel.complete(failed("The file dialog closed without an answer"));
		} else if (const auto path = portal::pathFromFileUri(response.uris.front()); path.has_value()) {
			channel.complete({ Status::Chosen, *path, {} });
		} else {
			channel.complete(failed("Not a local file: " + response.uris.front()));
		}
		return true;
	}
}

//------------------------------------------------------------------------------------------------//
// THE FALLBACKS                                                                                    //
//------------------------------------------------------------------------------------------------//

/// Run `tool` with `args` — directly, no shell — and read its answer. False: it is not installed.
bool runTool(const char* tool, const std::vector<std::string>& args, FileDialogChannel& channel,
             const Control& control) {
	int out[2];
	if (::pipe2(out, O_CLOEXEC) != 0) {
		channel.complete(failed("Could not start a file dialog"));
		return true;
	}
	std::vector<char*> argv;
	argv.push_back(const_cast<char*>(tool));
	for (const std::string& a : args) {
		argv.push_back(const_cast<char*>(a.c_str()));
	}
	argv.push_back(nullptr);

	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO);
	// GTK and Qt both talk on stderr; it is not ours to show and must not block on a full pipe.
	posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
	pid_t pid = 0;
	const int spawned = posix_spawnp(&pid, tool, &actions, nullptr, argv.data(), environ);
	posix_spawn_file_actions_destroy(&actions);
	::close(out[1]);
	if (spawned != 0) {
		::close(out[0]);
		return false;
	}

	std::string text;
	bool cancelled = false;
	for (;;) {
		pollfd fds[2] = { { out[0], POLLIN, 0 }, { control.wakeFd(), POLLIN, 0 } };
		if (::poll(fds, 2, -1) < 0) {
			if (errno == EINTR) {
				continue;
			}
			break;
		}
		if ((fds[1].revents & POLLIN) != 0) {
			// OUR OWN CHILD, BY PID — never a process looked up by name.
			(void)::kill(pid, SIGTERM);
			cancelled = true;
			break;
		}
		char buf[1024];
		const ssize_t got = ::read(out[0], buf, sizeof buf);
		if (got < 0 && errno == EINTR) {
			continue;
		}
		if (got <= 0) {
			break;
		}
		text.append(buf, static_cast<size_t>(got));
	}
	::close(out[0]);
	int status = 0;
	while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
	}
	if (cancelled) {
		channel.complete({ Status::Cancelled, {}, {} });
		return true;
	}
	// A libc that reports a missing program from the CHILD rather than from posix_spawnp: status 127.
	if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
		return false;
	}
	channel.complete(portal::fallbackResult(text, WIFEXITED(status) ? WEXITSTATUS(status) : -1));
	return true;
}

void runDialog(const FileDialogOptions& options, FileDialogChannel& channel, const Control& control) {
	if (runPortal(options, channel, control) || channel.abandoned()) {
		return;
	}
	if (runTool("zenity", portal::zenityArgs(options), channel, control)) {
		return;
	}
	if (runTool("kdialog", portal::kdialogArgs(options), channel, control)) {
		return;
	}
	channel.complete(failed("No file dialog is available: no desktop portal, zenity or kdialog"));
}

//------------------------------------------------------------------------------------------------//
// THE BACKEND                                                                                      //
//------------------------------------------------------------------------------------------------//

class LinuxSession final : public FileDialogBackend::Session {
public:
	explicit LinuxSession(std::shared_ptr<Control> control) : control_(std::move(control)) {}
	void cancel() override { control_->cancel(); }

private:
	std::shared_ptr<Control> control_;
};

/// `program` is an executable somewhere on PATH — what `posix_spawnp` will find.
bool onPath(const char* program) {
	const char* path = std::getenv("PATH");
	std::string_view dirs = path != nullptr ? path : "/usr/bin:/bin";
	while (!dirs.empty()) {
		const size_t colon = dirs.find(':');
		const std::string_view dir = dirs.substr(0, colon);
		dirs = colon == std::string_view::npos ? std::string_view {} : dirs.substr(colon + 1);
		if (!dir.empty() && ::access((std::string(dir) + "/" + program).c_str(), X_OK) == 0) {
			return true;
		}
	}
	return false;
}

class LinuxBackend final : public FileDialogBackend {
public:
	/// **A PORTAL, OR ZENITY, OR KDIALOG.** Asked on the message thread, so the bus is asked with a short
	/// bound on each step, and the answer is kept for a few seconds: a list that opens asks every time.
	bool available() override {
		const auto now = std::chrono::steady_clock::now();
		if (checked_ && now - checkedAt_ < kRecheck) {
			return available_;
		}
		available_ = onPath("zenity") || onPath("kdialog") || portal::portalReachable(kProbeTimeoutMs);
		checked_ = true;
		checkedAt_ = now;
		return available_;
	}

	~LinuxBackend() override {
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
		reap();
		auto control = std::make_shared<Control>();
		if (control->wakeFd() < 0) {
			channel->complete(failed("Could not start a file dialog"));
			return nullptr;
		}
		Worker& worker = workers_.emplace_back();
		worker.control = control;
		worker.thread = std::thread([options, channel, control] {
			runDialog(options, *channel, *control);
			control->finished.store(true, std::memory_order_release);
		});
		return std::make_unique<LinuxSession>(std::move(control));
	}

private:
	struct Worker {
		std::shared_ptr<Control> control;
		std::thread thread;
	};

	void reap() {
		for (auto it = workers_.begin(); it != workers_.end();) {
			if (it->control->finished.load(std::memory_order_acquire)) {
				it->thread.join();
				it = workers_.erase(it);
			} else {
				++it;
			}
		}
	}

	static constexpr int kProbeTimeoutMs = 250;
	static constexpr std::chrono::seconds kRecheck { 5 };

	std::list<Worker> workers_;
	bool checked_ = false;
	bool available_ = false;
	std::chrono::steady_clock::time_point checkedAt_;
};

} // namespace

FileDialogBackend& platformFileDialogBackend() {
	static LinuxBackend backend;
	return backend;
}

} // namespace feathers::ui

#elif !defined(_WIN32) && !defined(__APPLE__)

namespace feathers::ui {

namespace {

/// A platform with no dialog of ours: every request fails, and says why.
class NoBackend final : public FileDialogBackend {
public:
	std::unique_ptr<Session> start(const FileDialogOptions&, std::shared_ptr<FileDialogChannel> channel) override {
		channel->complete({ FileDialogResult::Status::Failed, {}, "No file dialog on this platform" });
		return nullptr;
	}
};

} // namespace

FileDialogBackend& platformFileDialogBackend() {
	static NoBackend backend;
	return backend;
}

} // namespace feathers::ui

#endif
