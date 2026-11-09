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
#include "plugin/ui/harm_expanded.h"

#include "device/device.h"
#include "plugin/ui/theme.h"

#include <algorithm>
#include <cmath>

namespace feathers::ui {

namespace hrm = feathers::fx::harm;

std::span<const HarmExpanded::KnobGroup> HarmExpanded::groups() {
	// SPECTRAL FIRST AND LARGEST, because it is the group the picture is about — the harmonic row and
	// the scope sit inside it, above its knobs. Level leads: it decides whether the sub is sounding
	// at all, and the scope's fallback to 1:1 hangs off it.
	static constexpr KnobEntry kSpectral[] = {
		{ fx::harm::Param::Level, "Level" },
		{ fx::harm::Param::Fine, "Fine" },
		{ fx::harm::Param::Width, "Width" },
		{ fx::harm::Param::Depth, "Depth" },
	};
	// THE SCOPE IS MONO AND CANNOT SHOW THESE TWO. That is not a defect in the picture; it is why
	// they are a separate group rather than two more circles under it.
	static constexpr KnobEntry kSpatial[] = {
		{ fx::harm::Param::Phase, "Phase" },
		{ fx::harm::Param::Spread, "Spread" },
	};
	// AND THE SCOPE HAS NO TIME AXIS AT ALL, which is the sharpest of the four distinctions: no
	// arrangement of the picture would ever show these.
	static constexpr KnobEntry kTemporal[] = {
		{ fx::harm::Param::Attack, "Attack" },
		{ fx::harm::Param::Release, "Release" },
		{ fx::harm::Param::Porta, "Porta" },
	};
	static constexpr KnobGroup kGroups[] = {
		{ "SPECTRAL", kSpectral },
		{ "SPATIAL", kSpatial },
		{ "TEMPORAL", kTemporal },
	};
	return kGroups;
}

HarmExpanded::HarmExpanded(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
                           Notifier* notifier)
    : host_(host), device_(id) {
	scope_ = std::make_unique<HarmScope>(host_, device_);
	addChild(scope_.get());

	// THE HARMONIC ROW, THE SAME WIDGET THE CARD BUILDS. This is what closes the gap the header used
	// to record: the scope below plots the notch where the harmonic puts it, and until now a hand
	// could read that and not change it.
	row_ = std::make_unique<HarmonicRow>(control(hrm::Param::Harmonic));
	// THE SCOPE REDRAWS WHEN THE HARMONIC MOVES, which is the whole reason this row belongs on this
	// panel: the picture and the control are the same gesture.
	row_->onChanged = [this] {
		if (scope_ != nullptr) {
			scope_->redraw();
		}
		redraw();
	};
	addChild(row_.get());

	const auto addKnob = [&](hrm::Param param, const char* label) {
		auto knob = std::make_unique<Knob>(control(param), label, notifier);
		knob->setFaceDiameter(kKnobDiameter);
		addChild(knob.get());
		knobs_.push_back(std::move(knob));
		// RECORDED AS THE KNOB IS BUILT, in the one place that knows which control it was built for.
		// FilterCard's rule, and the reachability invariant reads this rather than a list kept beside
		// it — so a knob added here is reachable-by-declaration without anybody remembering.
		knobParams_.push_back(param);
	};

	// BUILT FROM THE DECLARED GROUPS, in the order they are drawn — so `knobs_` and the layout walk
	// the same list and cannot disagree about which knob is where.
	for (const KnobGroup& group : groups()) {
		for (const KnobEntry& entry : group.knobs) {
			addKnob(entry.param, entry.label);
		}
	}

	auto poly = std::make_unique<SegmentedControl>(control(hrm::Param::Poly), registry,
	                                               hrm::kOffOnNames, notifier);
	// ALIVE OR DEAD BY THE DEVICE'S OWN DECLARATION, exactly as the card decides it — HARM declares a
	// `perVoiceControl` now, so it is alive. Asked rather than assumed, and asked BEFORE the first
	// paint, because a hand arrives through the mouse road and the mouse road does not wait for a
	// repaint.
	poly->setEnabled(hrm::kDescriptor.traits.perVoiceControl >= 0);
	addChild(poly.get());
	poly_ = std::move(poly);

	// DEPTH FOLLOWS ENV — Off and On both on its face, the Poly switch's form (a multivalue toggle is the
	// project's antipattern; two states drawn side by side is not one).
	depthEnv_ = std::make_unique<SegmentedControl>(control(hrm::Param::DepthFollowsEnv), registry,
	                                               hrm::kOffOnNames, notifier);
	addChild(depthEnv_.get());

	// NOTCH OR HIGH-PASS (backlog NL) — a chip, because it has two states and both are named on it as it flips.
	mode_ = std::make_unique<SwitchChip>(control(hrm::Param::Mode), "Mode");
	addChild(mode_.get());
	followMode(host_.fieldAt(device_, static_cast<size_t>(hrm::Param::Mode)) >= 0.5f);

	// **THE ENABLE SQUARE THAT STOOD HERE IS THE WINDOW HEADER'S NOW** (devices had been
	// inconsistently adding their own active chips in the window). The panel chrome draws one chip
	// for every device, over the same declared control (`ExpandedView::toggleDeviceEnable`).
}

DeviceControl& HarmExpanded::control(hrm::Param param) const {
	const size_t index = static_cast<size_t>(param);
	if (controls_[index] == nullptr) {
		// THE GENERIC ROAD — a descriptor and a control index, never a device type. The same road the
		// card takes to the same rows, so the two surfaces cannot disagree about a value: there is
		// one bridge under both.
		controls_[index] = std::make_unique<DeviceControl>(host_, device_, hrm::kDescriptor, index);
	}
	return *controls_[index];
}

void HarmExpanded::modulationBound(ModState* mod, EditHistory* history) {
	for (auto& knob : knobs_) {
		knob->setModulation(mod, history);
	}
	// AND THE SCOPE, whose grips are modulation destinations exactly as the knobs are — the same five
	// controls reached two ways. A picture whose handles could not be armed would be a set of
	// controls that silently is not one.
	if (scope_ != nullptr) {
		scope_->setModulation(mod, history);
	}
	// POLY IS NOT A DESTINATION and is not offered one — the device declares it unmodulatable because
	// it decides how many of the device there are rather than what one of them does. The card says
	// the same thing in the same words.
	// AND EVERY ADAPTER, INCLUDING THE ONES NO KNOB HOLDS.
	//
	// THIS LINE WAS MISSING AND HARM COULD NOT BE MODULATED AT ALL. A Knob forwards to the control it
	// was given, so the knobs' adapters were covered — but `DeviceControl::setAmount` and
	// `DeviceControl::display` both ask the adapter's OWN `mod_`, and every one of these was built by
	// `control()` with none. Ten declared destinations, `isModDestination()` true on all of them, and
	// `mappable()` false forever: the arrow armed, the knobs stayed dark, and nothing said why.
	//
	// `FilterCard::setModulation` HAS DONE BOTH SINCE IT WAS WRITTEN and its comment says exactly why
	// — *"an adapter that never learned the panel's state reports itself unarmed forever. Cheap,
	// total, and it cannot fall behind a widget list."* The rule was written down in the sibling file
	// and not followed here, which is the shape this repo keeps paying for: a comment stating a rule
	// is not the rule being followed.
	for (size_t i = 0; i < controls_.size(); ++i) {
		control(static_cast<hrm::Param>(i)).setModulation(mod, history);
	}
}

std::string HarmExpanded::title() const {
	// THE DEVICE'S OWN NAME, from its traits — the same authority the card's head asks, so the panel
	// and the card cannot disagree about which device is open.
	return hrm::kTraits.name;
}

visage::Point HarmExpanded::maximumSize() const {
	return { kNoCap, minimumSize().y + kPad };
}

visage::Point HarmExpanded::minimumSize() const {
	const float column = Knob::columnHeight(kKnobDiameter);
	// SUMMED OVER THE DECLARED GROUPS, so a control moving between them moves the height with it.
	// The spectral group carries the harmonic row and the scope as well as its knobs; the others are
	// a caption and their rows.
	float height = kPad + kSwitchHeight; // the modal row
	for (size_t g = 0; g < groups().size(); ++g) {
		const KnobGroup& group = groups()[g];
		height += kGroupGap + kCaptionHeight + kRowGap;
		if (g == 0) {
			height += HarmonicRow::kHeight + kRowGap + kScopeHeight + kRowGap;
		}
		const size_t rows = group.knobs.empty()
		                        ? 0
		                        : (group.knobs.size() + static_cast<size_t>(kRowCells) - 1)
		                              / static_cast<size_t>(kRowCells);
		height += static_cast<float>(rows) * column
		          + (rows > 1 ? static_cast<float>(rows - 1) * kRowGap : 0.0f);
	}
	height += kPad;
	return { 2.0f * kPad + static_cast<float>(kRowCells) * kCellWidth, height };
}

visage::Bounds HarmExpanded::harmonicBounds() const {
	// INSIDE THE SPECTRAL GROUP AND ABOVE THE SCOPE, because the scope is a picture of what this row
	// is set to. A control below its own readout reads as a caption for it.
	return { kPad, modalBottom() + kGroupGap + kCaptionHeight + kRowGap,
		     std::max(0.0f, width() - 2.0f * kPad), HarmonicRow::kHeight };
}

visage::Bounds HarmExpanded::scopeBounds() const {
	return { kPad, harmonicBounds().bottom() + kRowGap, std::max(0.0f, width() - 2.0f * kPad),
		     kScopeHeight };
}

float HarmExpanded::modalBottom() const {
	return kPad + kSwitchHeight;
}

visage::Bounds HarmExpanded::polyBounds() const {
	// THE MODAL ROW, AT THE TOP. Poly is a verb rather than a value — it decides how many of the device
	// there are — so it heads the panel instead of trailing it. (Enabled, the other verb, is the window
	// header's chip now, one row up.) Right-aligned, with its caption against it.
	return { std::max(kPad, width() - kPad - kSwitchWidth), kPad, kSwitchWidth, kSwitchHeight };
}

visage::Bounds HarmExpanded::polyLabelBounds() const {
	// LEFT OF THE SWITCH NOW, because the switch is right-aligned in the modal row and a caption
	// outboard of it would be off the panel. Right-aligned into the gap, so the word sits against
	// the control it names.
	const visage::Bounds box = polyBounds();
	const float left = modeBounds().right() + 8.0f; // clear of the Mode chip
	return { left, box.y(), std::max(0.0f, box.x() - 8.0f - left), box.height() };
}

visage::Bounds HarmExpanded::modeBounds() const {
	// THE LEFT OF THE MODAL ROW, centred on the Poly switch's line.
	return { kPad, kPad + (kSwitchHeight - SwitchChip::kHeight) * 0.5f, kModeChipWidth, SwitchChip::kHeight };
}

void HarmExpanded::followMode(bool highpass) {
	drawnMode_ = highpass ? 1 : 0;
	for (size_t i = 0; i < knobs_.size() && i < knobParams_.size(); ++i) {
		if (knobParams_[i] == hrm::Param::Width || knobParams_[i] == hrm::Param::Depth) {
			knobs_[i]->setEnabled(!highpass);
		}
	}
	if (depthEnv_ != nullptr) {
		depthEnv_->setEnabled(!highpass);
	}
	if (scope_ != nullptr) {
		scope_->redraw();
	}
	redraw();
}

bool HarmExpanded::updateContent() {
	// THE FIELD, NOT A CONTROL — `HarmCard::updateContent`'s rule: a tick must not construct widgets.
	const bool highpass = host_.fieldAt(device_, static_cast<size_t>(hrm::Param::Mode)) >= 0.5f;
	if (drawnMode_ == (highpass ? 1 : 0)) {
		return false;
	}
	followMode(highpass);
	return true;
}

std::string HarmExpanded::polyText() const {
	// THE CARD'S OWN WORDS, and the dead branch kept for the card's own reason: a working control
	// still labelled "not built yet" is the same lie as a dead one labelled "Poly", pointing the
	// other way. `perVoiceControl` is the question both surfaces ask.
	return hrm::kDescriptor.traits.perVoiceControl >= 0 ? "Poly"
	                                                    : "Poly — per-voice not built yet";
}

void HarmExpanded::resized() {
	const visage::Bounds row = harmonicBounds();
	row_->setBounds(row.x(), row.y(), row.width(), row.height());

	const visage::Bounds scope = scopeBounds();
	scope_->setBounds(scope.x(), scope.y(), scope.width(), scope.height());

	const visage::Bounds poly = polyBounds();
	poly_->setBounds(poly.x(), poly.y(), poly.width(), poly.height());
	const visage::Bounds mode = modeBounds();
	mode_->setBounds(mode.x(), mode.y(), mode.width(), mode.height());

	const float column = Knob::columnHeight(kKnobDiameter);
	const float inner = std::max(0.0f, width() - 2.0f * kPad);
	const float cell = inner / static_cast<float>(kRowCells);

	// ONE WALK OVER THE DECLARED GROUPS, placing captions and knobs together — so the caption a case
	// reads is the caption `draw` writes, at the rectangle `resized` chose. Two places computing one
	// geometry is the seam this lane has produced three times in a week.
	captionBounds_.clear();
	float top = modalBottom();
	size_t next = 0;
	for (size_t g = 0; g < groups().size(); ++g) {
		const KnobGroup& group = groups()[g];
		top += kGroupGap;
		captionBounds_.push_back({ kPad, top, inner, kCaptionHeight });
		top += kCaptionHeight + kRowGap;
		if (g == 0) {
			top = scope.bottom() + kRowGap; // the harmonic row and the scope live inside this group
		}
		for (size_t i = 0; i < group.knobs.size() && next < knobs_.size(); ++i, ++next) {
			const size_t line = i / static_cast<size_t>(kRowCells);
			const size_t at = i % static_cast<size_t>(kRowCells);
			knobs_[next]->setBounds(kPad + cell * static_cast<float>(at),
			                        top + static_cast<float>(line) * (column + kRowGap), cell,
			                        column);
		}
		// THE TEMPORAL GROUP'S FREE CELL holds the Depth Follows Env switch, centred in the knob column below
		// its caption — the group is three knobs in four cells, so nothing moves to make room for it.
		if (g + 1 == groups().size() && group.knobs.size() < static_cast<size_t>(kRowCells)) {
			const float x = kPad + cell * static_cast<float>(group.knobs.size());
			const float w = std::max(0.0f, cell - 8.0f);
			depthEnvBounds_ = { x + 4.0f, top + (column - kSwitchHeight) * 0.5f, w, kSwitchHeight };
			depthEnv_->setBounds(depthEnvBounds_.x(), depthEnvBounds_.y(), depthEnvBounds_.width(),
			                     depthEnvBounds_.height());
		}
		const size_t rows = group.knobs.empty()
		                        ? 0
		                        : (group.knobs.size() + static_cast<size_t>(kRowCells) - 1)
		                              / static_cast<size_t>(kRowCells);
		top += static_cast<float>(rows) * column
		       + (rows > 1 ? static_cast<float>(rows - 1) * kRowGap : 0.0f);
	}
}

visage::Bounds HarmExpanded::depthEnvLabelBounds() const {
	// ABOVE THE SWITCH, in its own cell — the word that makes "Off | On" mean something.
	const visage::Bounds box = depthEnvBounds_;
	return { box.x() - 4.0f, box.y() - kCaptionHeight - 4.0f, box.width() + 8.0f, kCaptionHeight };
}

std::vector<hrm::Param> HarmExpanded::addressedParamsForTest() const {
	// THE KNOBS ANSWER FOR THEMSELVES and the switch is named because it is the one bespoke control
	// here. `Enabled` and `Harmonic` are NOT offered by this surface — see the header for why — and
	// claiming them would make the reachability invariant pass on a control no hand can reach here.
	std::vector<hrm::Param> out = knobParams_;
	if (poly_ != nullptr) {
		out.push_back(hrm::Param::Poly);
	}
	if (depthEnv_ != nullptr) {
		out.push_back(hrm::Param::DepthFollowsEnv);
	}
	if (mode_ != nullptr) {
		out.push_back(hrm::Param::Mode);
	}
	if (row_ != nullptr) {
		out.push_back(hrm::Param::Harmonic); // the shared row: two steppers and a 102-entry menu
	}
	// `Enabled` IS NOT THIS BODY'S. The window header's chip reaches it for every panel
	// (`ExpandedPanel::enableBounds`), and the card's head square reaches it on the card.
	return out;
}

void HarmExpanded::draw(visage::Canvas& canvas) {
	// NO GROUND OF ITS OWN. The overlay's panel is already a raised surface and a second rectangle on
	// top of it would be a card inside a card. The knobs, the row and the scope draw themselves.

	// THE SWITCH'S CAPTION. Without it the control is the words "Off" and "On" with no subject —
	// the defect `filter_card.cpp` names in as many words, and one this panel shipped with for
	// exactly one render before a capture caught it.
	const bool polyLive = hrm::kDescriptor.traits.perVoiceControl >= 0;
	const visage::Bounds polyLabel = polyLabelBounds();
	canvas.setColor(polyLive ? kMuted : kDisabled);
	canvas.text(polyText(), font(kKnobReadoutSize), visage::Font::kRight, polyLabel.x(),
	            polyLabel.y(), polyLabel.width(), polyLabel.height());
	const visage::Bounds envLabel = depthEnvLabelBounds();
	if (depthEnvBounds_.width() > 0.0f) {
		canvas.setColor(drawnMode_ == 1 ? kDisabled : kMuted); // greyed with its switch in High-pass
		canvas.text("Env Depth", font(kKnobReadoutSize), visage::Font::kCenter, envLabel.x(), envLabel.y(),
		            envLabel.width(), envLabel.height());
	}

	// THE GROUP CAPTIONS, AND THEY ARE THE DESIGN PASS, so the expanded view is more intuitive
	// than a knob list. A list is UNDIFFERENTIATED — these twelve are
	// four different kinds of thing, and drawing all of them identically is what made eleven knobs
	// read as a row of circles rather than as a device.
	//
	// READ FROM `captionBounds_` RATHER THAN RECOMPUTED, so the caption is written where `resized`
	// put it. Two places computing one geometry is the seam this lane has produced three times in a
	// week, and it costs nothing to not do it a fourth.
	const std::span<const KnobGroup> all = groups();
	for (size_t g = 0; g < all.size() && g < captionBounds_.size(); ++g) {
		const visage::Bounds caption = captionBounds_[g];
		canvas.setColor(withAlpha(kMuted, 0.75f));
		drawTracked(canvas, all[g].caption, font(kKnobLabelSize), kKnobLabelTracking, caption.x(),
		            caption.y(), caption.height());
		// A HAIRLINE ACROSS THE REST OF THE ROW, so the caption reads as a section head rather than
		// as a stray word above a knob.
		const float textWidth =
		    trackedWidth(font(kKnobLabelSize), all[g].caption, kKnobLabelTracking);
		const float ruleLeft = caption.x() + textWidth + 8.0f;
		if (caption.right() > ruleLeft) {
			canvas.setColor(withAlpha(kLine, 0.55f));
			canvas.fill(ruleLeft, caption.y() + caption.height() * 0.5f, caption.right() - ruleLeft,
			            1.0f);
		}
	}
}

} // namespace feathers::ui
