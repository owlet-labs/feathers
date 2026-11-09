/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#include "rack/inner_chain_bundle.h"

#include <cstring>
#include <vector>

namespace feathers::rack {
namespace {

/// LITTLE-ENDIAN, BY HAND, because the rack format is and two codecs in one tree must not disagree
/// about byte order on the strength of the host they were compiled on.
void putU16(std::vector<uint8_t>& out, uint16_t v) {
	out.push_back(static_cast<uint8_t>(v & 0xFFu));
	out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
}

void putU32(std::vector<uint8_t>& out, uint32_t v) {
	out.push_back(static_cast<uint8_t>(v & 0xFFu));
	out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
	out.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
	out.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
}

/// A READER THAT CANNOT WALK OFF THE END — every get checks first and sets `bad` once.
struct Reader {
	const uint8_t* data = nullptr;
	size_t size = 0;
	size_t at = 0;
	bool bad = false;

	bool has(size_t n) const { return !bad && size - at >= n; }

	uint16_t u16() {
		if (!has(sizeof(uint16_t))) {
			bad = true;
			return 0;
		}
		const auto v = static_cast<uint16_t>(static_cast<uint16_t>(data[at])
		                                     | static_cast<uint16_t>(data[at + 1] << 8));
		at += sizeof(uint16_t);
		return v;
	}

	uint32_t u32() {
		if (!has(sizeof(uint32_t))) {
			bad = true;
			return 0;
		}
		const uint32_t v = static_cast<uint32_t>(data[at]) | (static_cast<uint32_t>(data[at + 1]) << 8)
		                   | (static_cast<uint32_t>(data[at + 2]) << 16)
		                   | (static_cast<uint32_t>(data[at + 3]) << 24);
		at += sizeof(uint32_t);
		return v;
	}
};

/// The chain a frame's lane id names, or null for one this holder does not have.
InnerChain* chainFor(std::span<const BundleLane> lanes, uint16_t id) {
	for (const BundleLane& lane : lanes) {
		if (lane.id == id) {
			return lane.chain;
		}
	}
	return nullptr;
}

/// ONE LANE'S BYTES INSIDE THE FRAME — where it starts and how long it is, resolved in the walk.
struct Found {
	InnerChain* chain = nullptr;
	const uint8_t* body = nullptr;
	size_t bytes = 0;
};

} // namespace

bool writeChainBundle(std::span<const BundleLane> lanes, const device::ByteSink& sink) {
	std::vector<uint8_t> head;
	putU32(head, kBundleMagic);
	putU16(head, kBundleVersion);
	putU16(head, static_cast<uint16_t>(lanes.size()));
	sink(head.data(), head.size());

	for (const BundleLane& lane : lanes) {
		// **EACH LANE INTO A BUFFER FIRST, BECAUSE THE LENGTH COMES BEFORE THE BYTES.** A sink is a
		// one-way write with no seek, so there is no way to leave a hole and fill it in. This is the
		// one place the bundle costs a copy, and it is per lane per save rather than per level: the
		// exponential shape `ContainerOps::saveState` warns about comes from asking a CHILD twice, and
		// nothing here asks anything twice.
		std::vector<uint8_t> body;
		bool ok = true;
		if (lane.chain != nullptr) {
			// A SINK OVER A `void*`, the vocabulary's own shape — `device::ByteSink` is a function
			// pointer and a context, not a `std::function`, so a capturing lambda cannot be one.
			const device::ByteSink into {
				&body, [](void* ctx, const uint8_t* p, size_t n) {
					auto* out = static_cast<std::vector<uint8_t>*>(ctx);
					out->insert(out->end(), p, p + n);
				}
			};
			ok = lane.chain->saveState(into);
		}
		if (!ok || body.size() > kMaxBodyBytes) {
			return false;
		}
		std::vector<uint8_t> tag;
		putU16(tag, lane.id);
		putU32(tag, static_cast<uint32_t>(body.size()));
		sink(tag.data(), tag.size());
		if (!body.empty()) {
			sink(body.data(), body.size());
		}
	}
	return true;
}

namespace {

/// **THE ONE WALK BOTH ROADS TAKE** — provisioning (`readChainBundle`) and a running instance
/// (`applyChainBundle`) differ only in the verb pass two calls, so the two passes are written once and
/// the verb is handed in. Two copies of an all-or-nothing walk are two places for "all" to drift.
template <class Apply>
bool walkChainBundle(std::span<const BundleLane> lanes, const uint8_t* data, size_t bytes, Apply&& apply) {
	if (data == nullptr || bytes == 0) {
		return false;
	}
	Reader r { data, bytes, 0, false };
	if (r.u32() != kBundleMagic || r.bad) {
		return false;
	}
	if (r.u16() != kBundleVersion || r.bad) {
		return false;
	}
	const uint16_t count = r.u16();
	if (r.bad || count > lanes.size() + 8) {
		// A COUNT NOTHING COULD HAVE WRITTEN. The slack admits a body from a build with more lanes than
		// this one has — which the skip road below then handles — without admitting a length that is
		// simply a misread of the stream as a count.
		return false;
	}

	// **PASS ONE: WALK THE WHOLE FRAME AND ASK, WITHOUT APPLYING ANYTHING.** This is what makes the
	// load all-or-nothing across several chains; see the header.
	std::vector<Found> found;
	found.reserve(count);
	for (uint16_t n = 0; n < count; ++n) {
		const uint16_t id = r.u16();
		const uint32_t len = r.u32();
		if (r.bad || len > kMaxBodyBytes || !r.has(len)) {
			return false;
		}
		const uint8_t* body = data + r.at;
		r.at += len;
		InnerChain* chain = chainFor(lanes, id);
		if (chain == nullptr) {
			continue; // a lane this build does not have, skipped BY ITS LENGTH
		}
		// AN EMPTY BODY IS A REAL ANSWER — a lane that holds nothing — and `canLoad` would refuse it,
		// so it is admitted here and applied as "leave this lane alone" below.
		if (len == 0) {
			continue;
		}
		if (!chain->canLoad(body, len)) {
			return false;
		}
		found.push_back(Found { chain, body, len });
	}
	// THE WHOLE FRAME AND NOTHING BUT — trailing bytes mean this is not the body it claims to be.
	if (r.at != bytes) {
		return false;
	}

	// **PASS TWO: APPLY.** Every lane here answered `canLoad` true a moment ago, on the same bytes and
	// at the same depth, so a refusal now would be a disagreement between `canLoad` and `loadState` —
	// which is why they are written to apply the same gates in the same order. Reported if it happens
	// rather than ignored, because a silent half-load is the state this whole arrangement exists to
	// prevent.
	bool ok = true;
	for (const Found& f : found) {
		ok = apply(*f.chain, f.body, f.bytes) && ok;
	}
	return ok;
}

} // namespace

bool readChainBundle(std::span<const BundleLane> lanes, const uint8_t* data, size_t bytes) {
	return walkChainBundle(lanes, data, bytes, [](InnerChain& chain, const uint8_t* body, size_t len) {
		return chain.loadState(body, len);
	});
}

bool applyChainBundle(std::span<const BundleLane> lanes, const uint8_t* data, size_t bytes) {
	// NOTHING AT ALL IS EVERY LANE EMPTY — `ContainerOps::applyState`'s reading of an empty body. Every
	// lane is checked before any is emptied, so this is all-or-nothing too.
	if (bytes == 0) {
		for (const BundleLane& lane : lanes) {
			if (lane.chain == nullptr || lane.chain->bridge() == nullptr) {
				return false;
			}
		}
		for (const BundleLane& lane : lanes) {
			(void)lane.chain->applyState(nullptr, 0);
		}
		return true;
	}
	return walkChainBundle(lanes, data, bytes, [](InnerChain& chain, const uint8_t* body, size_t len) {
		return chain.applyState(body, len);
	});
}

bool mapChainBundle(std::span<const uint8_t> bytes, LaneMap map, void* ctx, std::vector<uint8_t>* out) {
	Reader in { bytes.data(), bytes.size() };
	if (in.u32() != kBundleMagic || in.u16() != kBundleVersion || in.bad) {
		return false;
	}
	const uint16_t count = in.u16();
	std::vector<uint8_t> next;
	putU32(next, kBundleMagic);
	putU16(next, kBundleVersion);
	putU16(next, count);
	for (uint16_t i = 0; i < count; ++i) {
		const uint16_t id = in.u16();
		const uint32_t len = in.u32();
		if (in.bad || len > kMaxBodyBytes || !in.has(len)) {
			return false;
		}
		std::vector<uint8_t> lane;
		// AN EMPTY LANE STAYS EMPTY: there is nothing in it to name anything.
		if (len > 0 && !map(ctx, std::span<const uint8_t>(in.data + in.at, len), &lane)) {
			return false;
		}
		in.at += len;
		if (lane.size() > kMaxBodyBytes) {
			return false;
		}
		putU16(next, id);
		putU32(next, static_cast<uint32_t>(lane.size()));
		next.insert(next.end(), lane.begin(), lane.end());
	}
	if (in.at != bytes.size()) {
		return false; // the whole frame and nothing but, as the bundle's own reader says
	}
	*out = std::move(next);
	return true;
}

} // namespace feathers::rack
