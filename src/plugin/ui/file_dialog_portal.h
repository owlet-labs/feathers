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

#include "plugin/ui/dbus_wire.h"
#include "plugin/ui/file_dialog.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// THE LINUX DIALOG'S PURE HALF — every message it sends and every answer it reads, as functions of
// their inputs, so the whole conversation is testable with no bus (the container has none) and on
// every platform. `file_dialog_linux.cpp` is the socket, the thread and the fallbacks around it.
//
// THE CONVERSATION (xdg-desktop-portal, "org.freedesktop.portal.FileChooser" and "…Request"):
//   1. connect to $DBUS_SESSION_BUS_ADDRESS; "\0AUTH EXTERNAL <hex uid>\r\n", "OK <guid>", "BEGIN\r\n"
//   2. org.freedesktop.DBus.Hello → our unique name ":1.42"
//   3. AddMatch for the Response signal on the REQUEST PATH the portal will use —
//      /org/freedesktop/portal/desktop/request/1_42/<handle_token> — subscribed BEFORE the call, which is
//      why the token is ours to choose: an answer faster than the call's own reply is not missed
//   4. FileChooser.OpenFile / SaveFile (parent_window, title, options a{sv}) → the request's path
//   5. the Request's Response(u code, a{sv} results): 0 chosen (results["uris"]), 1 cancelled, 2 ended
//   Cancelling from our side is Request.Close on that path.
//
// Under Flatpak a chosen file comes back as a document-portal path, /run/user/<uid>/doc/<id>/<name>.
// It is readable, which is all a caller needs.

namespace feathers::ui::portal {

constexpr std::string_view kBusName = "org.freedesktop.DBus";
constexpr std::string_view kBusPath = "/org/freedesktop/DBus";
constexpr std::string_view kPortalName = "org.freedesktop.portal.Desktop";
constexpr std::string_view kPortalPath = "/org/freedesktop/portal/desktop";
constexpr std::string_view kChooserInterface = "org.freedesktop.portal.FileChooser";
constexpr std::string_view kRequestInterface = "org.freedesktop.portal.Request";

/// One `unix:` entry of a bus address. Other transports are not ours to speak.
struct BusAddress {
	bool abstract = false; ///< a Linux abstract-namespace socket rather than a file
	std::string path;
};
/// "unix:path=/run/user/1000/bus,guid=…;unix:abstract=…" → the unix entries, in order, percent-decoded.
std::vector<BusAddress> parseBusAddress(std::string_view address);

/// The first line of the SASL exchange: "\0AUTH EXTERNAL <uid as hex-encoded ASCII digits>\r\n".
std::string authExternal(uint32_t uid);

dbus::Message hello(uint32_t serial);
/// The bus's own "is `name` owned now?" (reply "b") and "which names can it start?" (reply "as") — the
/// two halves of "is there a portal": xdg-desktop-portal is usually started ON DEMAND, so a portal that
/// nobody has asked for yet is not owned but is activatable.
dbus::Message nameHasOwner(std::string_view name, uint32_t serial);
dbus::Message listActivatableNames(uint32_t serial);
std::optional<bool> parseBool(const dbus::Message& reply);
std::optional<std::vector<std::string>> parseStringArray(const dbus::Message& reply);
dbus::Message addMatch(std::string_view rule, uint32_t serial);
/// The match rule for the Response signal on `requestPath`.
std::string responseRule(std::string_view requestPath);
/// ":1.42" and "feathers7" → "/org/freedesktop/portal/desktop/request/1_42/feathers7".
std::string requestPath(std::string_view uniqueName, std::string_view token);
/// X11 window id → "x11:<hex>"; null → "" (no parent).
std::string parentHandle(void* x11Window);

/// OpenFile (a file or, with `directory`, a folder) or SaveFile, from the options.
dbus::Message fileChooserCall(const FileDialogOptions& options, std::string_view parent, std::string_view token,
                              uint32_t serial);
dbus::Message closeRequest(std::string_view requestPath, uint32_t serial);

/// A filter's extensions as the portal's case-insensitive globs: "wav" → "*.[wW][aA][vV]". GTK matches
/// globs by case, and a folder of `.WAV` files from a recorder must not look empty.
std::string caseInsensitiveGlob(std::string_view extension);

struct Response {
	uint32_t code = 2;              ///< 0 chosen, 1 cancelled, 2 ended some other way
	std::vector<std::string> uris; ///< results["uris"], when there
};
/// The Response signal's body ("ua{sv}"). False for any other shape.
bool parseResponse(const dbus::Message& signal, Response& out);
/// The object path in a method return ("o"): the request the portal actually made.
std::optional<std::string> parseObjectPath(const dbus::Message& reply);
/// The unique name in Hello's reply ("s").
std::optional<std::string> parseString(const dbus::Message& reply);

/// "file:///a/b%20c.wav" (or "file://localhost/…") → "/a/b c.wav". Anything else → nothing.
std::optional<std::filesystem::path> pathFromFileUri(std::string_view uri);

/// **AN ERROR THAT MEANS "NO PORTAL HERE"** rather than "the portal refused": the name is not on the
/// bus, or it is and has no FileChooser. These fall back to zenity/kdialog; any other error is final.
bool isNoPortalError(std::string_view errorName);

/// The argv (without the program) for the two fallbacks.
std::vector<std::string> zenityArgs(const FileDialogOptions& options);
std::vector<std::string> kdialogArgs(const FileDialogOptions& options);
/// A fallback's stdout and exit status → the answer: 0 and a line is Chosen, 1 is Cancelled.
FileDialogResult fallbackResult(const std::string& stdoutText, int exitStatus);

} // namespace feathers::ui::portal
