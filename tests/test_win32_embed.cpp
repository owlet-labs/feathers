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

// THE WIN32 EMBED, DRIVEN THE WAY A WINDOWS HOST DRIVES IT — and photographed, because nobody on the team has a
// Windows machine to look with.
//
// The plugin is stood up through its own `clap_entry`, as a host loads it, and its editor is embedded in a real
// top-level window this test makes: create, get_size, set_parent, show, set_size, destroy, then twenty more
// open/close cycles. Every claim reads the WINDOW SYSTEM's answer (GetParent, IsWindowVisible, GetClientRect, the
// process's USER and GDI object counts), never the plugin's own account of itself.
//
// THE PICTURE is taken with PrintWindow(PW_RENDERFULLCONTENT), which asks the compositor for what the window shows,
// D3D content included, and is written as a BMP beside the test (`win32-editor-*.bmp`; CI uploads them) for a person
// to look at. The case asserts only what a picture can carry by value: that it is not blank. A frame of one colour, or
// of a handful, is what an editor that never drew looks like, and that is what fails here.
//
// Windows only (tests/CMakeLists.txt). The runner has an interactive desktop, so windows really are created and shown.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "clap/clap.h"

#include <windows.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

extern "C" const clap_plugin_entry clap_entry;

namespace {

const void* CLAP_ABI hostGetExtension(const clap_host*, const char*) { return nullptr; }
void CLAP_ABI hostRequestRestart(const clap_host*) {}
void CLAP_ABI hostRequestProcess(const clap_host*) {}
void CLAP_ABI hostRequestCallback(const clap_host*) {}

const clap_host kHost = {
    CLAP_VERSION_INIT,  nullptr,          "feathers win32 embed test", "Owlet Records", "", "1.0",
    hostGetExtension,   hostRequestRestart, hostRequestProcess,        hostRequestCallback,
};

/// Run the thread's message loop for `ms`: the host's loop is what delivers paint, input and visage's frame requests
/// to the child window, so a test that never pumped would photograph an editor that was never given the chance to draw.
void pump(int ms) {
	const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
	while (std::chrono::steady_clock::now() < until) {
		MSG msg {};
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE) != 0) {
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
}

LRESULT CALLBACK hostWindowProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
	return DefWindowProcW(hwnd, msg, w, l);
}

/// THE HOST'S CONTAINER: a plain top-level window, shown, big enough for the editor at its preferred size.
struct HostWindow {
	HWND hwnd = nullptr;
	explicit HostWindow(int width, int height) {
		static const ATOM kClass = [] {
			WNDCLASSEXW wc {};
			wc.cbSize = sizeof(wc);
			wc.lpfnWndProc = hostWindowProc;
			wc.hInstance = GetModuleHandleW(nullptr);
			wc.lpszClassName = L"FeathersTestHost";
			wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
			return RegisterClassExW(&wc);
		}();
		(void)kClass;
		RECT r { 0, 0, width, height };
		AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
		hwnd = CreateWindowExW(0, L"FeathersTestHost", L"Feathers embed test", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, 0,
		                       0, r.right - r.left, r.bottom - r.top, nullptr, nullptr, GetModuleHandleW(nullptr),
		                       nullptr);
		REQUIRE(hwnd != nullptr);
		ShowWindow(hwnd, SW_SHOWNORMAL);
		UpdateWindow(hwnd);
		pump(50);
	}
	~HostWindow() { DestroyWindow(hwnd); }
	HostWindow(const HostWindow&) = delete;
	HostWindow& operator=(const HostWindow&) = delete;
};

/// The plugin, stood up through `clap_entry` as a host stands one up.
struct Plugin {
	const clap_plugin* plugin = nullptr;
	const clap_plugin_gui* gui = nullptr;
	Plugin() {
		REQUIRE(clap_entry.init("feathers-win32-embed-test"));
		const auto* factory = static_cast<const clap_plugin_factory*>(clap_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
		REQUIRE(factory != nullptr);
		plugin = factory->create_plugin(factory, &kHost, "com.owlet.feathers");
		REQUIRE(plugin != nullptr);
		REQUIRE(plugin->init(plugin));
		REQUIRE(plugin->activate(plugin, 48000.0, 1, 512));
		gui = static_cast<const clap_plugin_gui*>(plugin->get_extension(plugin, CLAP_EXT_GUI));
	}
	~Plugin() {
		plugin->deactivate(plugin);
		plugin->destroy(plugin);
		clap_entry.deinit();
	}
	Plugin(const Plugin&) = delete;
	Plugin& operator=(const Plugin&) = delete;
};

/// The editor's own window: the host container's one child.
HWND editorIn(HWND host) {
	return GetWindow(host, GW_CHILD);
}

SIZE clientSizeOf(HWND hwnd) {
	RECT r {};
	GetClientRect(hwnd, &r);
	return { r.right - r.left, r.bottom - r.top };
}

/// What a picture can say by value: how many distinct colours it has, and the spread of its luminance.
struct PictureStats {
	size_t distinctColours = 0;
	double luminanceStdDev = 0.0;
	uint64_t hash = 0;
};

/// PHOTOGRAPH `hwnd` as the compositor shows it, write it to `path` as a 32-bit BMP, and measure it.
PictureStats photograph(HWND hwnd, const char* path) {
	const SIZE size = clientSizeOf(hwnd);
	REQUIRE(size.cx > 0);
	REQUIRE(size.cy > 0);
	HDC screen = GetDC(nullptr);
	HDC dc = CreateCompatibleDC(screen);
	BITMAPINFO info {};
	info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	info.bmiHeader.biWidth = size.cx;
	info.bmiHeader.biHeight = -size.cy; // top-down
	info.bmiHeader.biPlanes = 1;
	info.bmiHeader.biBitCount = 32;
	info.bmiHeader.biCompression = BI_RGB;
	void* bits = nullptr;
	HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
	REQUIRE(bitmap != nullptr);
	HGDIOBJ old = SelectObject(dc, bitmap);
	constexpr UINT kRenderFullContent = 0x00000002; // PW_RENDERFULLCONTENT: DirectX content included
	const BOOL printed = PrintWindow(hwnd, dc, PW_CLIENTONLY | kRenderFullContent);
	CHECK(printed != 0);
	GdiFlush();

	const size_t pixels = static_cast<size_t>(size.cx) * static_cast<size_t>(size.cy);
	const auto* px = static_cast<const uint32_t*>(bits);
	PictureStats stats;
	std::unordered_set<uint32_t> colours;
	double sum = 0.0;
	double sumSq = 0.0;
	uint64_t h = 1469598103934665603ull;
	for (size_t i = 0; i < pixels; ++i) {
		const uint32_t c = px[i] & 0x00FFFFFFu;
		if (colours.size() < 100000) {
			colours.insert(c);
		}
		const double lum = 0.2126 * ((c >> 16) & 0xFF) + 0.7152 * ((c >> 8) & 0xFF) + 0.0722 * (c & 0xFF);
		sum += lum;
		sumSq += lum * lum;
		h = (h ^ c) * 1099511628211ull;
	}
	const double mean = sum / static_cast<double>(pixels);
	stats.distinctColours = colours.size();
	stats.luminanceStdDev = std::sqrt(std::max(0.0, sumSq / static_cast<double>(pixels) - mean * mean));
	stats.hash = h;

	// THE FILE, for a person: a BMP needs no encoder, and every viewer opens it.
	if (FILE* f = std::fopen(path, "wb"); f != nullptr) {
		BITMAPFILEHEADER file {};
		BITMAPINFOHEADER header = info.bmiHeader;
		const DWORD imageBytes = static_cast<DWORD>(pixels * 4);
		file.bfType = 0x4D42; // "BM"
		file.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
		file.bfSize = file.bfOffBits + imageBytes;
		std::fwrite(&file, sizeof(file), 1, f);
		std::fwrite(&header, sizeof(header), 1, f);
		std::fwrite(bits, imageBytes, 1, f);
		std::fclose(f);
	}

	SelectObject(dc, old);
	DeleteObject(bitmap);
	DeleteDC(dc);
	ReleaseDC(nullptr, screen);
	return stats;
}

/// The process's USER and GDI object counts: a window, a DC or a bitmap that an open/close cycle forgets to give back
/// shows up here and nowhere else.
struct GuiObjects {
	DWORD user = 0;
	DWORD gdi = 0;
};
GuiObjects guiObjects() {
	return { GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS), GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) };
}

/// One embed, the way a host does it: create, size, set_parent, show; the editor is then pumped for `drawMs`.
void open(const Plugin& p, HWND host, uint32_t width, uint32_t height, int drawMs) {
	REQUIRE(p.gui->create(p.plugin, CLAP_WINDOW_API_WIN32, false));
	uint32_t w = width;
	uint32_t h = height;
	REQUIRE(p.gui->adjust_size(p.plugin, &w, &h));
	REQUIRE(p.gui->set_size(p.plugin, w, h));
	clap_window window {};
	window.api = CLAP_WINDOW_API_WIN32;
	window.win32 = host;
	REQUIRE(p.gui->set_parent(p.plugin, &window));
	REQUIRE(p.gui->show(p.plugin));
	pump(drawMs);
}

} // namespace

TEST_CASE("win32 embed: the editor is offered as WIN32, embedded, opens in its host window at the size it says, and draws") {
	Plugin p;
	REQUIRE(p.gui != nullptr);
	CHECK(p.gui->is_api_supported(p.plugin, CLAP_WINDOW_API_WIN32, false));
	CHECK_FALSE(p.gui->is_api_supported(p.plugin, CLAP_WINDOW_API_WIN32, true));
	CHECK_FALSE(p.gui->is_api_supported(p.plugin, CLAP_WINDOW_API_X11, false));
	const char* preferred = nullptr;
	bool floating = true;
	REQUIRE(p.gui->get_preferred_api(p.plugin, &preferred, &floating));
	CHECK(std::strcmp(preferred, CLAP_WINDOW_API_WIN32) == 0);
	CHECK_FALSE(floating);

	REQUIRE(p.gui->create(p.plugin, CLAP_WINDOW_API_WIN32, false));
	uint32_t w = 0;
	uint32_t h = 0;
	REQUIRE(p.gui->get_size(p.plugin, &w, &h));
	MESSAGE("get_size before the embed: " << w << " x " << h);
	REQUIRE(w > 0);
	REQUIRE(h > 0);
	HostWindow host(static_cast<int>(w), static_cast<int>(h));

	clap_window window {};
	window.api = CLAP_WINDOW_API_WIN32;
	window.win32 = host.hwnd;
	REQUIRE(p.gui->set_parent(p.plugin, &window));
	REQUIRE(p.gui->show(p.plugin));
	pump(1500);

	// THE WINDOW SYSTEM'S ANSWERS: parented in the host's container, a child, visible, the size the editor said.
	const HWND editor = editorIn(host.hwnd);
	REQUIRE(editor != nullptr);
	CHECK(GetParent(editor) == host.hwnd);
	CHECK((GetWindowLongPtrW(editor, GWL_STYLE) & WS_CHILD) != 0);
	CHECK(IsWindowVisible(editor) != 0);
	uint32_t gotW = 0;
	uint32_t gotH = 0;
	REQUIRE(p.gui->get_size(p.plugin, &gotW, &gotH));
	const SIZE client = clientSizeOf(editor);
	MESSAGE("editor client " << client.cx << " x " << client.cy << ", get_size " << gotW << " x " << gotH);
	CHECK(client.cx == static_cast<LONG>(gotW));
	CHECK(client.cy == static_cast<LONG>(gotH));

	// IT DREW. Written for a person to look at, and measured: not one colour, not a handful, and with real contrast.
	const PictureStats first = photograph(editor, "win32-editor-open.bmp");
	MESSAGE("first open: " << first.distinctColours << " distinct colours, luminance std dev " << first.luminanceStdDev
	                       << ", hash " << first.hash);
	CHECK(first.distinctColours >= 64);
	CHECK(first.luminanceStdDev >= 8.0);

	// A HOST RESIZE reaches the child window: the size the plugin agreed to is the size the window is.
	uint32_t bigW = w + 200;
	uint32_t bigH = h + 100;
	REQUIRE(p.gui->adjust_size(p.plugin, &bigW, &bigH));
	SetWindowPos(host.hwnd, nullptr, 0, 0, static_cast<int>(bigW) + 64, static_cast<int>(bigH) + 64,
	             SWP_NOMOVE | SWP_NOZORDER);
	REQUIRE(p.gui->set_size(p.plugin, bigW, bigH));
	pump(500);
	const SIZE resized = clientSizeOf(editor);
	MESSAGE("after set_size " << bigW << " x " << bigH << ": client " << resized.cx << " x " << resized.cy);
	CHECK(resized.cx == static_cast<LONG>(bigW));
	CHECK(resized.cy == static_cast<LONG>(bigH));
	// NOT PHOTOGRAPHED: the runner's screen is 1024 x 768, so a window grown past the design's open size runs off it,
	// and the compositor has nothing to show for the part that is off screen (MEASURED: the picture came back cropped
	// to the screen, white beyond). The size above is the claim; the pictures are taken at sizes the screen holds.

	// CLOSED: hidden and out of the host's container, so the host may destroy the container without taking it.
	REQUIRE(p.gui->hide(p.plugin));
	p.gui->destroy(p.plugin);
	pump(100);
	CHECK(editorIn(host.hwnd) == nullptr);
	CHECK(IsWindowVisible(editor) == 0);
}

TEST_CASE("win32 embed: twenty open/close cycles each leave the editor parented, sized and visible, and leak nothing") {
	Plugin p;
	REQUIRE(p.gui != nullptr);
	HostWindow host(1600, 1000);
	uint32_t w = 1400;
	uint32_t h = 860;
	REQUIRE(p.gui->create(p.plugin, CLAP_WINDOW_API_WIN32, false));
	REQUIRE(p.gui->adjust_size(p.plugin, &w, &h));
	p.gui->destroy(p.plugin);

	GuiObjects afterFirst {};
	HWND firstEditor = nullptr;
	constexpr int kCycles = 21; // the first, then the twenty the claim is about
	for (int cycle = 0; cycle < kCycles; ++cycle) {
		CAPTURE(cycle);
		open(p, host.hwnd, w, h, cycle == 0 ? 800 : 150);
		const HWND editor = editorIn(host.hwnd);
		REQUIRE(editor != nullptr);
		CHECK(GetParent(editor) == host.hwnd);
		CHECK(IsWindowVisible(editor) != 0);
		const SIZE client = clientSizeOf(editor);
		CHECK(client.cx == static_cast<LONG>(w));
		CHECK(client.cy == static_cast<LONG>(h));
		// ONE EDITOR WINDOW, MOVED, NEVER REBUILT: the editor is built once and reparented (plugin_window.cpp's
		// create), so every open finds the same HWND.
		if (cycle == 0) {
			firstEditor = editor;
		}
		CHECK(editor == firstEditor);

		REQUIRE(p.gui->hide(p.plugin));
		p.gui->destroy(p.plugin);
		pump(50);
		CHECK(editorIn(host.hwnd) == nullptr);
		CHECK(IsWindowVisible(editor) == 0);
		if (cycle == 0) {
			afterFirst = guiObjects();
		}
	}
	const GuiObjects afterAll = guiObjects();
	MESSAGE("USER objects after the first cycle " << afterFirst.user << ", after all " << afterAll.user << "; GDI "
	                                              << afterFirst.gdi << " -> " << afterAll.gdi);
	// NOTHING GROWS PER CYCLE. A small allowance for the system's own lazily made objects (a caret, a cursor), which is
	// far under the twenty a per-cycle leak of even one object would add.
	CHECK(afterAll.user <= afterFirst.user + 4);
	CHECK(afterAll.gdi <= afterFirst.gdi + 4);

	// And the last open still draws: the twentieth reopen photographed, as the first one was.
	open(p, host.hwnd, w, h, 800);
	const PictureStats last = photograph(editorIn(host.hwnd), "win32-editor-reopened.bmp");
	MESSAGE("after twenty reopens: " << last.distinctColours << " distinct colours, luminance std dev "
	                                 << last.luminanceStdDev);
	CHECK(last.distinctColours >= 64);
	CHECK(last.luminanceStdDev >= 8.0);
	REQUIRE(p.gui->hide(p.plugin));
	p.gui->destroy(p.plugin);
}
