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

#include "backplane/graph.h"
#include "plugin/device_registry.h"
#include "plugin/fx_chain.h"
#include "plugin/mod_rack.h"
// `kMaxDeviceSlots`, `kMaxRackSlots` and `feathersAxes()` — the pool sizes the graph is built with. The
// coordinate vocabulary this file once built its lanes from is deleted (kill-lanes §2.4b).
#include "plugin/params.h"

#include <string>
#include <vector>

// **WHAT THIS FILE ASKED, AND WHAT IT ASKS NOW THAT THE QUESTION HAS BEEN ANSWERED.**
//
// IT WAS THE GATING MEASUREMENT OF THE EDGE ARC'S STEP A: *can a `backplane::Graph` hold Feathers' two
// chains and AGREE WITH FEATHERS about which
// routes exist?* Feathers ranked lanes in BANDS — `core < rack positions < the synth's own params <
// fx positions`, `device_slots.h`'s `orderIndexOfLane` — and `mayRoute` read that ranking, so the band
// was the authority on which routes were legal. The graph's order is TOPOLOGICAL over edges and
// chain-derived precedences and `graph.h` makes *"no contiguity promise"*, so two disconnected chains
// may interleave where the band spoke unconditionally. Two different functions of one chain shape,
// one of them deciding which routes a patch could hold: a disagreement would have made adoption a
// sound-affecting change wearing a refactor's clothes, which is the shape this repo keeps paying for.
// The cases answered it before any code moved, and the graph was adopted.
//
// **THE BAND IS NOW DELETED (kill-lanes step 5), SO THERE IS NO SECOND OPINION TO AGREE WITH, AND
// EVERY CASE HERE HAS BEEN RE-AIMED RATHER THAN LEFT GREEN.** A case whose two sides have become one
// side passes by construction, which is worse than no case. What they ask instead, and neither is
// circular:
//
//   - **GATE vs ROUTE.** `route()` succeeds exactly when `wouldCycle()` says it may. A surface asks
//     the second to decide whether a knob arms and the store runs the first; they are separately
//     written, and the honesty rule is violated the moment a glow and a drag disagree. The refusals
//     are offered too, which the band-filtered version structurally could not do.
//   - **THE MOVE VERBS, at the end** — `Graph::move` and `moveRackDevice` producing the same shape
//     from the same gesture, which is a claim about two implementations and not about a rank.
//
// **AND ONE CLAIM IS RETIRED RATHER THAN RE-AIMED, WITH THE MEASUREMENT THAT RETIRED IT** — the rank
// cases. See the tombstone above `Shape`.
//
// EACH PAIR IS PUT TO A FRESH GRAPH, because a successful `route()` adds a precedence and would
// change the answer for the pairs after it. The question is about the chain shape, not about the
// order the pairs happen to be asked in.

using namespace feathers;

namespace {

/// A LANE'S DEVICE, RESOLVED THE WAY FEATHERS RESOLVES IT — the row's tag through the registry,
/// never the lane's number. `slotsOf` is the one place that reads both chains.
/// WHERE THE FIXTURE PUTS EACH DEVICE — a pool lane handed out in restore order, the strip first, the
/// way `Graph::add` hands them out. A lane is the graph's own storage handle and NOTHING here derives a
/// chain, a position or an id from one: those are read back from this table. (The fixture used to place
/// by the fx/rack coordinate bands and decode a lane back through them; the bands are deleted with the
/// rest of the address arithmetic, kill-lanes §2.4b.)
struct Placed {
	const device::Descriptor* desc = nullptr;
	DeviceInstanceId id = kNoInstance;
	size_t chain = 0;
	size_t position = 0;
};

std::vector<Placed> placementOf(const FxChain& chain, const ModRack& rack) {
	std::vector<Placed> placed;
	for (size_t slot = 0; slot < rack.count; ++slot) {
		const device::Descriptor* desc = deviceForTag(rack.device[slot].tag.data());
		REQUIRE(desc != nullptr);
		placed.push_back({ desc, static_cast<DeviceInstanceId>(placed.size() + 1), 0, slot });
	}
	for (size_t position = 0; position < chain.count; ++position) {
		const device::Descriptor* desc = deviceForTag(fxDeviceAtPosition(chain, position).row.tag.data());
		REQUIRE(desc != nullptr);
		placed.push_back({ desc, static_cast<DeviceInstanceId>(placed.size() + 1), 1, position });
	}
	return placed;
}

const device::Descriptor* descriptorAtLane(const FxChain& chain, const ModRack& rack, size_t lane) {
	const std::vector<Placed> placed = placementOf(chain, rack);
	return lane < placed.size() ? placed[lane].desc : nullptr;
}

void fill(backplane::Graph& g, const FxChain& chain, const ModRack& rack) {
	const std::vector<Placed> placed = placementOf(chain, rack);
	for (size_t lane = 0; lane < placed.size(); ++lane) {
		REQUIRE(g.restore(*placed[lane].desc, placed[lane].id, lane));
		REQUIRE(g.insert(placed[lane].chain, placed[lane].position, placed[lane].id));
	}
}

size_t edgeCapacity() {
	const backplane::Axes& axes = feathersAxes();
	return backplane::Graph::edgeCapacityFor(axes);
}

/// A NAME FOR A FAILURE TO CARRY, because a red on a pair of integers says nothing about which two
/// devices disagreed and a red on a shape says everything.
std::string where(const FxChain& chain, const ModRack& rack, size_t fromLane, size_t toLane) {
	const device::Descriptor* a = descriptorAtLane(chain, rack, fromLane);
	const device::Descriptor* b = descriptorAtLane(chain, rack, toLane);
	return std::string(a != nullptr ? a->traits.name : "?") + " (lane " + std::to_string(fromLane)
	       + ") -> " + (b != nullptr ? b->traits.name : "?") + " (lane " + std::to_string(toLane) + ")";
}

/// **THE GATE AND THE ROUTE, PAIR FOR PAIR — and neither is allowed to be the other's restatement.**
///
/// **WHAT THIS HELPER USED TO ASK IS GONE, AND SAYING SO IS THE WHOLE OF THE CHANGE (kill-lanes step
/// 5).** It offered only the pairs `sourceLaneIsUpstreamOf` admitted — Feathers' BAND, the authority
/// on which routes were legal before the graph was adopted — and checked the graph accepted every
/// one, because adopting the graph as the store would otherwise have been a silent change to which
/// routes a patch can hold. That measurement did its job: the graph was adopted, and step 4b made
/// **`wouldCycle` the whole of the rule**. So there is no second opinion left to agree with, and a
/// pre-filter written in Feathers' terms would now be this graph filtering itself — every pair
/// passing by construction, which is the shape the plan warned to retire rather than leave green.
///
/// **SO IT ASKS WHAT IS LEFT, AND THAT IS STILL TWO FUNCTIONS WIDE: `route()` succeeds exactly when
/// `wouldCycle()` says it may.** A surface asks the second (through `mayRoute`) to decide whether a
/// knob arms; the store runs the first. They are separately written — `route` tests
/// `reaches(precedenceLaneOf(d), precedenceLaneOf(s))` inline, `wouldCycle` is the named question —
/// and the honesty rule is violated the moment a glow and a drag disagree.
///
/// **AND IT OFFERS THE REFUSALS TOO, WHICH THE OLD VERSION STRUCTURALLY COULD NOT.** A pair the band
/// refused never reached the graph, so what the graph would have said about it was not a fact anybody
/// could observe. Returns how many pairs were offered and counts the refusals through `refusals`.
///
/// **THE TWO REFUSAL COUNTS ARE SEPARATE BECAUSE ONE OF THEM IS NEARLY FREE, AND A MEASUREMENT SAID
/// SO.** A device modulating its OWN control is refused by `reaches` trivially — a node reaches
/// itself — and self-pairs are most of what a small shape offers. A refusal between two DIFFERENT
/// devices is the one that takes chain adjacency to produce, so it is the one a non-vacuity guard has
/// to count. MEASURED with chain adjacency dropped from `forEachPrecedence`: the total refusal count
/// stays healthy (self-pairs carry it) while `betweenDevices` goes to zero — so a guard written on
/// the total would have passed a build with no chain order at all, which is exactly what it happened
/// to do before this split.
struct Refusals {
	size_t total = 0;
	size_t betweenDevices = 0;
};

size_t checkGateAndRouteAgree(const FxChain& chain, const ModRack& rack, Refusals* refusals = nullptr) {
	const backplane::Axes& axes = feathersAxes();
	size_t offered = 0;
	for (size_t fromLane = 0; fromLane < kMaxDeviceSlots; ++fromLane) {
		const device::Descriptor* from = descriptorAtLane(chain, rack, fromLane);
		if (from == nullptr || from->outputCount() == 0) {
			continue;
		}
		for (size_t toLane = 0; toLane < kMaxDeviceSlots; ++toLane) {
			const device::Descriptor* to = descriptorAtLane(chain, rack, toLane);
			if (to == nullptr || to->modulatableCount() == 0) {
				continue;
			}
			for (size_t output = 0; output < from->outputCount(); ++output) {
				for (size_t ordinal = 0; ordinal < to->modulatableCount(); ++ordinal) {
					backplane::InstanceCounter ids;
					backplane::Graph g(axes, 2, edgeCapacity(), ids);
					fill(g, chain, rack);
					const DeviceInstanceId a = g.nodeAtLane(fromLane).id;
					const DeviceInstanceId b = g.nodeAtLane(toLane).id;
					// BY ENDPOINT (kill-lanes step 2): the ordinal walked here names the control index the
					// graph takes.
					const backplane::Source s = backplane::Source::output(a, output);
					const backplane::Destination d =
					    backplane::Destination::control(b, to->controlOfNthModulatable(ordinal));
					// ASKED BEFORE THE ROUTE IS WRITTEN, because writing it changes the answer.
					const bool cycles = g.wouldCycle(s, d);
					++offered;
					if (cycles && refusals != nullptr) {
						++refusals->total;
						if (fromLane != toLane) {
							++refusals->betweenDevices;
						}
					}
					INFO(where(chain, rack, fromLane, toLane)
					     << ", output " << output << " -> ordinal " << ordinal << "; wouldCycle " << cycles);
					CHECK(g.route(s, d, 0.5f) == !cycles);
				}
			}
		}
	}
	return offered;
}

// **`checkRanksAgree` AND ITS TWO CASES STOOD HERE AND ARE DELETED (kill-lanes step 5), AND THE
// REASON IS A MEASUREMENT RATHER THAN A JUDGEMENT.**
//
// They compared the graph's topological rank with Feathers' BAND, pair for pair, over an appended
// shape and over shapes built by middle-removal and by move. That was the gating measurement: the
// band was the number behind every glow and every loader gate, so adopting the graph meant showing
// the two induced the same order or admitting the adoption changed which routes a patch could hold.
//
// With the band deleted the comparison has one side. The obvious re-aim — compare the graph's rank
// with the chain's own POSITION order, the record against the sort — was written, and then
// **MEASURED with a known-bad: chain adjacency deleted from `Graph::forEachPrecedence`. Four suites
// went red (gui-bridge, backplane-graph, rack-engine, gui-widgets-dahdsr) and THIS ONE STAYED GREEN,
// all ten cases.** The re-aimed rank case could not fail, and the reason is structural: kill-lanes
// step 3c mints a device's coordinate FROM its position, so lane order and position order are the
// same order by construction, and the sort's "lowest ready lane" tie-break reproduces the chain's
// order whether or not a single chain precedence exists. The shapes built to make lane and position
// disagree cannot do so any more — the case's own comment had already recorded that its
// `anyDisagreement` guard went for that reason, and this is the rest of the same fact arriving.
//
// **SO THE CLAIM IS RETIRED WHERE IT CANNOT FAIL AND THE TEETH ARE MOVED TO WHERE IT CAN**: the
// `betweenDevices` refusal guard in `checkGateAndRouteAgree` above, which the same known-bad kills.
// Chain order itself is covered by the four suites that went red.

/// A CHAIN PAIR BUILT FROM TAGS, through the same `addFxDevice`/`addRackDevice` a gesture takes — so
/// the rows carry the instance ids the chain really hands out, not ones written here. (It used to say
/// "the lanes and instance ids"; a row carries no address since kill-lanes step 3c, and the id is the
/// whole of what a device is named by.)
struct Shape {
	FxChain chain;
	ModRack rack;
};

Shape build(std::vector<const char*> rackTags, std::vector<const char*> fxTags) {
	Shape s;
	s.chain.count = 0;
	s.rack.count = 0;
	// EACH VERB'S OWN REFUSAL SENTINEL, which is what it actually returns — `kMaxRackSlots` and
	// `kMaxFxSlots`. This compared against `kNoRouteLane` while a refusal meant "no lane to give"; the
	// two happen to be equal for the fx lane and are not for the rack, so the rack half was a weaker
	// test than it read as.
	for (const char* tag : rackTags) {
		REQUIRE(addRackDevice(&s.rack, tag) != kNoPosition);
	}
	for (const char* tag : fxTags) {
		REQUIRE(addFxDevice(&s.chain, tag) != kNoPosition);
	}
	return s;
}

/// EVERY MODULATOR THE REGISTRY HOLDS, and every device that can go in the lane. Read from the
/// registry rather than listed here, so a device added tomorrow is exercised without anybody
/// remembering to add it — the trap `available_device` exists to close on the other surface.
std::vector<const char*> modulatorTags() {
	std::vector<const char*> out;
	for (const device::Descriptor& d : kDevices) {
		// A MODULATOR, NOT MERELY A SOURCE — SKLITTER is an fx that also publishes, and is registered
		// first. `audioTags()` below is where a device that renders belongs, and a tag in BOTH lists
		// would have the cases below building a shape out of one device wearing two hats.
		if (d.mod != nullptr && d.outputCount() > 0 && d.audio == nullptr) {
			out.push_back(d.traits.tag);
		}
	}
	return out;
}

std::vector<const char*> audioTags() {
	std::vector<const char*> out;
	for (const device::Descriptor& d : kDevices) {
		if (d.audio != nullptr) {
			out.push_back(d.traits.tag);
		}
	}
	return out;
}

} // namespace

TEST_CASE("the registry has both kinds to build a shape out of") {
	// THE NON-VACUITY GUARD FOR EVERY CASE BELOW, and it is separate because a registry that
	// answered nothing would make all of them pass by asking nothing. Named as its own case so the
	// red says "the registry is empty" rather than "the graph disagreed about zero pairs".
	CHECK(!modulatorTags().empty());
	CHECK(!audioTags().empty());
}

TEST_CASE("a device publishes outputs IFF it declares mod ops — the invariant the graph's source rule rests on") {
	// **WHY THIS IS THE ARC'S BUSINESS AND NOT THE REGISTRY'S.** `Graph::sourceOf` refuses an output
	// from a device whose `mod` is null — that is how the graph decides what a source IS — while
	// Feathers hands EVERY device slot `kMaxDeviceOutputs` outputs on its node (fx lanes included) and asks
	// only `outputCount()`. The two agree today
	// **only because no device separates the two flags**.
	//
	// **THE SENTENCE THAT USED TO BE QUOTED HERE IS NOW FALSE, AND CORRECTING IT IS A COMMITMENT COMMIT
	// 9700d946 MADE AND DID NOT KEEP.** It quoted `busutil_types.h` as a fact about the tree: *"every
	// `src/fx/*` descriptor says `.outputs = {}` and only `src/mod/*` devices carry `.mod`."* That
	// commit's own body said *"that comment stops being true with the outputs and is corrected in the
	// commit that adds them, not here"* — the outputs landed in 96ed8299 and the correction was missed.
	// **SKLITTER is an `src/fx/*` device with seven `.outputs` AND a `.mod` table**
	// (`sklitter_types.h`'s `kOutputs` and `kModOps`; design §4.1), so the fx/mod split that quote
	// described is gone.
	//
	// **THE CHECK BELOW IS UNTOUCHED BY THAT, WHICH IS EXACTLY WHY IT STAYED GREEN AND IS WORTH SAYING
	// OUT LOUD.** The invariant is "outputs IFF `mod`", and SKLITTER declares BOTH — it separates
	// fx-from-mod, not outputs-from-`mod`. So this case never had anything to say about the change; its
	// stated PREMISE went false underneath a passing assertion, which is the quietest way a comment rots.
	//
	// The invariant itself is still a fact nothing enforces. The day a device declares outputs without `mod` — an audio device that
	// publishes an envelope follower is the obvious one, and Bus Utility's metering card is the one in
	// flight — **Feathers admits a source the graph refuses, and the route simply stops existing.**
	// This is what turns that from a silent divergence into a red.
	//
	// IT IS ALSO WHAT MAKES THE fx-LANE CASE'S COUNT HONEST. That count was zero until SKLITTER and is
	// 3178 now; this invariant is what lets that case assert the MECHANISM behind the number rather
	// than the number itself, so neither reading can be confused with a fixture that built nothing.
	for (const device::Descriptor& d : kDevices) {
		INFO(d.traits.name << " (" << d.traits.tag << ")");
		CHECK((d.outputCount() > 0) == (d.mod != nullptr));
	}
}

TEST_CASE("FOUND BY THE FIXTURE: Feathers' two chains mint colliding instance ids, and a graph refuses the second") {
	// **ARC A'S FIRST REQUIRED COMMIT, PINNED SO IT CANNOT BE FORGOTTEN.** `fill` above renumbers the
	// ids from the lane; without a case saying WHY, that compensation reads as a fixture detail and
	// the real migration inherits a graph that refuses half the devices.
	//
	// Feathers counts per chain and both start at 1 — `device_instance.h` argues that it is *"only
	// safe because a route says which chain it means"*, the saved record carrying a distinct KIND per
	// chain — and it names its own expiry: *"the per-chain counters here stand until steps 3 and 4
	// move Feathers' chains onto the graph."* This is that day arriving.
	//
	// **THE GRAPH'S RULE IS NOT NEGOTIABLE AND SHOULD NOT BE**: `DeviceInstanceId` is the node key, so
	// two nodes under one id is two devices at one address. Vision D1 put the counter on the top-level
	// host for exactly this. What has to change is Feathers, and it is a FORMAT change — both blocks
	// carry `nextInstanceId`, and `StoredRoute`'s per-chain kind stops being needed.
	const std::vector<const char*> mods = modulatorTags();
	const std::vector<const char*> audio = audioTags();
	REQUIRE(!mods.empty());
	REQUIRE(!audio.empty());

	Shape s = build({ mods.front() }, { audio.front() });
	REQUIRE(s.rack.count == 1);
	REQUIRE(s.chain.count == 1);

	// THE COLLISION ITSELF, asserted as the VALUE rather than inferred from the refusal below — the
	// refusal has other possible causes (a live coordinate, a bad one) and would pass for the wrong
	// reason if the ids ever stopped colliding while something else broke.
	const DeviceRow& rackRow = s.rack.device[0];
	const DeviceRow& fxRow = fxDeviceAtPosition(s.chain, 0).row;
	CHECK(rackRow.instanceId == fxRow.instanceId);

	// AND THE TWO COORDINATES ARE DISJOINT, which is what makes the id the sole problem.
	//
	// **IT IS A `static_assert` BECAUSE IT IS NOW A BUILD-TIME FACT (kill-lanes step 3c).** It was a
	// `CHECK` on `rackRow.routeLane != fxRow.routeLane` — two addresses two separate pools had handed
	// out at run time, which could genuinely have collided if a pool boundary moved. There is no pool:
	// a device's coordinate is minted from its position through two constexpr functions that map into
	// disjoint bands, so a runtime `CHECK` here would be true by construction and would report a
	// measurement it did not make. Stated at the strength it actually holds, and still failing the
	// build if the bands ever overlap.
	// TWO DISTINCT POOL LANES — any two, so the refusal below is attributable to the colliding instance
	// id and not to a shared lane.
	const size_t rackCoordinate = 0;
	const size_t fxCoordinate = 1;

	// AND WHAT A GRAPH DOES WITH THE REAL ROWS.
	backplane::InstanceCounter ids;
	backplane::Graph g(feathersAxes(), 2, edgeCapacity(), ids);
	const device::Descriptor* rackDesc = deviceForTag(rackRow.tag.data());
	const device::Descriptor* fxDesc = deviceForTag(fxRow.tag.data());
	REQUIRE(rackDesc != nullptr);
	REQUIRE(fxDesc != nullptr);
	CHECK(g.restore(*rackDesc, rackRow.instanceId, rackCoordinate));
	CHECK_FALSE(g.restore(*fxDesc, fxRow.instanceId, fxCoordinate));
	CHECK(g.nodeCount() == 1);
}

TEST_CASE("gate and route agree between a strip modulator and a lane device") {
	// THE CROSS-CHAIN SHAPE. This is where the band used to speak unconditionally and the graph cannot:
	// the strip and the lane are two chains with no edge between them, so nothing orders a pair across
	// them and nothing may be asserted about one (kill-lanes step 4b).
	const std::vector<const char*> mods = modulatorTags();
	const std::vector<const char*> audio = audioTags();
	REQUIRE(!mods.empty());
	REQUIRE(!audio.empty());

	size_t offered = 0;
	Refusals refusals;
	for (const char* m : mods) {
		for (const char* a : audio) {
			Shape s = build({ m }, { a });
			offered += checkGateAndRouteAgree(s.chain, s.rack, &refusals);
		}
	}
	CHECK(offered > 0);
	// NO `betweenDevices` GUARD HERE, AND THE ZERO IS THE POINT: nothing orders a pair across two
	// disconnected chains, so every cross-chain pair is admitted and the refusals this shape produces
	// are self-routes. Asserting a refusal here would be asserting the tie-break step 4b removed.
	MESSAGE("cross-chain pairs offered: " << offered << ", refused as cycles: " << refusals.total
	                                      << " (between distinct devices: " << refusals.betweenDevices
	                                      << ")");
}

TEST_CASE("gate and route agree WITHIN the strip — and here the refusals are real") {
	// TWO MODULATORS IN ONE CHAIN: here the graph DOES have an opinion — chain 0's path orders them —
	// so both directions are offered and one of them must be refused. **That is the non-vacuity guard
	// this file needs most**, because the old version of this helper filtered the refusals out before
	// the graph ever saw them, and a gate that says yes to everything passes a check that only ever
	// offers pairs it says yes to.
	const std::vector<const char*> mods = modulatorTags();
	REQUIRE(mods.size() >= 1);

	size_t offered = 0;
	Refusals refusals;
	for (const char* first : mods) {
		for (const char* second : mods) {
			Shape s = build({ first, second }, {});
			offered += checkGateAndRouteAgree(s.chain, s.rack, &refusals);
		}
	}
	CHECK(offered > 0);
	// **THE BACKWARD PAIR, AND IT MUST BE COUNTED BETWEEN DISTINCT DEVICES.** Position 1's output into
	// position 0's control closes a loop, because position 0 precedes position 1 BY CHAIN ADJACENCY —
	// there is no other reason for it, so this number going to zero means the chain has stopped being
	// an order. **MEASURED: the first draft of this guard read the TOTAL refusal count and a build with
	// chain adjacency deleted from `forEachPrecedence` passed it**, because self-routes are refused
	// trivially and there are plenty of them. This is the whole of this suite's teeth on chain
	// adjacency; the `betweenDevices` split exists because of that measurement.
	CHECK(refusals.betweenDevices > 0);
	MESSAGE("within-strip pairs offered: " << offered << ", refused as cycles: " << refusals.total
	                                       << " (between distinct devices: " << refusals.betweenDevices
	                                       << ")");
}

TEST_CASE("gate and route agree WITHIN the fx lane") {
	// AND THE SAME QUESTION ON THE OTHER CHAIN. An fx device is a graph SOURCE only if it declares
	// `mod` ops — `Graph::sourceOf` refuses an output from a device that does not — while Feathers
	// gives every device slot its own source columns. If any lane device publishes readings without
	// declaring `mod`, this is the case that says so.
	const std::vector<const char*> audio = audioTags();
	REQUIRE(audio.size() >= 1);

	size_t offered = 0;
	for (const char* first : audio) {
		for (const char* second : audio) {
			Shape s = build({}, { first, second });
			offered += checkGateAndRouteAgree(s.chain, s.rack);
		}
	}
	// **THIS COUNT WAS ZERO UNTIL SKLITTER, AND THE COMMENT THAT SAID SO OUTLIVED THE FACT — WHICH IS
	// WHY IT IS AN ASSERTION NOW AND NOT A MESSAGE.** What stood here said: *"NOT `> 0` here,
	// deliberately … a lane of two audio devices offers a pair only if the upstream one publishes an
	// output, and MEASURED it is a real answer that none of them does."* That was true of every
	// registry this case had ever seen. **SKLITTER falsified it** — it is the first `src/fx/*` device
	// with `.outputs`, `audioTags()` selects on `d.audio != nullptr` and therefore includes it, so the
	// arrangement the old comment said did not exist is now most of what this loop builds. MEASURED at
	// the arc's tip, 2026-09-19: **3178 pairs offered**, against the 0 the comment described.
	//
	// **THE SUITE STAYED GREEN THROUGH THAT, AND THAT IS THE DEFECT.** A `MESSAGE` cannot go red, so a
	// stated measurement went stale and this case's coverage changed by three thousand pairs with
	// nothing to mark either. The count is now asserted — but NOT as `> 0` and NOT as 3178, because the
	// old comment's reasoning was right even though its number stopped being: pinning today's registry
	// as a rule is what makes the next reader delete a device and get a red that is not a defect.
	//
	// **SO IT IS ASSERTED AS THE MECHANISM, DERIVED FROM THE REGISTRY IN THE SAME BREATH.** The lane
	// offers a pair iff some audio device publishes an output. That is true of the registry before this
	// branch (no such device, 0 pairs) and after it (SKLITTER, 3178), so it is a claim that cannot go
	// stale the way the sentence it replaces did — and it is a real check: it reds if the lane stops
	// offering pairs while a publisher is still registered, which is the divergence the case above
	// pins the invariant for.
	bool anyAudioPublishes = false;
	for (const device::Descriptor& d : kDevices) {
		if (d.audio != nullptr && d.outputCount() > 0) {
			anyAudioPublishes = true;
		}
	}
	INFO("an audio device publishes outputs: " << anyAudioPublishes);
	CHECK((offered > 0) == anyAudioPublishes);
	MESSAGE("fx-lane pairs offered: " << offered);
}

TEST_CASE("a full strip and a full lane: gate and route agree, pair for pair") {
	// THE SHAPE NOTHING ELSE HERE BUILDS: every rack slot and every fx slot occupied. It was built
	// because that is where the band's arithmetic ran to its ends — fx position 6 sat on the rank that
	// collided with the 'no lane' sentinel (backlog CV). The band is deleted, so the collision is not a
	// thing that can recur; the shape is kept because a full pool is still the one that exercises every
	// precedence row and the sort's widest frontier.
	const std::vector<const char*> mods = modulatorTags();
	const std::vector<const char*> audio = audioTags();
	REQUIRE(!mods.empty());
	REQUIRE(!audio.empty());

	std::vector<const char*> rackTags;
	for (size_t i = 0; i < kMaxRackEnvelopes; ++i) {
		rackTags.push_back(mods[i % mods.size()]);
	}
	std::vector<const char*> fxTags;
	for (size_t i = 0; i < kMaxFxDevices; ++i) {
		fxTags.push_back(audio[i % audio.size()]);
	}

	Shape s = build(rackTags, fxTags);
	REQUIRE(s.rack.count == kMaxRackEnvelopes);
	REQUIRE(s.chain.count == kMaxFxDevices);
	Refusals refusals;
	const size_t offered = checkGateAndRouteAgree(s.chain, s.rack, &refusals);
	CHECK(offered > 0);
	// FOUR MODULATORS IN ONE CHAIN, so the backward pairs are here too and at the pool's widest.
	CHECK(refusals.betweenDevices > 0);
	MESSAGE("full-shape pairs offered: " << offered << ", refused as cycles: " << refusals.total
	                                     << " (between distinct devices: " << refusals.betweenDevices
	                                     << ")");
}


//-----------------------------------------------------------------------------------------------//
// THE MOVE VERBS — asked before step 5 leans on them being the same one                           //
//-----------------------------------------------------------------------------------------------//

TEST_CASE("Graph::move and moveRackDevice reorder a strip identically, for every pair") {
	// **§22.3's FIRST CHECK, AND IT IS NOT WHAT §18.2 MEASURED.** That case showed the graph's RANK
	// agrees with Feathers' band for a GIVEN shape. It said nothing about the two hosts producing the
	// same shape from the same gesture — `moveRackDevice` shifts rows through an array and
	// `Graph::move` rotates a path of node ids, and the two were written years apart in different
	// files against different types.
	//
	// Step 5 puts the graph behind the bridge's mutators, so `moveRackDevice`'s answer becomes
	// `Graph::move`'s answer. If they disagree on any pair, a hand dragging a card in the strip
	// reorders it differently after the migration — a sound-affecting change arriving as a refactor.
	//
	// **THE SIDE EFFECTS DIFFER AND THAT IS NOT WHAT IS ASKED HERE.** `Graph::move` also clears the
	// routes the new order forbids (Feathers does that separately, through
	// `clearRoutesTheOrderForbids`), and it can REFUSE on an explicit-edge cycle, which no strip has.
	// The claim under test is narrower and is the one step 5 depends on: **the resulting ORDER of
	// devices is the same sequence.**
	const std::vector<const char*> mods = modulatorTags();
	REQUIRE(!mods.empty());

	// A FULL STRIP OF DISTINGUISHABLE DEVICES. Every slot filled, so `from` and `to` range over the
	// whole path; the ids are what makes a wrong rotation visible, since the tags may repeat.
	std::vector<const char*> rackTags;
	for (size_t i = 0; i < kMaxRackEnvelopes; ++i) {
		rackTags.push_back(mods[i % mods.size()]);
	}

	size_t pairs = 0;
	for (size_t from = 0; from < kMaxRackEnvelopes; ++from) {
		for (size_t to = 0; to < kMaxRackEnvelopes; ++to) {
			Shape s = build(rackTags, {});
			REQUIRE(s.rack.count == kMaxRackEnvelopes);

			// THE GRAPH'S ANSWER. Built from the rack BEFORE the move, so both start from one shape.
			backplane::InstanceCounter ids;
			backplane::Graph g(feathersAxes(), 2, edgeCapacity(), ids);
			fill(g, s.chain, s.rack);
			// **BOTH ANSWERS ARE READ AS A SEQUENCE OF ORIGINAL POSITIONS**, which is the only shape a
			// claim about REORDERING can take: a permutation of the starting chain, stated the same way
			// on both sides so that `==` means "the same rotation" and nothing else.
			//
			// `fill` fixed each node's graph lane in restore order, and a `Graph::move` permutes the PATH
			// without disturbing a lane — so the fixture's own placement table, asked about a node's lane,
			// gives where that device began, and walking `chain(0)` reads them off in the new order. The
			// chain check beside it is not decoration: a lane the table files under the OTHER chain would
			// otherwise be read as a strip position it never held.
			const std::vector<Placed> placed = placementOf(s.chain, s.rack);
			std::vector<size_t> graphOrder;
			REQUIRE(g.move(0, from, to).has_value());
			for (const DeviceInstanceId id : g.chain(0)) {
				if (id != kNoInstance) {
					const size_t lane = g.laneOf(id);
					REQUIRE(lane < placed.size());
					REQUIRE(placed[lane].chain == 0);
					graphOrder.push_back(placed[lane].position);
				}
			}

			// FEATHERS' ANSWER, on its own copy — **joined to the graph's BY INSTANCE ID**. It was
			// joined by lane: `moveRackDevice` carried each device's `routeLane` along in its row, so
			// reading the rows' lanes off in slot order gave the permutation directly. A row carries no
			// address since kill-lanes step 3c, so the join is now the thing that actually identifies a
			// device across the move, and the search back through the pre-move rack is what turns "the
			// device now at slot i" into "the slot it came from".
			ModRack moved = s.rack;
			REQUIRE(moveRackDevice(&moved, from, to));
			std::vector<size_t> rackOrder;
			for (size_t slot = 0; slot < moved.count; ++slot) {
				size_t began = kMaxRackSlots;
				for (size_t was = 0; was < s.rack.count; ++was) {
					if (s.rack.device[was].instanceId == moved.device[slot].instanceId) {
						began = was;
					}
				}
				REQUIRE(began < kMaxRackSlots);
				rackOrder.push_back(began);
			}

			++pairs;
			INFO("move " << from << " -> " << to);
			CHECK(graphOrder == rackOrder);
		}
	}
	CHECK(pairs == kMaxRackEnvelopes * kMaxRackEnvelopes);
	MESSAGE("move pairs compared: " << pairs);
}
