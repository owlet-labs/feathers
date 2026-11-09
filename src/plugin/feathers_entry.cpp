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

// The exported clap_entry symbol, and nothing else. Kept apart from the plugin so that every format
// can have one: the CLAP exports it for a host to dlsym, the VST3 and the standalone link it
// statically and hand it to the wrapper's internal host. Everything it points at lives in the
// feathers-impl static library.

#include "plugin/feathers_entry.h"
#include "plugin/event_log.h"

namespace {

// **WHICH FORMAT THIS BINARY IS** — this file is the one translation unit compiled once per format
// (clap-wrapper's `make_clapfirst_plugins`), so it is the one place that can know, and the event log's
// `init` line says it: a sighting in a host has to say whether the instance was the CLAP or the VST3.
// clap-wrapper defines CLAP_WRAPPER_BUILD_FOR_VST3 on its VST3 target and STATICALLY_LINKED_CLAP_ENTRY on
// its standalone; neither on the CLAP itself.
#if defined(CLAP_WRAPPER_BUILD_FOR_VST3)
constexpr const char* kFormat = "vst3";
#elif defined(CLAP_WRAPPER_BUILD_FOR_AAX)
constexpr const char* kFormat = "aax";
#elif defined(STATICALLY_LINKED_CLAP_ENTRY)
constexpr const char* kFormat = "standalone";
#else
constexpr const char* kFormat = "clap";
#endif
[[maybe_unused]] const bool kFormatSet = (feathers::eventlog::setFormat(kFormat), true);

} // namespace

extern "C" {

// clap_entry is const, which GCC does not consider a candidate for a visibility attribute on some
// versions; CLAP_EXPORT carries one regardless and the export is exactly what we want here.
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wattributes"
#endif

CLAP_EXPORT const clap_plugin_entry clap_entry = {
    CLAP_VERSION_INIT,
    feathers::entryInit,
    feathers::entryDeinit,
    feathers::entryGetFactory,
};

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

} // extern "C"
