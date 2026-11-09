/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#include "plugin/ui/widget_registry.h"
#include "plugin/ui/expanded_views.h"
#include "plugin/ui/generic_expanded.h"
#include "plugin/ui/sklitter_expanded.h"
#include "fx/sklitter/sklitter_types.h"

#include "device/device.h"
#include "fx/owltt/owltt_types.h"
#include "fx/harm/harm_types.h"
#include "mod/arp/arp_types.h"
#include "mod/arp/pattern_types.h"
#include "fx/featherverb/featherverb_types.h"
#include "plugin/ui/featherverb_expanded.h"
#include "plugin/ui/arp_expanded.h"
#include "plugin/ui/pattern_expanded.h"
#include "plugin/ui/owltt_expanded.h"
#include "plugin/ui/disperser_expanded.h"
#include "plugin/ui/phaser_expanded.h"
#include "plugin/ui/drawn_expanded.h"
#include "plugin/ui/eq_expanded.h"
#include "plugin/ui/spectral_expanded.h"
#include "plugin/ui/waypoint_expanded.h"
#include "plugin/ui/harm_expanded.h"
#include "plugin/ui/automod_expanded.h"
#include "plugin/ui/shaper_expanded.h"
#include "plugin/ui/parallel_rack_expanded.h"
#include "plugin/ui/delay_expanded.h"
#include "plugin/ui/comp_expanded.h"
#include "fx/comp/comp_types.h"
#include "fx/delay/delay_types.h"
#include "plugin/ui/resonate_expanded.h"
#include "fx/resonate/resonate_types.h"
#include "plugin/ui/smooth_expanded.h"

#include <memory>
#include <vector>

namespace feathers::ui {

namespace {

/// WHICH DEVICES THIS BUILD CAN OPEN, AND HOW — ONE LIST, because it was two.
///
/// THE DEFECT: no expanded view was available. OWLTT declared `hasExpanded`, `makeExpandedView`
/// had an arm for it, and no button appeared — because `buildable` was a SECOND list naming only
/// HARM, so `hasExpandedView` answered false, `OwlttCard::expandBounds` returned an empty rectangle,
/// and the mark was never drawn. The panel existed, was constructed correctly by its own tests, and
/// was unreachable.
///
/// AND THE OLD COMMENT HERE SAID *"the LIST IS THE DISPATCH — adding a device's expanded view is
/// adding an arm and nothing else."* That was false OF THE FILE IT WAS WRITTEN IN, and it is why the
/// second list went unnoticed: the note described the design rather than the code, and read as
/// permission to stop looking. A comment stating a rule is not the rule being followed — the third
/// instance this week and the first where the comment actively misled.
///
/// SO THE TWO ANSWERS COME FROM ONE TABLE. `buildable` looks a tag up; `makeExpandedView` looks the
/// same tag up and calls what it finds. A device added here gains both at once, and the two cannot
/// disagree because there is nothing left to disagree.
struct Entry {
	const char* tag;
	// EACH ENTRY TAKES BOTH ROADS AND USES THE ONE IT NEEDS. A converged panel ignores `bridge`; one
	// that has not converged returns nullptr when it is null, which is how a host without a parameter
	// road is told "not this one" rather than being handed a crash.
	std::unique_ptr<ExpandedView> (*make)(backplane::DeviceHost&, ParamBridge*, WidgetRegistry&,
	                                      DeviceInstanceId, Notifier*);
};

std::vector<Entry>& registered() {
	static std::vector<Entry> views;
	return views;
}

const Entry* entryFor(const device::Descriptor& desc) {
	for (const Entry& entry : registered()) {
		if (device::sameTag(desc.traits.tag, entry.tag)) {
			return &entry;
		}
	}
	static const Entry kEntries[] = {
		// **SKLITTER'S PANEL IS DERIVED, NOT WRITTEN.** `GenericExpanded` draws exactly the controls
		// `GenericCard` leaves off its face, so the two cannot disagree about where the line falls and
		// neither can lose one between them. That is the whole difference from a hand-written pair:
		// OWLTT's left fifteen of twenty-five with nowhere to be touched.
		{ fx::sklitter::kTraits.tag,
		  [](backplane::DeviceHost& h, ParamBridge*, WidgetRegistry& r, DeviceInstanceId s, Notifier* n)
		      -> std::unique_ptr<ExpandedView> {
			  return std::make_unique<SklitterExpanded>(h, r, s, fx::sklitter::kDescriptor, n);
		  } },
		{ fx::harm::kTraits.tag,
		  [](backplane::DeviceHost& h, ParamBridge*, WidgetRegistry& r, DeviceInstanceId s, Notifier* n)
		      -> std::unique_ptr<ExpandedView> {
			  return std::make_unique<HarmExpanded>(h, r, s, n);
		  } },
		{ fx::owltt::kTraits.tag,
		  [](backplane::DeviceHost& h, ParamBridge*, WidgetRegistry& r, DeviceInstanceId s, Notifier* n)
		      -> std::unique_ptr<ExpandedView> {
			  return std::make_unique<OwlttExpanded>(h, r, s, n);
		  } },
		{ fx::disperser::kTraits.tag,
		  [](backplane::DeviceHost& h, ParamBridge*, WidgetRegistry& r, DeviceInstanceId s, Notifier* n)
		      -> std::unique_ptr<ExpandedView> {
			  return std::make_unique<DisperserExpanded>(h, r, s, n);
		  } },
		// THE PHASER/FLANGER'S WINDOW: the response large, the explore pad and Rotate, and what the card leaves out.
		{ fx::phaser::kTraits.tag,
		  [](backplane::DeviceHost& h, ParamBridge*, WidgetRegistry& r, DeviceInstanceId s, Notifier* n)
		      -> std::unique_ptr<ExpandedView> {
			  return std::make_unique<PhaserExpanded>(h, r, s, n);
		  } },
		{ fx::drawn::kTraits.tag,
		  [](backplane::DeviceHost& h, ParamBridge*, WidgetRegistry& r, DeviceInstanceId s, Notifier* n)
		      -> std::unique_ptr<ExpandedView> {
			  return std::make_unique<DrawnExpanded>(h, r, s, n);
		  } },
		{ fx::waypoint::kTraits.tag,
		  [](backplane::DeviceHost& h, ParamBridge*, WidgetRegistry& r, DeviceInstanceId s, Notifier* n)
		      -> std::unique_ptr<ExpandedView> {
			  return std::make_unique<WaypointExpanded>(h, r, s, n);
		  } },
		{ fx::automod::kTraits.tag,
		  [](backplane::DeviceHost& h, ParamBridge*, WidgetRegistry& r, DeviceInstanceId s, Notifier* n)
		      -> std::unique_ptr<ExpandedView> {
			  return std::make_unique<AutomodExpanded>(h, r, s, n);
		  } },
		{ fx::shaper::kTraits.tag,
		  [](backplane::DeviceHost& h, ParamBridge*, WidgetRegistry& r, DeviceInstanceId s, Notifier* n)
		      -> std::unique_ptr<ExpandedView> {
			  return std::make_unique<ShaperExpanded>(h, r, s, n);
		  } },
		// THE RACK'S WINDOW (backlog NO): its own controls, and one rail per chain (`parallel_rack_expanded.h`).
		{ rack::parallel::kTraits.tag,
		  [](backplane::DeviceHost& h, ParamBridge*, WidgetRegistry& r, DeviceInstanceId s, Notifier* n)
		      -> std::unique_ptr<ExpandedView> {
			  return std::make_unique<ParallelRackExpanded>(h, r, s, n);
		  } },
		// COMP'S WINDOW (backlog QG): every control, the transfer plot, the Compander's lane (`comp_expanded.h`).
		{ fx::comp::kTraits.tag,
		  [](backplane::DeviceHost& h, ParamBridge*, WidgetRegistry& r, DeviceInstanceId s, Notifier* n)
		      -> std::unique_ptr<ExpandedView> {
			  return std::make_unique<CompExpanded>(h, r, s, n);
		  } },
		// THE DELAY'S WINDOW (delay-love S2): the time plane, every control, the FEEDBACK FX lane.
		{ fx::delay::kTraits.tag,
		  [](backplane::DeviceHost& h, ParamBridge*, WidgetRegistry& r, DeviceInstanceId s, Notifier* n)
		      -> std::unique_ptr<ExpandedView> {
			  return std::make_unique<DelayExpanded>(h, r, s, n);
		  } },
		// RESONATE'S PANEL: the card's face turned on its side — EXCITE, the string large, TONE and OUT.
		{ fx::resonate::kTraits.tag,
		  [](backplane::DeviceHost& h, ParamBridge*, WidgetRegistry& r, DeviceInstanceId s, Notifier* n)
		      -> std::unique_ptr<ExpandedView> {
			  return std::make_unique<ResonateExpanded>(h, r, s, n);
		  } },
		// THE SMOOTH SHAPER'S PANEL — both pictures large, and every control (`smooth_expanded.h`).
		{ fx::smooth::kTraits.tag,
		  [](backplane::DeviceHost& h, ParamBridge*, WidgetRegistry& r, DeviceInstanceId s, Notifier* n)
		      -> std::unique_ptr<ExpandedView> {
			  return std::make_unique<SmoothExpanded>(h, r, s, n);
		  } },
		// SPECTRAL'S WINDOW: the plot over the operators' panels, the Spectrum panel's layout (`spectral_expanded.h`).
		{ fx::spectral::kTraits.tag,
		  [](backplane::DeviceHost& h, ParamBridge*, WidgetRegistry& r, DeviceInstanceId s, Notifier* n)
		      -> std::unique_ptr<ExpandedView> {
			  return std::make_unique<SpectralExpanded>(h, r, s, n);
		  } },
		// THE EQ'S PANEL: uncapped, the plot takes the room (`eq_expanded.h`).
		{ fx::eq::kTraits.tag,
		  [](backplane::DeviceHost& h, ParamBridge*, WidgetRegistry& r, DeviceInstanceId s, Notifier* n)
		      -> std::unique_ptr<ExpandedView> {
			  return std::make_unique<EqExpanded>(h, r, s, n);
		  } },
		// FEATHERVERB'S PANEL: `GenericExpanded` over the descriptor, which seats `Size Mode`
		// (a `modeCell` Choice) as a zone knob with `Size` as its box — the IW shape, and the one the
		// design asks for — and below the grid, the conv drawer (`FeatherverbExpanded`, backlog JD).
		{ fx::featherverb::kTraits.tag,
		  [](backplane::DeviceHost& h, ParamBridge*, WidgetRegistry& r, DeviceInstanceId s, Notifier* n)
		      -> std::unique_ptr<ExpandedView> {
			  return std::make_unique<FeatherverbExpanded>(h, r, s, fx::featherverb::kDescriptor, n);
		  } },
		// THE ARP'S WINDOW (`ArpExpanded`): the step lane and the three source handles over `GenericExpanded`'s grid of every
		// control — window ⊇ card in what it shows as well as in its controls.
		{ mod::arp::kTraits.tag,
		  [](backplane::DeviceHost& h, ParamBridge*, WidgetRegistry& r, DeviceInstanceId s, Notifier* n)
		      -> std::unique_ptr<ExpandedView> {
			  return std::make_unique<ArpExpanded>(h, r, s, n);
		  } },
		// THE PATTERN DEVICE'S WINDOW (`PatternExpanded`): the tabs, the focused pattern's terrain and lane, over
		// `GenericExpanded`'s grid of every control — so the window ⊇ the card at every focus by construction.
		{ mod::arp::pat::kTraits.tag,
		  [](backplane::DeviceHost& h, ParamBridge*, WidgetRegistry& r, DeviceInstanceId s, Notifier* n)
		      -> std::unique_ptr<ExpandedView> {
			  return std::make_unique<PatternExpanded>(h, r, s, n);
		  } },
	};
	for (const Entry& entry : kEntries) {
		if (device::sameTag(desc.traits.tag, entry.tag)) {
			return &entry;
		}
	}
	return nullptr;
}

} // namespace
void registerExpandedView(const char* tag, ExpandedMaker maker) {
	if (tag == nullptr || maker == nullptr) {
		return;
	}
	for (Entry& entry : registered()) {
		if (device::sameTag(tag, entry.tag)) {
			entry.make = maker;
			return;
		}
	}
	registered().push_back(Entry { tag, maker });
}


const device::Descriptor& siteViewDescriptor() {
	static const device::Descriptor kSiteView {
		.traits = { .tag = "site", .name = "Pre / Post", .hasCard = false, .hasExpanded = true, .perVoiceControl = -1,
		            .siteLanes = device::Traits::SiteLanes::None },
		.params = {},
		.outputs = {},
	};
	return kSiteView;
}

bool hasExpandedView(const device::Descriptor& desc) {
	// BOTH HALVES, AND THE DECLARATION FIRST. A device that has not declared `hasExpanded` does not
	// get one even if an arm exists for it — the declaration is the device's own statement about its
	// presentation and this file does not overrule it.
	return desc.traits.hasExpanded && entryFor(desc) != nullptr;
}

std::unique_ptr<ExpandedView> makeExpandedView(backplane::DeviceHost& host, ParamBridge* bridge,
                                               WidgetRegistry& registry,
                                               DeviceInstanceId id, const device::Descriptor& desc,
                                               Notifier* notifier) {
	if (!hasExpandedView(desc)) {
		return nullptr;
	}
	const Entry* entry = entryFor(desc);
	// UNREACHABLE WHILE `hasExpandedView` AND THIS READ ONE TABLE, which they now do. Kept as a
	// return rather than an assert for the old reason: the failure of that agreement should be "no
	// panel opens", not a crash in front of the user.
	std::unique_ptr<ExpandedView> view = entry != nullptr ? entry->make(host, bridge, registry, id, notifier) : nullptr;
	// WHICH INSTANCE THIS PANEL IS FOR, AND THROUGH WHOM, stamped here so that no panel has to
	// remember to say — the overlay keys, places and tints by the device, and the base's
	// live-modulation walk (`ExpandedView::update`) runs only for a panel that names all three
	// (`ExpandedView::setWatched`). AND THE WINDOW HEADER'S ENABLE CHIP WRITES THROUGH THE SAME STAMP, so
	// every panel this factory builds gets the chip without one of them having to ask.
	if (view != nullptr) {
		view->setWatched(&host, &desc, id, notifier);
	}
	return view;
}

} // namespace feathers::ui
