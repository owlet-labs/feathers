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

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "backplane/container_bodies.h"

#include <cstdint>
#include <vector>

// WHERE A CONTAINER'S BODY WAITS (chain-as-a-device, option A).

using namespace feathers;
// `DeviceInstanceId` and `kNoInstance` are `feathers::`, not `feathers::backplane::` — the alias
// lives beside the sentinels in device_instance_id.h and the namespace is the outer one.

namespace {

std::vector<uint8_t> bytesOf(std::initializer_list<int> vs) {
	std::vector<uint8_t> out;
	for (int v : vs) {
		out.push_back(static_cast<uint8_t>(v));
	}
	return out;
}

bool sameBytes(std::span<const uint8_t> got, const std::vector<uint8_t>& want) {
	if (got.size() != want.size()) {
		return false;
	}
	for (size_t i = 0; i < want.size(); ++i) {
		if (got[i] != want[i]) {
			return false;
		}
	}
	return true;
}

} // namespace

TEST_CASE("a body comes back to the device that owned it, byte for byte") {
	ContainerBodies held;
	const auto a = bytesOf({ 1, 2, 3, 4 });
	const auto b = bytesOf({ 9, 8 });

	CHECK(held.store(41, a.data(), a.size()));
	CHECK(held.store(42, b.data(), b.size()));
	CHECK(held.count() == 2);

	// **THE BYTES, NOT "IT STORED".** A carrier that kept the id and dropped the payload would pass
	// every count and presence check in this file.
	CHECK(sameBytes(held.bodyFor(41), a));
	CHECK(sameBytes(held.bodyFor(42), b));
}

TEST_CASE("a body is COPIED, so it outlives the stream buffer it was read from") {
	ContainerBodies held;
	std::vector<uint8_t> scratch = bytesOf({ 7, 7, 7 });
	REQUIRE(held.store(5, scratch.data(), scratch.size()));

	// THE READ BUFFER IS REUSED, which is exactly what a stream reader does with its scratch.
	scratch[0] = 0;
	scratch[1] = 0;
	scratch[2] = 0;

	CHECK(sameBytes(held.bodyFor(5), bytesOf({ 7, 7, 7 })));
}

TEST_CASE("a stored body's bytes do not move when another is stored, which one arena could not "
          "promise") {
	// **THE STRUCTURAL CLAIM THIS FILE'S DESIGN RESTS ON.** One arena with offsets would invalidate
	// this span on the next push, silently — the same shape as the pointer-identity rule: an address
	// is only an identity while the thing at it cannot be replaced. Per body per buffer makes it
	// impossible rather than documented against, and this is the case that says so.
	ContainerBodies held;
	const auto first = bytesOf({ 1, 1, 1, 1, 1, 1, 1, 1 });
	REQUIRE(held.store(10, first.data(), first.size()));

	const std::span<const uint8_t> taken = held.bodyFor(10);
	const uint8_t* address = taken.data();
	REQUIRE(address != nullptr);

	// Enough stores to force any vector-of-bytes arena to reallocate several times over.
	for (uint32_t i = 0; i < 64; ++i) {
		const auto filler = bytesOf({ 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2 });
		REQUIRE(held.store(100 + i, filler.data(), filler.size()));
	}

	CHECK(held.bodyFor(10).data() == address);
	CHECK(sameBytes(taken, first));
}

TEST_CASE("an empty body is not the same fact as no body") {
	ContainerBodies held;
	const auto nothing = bytesOf({});
	REQUIRE(held.store(3, nothing.data(), 0));

	// AN EMPTY CONTAINER SAVED AN EMPTY BODY, and that is a real answer — `ContainerOps::saveState`
	// says zero is one. A caller that read the empty span as "there was none" would load a saved
	// empty chain by leaving whatever was already there.
	CHECK(held.has(3));
	CHECK(held.bodyFor(3).empty());

	CHECK(held.has(4) == false);
	CHECK(held.bodyFor(4).empty());
	// The two questions give the same span and different answers, which is why there are two.
}

TEST_CASE("a second body for one id replaces the first, because two records naming one instance is "
          "a malformed file") {
	ContainerBodies held;
	const auto first = bytesOf({ 1, 1 });
	const auto second = bytesOf({ 2, 2, 2 });
	REQUIRE(held.store(9, first.data(), first.size()));
	REQUIRE(held.store(9, second.data(), second.size()));

	CHECK(held.count() == 1);
	CHECK(sameBytes(held.bodyFor(9), second));
}

TEST_CASE("an id that addresses nothing is refused, and so is a null with a length") {
	ContainerBodies held;
	const auto some = bytesOf({ 1 });

	CHECK(held.store(kNoInstance, some.data(), some.size()) == false);
	CHECK(held.count() == 0);

	CHECK(held.store(7, nullptr, 4) == false);
	CHECK(held.count() == 0);

	// AND THE ACCEPT PROVES THE REFUSALS ARE NOT VACUOUS — a `store` returning false unconditionally
	// would pass both checks above.
	CHECK(held.store(7, some.data(), some.size()));
	CHECK(held.count() == 1);
}

TEST_CASE("a body is given up once its container has taken it, so two cannot claim one chain") {
	ContainerBodies held;
	const auto body = bytesOf({ 4, 5, 6 });
	REQUIRE(held.store(11, body.data(), body.size()));
	REQUIRE(held.store(12, body.data(), body.size()));

	CHECK(held.erase(11));
	CHECK(held.has(11) == false);
	CHECK(held.count() == 1);

	// A SECOND ERASE FINDS NOTHING, which is what makes a provisioner that erases as it goes unable
	// to hand one body to two containers.
	CHECK(held.erase(11) == false);

	// AND THE OTHER BODY IS UNTOUCHED — an erase that cleared the lot would pass every check above.
	CHECK(held.has(12));
	CHECK(sameBytes(held.bodyFor(12), body));
}

TEST_CASE("what is left after a provisioner has drained is what nothing claimed") {
	ContainerBodies held;
	const auto body = bytesOf({ 1 });
	REQUIRE(held.store(20, body.data(), body.size()));
	REQUIRE(held.store(21, body.data(), body.size()));
	REQUIRE(held.store(22, body.data(), body.size()));

	// Two containers existed in this build; the third body came from a file written by a build with
	// a container this one lacks. That is a legitimate file, not a corrupt one.
	CHECK(held.erase(20));
	CHECK(held.erase(21));

	CHECK(held.count() == 1);
	CHECK(held.has(22));

	held.clear();
	CHECK(held.count() == 0);
}

TEST_CASE("a body's generation is never reused, across stores or across objects — no ABA") {
	// **THE COMPARISON THIS EXISTS FOR** is a provisioner deciding whether a chain whose id has not
	// changed has new contents. A per-object counter would hand a freshly loaded body the same number
	// as the body it replaced, and the rebuild that should happen would not.
	ContainerBodies first;
	const auto a = bytesOf({ 1, 2, 3 });
	REQUIRE(first.store(40, a.data(), a.size()));
	const uint64_t g1 = first.generationOf(40);
	CHECK(g1 != 0);

	// RE-STORING THE SAME ID MOVES IT, even to identical bytes — a reload is a new version whatever
	// it contains, and deciding otherwise would mean comparing bodies byte by byte to save a rebuild.
	REQUIRE(first.store(40, a.data(), a.size()));
	const uint64_t g2 = first.generationOf(40);
	CHECK(g2 > g1);

	// **A SECOND OBJECT DOES NOT START AGAIN**, which is the ABA half: a state load hands the bridge a
	// brand-new ContainerBodies.
	ContainerBodies second;
	REQUIRE(second.store(40, a.data(), a.size()));
	CHECK(second.generationOf(40) > g2);

	// AND NO BODY IS GENERATION ZERO, which is reserved for "none held".
	CHECK(first.generationOf(41) == 0);
}
