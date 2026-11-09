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

// THE OS FILE DIALOG (`file_dialog.h`) — three layers, three kinds of evidence:
//
//   1. THE CONTRACT, with a fake backend: the answer arrives on the message thread from `poll()`,
//      never inside `open()` and never on the thread that produced it; cancel drops it; a `FileDialog`
//      destroyed while open is never called back, whatever its worker does afterwards.
//   2. THE WIRE, against GOLDEN BYTES FROM OTHER ENCODERS — never from ours (CLAUDE.md: fixtures take
//      values from the platform's contract). Captured 2026-09-25 with `dbus-monitor --binary` on a
//      private `dbus-daemon`: the method call and the Response signals were encoded by GLib (`gdbus
//      call` / `gdbus emit`), the Hello reply and the ServiceUnknown error by the daemon itself
//      (libdbus). Two independent encoders; our decoder must read both, and our encoder must produce
//      GLib's body byte for byte.
//   3. A LIVE BUS, Linux only and only where `dbus-daemon` is installed: a private daemon, and a fake
//      portal standing on it that owns org.freedesktop.portal.Desktop. The daemon validates every
//      message it routes, so this is the one place our header encoding meets a real parser. PATH is
//      emptied for those cases, so no zenity or kdialog can ever open on somebody's desktop.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "plugin/ui/dbus_wire.h"
#include "plugin/ui/file_dialog.h"
#include "plugin/ui/file_dialog_portal.h"

#if defined(__linux__)
#include "plugin/ui/file_dialog_bus.h"

#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace feathers::ui;
using Mode = FileDialogOptions::Mode;
using Status = FileDialogResult::Status;

namespace {

std::vector<uint8_t> fromHex(std::string_view hex) {
	std::vector<uint8_t> out;
	for (size_t i = 0; i + 1 < hex.size(); i += 2) {
		out.push_back(static_cast<uint8_t>(std::stoi(std::string(hex.substr(i, 2)), nullptr, 16)));
	}
	return out;
}

//------------------------------------------------------------------------------------------------//
// GOLDEN BYTES — see the header for where each came from                                           //
//------------------------------------------------------------------------------------------------//

/// GLib: `gdbus call … org.freedesktop.portal.FileChooser.OpenFile "x11:3a0000f" "Load IR"
/// "{'handle_token': <'feathers1'>, 'filters': <[('Audio', [(uint32 0, '*.[wW][aA][vV]'),
/// (uint32 0, '*.[fF][lL][aA][cC]')])]>, 'current_folder': <b'/home/user/IRs'>}"`, serial 3, as the
/// daemon forwarded it (it appends SENDER ":1.1").
const std::string_view kGlibOpenFile =
    "6c010001db00000003000000b500000001016f001f0000002f6f72672f66"
    "7265656465736b746f702f706f7274616c2f6465736b746f700002017300"
    "220000006f72672e667265656465736b746f702e706f7274616c2e46696c"
    "6543686f6f736572000000000000060173001e0000006f72672e66726565"
    "6465736b746f702e706f7274616c2e4465736b746f700000080167000773"
    "73617b73767d0000000003017300080000004f70656e46696c6500000000"
    "0000000007017300040000003a312e31000000000b0000007831313a3361"
    "303030306600070000004c6f616420495200bb0000000c00000068616e64"
    "6c655f746f6b656e00017300090000006665617468657273310000000000"
    "00000700000066696c746572730009612873612875732929000043000000"
    "0000000005000000417564696f00000033000000000000000e0000002a2e"
    "5b77575d5b61415d5b76565d000000000000120000002a2e5b66465d5b6c"
    "4c5d5b61415d5b63435d0000000000000e00000063757272656e745f666f"
    "6c6465720002617900000f0000002f686f6d652f757365722f49527300";
/// Where that message's body starts: 16 + the 0xb5 bytes of fields, padded to 8.
constexpr size_t kGlibOpenFileBodyAt = 200;

/// GLib: `gdbus emit … Request.Response "uint32 0" "{'uris': <['file:///home/user/IRs/Big%20Hall.wav']>,
/// 'choices': <@a(ss) []>, 'current_filter': <('Audio', [(uint32 0, '*.wav')])>}"` — the uris BETWEEN
/// two entries of other shapes, so the reader must step over an empty array and a nested struct.
const std::string_view kGlibResponseChosen =
    "6c0401019e000000020000009d00000001016f00360000002f6f72672f66"
    "7265656465736b746f702f706f7274616c2f6465736b746f702f72657175"
    "6573742f315f34322f6665617468657273310000020173001e0000006f72"
    "672e667265656465736b746f702e706f7274616c2e526571756573740000"
    "080167000675617b73767d00000000000301730008000000526573706f6e"
    "7365000000000000000007017300040000003a312e340000000000000000"
    "960000000400000075726973000261730000000029000000240000006669"
    "6c653a2f2f2f686f6d652f757365722f4952732f42696725323048616c6c"
    "2e776176000000000700000063686f696365730005612873732900000000"
    "00000e00000063757272656e745f66696c74657200082873612875732929"
    "0000000005000000417564696f0000000e00000000000000050000002a2e"
    "77617600";

/// GLib: `gdbus emit … Request.Response "uint32 1" "@a{sv} {}"` — a cancel.
const std::string_view kGlibResponseCancelled =
    "6c04010108000000020000009d00000001016f00360000002f6f72672f66"
    "7265656465736b746f702f706f7274616c2f6465736b746f702f72657175"
    "6573742f315f34322f6665617468657273320000020173001e0000006f72"
    "672e667265656465736b746f702e706f7274616c2e526571756573740000"
    "080167000675617b73767d00000000000301730008000000526573706f6e"
    "7365000000000000000007017300040000003a312e350000000001000000"
    "00000000";

/// libdbus (the daemon): the reply to Hello, ":1.1".
const std::string_view kDaemonHelloReply =
    "6c02010109000000010000003d00000006017300040000003a312e310000"
    "00000501750001000000080167000173000007017300140000006f72672e"
    "667265656465736b746f702e4442757300000000040000003a312e3100";

/// libdbus (the daemon): the answer to calling a portal that is not there.
const std::string_view kDaemonServiceUnknown =
    "6c03010153000000040000007500000006017300040000003a312e310000"
    "000004017300290000006f72672e667265656465736b746f702e44427573"
    "2e4572726f722e53657276696365556e6b6e6f776e000000000000000501"
    "750003000000080167000173000007017300140000006f72672e66726565"
    "6465736b746f702e44427573000000004e000000546865206e616d65206f"
    "72672e667265656465736b746f702e706f7274616c2e4465736b746f7020"
    "776173206e6f742070726f766964656420627920616e79202e7365727669"
    "63652066696c657300";

dbus::Message decodeHex(std::string_view hex) {
	const std::vector<uint8_t> bytes = fromHex(hex);
	dbus::Message m;
	REQUIRE(dbus::decode(bytes.data(), bytes.size(), m));
	return m;
}

//------------------------------------------------------------------------------------------------//
// THE FAKE BACKEND                                                                                 //
//------------------------------------------------------------------------------------------------//

class FakeBackend final : public FileDialogBackend {
public:
	class FakeSession final : public Session {
	public:
		explicit FakeSession(FakeBackend& owner) : owner_(owner) {}
		void cancel() override { ++owner_.cancels; }

	private:
		FakeBackend& owner_;
	};

	std::unique_ptr<Session> start(const FileDialogOptions& options, std::shared_ptr<FileDialogChannel> channel) override {
		++starts;
		last = options;
		this->channel = channel;
		if (answerAtOnce.has_value()) {
			channel->complete(*answerAtOnce);
		}
		return std::make_unique<FakeSession>(*this);
	}

	/// The worker's side: answer from a thread of its own, and wait for it to have answered.
	void answerFromWorker(FileDialogResult result) {
		std::thread([c = channel, r = std::move(result)] { c->complete(r); }).join();
	}

	int starts = 0;
	int cancels = 0;
	FileDialogOptions last;
	std::shared_ptr<FileDialogChannel> channel;
	std::optional<FileDialogResult> answerAtOnce;
};

FileDialogResult chosen(const char* path) { return { Status::Chosen, path, {} }; }

} // namespace

//------------------------------------------------------------------------------------------------//
// 1. THE CONTRACT                                                                                  //
//------------------------------------------------------------------------------------------------//

TEST_CASE("file dialog: the answer is delivered on the message thread, by poll, once") {
	FakeBackend backend;
	FileDialog dialog(&backend);
	int calls = 0;
	std::thread::id calledOn;
	FileDialogResult got;
	FileDialogOptions options;
	options.mode = Mode::OpenFile;
	options.title = "Load IR";
	REQUIRE(dialog.open(options, [&](const FileDialogResult& r) {
		++calls;
		calledOn = std::this_thread::get_id();
		got = r;
	}));
	CHECK(backend.starts == 1);
	CHECK(backend.last.title == "Load IR");
	CHECK(dialog.isOpen());

	backend.answerFromWorker(chosen("/tmp/hall.wav"));
	// THE WORKER HAS ANSWERED AND NOTHING HAS BEEN CALLED: the answer waits in the channel.
	CHECK(calls == 0);

	// A POLL FROM ANOTHER THREAD DELIVERS NOTHING, and leaves the answer for the message thread.
	bool offThread = true;
	std::thread([&] { offThread = dialog.poll(); }).join();
	CHECK_FALSE(offThread);
	CHECK(calls == 0);

	CHECK(dialog.poll());
	CHECK(calls == 1);
	CHECK(calledOn == std::this_thread::get_id());
	CHECK(got.status == Status::Chosen);
	CHECK(got.path == "/tmp/hall.wav");
	CHECK_FALSE(dialog.isOpen());

	CHECK_FALSE(dialog.poll());
	CHECK(calls == 1);
}

TEST_CASE("file dialog: an answer given inside start still waits for poll — never a call back from inside open") {
	FakeBackend backend;
	backend.answerAtOnce = FileDialogResult { Status::Failed, {}, "no dialog here" };
	FileDialog dialog(&backend);
	int calls = 0;
	std::string error;
	REQUIRE(dialog.open({}, [&](const FileDialogResult& r) {
		++calls;
		error = r.error;
	}));
	CHECK(calls == 0);
	CHECK(dialog.poll());
	CHECK(calls == 1);
	CHECK(error == "no dialog here");
}

TEST_CASE("file dialog: its own timer delivers the answer from visage's event loop") {
	FakeBackend backend;
	FileDialog dialog(&backend);
	int calls = 0;
	REQUIRE(dialog.open({}, [&](const FileDialogResult&) { ++calls; }));
	backend.answerFromWorker(chosen("/tmp/a.wav"));
	// visage's frame loop, stood in for by the one call it makes, until the period has certainly passed.
	const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (calls == 0 && std::chrono::steady_clock::now() < until) {
		std::this_thread::sleep_for(std::chrono::milliseconds(FileDialog::kPollMs));
		visage::EventManager::instance().checkEventTimers();
	}
	CHECK(calls == 1);
}

TEST_CASE("file dialog: a second open while one is up is refused, and the first is untouched") {
	FakeBackend backend;
	FileDialog dialog(&backend);
	int first = 0;
	int second = 0;
	REQUIRE(dialog.open({}, [&](const FileDialogResult&) { ++first; }));
	CHECK_FALSE(dialog.open({}, [&](const FileDialogResult&) { ++second; }));
	CHECK(backend.starts == 1);
	backend.answerFromWorker(chosen("/tmp/a.wav"));
	CHECK(dialog.poll());
	CHECK(first == 1);
	CHECK(second == 0);
	// AND ONCE ANSWERED, THE NEXT OPEN IS ALLOWED.
	CHECK(dialog.open({}, [&](const FileDialogResult&) { ++second; }));
	CHECK(backend.starts == 2);
}

TEST_CASE("file dialog: cancel closes the dialog and drops the answer") {
	FakeBackend backend;
	FileDialog dialog(&backend);
	int calls = 0;
	REQUIRE(dialog.open({}, [&](const FileDialogResult&) { ++calls; }));
	const std::shared_ptr<FileDialogChannel> channel = backend.channel;
	dialog.cancel();
	CHECK(backend.cancels == 1);
	CHECK(channel->abandoned());
	CHECK_FALSE(dialog.isOpen());
	// The worker finishes anyway, late: nobody is told.
	backend.answerFromWorker(chosen("/tmp/late.wav"));
	CHECK_FALSE(dialog.poll());
	CHECK(calls == 0);
}

TEST_CASE("file dialog: destroyed while open — the dialog is closed and a late answer calls nothing") {
	// THE EDITOR CLOSED WITH THE DIALOG UP. The callback captures an object that dies with the dialog's
	// owner, as a widget's `this` would; under the sanitizer gate a call into it is a report, and here it
	// is also a count that must stay zero.
	FakeBackend backend;
	auto owner = std::make_unique<int>(0);
	int* ownerState = owner.get();
	int calls = 0;
	auto dialog = std::make_unique<FileDialog>(&backend);
	REQUIRE(dialog->open({}, [ownerState, &calls](const FileDialogResult&) {
		++calls;
		++*ownerState;
	}));
	const std::shared_ptr<FileDialogChannel> channel = backend.channel;
	dialog.reset();
	owner.reset();
	CHECK(backend.cancels == 1);
	CHECK(channel->abandoned());

	backend.answerFromWorker(chosen("/tmp/after.wav"));
	// Whatever the event loop does next, nothing is called: there is nothing left to call.
	std::this_thread::sleep_for(std::chrono::milliseconds(2 * FileDialog::kPollMs));
	visage::EventManager::instance().checkEventTimers();
	CHECK(calls == 0);
}

TEST_CASE("file dialog: the callback may destroy its own dialog") {
	FakeBackend backend;
	auto dialog = std::make_unique<FileDialog>(&backend);
	int calls = 0;
	REQUIRE(dialog->open({}, [&](const FileDialogResult&) {
		++calls;
		dialog.reset();
	}));
	backend.answerFromWorker(chosen("/tmp/a.wav"));
	CHECK(dialog->poll());
	CHECK(calls == 1);
	CHECK(dialog == nullptr);
}

TEST_CASE("file dialog: a scoped backend is what a default-built dialog uses, and it nests") {
	FakeBackend outer;
	FakeBackend inner;
	{
		ScopedFileDialogBackend a(outer);
		{
			ScopedFileDialogBackend b(inner);
			FileDialog d;
			REQUIRE(d.open({}, [](const FileDialogResult&) {}));
			CHECK(inner.starts == 1);
		}
		FileDialog d;
		REQUIRE(d.open({}, [](const FileDialogResult&) {}));
		CHECK(outer.starts == 1);
	}
	CHECK(inner.starts == 1);
	CHECK(outer.starts == 1);
}

TEST_CASE("file dialog: a frame in no window has no native parent") {
	visage::Frame frame;
	CHECK(nativeWindowOf(&frame) == nullptr);
	CHECK(nativeWindowOf(nullptr) == nullptr);
}

//------------------------------------------------------------------------------------------------//
// 2. THE WIRE                                                                                      //
//------------------------------------------------------------------------------------------------//

TEST_CASE("dbus wire: GLib's OpenFile call decodes to its header and body") {
	const dbus::Message m = decodeHex(kGlibOpenFile);
	CHECK(m.type == dbus::Type::MethodCall);
	CHECK(m.flags == 0);
	CHECK(m.serial == 3);
	CHECK(m.path == "/org/freedesktop/portal/desktop");
	CHECK(m.interface == "org.freedesktop.portal.FileChooser");
	CHECK(m.member == "OpenFile");
	CHECK(m.destination == "org.freedesktop.portal.Desktop");
	CHECK(m.sender == ":1.1");
	CHECK(m.signature == "ssa{sv}");
	CHECK(m.body.size() == 0xdb);
}

TEST_CASE("dbus wire: our OpenFile body is GLib's, byte for byte") {
	FileDialogOptions options;
	options.mode = Mode::OpenFile;
	options.title = "Load IR";
	options.filters = { { "Audio", { "wav", "flac" } } };
	options.startDir = "/home/user/IRs";
	const dbus::Message ours = portal::fileChooserCall(options, "x11:3a0000f", "feathers1", 3);
	const std::vector<uint8_t> glib = fromHex(kGlibOpenFile);
	const std::vector<uint8_t> glibBody(glib.begin() + kGlibOpenFileBodyAt, glib.end());
	CHECK(ours.body == glibBody);
	CHECK(ours.signature == "ssa{sv}");
	CHECK(ours.member == "OpenFile");
	CHECK(ours.path == "/org/freedesktop/portal/desktop");
	CHECK(ours.interface == "org.freedesktop.portal.FileChooser");
	CHECK(ours.destination == "org.freedesktop.portal.Desktop");
}

TEST_CASE("dbus wire: our Hello is GLib's header field records, in code order") {
	// THE SPEC LEAVES THE ORDER OF HEADER FIELDS FREE, and GLib's is not ours (it wrote PATH, INTERFACE,
	// DESTINATION, MEMBER; libdbus writes DESTINATION first). Each 8-aligned record is fixed, though, so
	// the golden is GLib's four records copied from its Hello, ours in ascending code order, the daemon's
	// SENDER left out, and the array length and padding that follow from that: 32+32+16+29 = 0x6d.
	const std::string expected = std::string("6c010001" "00000000" "01000000" "6d000000") +
	                             /* PATH o */ "01016f00150000002f6f72672f667265656465736b746f702f44427573000000" +
	                             /* INTERFACE s */ "02017300140000006f72672e667265656465736b746f702e4442757300000000" +
	                             /* MEMBER s */ "030173000500000048656c6c6f000000" +
	                             /* DESTINATION s */ "06017300140000006f72672e667265656465736b746f702e4442757300000000";
	CHECK(dbus::encode(portal::hello(1)) == fromHex(expected));
}

TEST_CASE("dbus wire: what we encode, we decode — every field of a call with a body") {
	FileDialogOptions options;
	options.mode = Mode::SaveFile;
	options.title = "Export preset";
	options.defaultName = "Glass.fpreset";
	options.filters = { { "Feathers preset", { "fpreset" } } };
	const dbus::Message call = portal::fileChooserCall(options, "", "feathers9", 77);
	const std::vector<uint8_t> bytes = dbus::encode(call);
	bool malformed = true;
	CHECK(dbus::messageLength(bytes.data(), bytes.size(), &malformed) == bytes.size());
	CHECK_FALSE(malformed);
	dbus::Message back;
	REQUIRE(dbus::decode(bytes.data(), bytes.size(), back));
	CHECK(back.serial == 77);
	CHECK(back.member == "SaveFile");
	CHECK(back.signature == call.signature);
	CHECK(back.body == call.body);

	// AND THE BODY SAYS WHAT WAS ASKED: parent, title, then current_name among the options.
	dbus::Reader r(back);
	CHECK(r.string().empty());
	CHECK(r.string() == "Export preset");
	const size_t end = r.beginArray(8);
	std::string currentName;
	while (r.ok() && r.pos() < end) {
		r.align(8);
		const std::string key = r.string();
		const std::string type = r.signature();
		if (key == "current_name" && type == "s") {
			currentName = r.string();
		} else {
			std::string_view sig = type;
			REQUIRE(r.skip(sig));
		}
	}
	CHECK(r.ok());
	CHECK(currentName == "Glass.fpreset");
}

TEST_CASE("dbus wire: the daemon's Hello reply and its ServiceUnknown error decode") {
	const dbus::Message reply = decodeHex(kDaemonHelloReply);
	CHECK(reply.type == dbus::Type::MethodReturn);
	CHECK(reply.replySerial == 1);
	CHECK(portal::parseString(reply) == std::optional<std::string>(":1.1"));

	const dbus::Message error = decodeHex(kDaemonServiceUnknown);
	CHECK(error.type == dbus::Type::Error);
	CHECK(error.replySerial == 3);
	CHECK(error.errorName == "org.freedesktop.DBus.Error.ServiceUnknown");
	CHECK(portal::isNoPortalError(error.errorName));
	CHECK_FALSE(portal::isNoPortalError("org.freedesktop.DBus.Error.AccessDenied"));
}

TEST_CASE("dbus wire: a big-endian message decodes too") {
	// THE DAEMON FORWARDS IN THE SENDER'S BYTE ORDER. The daemon's Hello reply above, rewritten by hand
	// big-endian: 'B', and every u32 (lengths, serials) byte-swapped. Nothing else moves.
	const dbus::Message m = decodeHex(
	    "42020101" "00000009" "00000001" "0000003d"
	    "06017300000000043a312e3100000000"
	    "0501750000000001"
	    "0801670001730000"
	    "07017300000000146f72672e667265656465736b746f702e4442757300000000"
	    "000000043a312e3100");
	CHECK(m.bigEndian);
	CHECK(m.replySerial == 1);
	CHECK(m.sender == "org.freedesktop.DBus");
	CHECK(portal::parseString(m) == std::optional<std::string>(":1.1"));
}

TEST_CASE("dbus wire: a message that is not all there, or not a message, is refused") {
	const std::vector<uint8_t> bytes = fromHex(kGlibOpenFile);
	// EVERY PREFIX: the length is unknown, or known to be longer than what arrived.
	for (size_t n = 0; n < bytes.size(); ++n) {
		bool malformed = false;
		const size_t length = dbus::messageLength(bytes.data(), n, &malformed);
		CHECK_FALSE(malformed);
		CHECK((length == 0 || length > n));
		dbus::Message m;
		CHECK_FALSE(dbus::decode(bytes.data(), n, m));
	}
	// A STRING WITHOUT ITS NUL: the byte after the path "/org/freedesktop/portal/desktop" (at 24+31).
	std::vector<uint8_t> broken = bytes;
	broken[24 + 31] = 'x';
	dbus::Message m;
	CHECK_FALSE(dbus::decode(broken.data(), broken.size(), m));
	// NOT A BYTE ORDER, NOT A VERSION.
	std::vector<uint8_t> garbage = bytes;
	garbage[0] = 'x';
	bool malformed = false;
	CHECK(dbus::messageLength(garbage.data(), garbage.size(), &malformed) == 0);
	CHECK(malformed);
	garbage = bytes;
	garbage[3] = 2;
	CHECK(dbus::messageLength(garbage.data(), garbage.size(), &malformed) == 0);
	CHECK(malformed);
}

TEST_CASE("portal: GLib's Response is read past the entries around the uris") {
	portal::Response r;
	REQUIRE(portal::parseResponse(decodeHex(kGlibResponseChosen), r));
	CHECK(r.code == 0);
	REQUIRE(r.uris.size() == 1);
	CHECK(r.uris[0] == "file:///home/user/IRs/Big%20Hall.wav");
	CHECK(portal::pathFromFileUri(r.uris[0]) == std::optional<std::filesystem::path>("/home/user/IRs/Big Hall.wav"));

	portal::Response c;
	REQUIRE(portal::parseResponse(decodeHex(kGlibResponseCancelled), c));
	CHECK(c.code == 1);
	CHECK(c.uris.empty());

	// Not a Response: the OpenFile call.
	portal::Response n;
	CHECK_FALSE(portal::parseResponse(decodeHex(kGlibOpenFile), n));
}

TEST_CASE("portal: the request path, the match rule, the parent handle, the auth line") {
	CHECK(portal::requestPath(":1.42", "feathers1") == "/org/freedesktop/portal/desktop/request/1_42/feathers1");
	CHECK(portal::responseRule("/p") ==
	      "type='signal',sender='org.freedesktop.portal.Desktop',path='/p',"
	      "interface='org.freedesktop.portal.Request',member='Response'");
	CHECK(portal::parentHandle(nullptr).empty());
	CHECK(portal::parentHandle(reinterpret_cast<void*>(uintptr_t { 0x3a0000f })) == "x11:3a0000f");
	// The spec's own example: uid 1000 is the ASCII "1000", hex-encoded.
	CHECK(portal::authExternal(1000) == std::string("\0AUTH EXTERNAL 31303030\r\n", 25));
	CHECK(portal::caseInsensitiveGlob("wav") == "*.[wW][aA][vV]");
	CHECK(portal::caseInsensitiveGlob("mp3") == "*.[mM][pP]3");
}

TEST_CASE("portal: folder mode asks for a directory and no filters") {
	FileDialogOptions options;
	options.mode = Mode::OpenFolder;
	options.filters = { { "Audio", { "wav" } } };
	const dbus::Message m = portal::fileChooserCall(options, "", "t", 1);
	CHECK(m.member == "OpenFile");
	dbus::Reader r(m);
	(void)r.string();
	(void)r.string();
	const size_t end = r.beginArray(8);
	bool directory = false;
	bool filters = false;
	while (r.ok() && r.pos() < end) {
		r.align(8);
		const std::string key = r.string();
		const std::string type = r.signature();
		if (key == "directory" && type == "b") {
			directory = r.boolean();
			continue;
		}
		filters = filters || key == "filters";
		std::string_view sig = type;
		REQUIRE(r.skip(sig));
	}
	CHECK(r.ok());
	CHECK(directory);
	CHECK_FALSE(filters);
}

TEST_CASE("portal: bus addresses, file uris") {
	const auto a = portal::parseBusAddress("tcp:host=x;unix:path=/run/user/1000/bus,guid=abc;unix:abstract=/tmp/dbus-%41b");
	REQUIRE(a.size() == 2);
	CHECK_FALSE(a[0].abstract);
	CHECK(a[0].path == "/run/user/1000/bus");
	CHECK(a[1].abstract);
	CHECK(a[1].path == "/tmp/dbus-Ab");
	CHECK(portal::parseBusAddress("").empty());
	CHECK(portal::parseBusAddress("unix:path=%4").empty());

	CHECK(portal::pathFromFileUri("file://localhost/a/b") == std::optional<std::filesystem::path>("/a/b"));
	CHECK(portal::pathFromFileUri("file:///run/user/1000/doc/9f/x%25y.wav") ==
	      std::optional<std::filesystem::path>("/run/user/1000/doc/9f/x%y.wav"));
	CHECK_FALSE(portal::pathFromFileUri("https://example.com/a.wav").has_value());
	CHECK_FALSE(portal::pathFromFileUri("file://otherhost/a.wav").has_value());
	CHECK_FALSE(portal::pathFromFileUri("file:///a%2").has_value());
}

TEST_CASE("portal: the zenity and kdialog fallbacks' arguments and answers") {
	FileDialogOptions open;
	open.mode = Mode::OpenFile;
	open.title = "Load IR";
	open.startDir = "/irs";
	open.filters = { { "Audio", { "wav", "flac" } } };
	CHECK(portal::zenityArgs(open) == std::vector<std::string> { "--file-selection", "--title=Load IR", "--filename=/irs/",
	                                                             "--file-filter=Audio | *.[wW][aA][vV] *.[fF][lL][aA][cC]" });
	CHECK(portal::kdialogArgs(open) ==
	      std::vector<std::string> { "--title", "Load IR", "--getopenfilename", "/irs", "Audio (*.wav *.flac)" });

	FileDialogOptions folder;
	folder.mode = Mode::OpenFolder;
	CHECK(portal::zenityArgs(folder) == std::vector<std::string> { "--file-selection", "--directory" });
	CHECK(portal::kdialogArgs(folder) == std::vector<std::string> { "--getexistingdirectory", "." });

	FileDialogOptions save;
	save.mode = Mode::SaveFile;
	save.startDir = "/p";
	save.defaultName = "Glass.fpreset";
	CHECK(portal::zenityArgs(save) ==
	      std::vector<std::string> { "--file-selection", "--save", "--confirm-overwrite", "--filename=/p/Glass.fpreset" });
	CHECK(portal::kdialogArgs(save) == std::vector<std::string> { "--getsavefilename", "/p/Glass.fpreset" });

	CHECK(portal::fallbackResult("/a b.wav\n", 0).status == Status::Chosen);
	CHECK(portal::fallbackResult("/a b.wav\n", 0).path == "/a b.wav");
	CHECK(portal::fallbackResult("", 1).status == Status::Cancelled);
	CHECK(portal::fallbackResult("", 0).status == Status::Failed);
	CHECK(portal::fallbackResult("", 5).status == Status::Failed);
}

//------------------------------------------------------------------------------------------------//
// 3. A LIVE BUS (Linux, where dbus-daemon is installed)                                            //
//------------------------------------------------------------------------------------------------//

#if defined(__linux__)

namespace {

std::string findOnPath(const char* program) {
	const char* path = std::getenv("PATH");
	std::string_view dirs = path != nullptr ? path : "/usr/bin:/bin";
	while (!dirs.empty()) {
		const size_t colon = dirs.find(':');
		const std::string dir(dirs.substr(0, colon));
		dirs = colon == std::string_view::npos ? std::string_view {} : dirs.substr(colon + 1);
		const std::string candidate = dir + "/" + program;
		if (!dir.empty() && ::access(candidate.c_str(), X_OK) == 0) {
			return candidate;
		}
	}
	return {};
}

/// **A PRIVATE SESSION BUS** for one case: its own daemon on a socket in a scratch folder, stopped by
/// its pid when the case ends. The environment points at it — and PATH at an empty folder, so the
/// dialog's zenity/kdialog fallbacks cannot be found — for exactly as long as it lives.
///
/// **ITS OWN CONFIG, NOT `--session`.** The system's session config lists the system's service
/// directories, and on a desktop that has xdg-desktop-portal installed the daemon would START THE REAL
/// PORTAL the moment a case called it — a real dialog on somebody's screen. This config has no service
/// directory at all, or (`activatablePortal`) one of our own naming a portal whose Exec is `/bin/false`:
/// listed as startable, never started by anything here.
class PrivateBus {
public:
	explicit PrivateBus(bool activatablePortal = false) {
		const std::string daemon = findOnPath("dbus-daemon");
		if (daemon.empty()) {
			return;
		}
		static int counter = 0;
		dir_ = std::filesystem::temp_directory_path() /
		       ("feathers-bus-" + std::to_string(::getpid()) + "-" + std::to_string(++counter));
		std::filesystem::create_directories(dir_ / "empty-path");
		const std::string socket = (dir_ / "bus").string();
		std::string services;
		if (activatablePortal) {
			std::filesystem::create_directories(dir_ / "services");
			std::ofstream(dir_ / "services" / "portal.service")
			    << "[D-BUS Service]\nName=org.freedesktop.portal.Desktop\nExec=/bin/false\n";
			services = "  <servicedir>" + (dir_ / "services").string() + "</servicedir>\n";
		}
		std::ofstream(dir_ / "bus.conf")
		    << "<!DOCTYPE busconfig PUBLIC \"-//freedesktop//DTD D-BUS Bus Configuration 1.0//EN\"\n"
		       " \"http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd\">\n"
		       "<busconfig>\n  <type>session</type>\n  <listen>unix:path="
		    << socket << "</listen>\n  <auth>EXTERNAL</auth>\n" << services
		    << "  <policy context=\"default\">\n    <allow send_destination=\"*\" eavesdrop=\"true\"/>\n"
		       "    <allow eavesdrop=\"true\"/>\n    <allow own=\"*\"/>\n  </policy>\n</busconfig>\n";
		const std::string config = "--config-file=" + (dir_ / "bus.conf").string();
		const char* argv[] = { daemon.c_str(), config.c_str(), "--nofork", "--nopidfile", nullptr };
		posix_spawn_file_actions_t actions;
		posix_spawn_file_actions_init(&actions);
		posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
		posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
		const int spawned = posix_spawn(&pid_, daemon.c_str(), &actions, nullptr, const_cast<char**>(argv), environ);
		posix_spawn_file_actions_destroy(&actions);
		if (spawned != 0) {
			pid_ = 0;
			return;
		}
		address_ = { false, socket };
		for (int i = 0; i < 200; ++i) {
			portal::BusConnection probe;
			if (probe.connect(address_)) {
				ready_ = true;
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(25));
		}
		if (const char* old = std::getenv("DBUS_SESSION_BUS_ADDRESS"); old != nullptr) {
			oldBus_ = old;
		}
		if (const char* old = std::getenv("PATH"); old != nullptr) {
			oldPath_ = old;
		}
		::setenv("DBUS_SESSION_BUS_ADDRESS", ("unix:path=" + socket).c_str(), 1);
		::setenv("PATH", (dir_ / "empty-path").c_str(), 1);
	}
	~PrivateBus() {
		if (pid_ != 0) {
			::setenv("PATH", oldPath_.c_str(), 1);
			if (oldBus_.has_value()) {
				::setenv("DBUS_SESSION_BUS_ADDRESS", oldBus_->c_str(), 1);
			} else {
				::unsetenv("DBUS_SESSION_BUS_ADDRESS");
			}
			(void)::kill(pid_, SIGTERM); // our own child, by pid
			int status = 0;
			(void)::waitpid(pid_, &status, 0);
		}
		if (!dir_.empty()) {
			std::error_code ec;
			std::filesystem::remove_all(dir_, ec);
		}
	}
	PrivateBus(const PrivateBus&) = delete;
	PrivateBus& operator=(const PrivateBus&) = delete;

	[[nodiscard]] bool ready() const noexcept { return ready_; }
	[[nodiscard]] const portal::BusAddress& address() const noexcept { return address_; }

private:
	std::filesystem::path dir_;
	pid_t pid_ = 0;
	portal::BusAddress address_;
	bool ready_ = false;
	std::optional<std::string> oldBus_;
	std::string oldPath_;
};

/// **A FAKE xdg-desktop-portal**: owns org.freedesktop.portal.Desktop on the private bus, answers the
/// FileChooser call with the request path the spec prescribes, and then either sends the Response
/// (`uri` non-empty) or waits for our Request.Close. Written with the same connection class as the
/// dialog — the independent party in the room is the daemon, which validates and routes every byte.
class FakePortal {
public:
	FakePortal(const portal::BusAddress& address, std::string uri) : uri_(std::move(uri)) {
		thread_ = std::thread([this, address] { run(address); });
		const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (!ready_.load() && !failed_.load() && std::chrono::steady_clock::now() < until) {
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
	}
	~FakePortal() {
		stop_.store(true);
		thread_.join();
	}
	FakePortal(const FakePortal&) = delete;
	FakePortal& operator=(const FakePortal&) = delete;

	[[nodiscard]] bool ready() const { return ready_.load(); }
	[[nodiscard]] bool called() const { return called_.load(); }
	[[nodiscard]] bool closed() const { return closed_.load(); }
	std::string member;
	std::string title;
	std::string token;
	bool directory = false;

private:
	void run(const portal::BusAddress& address) {
		portal::BusConnection bus;
		if (!bus.connect(address) || !bus.authenticate(-1, 5000)) {
			failed_.store(true);
			return;
		}
		if (bus.hello(-1, 5000).empty()) {
			failed_.store(true);
			return;
		}
		dbus::Message request;
		request.type = dbus::Type::MethodCall;
		request.path = portal::kBusPath;
		request.interface = portal::kBusName;
		request.member = "RequestName";
		request.destination = portal::kBusName;
		request.signature = "su";
		dbus::Writer w;
		w.string(portal::kPortalName);
		w.u32(4); // DBUS_NAME_FLAG_DO_NOT_QUEUE
		request.body = w.take();
		const uint32_t serial = bus.send(request);
		dbus::Message msg;
		for (;;) {
			if (bus.next(msg, -1, 5000) != portal::BusConnection::Wait::Got) {
				failed_.store(true);
				return;
			}
			if (msg.replySerial == serial) {
				dbus::Reader r(msg);
				if (msg.type != dbus::Type::MethodReturn || r.u32() != 1) { // PRIMARY_OWNER
					failed_.store(true);
					return;
				}
				break;
			}
		}
		ready_.store(true);

		std::string handle;
		while (!stop_.load()) {
			const auto got = bus.next(msg, -1, 50);
			if (got == portal::BusConnection::Wait::TimedOut) {
				continue;
			}
			if (got != portal::BusConnection::Wait::Got) {
				return;
			}
			if (msg.type != dbus::Type::MethodCall) {
				continue;
			}
			if (msg.interface == portal::kRequestInterface && msg.member == "Close" && msg.path == handle) {
				closed_.store(true);
				continue;
			}
			if (msg.interface != portal::kChooserInterface) {
				continue;
			}
			member = msg.member;
			dbus::Reader r(msg);
			(void)r.string();
			title = r.string();
			const size_t end = r.beginArray(8);
			while (r.ok() && r.pos() < end) {
				r.align(8);
				const std::string key = r.string();
				const std::string type = r.signature();
				if (key == "handle_token" && type == "s") {
					token = r.string();
				} else if (key == "directory" && type == "b") {
					directory = r.boolean();
				} else {
					std::string_view sig = type;
					(void)r.skip(sig);
				}
			}
			handle = portal::requestPath(msg.sender, token);

			dbus::Message reply;
			reply.type = dbus::Type::MethodReturn;
			reply.flags = dbus::kNoReplyExpected;
			reply.replySerial = msg.serial;
			reply.destination = msg.sender;
			reply.signature = "o";
			dbus::Writer rw;
			rw.objectPath(handle);
			reply.body = rw.take();
			(void)bus.send(reply);
			called_.store(true);

			if (!uri_.empty()) {
				dbus::Message signal;
				signal.type = dbus::Type::Signal;
				signal.flags = dbus::kNoReplyExpected;
				signal.path = handle;
				signal.interface = portal::kRequestInterface;
				signal.member = "Response";
				signal.signature = "ua{sv}";
				dbus::Writer sw;
				sw.u32(0);
				const auto results = sw.beginArray(8);
				sw.beginStruct();
				sw.string("uris");
				sw.variantSignature("as");
				const auto uris = sw.beginArray(4);
				sw.string(uri_);
				sw.endArray(uris);
				sw.endArray(results);
				signal.body = sw.take();
				(void)bus.send(signal);
			}
		}
	}

	std::string uri_;
	std::thread thread_;
	std::atomic<bool> ready_ { false };
	std::atomic<bool> failed_ { false };
	std::atomic<bool> called_ { false };
	std::atomic<bool> closed_ { false };
	std::atomic<bool> stop_ { false };
};

/// Poll until the callback has run, or give up.
bool pumpUntil(FileDialog& dialog, const bool& done, int seconds = 10) {
	const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
	while (!done && std::chrono::steady_clock::now() < until) {
		(void)dialog.poll();
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	return done;
}

template <typename Pred>
bool waitFor(Pred pred, int seconds = 10) {
	const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
	while (!pred() && std::chrono::steady_clock::now() < until) {
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	return pred();
}

} // namespace

TEST_CASE("live bus: the portal answers, and the file comes back as a path") {
	PrivateBus bus;
	if (!bus.ready()) {
		MESSAGE("no dbus-daemon here; the live-bus cases are skipped");
		return;
	}
	FakePortal portalService(bus.address(), "file:///tmp/Big%20Hall.wav");
	REQUIRE(portalService.ready());

	FileDialog dialog(&platformFileDialogBackend());
	bool done = false;
	FileDialogResult got;
	FileDialogOptions options;
	options.title = "Load IR";
	options.filters = { { "Audio", { "wav", "flac" } } };
	REQUIRE(dialog.open(options, [&](const FileDialogResult& r) {
		got = r;
		done = true;
	}));
	REQUIRE(pumpUntil(dialog, done));
	CHECK(got.status == Status::Chosen);
	CHECK(got.path == "/tmp/Big Hall.wav");
	CHECK(portalService.member == "OpenFile");
	CHECK(portalService.title == "Load IR");
	CHECK_FALSE(portalService.directory);
}

TEST_CASE("live bus: a folder is asked for with directory=true") {
	PrivateBus bus;
	if (!bus.ready()) {
		MESSAGE("no dbus-daemon here; the live-bus cases are skipped");
		return;
	}
	FakePortal portalService(bus.address(), "file:///tmp/irs");
	REQUIRE(portalService.ready());
	FileDialog dialog(&platformFileDialogBackend());
	bool done = false;
	FileDialogResult got;
	FileDialogOptions options;
	options.mode = Mode::OpenFolder;
	REQUIRE(dialog.open(options, [&](const FileDialogResult& r) {
		got = r;
		done = true;
	}));
	REQUIRE(pumpUntil(dialog, done));
	CHECK(got.path == "/tmp/irs");
	CHECK(portalService.directory);
}

TEST_CASE("live bus: a dialog destroyed while the portal has it up closes the portal's request") {
	PrivateBus bus;
	if (!bus.ready()) {
		MESSAGE("no dbus-daemon here; the live-bus cases are skipped");
		return;
	}
	FakePortal portalService(bus.address(), ""); // never answers
	REQUIRE(portalService.ready());
	int calls = 0;
	{
		FileDialog dialog(&platformFileDialogBackend());
		REQUIRE(dialog.open({}, [&](const FileDialogResult&) { ++calls; }));
		REQUIRE(waitFor([&] { return portalService.called(); }));
	}
	CHECK(waitFor([&] { return portalService.closed(); }));
	CHECK(calls == 0);
}

TEST_CASE("live bus: no portal and no fallback program is a clean failure that says so") {
	PrivateBus bus;
	if (!bus.ready()) {
		MESSAGE("no dbus-daemon here; the live-bus cases are skipped");
		return;
	}
	FileDialog dialog(&platformFileDialogBackend());
	bool done = false;
	FileDialogResult got;
	REQUIRE(dialog.open({}, [&](const FileDialogResult& r) {
		got = r;
		done = true;
	}));
	REQUIRE(pumpUntil(dialog, done));
	CHECK(got.status == Status::Failed);
	CHECK(got.error.find("No file dialog is available") != std::string::npos);
}

TEST_CASE("live bus: a portal is reachable when it is OWNED, and when it is only STARTABLE; not when it is neither") {
	{
		PrivateBus bus;
		if (!bus.ready()) {
			MESSAGE("no dbus-daemon here; the live-bus cases are skipped");
			return;
		}
		CHECK_FALSE(portal::portalReachable(2000));
		FakePortal owned(bus.address(), "");
		REQUIRE(owned.ready());
		CHECK(portal::portalReachable(2000));
	}
	{
		// xdg-desktop-portal IS USUALLY STARTED ON DEMAND: nobody owns the name until the first call, so
		// "not owned" must not read as "no portal". Listed as startable (Exec /bin/false, never run here).
		PrivateBus bus(true);
		REQUIRE(bus.ready());
		CHECK(portal::portalReachable(2000));
	}
}

TEST_CASE("live bus: FileDialog::available() is false with no portal and no fallback program on PATH") {
	PrivateBus bus;
	if (!bus.ready()) {
		MESSAGE("no dbus-daemon here; the live-bus cases are skipped");
		return;
	}
	// The ONE case in this binary that asks the platform backend, whose answer is kept for 5 s.
	CHECK_FALSE(FileDialog::available());
}

#endif

TEST_CASE("file dialog: available() asks the backend open() would use — the scoped one when installed") {
	FakeBackend yes;
	class NoDialogs final : public FileDialogBackend {
	public:
		bool available() override { return false; }
		std::unique_ptr<Session> start(const FileDialogOptions&, std::shared_ptr<FileDialogChannel>) override {
			return nullptr;
		}
	} no;
	{
		ScopedFileDialogBackend a(yes);
		CHECK(FileDialog::available());
		{
			ScopedFileDialogBackend b(no);
			CHECK_FALSE(FileDialog::available());
		}
		CHECK(FileDialog::available());
	}
}
