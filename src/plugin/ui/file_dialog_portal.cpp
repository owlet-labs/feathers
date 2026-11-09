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

#include "plugin/ui/file_dialog_portal.h"

#include <cctype>
#include <cstdio>

namespace feathers::ui::portal {

namespace {

int hexValue(char c) {
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	return -1;
}

/// "%XX" escapes decoded; a broken escape → nothing.
std::optional<std::string> percentDecode(std::string_view s) {
	std::string out;
	out.reserve(s.size());
	for (size_t i = 0; i < s.size(); ++i) {
		if (s[i] != '%') {
			out.push_back(s[i]);
			continue;
		}
		if (i + 2 >= s.size()) {
			return std::nullopt;
		}
		const int hi = hexValue(s[i + 1]);
		const int lo = hexValue(s[i + 2]);
		if (hi < 0 || lo < 0) {
			return std::nullopt;
		}
		out.push_back(static_cast<char>(hi * 16 + lo));
		i += 2;
	}
	return out;
}

dbus::Message busCall(std::string_view member, uint32_t serial) {
	dbus::Message m;
	m.type = dbus::Type::MethodCall;
	m.serial = serial;
	m.path = kBusPath;
	m.interface = kBusName;
	m.member = member;
	m.destination = kBusName;
	return m;
}

/// One `{sv}` entry of an options dictionary, its value written by `value`.
template <typename Fn>
void option(dbus::Writer& w, std::string_view key, std::string_view signature, Fn&& value) {
	w.beginStruct();
	w.string(key);
	w.variantSignature(signature);
	value();
}

} // namespace

std::vector<BusAddress> parseBusAddress(std::string_view address) {
	std::vector<BusAddress> out;
	while (!address.empty()) {
		const size_t semi = address.find(';');
		const std::string_view entry = address.substr(0, semi);
		address = semi == std::string_view::npos ? std::string_view {} : address.substr(semi + 1);
		if (entry.substr(0, 5) != "unix:") {
			continue;
		}
		std::string_view keys = entry.substr(5);
		while (!keys.empty()) {
			const size_t comma = keys.find(',');
			const std::string_view pair = keys.substr(0, comma);
			keys = comma == std::string_view::npos ? std::string_view {} : keys.substr(comma + 1);
			const size_t eq = pair.find('=');
			if (eq == std::string_view::npos) {
				continue;
			}
			const std::string_view key = pair.substr(0, eq);
			if (key != "path" && key != "abstract") {
				continue;
			}
			if (const auto value = percentDecode(pair.substr(eq + 1)); value.has_value() && !value->empty()) {
				out.push_back({ key == "abstract", *value });
			}
		}
	}
	return out;
}

std::string authExternal(uint32_t uid) {
	static constexpr char kHex[] = "0123456789abcdef";
	const std::string digits = std::to_string(uid);
	std::string out(1, '\0');
	out += "AUTH EXTERNAL ";
	for (const char c : digits) {
		out.push_back(kHex[(static_cast<unsigned char>(c) >> 4) & 0xf]);
		out.push_back(kHex[static_cast<unsigned char>(c) & 0xf]);
	}
	out += "\r\n";
	return out;
}

dbus::Message hello(uint32_t serial) { return busCall("Hello", serial); }

dbus::Message nameHasOwner(std::string_view name, uint32_t serial) {
	dbus::Message m = busCall("NameHasOwner", serial);
	m.signature = "s";
	dbus::Writer w;
	w.string(name);
	m.body = w.take();
	return m;
}

dbus::Message listActivatableNames(uint32_t serial) { return busCall("ListActivatableNames", serial); }

std::optional<bool> parseBool(const dbus::Message& reply) {
	if (reply.type != dbus::Type::MethodReturn || reply.signature != "b") {
		return std::nullopt;
	}
	dbus::Reader r(reply);
	const bool value = r.boolean();
	return r.ok() ? std::optional<bool>(value) : std::nullopt;
}

std::optional<std::vector<std::string>> parseStringArray(const dbus::Message& reply) {
	if (reply.type != dbus::Type::MethodReturn || reply.signature != "as") {
		return std::nullopt;
	}
	dbus::Reader r(reply);
	std::vector<std::string> out;
	const size_t end = r.beginArray(4);
	while (r.ok() && r.pos() < end) {
		out.push_back(r.string());
	}
	return r.ok() ? std::optional<std::vector<std::string>>(std::move(out)) : std::nullopt;
}

dbus::Message addMatch(std::string_view rule, uint32_t serial) {
	dbus::Message m = busCall("AddMatch", serial);
	m.signature = "s";
	dbus::Writer w;
	w.string(rule);
	m.body = w.take();
	return m;
}

std::string responseRule(std::string_view requestPath) {
	std::string rule = "type='signal',sender='";
	rule += kPortalName;
	rule += "',path='";
	rule += requestPath;
	rule += "',interface='";
	rule += kRequestInterface;
	rule += "',member='Response'";
	return rule;
}

std::string requestPath(std::string_view uniqueName, std::string_view token) {
	std::string sender(uniqueName.substr(uniqueName.empty() || uniqueName[0] != ':' ? 0 : 1));
	for (char& c : sender) {
		if (c == '.') {
			c = '_';
		}
	}
	std::string out(kPortalPath);
	out += "/request/";
	out += sender;
	out += '/';
	out += token;
	return out;
}

std::string parentHandle(void* x11Window) {
	if (x11Window == nullptr) {
		return {};
	}
	char buf[32];
	(void)std::snprintf(buf, sizeof buf, "x11:%llx",
	                    static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(x11Window)));
	return buf;
}

std::string caseInsensitiveGlob(std::string_view extension) {
	std::string out = "*.";
	for (const char c : extension) {
		const auto u = static_cast<unsigned char>(c);
		if (std::isalpha(u) != 0) {
			out += '[';
			out.push_back(static_cast<char>(std::tolower(u)));
			out.push_back(static_cast<char>(std::toupper(u)));
			out += ']';
		} else {
			out.push_back(c);
		}
	}
	return out;
}

dbus::Message fileChooserCall(const FileDialogOptions& options, std::string_view parent, std::string_view token,
                              uint32_t serial) {
	using Mode = FileDialogOptions::Mode;
	dbus::Message m;
	m.type = dbus::Type::MethodCall;
	m.serial = serial;
	m.path = kPortalPath;
	m.interface = kChooserInterface;
	m.member = options.mode == Mode::SaveFile ? "SaveFile" : "OpenFile";
	m.destination = kPortalName;
	m.signature = "ssa{sv}";

	dbus::Writer w;
	w.string(parent);
	w.string(options.title);
	const auto dict = w.beginArray(8);
	option(w, "handle_token", "s", [&] { w.string(token); });
	if (options.mode == Mode::OpenFolder) {
		option(w, "directory", "b", [&] { w.boolean(true); });
	}
	if (options.mode != Mode::OpenFolder && !options.filters.empty()) {
		option(w, "filters", "a(sa(us))", [&] {
			const auto filters = w.beginArray(8);
			for (const FileFilter& filter : options.filters) {
				w.beginStruct();
				w.string(filter.label);
				const auto globs = w.beginArray(8);
				for (const std::string& ext : filter.extensions) {
					w.beginStruct();
					w.u32(0); // 0: a glob; 1 would be a MIME type
					w.string(caseInsensitiveGlob(ext));
				}
				w.endArray(globs);
			}
			w.endArray(filters);
		});
	}
	if (options.mode == Mode::SaveFile && !options.defaultName.empty()) {
		option(w, "current_name", "s", [&] { w.string(options.defaultName); });
	}
	if (!options.startDir.empty()) {
		// A BYTE ARRAY, NUL INCLUDED — the portal's own type for a path, since a path need not be UTF-8.
		option(w, "current_folder", "ay", [&] {
			const auto bytes = w.beginArray(1);
			for (const char c : options.startDir.string()) {
				w.byte(static_cast<uint8_t>(c));
			}
			w.byte(0);
			w.endArray(bytes);
		});
	}
	w.endArray(dict);
	m.body = w.take();
	return m;
}

dbus::Message closeRequest(std::string_view requestPath, uint32_t serial) {
	dbus::Message m;
	m.type = dbus::Type::MethodCall;
	m.flags = dbus::kNoReplyExpected;
	m.serial = serial;
	m.path = requestPath;
	m.interface = kRequestInterface;
	m.member = "Close";
	m.destination = kPortalName;
	return m;
}

bool parseResponse(const dbus::Message& signal, Response& out) {
	if (signal.type != dbus::Type::Signal || signal.interface != kRequestInterface || signal.member != "Response" ||
	    signal.signature != "ua{sv}") {
		return false;
	}
	out = Response {};
	dbus::Reader r(signal);
	out.code = r.u32();
	const size_t end = r.beginArray(8);
	while (r.ok() && r.pos() < end) {
		r.align(8);
		const std::string key = r.string();
		const std::string type = r.signature();
		if (key == "uris" && type == "as") {
			const size_t urisEnd = r.beginArray(4);
			while (r.ok() && r.pos() < urisEnd) {
				out.uris.push_back(r.string());
			}
		} else {
			std::string_view sig = type;
			if (!r.skip(sig) || !sig.empty()) {
				return false;
			}
		}
	}
	return r.ok();
}

std::optional<std::string> parseObjectPath(const dbus::Message& reply) {
	if (reply.type != dbus::Type::MethodReturn || reply.signature != "o") {
		return std::nullopt;
	}
	dbus::Reader r(reply);
	std::string path = r.string();
	return r.ok() ? std::optional<std::string>(std::move(path)) : std::nullopt;
}

std::optional<std::string> parseString(const dbus::Message& reply) {
	if (reply.type != dbus::Type::MethodReturn || reply.signature != "s") {
		return std::nullopt;
	}
	dbus::Reader r(reply);
	std::string value = r.string();
	return r.ok() ? std::optional<std::string>(std::move(value)) : std::nullopt;
}

std::optional<std::filesystem::path> pathFromFileUri(std::string_view uri) {
	constexpr std::string_view kScheme = "file://";
	if (uri.substr(0, kScheme.size()) != kScheme) {
		return std::nullopt;
	}
	uri.remove_prefix(kScheme.size());
	if (uri.substr(0, 9) == "localhost") {
		uri.remove_prefix(9);
	}
	if (uri.empty() || uri[0] != '/') {
		return std::nullopt; // another host's file is not a file we can open
	}
	const auto decoded = percentDecode(uri);
	if (!decoded.has_value()) {
		return std::nullopt;
	}
	return std::filesystem::path(*decoded);
}

bool isNoPortalError(std::string_view errorName) {
	return errorName == "org.freedesktop.DBus.Error.ServiceUnknown" ||
	       errorName == "org.freedesktop.DBus.Error.NameHasNoOwner" ||
	       errorName == "org.freedesktop.DBus.Error.UnknownMethod" ||
	       errorName == "org.freedesktop.DBus.Error.UnknownInterface" ||
	       errorName == "org.freedesktop.DBus.Error.UnknownObject";
}

std::vector<std::string> zenityArgs(const FileDialogOptions& options) {
	using Mode = FileDialogOptions::Mode;
	std::vector<std::string> args { "--file-selection" };
	if (!options.title.empty()) {
		args.push_back("--title=" + options.title);
	}
	if (options.mode == Mode::OpenFolder) {
		args.emplace_back("--directory");
	}
	if (options.mode == Mode::SaveFile) {
		args.emplace_back("--save");
		args.emplace_back("--confirm-overwrite");
	}
	std::string start;
	if (!options.startDir.empty()) {
		start = options.startDir.string();
		if (start.back() != '/') {
			start += '/'; // a trailing slash is what makes zenity OPEN the folder rather than select it
		}
	}
	if (options.mode == Mode::SaveFile) {
		start += options.defaultName;
	}
	if (!start.empty()) {
		args.push_back("--filename=" + start);
	}
	if (options.mode != Mode::OpenFolder) {
		for (const FileFilter& filter : options.filters) {
			std::string arg = "--file-filter=" + filter.label + " |";
			for (const std::string& ext : filter.extensions) {
				arg += ' ' + caseInsensitiveGlob(ext);
			}
			args.push_back(std::move(arg));
		}
	}
	return args;
}

std::vector<std::string> kdialogArgs(const FileDialogOptions& options) {
	using Mode = FileDialogOptions::Mode;
	std::vector<std::string> args;
	if (!options.title.empty()) {
		args.emplace_back("--title");
		args.push_back(options.title);
	}
	std::string start = options.startDir.empty() ? std::string(".") : options.startDir.string();
	switch (options.mode) {
	case Mode::OpenFile: args.emplace_back("--getopenfilename"); break;
	case Mode::OpenFolder: args.emplace_back("--getexistingdirectory"); break;
	case Mode::SaveFile:
		args.emplace_back("--getsavefilename");
		if (!options.defaultName.empty()) {
			start = (std::filesystem::path(start) / options.defaultName).generic_string(); // kdialog is POSIX: always "/"
		}
		break;
	}
	args.push_back(start);
	if (options.mode != Mode::OpenFolder && !options.filters.empty()) {
		// "Label (*.a *.b)" per filter, newline-separated: KDE's own filter syntax.
		std::string spec;
		for (const FileFilter& filter : options.filters) {
			if (!spec.empty()) {
				spec += '\n';
			}
			spec += filter.label + " (";
			bool first = true;
			for (const std::string& ext : filter.extensions) {
				spec += (first ? "*." : " *.") + ext;
				first = false;
			}
			spec += ')';
		}
		args.push_back(std::move(spec));
	}
	return args;
}

FileDialogResult fallbackResult(const std::string& stdoutText, int exitStatus) {
	FileDialogResult result;
	if (exitStatus == 1) {
		result.status = FileDialogResult::Status::Cancelled;
		return result;
	}
	std::string line = stdoutText.substr(0, stdoutText.find('\n'));
	if (exitStatus != 0 || line.empty()) {
		result.status = FileDialogResult::Status::Failed;
		result.error = "The file dialog exited with status " + std::to_string(exitStatus);
		return result;
	}
	result.status = FileDialogResult::Status::Chosen;
	result.path = std::move(line);
	return result;
}

} // namespace feathers::ui::portal
