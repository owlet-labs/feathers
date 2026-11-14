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
#include "plugin/ui/menu_anchor.h"
#include "plugin/ui/owltt_expanded.h"

#include "plugin/gui_bridge.h"
#include "plugin/ui/diagnostic.h"
#include "plugin/ui/theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <functional>
#include <iterator>
#include <string_view>
#include <array>

namespace feathers::ui {

namespace dtt = feathers::fx::owltt;

namespace {

/// THE GLOBAL ROW'S CONTROLS AND WHAT EACH IS CALLED THERE. The first six are SHIFTS AND OFFSETS over the bands (`effectiveBandOf`:
/// a band's own value plus the global's distance from neutral), which is why they share a row and a caption; Link, Width
/// and Blend are the stereo and mix settings that are not one band's.
struct GlobalEntry {
	dtt::Param param;
	const char* label;
};
constexpr GlobalEntry kGlobal[] = {
	{ dtt::Param::Threshold, "Threshold" },
	{ dtt::Param::Bandwidth, "Bandwidth" },
	{ dtt::Param::Ratio, "Ratio" },
	{ dtt::Param::UpDownSkew, "Up/Down" },
	{ dtt::Param::Attack, "Attack" },
	{ dtt::Param::Release, "Release" },
	{ dtt::Param::StereoLink, "Link" },
	{ dtt::Param::Width, "Width" },
	{ dtt::Param::Blend, "Blend" },
};
constexpr dtt::Param kGlobalParams[] = {
	kGlobal[0].param, kGlobal[1].param, kGlobal[2].param, kGlobal[3].param, kGlobal[4].param,
	kGlobal[5].param, kGlobal[6].param, kGlobal[7].param, kGlobal[8].param,
};

} // namespace

std::span<const dtt::Param> OwlttExpanded::globalParams() {
	return kGlobalParams;
}

OwlttExpanded::OwlttExpanded(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
                           Notifier* notifier)
    : host_(host), device_(id) {
	// REGISTRY UNUSED SINCE `Knob` STOPPED TAKING ONE; it stays in the signature because removing it touches every card.
	(void)registry;
	// THE PANEL'S OWN GRAPH, over the same device through the same host, as the editor: the crossover lane, the ratio thumbs
	// and the table of per-band boxes. Nothing is shared with the card's instance; the meters are a READ off the bridge.
	graph_ = std::make_unique<OwlttGraph>(host_, device_, notifier);
	graph_->setEditor(true);
	addChild(graph_.get());

	// L/R OR M/S (backlog LQ) — two states, so a chip, printing the live choice.
	stereo_ = std::make_unique<SwitchChip>(control(dtt::Param::StereoMode), "Stereo");
	addChild(stereo_.get());

	// DRIVE LEFT OF THE PLOT, OUTPUT RIGHT OF IT: the same fader, the same size, one on each side.
	drive_ = std::make_unique<VerticalFader>(control(dtt::Param::Drive), "Drive");
	output_ = std::make_unique<VerticalFader>(control(dtt::Param::OutputGain), "Output");
	// THE OUTPUT'S READOUT IS IN dB: its value is a 0..1 knob (0.5 unity), and the DSP's own mapping says what gain that is.
	output_->setReadoutFormatter([](double knob) {
		const float gain = dtt::outputGainFromKnob(static_cast<float>(knob));
		if (gain < 1.0e-4f) {
			return std::string("-inf");
		}
		const double db = 20.0 * std::log10(static_cast<double>(gain));
		if (std::fabs(db) < 0.05) {
			return std::string("0");
		}
		char text[16];
		std::snprintf(text, sizeof(text), std::fabs(db) < 9.95 ? "%+.1f" : "%+.0f", db);
		return std::string(text);
	});
	if (notifier != nullptr) {
		drive_->setAnnouncer([notifier](const std::string& line) { notifier->say(line); });
		output_->setAnnouncer([notifier](const std::string& line) { notifier->say(line); });
	}
	addChild(drive_.get());
	addChild(output_.get());

	// THE LOOKAHEAD (backlog ND) — a choice of latencies, so a box with its menu (every choice seen before it applies).
	lookahead_ = std::make_unique<ValueBox>(control(dtt::Param::Lookahead), "Lookahead");
	lookahead_->setCompact(true);
	addChild(lookahead_.get());
	// KEY TRACK — two states, a chip: the played note places the crossovers.
	keyTrack_ = std::make_unique<SwitchChip>(control(dtt::Param::KeyTrack), "Key Track");
	addChild(keyTrack_.get());
	// THE CLIP — Off, Master or Bands: three choices that do not fit a chip, so a box that opens its menu.
	clip_ = std::make_unique<ValueBox>(control(dtt::Param::SoftClip), "Clip");
	clip_->setCompact(true);
	addChild(clip_.get());

	// THE GLOBAL ROW: boxes in the house form (a caption over a value), one per control.
	for (const GlobalEntry& entry : kGlobal) {
		auto box = std::make_unique<ValueBox>(control(entry.param), entry.label);
		addChild(box.get());
		cells_.push_back(box.get());
		globalBoxes_.push_back(std::move(box));
	}

	// THE TONE GROUP: Character and Vibe side by side. Each is a mode cell: a dropdown of its eight behaviours and Free, and the
	// one unbounded value (its fraction how far into the behaviour, its whole part the lap; in Free the rolling coordinate).
	const auto addCoord = [&](dtt::Param coordinate, dtt::Param mode, const char* label,
	                          std::span<const char* const> modeNames, std::span<const dtt::ZoneName> descriptions) {
		std::vector<ZoneSelect::ZoneName> names;
		for (size_t i = 0; i < modeNames.size(); ++i) {
			names.push_back({ modeNames[i], i < descriptions.size() ? descriptions[i].description
			                                                         : "Rolls through all eight as the value turns" });
		}
		auto cell = std::make_unique<ModeValueCell>(control(mode), label, std::move(names), [](size_t) { return true; });
		cell->addFollower(static_cast<size_t>(coordinate), std::make_unique<ValueBox>(control(coordinate), ""));
		cell->setFaceDiameter(kToneFace);
		cell->refresh();
		addChild(cell.get());
		cells_.push_back(cell.get());
		modeCells_.push_back(std::move(cell));
		coordParams_.push_back(coordinate);
		coordParams_.push_back(mode);
	};
	addCoord(dtt::Param::Character, dtt::Param::CharacterMode, "Character", dtt::kCharacterModeNames, dtt::kCharacterZoneNames);
	addCoord(dtt::Param::Vibe, dtt::Param::VibeMode, "Vibe", dtt::kVibeModeNames, dtt::kVibeZoneNames);

	// THE MODE PICKER IS THIS BODY'S ONE HAND-DRAWN CONTROL. The enable square that shared the row is the window header's chip.
	onMouseDown() = [this](const visage::MouseEvent& e) {
		if (modeBounds().contains(e.position)) {
			showModeMenu();
		}
	};
}

DeviceControl& OwlttExpanded::control(dtt::Param param) const {
	const size_t index = static_cast<size_t>(param);
	if (controls_[index] == nullptr) {
		// THE GENERIC ROAD — a descriptor and a control index, never a device type. The same road the card takes to the same
		// rows, so the two surfaces cannot disagree about a value.
		controls_[index] = std::make_unique<DeviceControl>(host_, device_, dtt::kDescriptor, index);
	}
	return *controls_[index];
}

bool OwlttExpanded::updateContent() {
	bool changed = graph_ != nullptr && graph_->update();
	for (const auto& cell : modeCells_) {
		changed = cell->refresh() || changed;
	}
	return changed;
}

void OwlttExpanded::modulationBound(ModState* mod, EditHistory* history) {
	// THE PLOT'S HANDLES ARE DESTINATIONS TOO, exactly as the boxes are — the same controls reached two ways.
	if (graph_ != nullptr) {
		graph_->setModulation(mod, history);
	}
	// EVERY ADAPTER, INCLUDING THE ONES NO WIDGET HOLDS: `DeviceControl::setAmount` and `display` ask the adapter's OWN `mod_`,
	// and an adapter built by `control()` has none until this says so (HARM shipped without this line and could not be
	// modulated at all).
	for (size_t i = 0; i < controls_.size(); ++i) {
		control(static_cast<dtt::Param>(i)).setModulation(mod, history);
	}
}

std::string OwlttExpanded::title() const {
	// THE DEVICE'S OWN NAME, from its traits — the same authority the card's head asks.
	return dtt::kTraits.name;
}

float OwlttExpanded::modalBottom() const {
	return kPad + kSwitchHeight;
}

visage::Bounds OwlttExpanded::graphBounds() const {
	const float side = kPad + kDriveWidth + kDriveGap;
	return { side, modalBottom() + kGroupGap, std::max(0.0f, width() - 2.0f * side), kGraphHeight };
}

visage::Bounds OwlttExpanded::driveBounds() const {
	return { kPad, modalBottom() + kGroupGap, kDriveWidth, kGraphHeight };
}

visage::Bounds OwlttExpanded::outputBounds() const {
	// THE MIRROR OF THE DRIVE: the same size, the same distance from the plot and from the window's edge.
	return { std::max(kPad, width() - kPad - kDriveWidth), modalBottom() + kGroupGap, kDriveWidth, kGraphHeight };
}

visage::Bounds OwlttExpanded::globalRow() const {
	const visage::Bounds plot = graphBounds();
	const float x = kPad + kBandLabelWidth;
	return { x, plot.bottom() + kRowGap + 4.0f, std::max(0.0f, width() - kPad - x), ValueBox::kHeight };
}

visage::Bounds OwlttExpanded::toneRow() const {
	const visage::Bounds row = globalRow();
	return { row.x(), row.bottom() + kRowGap + 4.0f, row.width(), Knob::columnHeight(kToneFace) };
}

visage::Bounds OwlttExpanded::keyTrackBounds() const {
	return { kPad, kPad + (kSwitchHeight - SwitchChip::kHeight) * 0.5f, kKeyTrackWidth, SwitchChip::kHeight };
}

visage::Bounds OwlttExpanded::lookaheadBounds() const {
	const visage::Bounds before = keyTrackBounds();
	return { before.right() + kRowGap, kPad + (kSwitchHeight - ValueBox::kCompactHeight) * 0.5f, kLookaheadWidth,
		     ValueBox::kCompactHeight };
}

visage::Bounds OwlttExpanded::stereoBounds() const {
	const visage::Bounds before = lookaheadBounds();
	return { before.right() + kRowGap, kPad + (kSwitchHeight - SwitchChip::kHeight) * 0.5f, kChipWidth, SwitchChip::kHeight };
}

visage::Bounds OwlttExpanded::modeBounds() const {
	// THE CROSSOVER TYPE, because it is a verb like the enable rather than a value: it decides which filter topology the
	// whole device is built from.
	const visage::Bounds before = stereoBounds();
	return { before.right() + kRowGap, kPad, kModeWidth, kSwitchHeight };
}

visage::Bounds OwlttExpanded::clipBounds() const {
	const visage::Bounds before = modeBounds();
	return { before.right() + kRowGap, kPad + (kSwitchHeight - ValueBox::kCompactHeight) * 0.5f, kClipWidth,
		     ValueBox::kCompactHeight };
}

std::string OwlttExpanded::modeText() const {
	// THE CARD'S OWN WORDS, from the same `kModeNames` its folded head prints.
	const auto at = static_cast<int32_t>(std::lround(control(dtt::Param::Mode).value()));
	const int32_t index = at - 1; // the parameter runs 1..10; the names run 0..9
	if (index < 0 || static_cast<size_t>(index) >= std::size(dtt::kModeNames)) {
		return {};
	}
	return dtt::kModeNames[static_cast<size_t>(index)];
}

void OwlttExpanded::chooseMode(size_t index) {
	if (index >= std::size(dtt::kModeNames)) {
		return;
	}
	DeviceControl& c = control(dtt::Param::Mode);
	// BRACKETED AS ONE EDIT, like every other value write on this surface.
	c.beginGesture();
	c.setValue(static_cast<double>(index) + 1.0); // names are 0-based, the parameter is 1-based
	c.endGesture();
	redraw();
}

void OwlttExpanded::showModeMenu() {
	visage::PopupMenu menu;
	for (size_t i = 0; i < std::size(dtt::kModeNames); ++i) {
		menu.addOption(static_cast<int>(i), dtt::kModeNames[i]);
	}
	menu.onSelection() = [this](int id) {
		if (id >= 0) {
			chooseMode(static_cast<size_t>(id));
		}
	};
	const visage::Bounds box = modeBounds();
	showMenuAt(std::move(menu), *this, box);
}

visage::Point OwlttExpanded::maximumSize() const {
	return { kNoCap, minimumSize().y + kPad };
}

visage::Point OwlttExpanded::minimumSize() const {
	const float plotBottom = modalBottom() + kGroupGap + kGraphHeight;
	const float tone = plotBottom + kRowGap + 4.0f + ValueBox::kHeight + kRowGap + 4.0f + Knob::columnHeight(kToneFace);
	return { 2.0f * kPad + kBodyWidth, tone + kPad };
}

void OwlttExpanded::resized() {
	const auto place = [](visage::Frame* frame, const visage::Bounds& b) {
		if (frame != nullptr) {
			frame->setBounds(b.x(), b.y(), b.width(), b.height());
		}
	};
	place(keyTrack_.get(), keyTrackBounds());
	place(lookahead_.get(), lookaheadBounds());
	place(stereo_.get(), stereoBounds());
	place(clip_.get(), clipBounds());
	place(drive_.get(), driveBounds());
	place(output_.get(), outputBounds());
	const visage::Bounds plot = graphBounds();
	if (graph_ != nullptr) {
		graph_->setBounds(plot.x(), plot.y(), plot.width(), plot.height());
#ifdef FEATHERS_DIAGNOSTIC
		diagnostic::plotLaidOut("panel", graph_->positionInWindow().x, graph_->positionInWindow().y,
		                        graph_->width(), graph_->height(), graph_->isVisible());
#endif
	}

	// THE GLOBAL ROW: nine boxes sharing the width, a small gap between.
	const visage::Bounds row = globalRow();
	const size_t n = globalBoxes_.size();
	if (n > 0) {
		const float w = (row.width() - kBoxGap * static_cast<float>(n - 1)) / static_cast<float>(n);
		for (size_t i = 0; i < n; ++i) {
			place(globalBoxes_[i].get(), { row.x() + static_cast<float>(i) * (w + kBoxGap), row.y(), w, row.height() });
		}
	}
	// THE TONE GROUP: the two cells side by side, inside one frame.
	const visage::Bounds tone = toneRow();
	const size_t cells = modeCells_.size();
	if (cells > 0) {
		const float w = (tone.width() - kBoxGap * static_cast<float>(cells - 1)) / static_cast<float>(cells);
		for (size_t i = 0; i < cells; ++i) {
			place(modeCells_[i].get(), { tone.x() + static_cast<float>(i) * (w + kBoxGap), tone.y(), w, tone.height() });
		}
	}
}

void OwlttExpanded::draw(visage::Canvas& canvas) {
	// NO PANEL BACKGROUND — the overlay draws one and a second would be a card inside a card.

	// THE CROSSOVER TYPE. A framed readout rather than a bare word, because it is pressable and a word that opens a menu with
	// nothing around it is not discoverable.
	const visage::Bounds mode = modeBounds();
	canvas.setColor(withAlpha(kLine, 0.55f));
	canvas.roundedRectangleBorder(mode.x(), mode.y(), mode.width(), mode.height(), 3.0f, 1.0f);
	canvas.setColor(kInk);
	canvas.text(modeText(), font(kKnobReadoutSize), visage::Font::kLeft, mode.x() + 7.0f, mode.y(),
	            mode.width() - 20.0f, mode.height());
	// A CHEVRON, GEOMETRY AND NOT A GLYPH — this font has no guaranteed arrow.
	{
		const float cx = mode.right() - 10.0f;
		const float cy = mode.y() + mode.height() * 0.5f;
		canvas.setColor(kMuted);
		canvas.segment(cx - 3.5f, cy - 1.5f, cx, cy + 2.0f, 1.3f, true);
		canvas.segment(cx, cy + 2.0f, cx + 3.5f, cy - 1.5f, 1.3f, true);
	}

	// THE ROWS' HEADINGS, in the margin, as the Delay window's.
	const visage::Bounds global = globalRow();
	const visage::Bounds tone = toneRow();
	canvas.setColor(kMuted);
	canvas.text("GLOBAL", font(kKnobLabelSize), visage::Font::kLeft, kPad, global.y(), kBandLabelWidth, global.height());
	canvas.text("TONE", font(kKnobLabelSize), visage::Font::kLeft, kPad, tone.y(), kBandLabelWidth, tone.height());
}

dtt::Param OwlttExpanded::paramOfGlobalForTest(const ValueBox* box) const {
	for (size_t i = 0; i < globalBoxes_.size() && i < std::size(kGlobal); ++i) {
		if (globalBoxes_[i].get() == box) {
			return kGlobal[i].param;
		}
	}
	return dtt::Param::kCount;
}

std::vector<dtt::Param> OwlttExpanded::addressedParamsForTest() const {
	// WHAT THIS SURFACE REALLY REACHES, as the widgets that were built declare it: every `ControlFace` frame in the tree (the
	// plot's handles, the table's boxes, the boxes, the faders, the chips and the mode cells) plus the hand-drawn crossover
	// type. Nothing is claimed that no hand can reach.
	std::vector<dtt::Param> out;
	std::vector<ControlFace::Offered> offered;
	std::function<void(const visage::Frame*)> walk = [&](const visage::Frame* frame) {
		if (const auto* face = dynamic_cast<const ControlFace*>(frame)) {
			offered.clear();
			face->offeredControls(offered);
			for (const ControlFace::Offered& o : offered) {
				// BY KEY, not by object: the plot builds its own adapters over the same device.
				for (size_t i = 0; i < std::size(dtt::kParamDescs); ++i) {
					const auto p = static_cast<dtt::Param>(i);
					if (std::string_view(o.control->desc().key) == std::string_view(dtt::kParamDescs[i].key)
					    && std::find(out.begin(), out.end(), p) == out.end()) {
						out.push_back(p);
					}
				}
			}
		}
		for (const visage::Frame* child : frame->children()) {
			walk(child);
		}
	};
	walk(this);
	return out;
}

void OwlttExpanded::offeredControls(std::vector<Offered>& out) const {
	// THE MODE, drawn by this view and chosen from its menu (`chooseMode`) — every mode on demand before it applies.
	out.push_back({ &control(dtt::Param::Mode), Road::Face });
}

} // namespace feathers::ui
