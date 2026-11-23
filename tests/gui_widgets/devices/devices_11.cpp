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

// PIECE 11 OF 18 of the devices suite: its cases in their original order, with the helpers only they use. See devices_common.h.

#include "devices/devices_common.h"

namespace {
/// ONE MODULATOR CARD UNDER TEST, built on the recording host at the strip's own width — the only
/// width these cards are laid out at (`ModPanel::kCardWidth`, and `laneExtent` answers the same number
/// horizontally), so it is both the default and the minimum.
template <typename Card>
struct GgCard {
	ModFixture f;
	RecordingDeviceHost host { f.deviceHost };
	feathers::ui::ModState mod;
	feathers::ui::EditHistory history;
	DeviceInstanceId id = kNoInstance;
	std::unique_ptr<Card> card;

	explicit GgCard(const char* tag) {
		size_t slot = 0; // the strip's pinned envelope
		if (tag != nullptr) {
			slot = f.bridge.addRackDevice(tag);
			REQUIRE(slot < kMaxRackSlots);
			f.bridge.refresh();
		}
		id = f.bridge.rackDeviceAt(slot);
		card = std::make_unique<Card>(host, id, mod, history);
		card->setBounds(0, 0, static_cast<int>(ui::ModPanel::kCardWidth),
		                static_cast<int>(card->laneHeightAt(ui::ModPanel::kCardWidth)));
		card->resized();
	}
};
/// THE CLAIMS EVERY CARD SHARES, asked of one card. `desc` is the device's descriptor, so the list is
/// checked against the DECLARATION rather than against what the card happens to print.
template <typename Card>
void checkChoicesAreMenus(GgCard<Card>& g, const device::Descriptor& desc) {
	Card& card = *g.card;
	for (size_t i = 0; i < Card::kChoiceCount; ++i) {
		const auto param = static_cast<size_t>(Card::choices()[i]);
		const device::ParamDesc& declared = desc.params[param];
		CAPTURE(std::string(declared.name));
		ui::ValueBox* box = card.choiceBoxForTest(i);
		REQUIRE(box != nullptr);

		// A BOX OVER THE DECLARED CONTROL, and one that opens a list rather than taking a drag.
		CHECK(&box->control().desc() == &declared);
		CHECK(box->choiceMenuAvailable());
		CHECK(box->entryCount() == declared.names.size());
		CHECK(box->isVisible());

		// NO CHIP CARRIES IT, and every chip that remains is a two-state switch — the rule's own
		// table: a switch passes because *next* and *which* are the same thing.
		for (const auto& chip : Card::chips()) {
			const device::ParamDesc& chipDesc = desc.params[static_cast<size_t>(chip.param)];
			CAPTURE(std::string(chipDesc.name));
			CHECK(static_cast<size_t>(chip.param) != param);
			CHECK(chipDesc.max - chipDesc.min == doctest::Approx(1.0));
		}

		// IN ITS CELL, AND A PRESS THERE REACHES IT — so the press opens the menu and not the card's
		// old chip road.
		const visage::Bounds cell = card.choiceBounds(i);
		CHECK(box->x() >= cell.x());
		CHECK(box->right() <= cell.right());
		CHECK(box->y() >= cell.y());
		CHECK(box->bottom() <= cell.bottom());
		const visage::Point centre { cell.x() + cell.width() * 0.5f, cell.y() + cell.height() * 0.5f };
		CHECK(card.frameAtPoint(centre) == static_cast<visage::Frame*>(box));
		// AND THE ROW STILL FITS THE CARD — the height is unchanged, so the cell ends above the foot.
		CHECK(cell.bottom() <= card.bodyTop() + Card::height());

		// THE MENU LISTS EVERY DECLARED CHOICE, in order, with the ids a pick hands back.
		visage::PopupMenu menu = box->choiceMenu();
		REQUIRE(static_cast<size_t>(menu.size()) == declared.names.size());
		for (size_t row = 0; row < declared.names.size(); ++row) {
			const std::string name = menu.options()[row].name().toUtf8();
			CAPTURE(name);
			CHECK(menu.options()[row].id() == static_cast<int>(row));
			const std::string declaredName = declared.names[row];
			REQUIRE(name.size() >= declaredName.size());
			CHECK(name.substr(name.size() - declaredName.size()) == declaredName);
		}

		// EVERY FACE FITS ITS CELL at this width — the one number the layout can get wrong.
		const visage::Font face = ui::font(ui::kKnobReadoutSize);
		for (size_t v = 0; v < declared.names.size(); ++v) {
			g.host.setField(g.id, param, static_cast<float>(v));
			const std::string text = box->readout();
			CAPTURE(text);
			CHECK(face.stringWidth(std::u32string(text.begin(), text.end())) <= box->readoutBox().width);
		}
		g.host.setField(g.id, param, static_cast<float>(declared.def));

		// AND EVERY CHIP'S CAPTION FITS ITS CHIP, inside the one-pixel border — the chips share this row, and a fourth
		// (Carry, backlog MM) is what made them narrower.
		const visage::Font chipFace = ui::font(ui::kKnobLabelSize);
		for (size_t c = 0; c < Card::kChipCount; ++c) {
			const std::string label = card.chipLabel(c);
			CAPTURE(label);
			const float width = chipFace.stringWidth(std::u32string(label.begin(), label.end()));
			CHECK(width <= card.chipBounds(c).width() - 2.0f);
			// AND ON THE CARD, clear of every other chip and choice — a chip pushed past the row's end fits its own
			// caption perfectly well while a hand cannot find it.
			const visage::Bounds b = card.chipBounds(c);
			CHECK(b.x() >= 0.0f);
			CHECK(b.right() <= static_cast<float>(ui::ModPanel::kCardWidth));
			for (size_t other = 0; other < Card::kChipCount; ++other) {
				if (other != c) {
					CHECK_FALSE(b.overlaps(card.chipBounds(other)));
				}
			}
			for (size_t k = 0; k < Card::kChoiceCount; ++k) {
				CHECK_FALSE(b.overlaps(card.choiceBounds(k)));
			}
		}

		// MODULATION AND THE ARM GLOW AS A GENERIC CARD'S BOX OVER THE SAME CONTROL HAS THEM — the
		// precedent's reading of the same declaration, asked of the same host.
		ui::GenericCard generic { g.host, g.id, desc, nullptr };
		const ui::ValueBox* reference = nullptr;
		for (const auto& b : generic.boxesForTest()) {
			if (&b->control().desc() == &declared) {
				reference = b.get();
			}
		}
		REQUIRE(reference != nullptr);
		CHECK(reference->choiceMenuAvailable());
		g.mod.focus(backplane::Source::output(g.id, 0));
		g.mod.arm(true);
		CHECK(box->control().display().isDestination == reference->control().display().isDestination);
		CHECK(box->control().display().isDestination == declared.modulatable);
		g.mod.arm(false);
	}
}
/// A PICK WRITES THE VALUE PICKED, ONCE, AND NOTHING ELSE — from every start to every target, through
/// the menu's own `onSelection`, which is the call `visage::PopupMenuFrame::optionSelected` makes.
/// The walk from Once back to Loop is the one the chip made through Free, LFO Retrig and Gate.
template <typename Card>
void checkPicksWriteOnce(GgCard<Card>& g, const device::Descriptor& desc) {
	Card& card = *g.card;
	for (size_t i = 0; i < Card::kChoiceCount; ++i) {
		const auto param = static_cast<size_t>(Card::choices()[i]);
		const device::ParamDesc& declared = desc.params[param];
		CAPTURE(std::string(declared.name));
		ui::ValueBox* box = card.choiceBoxForTest(i);
		REQUIRE(box != nullptr);
		const size_t count = declared.names.size();
		for (size_t from = 0; from < count; ++from) {
			for (size_t to = 0; to < count; ++to) {
				if (from == to) {
					continue;
				}
				CAPTURE(from);
				CAPTURE(to);
				g.host.setField(g.id, param, static_cast<float>(from));
				g.host.writes.clear();
				const size_t steps = g.history.length();

				box->choiceMenu().onSelection().callback(static_cast<int>(to));

				// EXACTLY ONE WRITE, TO THIS CONTROL, OF THE VALUE PICKED.
				REQUIRE(g.host.writes.size() == 1);
				CHECK(g.host.writes[0].device == g.id);
				CHECK(g.host.writes[0].control == param);
				CHECK(g.host.writes[0].value == static_cast<float>(to));
				CHECK(g.host.fieldAt(g.id, param) == static_cast<float>(to));
				// AND ONE UNDO STEP, which puts back where it started — redone, so the history only
				// grows and the next pick's count is measured from a clean end.
				CHECK(g.history.length() == steps + 1);
				g.history.undo();
				CHECK(g.host.fieldAt(g.id, param) == static_cast<float>(from));
				g.history.redo();
				CHECK(g.host.fieldAt(g.id, param) == static_cast<float>(to));
			}
		}
	}
}
} // namespace

TEST_CASE("RESONATE's drone chips come from the drone's strings and note chips from the heads, the drone's the quieter kind") {
	// THE CHORD CONTRACT (67504816): `heads` carry the played notes only; the drone's strings are their own
	// fields. Each kind of chip is read from its own fields, played notes first, and a drone chip says it is one.
	ResonateFixture d(rsn::kTraits.tag);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(d.f.deviceHost, &d.f.bridge, registry, d.device, rsn::kDescriptor);
	auto* panel = dynamic_cast<ui::ResonateExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	const visage::Point min = panel->minimumSize();
	panel->setBounds(0.0f, 0.0f, min.x, min.y);
	rsn::Reading r = syntheticReading(rsn::Model::Ring);
	r.headNotes = {};
	r.headGains = {};
	r.headNotes[0] = 64.0f; // a played E
	r.headGains[0] = 0.8f;
	r.headNotes[1] = 67.0f; // and a played G
	r.headGains[1] = 0.5f;
	// A MAJOR CHORD ON C2 (four strings): three sounding, the fifth silent — and a stale fifth slot beyond
	// the chord's count that must not be read.
	r.droneCount = 4.0f;
	r.droneNotes = { 48.0f, 52.0f, 55.0f, 60.0f, 72.0f, 0.0f };
	r.droneGains = { 0.3f, 0.2f, 0.0f, 0.1f, 0.9f, 0.0f };
	publishReading(d, r);
	(void)panel->update();
	const ui::ResonateRingView* ring = panel->face().ringView();
	const std::vector<ui::ResonateRingView::Chip> chips = ring->chips();
	REQUIRE(chips.size() == 5);
	const float wantNotes[] = { 64.0f, 67.0f, 48.0f, 52.0f, 60.0f };
	const float wantGains[] = { 0.8f, 0.5f, 0.3f, 0.2f, 0.1f };
	const bool wantDrone[] = { false, false, true, true, true };
	for (size_t i = 0; i < chips.size(); ++i) {
		CAPTURE(i);
		CHECK(chips[i].note == wantNotes[i]);
		CHECK(chips[i].drone == wantDrone[i]);
		CHECK(chips[i].alpha == doctest::Approx(0.35f + 0.65f * wantGains[i]));
	}
	// A DRONE-ONLY READING: the string's chips are all drone chips, and a hover names them as the drone's.
	r.headGains = {};
	publishReading(d, r);
	(void)panel->update();
	const std::vector<ui::ResonateRingView::Chip> droneOnly = ring->chips();
	REQUIRE(droneOnly.size() == 3);
	for (const ui::ResonateRingView::Chip& c : droneOnly) {
		CHECK(c.drone);
	}
	const visage::Point at { droneOnly[0].box.x() + 2.0f, droneOnly[0].box.y() + 2.0f };
	CHECK(std::string(ring->tooltipAt(at)) == std::string(ui::ResonateRingView::kDroneChipTooltipPrefix) + "C2");
	// AND A DRONE STRING'S GAIN IS A CHANGE THE VIEW DRAWS: the card's (drawing) view repaints for it, once.
	ui::ResonateRingView* cardRing = d.card->face().ringView();
	REQUIRE(cardRing->isDrawing());
	(void)d.card->updateContent();
	RedrawProbe probe(cardRing);
	(void)d.card->updateContent();
	CHECK(probe.count == 0);
	r.droneGains[0] = 0.25f;
	publishReading(d, r);
	probe.rearm();
	(void)d.card->updateContent();
	CHECK(probe.count == 1);
}
TEST_CASE("RESONATE's pluck flash fades with its age") {
	ResonateFixture d(rsn::kTraits.tag);
	ui::ResonateRingView* ring = d.card->face().ringView();
	const float flash = ui::ResonateRingView::kFlashTicks;
	for (const float age : { 0.0f, flash * 0.25f, flash * 0.5f, flash, flash * 4.0f }) {
		CAPTURE(age);
		publishReading(d, syntheticReading(rsn::Model::Ring, 0.5f, 0.0f, age));
		(void)d.card->updateContent();
		CHECK(ring->flashAlpha() == doctest::Approx(std::max(0.0f, 1.0f - age / flash)));
	}
}
TEST_CASE("RESONATE's ring view is the Character x Morph surface: a drag moves both by the hand, the puck follows") {
	ResonateFixture d(rsn::kTraits.tag);
	ui::ResonateRingView* ring = d.card->face().ringView();
	(void)d.card->updateContent();
	const double c0 = d.f.deviceHost.fieldAt(d.device, static_cast<size_t>(rsn::Param::Character));
	const double m0 = d.f.deviceHost.fieldAt(d.device, static_cast<size_t>(rsn::Param::Morph));
	const visage::Bounds box = ring->plotBounds();
	const visage::Point start { box.x() + box.width() * 0.5f, box.y() + box.height() * 0.5f };
	ring->processMouseDown(at(start.x, start.y));
	CHECK(d.f.deviceHost.fieldAt(d.device, static_cast<size_t>(rsn::Param::Character)) == doctest::Approx(c0));
	ring->processMouseDrag(at(start.x + box.width() * 0.1f, start.y - box.height() * 0.2f));
	ring->processMouseUp(at(start.x + box.width() * 0.1f, start.y - box.height() * 0.2f));
	d.f.bridge.refresh();
	const double c1 = d.f.deviceHost.fieldAt(d.device, static_cast<size_t>(rsn::Param::Character));
	const double m1 = d.f.deviceHost.fieldAt(d.device, static_cast<size_t>(rsn::Param::Morph));
	CHECK(c1 == doctest::Approx(c0 + 0.1).epsilon(1e-4));
	CHECK(m1 == doctest::Approx(m0 + 0.2).epsilon(1e-4));
	const visage::Point puck = ring->puckPoint();
	CHECK(puck.x == doctest::Approx(box.x() + box.width() * static_cast<float>(c1)));
	CHECK(puck.y == doctest::Approx(box.bottom() - box.height() * static_cast<float>(m1)));
	// AT MORPH 0 THE PUCK IS INSIDE THE PLOT, CLEAR OF THE BOTTOM BAND where Character's box sits.
	d.set(static_cast<size_t>(rsn::Param::Morph), 0.0f);
	(void)d.card->updateContent();
	const float puckBottom = ring->puckPoint().y + ui::ResonateRingView::kPuckDiameter * 0.5f;
	CHECK(puckBottom <= ring->characterBand().y() + 0.01f);
}
TEST_CASE("RESONATE's Model is five pictograms on the face, and a press on one selects it") {
	ResonateFixture d(rsn::kTraits.tag);
	ui::ResonateModelPicker* picker = d.card->face().modelPicker();
	REQUIRE(picker != nullptr);
	REQUIRE(picker->isVisible());
	CHECK(picker->selected() == 0);
	for (size_t i = 0; i < ui::ResonateModelPicker::kCells; ++i) {
		const visage::Bounds cell = picker->cellBounds(i);
		CHECK(cell.width() > 20.0f);
		picker->processMouseDown(at(cell.x() + cell.width() * 0.5f, cell.y() + cell.height() * 0.5f));
		d.f.bridge.refresh();
		CHECK(std::lround(d.f.deviceHost.fieldAt(d.device, static_cast<size_t>(rsn::Param::Model))) == static_cast<long>(i));
		CHECK(picker->selected() == i);
	}
}
TEST_CASE("RESONATE's meter is the input's level and a pluck strip lit while a pluck is young") {
	// v19 (by design): the input's onset detector is gone, so the meter draws no detector value and no threshold tick; its
	// strip lights for a pluck (a note-on's or the Cycle's: the reading's pluck age) and the bar is the input's level.
	ResonateFixture d(rsn::kTraits.tag);
	// THE METER IS THE PANEL'S (the simplified card has none); `fractions()` reads the host, so a panel that is
	// built but not opened answers the same values.
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(d.f.deviceHost, &d.f.bridge, registry, d.device, rsn::kDescriptor);
	auto* panel = dynamic_cast<ui::ResonateExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	ui::ResonateMeter* meter = panel->face().meter();
	REQUIRE(meter != nullptr);
	(void)d.card->updateContent();
	rsn::Reading r = syntheticReading(rsn::Model::Ring);
	r.inLevel = 0.5f;
	r.pluckAge = 10.0f;
	publishReading(d, r);
	(void)d.card->updateContent();
	const ui::ResonateMeter::Fractions m = meter->fractions();
	CHECK(std::fabs(m.level - expectedLevel(0.5f)) < 1e-5f);
	CHECK(m.flashing);
	// AN OLD PLUCK: no flash.
	r.pluckAge = 1024.0f;
	publishReading(d, r);
	(void)d.card->updateContent();
	CHECK_FALSE(meter->fractions().flashing);
}
TEST_CASE("RESONATE's ring view follows a change of SCALE: its NATIVE width is converted at the new one") {
	ResonateFixture d(rsn::kTraits.tag);
	ui::ResonateRingView* ring = d.card->face().ringView();
	(void)d.card->updateContent();
	const float logical = static_cast<float>(ring->width());
	REQUIRE(logical > 0.0f);
	const float was = d.card->dpiScale();
	REQUIRE(static_cast<float>(ring->nativeWidth()) == doctest::Approx(logical * was).epsilon(0.02));
	const float now = was * 1.75f;
	d.f.root->setDpiScale(now);
	CHECK(static_cast<float>(ring->width()) == doctest::Approx(logical));
	CHECK(static_cast<float>(ring->nativeWidth()) == doctest::Approx(logical * now).epsilon(0.02));
	(void)d.card->update();
	CHECK(static_cast<float>(ring->nativeWidth()) == doctest::Approx(logical * now).epsilon(0.02));
	d.f.root->setDpiScale(was);
}
TEST_CASE("RESONATE's panel lays the same face out left to right, the ring the biggest thing in it") {
	ResonateFixture d(rsn::kTraits.tag);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(d.f.deviceHost, &d.f.bridge, registry, d.device, rsn::kDescriptor);
	REQUIRE(view != nullptr);
	auto* panel = dynamic_cast<ui::ResonateExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	const visage::Point min = panel->minimumSize();
	panel->setBounds(0.0f, 0.0f, min.x, min.y);
	(void)panel->update();
	panel->resized();
	const ui::ResonateFace& face = panel->face();
	const visage::Frame* pluck = face.widgetFor(rsn::Param::Pluck); // the NOTES stack, left of the string view (v21)
	const visage::Frame* drive = face.widgetFor(rsn::Param::Drive);
	const visage::Frame* bow = face.widgetFor(rsn::Param::Bow);
	const visage::Frame* ring = face.ringView();
	const visage::Frame* mix = face.widgetFor(rsn::Param::Mix);
	CHECK(pluck->x() + pluck->width() <= ring->x() + 0.01f);
	// EXCITE UNDER THE STRING VIEW (v22: the plugin had grown too tall): the meter, Drive and Bow in one row below it, inside
	// its width, the knobs at least 50 px wide and the meter a whole knob tall (readable).
	const visage::Frame* meter = face.meter();
	REQUIRE(meter != nullptr);
	for (const visage::Frame* w : { meter, drive, bow }) {
		CHECK(w->y() >= ring->y() + ring->height() - 0.01f);
		CHECK(w->x() >= ring->x() - 0.01f);
		CHECK(w->x() + w->width() <= ring->x() + ring->width() + 0.01f);
	}
	CHECK(drive->width() >= 50.0f);
	CHECK(bow->width() >= 50.0f);
	CHECK(meter->height() >= drive->height() - 0.01f);
	CHECK(bow->x() >= drive->x() + drive->width() - 0.01f);
	// AND NO TALLER THAN v20 (477 against v20's 495 and v21's 657: MEASURED 2026-10-05).
	CHECK(min.y <= 495.0f);
	CHECK(ring->x() + ring->width() <= mix->x() + 0.01f);
	for (const rsn::Param p : panel->addressedParamsForTest()) {
		const visage::Frame* w = face.widgetFor(p);
		REQUIRE(w != nullptr);
		if (w != ring && w->isVisible()) {
			CHECK(w->width() * w->height() < ring->width() * ring->height());
		}
		CHECK(w->x() + w->width() <= min.x + 0.01f);
		CHECK(w->y() + w->height() <= min.y + 0.01f);
	}
}
TEST_CASE("RESONATE's live string is drawn at the display motion gain, and its rest ghost is not") {
	ResonateFixture d(rsn::kTraits.tag);
	ui::ResonateRingView* ring = d.card->face().ringView();
	(void)d.card->updateContent();
	// A BAR, WITH MOTION ABOUT A REST POSE: y is `cy - 0.42 H s value` per node, restated from the geometry.
	rsn::Reading r = syntheticReading(rsn::Model::Bar, 0.3f);
	for (size_t i = 0; i < rsn::kChainNodes; ++i) {
		r.rest[i] = 0.4f * std::sin(3.14159265f * static_cast<float>(i) / 31.0f);
		r.nodes[i] = r.rest[i] + 0.05f * std::sin(6.2831853f * static_cast<float>(i) / 31.0f * 3.0f);
	}
	publishReading(d, r);
	(void)d.card->updateContent();
	const visage::Bounds box = ring->plotBounds();
	const float cy = box.y() + box.height() * 0.5f;
	const float s = ring->displayScale();
	const std::vector<visage::Point> live = ring->livePath();
	const std::vector<visage::Point> rest = ring->restPath();
	REQUIRE(live.size() == rsn::kChainNodes);
	const float g = ui::ResonateRingView::kMotionGain;
	for (size_t i = 0; i < rsn::kChainNodes; ++i) {
		CAPTURE(i);
		// THE GHOST, UNSCALED.
		CHECK(rest[i].y == doctest::Approx(cy - box.height() * 0.42f * s * r.rest[i]).epsilon(1e-4));
		// THE DRAWN DEVIATION IS THE GAIN TIMES THE READING'S, on the same ruler.
		CHECK(live[i].y - rest[i].y == doctest::Approx(-box.height() * 0.42f * s * g * (r.nodes[i] - r.rest[i])).epsilon(1e-3));
		// AND INSIDE THE PLOT, whatever the gain.
		CHECK(live[i].y >= box.y() - 0.01f);
		CHECK(live[i].y <= box.bottom() + 0.01f);
	}
	// THE RULER INCLUDES THE AMPLIFIED SHAPE: a gain that would leave the plot shrinks the ruler instead.
	float peak = 0.0f;
	for (size_t i = 0; i < rsn::kChainNodes; ++i) {
		peak = std::max({ peak, std::fabs(r.rest[i] + g * (r.nodes[i] - r.rest[i])), std::fabs(r.rest[i]) });
	}
	CHECK(s == doctest::Approx(1.0f / std::max(peak, ui::ResonateRingView::kScaleFloor)));
}
TEST_CASE("RESONATE's view draws a parked string dimmed and marked held, and repaints only when that changes") {
	ResonateFixture d(rsn::kTraits.tag);
	ui::ResonateRingView* ring = d.card->face().ringView();
	REQUIRE(ring->isDrawing());
	rsn::Reading r = syntheticReading(rsn::Model::Ring);
	publishReading(d, r);
	(void)d.card->updateContent();
	CHECK_FALSE(ring->parked());
	CHECK(ring->liveAlpha() == 1.0f);
	RedrawProbe probe(ring);
	// PARKED: the device's flag, as a value — dimmed, one repaint.
	r.parked = 1.0f;
	publishReading(d, r);
	probe.rearm();
	(void)d.card->updateContent();
	CHECK(ring->parked());
	CHECK(ring->liveAlpha() == doctest::Approx(ui::ResonateRingView::kParkedAlpha));
	CHECK(probe.count == 1);
	// STILL PARKED, THE SAME READING: no repaint, on every pass.
	for (int i = 0; i < 4; ++i) {
		publishReading(d, r);
		probe.rearm();
		(void)d.card->updateContent();
		CHECK(probe.count == 1);
	}
	// WOKEN: full alpha again, one repaint.
	r.parked = 0.0f;
	publishReading(d, r);
	probe.rearm();
	(void)d.card->updateContent();
	CHECK_FALSE(ring->parked());
	CHECK(ring->liveAlpha() == 1.0f);
	CHECK(probe.count == 2);
}
TEST_CASE("GG: the modulator cards' multivalue controls are menu boxes that list every choice") {
	namespace lfo = feathers::mod::lfo;
	namespace ms = feathers::mod::mseg;
	namespace dsr = feathers::mod::dahdsr;
	SUBCASE("LFO: Waveform and Mode") {
		GgCard<ui::LfoCard> g { lfo::kTraits.tag };
		REQUIRE(ui::LfoCard::kChoiceCount == 2);
		CHECK(ui::LfoCard::choices()[0] == lfo::Param::Waveform);
		CHECK(ui::LfoCard::choices()[1] == lfo::Param::Mode);
		checkChoicesAreMenus(g, lfo::kDescriptor);
	}
	SUBCASE("MSEG: Mode") {
		GgCard<ui::MsegCard> g { ms::kTraits.tag };
		REQUIRE(ui::MsegCard::kChoiceCount == 1);
		CHECK(ui::MsegCard::choices()[0] == ms::Param::Mode);
		checkChoicesAreMenus(g, ms::kDescriptor);
	}
	SUBCASE("DAHDSR: Mode") {
		GgCard<ui::EnvCard> g { nullptr };
		REQUIRE(ui::EnvCard::kChoiceCount == 1);
		CHECK(ui::EnvCard::choices()[0] == dsr::Param::Mode);
		checkChoicesAreMenus(g, dsr::kDescriptor);
	}
}
TEST_CASE("GG: a pick from a modulator card's menu writes one value and passes through none") {
	namespace lfo = feathers::mod::lfo;
	namespace ms = feathers::mod::mseg;
	namespace dsr = feathers::mod::dahdsr;
	SUBCASE("LFO") {
		GgCard<ui::LfoCard> g { lfo::kTraits.tag };
		checkPicksWriteOnce(g, lfo::kDescriptor);
	}
	SUBCASE("MSEG") {
		GgCard<ui::MsegCard> g { ms::kTraits.tag };
		checkPicksWriteOnce(g, ms::kDescriptor);
	}
	SUBCASE("DAHDSR") {
		GgCard<ui::EnvCard> g { nullptr };
		checkPicksWriteOnce(g, dsr::kDescriptor);
		// AND THE PICK DOES WHAT THE CHIP'S PRESS DID AFTER ITS WRITE: Sustain is a floor while it loops.
		ui::ValueBox* mode = g.card->choiceBoxForTest(0);
		REQUIRE(mode != nullptr);
		size_t sustain = ui::EnvCard::kBoxCount;
		for (size_t b = 0; b < ui::EnvCard::kBoxCount; ++b) {
			if (ui::EnvCard::layout()[b] == dsr::Param::Sustain) {
				sustain = b;
			}
		}
		REQUIRE(sustain < ui::EnvCard::kBoxCount);
		mode->choiceMenu().onSelection().callback(static_cast<int>(dsr::Mode::Loop));
		CHECK(g.card->boxCaption(sustain) == "Floor");
		mode->choiceMenu().onSelection().callback(static_cast<int>(dsr::Mode::Gate));
		CHECK(g.card->boxCaption(sustain) != "Floor");
	}
}

namespace {
//--------------------------------------------------------------------------------------------------//
// A CHOICE LIST OPENS ON THE CURRENT CHOICE, MARKED                                //
//--------------------------------------------------------------------------------------------------//
//
// In HARM and other places with long lists of options, the menu reopens centred on the current option, with
// an "x" next to the current selection. `menu_anchor.h` has the design:
// the row is flagged and marked by one call, and a list taller than its room is SCROLLED (not moved over the
// box) so the flagged row sits mid-list. These read the list visage built — its rows, its scroll, its row
// geometry — rather than the placement report alone, so the report cannot agree with itself.

struct OpenedChoice {
	visage::PopupList* list = nullptr;
	ui::MenuPlacement placed;
};
} // namespace

TEST_CASE("a long choice list opens scrolled so the current choice sits in its middle, marked with an X; HARM's ratios and Pattern's Scale") {
	using menu_check::checkHangsOff;
	using menu_check::inTop;
	using menu_check::newestPopup;
	using menu_check::rowsOf;
	SUBCASE("Pattern's Scale box, at a mid scale") {
		namespace pt = feathers::mod::arp::pat;
		ModFixture f;
		const size_t slot = f.bridge.addRackDevice(pt::kTraits.tag);
		REQUIRE(slot < feathers::kMaxRackSlots);
		f.bridge.refresh();
		f.panel->resized();
		auto* card = const_cast<ui::PatternCard*>(dynamic_cast<const ui::PatternCard*>(f.panel->cardFrameForTest(slot)));
		REQUIRE(card != nullptr);
		const int count = static_cast<int>(feathers::mod::arp::theory::kScaleCount);
		const int want = count / 2;
		f.deviceHost.setField(card->device(), pt::at(0, pt::Field::Scale), static_cast<float>(want));
		card->setFocus(0);
		(void)card->updateContent();
		ui::ValueBox* box = card->scaleBoxForTest(0);
		REQUIRE(box != nullptr);
		REQUIRE(box->showChoiceMenu());
		visage::PopupMenuFrame* popup = newestPopup(*box);
		REQUIRE(popup != nullptr);
		visage::PopupList* list = rowsOf(*popup);
		REQUIRE(list != nullptr);
		REQUIRE(list->numOptions() == count);
		const ui::MenuPlacement placed = ui::lastMenuPlacementForTest();
		CHECK(placed.current == want);
		checkMarkedOnly(*list, want);
		checkCentred(*list, want);
		checkHangsOff(placed, inTop(*box, { 0.0f, 0.0f, box->width(), box->height() }));
		ui::dismissMenuOf(*box);
		REQUIRE(f.bridge.removeRackDevice(slot));
	}
}
TEST_CASE("a short choice list is only marked: it shows every row, so it opens unscrolled with the X on the current one") {
	namespace pt = feathers::mod::arp::pat;
	ModFixture f;
	const size_t slot = f.bridge.addRackDevice(pt::kTraits.tag);
	REQUIRE(slot < feathers::kMaxRackSlots);
	f.bridge.refresh();
	f.panel->resized();
	auto* card = const_cast<ui::PatternCard*>(dynamic_cast<const ui::PatternCard*>(f.panel->cardFrameForTest(slot)));
	REQUIRE(card != nullptr);
	// MODE, seven entries: the Scale box's neighbour.
	const int want = 2; // Chords
	f.deviceHost.setField(card->device(), pt::at(0, pt::Field::Mode), static_cast<float>(want));
	REQUIRE(f.deviceHost.fieldAt(card->device(), pt::at(0, pt::Field::Mode)) == static_cast<float>(want));
	card->setFocus(0);
	(void)card->updateContent();
	ui::ValueBox* mode = card->modeBoxForTest(0);
	REQUIRE(mode != nullptr);
	REQUIRE(mode->entryCount() == static_cast<size_t>(pt::kModeChoices));
	REQUIRE(mode->showChoiceMenu());
	visage::PopupMenuFrame* popup = menu_check::newestPopup(*mode);
	REQUIRE(popup != nullptr);
	visage::PopupList* list = menu_check::rowsOf(*popup);
	REQUIRE(list != nullptr);
	REQUIRE(list->numOptions() == static_cast<int>(pt::kModeChoices));
	CHECK(list->renderHeight() <= list->height() + 0.5f); // every row shown
	const ui::MenuPlacement placed = ui::lastMenuPlacementForTest();
	CHECK(placed.current == want);
	CHECK(placed.scroll == 0.0f);
	CHECK(list->yPosition() == 0.0f);
	checkMarkedOnly(*list, want);
	ui::dismissMenuOf(*mode);
	REQUIRE(f.bridge.removeRackDevice(slot));
}
TEST_CASE("SHOT choice menus opened on a mid-list choice: HARM's ratio and Pattern's Scale (FEATHERS_SHOT_DIR)") {
	// NOT AN ASSERTION — A CAMERA, for the "opened on the current choice" change (`menu_anchor.h`). The whole editor with
	// the menu open, written as choice_menu_harm.png and choice_menu_scale.png; the list's rectangle and the current
	// row's are printed so a close-up can be cut. Run under `scripts/venue-xvfb.sh` with FEATHERS_REDUCED_MOTION=1.
	const char* dir = std::getenv("FEATHERS_SHOT_DIR");
	if (dir == nullptr) {
		return;
	}
	const auto shoot = [dir](visage::Frame* rootFrame, visage::Frame& source, const char* name) {
		auto* root = dynamic_cast<visage::ApplicationEditor*>(rootFrame);
		REQUIRE(root != nullptr);
		visage::PopupMenuFrame* popup = menu_check::newestPopup(source);
		REQUIRE(popup != nullptr);
		visage::PopupList* list = menu_check::rowsOf(*popup);
		REQUIRE(list != nullptr);
		// FULLY FADED IN — the popup's opacity animates on its draws.
		for (int i = 0; i < 60; ++i) {
			root->redrawAll();
			root->drawWindow();
		}
		const visage::Screenshot& shot = root->takeScreenshot();
		if (shot.width() == 0) {
			MESSAGE("SKIP no screenshot");
			return;
		}
		const std::string path = std::string(dir) + "/" + name + ".png";
		shot.save(path);
		const ui::MenuPlacement p = ui::lastMenuPlacementForTest();
		const float rowTop = static_cast<float>(list->yForIndex(p.current)) - list->yPosition();
		const float rowH = static_cast<float>(list->yForIndex(p.current + 1) - list->yForIndex(p.current));
		MESSAGE(name << " list " << list->x() << "," << list->y() << " " << list->width() << "x" << list->height()
		             << " scroll " << list->yPosition() << " current " << p.current << " row at y " << list->y() + rowTop
		             << " h " << rowH << " anchor " << p.anchor.x() << "," << p.anchor.y() << " " << p.anchor.width() << "x"
		             << p.anchor.height() << " scale " << root->dpiScale() << " shot " << shot.width() << "x" << shot.height()
		             << " -> " << path);
	};
	{
		namespace pt = feathers::mod::arp::pat;
		ModFixture f;
		auto* root = dynamic_cast<visage::ApplicationEditor*>(f.root);
		REQUIRE(root != nullptr);
		root->setWindowless(static_cast<int>(f.root->width()), static_cast<int>(f.root->height()));
		const size_t slot = f.bridge.addRackDevice(pt::kTraits.tag);
		REQUIRE(slot < feathers::kMaxRackSlots);
		f.bridge.refresh();
		f.panel->resized();
		auto* card = const_cast<ui::PatternCard*>(dynamic_cast<const ui::PatternCard*>(f.panel->cardFrameForTest(slot)));
		REQUIRE(card != nullptr);
		f.deviceHost.setField(card->device(), pt::at(0, pt::Field::Scale),
		                      static_cast<float>(feathers::mod::arp::theory::kScaleCount / 2));
		card->setFocus(0);
		(void)card->updateContent();
		ui::ValueBox* box = card->scaleBoxForTest(0);
		REQUIRE(box != nullptr);
		REQUIRE(box->showChoiceMenu());
		shoot(f.root, *box, "choice_menu_scale");
		ui::dismissMenuOf(*box);
		REQUIRE(f.bridge.removeRackDevice(slot));
	}
}
//--------------------------------------------------------------------------------------------------//
// EVERY DROPDOWN HANGS OFF ITS BOX — the placement sweep it was asked for with GG (2026-09-24)          //
//--------------------------------------------------------------------------------------------------//
//
// In some places a dropdown menu did not appear next to the box it belongs to. Every site
// that opens a menu goes through `ui::showMenuAt` (`menu_anchor.h`) now; this case opens each one by
// the function its press calls, and checks the rows against the box the CASE names — from the widget's
// own accessor — in the frame the popup was parented on. `tests/gui_windowed` asks the same of OWLTT's
// panel in all three of its hosts, which is where it was seen it.

TEST_CASE("GG placement: every dropdown in the editor hangs off the box it was opened from") {
	using menu_check::checkHangsOff;
	using menu_check::inTop;
	namespace lfo = feathers::mod::lfo;

	SUBCASE("a modulator card's choice box, at the top of the strip and scrolled to its end") {
		FilterFixture f;
		const size_t slot = f.bridge.addRackDevice(lfo::kTraits.tag);
		REQUIRE(slot < kMaxRackSlots);
		f.bridge.refresh();
		f.panel->resized();
		auto* card = const_cast<ui::LfoCard*>(dynamic_cast<const ui::LfoCard*>(f.panel->cardFrameForTest(slot)));
		REQUIRE(card != nullptr);
		for (size_t i = 0; i < ui::LfoCard::kChoiceCount; ++i) {
			ui::ValueBox* box = card->choiceBoxForTest(i);
			REQUIRE(box != nullptr);
			REQUIRE(box->showChoiceMenu());
			checkHangsOff(ui::lastMenuPlacementForTest(), inTop(*box, { 0.0f, 0.0f, box->width(), box->height() }));
			ui::dismissMenuOf(*box);
		}
		// SCROLLED TO THE END, the card's chip row is at the strip's foot — where a menu must go UP.
		f.panel->setScroll(f.panel->maxScroll());
		ui::ValueBox* mode = card->choiceBoxForTest(1);
		REQUIRE(mode->showChoiceMenu());
		checkHangsOff(ui::lastMenuPlacementForTest(), inTop(*mode, { 0.0f, 0.0f, mode->width(), mode->height() }));
		ui::dismissMenuOf(*mode);
	}

	SUBCASE("the strip's (+) and a card's slot menu") {
		FilterFixture f;
		f.panel->showAddMenu();
		checkHangsOff(ui::lastMenuPlacementForTest(), inTop(*f.panel, f.panel->addBounds()));
		const size_t slot = f.bridge.addRackDevice(lfo::kTraits.tag);
		REQUIRE(slot < kMaxRackSlots);
		f.bridge.refresh();
		f.panel->resized();
		f.panel->showSlotMenu(slot);
		checkHangsOff(ui::lastMenuPlacementForTest(),
		              inTop(*f.panel, f.panel->slotMenuBounds(backplane::Source::output(f.bridge.rackDeviceAt(slot), 0))));
	}

	SUBCASE("the fx lane's (+) and a card's slot menu, with the chain full and scrolled to its end") {
		FilterFixture f;
		const auto headOf = [&](size_t slot) {
			const ui::FxCard* card = f.lane->cardAt(slot);
			REQUIRE(card != nullptr);
			return inTop(*f.lane, { card->x(), card->y(), card->width(), ui::FxCard::kSlotHeadHeight });
		};
		f.lane->showAddMenu();
		checkHangsOff(ui::lastMenuPlacementForTest(), inTop(*f.lane, f.lane->addBounds()));
		f.lane->showSlotMenu(0);
		checkHangsOff(ui::lastMenuPlacementForTest(), headOf(0));
		while (f.lane->deviceCount() < feathers::kMaxFxSlots) {
			REQUIRE(f.lane->addChoiceForTest(1) < feathers::kMaxFxSlots);
		}
		f.lane->setScroll(f.lane->maxScroll());
		f.lane->showAddMenu();
		checkHangsOff(ui::lastMenuPlacementForTest(), inTop(*f.lane, f.lane->addBounds()));
		const size_t last = f.lane->deviceCount() - 1;
		f.lane->showSlotMenu(last);
		checkHangsOff(ui::lastMenuPlacementForTest(), headOf(last));
	}

	SUBCASE("the filter card's Type and Slope") {
		FilterFixture f;
		f.card->showTypeMenu();
		checkHangsOff(ui::lastMenuPlacementForTest(), inTop(*f.card, f.card->typeMenuBounds()));
		f.card->showSlopeMenu();
		checkHangsOff(ui::lastMenuPlacementForTest(), inTop(*f.card, f.card->slopeBoxBounds()));
	}

	SUBCASE("a generic card's Choice box — the delay's Mode, in the lane") {
		FilterFixture f;
		namespace dly = feathers::fx::delay;
		const size_t slot = f.bridge.addFxDevice(dly::kTraits.tag);
		REQUIRE(slot < feathers::kMaxFxSlots);
		f.bridge.refresh();
		f.lane->resized();
		f.lane->scrollCardIntoView(slot);
		auto* card = dynamic_cast<ui::GenericCard*>(f.lane->cardAt(slot));
		REQUIRE(card != nullptr);
		ui::ValueBox* mode = nullptr;
		for (const auto& b : card->boxesForTest()) {
			if (&b->control().desc() == &dly::kParamDescs[static_cast<size_t>(dly::Param::Mode)]) {
				mode = b.get();
			}
		}
		REQUIRE(mode != nullptr);
		REQUIRE(mode->showChoiceMenu());
		checkHangsOff(ui::lastMenuPlacementForTest(), inTop(*mode, { 0.0f, 0.0f, mode->width(), mode->height() }));
	}

	SUBCASE("the Engine rail's engine and the preset browser's name") {
		FilterFixture f;
		ui::EngineRail* rail = findInTree<ui::EngineRail>(f.root);
		REQUIRE(rail != nullptr);
		ui::ValueBox* engineBox = rail->engineBoxForTest();
		REQUIRE(engineBox != nullptr);
		REQUIRE(engineBox->showChoiceMenu());
		checkHangsOff(ui::lastMenuPlacementForTest(), inTop(*engineBox, { 0.0f, 0.0f, engineBox->width(), engineBox->height() }));
		ui::PresetBrowser* browser = f.gui.presetBrowserForTest();
		REQUIRE(browser != nullptr);
		browser->showPresetMenu();
		checkHangsOff(ui::lastMenuPlacementForTest(), inTop(*browser, browser->nameBounds()));
	}

	SUBCASE("a macro knob's menu, and a routed knob's inbound-route menu") {
		FilterFixture f;
		ui::Knob* macro = f.panel->macroKnobForTest(0);
		REQUIRE(macro != nullptr);
		REQUIRE(macro->showMacroMenu());
		checkHangsOff(ui::lastMenuPlacementForTest(), inTop(*macro, { 0.0f, 0.0f, macro->width(), macro->height() }));

		std::vector<ui::Knob*> knobs;
		collectKnobs(f.lane, &knobs);
		ui::Knob* knob = nullptr;
		for (ui::Knob* k : knobs) {
			if (knob == nullptr && k->control() != nullptr && k->control()->display().isDestination) {
				knob = k;
			}
		}
		REQUIRE(knob != nullptr);
		ui::ModState* mod = f.gui.modForTest();
		REQUIRE(mod != nullptr);
		mod->focus(backplane::Source::core(1));
		mod->arm(true);
		knob->control()->beginAmountGesture();
		knob->control()->setAmount(0.4f);
		knob->control()->endAmountGesture();
		mod->arm(false);
		REQUIRE(knob->showRouteMenu());
		checkHangsOff(ui::lastMenuPlacementForTest(), inTop(*knob, { 0.0f, 0.0f, knob->width(), knob->height() }));
	}


	SUBCASE("the drawn filter's band-type menu, which opens at the point pressed") {
		namespace dr = feathers::fx::drawn;
		FilterFixture f;
		ui::WidgetRegistry registry;
		const size_t slot = f.bridge.addFxDevice(dr::kTraits.tag);
		REQUIRE(slot < feathers::kMaxFxSlots);
		auto view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, f.bridge.fxDeviceAt(slot), dr::kDescriptor);
		auto* panel = dynamic_cast<ui::DrawnExpanded*>(view.get());
		REQUIRE(panel != nullptr);
		panel->setBounds(0.0f, 0.0f, 560.0f, panel->minimumSize().y);
		panel->resized();
		panel->selectFrame(0);
		// A POPUP-TRIGGER PRESS ON A POLE — the gesture that opens it (`shouldTriggerPopup`).
		const visage::Point at = panel->poleCentre(0);
		REQUIRE(panel->hitAt(at).what == ui::DrawnExpanded::Grab::Pole);
		visage::MouseEvent e;
		e.position = at;
		e.window_position = at;
		e.event_frame = panel;
		e.button_id = visage::kMouseButtonRight;
		e.button_state = visage::kMouseButtonRight;
		e.is_down = true;
		e.repeat_click_count = 1;
		panel->processMouseDown(e);
		checkHangsOff(ui::lastMenuPlacementForTest(), inTop(*panel, { at.x, at.y, 0.0f, 0.0f }));
	}

	SUBCASE("a zone selector — SKLITTER's Groove Mode, on its panel") {
		namespace sc = feathers::fx::sklitter;
		ModFixture f;
		ui::WidgetRegistry reg;
		const DeviceInstanceId id = f.bridge.fxDeviceAt(f.bridge.addFxDevice(sc::kTraits.tag));
		f.bridge.setDeviceControl(id, static_cast<size_t>(sc::Param::Mode), static_cast<float>(sc::Mode::Shuffle));
		ui::GenericExpanded panel(f.deviceHost, reg, id, sc::kDescriptor, nullptr);
		const visage::Point want = panel.minimumSize();
		panel.setBounds(0, 0, static_cast<int>(want.x), static_cast<int>(want.y));
		panel.resized();
		(void)panel.update();
		ui::ModeValueCell* groove = sklitterCellFor<ui::ModeValueCell>(panel, sc::Param::GrooveMode);
		REQUIRE(groove != nullptr);
		ui::ZoneSelect& select = groove->select();
		REQUIRE(select.pressAt({ select.width() * 0.5f, select.height() * 0.5f }));
		checkHangsOff(ui::lastMenuPlacementForTest(), inTop(select, { 0.0f, 0.0f, select.width(), select.height() }));
	}

	SUBCASE("SKLITTER's chain column — its (+) and a card's slot menu") {
		HeldChain held;
		ui::WidgetRegistry registry;
		ui::ModState mod;
		ui::EditHistory history;
		ui::ChainColumn column(held.bridge(), registry, mod, history, nullptr);
		// A COLUMN STANDING ALONE IS ITS OWN WINDOW, so it is given the editor's height rather than a rail's:
		// its (+) menu lists every device the chain can take (~19 rows, 426 px), which fits above or below
		// a row in the editor and fits NEITHER side of a row in a 600 px frame — a venue, not the product.
		column.setBounds(0, 0, static_cast<int>(ui::FxLaneRail::kWidth), 1000);
		const DeviceInstanceId first = column.addChoiceForTest(0);
		REQUIRE(first != kNoInstance);
		column.resized();
		column.showAddMenu();
		checkHangsOff(ui::lastMenuPlacementForTest(), inTop(column, column.addBounds()));
		const ui::FxCard* card = column.cardOf(first);
		REQUIRE(card != nullptr);
		column.showSlotMenu(first);
		checkHangsOff(ui::lastMenuPlacementForTest(),
		              inTop(column, { card->x(), card->y(), card->width(), ui::FxCard::kSlotHeadHeight }));
	}

	SUBCASE("Featherverb's IR stepper — its list, and the folder menu its verb opens a tick later") {
		// Added to mainline after this sweep began (the IR browser); a panel standing alone is its own
		// window, so this is the SMALL-window case too.
		FakeIrDialogs dialogs; // NO OS DIALOG here: Choose folder... falls back to the menu this sweep places
		dialogs.canShow = false;
		ui::ScopedFileDialogBackend useFake(dialogs);
		VerbRig rig;
		ui::ItemStepper& stepper = rig.panel->stepperForTest();
		rig.panel->stepperForTest().choose(ui::ItemStepper::kActionBase + ui::FeatherverbExpanded::kChooseFolderAction);
		(void)rig.panel->updateContent(); // the next tick opens it
		checkHangsOff(ui::lastMenuPlacementForTest(), inTop(stepper, stepper.nameBounds()));
		ui::dismissMenuOf(stepper);
		stepper.showList();
		checkHangsOff(ui::lastMenuPlacementForTest(), inTop(stepper, stepper.nameBounds()));
	}

	SUBCASE("Explore's Warp mode") {
		ExploreFixture f;
		f.set(ParamId::OscType, 2.0 /* V5 */);
		f.set(ParamId::AEnable, 1.0);
		f.stage.update();
		f.stage.showWarpModeMenu();
		checkHangsOff(ui::lastMenuPlacementForTest(), inTop(f.stage, f.stage.warpModeBounds()));
	}
}
TEST_CASE("GG toggle: a press on the box that owns an open menu closes it, and the next press opens it again") {
	// Re-clicking the box closes the menu; it used to stay open until a click landed anywhere else.
	// A `PopupMenuFrame` ignores the mouse, so the press reaches the box,
	// whose press used to open a SECOND menu over the first. Driven through the real press road — hit
	// test from the window root — and counted in popups, because "closed" and "reopened at once" look
	// the same from the menu's own state.
	namespace lfo = feathers::mod::lfo;
	FilterFixture f;
	const size_t slot = f.bridge.addRackDevice(lfo::kTraits.tag);
	REQUIRE(slot < kMaxRackSlots);
	f.bridge.refresh();
	f.panel->resized();
	auto* card = const_cast<ui::LfoCard*>(dynamic_cast<const ui::LfoCard*>(f.panel->cardFrameForTest(slot)));
	REQUIRE(card != nullptr);
	ui::ValueBox* box = card->choiceBoxForTest(0);
	REQUIRE(box != nullptr);
	visage::Frame* top = box->topParentFrame();
	const auto popups = [top] {
		size_t n = 0;
		for (visage::Frame* child : top->children()) {
			n += dynamic_cast<visage::PopupMenuFrame*>(child) != nullptr ? 1u : 0u;
		}
		return n;
	};
	const visage::Point centre = box->positionInWindow() + visage::Point { box->width() * 0.5f, box->height() * 0.5f };
	REQUIRE(f.root->frameAtPoint(centre) == static_cast<visage::Frame*>(box));
	const size_t before = popups();

	dragThroughRoot(f.root, centre, centre); // open
	CHECK(ui::menuOpenFor(*box));
	CHECK(popups() == before + 1);

	dragThroughRoot(f.root, centre, centre); // the same box again: closed, and no second menu
	CHECK_FALSE(ui::menuOpenFor(*box));
	CHECK_FALSE(ui::lastMenuPlacementForTest().shown);
	CHECK(popups() == before + 1);

	dragThroughRoot(f.root, centre, centre); // and again: open
	CHECK(ui::menuOpenFor(*box));
	CHECK(ui::lastMenuPlacementForTest().shown);
	CHECK(popups() == before + 2);

	// A DIFFERENT BOX IS NOT A RE-CLICK — the Mode box opens its own menu while Waveform's is open.
	ui::ValueBox* other = card->choiceBoxForTest(1);
	REQUIRE(other != nullptr);
	REQUIRE(other->showChoiceMenu());
	CHECK(ui::menuOpenFor(*other));
	CHECK(ui::lastMenuPlacementForTest().shown);
}
TEST_CASE("GG on the macro menu: the knob's and the pad's menu hang off their box, and a second open closes it") {
	// macro-mod replaced the knob's polarity menu with `showMacroMenu` (type, polarity, rename), reached from
	// a knob AND from a Latch/Momentary slot's `SwitchChip`; both go through `showMenuAt`, so both get the
	// placement and the re-click toggle the rest of the editor's dropdowns have.
	using menu_check::checkHangsOff;
	using menu_check::inTop;
	FilterFixture f;
	ui::Knob* macro = f.panel->macroKnobForTest(0);
	REQUIRE(macro != nullptr);
	REQUIRE(macro->showMacroMenu());
	CHECK(ui::menuOpenFor(*macro));
	checkHangsOff(ui::lastMenuPlacementForTest(), inTop(*macro, { 0.0f, 0.0f, macro->width(), macro->height() }));
	// THE SAME BOX AGAIN, while its menu is open: closed, not a second menu. The press is still the menu's.
	CHECK(macro->showMacroMenu());
	CHECK_FALSE(ui::menuOpenFor(*macro));
	CHECK_FALSE(ui::lastMenuPlacementForTest().shown);
	CHECK(macro->showMacroMenu());
	CHECK(ui::menuOpenFor(*macro));

	// THE PAD: a MACROS slot turned Latch shows its chip, and the chip anchors the same menu.
	ModFixture m;
	const MacrosSeat seat = seatMacros(m);
	ui::MacroSlot& slot = seat.card->slot(0);
	ui::chooseMacroMenuEntry(slot.menuHooks(), ui::kMacroMenuLatch);
	REQUIRE(slot.chip().isVisible());
	ui::SwitchChip& chip = slot.chip();
	REQUIRE(ui::showMacroMenu(chip, slot.menuHooks()));
	CHECK(ui::menuOpenFor(chip));
	checkHangsOff(ui::lastMenuPlacementForTest(), inTop(chip, { 0.0f, 0.0f, chip.width(), chip.height() }));
	CHECK(ui::showMacroMenu(chip, slot.menuHooks()));
	CHECK_FALSE(ui::menuOpenFor(chip));
}

namespace cascade {
using menu_check::pickRow;
} // namespace cascade

TEST_CASE("GG cascade: the fx lane's \"Add before…\" opens the add menu at the card, and a pick inserts BEFORE it") {
	using namespace cascade;
	FilterFixture f;
	REQUIRE(f.lane->addChoiceForTest(0) < feathers::kMaxFxSlots);
	const size_t anchorSlot = f.lane->deviceCount() - 1;
	REQUIRE(anchorSlot >= 1);
	const DeviceInstanceId anchor = f.bridge.fxDeviceAt(anchorSlot);
	const DeviceInstanceId earlier = f.bridge.fxDeviceAt(anchorSlot - 1);
	const size_t count = f.lane->deviceCount();
	f.lane->resized();
	f.lane->scrollCardIntoView(anchorSlot);
	f.lane->resized();
	ui::FxCard* card = f.lane->cardAt(anchorSlot);
	REQUIRE(card != nullptr);

	press(f.root, *card, card->slotMenuBounds());
	visage::PopupMenuFrame* slotMenu = newestPopup(*f.lane);
	REQUIRE(slotMenu != nullptr);
	REQUIRE(ui::menuOpenFor(*f.lane));
	REQUIRE(pickRow(*slotMenu, "Add before…"));

	// THE ADD MENU IS OPEN — a new popup, owned by the lane, the one the placement was taken of.
	visage::PopupMenuFrame* addMenu = newestPopup(*f.lane);
	CHECK(addMenu != slotMenu);
	CHECK(ui::menuOpenFor(*f.lane));
	CHECK(ui::lastMenuPlacementForTest().shown);
	REQUIRE(addMenu != nullptr);
	REQUIRE(addMenu != slotMenu);

	// AND A PICK IN IT INSERTS BEFORE THE CARD whose dots were pressed.
	const device::Descriptor* chosen = f.lane->addableForTest().front();
	REQUIRE(pickRow(*addMenu, chosen->traits.name));
	REQUIRE(f.lane->deviceCount() == count + 1);
	CHECK(f.bridge.fxDeviceAt(anchorSlot - 1) == earlier);
	CHECK(f.bridge.fxDeviceAt(anchorSlot + 1) == anchor);
	const DeviceInstanceId inserted = f.bridge.fxDeviceAt(anchorSlot);
	CHECK(inserted != anchor);
	CHECK(inserted != earlier);
	CHECK(std::string(fxDeviceAtPosition(f.bridge.fxChain(), anchorSlot).tag().data()) == chosen->traits.tag);
}
TEST_CASE("GG cascade: the rack strip's \"Add before…\" opens the add menu at the card, and a pick inserts BEFORE it") {
	using namespace cascade;
	namespace lfo = feathers::mod::lfo;
	FilterFixture f;
	REQUIRE(f.bridge.addRackDevice(lfo::kTraits.tag) < kMaxRackSlots);
	const size_t anchorSlot = f.bridge.addRackDevice(lfo::kTraits.tag);
	REQUIRE(anchorSlot < kMaxRackSlots);
	REQUIRE(anchorSlot >= 1);
	f.bridge.refresh();
	f.panel->resized();
	const DeviceInstanceId anchor = f.bridge.rackDeviceAt(anchorSlot);
	const DeviceInstanceId earlier = f.bridge.rackDeviceAt(anchorSlot - 1);
	const size_t count = f.panel->deviceCount();

	// THE LAST CARD IS BELOW THE STRIP'S FOOT until the strip is scrolled to its end.
	f.panel->setScroll(f.panel->maxScroll());
	const visage::Bounds dots = f.panel->slotMenuBounds(backplane::Source::output(anchor, 0));
	REQUIRE(dots.bottom() <= f.panel->height());
	press(f.root, *f.panel, dots);
	REQUIRE(f.panel->lastSlotMenuForTest() == anchorSlot);
	visage::PopupMenuFrame* slotMenu = newestPopup(*f.panel);
	REQUIRE(slotMenu != nullptr);
	REQUIRE(pickRow(*slotMenu, "Add before…"));

	visage::PopupMenuFrame* addMenu = newestPopup(*f.panel);
	CHECK(ui::menuOpenFor(*f.panel));
	CHECK(ui::lastMenuPlacementForTest().shown);
	REQUIRE(addMenu != nullptr);
	REQUIRE(addMenu != slotMenu);

	const device::Descriptor* chosen = f.panel->addableForTest().front();
	REQUIRE(pickRow(*addMenu, chosen->traits.name));
	f.bridge.refresh();
	REQUIRE(f.panel->deviceCount() == count + 1);
	CHECK(f.bridge.rackDeviceAt(anchorSlot - 1) == earlier);
	CHECK(f.bridge.rackDeviceAt(anchorSlot + 1) == anchor);
	CHECK(f.bridge.rackDeviceAt(anchorSlot) != anchor);
	CHECK(f.bridge.rackDeviceAt(anchorSlot) != earlier);
}
TEST_CASE("GG cascade: SKLITTER's chain column's \"Add before…\" opens the add menu at the card, and a pick inserts "
          "BEFORE it") {
	using namespace cascade;
	HeldChain held;
	ui::WidgetRegistry registry;
	ui::ModState mod;
	ui::EditHistory history;
	ui::ChainColumn column(held.bridge(), registry, mod, history, nullptr);
	column.setBounds(0, 0, static_cast<int>(ui::FxLaneRail::kWidth), 1000);
	const DeviceInstanceId earlier = column.addChoiceForTest(0);
	const DeviceInstanceId anchor = column.addChoiceForTest(0);
	REQUIRE(earlier != kNoInstance);
	REQUIRE(anchor != kNoInstance);
	column.resized();
	const size_t anchorAt = held.bridge().positionOf(anchor);
	REQUIRE(anchorAt == held.bridge().positionOf(earlier) + 1);
	const size_t count = column.deviceCount();
	ui::FxCard* card = column.cardOf(anchor);
	REQUIRE(card != nullptr);

	press(&column, *card, card->slotMenuBounds());
	visage::PopupMenuFrame* slotMenu = newestPopup(column);
	REQUIRE(slotMenu != nullptr);
	REQUIRE(pickRow(*slotMenu, "Add before…"));

	visage::PopupMenuFrame* addMenu = newestPopup(column);
	CHECK(ui::menuOpenFor(column));
	CHECK(ui::lastMenuPlacementForTest().shown);
	REQUIRE(addMenu != nullptr);
	REQUIRE(addMenu != slotMenu);

	const device::Descriptor* chosen = column.addable().front();
	REQUIRE(pickRow(*addMenu, chosen->traits.name));
	REQUIRE(column.deviceCount() == count + 1);
	CHECK(held.bridge().positionOf(anchor) == anchorAt + 1);
	CHECK(held.bridge().positionOf(earlier) == anchorAt - 1);
	const DeviceInstanceId inserted = held.bridge().deviceAtPosition(anchorAt);
	CHECK(inserted != anchor);
	CHECK(std::string(held.bridge().heldTagOf(inserted).data()) == chosen->traits.tag);
}

namespace cascade {
// **"ADD AFTER…"** — the same picker at the same card,
// and the pick lands DIRECTLY BEHIND it: in the middle of a chain it goes before the next device, at the
// end it appends. Driven through the real road like "Add before…" above, and asserted as the whole chain
// in order — every device before the anchor unmoved, every one after it shifted by exactly one — so an
// insert on the wrong side of the anchor, or anywhere else, is a named identity in the wrong place.

/// The chain's identities, in order, from `at(i)` for `i < count`.
template <typename At>
std::vector<DeviceInstanceId> chainOf(size_t count, At at) {
	std::vector<DeviceInstanceId> out;
	for (size_t i = 0; i < count; ++i) {
		out.push_back(at(i));
	}
	return out;
}
/// `was` with `inserted` directly after position `anchor`.
std::vector<DeviceInstanceId> withAfter(std::vector<DeviceInstanceId> was, size_t anchor, DeviceInstanceId inserted) {
	was.insert(was.begin() + static_cast<std::ptrdiff_t>(anchor + 1), inserted);
	return was;
}
void fxLaneAddAfter(bool atEnd) {
	FilterFixture f;
	while (f.lane->deviceCount() < 3) {
		REQUIRE(f.lane->addChoiceForTest(0) < feathers::kMaxFxSlots);
	}
	const size_t count = f.lane->deviceCount();
	const size_t anchorSlot = atEnd ? count - 1 : 1;
	const auto at = [&](size_t i) { return f.bridge.fxDeviceAt(i); };
	const std::vector<DeviceInstanceId> was = chainOf(count, at);
	f.lane->resized();
	f.lane->scrollCardIntoView(anchorSlot);
	f.lane->resized();
	ui::FxCard* card = f.lane->cardAt(anchorSlot);
	REQUIRE(card != nullptr);

	press(f.root, *card, card->slotMenuBounds());
	visage::PopupMenuFrame* slotMenu = newestPopup(*f.lane);
	REQUIRE(slotMenu != nullptr);
	REQUIRE(pickRow(*slotMenu, "Add after…"));
	visage::PopupMenuFrame* addMenu = newestPopup(*f.lane);
	CHECK(ui::menuOpenFor(*f.lane));
	REQUIRE(addMenu != nullptr);
	REQUIRE(addMenu != slotMenu);

	const device::Descriptor* chosen = f.lane->addableForTest().front();
	REQUIRE(pickRow(*addMenu, chosen->traits.name));
	REQUIRE(f.lane->deviceCount() == count + 1);
	const DeviceInstanceId inserted = at(anchorSlot + 1);
	CHECK(chainOf(count + 1, at) == withAfter(was, anchorSlot, inserted));
	CHECK(std::find(was.begin(), was.end(), inserted) == was.end());
	CHECK(std::string(fxDeviceAtPosition(f.bridge.fxChain(), anchorSlot + 1).tag().data()) == chosen->traits.tag);
}
void rackAddAfter(bool atEnd) {
	namespace lfo = feathers::mod::lfo;
	FilterFixture f;
	while (f.panel->deviceCount() < 3) {
		REQUIRE(f.bridge.addRackDevice(lfo::kTraits.tag) < kMaxRackSlots);
		f.bridge.refresh();
	}
	f.panel->resized();
	const size_t count = f.panel->deviceCount();
	const size_t anchorSlot = atEnd ? count - 1 : 1;
	const auto at = [&](size_t i) { return f.bridge.rackDeviceAt(i); };
	const std::vector<DeviceInstanceId> was = chainOf(count, at);
	// THE LAST CARD'S DOTS ARE BELOW THE STRIP'S FOOT until it is scrolled to its end; the middle card's
	// are on screen at the top.
	f.panel->setScroll(atEnd ? f.panel->maxScroll() : 0.0f);
	const visage::Bounds dots = f.panel->slotMenuBounds(backplane::Source::output(at(anchorSlot), 0));
	REQUIRE(dots.y() >= 0.0f);
	REQUIRE(dots.bottom() <= f.panel->height());

	press(f.root, *f.panel, dots);
	REQUIRE(f.panel->lastSlotMenuForTest() == anchorSlot);
	visage::PopupMenuFrame* slotMenu = newestPopup(*f.panel);
	REQUIRE(slotMenu != nullptr);
	REQUIRE(pickRow(*slotMenu, "Add after…"));
	visage::PopupMenuFrame* addMenu = newestPopup(*f.panel);
	CHECK(ui::menuOpenFor(*f.panel));
	REQUIRE(addMenu != nullptr);
	REQUIRE(addMenu != slotMenu);

	REQUIRE(pickRow(*addMenu, f.panel->addableForTest().front()->traits.name));
	f.bridge.refresh();
	REQUIRE(f.panel->deviceCount() == count + 1);
	const DeviceInstanceId inserted = at(anchorSlot + 1);
	CHECK(chainOf(count + 1, at) == withAfter(was, anchorSlot, inserted));
	CHECK(std::find(was.begin(), was.end(), inserted) == was.end());
}
void columnAddAfter(bool atEnd) {
	HeldChain held;
	ui::WidgetRegistry registry;
	ui::ModState mod;
	ui::EditHistory history;
	ui::ChainColumn column(held.bridge(), registry, mod, history, nullptr);
	column.setBounds(0, 0, static_cast<int>(ui::FxLaneRail::kWidth), 1000);
	while (column.deviceCount() < 3) {
		REQUIRE(column.addChoiceForTest(0) != kNoInstance);
	}
	column.resized();
	const size_t count = column.deviceCount();
	const size_t anchorAt = atEnd ? count - 1 : 1;
	const auto at = [&](size_t i) { return held.bridge().deviceAtPosition(i); };
	const std::vector<DeviceInstanceId> was = chainOf(count, at);
	ui::FxCard* card = column.cardOf(was[anchorAt]);
	REQUIRE(card != nullptr);

	press(&column, *card, card->slotMenuBounds());
	visage::PopupMenuFrame* slotMenu = newestPopup(column);
	REQUIRE(slotMenu != nullptr);
	REQUIRE(pickRow(*slotMenu, "Add after…"));
	visage::PopupMenuFrame* addMenu = newestPopup(column);
	CHECK(ui::menuOpenFor(column));
	REQUIRE(addMenu != nullptr);
	REQUIRE(addMenu != slotMenu);

	const device::Descriptor* chosen = column.addable().front();
	REQUIRE(pickRow(*addMenu, chosen->traits.name));
	REQUIRE(column.deviceCount() == count + 1);
	const DeviceInstanceId inserted = at(anchorAt + 1);
	CHECK(chainOf(count + 1, at) == withAfter(was, anchorAt, inserted));
	CHECK(std::find(was.begin(), was.end(), inserted) == was.end());
	CHECK(std::string(held.bridge().heldTagOf(inserted).data()) == chosen->traits.tag);
}
} // namespace cascade

TEST_CASE("Add after: the fx lane's pick lands directly behind a card in the MIDDLE of the chain") {
	cascade::fxLaneAddAfter(false);
}
TEST_CASE("Add after: the fx lane's pick behind the LAST card appends") {
	cascade::fxLaneAddAfter(true);
}
TEST_CASE("Add after: the rack strip's pick lands directly behind a card in the MIDDLE of the rack") {
	cascade::rackAddAfter(false);
}
TEST_CASE("Add after: the rack strip's pick behind the LAST card appends") {
	cascade::rackAddAfter(true);
}
TEST_CASE("Add after: SKLITTER's chain column's pick lands directly behind a card in the MIDDLE of the chain") {
	cascade::columnAddAfter(false);
}
TEST_CASE("Add after: SKLITTER's chain column's pick behind the LAST card appends") {
	cascade::columnAddAfter(true);
}
TEST_CASE("Add after is offered beside Add before, and both stay live past every chain's starting size") {
	// THE SAME ROOM RULE AS "Add before", on every surface — and since unbounded devices (step 6) the rule is
	// that there is always room: past the size each chain was built with, both verbs are still offered. (This
	// case greyed both at a full chain while chains stopped.)
	FilterFixture f;
	const auto rowsOf = [](const auto& options, auto before, auto after) {
		REQUIRE(options.size() >= 2);
		CHECK(options[0].verb == before);
		CHECK(options[1].verb == after);
		return std::pair { options[0].enabled, options[1].enabled };
	};
	using LaneVerb = ui::FxLane::SlotVerb;
	CHECK(rowsOf(f.lane->slotOptionsForTest(0), LaneVerb::AddBefore, LaneVerb::AddAfter) == std::pair { true, true });
	while (f.lane->deviceCount() <= feathers::kMaxFxSlots) {
		REQUIRE(f.lane->addChoiceForTest(0) != feathers::kNoPosition);
	}
	CHECK(rowsOf(f.lane->slotOptionsForTest(0), LaneVerb::AddBefore, LaneVerb::AddAfter) == std::pair { true, true });

	using RackVerb = ui::ModPanel::SlotVerb;
	CHECK(rowsOf(f.panel->slotOptionsForTest(0), RackVerb::AddBefore, RackVerb::AddAfter) == std::pair { true, true });
	while (f.bridge.modRack().count <= kMaxRackSlots) { // the BRIDGE's count: the panel's follows at its next sync
		REQUIRE(f.bridge.addRackDevice(feathers::mod::lfo::kTraits.tag) != feathers::kNoPosition);
		f.bridge.refresh();
	}
	CHECK(rowsOf(f.panel->slotOptionsForTest(0), RackVerb::AddBefore, RackVerb::AddAfter) == std::pair { true, true });

	HeldChain held;
	ui::WidgetRegistry registry;
	ui::ModState mod;
	ui::EditHistory history;
	ui::ChainColumn column(held.bridge(), registry, mod, history, nullptr);
	using ColumnVerb = ui::ChainColumn::SlotVerb;
	const DeviceInstanceId first = column.addChoiceForTest(0);
	REQUIRE(first != kNoInstance);
	CHECK(rowsOf(column.slotOptionsFor(first), ColumnVerb::AddBefore, ColumnVerb::AddAfter) == std::pair { true, true });
	const size_t built = held.bridge().capacity(); // read ONCE: it grows as the loop adds
	for (size_t i = 0; i <= built; ++i) {
		REQUIRE(column.addChoiceForTest(0) != kNoInstance);
	}
	CHECK(rowsOf(column.slotOptionsFor(first), ColumnVerb::AddBefore, ColumnVerb::AddAfter) == std::pair { true, true });
}
TEST_CASE("GG caret: every value box that opens a menu draws a dropdown caret, no other box does, and it never "
          "sits on the text") {
	// 2026-09-24, asked whether the menu boxes should say so: *yes*. `ValueBox::caretBounds` is
	// where the caret is drawn and is empty for a box that opens nothing, so the claim is a value.
	// Walked over the WHOLE editor with a modulator in the strip and several devices in the lane, so a
	// box on any surface answers it — and asserted non-vacuous both ways.
	namespace lfo = feathers::mod::lfo;
	namespace dly = feathers::fx::delay;
	FilterFixture f;
	REQUIRE(f.bridge.addRackDevice(lfo::kTraits.tag) < kMaxRackSlots);
	for (const char* tag : { dly::kTraits.tag, fx::harm::kTraits.tag, fx::owltt::kTraits.tag }) {
		REQUIRE(f.bridge.addFxDevice(tag) < feathers::kMaxFxSlots);
	}
	f.bridge.refresh();
	f.panel->resized();
	f.lane->resized();

	std::vector<ui::ValueBox*> boxes;
	std::function<void(visage::Frame*)> collect = [&](visage::Frame* frame) {
		if (auto* box = dynamic_cast<ui::ValueBox*>(frame)) {
			boxes.push_back(box);
		}
		for (visage::Frame* child : frame->children()) {
			collect(child);
		}
	};
	collect(f.root);

	size_t menus = 0;
	size_t plain = 0;
	const visage::Font face = ui::font(ui::kKnobReadoutSize);
	for (ui::ValueBox* box : boxes) {
		if (box->width() <= 0.0f) {
			continue;
		}
		const device::ParamDesc& desc = box->control().desc();
		CAPTURE(std::string(desc.name));
		const visage::Bounds caret = box->caretBounds();
		CHECK((caret.width() > 0.0f) == box->opensMenu());
		if (!box->opensMenu()) {
			++plain;
			continue;
		}
		++menus;
		// INSIDE THE BOX, and right of all the room the text on its row is given — the value's in a
		// compact box, the caption's in a stacked one (whose value keeps the whole width).
		const ui::ValueBox::ReadoutBox room = box->readoutBox();
		const ui::ValueBox::ReadoutBox row = box->isCompact() ? room : box->captionBox();
		CHECK(caret.x() >= row.x + row.width);
		CHECK(caret.right() <= box->width());
		CHECK(caret.y() >= 0.0f);
		CHECK(caret.bottom() <= box->height());
		if (!box->isCompact()) {
			const std::string caption = box->label();
			CAPTURE(caption);
			CHECK(ui::font(ui::kKnobLabelSize).stringWidth(std::u32string(caption.begin(), caption.end()))
			      <= row.width + 0.5f);
		}
		// AND EVERY FACE THE BOX CAN SHOW FITS THAT ROOM — the caret costs no name its space.
		const double before = box->control().value();
		for (size_t i = 0; i < box->entryCount(); ++i) {
			box->control().setValue(desc.min + static_cast<double>(i));
			const std::string text = box->readout();
			CAPTURE(text);
			CHECK(face.stringWidth(std::u32string(text.begin(), text.end())) <= room.width + 0.5f);
		}
		box->control().setValue(before);
	}
	CHECK(menus >= 4);  // the LFO's two, the delay's Mode, the DAHDSR's Mode at least
	CHECK(plain >= 10); // and plenty of ordinary number boxes beside them
}

namespace {
//--------------------------------------------------------------------------------------------------//
// THE SMOOTH SHAPER'S CARD AND PANEL — the picture is asserted as values, and so is its delivery.     //
//--------------------------------------------------------------------------------------------------//
namespace smth = feathers::fx::smooth;
smth::Settings smoothSettingsOf(const DynCardFixture<ui::SmoothCard>& d) {
	smth::Row row {};
	for (size_t i = 0; i < row.size(); ++i) {
		row[i] = d.f.deviceHost.fieldAt(d.device, i);
	}
	return smth::stateOf(row, row.size()).settings;
}
} // namespace

TEST_CASE("SMOOTH SHAPER: the card's curve is the core at the card's settings, and the ceiling is where it saturates") {
	DynCardFixture<ui::SmoothCard> d(smth::kTraits.tag);
	ui::SmoothCard* card = d.card;
	ui::SmoothCurveView* curve = card->curveForTest();
	REQUIRE(curve != nullptr);
	for (const int mode : { 0, 2, 7 }) {
		d.set(static_cast<size_t>(smth::Param::HarmonicMode), static_cast<float>(mode));
		d.set(static_cast<size_t>(smth::Param::Drive), 0.4f);
		(void)card->updateContent();
		CAPTURE(mode);
		const smth::Settings s = smoothSettingsOf(d);
		REQUIRE(s.harmonicMode == mode);
		// EVERY POINT DRAWN IS THE CORE'S OUTPUT for the card's own settings — read back from the host, not from
		// the model, so a card that drew someone else's settings would be caught.
		const smth::Picture p = smth::pictureOf(s);
		const ui::SmoothPictureModel& m = card->modelForTest();
		for (int i = 0; i < ui::SmoothPictureModel::kCurvePoints; i += 7) {
			CHECK(m.curve[static_cast<size_t>(i)] == smth::transferAt(p, ui::SmoothPictureModel::inputAt(i)));
		}
		// THE CEILING'S x: 0.5 / drive for the Chebyshevs, 1 / drive for Poly and as the FM reference.
		const float g = smth::driveGainOf(smth::firmwareControlsOf(s).drive);
		const float want = (mode == 0 ? 0.5f : 1.0f) / g;
		CHECK(curve->ceilingX() == doctest::Approx(curve->xOfInput(std::min(want, 1.0f))).epsilon(1e-5));
		CHECK(curve->ceilingOffScale() == (want > 1.0f));
	}
}
TEST_CASE("SMOOTH SHAPER: dragging the ceiling writes Drive, a double-click resets it, and a press elsewhere writes nothing") {
	DynCardFixture<ui::SmoothCard> d(smth::kTraits.tag);
	ui::SmoothCard* card = d.card;
	ui::SmoothCurveView* curve = card->curveForTest();
	(void)card->updateContent();
	const size_t drive = static_cast<size_t>(smth::Param::Drive);
	REQUIRE(d.f.deviceHost.fieldAt(d.device, drive) == 0.5f);
	const visage::Bounds plot = curve->plotBounds();
	const float midY = plot.y() + plot.height() * 0.5f;

	// A PRESS ON THE CURVE AWAY FROM THE HANDLE: nothing moves.
	curve->processMouseDown(at(curve->xOfInput(0.1f), midY));
	curve->processMouseUp(at(curve->xOfInput(0.1f), midY));
	CHECK(d.f.deviceHost.fieldAt(d.device, drive) == 0.5f);

	// GRAB THE CEILING (3579 at unity: input 0.5) AND PULL IT IN TO 0.25 — twice the drive, fader √2 / 2.
	REQUIRE(curve->grabsDrive({ curve->ceilingX(), midY }));
	curve->processMouseDown(at(curve->ceilingX(), midY));
	CHECK(curve->draggingForTest());
	curve->processMouseDrag(at(curve->xOfInput(0.25f), midY));
	curve->processMouseUp(at(curve->xOfInput(0.25f), midY));
	CHECK_FALSE(curve->draggingForTest());
	const float dragged = d.f.deviceHost.fieldAt(d.device, drive);
	CHECK(dragged == doctest::Approx(smth::driveKnobForCeiling(0, 0.25f)).epsilon(1e-3));
	CHECK(dragged == doctest::Approx(std::sqrt(2.0) / 2.0).epsilon(1e-3));
	// AND THE PICTURE FOLLOWS: the ceiling is now drawn at 0.25.
	(void)card->updateContent();
	CHECK(curve->ceilingX() == doctest::Approx(curve->xOfInput(0.25f)).epsilon(1e-3));

	// THE MIRROR LINE ON THE NEGATIVE SIDE IS THE SAME HANDLE.
	curve->processMouseDown(at(curve->xOfInput(-0.25f), midY));
	curve->processMouseDrag(at(curve->xOfInput(-0.5f), midY));
	curve->processMouseUp(at(curve->xOfInput(-0.5f), midY));
	CHECK(d.f.deviceHost.fieldAt(d.device, drive) == doctest::Approx(0.5f).epsilon(1e-4));

	// A DOUBLE-CLICK ON THE HANDLE IS THE DEFAULT, as on every knob.
	d.set(drive, 0.7f);
	(void)card->updateContent();
	curve->processMouseDown(doubleClickAt(curve->ceilingX(), midY));
	CHECK(d.f.deviceHost.fieldAt(d.device, drive) == 0.5f);
}
TEST_CASE("SMOOTH SHAPER: a bare click on the ceiling writes nothing, and a drag moves it by the hand's travel from where it was") {
	DynCardFixture<ui::SmoothCard> d(smth::kTraits.tag);
	ui::SmoothCard* card = d.card;
	ui::SmoothCurveView* curve = card->curveForTest();
	const size_t drive = static_cast<size_t>(smth::Param::Drive);
	// POLY BELOW UNITY: fader 0.375 is a gain of 4 · 0.375² = 0.5625, so the ceiling is at 1/0.5625 = 1.78 — off the plotted
	// span, drawn pinned at its edge. A press there under the old road set Drive for a ceiling AT the edge.
	d.set(static_cast<size_t>(smth::Param::HarmonicMode), 7.0f);
	d.set(drive, 0.375f);
	(void)card->updateContent();
	REQUIRE(curve->ceilingOffScale());
	const float stored = smth::ceilingInputOf(7, 0.375f);
	REQUIRE(stored == doctest::Approx(1.0f / 0.5625f).epsilon(1e-5));
	const visage::Bounds plot = curve->plotBounds();
	const float midY = plot.y() + plot.height() * 0.5f;
	EditHistory* history = d.f.gui.history();
	REQUIRE(history != nullptr);

	// A PRESS AND A RELEASE WITH NO MOVEMENT — a few pixels off the line, inside the grab slop: no value
	// written and no undo step.
	const float grabX = curve->ceilingX() - 3.0f;
	REQUIRE(curve->grabsDrive({ grabX, midY }));
	const size_t before = history->length();
	curve->processMouseDown(at(grabX, midY));
	CHECK(curve->draggingForTest());
	CHECK(d.f.deviceHost.fieldAt(d.device, drive) == 0.375f);
	curve->processMouseUp(at(grabX, midY));
	CHECK(d.f.deviceHost.fieldAt(d.device, drive) == 0.375f);
	CHECK(history->length() == before);

	// A DRAG INWARD BY 0.1 OF INPUT FROM THAT GRAB: the ceiling moves from 1.78 to 1.68 — anchored to the
	// stored value, not to the plot edge (1.0) it is drawn at, and by the hand's travel, not to the pointer.
	curve->processMouseDown(at(grabX, midY));
	const float toX = grabX - (curve->xOfInput(0.1f) - curve->xOfInput(0.0f));
	curve->processMouseDrag(at(toX, midY));
	curve->processMouseUp(at(toX, midY));
	CHECK(d.f.deviceHost.fieldAt(d.device, drive) ==
	      doctest::Approx(smth::driveKnobForCeiling(7, stored - 0.1f)).epsilon(1e-3));
	CHECK(smth::ceilingInputOf(7, d.f.deviceHost.fieldAt(d.device, drive)) == doctest::Approx(stored - 0.1f).epsilon(1e-3));
	CHECK(history->length() == before + 1);
}
TEST_CASE("SMOOTH SHAPER: the card recomputes and redraws when a setting or the reading moves, and at rest does neither") {
	DynCardFixture<ui::SmoothCard> d(smth::kTraits.tag);
	ui::SmoothCard* card = d.card;
	REQUIRE(card->isVisible());
	REQUIRE(card->isDrawing());
	(void)card->updateContent();
	RedrawProbe probe(card);
	const uint32_t settled = card->modelForTest().computations;

	// AT REST: no computation, no frame, however many ticks.
	for (int i = 0; i < 5; ++i) {
		CHECK_FALSE(card->updateContent());
	}
	CHECK(probe.count == 0);
	CHECK(card->modelForTest().computations == settled);

	// A MODE CHANGE: one computation, one frame.
	d.set(static_cast<size_t>(smth::Param::HarmonicMode), 3.0f);
	probe.rearm();
	CHECK(card->updateContent());
	CHECK(probe.count == 1);
	CHECK(card->modelForTest().computations == settled + 1);
	CHECK(card->modelForTest().picture.zone == 3);

	// THE INPUT PEAK PUBLISHED (the bus): a frame, and no recomputation — the marker is not a setting.
	std::array<float, static_cast<size_t>(smth::Reading::kCount)> v { 0.8f };
	d.publish(v);
	probe.rearm();
	CHECK(card->updateContent());
	CHECK(probe.count == 2);
	CHECK(card->modelForTest().computations == settled + 1);
	CHECK(card->hasReadings());
	CHECK(card->inputPeak() == 0.8f);

	// AND STILL AGAIN.
	probe.rearm();
	CHECK_FALSE(card->updateContent());
	CHECK(probe.count == 2);
}
TEST_CASE("SMOOTH SHAPER: the hand-drawn parts speak for their controls — the ceiling for Drive, the bars for Harmonic") {
	DynCardFixture<ui::SmoothCard> d(smth::kTraits.tag);
	ui::SmoothCard* card = d.card;
	(void)card->updateContent();
	ui::SmoothCurveView* curve = card->curveForTest();
	const visage::Bounds plot = curve->plotBounds();
	const char* onHandle = curve->tooltipAt({ curve->ceilingX(), plot.y() + 4.0f });
	// A TIP IS A CONTROL'S DECLARATION (the sweep's rule): the ceiling is Drive, the bars are what Harmonic moves.
	REQUIRE(onHandle != nullptr);
	CHECK(std::string(onHandle) == smth::kParamDescs[static_cast<size_t>(smth::Param::Drive)].tooltip);
	CHECK(curve->tooltipAt({ curve->xOfInput(0.05f), plot.y() + plot.height() * 0.5f }) == nullptr);
	// THE BARS ARE THE PANEL'S NOW, so they are asked there.
	ModFixture f;
	const size_t slot = f.bridge.addFxDevice(smth::kTraits.tag);
	REQUIRE(slot < feathers::kMaxFxSlots);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, f.bridge.fxDeviceAt(slot), smth::kDescriptor);
	auto* panel = dynamic_cast<ui::SmoothExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	panel->setBounds(0.0f, 0.0f, panel->minimumSize().x, panel->minimumSize().y);
	(void)panel->updateContent();
	ui::SmoothHarmonicsView* bars = panel->barsForTest();
	const visage::Bounds third = bars->barBounds(2);
	const char* onBar = bars->tooltipAt({ third.x() + third.width() * 0.5f, bars->barsArea().bottom() - 1.0f });
	REQUIRE(onBar != nullptr);
	CHECK(std::string(onBar) == smth::kParamDescs[static_cast<size_t>(smth::Param::Harmonic)].tooltip);
}
TEST_CASE("SMOOTH SHAPER: the card's picture keeps its native size through a scale change") {
	// CLAUDE.md's DPI rule: a scale change moves nothing logical, so a layout guarded on the logical rectangle
	// would keep the old scale's native size. `layOut` re-sets unconditionally; asserted on the NATIVE width.
	DynCardFixture<ui::SmoothCard> d(smth::kTraits.tag);
	ui::SmoothCard* card = d.card;
	ui::SmoothCurveView* curve = card->curveForTest();
	for (const float scale : { 1.75f, 1.0f, 1.5f }) {
		d.f.root->setDpiScale(scale);
		card->resized();
		CAPTURE(scale);
		CHECK(std::abs(static_cast<float>(curve->nativeWidth()) - scale * static_cast<float>(curve->width())) <= 1.0f);
		CHECK(std::abs(static_cast<float>(card->driveForTest()->nativeWidth())
		               - scale * static_cast<float>(card->driveForTest()->width())) <= 1.0f);
	}
}
TEST_CASE("SMOOTH SHAPER: the panel draws the same picture from the same settings, and holds every control") {
	ModFixture f;
	const size_t slot = f.bridge.addFxDevice(smth::kTraits.tag);
	REQUIRE(slot < feathers::kMaxFxSlots);
	const DeviceInstanceId device = f.bridge.fxDeviceAt(slot);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, device, smth::kDescriptor);
	REQUIRE(view != nullptr);
	auto* panel = dynamic_cast<ui::SmoothExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	f.bridge.setDeviceControl(device, static_cast<size_t>(smth::Param::HarmonicMode), 7.0f);
	f.bridge.refresh();
	panel->setBounds(0.0f, 0.0f, panel->minimumSize().x, panel->minimumSize().y);
	(void)panel->updateContent();
	CHECK(panel->modelForTest().picture.zone == 7);
	CHECK(panel->modelForTest().picture.clips);
	// EVERY CONTROL THE DEVICE DECLARES IS ON THE PANEL.
	std::set<size_t> reached;
	for (const smth::Param p : panel->addressedParamsForTest()) {
		reached.insert(static_cast<size_t>(p));
	}
	// AND `Enabled` IN THE WINDOW'S HEADER CHIP, which every window carries for its device (2026-09-26).
	REQUIRE(panel->deviceEnableControl() == static_cast<size_t>(smth::Param::Enabled));
	reached.insert(panel->deviceEnableControl());
	CHECK(reached.size() == static_cast<size_t>(smth::Param::kCount));
	// CAPPED BOTH WAYS near its natural size — every cell is fixed.
	CHECK(panel->maximumSize().x < ui::ExpandedView::kNoCap);
	CHECK(panel->maximumSize().y < ui::ExpandedView::kNoCap);
}
TEST_CASE("SMOOTH SHAPER: HP Wet is a knob over its control, Sub Thru a chip that is live only while the filter is in the signal") {
	ModFixture f;
	const size_t slot = f.bridge.addFxDevice(smth::kTraits.tag);
	REQUIRE(slot < feathers::kMaxFxSlots);
	const DeviceInstanceId device = f.bridge.fxDeviceAt(slot);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, device, smth::kDescriptor);
	auto* panel = dynamic_cast<ui::SmoothExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	panel->setBounds(0.0f, 0.0f, panel->minimumSize().x, panel->minimumSize().y);
	(void)panel->updateContent();
	ui::WetHighpassControl* hp = panel->hpForTest();
	REQUIRE(hp != nullptr);
	// HERTZ ONLY: this device has no tracked note to give semitones from.
	CHECK(hp->semitones() == nullptr);
	CHECK(hp->hertz()->isVisible());
	CHECK(&hp->hertz()->control()->desc() == &smth::kParamDescs[static_cast<size_t>(smth::Param::HpWet)]);
	CHECK(&hp->subThru()->control().desc() == &smth::kParamDescs[static_cast<size_t>(smth::Param::SubThru)]);
	// AT THE BOTTOM THERE IS NO FILTER: Sub Thru has nothing to give back and is greyed.
	CHECK_FALSE(hp->subThru()->enabled());
	f.bridge.setDeviceControl(device, static_cast<size_t>(smth::Param::HpWet), 9.0f);
	f.bridge.refresh();
	(void)panel->updateContent();
	CHECK(hp->subThru()->enabled());
	// AT MIX 0 THE DEVICE PASSES THE INPUT: nothing to give back there either.
	f.bridge.setDeviceControl(device, static_cast<size_t>(smth::Param::Mix), 0.0f);
	f.bridge.refresh();
	(void)panel->updateContent();
	CHECK_FALSE(hp->subThru()->enabled());
	f.bridge.setDeviceControl(device, static_cast<size_t>(smth::Param::Mix), 1.0f);
	f.bridge.setDeviceControl(device, static_cast<size_t>(smth::Param::HpWet), static_cast<float>(feathers::fx::kWetHighpassLog2Min));
	f.bridge.refresh();
	(void)panel->updateContent();
	CHECK_FALSE(hp->subThru()->enabled());
}

namespace {
/// EVERY PLACE A PRESS ON THIS BOX'S READOUT CAN GRAB, swept pixel by pixel across the whole box — so the case
/// needs no knowledge of where the text sits.
std::set<double> placesOf(const ui::ValueBox& box) {
	std::set<double> places;
	for (float x = 0.0f; x <= box.width(); x += 0.25f) {
		const double p = box.placeAt(x);
		if (p != 0.0) {
			places.insert(std::round(p * 1.0e6) / 1.0e6);
		}
	}
	return places;
}
} // namespace

TEST_CASE("SMOOTH SHAPER: Harmonic and Twist are unbounded coordinates with coarse and fine digits, one control on the card and the panel") {
	// The zone knobs lacked the coarse.fine click zones, and since within a zone they drive phi triangles,
	// they are unbounded rather than clipped at 100%.
	DynCardFixture<ui::SmoothCard> d(smth::kTraits.tag);
	ui::SmoothCard* card = d.card;
	const size_t harmonic = static_cast<size_t>(smth::Param::Harmonic);
	const size_t twist = static_cast<size_t>(smth::Param::Sweep);
	d.set(harmonic, 12.375f);
	d.set(twist, 3.625f);
	(void)card->updateContent();
	// NOT CLIPPED: past a lap the host holds what was set.
	CHECK(d.f.deviceHost.fieldAt(d.device, harmonic) == 12.375f);
	CHECK(d.f.deviceHost.fieldAt(d.device, twist) == 3.625f);

	auto* hCell = dynamic_cast<ui::ZoneCoordCell*>(card->widgetForTest(smth::Param::Harmonic));
	auto* tCell = dynamic_cast<ui::ZoneCoordCell*>(card->widgetForTest(smth::Param::Sweep));
	REQUIRE(hCell != nullptr);
	REQUIRE(tCell != nullptr);
	for (ui::ZoneCoordCell* cell : { hCell, tCell }) {
		CAPTURE(cell->box().readout());
		const std::set<double> places = placesOf(cell->box());
		// THE COARSE DIGIT MOVES LAPS AND THE FINE ONE TENTHS OF ONE — the "click zones".
		CHECK(places.count(1.0) == 1);
		CHECK(places.count(0.1) == 1);
		CHECK(places.size() >= 3);
	}
	// NEITHER NAMES ANYTHING (the pills are the field modifiers, so no dropdowns): the Mode menu
	// names the waveshaper, Twist is the sweep alone, and each cell marks its lap instead.
	for (ui::ZoneCoordCell* cell : { hCell, tCell }) {
		CHECK_FALSE(cell->hasNames());
		CHECK(cell->sigil() != nullptr);
		CHECK_FALSE(cell->select().isVisible());
	}
	// THE HARMONIC'S TEXT IS THE MODE'S ROW OF THE TABLE (`smooth_tips.h`), for the mode the card is drawn in.
	CHECK(std::string(hCell->tooltipAt({ 1.0f, 1.0f })) == std::string(smth::modeTipsFor(card->drawnFromForTest().harmonicMode).harmonic));
	CHECK(std::string(tCell->tooltipAt({ 1.0f, 1.0f })) == smth::kParamDescs[twist].tooltip);

	// THE PANEL SEATS THE SAME TWO, THE SAME CONTROL (no dropdown there either), AND T PHASE IS GONE FROM IT.
	ModFixture f;
	const size_t slot = f.bridge.addFxDevice(smth::kTraits.tag);
	REQUIRE(slot < feathers::kMaxFxSlots);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, f.bridge.fxDeviceAt(slot), smth::kDescriptor);
	REQUIRE(view != nullptr);
	auto* panel = dynamic_cast<ui::SmoothExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	panel->setBounds(0.0f, 0.0f, panel->minimumSize().x, panel->minimumSize().y);
	std::vector<ui::ZoneCoordCell*> cells;
	collectZoneCells(panel, &cells);
	std::set<std::string> keys;
	for (ui::ZoneCoordCell* cell : cells) {
		keys.insert(cell->box().control().desc().key);
		CHECK_FALSE(cell->hasNames());
		CHECK(cell->sigil() != nullptr);
	}
	CHECK(keys == std::set<std::string> { "smooth_harmonic_coord", "smooth_sweep" });
}
TEST_CASE("SMOOTH SHAPER: each pill's click is its power and keeps the amount — off and back on is the last amount, heard again") {
	// Width, Evens, Rect and Fdbk are FaderPills, the Disperser's. A click toggles the power and
	// leaves the amount; a drag sets the amount. VALUES: the device's row, and what the shaper hears (`twistOf`).
	DynCardFixture<ui::SmoothCard> d(smth::kTraits.tag);
	d.set(static_cast<size_t>(smth::Param::Harmonic), 0.3f); // Evens' positive half; Width at full depth
	(void)d.card->updateContent();
	const auto heard = [&](smth::Pill p) {
		const smth::TwistParams t = smth::twistOf(smth::firmwareControlsOf(smoothSettingsOf(d)));
		switch (p) {
		case smth::Pill::Width: return t.stereoWidth;
		case smth::Pill::Evens: return t.evenAmount + t.evenDryBlend;
		case smth::Pill::Rect: return t.rectAmount;
		default: return 4.0f * t.feedbackAmount;
		}
	};
	const visage::Bounds body { d.card->bodyLeft(), 0.0f, d.card->bodyWidth(), d.card->height() };
	for (size_t i = 0; i < smth::kPillCount; ++i) {
		const auto pill = static_cast<smth::Pill>(i);
		const size_t amount = static_cast<size_t>(smth::pillAmountParam(pill));
		const size_t power = static_cast<size_t>(smth::pillOnParam(pill));
		CAPTURE(smth::kPillNames[i]);
		ui::FaderPill* widget = d.card->pillForTest(i);
		REQUIRE(widget != nullptr);
		CHECK(widget->isVisible());
		CHECK(widget->bounds().right() <= body.right() + 0.5f);
		CHECK(widget->bounds().bottom() <= d.card->height() + 0.5f);
		const float cx = widget->width() * 0.5f;
		const float cy = widget->height() * 0.5f;
		const auto click = [&] {
			widget->onMouseDown().callback(at(cx, cy));
			widget->onMouseUp().callback(at(cx, cy));
		};
		REQUIRE(d.f.deviceHost.fieldAt(d.device, power) == 0.0f);
		CHECK(heard(pill) == 0.0f);
		// A DRAG SETS THE AMOUNT (0.5 -> 0.75) AND LEAVES THE POWER OFF: still nothing heard.
		const float quarter = 0.25f * ui::FaderPill::kSpan;
		widget->onMouseDown().callback(at(cx, cy));
		widget->onMouseDrag().callback(at(cx + quarter, cy));
		widget->onMouseUp().callback(at(cx + quarter, cy));
		CHECK(d.f.deviceHost.fieldAt(d.device, amount) == doctest::Approx(0.75f));
		CHECK(d.f.deviceHost.fieldAt(d.device, power) == 0.0f);
		CHECK(heard(pill) == 0.0f);
		// ON: heard, at the amount (Rect up its triangle, Fdbk a quarter of it — both near the amount itself).
		click();
		CHECK(d.f.deviceHost.fieldAt(d.device, power) == 1.0f);
		const float on = heard(pill);
		CHECK(on == doctest::Approx(0.75f).epsilon(1e-3));
		// OFF: silent, the amount kept.
		click();
		CHECK(d.f.deviceHost.fieldAt(d.device, power) == 0.0f);
		CHECK(d.f.deviceHost.fieldAt(d.device, amount) == doctest::Approx(0.75f));
		CHECK(heard(pill) == 0.0f);
		// BACK ON: the LAST amount, heard exactly as before.
		click();
		CHECK(heard(pill) == on);
		click();
	}
	// NO TWO PILLS OVERLAP, AND NONE SITS ON TWIST OR ITS POWER.
	std::vector<visage::Bounds> boxes { d.card->widgetForTest(smth::Param::Sweep)->bounds(),
		                                d.card->widgetForTest(smth::Param::SweepOn)->bounds() };
	for (size_t i = 0; i < smth::kPillCount; ++i) {
		boxes.push_back(d.card->pillForTest(i)->bounds());
	}
	for (size_t i = 0; i < boxes.size(); ++i) {
		for (size_t j = i + 1; j < boxes.size(); ++j) {
			const visage::Bounds a = boxes[i];
			const visage::Bounds b = boxes[j];
			CHECK((a.right() <= b.x() || b.right() <= a.x() || a.bottom() <= b.y() || b.bottom() <= a.y()));
		}
	}
	// AND THE WINDOW HAS THEM ALL, inside it.
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(d.f.deviceHost, &d.f.bridge, registry, d.device, smth::kDescriptor);
	auto* panel = dynamic_cast<ui::SmoothExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	panel->setBounds(0.0f, 0.0f, panel->minimumSize().x, panel->minimumSize().y);
	for (size_t i = 0; i < smth::kPillCount; ++i) {
		REQUIRE(panel->pillForTest(i) != nullptr);
		CHECK(panel->pillForTest(i)->bounds().bottom() <= panel->height());
		CHECK(panel->pillForTest(i)->bounds().right() <= panel->width());
	}
}

namespace {
/// THE FILTER CARD'S KNOB FOR THE CONTROL WHOSE KEY IS `key`, asked of the knob's own control.
ui::Knob* filterKnob(ui::FilterCard& card, const char* key) {
	for (const std::unique_ptr<ui::Knob>& k : card.knobsForTest()) {
		if (k->control() != nullptr && k->control()->desc().key != nullptr
		    && std::string(k->control()->desc().key) == key) {
			return k.get();
		}
	}
	return nullptr;
}
/// A TARGET NO DEVICE HOLDS, for filling the pool: the pool's bookkeeping does not care, and nothing here
/// reconciles between the fill and the ask.
hs::Target synthetic(uint32_t n) {
	hs::Target t;
	t.path = { static_cast<feathers::DeviceInstanceId>(90000 + n) };
	t.key = "fill";
	t.tag = "fill";
	t.label = "Fill " + std::to_string(n);
	return t;
}
} // namespace

TEST_CASE("Expose to host pins the control and the host sees its name") {
	FilterFixture f;
	emptyPool(f.bridge);
	ui::Knob* knob = filterKnob(*f.card, "filter_cutoff_wide");
	REQUIRE(knob != nullptr);
	auto* control = dynamic_cast<ui::DeviceControl*>(knob->control());
	REQUIRE(control != nullptr);
	BridgeLookup lookup { f.bridge };
	hs::SlotHost host { f.bridge.slotPool(), lookup };
	(void)visit(f.bridge, host); // whatever the fixture's setup owed, paid before the gesture
	(void)f.bridge.takeStateDirty();

	// THE MENU: nothing routed here (the fixture routes nothing), so the host row alone — and a right-click
	// that used to fall through to a drag now opens it.
	CHECK(control->hostSlot().state == feathers::device::HostSlotStatus::State::Unslotted);
	std::optional<visage::PopupMenu> menu = knob->contextMenu();
	REQUIRE(menu.has_value());
	std::vector<SeenRow> rows = rowsOf(*menu);
	REQUIRE(rows.size() == 1);
	CHECK(rows[0].label == "Expose to host");
	CHECK(rows[0].enabled);
	CHECK_FALSE(knob->hostMarked());

	// THE POPUP'S OWN PICK.
	menu->onSelection().callback(rows[0].id);

	// THE POOL: slot 0 — the first Empty, the pool's documented order — Pinned, naming this control.
	const std::optional<hs::Target> target = f.bridge.slotTargetOf(control->device(), control->control());
	REQUIRE(target.has_value());
	const hs::SlotIndex slot = f.bridge.slotPool().slotOf(*target);
	CHECK(slot == 0u);
	REQUIRE(slot != hs::kNoSlot);
	CHECK(f.bridge.slotPool().state(slot) == hs::SlotState::Pinned);
	CHECK(f.bridge.slotPool().target(slot).key == "filter_cutoff_wide");
	CHECK(f.bridge.takeStateDirty()); // the pool is saved with the project: a pin is an edit
	CHECK(knob->hostMarked());

	// THE HOST: nothing is sent from the gesture; the next visit owes ONE rescan, of the names and texts.
	const clap_param_rescan_flags owed = visit(f.bridge, host);
	CHECK((owed & CLAP_PARAM_RESCAN_INFO) != 0);
	CHECK((owed & CLAP_PARAM_RESCAN_TEXT) != 0);
	CHECK((owed & CLAP_PARAM_RESCAN_ALL) == 0);
	CHECK(visit(f.bridge, host) == 0u); // and a visit with nothing new owes nothing
	clap_param_info info {};
	REQUIRE(host.info(slot, &info));
	CHECK(std::string(info.name) == "Filter " + kSlotDot + " Cutoff");
	CHECK((info.flags & CLAP_PARAM_IS_HIDDEN) == 0);
	// AND THE ENGINE DRIVES IT: the record binds the slot to this control after the visit.
	CHECK(f.bridge.slotBoundTo(control->device(), control->control()) == slot);

	// THE MENU NOW SAYS WHERE IT IS, and offers the one verb a Pinned slot admits.
	menu = knob->contextMenu();
	REQUIRE(menu.has_value());
	rows = rowsOf(*menu);
	REQUIRE(rows.size() == 2);
	CHECK(rows[0].label == "Host: Slot 1 " + kSlotDot + " Pinned");
	CHECK_FALSE(rows[0].enabled);
	CHECK(rows[1].label == "Unpin");
	CHECK(rows[1].enabled);
}
TEST_CASE("a host slot reads a typed value back through its target's unit (backlog LS)") {
	// `params.text_to_value` FOR A SLOT WAS `false` — the DAW's own type-in on a device control did nothing.
	// It reads through `device::parseValue` now, the inverse of the `formatValue` its value_to_text prints.
	FilterFixture f;
	emptyPool(f.bridge);
	ui::Knob* knob = filterKnob(*f.card, "filter_cutoff_wide");
	REQUIRE(knob != nullptr);
	auto* control = dynamic_cast<ui::DeviceControl*>(knob->control());
	REQUIRE(control != nullptr);
	BridgeLookup lookup { f.bridge };
	hs::SlotHost host { f.bridge.slotPool(), lookup };
	REQUIRE(control->exposeToHost(nullptr));
	(void)visit(f.bridge, host);
	const std::optional<hs::Target> target = f.bridge.slotTargetOf(control->device(), control->control());
	REQUIRE(target.has_value());
	const hs::SlotIndex slot = f.bridge.slotPool().slotOf(*target);
	REQUIRE(slot != hs::kNoSlot);
	const clap_id id = hs::slotId(slot);
	const feathers::device::ParamDesc& p = control->desc();

	// WHAT THE DAW SHOWS, READ BACK: every printed position comes back to a position that prints the same.
	for (int k = 0; k <= 16; ++k) {
		const double normal = static_cast<double>(k) / 16.0;
		char shown[64] = {};
		REQUIRE(host.valueToText(id, normal, shown, sizeof shown));
		double back = -1.0;
		INFO("position " << normal << " shows \"" << shown << "\"");
		REQUIRE(host.textToValue(id, shown, &back));
		char again[64] = {};
		REQUIRE(host.valueToText(id, back, again, sizeof again));
		CHECK(std::string(again) == shown);
	}
	// A TYPED FREQUENCY, in the unit the cutoff shows: hertz, with a k for thousands.
	double typed = -1.0;
	REQUIRE(host.textToValue(id, "2k", &typed));
	CHECK(std::abs(typed - feathers::device::toNormal(p, std::log2(2000.0))) < 1e-9);
	REQUIRE(host.textToValue(id, "440", &typed));
	CHECK(std::abs(typed - feathers::device::toNormal(p, std::log2(440.0))) < 1e-9);
	// NOT A NUMBER, AND NOT A SLOT, ARE REFUSED.
	CHECK_FALSE(host.textToValue(id, "loud", &typed));
	CHECK_FALSE(host.textToValue(static_cast<clap_id>(0), "440", &typed));
}
TEST_CASE("Unpin frees it") {
	// **"FREES" IS T1's UNPIN: Pinned → Recent.** The slot keeps its mapping (a host lane on it still
	// drives the control) and becomes recyclable — the last resort when the pool is full. Asserted both
	// ways: the state, and that a full pool's next pin takes exactly this slot.
	FilterFixture f;
	emptyPool(f.bridge);
	ui::Knob* knob = filterKnob(*f.card, "filter_cutoff_wide");
	REQUIRE(knob != nullptr);
	auto* control = dynamic_cast<ui::DeviceControl*>(knob->control());
	REQUIRE(control != nullptr);
	REQUIRE(ui::chooseHostMenuEntry(*control, ui::kControlMenuExpose, nullptr));
	const hs::SlotIndex slot = control->hostSlot().slot;
	REQUIRE(slot == 0u);
	(void)f.bridge.takeStateDirty();

	std::optional<visage::PopupMenu> menu = knob->contextMenu();
	REQUIRE(menu.has_value());
	std::vector<SeenRow> rows = rowsOf(*menu);
	REQUIRE(rows.size() == 2);
	REQUIRE(rows[1].label == "Unpin");
	menu->onSelection().callback(rows[1].id);

	CHECK(f.bridge.slotPool().state(slot) == hs::SlotState::Recent);
	CHECK(control->hostSlot().state == feathers::device::HostSlotStatus::State::Recent);
	CHECK(control->hostSlot().slot == slot); // still mapped: the host's lane still drives it
	CHECK(f.bridge.takeStateDirty());
	CHECK_FALSE(knob->hostMarked()); // the mark is for a slot that STAYS; this one may go
	rows = rowsOf(*knob->contextMenu());
	REQUIRE(rows.size() == 2);
	CHECK(rows[0].label == "Host: Slot 1 " + kSlotDot + " Recent");
	CHECK(rows[1].label == "Pin");

	// RECYCLABLE: pin every other slot, then one more — the pool gives up exactly this one, and says so.
	hs::SlotPool& pool = f.bridge.slotPool();
	for (uint32_t n = 1; n < pool.capacity(); ++n) {
		REQUIRE(pool.pin(synthetic(n)).slot == n);
	}
	const hs::Allocation took = pool.pin(synthetic(9999));
	CHECK(took.slot == slot);
	CHECK(took.took == hs::Allocation::Took::Recent);
	CHECK(took.announce);
	CHECK(control->hostSlot().state == feathers::device::HostSlotStatus::State::Unslotted);
}
TEST_CASE("a control whose host has no pool shows no entry") {
	// TWO HOSTS WITH NO POOL. (1) A header macro is ALREADY a host parameter (a core CLAP param), so its
	// control answers `Unavailable` and its menu is the macro's own rows alone. (2) A device in a rack
	// bridge built WITHOUT a pool (`HostSlots::None`, the default) — a nested rack's, until T11; the
	// standalone rack's top bridge owns one since T10 (its own case: "the standalone rack's knobs offer
	// Expose to host too"). A pool-less bridge keeps
	// `DeviceHost`'s defaults, so the control answers `Unavailable` and a right-click with nothing routed
	// opens nothing at all, exactly as before T7.
	FilterFixture f;
	ui::Knob* macro = f.panel->macroKnobForTest(0);
	REQUIRE(macro != nullptr);
	REQUIRE(macro->control() != nullptr);
	CHECK(macro->control()->hostSlot().state == feathers::device::HostSlotStatus::State::Unavailable);
	CHECK(ui::hostMenuRows(*macro->control()).empty());
	std::optional<visage::PopupMenu> menu = macro->contextMenu();
	REQUIRE(menu.has_value()); // the macro menu still opens
	for (const SeenRow& row : rowsOf(*menu)) {
		CAPTURE(row.label);
		CHECK(row.label.find("host") == std::string::npos);
		CHECK(row.label.find("Host") == std::string::npos);
		CHECK((row.id < ui::kControlMenuExpose || row.id > ui::kControlMenuLast));
	}

	feathers::rack::RackEngine engine { feathers::rack::kRackAxes, feathers::hostCatalogue() };
	engine.prepare(48000.0, 4096);
	feathers::rack::RackBridge rack { engine };
	const feathers::DeviceInstanceId id = rack.addDevice(feathers::fx::filter::kTraits.tag);
	rack.drain();
	REQUIRE(id != feathers::kNoInstance);
	const feathers::device::Descriptor* desc = rack.descriptorOf(id);
	REQUIRE(desc != nullptr);
	ui::DeviceControl cutoff { rack, id, *desc, static_cast<size_t>(feathers::fx::filter::Param::Cutoff) };
	REQUIRE(std::string(cutoff.desc().key) == "filter_cutoff_wide");
	CHECK(cutoff.hostSlot().state == feathers::device::HostSlotStatus::State::Unavailable);
	CHECK(ui::hostMenuRows(cutoff).empty());
	CHECK_FALSE(ui::chooseHostMenuEntry(cutoff, ui::kControlMenuExpose, nullptr));
	ui::Knob knob { cutoff, "Cutoff" };
	CHECK_FALSE(knob.contextMenu().has_value());
	CHECK_FALSE(knob.hostMarked());
}
TEST_CASE("an automated slot's menu names it Automated and offers no verb") {
	FilterFixture f;
	emptyPool(f.bridge);
	BridgeLookup lookup { f.bridge };
	hs::SlotHost host { f.bridge.slotPool(), lookup };
	ui::Knob* cutoffKnob = filterKnob(*f.card, "filter_cutoff_wide");
	ui::Knob* resKnob = filterKnob(*f.card, "filter_res");
	REQUIRE(cutoffKnob != nullptr);
	REQUIRE(resKnob != nullptr);
	auto* cutoff = dynamic_cast<ui::DeviceControl*>(cutoffKnob->control());
	auto* res = dynamic_cast<ui::DeviceControl*>(resKnob->control());
	REQUIRE(cutoff != nullptr);
	REQUIRE(res != nullptr);

	// CUTOFF PINNED, RESONANCE RECENT (a touch, the pool's own road).
	REQUIRE(ui::chooseHostMenuEntry(*cutoff, ui::kControlMenuExpose, nullptr));
	const std::optional<hs::Target> resTarget = f.bridge.slotTargetOf(res->device(), res->control());
	REQUIRE(resTarget.has_value());
	const hs::Allocation touched = f.bridge.slotPool().touch(*resTarget);
	REQUIRE(touched.slot != hs::kNoSlot);
	(void)visit(f.bridge, host); // bound, so the host's writes below reach the controls
	const hs::SlotIndex cutoffSlot = cutoff->hostSlot().slot;
	const hs::SlotIndex resSlot = touched.slot;
	REQUIRE(f.bridge.slotPool().state(cutoffSlot) == hs::SlotState::Pinned);
	REQUIRE(f.bridge.slotPool().state(resSlot) == hs::SlotState::Recent);

	// THE HOST WRITES BOTH — T6b's road: a value on the bus, anchored at the next visit.
	f.bridge.slotBus().onHostValue(cutoffSlot, static_cast<float>(std::log2(800.0)));
	f.bridge.slotBus().onHostValue(resSlot, 0.5f);
	(void)visit(f.bridge, host);
	REQUIRE(f.bridge.slotPool().state(cutoffSlot) == hs::SlotState::Anchored);
	REQUIRE(f.bridge.slotPool().state(resSlot) == hs::SlotState::Anchored);
	CHECK(cutoffKnob->hostMarked()); // Anchored shares the mark
	CHECK(resKnob->hostMarked());
	(void)f.bridge.takeStateDirty();

	// THE MENU SAYS "Automated" AND OFFERS NOTHING: the status line alone, not a verb.
	for (ui::Knob* knob : { cutoffKnob, resKnob }) {
		std::optional<visage::PopupMenu> menu = knob->contextMenu();
		REQUIRE(menu.has_value());
		const std::vector<SeenRow> rows = rowsOf(*menu);
		REQUIRE(rows.size() == 1);
		const hs::SlotIndex slot = knob == cutoffKnob ? cutoffSlot : resSlot;
		CHECK(rows[0].label == "Host: Slot " + std::to_string(slot + 1) + " " + kSlotDot + " Automated");
		CHECK_FALSE(rows[0].enabled);
		// A pick of the line (were a host to send one) changes nothing.
		menu->onSelection().callback(rows[0].id);
	}
	CHECK(f.bridge.slotPool().state(cutoffSlot) == hs::SlotState::Anchored);
	CHECK(f.bridge.slotPool().state(resSlot) == hs::SlotState::Anchored);
	CHECK_FALSE(f.bridge.takeStateDirty());
	// THE POOL'S OWN `release` REMAINS AS API and still returns each to what it was — no UI offers it.
	REQUIRE(f.bridge.slotPool().release(cutoffSlot));
	REQUIRE(f.bridge.slotPool().release(resSlot));
	CHECK(f.bridge.slotPool().state(cutoffSlot) == hs::SlotState::Pinned);
	CHECK(f.bridge.slotPool().state(resSlot) == hs::SlotState::Recent);
	CHECK(cutoffKnob->hostMarked());
	CHECK_FALSE(resKnob->hostMarked());
}

namespace {
// ---------------------------------------------------------------------------------------------------
// HOST SLOTS T9 — THE RECENT RING AND THE TRAY. A hand on an unslotted control takes a slot; recycling a used
// Recent, or taking an id the host has touched, is SAID. Every case reads the pool, the outbox and the tray
// line the bridge hands the editor's tick (`takeSlotNotice`) — values, not a render.
// ---------------------------------------------------------------------------------------------------

struct SentEvent {
	hs::SlotEventKind kind;
	uint16_t slot;
	float value;
};
/// Everything the outbox holds, in order, as the plugin's drain would hand it to the host.
std::vector<SentEvent> drainSent(ParamBridge& bridge) {
	std::vector<SentEvent> out;
	bridge.slotOutbox().drain([&](hs::SlotEventKind kind, uint16_t slot, float value) {
		out.push_back({ kind, slot, value });
		return true;
	});
	return out;
}
/// The cutoff knob's control, and its declaration.
ui::DeviceControl& cutoffControl(FilterFixture& f) {
	ui::Knob* knob = filterKnob(*f.card, "filter_cutoff_wide");
	REQUIRE(knob != nullptr);
	auto* control = dynamic_cast<ui::DeviceControl*>(knob->control());
	REQUIRE(control != nullptr);
	return *control;
}
} // namespace

TEST_CASE("touching one more control than the pool holds recycles the LRU Recent and the tray says so") {
	// (The brief's "touching 65 controls", from when the pool was 64: named from the capacity now.)
	FilterFixture f;
	emptyPool(f.bridge);
	hs::SlotPool& pool = f.bridge.slotPool();
	for (uint32_t n = 0; n < pool.capacity(); ++n) {
		REQUIRE(pool.touch(synthetic(n)).slot == n); // every slot a Recent, "Fill 0" the least recently used
	}
	(void)f.bridge.takeStateDirty();
	ui::DeviceControl& cutoff = cutoffControl(f);

	cutoff.beginGesture();
	// THE POOL MOVED AT THE BEGIN: slot 0 recycled.
	CHECK(cutoff.hostSlot().slot == 0u);
	CHECK(pool.state(0) == hs::SlotState::Recent);
	CHECK(pool.target(0).key == "filter_cutoff_wide");
	CHECK(f.bridge.takeStateDirty()); // a new mapping is saved with the project
	// HELD WHILE THE HAND IS DOWN: the knob's own readout would write over it a frame later.
	CHECK(f.bridge.takeSlotNotice().empty());
	cutoff.setValue(std::log2(700.0));
	cutoff.endGesture();
	CHECK(f.bridge.takeSlotNotice() == "Host slot 1: Fill 0 -> Filter " + kSlotDot + " Cutoff");
	CHECK(f.bridge.takeSlotNotice().empty()); // said once

	// AND THE TRAY SAYS EXACTLY THAT LINE when the editor's tick hands it on.
	ui::NotifyTray tray([] { return int64_t { 0 }; });
	pool.touch(synthetic(9000)); // recycles the next LRU, "Fill 1" on slot 1 — through the pool, not a hand
	CHECK(f.bridge.takeSlotNotice().empty()); // only a HAND's touch is announced by this road
	ui::Knob* res = filterKnob(*f.card, "filter_res");
	REQUIRE(res != nullptr);
	res->control()->beginGesture();
	res->control()->endGesture();
	const std::string said = f.bridge.takeSlotNotice();
	tray.say(said);
	CHECK(tray.text() == "Host slot 3: Fill 2 -> Filter " + kSlotDot + " Res");
}
TEST_CASE("an untouched Default takeover says nothing and emits Value without a gesture") {
	FilterFixture f;
	emptyPool(f.bridge);
	BridgeLookup lookup { f.bridge };
	hs::SlotHost host { f.bridge.slotPool(), lookup };
	(void)visit(f.bridge, host);
	hs::SlotPool& pool = f.bridge.slotPool();
	// EVERY SLOT AN UNTOUCHED DEFAULT (seeded through the pool's own fill), ranked so the last is the lowest.
	std::vector<hs::Candidate> cands;
	for (uint32_t n = 0; n < pool.capacity(); ++n) {
		cands.push_back({ synthetic(n), 0, static_cast<int32_t>(n), 0 });
	}
	REQUIRE(pool.fillDefaults(cands) == pool.capacity());
	const hs::SlotIndex lowest = pool.capacity() - 1;
	(void)drainSent(f.bridge);
	ui::DeviceControl& cutoff = cutoffControl(f);
	const feathers::device::ParamDesc& desc = cutoff.desc();

	cutoff.beginGesture();
	CHECK(cutoff.hostSlot().slot == lowest); // the lowest-priority Default, taken silently
	const double v1 = std::log2(700.0);
	cutoff.setValue(v1);
	CHECK(drainSent(f.bridge).empty()); // not bound yet: nothing sent
	(void)visit(f.bridge, host);
	const auto normal = [&](double v) {
		return static_cast<float>(feathers::device::toNormal(
		    desc, static_cast<double>(feathers::device::coerceControl(desc, static_cast<float>(v)))));
	};
	// THE TAKEOVER, THEN THE HAND'S GESTURE: a Value outside any gesture, then Begin and the value inside it.
	std::vector<SentEvent> ev = drainSent(f.bridge);
	REQUIRE(ev.size() == 3);
	CHECK((ev[0].kind == hs::SlotEventKind::Value && ev[0].slot == lowest && ev[0].value == normal(v1)));
	CHECK((ev[1].kind == hs::SlotEventKind::Begin && ev[1].slot == lowest));
	CHECK((ev[2].kind == hs::SlotEventKind::Value && ev[2].slot == lowest && ev[2].value == normal(v1)));
	// THE DRAG GOES ON INSIDE ITS GESTURE, and closes at the release.
	const double v2 = std::log2(1200.0);
	cutoff.setValue(v2);
	cutoff.endGesture();
	ev = drainSent(f.bridge);
	REQUIRE(ev.size() == 2);
	CHECK((ev[0].kind == hs::SlotEventKind::Value && ev[0].value == normal(v2)));
	CHECK((ev[1].kind == hs::SlotEventKind::End && ev[1].slot == lowest));
	// SILENT: a Default nobody automated is taken without a word.
	CHECK(f.bridge.takeSlotNotice().empty());
}
TEST_CASE("pinned and anchored slots are never recycled by touches") {
	// A POOL FULL OF PINS AND ANCHORS: a hand on an unslotted control allocates nothing and SAYS NOTHING.
	// Chosen over "no free slot" in the tray: every drag would say it again, and the right-click already
	// shows "Expose to host (no free slot)" switched off for anyone who asks.
	FilterFixture f;
	emptyPool(f.bridge);
	hs::SlotPool& pool = f.bridge.slotPool();
	for (uint32_t n = 0; n < pool.capacity(); ++n) {
		REQUIRE(pool.pin(synthetic(n)).slot == n);
		if (n % 2 == 0) {
			REQUIRE(pool.anchor(n));
		}
	}
	const hs::PoolSnapshot before = pool.snapshot();
	const uint64_t gen = pool.generation();
	(void)drainSent(f.bridge);
	(void)f.bridge.takeStateDirty();
	ui::DeviceControl& cutoff = cutoffControl(f);
	cutoff.beginGesture();
	cutoff.setValue(std::log2(700.0));
	cutoff.endGesture();
	CHECK(pool.generation() == gen);
	CHECK(pool.snapshot() == before);
	CHECK(cutoff.hostSlot().state == feathers::device::HostSlotStatus::State::Unslotted);
	CHECK(f.bridge.takeSlotNotice().empty());
	CHECK(drainSent(f.bridge).empty());
}
TEST_CASE("taking a host-marked slot announces the host's leftover lanes") {
	// THE HOST'S LANES OUTLIVE THE SLOT: the host keeps its lanes and modulators on an id after the slot is released; the one
	// control that takes such an id is told, in the tray, that they now drive it.
	FilterFixture f;
	emptyPool(f.bridge);
	hs::SlotPool& pool = f.bridge.slotPool();
	REQUIRE(pool.markHost(5)); // Empty, and the host has a lane on it
	// EVERY CLEAN SLOT PINNED — and the pins themselves pass over slot 5, the marked one, which is taken last.
	for (uint32_t n = 0; n + 1 < pool.capacity(); ++n) {
		const hs::Allocation a = pool.pin(synthetic(n));
		REQUIRE(a.slot != 5u);
		REQUIRE_FALSE(a.hostMarked);
	}
	REQUIRE(pool.state(5) == hs::SlotState::Empty);
	ui::DeviceControl& cutoff = cutoffControl(f);
	cutoff.beginGesture();
	cutoff.endGesture();
	CHECK(cutoff.hostSlot().slot == 5u);
	CHECK(f.bridge.takeSlotNotice()
	      == "Slot 6 had host automation or modulation; it now drives Filter " + kSlotDot + " Cutoff");
}
TEST_CASE("an undo or a row writer never allocates a slot") {
	// ONLY A HAND TOUCHES: `DeviceValueEdit` (undo/redo) and `ControlGestureScope` (a card's row writer)
	// bracket the host, and neither is a hand, so neither gives a control a slot.
	FilterFixture f;
	emptyPool(f.bridge);
	ui::DeviceControl& cutoff = cutoffControl(f);
	const uint64_t gen = f.bridge.slotPool().generation();
	ui::DeviceValueEdit edit { f.deviceHost, cutoff.device(), cutoff.control(), std::log2(500.0), std::log2(900.0) };
	edit.undo();
	edit.redo();
	{
		feathers::backplane::ControlGestureScope scope { f.deviceHost, cutoff.device() };
		scope.setField(cutoff.control(), static_cast<float>(std::log2(2000.0)));
	}
	CHECK(f.bridge.slotPool().generation() == gen);
	CHECK(cutoff.hostSlot().state == feathers::device::HostSlotStatus::State::Unslotted);
	// NOT VACUOUS: the value moved, so the roads ran.
	CHECK(cutoff.value() == static_cast<double>(static_cast<float>(std::log2(2000.0))));
	// AND A HAND DOES.
	cutoff.beginGesture();
	cutoff.endGesture();
	CHECK(cutoff.hostSlot().state == feathers::device::HostSlotStatus::State::Recent);
}
TEST_CASE("with no free slot the entry is disabled") {
	FilterFixture f;
	emptyPool(f.bridge);
	hs::SlotPool& pool = f.bridge.slotPool();
	// SIXTY-FOUR ANCHORS: nothing Empty, Default or Recent is left to give.
	for (uint32_t n = 0; n < pool.capacity(); ++n) {
		REQUIRE(pool.pin(synthetic(n)).slot == n);
		REQUIRE(pool.anchor(n));
	}
	ui::Knob* knob = filterKnob(*f.card, "filter_cutoff_wide");
	REQUIRE(knob != nullptr);
	CHECK(knob->control()->hostSlot().state == feathers::device::HostSlotStatus::State::Unslotted);
	CHECK_FALSE(knob->control()->hostSlot().canExpose);
	std::optional<visage::PopupMenu> menu = knob->contextMenu();
	REQUIRE(menu.has_value()); // SHOWN, switched off — not hidden
	const std::vector<SeenRow> rows = rowsOf(*menu);
	REQUIRE(rows.size() == 1);
	CHECK(rows[0].label == "Expose to host (no free slot)");
	CHECK_FALSE(rows[0].enabled);
	// AND THE VERB BEHIND IT REFUSES, should anything reach it: the pool is untouched.
	const uint64_t before = pool.generation();
	menu->onSelection().callback(rows[0].id);
	CHECK(pool.generation() == before);
	CHECK(knob->control()->hostSlot().state == feathers::device::HostSlotStatus::State::Unslotted);

	// ONE RELEASE AND THE ENTRY COMES BACK: a Recent is the last resort, and the pin says what it took.
	REQUIRE(pool.release(7));
	REQUIRE(pool.unpin(7));
	CHECK(knob->control()->hostSlot().canExpose);
	std::string displaced;
	REQUIRE(ui::chooseHostMenuEntry(*knob->control(), ui::kControlMenuExpose, &displaced));
	CHECK(knob->control()->hostSlot().slot == 7u);
	CHECK(displaced == "Fill 7");
}
TEST_CASE("an Expose to host popup left open across the card's destruction writes nothing") {
	// (The brief's name, "a popup left open across the card's destruction writes nothing", is already the
	// MATH card's case above; a doctest filter would run both, so this one says whose popup it is.)
	//
	// A CARD BUILT BY THE CASE, over the fixture's own host, so it can be destroyed while its device stays —
	// the state a menu outlives when a lane rebuilds its cards. The pick that arrives afterwards must find
	// the knob's token gone and write NOTHING: a guard that let it through would reach the card's freed
	// `DeviceControl` (ASan: heap-use-after-free) and, with it still readable, pin the control.
	FilterFixture f;
	emptyPool(f.bridge);
	const feathers::DeviceInstanceId id = f.bridge.fxDeviceAt(0);
	const feathers::device::Descriptor* desc = f.deviceHost.descriptorOf(id);
	REQUIRE(desc != nullptr);
	ui::WidgetRegistry registry;
	std::unique_ptr<ui::FxCard> made = ui::makeCustomCard(f.deviceHost, registry, id, *desc, nullptr);
	auto* card = dynamic_cast<ui::FilterCard*>(made.get());
	REQUIRE(card != nullptr);
	ui::Knob* knob = filterKnob(*card, "filter_cutoff_wide");
	REQUIRE(knob != nullptr);
	std::optional<visage::PopupMenu> menu = knob->contextMenu();
	REQUIRE(menu.has_value());
	const std::vector<SeenRow> rows = rowsOf(*menu);
	REQUIRE(rows.size() == 1);
	REQUIRE(rows[0].label == "Expose to host");

	// THE CONTROL: the proof the pick WOULD pin it, taken from a menu over a card that stays alive.
	const std::optional<hs::Target> target = f.bridge.slotTargetOf(id, static_cast<size_t>(feathers::fx::filter::Param::Cutoff));
	REQUIRE(target.has_value());

	made.reset(); // the card, its knobs and its controls go; the device and the pool stay
	const uint64_t before = f.bridge.slotPool().generation();
	menu->onSelection().callback(rows[0].id);
	CHECK(f.bridge.slotPool().generation() == before);
	CHECK(f.bridge.slotPool().slotOf(*target) == hs::kNoSlot);

	// THE CONTROL CASE: the same pick from the live card's menu does pin it — so the silence above is the
	// guard's, not a pick that could never have written.
	ui::Knob* live = filterKnob(*f.card, "filter_cutoff_wide");
	REQUIRE(live != nullptr);
	std::optional<visage::PopupMenu> liveMenu = live->contextMenu();
	REQUIRE(liveMenu.has_value());
	liveMenu->onSelection().callback(rowsOf(*liveMenu)[0].id);
	CHECK(f.bridge.slotPool().slotOf(*target) != hs::kNoSlot);
}

namespace {
//------------------------------------------------------------------------------------------------//
// T7 REVIEW, AT THE INTEGRATION: the menu a hand reaches on a ROUTED knob (the route cases elsewhere //
// drive `showRouteMenu`, which the knob's right-click no longer opens), the route pick across the   //
// card's destruction, a newly anchored slot marking the patch, and the standalone rack's Expose.    //
//------------------------------------------------------------------------------------------------//

/// Routes `source` into `knob`'s control at `amount`, the way a hand does: armed, one amount gesture.
void routeInto(ui::ModState& mod, ui::Knob& knob, backplane::Source source, float amount) {
	mod.focus(source);
	mod.arm(true);
	knob.control()->beginAmountGesture();
	knob.control()->setAmount(amount);
	knob.control()->endAmountGesture();
	mod.arm(false);
}
} // namespace

// ---------------------------------------------------------------------------------------------------
// THE EQ'S CARD AND PANEL
// ---------------------------------------------------------------------------------------------------

TEST_CASE("EQ's Q hover text follows its band's Type, on the card and in the window, for an unselected band too") {
	namespace eq = feathers::fx::eq;
	DynCardFixture<ui::EqCard> d(eq::kTraits.tag);
	ui::WidgetRegistry registry;
	ui::EqExpanded panel { d.f.deviceHost, registry, d.device };
	const auto hover = [](ui::Knob* k) { return ui::hoverText(*k, visage::Point { k->width() * 0.5f, k->height() * 0.5f }); };
	// THE EXPECTED TEXT IS RESTATED HERE, LITERALLY. The hover road may put the control's name in front, so the tail is read.
	const auto tailIs = [](const std::string& text, const char* want) {
		const std::string tail(want);
		return text.size() >= tail.size() && text.compare(text.size() - tail.size(), tail.size(), tail) == 0;
	};
	// Band 0 is the selected one: its Q is on the face once the band is on.
	d.set(eq::bandIndex(0, eq::BandField::On), 1.0f);
	const auto selectedAs = [&](float type, const char* want) {
		d.set(eq::bandIndex(0, eq::BandField::Type), type);
		d.card->updateContent();
		panel.updateContent();
		ui::Knob* onCard = d.card->controlsForTest()->knobForTest(eq::bandIndex(0, eq::BandField::Q));
		ui::Knob* inWindow = panel.controlsForTest()->knobForTest(eq::bandIndex(0, eq::BandField::Q));
		REQUIRE(onCard != nullptr);
		REQUIRE(inWindow != nullptr);
		CAPTURE(type);
		CHECK(tailIs(hover(onCard), want));
		CHECK(tailIs(hover(inWindow), want));
	};
	selectedAs(0.0f, "How narrow the bell is: a higher Q is a narrower band");
	selectedAs(1.0f, "How steep the shelf's transition is: a higher Q is a sharper knee, up to 1.5");
	selectedAs(3.0f, "On a cut it is resonance: 0.71 is flat, higher peaks at the corner");
	selectedAs(5.0f, "How narrow the notch is: a higher Q is a narrower slice");

	// BAND 3 IS NOT SELECTED: its Type moves, and its Q's text moves with it on the next poll (the card's and the window's).
	d.set(eq::bandIndex(3, eq::BandField::Type), 5.0f);
	d.card->updateContent();
	panel.updateContent();
	for (ui::EqControls* c : { d.card->controlsForTest(), panel.controlsForTest() }) {
		ui::Knob* q = c->knobForTest(eq::bandIndex(3, eq::BandField::Q));
		REQUIRE(q != nullptr);
		REQUIRE(q->control() != nullptr);
		const char* own = q->control()->tooltipOverride();
		REQUIRE(own != nullptr);
		CHECK(std::string(own) == "How narrow the notch is: a higher Q is a narrower slice");
	}
}
TEST_CASE("EQ's card offers only what the selected band uses, and says so when it is off") {
	namespace eq = feathers::fx::eq;
	DynCardFixture<ui::EqCard> d(eq::kTraits.tag);
	ui::EqCard* card = d.card;
	ui::EqControls* c = card->controlsForTest();
	const auto visible = [&](int band, eq::BandField f) {
		const size_t p = eq::bandIndex(band, f);
		if (ui::Knob* k = c->knobForTest(p)) {
			return k->isVisible();
		}
		if (ui::ValueBox* b = c->boxForTest(p)) {
			return b->isVisible();
		}
		ui::SwitchChip* s = c->chipForTest(p);
		REQUIRE(s != nullptr);
		return s->isVisible();
	};

	// BAND 1 IS SELECTED AND IS OFF BY DEFAULT: its On switch, and nothing that would change nothing.
	REQUIRE(card->graphForTest()->selected() == 0);
	CHECK(visible(0, eq::BandField::On));
	for (const auto f : { eq::BandField::Type, eq::BandField::Freq, eq::BandField::Gain, eq::BandField::Q,
	                      eq::BandField::Slope }) {
		CAPTURE(static_cast<int>(f));
		CHECK_FALSE(visible(0, f));
	}
	CHECK(c->offCaption().width() > 0.0f);

	// ON, AS THE LOW CUT IT IS: type, slope, frequency and Q — no gain, because a cut has none.
	d.set(eq::bandIndex(0, eq::BandField::On), 1.0f);
	CHECK(card->updateContent());
	CHECK(c->offCaption().width() == 0.0f);
	CHECK(visible(0, eq::BandField::Type));
	CHECK(visible(0, eq::BandField::Slope));
	CHECK(visible(0, eq::BandField::Freq));
	CHECK(visible(0, eq::BandField::Q));
	CHECK_FALSE(visible(0, eq::BandField::Gain));
	CHECK(c->knobForTest(eq::bandIndex(0, eq::BandField::Q))->active());

	// A 6 dB CUT IS ONE FIRST-ORDER SECTION: its Q is greyed.
	d.set(eq::bandIndex(0, eq::BandField::Slope), 0.0f);
	CHECK(card->updateContent());
	CHECK_FALSE(c->knobForTest(eq::bandIndex(0, eq::BandField::Q))->active());

	// A BELL: gain, no slope.
	d.set(eq::bandIndex(0, eq::BandField::Type), static_cast<float>(eq::BandType::Bell));
	CHECK(card->updateContent());
	CHECK(visible(0, eq::BandField::Gain));
	CHECK_FALSE(visible(0, eq::BandField::Slope));

	// NO OTHER BAND'S WIDGETS ARE SHOWN.
	for (int b = 1; b < eq::kBandCount; ++b) {
		CAPTURE(b);
		CHECK_FALSE(visible(b, eq::BandField::On));
		CHECK_FALSE(visible(b, eq::BandField::Freq));
	}
}
TEST_CASE("EQ's globals are greyed while nothing they act on is active") {
	namespace eq = feathers::fx::eq;
	DynCardFixture<ui::EqCard> d(eq::kTraits.tag);
	ui::EqControls* c = d.card->controlsForTest();
	ui::Knob* offset = c->knobForTest(eq::paramIndex(eq::Param::Offset));
	ui::Knob* amount = c->knobForTest(eq::paramIndex(eq::Param::Amount));
	REQUIRE(offset != nullptr);
	REQUIRE(amount != nullptr);
	// THE DEFAULT EQ IS IDENTITY: every gain at 0 dB and both cuts off, so neither changes the sound.
	CHECK_FALSE(offset->active());
	CHECK_FALSE(amount->active());
	// A GAIN MOVES: both now act.
	d.set(eq::bandIndex(3, eq::BandField::Gain), 6.0f);
	CHECK(d.card->updateContent());
	CHECK(offset->active());
	CHECK(amount->active());
	// A CUT ALONE: Offset moves it, Amount has nothing to scale.
	d.set(eq::bandIndex(3, eq::BandField::Gain), 0.0f);
	d.set(eq::bandIndex(0, eq::BandField::On), 1.0f);
	CHECK(d.card->updateContent());
	CHECK(offset->active());
	CHECK_FALSE(amount->active());
	// A TILT ALONE: Amount scales it.
	d.set(eq::bandIndex(0, eq::BandField::On), 0.0f);
	d.set(eq::paramIndex(eq::Param::Tilt), 2.0f);
	CHECK(d.card->updateContent());
	CHECK(amount->active());
}
TEST_CASE("EQ's graph asks the device for its spectrum while it is shown, and not while it is hidden or both taps are off") {
	namespace eq = feathers::fx::eq;
	DynCardFixture<ui::EqCard> d(eq::kTraits.tag);
	ui::EqGraph* g = d.card->graphForTest();
	const size_t slot = d.f.bridge.fxSlotOf(d.device);
	const auto spend = [&] {
		while (d.f.bridge.deviceReadingsWanted(slot, 24000)) {
		}
		REQUIRE(d.f.bridge.deviceReadingsLeaseForTest(slot) == 0);
	};
	spend();
	// SHOWN, TAPS ON (the default): the lease is the whole half second the bridge grants.
	g->setVisible(true);
	(void)g->update();
	CHECK(d.f.bridge.deviceReadingsLeaseForTest(slot) == 24000);
	// HIDDEN: no renewal.
	spend();
	g->setVisible(false);
	(void)g->update();
	CHECK(d.f.bridge.deviceReadingsLeaseForTest(slot) == 0);
	// BOTH TAPS OFF: nothing to draw, nothing asked for.
	g->setVisible(true);
	d.f.deviceHost.setField(d.device, eq::extraIndex(eq::Extra::Pre), 1.0f);
	d.f.deviceHost.setField(d.device, eq::extraIndex(eq::Extra::Post), 1.0f);
	(void)g->update();
	CHECK(d.f.bridge.deviceReadingsLeaseForTest(slot) == 0);
}
TEST_CASE("EQ's selection lives in the extra state and moves the strip") {
	namespace eq = feathers::fx::eq;
	DynCardFixture<ui::EqCard> d(eq::kTraits.tag);
	ui::EqGraph* g = d.card->graphForTest();
	ui::EqControls* c = d.card->controlsForTest();
	g->select(4);
	CHECK(g->selected() == 4);
	CHECK(d.f.deviceHost.fieldAt(d.device, eq::extraIndex(eq::Extra::Selected)) == 4.0f);
	CHECK(c->knobForTest(eq::bandIndex(4, eq::BandField::Freq))->isVisible());
	CHECK_FALSE(c->knobForTest(eq::bandIndex(3, eq::BandField::Freq))->isVisible());
}
TEST_CASE("EQ's handles sit at each band's stored frequency and gain, whatever the stored Offset, Amount and Tilt say") {
	// SHAPE B OF BACKLOG MP (decided 2026-10-17): the handles are the BANDS' stored values; the globals
	// act on the heard curve only. (This case used to assert R13 — the handle on the heard curve, at +3 dB under Amount
	// 50% and 1.6 kHz under Offset +1 — which is what moved the handles from under the hand.)
	namespace eq = feathers::fx::eq;
	DynCardFixture<ui::EqCard> d(eq::kTraits.tag);
	ui::EqGraph* g = d.card->graphForTest();
	// Band 4 (800 Hz by default) lifted to +6 dB.
	d.set(eq::bandIndex(3, eq::BandField::Gain), 6.0f);
	const auto geo = g->geometry();
	const visage::Point stored { geo.xForHz(800.0), geo.yForDb(6.0) };
	const auto handle = [&] { return g->handleAt(g->baseView(), 3); };
	CHECK(handle().x == doctest::Approx(stored.x).epsilon(1e-4));
	CHECK(handle().y == doctest::Approx(stored.y).epsilon(1e-4));
	// AMOUNT 50%, then OFFSET +1 OCTAVE and a TILT, all STORED: the handle does not move; the heard band does.
	d.set(eq::paramIndex(eq::Param::Amount), 0.5f);
	d.set(eq::paramIndex(eq::Param::Offset), 1.0f);
	d.set(eq::paramIndex(eq::Param::Tilt), 2.0f);
	CHECK(handle().x == doctest::Approx(stored.x).epsilon(1e-4));
	CHECK(handle().y == doctest::Approx(stored.y).epsilon(1e-4));
	CHECK(g->view().bands[3].freqHz == doctest::Approx(1600.0).epsilon(1e-6));
	CHECK(g->view().bands[3].gainDb == doctest::Approx(3.0).epsilon(1e-6));
	// TWO CURVES AT REST: base against heard, because the globals are not at identity.
	CHECK_FALSE(ui::EqGraph::sameResponse(g->baseView(), g->view()));
	// AN OFF BAND SITS ON THE RAIL under the plot.
	CHECK(g->railBounds().contains(g->handleAt(g->baseView(), 0)));
}
TEST_CASE("EQ's handles stay still while the host automates the stored Offset, and a drag is one to one under Amount") {
	// THE CASE (backlog MP): a Bitwig lane on a host slot writes the STORED value (host-slots design §4), block after
	// block. The handle stays at the band's own frequency and gain; the hand grabs it there; a drag moves it with the
	// pointer, a pixel of travel a pixel of the band's stored dB, whatever Amount scales the heard curve by.
	namespace eq = feathers::fx::eq;
	DynCardFixture<ui::EqCard> d(eq::kTraits.tag);
	ui::EqGraph* g = d.card->graphForTest();
	d.set(eq::bandIndex(3, eq::BandField::Gain), 6.0f);
	d.set(eq::paramIndex(eq::Param::Amount), 0.5f);
	const auto geo = g->geometry();
	const visage::Point stored { geo.xForHz(800.0), geo.yForDb(6.0) };
	for (const float automated : { 1.0f, -2.0f, 3.5f }) {
		CAPTURE(automated);
		d.set(eq::paramIndex(eq::Param::Offset), automated); // the host's write: the stored value itself
		const visage::Point h = g->handleAt(g->baseView(), 3);
		CHECK(h.x == doctest::Approx(stored.x).epsilon(1e-4));
		CHECK(h.y == doctest::Approx(stored.y).epsilon(1e-4));
		CHECK(g->view().bands[3].freqHz == doctest::Approx(800.0 * std::exp2(static_cast<double>(automated))).epsilon(1e-6));
	}
	// GRABBED WHERE IT SITS, dragged up by the pixels of 4 dB: the stored gain is +10 dB, not +6 + 4 / 0.5.
	g->select(0);
	const float up = geo.yForDb(6.0) - geo.yForDb(10.0);
	g->onMouseDown().callback(at(stored.x, stored.y));
	CHECK(g->selected() == 3);
	g->onMouseDrag().callback(at(stored.x, stored.y - up));
	g->onMouseUp().callback(at(stored.x, stored.y - up));
	CHECK(d.f.deviceHost.fieldAt(d.device, eq::bandIndex(3, eq::BandField::Gain)) == doctest::Approx(10.0).epsilon(1e-4));
	CHECK(g->handleAt(g->baseView(), 3).y == doctest::Approx(geo.yForDb(10.0)).epsilon(1e-4));
	// AND AT AMOUNT 0 THE DRAG STILL WRITES (the old <5% rule is gone: there is no division left to guard).
	d.set(eq::paramIndex(eq::Param::Amount), 0.0f);
	g->onMouseDown().callback(at(stored.x, geo.yForDb(10.0)));
	g->onMouseDrag().callback(at(stored.x, geo.yForDb(12.0)));
	g->onMouseUp().callback(at(stored.x, geo.yForDb(12.0)));
	CHECK(d.f.deviceHost.fieldAt(d.device, eq::bandIndex(3, eq::BandField::Gain)) == doctest::Approx(12.0).epsilon(1e-4));
}

namespace {
/// THE ENGINE'S MODULATED ROW, PUBLISHED: the stored chain with `control` of the fixture's EQ moved to `value` — where
/// modulation left it this block (the road `modulatedFieldAt` reads).
template <typename Fixture>
void publishEqModulated(Fixture& d, size_t control, float value) {
	feathers::FxChain moved = d.f.bridge.fxChain();
	moved.device[d.slot].row.field[control] = value;
	d.f.bridge.publishModulated(feathers::defaultModRack(), moved, feathers::RackPlayheads {},
	                            feathers::mod::TransportClock::unknown());
}
} // namespace

TEST_CASE("EQ's handles stay on the base while Offset is modulated, and the heard curve moves without them") {
	// BACKLOG MP: the base curve stays still, and the effective curve, which does move, is shown
	// separately without handles.
	namespace eq = feathers::fx::eq;
	DynCardFixture<ui::EqCard> d(eq::kTraits.tag);
	ui::EqGraph* g = d.card->graphForTest();
	d.set(eq::bandIndex(3, eq::BandField::Gain), 6.0f); // band 4, 800 Hz, +6 dB
	const auto geo = g->geometry();
	const size_t offset = eq::paramIndex(eq::Param::Offset);

	// NOTHING MODULATED: the two views draw one response, so only one curve is drawn.
	publishEqModulated(d, offset, 0.0f);
	CHECK(ui::EqGraph::sameResponse(g->baseView(), g->view()));

	// OFFSET MODULATED TO +1 OCTAVE, then to -0.5, block after block: the handle is where the STORED values put it,
	// both times — 800 Hz at +6 dB.
	const visage::Point stored { geo.xForHz(800.0), geo.yForDb(6.0) };
	for (const float moved : { 1.0f, -0.5f }) {
		CAPTURE(moved);
		publishEqModulated(d, offset, moved);
		const visage::Point h = g->handleAt(g->baseView(), 3);
		CHECK(h.x == doctest::Approx(stored.x).epsilon(1e-4));
		CHECK(h.y == doctest::Approx(stored.y).epsilon(1e-4));
		// THE HEARD CURVE IS THE ENGINE'S RESPONSE AT ITS LIVE VALUES: the band acts at 800 × 2^moved, restated.
		const ui::EqGraph::View live = g->view();
		const double heardHz = 800.0 * std::exp2(static_cast<double>(moved));
		CHECK(live.bands[3].freqHz == doctest::Approx(heardHz).epsilon(1e-6));
		CHECK(ui::EqGraph::pathDb(live, eq::Channel::Stereo, heardHz) == doctest::Approx(6.0).epsilon(0.01));
		// NOT VACUOUS: the two views part, so the heard curve is drawn as well.
		CHECK_FALSE(ui::EqGraph::sameResponse(g->baseView(), live));
	}

	// AND THE HAND TAKES IT THERE: a press on the still handle grabs band 4, and a drag of one octave writes its
	// STORED frequency one octave up (the drag is relative to the stored value, as before).
	publishEqModulated(d, offset, 1.0f);
	g->select(0);
	g->onMouseDown().callback(at(stored.x, stored.y));
	CHECK(g->selected() == 3);
	g->onMouseDrag().callback(at(stored.x + geo.pixelsPerOctave(), stored.y));
	g->onMouseUp().callback(at(stored.x + geo.pixelsPerOctave(), stored.y));
	CHECK(d.f.deviceHost.fieldAt(d.device, eq::bandIndex(3, eq::BandField::Freq))
	      == doctest::Approx(std::log2(1600.0)).epsilon(1e-5));
	// A press where the HEARD band is (1.6 kHz with the Offset) grabs nothing: there is no handle on that curve.
	d.set(eq::bandIndex(3, eq::BandField::Freq), static_cast<float>(std::log2(800.0)));
	publishEqModulated(d, offset, 1.0f);
	g->select(0);
	g->onMouseDown().callback(at(geo.xForHz(1600.0), geo.yForDb(6.0)));
	g->onMouseUp().callback(at(geo.xForHz(1600.0), geo.yForDb(6.0)));
	CHECK(g->selected() == 0);
}
TEST_CASE("EQ's Key Track draws the heard curve at the tracked frequencies, and the handles stay on the base") {
	// BACKLOG MQ: the plot reads the device's own shift (`Reading::KeyOctaves`), gated by the stored switch.
	namespace eq = feathers::fx::eq;
	DynCardFixture<ui::EqCard> d(eq::kTraits.tag);
	ui::EqGraph* g = d.card->graphForTest();
	d.set(eq::bandIndex(3, eq::BandField::Gain), 6.0f);
	const auto geo = g->geometry();
	std::array<float, eq::kReadingCount> frame {};
	frame[static_cast<size_t>(eq::Reading::KeyOctaves)] = 1.0f; // the device: an octave above C3
	d.publish(frame);
	// OFF: the stored switch gates it — nothing moves, whatever was published.
	CHECK(g->keyOctaves() == 0.0);
	CHECK(g->view().bands[3].freqHz == doctest::Approx(800.0).epsilon(1e-6));
	// ON: every band an octave up in the heard curve; the handle still at 800 Hz.
	d.set(eq::paramIndex(eq::Param::KeyTrack), 1.0f);
	CHECK(g->keyOctaves() == 1.0);
	for (int b = 1; b < eq::kBandCount - 1; ++b) {
		CAPTURE(b);
		CHECK(g->view().bands[static_cast<size_t>(b)].freqHz
		      == doctest::Approx(2.0 * g->baseView().bands[static_cast<size_t>(b)].freqHz).epsilon(1e-6));
	}
	CHECK(g->handleAt(g->baseView(), 3).x == doctest::Approx(geo.xForHz(800.0)).epsilon(1e-4));
	CHECK_FALSE(ui::EqGraph::sameResponse(g->baseView(), g->view()));
}
TEST_CASE("EQ's Key Track is a two-state chip in the panel's globals row that writes the stored switch, and not on the card") {
	namespace eq = feathers::fx::eq;
	DynCardFixture<ui::EqCard> d(eq::kTraits.tag);
	CHECK(d.card->controlsForTest()->chipForTest(eq::paramIndex(eq::Param::KeyTrack)) == nullptr);
	std::vector<std::unique_ptr<ui::DeviceControl>> adapters;
	for (size_t i = 0; i < eq::kParamCount; ++i) {
		adapters.push_back(std::make_unique<ui::DeviceControl>(d.f.deviceHost, d.device, eq::kDescriptor, i));
	}
	ui::EqControls controls(*d.card, d.f.deviceHost, d.device,
	                        [&](size_t i) -> ui::DeviceControl& { return *adapters[i]; }, nullptr, 34.0f, true);
	controls.layOut({ 0.0f, 0.0f, 400.0f, 80.0f }, { 0.0f, 100.0f, 400.0f, 60.0f }, 3, true);
	ui::SwitchChip* chip = controls.chipForTest(eq::paramIndex(eq::Param::KeyTrack));
	REQUIRE(chip != nullptr);
	CHECK(chip->isVisible());
	// The LAST cell of the row, after Phase.
	ui::SwitchChip* phase = controls.chipForTest(eq::paramIndex(eq::Param::Phase));
	REQUIRE(phase != nullptr);
	CHECK(chip->bounds().x() >= phase->bounds().right() - 0.5f);
	CHECK(chip->bounds().right() <= 400.0f + 0.5f);
	CHECK(chip->control().value() == 0.0);
	chip->control().setValue(1.0);
	CHECK(d.f.deviceHost.fieldAt(d.device, eq::paramIndex(eq::Param::KeyTrack)) == 1.0f);
	const std::vector<size_t> addressed = controls.addressedParams();
	CHECK(std::find(addressed.begin(), addressed.end(), eq::paramIndex(eq::Param::KeyTrack)) != addressed.end());
}
TEST_CASE("EQ's crosshair reads the note, Hz and the curve's gain at the pointer") {
	namespace eq = feathers::fx::eq;
	DynCardFixture<ui::EqCard> d(eq::kTraits.tag);
	ui::EqGraph* g = d.card->graphForTest();
	d.set(eq::bandIndex(3, eq::BandField::Freq), static_cast<float>(std::log2(440.0)));
	d.set(eq::bandIndex(3, eq::BandField::Gain), 6.0f);
	const auto geo = g->geometry();
	// AT 440 Hz: A3 in the house naming, and the bell's own +6 dB at its centre (the others are flat).
	const std::string text = g->crosshairText({ geo.xForHz(440.0), geo.yForDb(0.0) });
	CAPTURE(text);
	CHECK(text.rfind("A3 +0c / 440.0 Hz", 0) == 0);
	CHECK(text.find("EQ +6.0 dB") != std::string::npos);
}
TEST_CASE("a routed knob's right-click lists its routes, a break, then its host rows") {
	FilterFixture f;
	emptyPool(f.bridge);
	ui::ModState* mod = f.gui.modForTest();
	REQUIRE(mod != nullptr);
	ui::Knob* knob = filterKnob(*f.card, "filter_cutoff_wide");
	REQUIRE(knob != nullptr);
	const backplane::Source source = backplane::Source::core(1);
	routeInto(*mod, *knob, source, 0.4f);
	REQUIRE(ui::inboundSources(*knob->control()).size() == 1);

	std::optional<visage::PopupMenu> menu = knob->contextMenu();
	REQUIRE(menu.has_value());
	const std::vector<SeenRow> rows = rowsOf(*menu);
	REQUIRE(rows.size() == 5);
	// THE LABEL RESTATED FROM THE FORMAT THE MENU PROMISES: the source's name, three spaces, the signed percent.
	CHECK(rows[0].label == mod->sourceName(source) + "   +40%");
	CHECK(rows[0].enabled);
	CHECK(rows[1].label.empty()); // the break
	CHECK(rows[2].label == "Remove modulator");
	CHECK(rows[3].label.empty()); // the break before the host section
	CHECK(rows[4].label == "Expose to host");
	CHECK(rows[4].id == ui::kControlMenuExpose);

	// AND THE ROUTE PICK DOES WHAT THE ROUTE MENU'S DOES: focus and arm that source.
	REQUIRE_FALSE(mod->armed());
	menu->onSelection().callback(rows[0].id);
	CHECK(mod->armed());
	CHECK(mod->source() == source);
	mod->arm(false);
}
TEST_CASE("a route pick from a popup left open across the card's destruction writes nothing") {
	// THE ROUTE HALF OF THE DESTRUCTION CASE ABOVE, which covers only a host pick: the route pick holds the
	// control by a raw pointer, and only the knob's token stands between it and a freed `DeviceControl`.
	FilterFixture f;
	emptyPool(f.bridge);
	ui::ModState* mod = f.gui.modForTest();
	REQUIRE(mod != nullptr);
	const feathers::DeviceInstanceId id = f.bridge.fxDeviceAt(0);
	const feathers::device::Descriptor* desc = f.deviceHost.descriptorOf(id);
	REQUIRE(desc != nullptr);
	ui::WidgetRegistry registry;
	std::unique_ptr<ui::FxCard> made = ui::makeCustomCard(f.deviceHost, registry, id, *desc, nullptr, mod);
	auto* card = dynamic_cast<ui::FilterCard*>(made.get());
	REQUIRE(card != nullptr);
	card->setModulation(mod, nullptr); // as the lane wires every card it builds (`FxLane::setModulation`)
	ui::Knob* knob = filterKnob(*card, "filter_cutoff_wide");
	REQUIRE(knob != nullptr);
	const backplane::Source source = backplane::Source::core(1);
	routeInto(*mod, *knob, source, 0.4f);
	std::optional<visage::PopupMenu> menu = knob->contextMenu();
	REQUIRE(menu.has_value());
	const std::vector<SeenRow> rows = rowsOf(*menu);
	REQUIRE(rows.size() >= 3);
	REQUIRE(rows[0].label == mod->sourceName(source) + "   +40%");

	// SOMETHING ELSE FOCUSED AND NOTHING ARMED, so a pick that got through would show.
	const backplane::Source other = backplane::Source::core(2);
	mod->focus(other);
	REQUIRE_FALSE(mod->armed());

	made.reset(); // the card, its knobs and its controls go; the device, the routes and the state stay
	menu->onSelection().callback(rows[0].id);
	CHECK_FALSE(mod->armed());
	CHECK(mod->source() == other);

	// THE CONTROL CASE: the same pick from the live card's menu does arm it — the silence above is the guard's.
	ui::Knob* live = filterKnob(*f.card, "filter_cutoff_wide");
	REQUIRE(live != nullptr);
	std::optional<visage::PopupMenu> liveMenu = live->contextMenu();
	REQUIRE(liveMenu.has_value());
	const std::vector<SeenRow> liveRows = rowsOf(*liveMenu);
	REQUIRE(liveRows[0].label == mod->sourceName(source) + "   +40%");
	liveMenu->onSelection().callback(liveRows[0].id);
	CHECK(mod->armed());
	CHECK(mod->source() == source);
	mod->arm(false);
}
TEST_CASE("a slot newly anchored by the host marks the patch; a write on an anchored one does not") {
	FilterFixture f;
	emptyPool(f.bridge);
	BridgeLookup lookup { f.bridge };
	hs::SlotHost host { f.bridge.slotPool(), lookup };
	ui::Knob* knob = filterKnob(*f.card, "filter_cutoff_wide");
	REQUIRE(knob != nullptr);
	REQUIRE(ui::chooseHostMenuEntry(*knob->control(), ui::kControlMenuExpose, nullptr));
	(void)visit(f.bridge, host);
	const hs::SlotIndex slot = knob->control()->hostSlot().slot;
	REQUIRE(slot == 0u);
	(void)f.bridge.takeStateDirty();

	f.bridge.slotBus().onHostValue(slot, static_cast<float>(std::log2(800.0)));
	(void)visit(f.bridge, host);
	REQUIRE(f.bridge.slotPool().state(slot) == hs::SlotState::Anchored);
	CHECK(f.bridge.takeStateDirty());

	// ALREADY ANCHORED: the host's value is the host's, and the saved pool did not change.
	f.bridge.slotBus().onHostValue(slot, static_cast<float>(std::log2(900.0)));
	(void)visit(f.bridge, host);
	CHECK_FALSE(f.bridge.takeStateDirty());
}
TEST_CASE("the standalone rack's knobs offer Expose to host too") {
	// THE RACK'S TOP BRIDGE OWNS A POOL SINCE T10 (`HostSlots::OwnPool`), and implements the four host-slot
	// verbs through the same `SlotOwner` Feathers' bridge does — so the same knob, over the rack's bridge,
	// lists the same row and its pick pins the same slot.
	feathers::rack::RackEngine engine { feathers::rack::kRackAxes, feathers::hostCatalogue() };
	engine.prepare(48000.0, 4096);
	feathers::rack::RackBridge rack { engine, nullptr, feathers::rack::RackBridge::HostSlots::OwnPool };
	REQUIRE(rack.hostSlots() != nullptr);
	const feathers::DeviceInstanceId id = rack.addDevice(feathers::fx::filter::kTraits.tag);
	rack.drain();
	REQUIRE(id != feathers::kNoInstance);
	const feathers::device::Descriptor* desc = rack.descriptorOf(id);
	REQUIRE(desc != nullptr);
	const size_t index = static_cast<size_t>(feathers::fx::filter::Param::Cutoff);
	ui::DeviceControl cutoff { rack, id, *desc, index };
	REQUIRE(std::string(cutoff.desc().key) == "filter_cutoff_wide");
	CHECK(cutoff.hostSlot().state == feathers::device::HostSlotStatus::State::Unslotted);
	ui::Knob knob { cutoff, "Cutoff" };
	CHECK_FALSE(knob.hostMarked());

	std::optional<visage::PopupMenu> menu = knob.contextMenu();
	REQUIRE(menu.has_value());
	std::vector<SeenRow> rows = rowsOf(*menu);
	REQUIRE(rows.size() == 1);
	CHECK(rows[0].label == "Expose to host");
	menu->onSelection().callback(rows[0].id);

	// THE POOL: slot 0, Pinned, naming this control; the mark follows at once (the generation moved).
	const std::optional<hs::Target> target = rack.hostSlots()->targetOf(id, index);
	REQUIRE(target.has_value());
	const hs::SlotIndex slot = rack.hostSlots()->pool().slotOf(*target);
	CHECK(slot == 0u);
	REQUIRE(slot != hs::kNoSlot);
	CHECK(rack.hostSlots()->pool().state(slot) == hs::SlotState::Pinned);
	CHECK(knob.hostMarked());
	// THE VISIT BINDS IT, and the engine drives it.
	(void)rack.serviceHostSlots();
	CHECK(rack.hostSlots()->boundTo(id, index) == slot);

	// THE MENU NOW SAYS WHERE IT IS; Unpin returns it to Recent and the mark goes.
	menu = knob.contextMenu();
	REQUIRE(menu.has_value());
	rows = rowsOf(*menu);
	REQUIRE(rows.size() == 2);
	CHECK(rows[0].label == "Host: Slot 1 " + kSlotDot + " Pinned");
	CHECK(rows[1].label == "Unpin");
	menu->onSelection().callback(rows[1].id);
	CHECK(rack.hostSlots()->pool().state(slot) == hs::SlotState::Recent);
	CHECK_FALSE(knob.hostMarked());
}

