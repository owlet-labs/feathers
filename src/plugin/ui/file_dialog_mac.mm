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

// THE MACOS FILE DIALOG — NSOpenPanel / NSSavePanel, begun with a completion handler so nothing waits.
//
// **ON THE MAIN THREAD, WHICH IS THE MESSAGE THREAD** in every macOS plugin host, so `start` begins the
// panel directly. It is a FREE-STANDING panel (`beginWithCompletionHandler:`), not a sheet on the host's
// window: a sheet belongs to a window the plugin does not own, and a host that closes or reparents that
// window while the sheet is up is a case nobody here can test. The panel is brought to the front.
//
// THE FILTER IS A DELEGATE, not `allowedFileTypes` (deprecated since 12) nor `allowedContentTypes`
// (needs UniformTypeIdentifiers and a UTType for every extension a person might have): folders stay
// enabled so they can be opened, and a file is enabled when its extension matches, in any case.
//
// Compiled with ARC (`-fobjc-arc`, set on this file in CMake, as visage's own Objective-C is).
// NOT RUN BY THIS SESSION: the Linux container cannot build it. macOS CI compiles it.

#if defined(__APPLE__)

#include "plugin/ui/file_dialog.h"

#import <AppKit/AppKit.h>

#include <string>
#include <vector>

using feathers::ui::FileDialogBackend;
using feathers::ui::FileDialogChannel;
using feathers::ui::FileDialogOptions;
using feathers::ui::FileDialogResult;

@interface FeathersPanelFilter : NSObject <NSOpenSavePanelDelegate>
@property(nonatomic, strong) NSArray<NSString*>* extensions; // lower case, no dot; empty: anything
@end

@implementation FeathersPanelFilter
- (BOOL)panel:(id)sender shouldEnableURL:(NSURL*)url {
	(void)sender;
	if (self.extensions.count == 0) {
		return YES;
	}
	NSNumber* isDirectory = nil;
	[url getResourceValue:&isDirectory forKey:NSURLIsDirectoryKey error:nil];
	if (isDirectory.boolValue) {
		return YES;
	}
	return [self.extensions containsObject:url.pathExtension.lowercaseString];
}
@end

namespace feathers::ui {

namespace {

NSString* ns(const std::string& s) { return [NSString stringWithUTF8String:s.c_str()]; }

class MacSession final : public FileDialogBackend::Session {
public:
	explicit MacSession(NSSavePanel* panel) : panel_(panel) {}
	/// CLOSES THE PANEL; its handler then runs with Cancel, into a channel nobody reads any more.
	void cancel() override { [panel_ cancel:nil]; }

private:
	NSSavePanel* panel_;
};

class MacBackend final : public FileDialogBackend {
public:
	std::unique_ptr<Session> start(const FileDialogOptions& options, std::shared_ptr<FileDialogChannel> channel) override {
		using Mode = FileDialogOptions::Mode;
		NSSavePanel* panel = nil;
		if (options.mode == Mode::SaveFile) {
			NSSavePanel* save = [NSSavePanel savePanel];
			save.canCreateDirectories = YES;
			if (!options.defaultName.empty()) {
				save.nameFieldStringValue = ns(options.defaultName);
			}
			panel = save;
		} else {
			NSOpenPanel* open = [NSOpenPanel openPanel];
			open.allowsMultipleSelection = NO;
			open.canChooseFiles = options.mode == Mode::OpenFile;
			open.canChooseDirectories = options.mode == Mode::OpenFolder;
			open.canCreateDirectories = options.mode == Mode::OpenFolder;
			panel = open;
		}
		if (!options.title.empty()) {
			panel.title = ns(options.title);
			panel.message = ns(options.title);
		}
		if (!options.startDir.empty()) {
			panel.directoryURL = [NSURL fileURLWithPath:ns(options.startDir.string()) isDirectory:YES];
		}

		FeathersPanelFilter* filter = [[FeathersPanelFilter alloc] init];
		NSMutableArray<NSString*>* extensions = [NSMutableArray array];
		if (options.mode != Mode::OpenFolder) {
			for (const FileFilter& f : options.filters) {
				for (const std::string& ext : f.extensions) {
					[extensions addObject:ns(ext).lowercaseString];
				}
			}
		}
		filter.extensions = extensions;
		panel.delegate = filter;

		// THE DELEGATE IS WEAK ON THE PANEL, so the handler holds it until the panel is done.
		[panel beginWithCompletionHandler:^(NSModalResponse response) {
			(void)filter;
			FileDialogResult result;
			if (response == NSModalResponseOK && panel.URL != nil && panel.URL.fileURL) {
				result.status = FileDialogResult::Status::Chosen;
				result.path = std::filesystem::path(panel.URL.path.UTF8String);
			} else {
				result.status = FileDialogResult::Status::Cancelled;
			}
			channel->complete(std::move(result));
		}];
		[panel makeKeyAndOrderFront:nil];
		return std::make_unique<MacSession>(panel);
	}
};

} // namespace

FileDialogBackend& platformFileDialogBackend() {
	static MacBackend backend;
	return backend;
}

} // namespace feathers::ui

#endif
