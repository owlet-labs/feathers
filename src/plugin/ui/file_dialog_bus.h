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

#if defined(__linux__)

#include "plugin/ui/dbus_wire.h"
#include "plugin/ui/file_dialog_portal.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// ONE CONNECTION TO A SESSION BUS, BLOCKING, FOR ONE WORKER THREAD — the Linux file dialog's socket.
// In a header of its own only so the live-bus case can stand a fake portal up with the same code; the
// dialog is its one real user. Every wait also watches `wakeFd`, which is how a cancel gets in.

namespace feathers::ui::portal {

class BusConnection {
public:
	enum class Wait { Got, Cancelled, Closed, TimedOut };

	BusConnection() = default;
	~BusConnection();
	BusConnection(const BusConnection&) = delete;
	BusConnection& operator=(const BusConnection&) = delete;

	bool connect(const BusAddress& address);
	/// SASL EXTERNAL then BEGIN: the kernel vouches for our uid.
	bool authenticate(int wakeFd, int timeoutMs);
	/// Stamps the next serial on `message` and writes it. 0: the write failed.
	uint32_t send(dbus::Message message);
	/// The next whole message, or why there is none. `timeoutMs` -1 waits for ever (for the wake fd).
	Wait next(dbus::Message& out, int wakeFd, int timeoutMs);
	/// Hello, and the unique name it returns; empty on any failure.
	std::string hello(int wakeFd, int timeoutMs);
	/// Send `message` and wait for ITS reply (a return or an error); nothing on a timeout or a close.
	std::optional<dbus::Message> call(dbus::Message message, int wakeFd, int timeoutMs);

private:
	bool write(const void* data, size_t size);
	Wait readLine(std::string& line, int wakeFd, int timeoutMs);
	Wait fill(int wakeFd, int timeoutMs);

	int fd_ = -1;
	uint32_t serial_ = 0;
	std::vector<uint8_t> in_;
};

/// `$DBUS_SESSION_BUS_ADDRESS`, else systemd's `$XDG_RUNTIME_DIR/bus`.
std::vector<BusAddress> sessionBusAddresses();

/// **IS THERE A FILE CHOOSER PORTAL ON THE SESSION BUS** — owned now, or startable on demand. Blocking,
/// each step bounded by `timeoutMs`; false for no bus, no portal, or a bus that does not answer.
bool portalReachable(int timeoutMs);

} // namespace feathers::ui::portal

#endif
