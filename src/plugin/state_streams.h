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

// THE HOST'S STATE STREAMS, WRAPPED — for the two things a state road needs and a bare `clap_istream` cannot
// give: the WHOLE patch in hand before anything is committed (so a load is all-or-nothing, see
// `FeathersPlugin::loadWalk`), and a byte count and a failure position for the event log.
//
// Main thread only: `clap_plugin_state` is `[main-thread]`, and the buffer allocates.

#include "clap/clap.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace feathers {

/// Bigger than any patch this build writes, by a wide margin: a cap, so a host stream that never ends
/// cannot take the process with it.
inline constexpr size_t kMaxStateBytes = 256u * 1024u * 1024u;

/// Every byte of `stream`, to its end. False on a read error or a stream past `kMaxStateBytes`.
inline bool readWholeStream(const clap_istream* stream, std::vector<uint8_t>* out) {
	out->clear();
	uint8_t chunk[16384];
	for (;;) {
		const int64_t n = stream->read(stream, chunk, sizeof(chunk));
		if (n < 0) {
			return false;
		}
		if (n == 0) {
			return true;
		}
		out->insert(out->end(), chunk, chunk + n);
		if (out->size() > kMaxStateBytes) {
			return false;
		}
	}
}

/// **A BUFFERED PATCH, READ AS A STREAM** — with the position and whether a read was ever cut short by the
/// end, which is how a refusal is told apart: "ran out of bytes" (truncated) against "read what it wanted
/// and did not like it" (a block's own refusal).
struct BufferIStream {
	explicit BufferIStream(const std::vector<uint8_t>& bytes) : data(bytes) {
		stream.ctx = this;
		stream.read = &BufferIStream::readFn;
	}
	BufferIStream(const BufferIStream&) = delete;
	BufferIStream& operator=(const BufferIStream&) = delete;

	const std::vector<uint8_t>& data;
	size_t at = 0;
	bool hitEnd = false; ///< a read asked for more than was left
	clap_istream_t stream {};

	static int64_t CLAP_ABI readFn(const clap_istream* s, void* buffer, uint64_t size) {
		auto* self = static_cast<BufferIStream*>(s->ctx);
		const size_t left = self->data.size() - self->at;
		if (size > left) {
			self->hitEnd = true;
		}
		const size_t n = static_cast<size_t>(std::min<uint64_t>(size, left));
		if (n > 0) {
			std::memcpy(buffer, self->data.data() + self->at, n);
			self->at += n;
		}
		return static_cast<int64_t>(n);
	}
};

/// **A SAVE SERIALISED INTO MEMORY FIRST** — the buffer a whole state is built in before any of it reaches the
/// host, so a part that refuses writes nothing there (the atomic save).
struct MemOStreamForState {
	explicit MemOStreamForState(std::vector<uint8_t>* target) : bytes(target) {
		stream.ctx = this;
		stream.write = &MemOStreamForState::writeFn;
	}
	MemOStreamForState(const MemOStreamForState&) = delete;
	MemOStreamForState& operator=(const MemOStreamForState&) = delete;

	std::vector<uint8_t>* bytes;
	clap_ostream_t stream {};

	static int64_t CLAP_ABI writeFn(const clap_ostream* s, const void* buffer, uint64_t size) {
		auto* self = static_cast<MemOStreamForState*>(s->ctx);
		const auto* from = static_cast<const uint8_t*>(buffer);
		self->bytes->insert(self->bytes->end(), from, from + size);
		return static_cast<int64_t>(size);
	}
};

/// **THE HOST'S OUTPUT STREAM, COUNTED** — what a save actually handed over, for the log.
struct CountingOStream {
	explicit CountingOStream(const clap_ostream* target) : inner(target) {
		stream.ctx = this;
		stream.write = &CountingOStream::writeFn;
	}
	CountingOStream(const CountingOStream&) = delete;
	CountingOStream& operator=(const CountingOStream&) = delete;

	const clap_ostream* inner;
	uint64_t written = 0;
	clap_ostream_t stream {};

	static int64_t CLAP_ABI writeFn(const clap_ostream* s, const void* buffer, uint64_t size) {
		auto* self = static_cast<CountingOStream*>(s->ctx);
		const int64_t n = self->inner->write(self->inner, buffer, size);
		if (n > 0) {
			self->written += static_cast<uint64_t>(n);
		}
		return n;
	}
};

/// **THE HOST'S INPUT STREAM, COUNTED** — for a road that reads the stream itself (the rack's `parseRack`).
struct CountingIStream {
	explicit CountingIStream(const clap_istream* source) : inner(source) {
		stream.ctx = this;
		stream.read = &CountingIStream::readFn;
	}
	CountingIStream(const CountingIStream&) = delete;
	CountingIStream& operator=(const CountingIStream&) = delete;

	const clap_istream* inner;
	uint64_t read = 0;
	clap_istream_t stream {};

	static int64_t CLAP_ABI readFn(const clap_istream* s, void* buffer, uint64_t size) {
		auto* self = static_cast<CountingIStream*>(s->ctx);
		const int64_t n = self->inner->read(self->inner, buffer, size);
		if (n > 0) {
			self->read += static_cast<uint64_t>(n);
		}
		return n;
	}
};

} // namespace feathers
