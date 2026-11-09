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

#include "backplane/device_host.h"
#include "fx/filter/filter_types.h"
#include "mod/dahdsr/dahdsr_types.h"
#include "plugin/ui/device_control.h"

#include <algorithm>
#include <array>
#include <vector>

// A DEVICE'S CONTROL, DRIVEN BY A HOST THAT IS NOT FEATHERS.
//
// THIS FILE IS THE POINT OF THE WHOLE EXTRACTION, and the host below is why. `TinyHost` implements
// `backplane::DeviceHost` over a list of routes and a couple of arrays — about forty lines, no
// `ParamBridge`, no `ParamSet`, no clap ids, no synth. If a card's control behaves correctly against
// it, the card layer is genuinely portable; if it does not, everything above this is a story.
//
// IT IS ALSO THE SECOND IMPLEMENTOR the modulation interface was supposed to be designed against.
// `device.h` set that as the condition for writing the contract at all, and until now Feathers has
// been the only one — so every rule in it has been, strictly, a rule with one example.

using namespace feathers;
namespace filter = feathers::fx::filter;
namespace dahdsr = feathers::mod::dahdsr;

namespace {

constexpr backplane::Axes kAxes {
	.coreSources = 8,      // the rack's macros
	.coreDestinations = 0, // and no parameters of its own
	.deviceSlots = 4,
	.outputStride = 4,
};

/// THE DEVICES THIS HOST HOLDS, BY ID — deliberately NOT 0, 1, 2: an id is not an index, and a double
/// whose ids happened to equal its array slots could not tell a control that confused the two.
constexpr DeviceInstanceId kFilter = 7;
constexpr DeviceInstanceId kEnvelope = 11;
/// AN ID THIS HOST HOLDS NOTHING UNDER.
constexpr DeviceInstanceId kNobody = 42;

/// THE FIXTURE'S OWN "no order" MARK. It was `DeviceView::kUnranked`, which went with the rank rule at
/// kill-lanes step 4b; the fixture still keeps ranks because a rank is the easiest way for a TEST to
/// state an order, and derives the relation the authority now asks for from them.
constexpr size_t kNoRank = static_cast<size_t>(-1);

/// A HOST IN FORTY LINES. It holds a filter and a DAHDSR, each under its id, and keeps them in slots
/// of its own that no caller ever sees.
class TinyHost final : public backplane::DeviceHost {
public:
	TinyHost() {
		ids_[0] = kFilter;
		at_[0] = &filter::kDescriptor;
		ids_[1] = kEnvelope;
		at_[1] = &dahdsr::kDescriptor;
		// THE FLOW: the modulator first, then the effect it reaches — the routing rule's input
		// Without ranks this host's `mayRoute` would refuse every device-to-device
		// pair, by design, and every routing case below would go vacuous rather than red.
		rank_[1] = 0;
		rank_[0] = 1;
		for (auto& row : field_) {
			row.fill(0.0f);
		}
	}

	using backplane::DeviceHost::fieldAt;
	using backplane::DeviceHost::setField;
	using backplane::DeviceHost::fieldsPresentAt;

	const backplane::Axes& axes() const noexcept override { return kAxes; }
	backplane::DeviceView devices() const noexcept override {
		return { this,
			     [](const void* h, DeviceInstanceId id) -> const device::Descriptor* {
				     const auto* self = static_cast<const TinyHost*>(h);
				     const size_t at = self->slotOf(id);
				     return at < self->at_.size() ? self->at_[at] : nullptr;
			     },
			     // **A CYCLE RATHER THAN A RANK COMPARISON (kill-lanes step 4b).** The fixture still keeps a
			     // rank per slot, because a rank is the easiest way for a TEST to state an order; the
			     // question the authority asks is now the relation, so the fixture derives it here rather
			     // than handing over a number the rule would have to interpret.
			     [](const void* h, backplane::Source s, backplane::Destination d) {
				     const auto* self = static_cast<const TinyHost*>(h);
				     if (!s.isDevice() || !d.isDevice()) {
					     return false;
				     }
				     const size_t from = self->slotOf(s.device);
				     const size_t to = self->slotOf(d.device);
				     if (from >= self->rank_.size() || to >= self->rank_.size()) {
					     return true; // a device the host does not hold cannot be ordered, so refuse
				     }
				     const size_t rf = self->rank_[from];
				     const size_t rt = self->rank_[to];
				     return rf == kNoRank || rt == kNoRank || rf >= rt;
			     } };
	}

	float fieldAt(DeviceInstanceId device, size_t control) const override {
		return field_[slotOf(device)][control];
	}
	void setField(DeviceInstanceId device, size_t control, float value) override {
		field_[slotOf(device)][control] = value;
		++writes;
	}
	size_t fieldsPresentAt(DeviceInstanceId device) const override { return present_[slotOf(device)]; }

	/// A FIXED RATE. Nothing this double is asked about depends on it — but it is ANSWERED rather than
	/// left to a default, because `DeviceHost` is the seam under test and a double that quietly
	/// disagrees with the contract is a double testing a different object.
	double sampleRate() const noexcept override { return 48000.0; }

	/// NO READINGS. This double is about fields and routes; a device that publishes nothing answers
	/// zero, which is the same answer a host with no data gives — and a caller cannot tell them apart
	/// by design, because a meter with no data and a device with no meters both draw as nothing.
	size_t readingCountAt(DeviceInstanceId) const noexcept override { return 0; }
	float readingAt(DeviceInstanceId, size_t) const noexcept override { return 0.0f; }

	/// THE ROUTES, AS A LIST OF EDGES — every route is an edge between endpoints (kill-lanes step 2).
	float routeAmount(backplane::Source s, backplane::Destination d) const override {
		const auto it = std::find_if(routes_.begin(), routes_.end(),
		                             [&](const backplane::Route& r) { return r.source == s && r.destination == d; });
		return it != routes_.end() ? it->amount : 0.0f;
	}
	void setRouteAmount(backplane::Source s, backplane::Destination d, float a) override {
		std::erase_if(routes_, [&](const backplane::Route& r) { return r.source == s && r.destination == d; });
		if (a != 0.0f) {
			routes_.push_back({ s, d, a });
		}
	}
	bool routedElsewhere(backplane::Destination d, backplane::Source except) const override {
		return std::any_of(routes_.begin(), routes_.end(),
		                   [&](const backplane::Route& r) { return r.destination == d && r.source != except; });
	}
	void beginRouteGesture() override { ++gesturesOpen; }
	void endRouteGesture() override { ++gesturesClosed; }
	void forEachRouteFrom(backplane::Source s, void* ctx, void (*f)(void*, backplane::Destination)) const override {
		for (const backplane::Route& r : routes_) {
			if (r.source == s) {
				f(ctx, r.destination);
			}
		}
	}
	std::string destinationName(backplane::Destination) const override { return "Route"; }

	void setPresent(DeviceInstanceId device, size_t n) { present_[slotOf(device)] = n; }

	int writes = 0;
	int gesturesOpen = 0;
	int gesturesClosed = 0;

private:
	/// THIS HOST'S OWN POOL INDEX FOR AN ID — its business alone, and `kSlots` for one it does not hold
	/// (which indexes the spare row every array keeps, so an unknown id reads zeros rather than faulting).
	static constexpr size_t kSlots = 2;
	size_t slotOf(DeviceInstanceId id) const noexcept {
		for (size_t i = 0; i < kSlots; ++i) {
			if (ids_[i] == id) {
				return i;
			}
		}
		return kSlots;
	}

	std::array<DeviceInstanceId, kSlots> ids_ {};
	std::array<const device::Descriptor*, kSlots + 1> at_ {};
	std::array<size_t, kSlots + 1> rank_ { kNoRank, kNoRank,
		                                   kNoRank };
	std::array<std::array<float, 128>, kSlots + 1> field_ {};
	std::array<size_t, kSlots + 1> present_ { 128, 128, 128 };
	std::vector<backplane::Route> routes_;
};

/// The filter's first modulatable control, by index into its own declarations.
size_t firstModulatable(const device::Descriptor& d) {
	for (size_t i = 0; i < d.params.size(); ++i) {
		if (d.params[i].modulatable) {
			return i;
		}
	}
	return d.params.size();
}

} // namespace

TEST_CASE("a control reads and writes its device's field through the host") {
	TinyHost host;
	const size_t control = firstModulatable(filter::kDescriptor);
	ui::DeviceControl c(host, kFilter, filter::kDescriptor, control);

	c.setValue(0.25);
	REQUIRE(host.writes == 1);
	REQUIRE(c.value() == doctest::Approx(0.25));
	REQUIRE(host.fieldAt(kFilter, control) == doctest::Approx(0.25f));
}

TEST_CASE("an absent field reads as the control's DECLARED DEFAULT, never as zero") {
	// device_row.h's rule, and the reason `fieldsPresentAt` is on the host contract at all: a patch
	// written by a build that declared fewer controls loads with the rest ABSENT, and absent is not
	// the same fact as zero.
	TinyHost host;
	// A CONTROL WHOSE DEFAULT IS NOT ZERO, found rather than assumed. The first draft took the
	// descriptor's LAST control and asserted its default was non-zero; the filter's happens to be 0,
	// so the case could not have told "absent" from "zero" — which is the only thing it is for. The
	// premise assertion caught that, which is why it is written as one.
	size_t control = filter::kDescriptor.params.size();
	for (size_t i = 0; i < filter::kDescriptor.params.size(); ++i) {
		if (filter::kDescriptor.params[i].def != 0.0) {
			control = i;
			break;
		}
	}
	REQUIRE(control < filter::kDescriptor.params.size());
	const double declared = filter::kDescriptor.params[control].def;
	REQUIRE(declared != 0.0);

	host.setPresent(kFilter, control); // the row stops one short of this control
	ui::DeviceControl c(host, kFilter, filter::kDescriptor, control);
	REQUIRE(c.value() == doctest::Approx(declared));

	host.setPresent(kFilter, control + 1); // now it reaches
	REQUIRE(c.value() == doctest::Approx(0.0));
}

TEST_CASE("a host with no right-ear channel draws every control mono: stereo is the declaration, never live") {
	// **BACKLOG KU, THE DEFAULTS:** `DeviceHost::modulatedFieldRAt` answers false unless overridden, so a third-party
	// host's rings draw as they always have — `modulatedR` is `modulated`, and only the capability shows.
	TinyHost host;
	const size_t cutoff = static_cast<size_t>(filter::Param::Cutoff);
	REQUIRE(filter::kDescriptor.params[cutoff].stereo);
	const device::ModDisplay shown = ui::DeviceControl(host, kFilter, filter::kDescriptor, cutoff).display();
	CHECK(shown.isDestination);
	CHECK(shown.stereo);
	CHECK_FALSE(shown.stereoLive);
	CHECK(shown.modulatedR == shown.modulated);
	const size_t attack = firstModulatable(dahdsr::kDescriptor);
	REQUIRE(attack < dahdsr::kDescriptor.params.size());
	const device::ModDisplay mono = ui::DeviceControl(host, kEnvelope, dahdsr::kDescriptor, attack).display();
	CHECK_FALSE(mono.stereo);
	CHECK_FALSE(mono.stereoLive);
	CHECK(mono.modulatedR == mono.modulated);
}

TEST_CASE("a modulatable control is a destination and a non-modulatable one is not") {
	TinyHost host;
	const device::Descriptor& d = filter::kDescriptor;
	size_t checkedMod = 0;
	size_t checkedPlain = 0;
	for (size_t i = 0; i < d.params.size(); ++i) {
		ui::DeviceControl c(host, kFilter, d, i);
		const bool isDest = c.display().isDestination;
		REQUIRE(isDest == d.params[i].modulatable);
		(d.params[i].modulatable ? checkedMod : checkedPlain)++;
	}
	REQUIRE(checkedMod > 0);
	REQUIRE(checkedPlain > 0); // both kinds must exist, or this proved half a thing
}

TEST_CASE("a control on an id the host does not hold is not a destination") {
	TinyHost host;
	ui::DeviceControl c(host, kNobody, filter::kDescriptor, firstModulatable(filter::kDescriptor));
	REQUIRE(host.descriptorOf(kNobody) == nullptr);
	REQUIRE_FALSE(c.display().isDestination);
}

TEST_CASE("an amount written through the control reaches the host's table") {
	TinyHost host;
	ui::ModState mod;
	const size_t control = firstModulatable(filter::kDescriptor);
	ui::DeviceControl c(host, kFilter, filter::kDescriptor, control, &mod, nullptr);

	mod.focus(backplane::Source::core(0));
	c.setAmount(0.5f);
	REQUIRE(host.routeAmount(backplane::Source::core(0), c.destination()) == doctest::Approx(0.5f));
	REQUIRE(c.display().amount == doctest::Approx(0.5f));
}

TEST_CASE("armed and routable are one answer, and a device source must be live to reach anything") {
	TinyHost host;
	ui::ModState mod;
	ui::DeviceControl c(host, kFilter, filter::kDescriptor, firstModulatable(filter::kDescriptor), &mod);

	// Unarmed: never routable, whatever else is true.
	REQUIRE(c.display().isDestination);
	REQUIRE_FALSE(c.display().routable);

	mod.arm(true);
	mod.focus(backplane::Source::core(0)); // a macro — always live
	REQUIRE(c.display().routable);

	// A source belonging to a device the host does not hold is not live, so nothing may be routed from it.
	mod.focus(backplane::Source::output(kNobody, 0));
	REQUIRE_FALSE(c.display().routable);

	// One belonging to the DAHDSR is.
	mod.focus(backplane::Source::output(kEnvelope, 0));
	REQUIRE(c.display().routable);
}

TEST_CASE("routedElsewhere discloses a route the armed source did not make") {
	// The honesty rule read from the other end: a control must not stay silent about what is being
	// done TO it. Found in Feathers by a ten-second envelope nobody could see the source of.
	TinyHost host;
	ui::ModState mod;
	ui::DeviceControl c(host, kFilter, filter::kDescriptor, firstModulatable(filter::kDescriptor), &mod);
	const backplane::Destination dest = c.destination();

	mod.focus(backplane::Source::core(1));
	REQUIRE_FALSE(c.display().routedElsewhere);

	host.setRouteAmount(backplane::Source::core(0), dest, 0.4f); // a DIFFERENT source reaches it
	REQUIRE(c.display().routedElsewhere);
	REQUIRE(c.display().amount == doctest::Approx(0.0f)); // and the focused one still reads nothing

	mod.focus(backplane::Source::core(0)); // focus the source that made it
	REQUIRE_FALSE(c.display().routedElsewhere);
	REQUIRE(c.display().amount == doctest::Approx(0.4f));
}

TEST_CASE("an amount gesture is bracketed on the host") {
	TinyHost host;
	ui::ModState mod;
	ui::DeviceControl c(host, kFilter, filter::kDescriptor, firstModulatable(filter::kDescriptor), &mod);
	c.beginAmountGesture();
	c.setAmount(0.3f);
	c.endAmountGesture();
	REQUIRE(host.gesturesOpen == 1);
	REQUIRE(host.gesturesClosed == 1);
}

TEST_CASE("a value gesture records one undo step, and none for a gesture that moved nothing") {
	TinyHost host;
	ui::EditHistory history;
	const size_t control = firstModulatable(filter::kDescriptor);
	ui::DeviceControl c(host, kFilter, filter::kDescriptor, control, nullptr, &history);

	c.beginGesture();
	c.setValue(0.75);
	c.endGesture();
	REQUIRE(history.canUndo());

	history.undo();
	REQUIRE(c.value() == doctest::Approx(0.0));
	history.redo();
	REQUIRE(c.value() == doctest::Approx(0.75));

	// A gesture that changes nothing records nothing.
	const bool couldUndo = history.canUndo();
	c.beginGesture();
	c.endGesture();
	REQUIRE(history.canUndo() == couldUndo);
}

TEST_CASE("the control prints from its declared unit") {
	TinyHost host;
	ui::DeviceControl c(host, kFilter, filter::kDescriptor, firstModulatable(filter::kDescriptor));
	c.setValue(0.5);
	REQUIRE_FALSE(c.text().empty());
}
