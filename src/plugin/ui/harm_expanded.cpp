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

namespace {

/// THE ENVELOPE'S SIX VALUES, in the order they happen along a note, with the words the boxes carry.
struct EnvelopeEntry {
	hrm::Param param;
	const char* label;
};
constexpr EnvelopeEntry kEnvelopeEntries[] = {
	{ hrm::Param::Delay, "Delay" },   { hrm::Param::Attack, "Attack" }, { hrm::Param::Hold, "Hold" },
	{ hrm::Param::Decay, "Decay" },   { hrm::Param::Sustain, "Sustain" }, { hrm::Param::Release, "Release" },
};
constexpr int kEnvelopeColumns = 3;
constexpr float kBoxGap = 8.0f;

/// THE SELECTED PARTIAL'S KNOBS: SPECTRAL's three and SPATIAL's two.
struct KnobEntry {
	hrm::PartialField field;
	const char* label;
};
constexpr KnobEntry kKnobEntries[] = {
	{ hrm::PartialField::Fine, "Fine" },   { hrm::PartialField::Width, "Width" }, { hrm::PartialField::Depth, "Depth" },
	{ hrm::PartialField::Phase, "Phase" }, { hrm::PartialField::Spread, "Spread" },
};
constexpr int kSpectralKnobs = 3;

} // namespace

/// EVERY RECTANGLE OF THE WINDOW, from its width — computed once, in one place, so what `resized` places, what `draw` writes and
/// what a case reads are one geometry.
struct HarmExpanded::Layout {
	visage::Bounds partialsCaption, strip;
	visage::Bounds spectralCaption, harmonic, mode, scope, spatialCaption, knobRow;
	visage::Bounds envelopeCaption, envelope;
	std::array<visage::Bounds, 6> boxes {};
	visage::Bounds masterCaption, master, porta, poly, depthEnv;
	float knobCell = 0.0f;
	float bottom = 0.0f;
};

HarmExpanded::Layout HarmExpanded::layout() const {
	return layoutFor(width());
}

HarmExpanded::Layout HarmExpanded::layoutFor(float totalWidth) const {
	Layout l;
	const float inner = std::max(0.0f, totalWidth - 2.0f * kPad);
	// THE DETAIL'S TWO COLUMNS SHARE WHATEVER WIDTH THERE IS in the proportion they were designed in.
	const float columns = std::max(0.0f, inner - kColumnGap);
	const float leftW = columns * kLeftWidth / (kLeftWidth + kRightWidth);
	const float rightW = columns - leftW;
	const float rightX = kPad + leftW + kColumnGap;

	float y = kPad;
	l.partialsCaption = { kPad, y, inner, kCaptionHeight };
	y += kCaptionHeight + kCaptionGap;
	l.strip = { kPad, y, inner, HarmPartialStrip::kHeight };
	y += HarmPartialStrip::kHeight + kRowGap;
	// THE STRIP IS THE TAB BAR: a press on a partial's cell opens it, so there is no row of tabs between it and the detail.
	y += kGroupGap - kRowGap;
	const float bodyTop = y;

	// LEFT: THE SELECTED PARTIAL.
	y = bodyTop;
	l.spectralCaption = { kPad, y, leftW, kCaptionHeight };
	y += kCaptionHeight + kCaptionGap;
	l.harmonic = { kPad, y, std::max(0.0f, leftW - kModeChipWidth - kRowGap), HarmonicRow::kHeight };
	l.mode = { l.harmonic.right() + kRowGap, y + (HarmonicRow::kHeight - SwitchChip::kHeight) * 0.5f, kModeChipWidth,
		       SwitchChip::kHeight };
	y += HarmonicRow::kHeight + kRowGap;
	l.scope = { kPad, y, leftW, kScopeHeight };
	y += kScopeHeight + kRowGap;
	l.knobCell = leftW / static_cast<float>(kKnobCells);
	l.spatialCaption = { kPad + l.knobCell * static_cast<float>(kSpectralKnobs) + 8.0f, y,
		                 l.knobCell * (kKnobCells - kSpectralKnobs) - 8.0f, kCaptionHeight };
	y += kCaptionHeight + kCaptionGap;
	const float column = Knob::columnHeight(kKnobDiameter);
	l.knobRow = { kPad, y, leftW, column };
	y += column;
	const float leftBottom = y;

	// RIGHT: THE ENVELOPE — a caption, its curve, and its six values under it in two rows.
	y = bodyTop;
	l.envelopeCaption = { rightX, y, rightW, kCaptionHeight };
	y += kCaptionHeight + kCaptionGap;
	const float boxesHeight = 2.0f * ValueBox::kHeight + kBoxGap;
	const float curveHeight = std::max(60.0f, leftBottom - y - kRowGap - boxesHeight);
	l.envelope = { rightX, y, rightW, curveHeight };
	y += curveHeight + kRowGap;
	const float boxW = (rightW - kBoxGap * static_cast<float>(kEnvelopeColumns - 1)) / static_cast<float>(kEnvelopeColumns);
	for (size_t i = 0; i < l.boxes.size(); ++i) {
		const float col = static_cast<float>(i % kEnvelopeColumns);
		const float row = static_cast<float>(i / kEnvelopeColumns);
		l.boxes[i] = { rightX + col * (boxW + kBoxGap), y + row * (ValueBox::kHeight + kBoxGap), boxW, ValueBox::kHeight };
	}
	y = std::max(leftBottom, y + boxesHeight);

	// MASTER: what belongs to the device and to no one partial.
	y += kGroupGap;
	l.masterCaption = { kPad, y, inner, kCaptionHeight };
	y += kCaptionHeight + kCaptionGap;
	l.master = { kPad, y, inner, kSwitchHeight + 6.0f };
	l.porta = { kPad, y, kPortaWidth, ValueBox::kHeight };
	const float chipY = y + (ValueBox::kHeight - SwitchChip::kHeight) * 0.5f;
	l.poly = { l.porta.right() + kRowGap + 6.0f, chipY, kPolyChipWidth, SwitchChip::kHeight };
	l.depthEnv = { l.poly.right() + kRowGap, chipY, kEnvDepthChipWidth, SwitchChip::kHeight };
	y += ValueBox::kHeight;
	l.bottom = y;
	return l;
}

HarmExpanded::HarmExpanded(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
                           Notifier* notifier)
    : host_(host), device_(id) {
	(void)registry; // the chips take none, as the knobs do not

	// THE PICTURE.
	scope_ = std::make_unique<HarmScope>(host_, device_);
	addChild(scope_.get());

	// THE STRIP — four partials' number, ratio, Active and Level, on show whichever tab is open.
	strip_ = std::make_unique<HarmPartialStrip>(host_, device_, notifier);
	strip_->onSelect = [this](int partial) { selectPartial(partial); };
	addChild(strip_.get());

	// THE DETAIL, BUILT FOR ALL FOUR AND SHOWN FOR ONE: the same layout bound to four sets of parameters.
	for (int k = 0; k < hrm::kPartialCount; ++k) {
		Detail& d = detail_[static_cast<size_t>(k)];
		// THE HARMONIC ROW, THE SAME WIDGET THE CARD BUILDS. This is what closes the gap the header used
		// to record: the scope plots the notch where the harmonic puts it, and until now a hand
		// could read that and not change it.
		d.row = std::make_unique<HarmonicRow>(control(hrm::partialParam(k, hrm::PartialField::Harmonic)));
		// THE SCOPE REDRAWS WHEN THE HARMONIC MOVES, which is the whole reason this row belongs on this
		// panel: the picture and the control are the same gesture.
		d.row->onChanged = [this] {
			if (scope_ != nullptr) {
				scope_->redraw();
			}
			if (strip_ != nullptr) {
				strip_->refresh();
			}
			redraw();
		};
		addChild(d.row.get());
		for (const KnobEntry& entry : kKnobEntries) {
			const hrm::Param param = hrm::partialParam(k, entry.field);
			auto knob = std::make_unique<Knob>(control(param), entry.label, notifier);
			knob->setFaceDiameter(kKnobDiameter);
			addChild(knob.get());
			d.knobs.push_back(std::move(knob));
			// RECORDED AS THE KNOB IS BUILT, in the one place that knows which control it was built for.
			d.knobParams.push_back(param);
		}
		// NOTCH OR HIGH-PASS (backlog NL) — a chip, because it has two states and both are named on it as it flips.
		d.mode = std::make_unique<SwitchChip>(control(hrm::partialParam(k, hrm::PartialField::Mode)), "Mode");
		addChild(d.mode.get());
	}

	// THE ENVELOPE: its curve and its six values.
	envelopeView_ = std::make_unique<HarmEnvelopeView>(host_, device_);
	addChild(envelopeView_.get());
	for (const EnvelopeEntry& entry : kEnvelopeEntries) {
		auto box = std::make_unique<ValueBox>(control(entry.param), entry.label);
		addChild(box.get());
		envelopeBoxes_.push_back(std::move(box));
	}

	// MASTER.
	porta_ = std::make_unique<ValueBox>(control(hrm::Param::Porta), "Porta");
	addChild(porta_.get());
	auto poly = std::make_unique<SwitchChip>(control(hrm::Param::Poly), "Poly");
	// ALIVE OR DEAD BY THE DEVICE'S OWN DECLARATION, exactly as the card decides it — HARM declares a
	// `perVoiceControl` now, so it is alive. Asked rather than assumed, and asked BEFORE the first
	// paint, because a hand arrives through the mouse road and the mouse road does not wait for a
	// repaint.
	poly->setEnabled(hrm::kDescriptor.traits.perVoiceControl >= 0);
	addChild(poly.get());
	poly_ = std::move(poly);
	// DEPTH FOLLOWS ENV — a two-state control, so a chip that names itself, as Poly is.
	depthEnv_ = std::make_unique<SwitchChip>(control(hrm::Param::DepthFollowsEnv), "Env Depth");
	addChild(depthEnv_.get());

	selectPartial(0);
	followState();

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

void HarmExpanded::selectPartial(int partial) {
	partial = std::clamp(partial, 0, hrm::kPartialCount - 1);
	const bool changed = partial != selected_;
	selected_ = partial;
	for (int k = 0; k < hrm::kPartialCount; ++k) {
		const bool shown = k == selected_;
		Detail& d = detail_[static_cast<size_t>(k)];
		d.row->setVisible(shown);
		d.mode->setVisible(shown);
		for (auto& knob : d.knobs) {
			knob->setVisible(shown);
		}
	}
	if (strip_ != nullptr) {
		strip_->setSelected(selected_);
	}
	if (scope_ != nullptr) {
		scope_->setSelectedPartial(selected_);
	}
	if (changed) {
		redraw();
	}
}

void HarmExpanded::modulationBound(ModState* mod, EditHistory* history) {
	for (Detail& d : detail_) {
		for (auto& knob : d.knobs) {
			knob->setModulation(mod, history);
		}
	}
	// AND THE SCOPE, whose grips are modulation destinations exactly as the knobs are — the same controls
	// reached two ways. A picture whose handles could not be armed would be a set of controls that silently is not one.
	if (scope_ != nullptr) {
		scope_->setModulation(mod, history);
	}
	if (strip_ != nullptr) {
		strip_->setModulation(mod, history);
	}
	// POLY IS NOT A DESTINATION and is not offered one — the device declares it unmodulatable because
	// it decides how many of the device there are rather than what one of them does.
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
	const float minWidth = 2.0f * kPad + kLeftWidth + kColumnGap + kRightWidth;
	return { minWidth, layoutFor(minWidth).bottom + kPad };
}

visage::Bounds HarmExpanded::stripBounds() const { return layout().strip; }
visage::Bounds HarmExpanded::harmonicBounds() const { return layout().harmonic; }
visage::Bounds HarmExpanded::scopeBounds() const { return layout().scope; }
visage::Bounds HarmExpanded::modeBounds() const { return layout().mode; }
visage::Bounds HarmExpanded::knobRowBounds() const { return layout().knobRow; }
visage::Bounds HarmExpanded::envelopeBounds() const { return layout().envelope; }
visage::Bounds HarmExpanded::envelopeBoxBounds(size_t index) const {
	const Layout l = layout();
	return index < l.boxes.size() ? l.boxes[index] : visage::Bounds {};
}
visage::Bounds HarmExpanded::masterBounds() const { return layout().master; }
visage::Bounds HarmExpanded::portaBounds() const { return layout().porta; }
visage::Bounds HarmExpanded::polyBounds() const { return layout().poly; }
visage::Bounds HarmExpanded::depthEnvBounds() const { return layout().depthEnv; }

float HarmExpanded::modalBottom() const {
	const Layout l = layout();
	return l.strip.bottom();
}

std::string HarmExpanded::polyText() const {
	// THE CARD'S OWN WORDS, and the dead branch kept for the card's own reason: a working control
	// still labelled "not built yet" is the same lie as a dead one labelled "Poly", pointing the
	// other way. `perVoiceControl` is the question both surfaces ask.
	return hrm::kDescriptor.traits.perVoiceControl >= 0 ? "Poly"
	                                                    : "Poly — per-voice not built yet";
}

std::vector<float> HarmExpanded::signature() const {
	std::vector<float> out;
	out.reserve(static_cast<size_t>(hrm::Param::kCount) + 1);
	for (size_t i = 0; i < static_cast<size_t>(hrm::Param::kCount); ++i) {
		out.push_back(host_.fieldAt(device_, i));
	}
	out.push_back(static_cast<float>(selected_));
	return out;
}

void HarmExpanded::followState() {
	// EACH PARTIAL'S WIDTH AND DEPTH GREY WITH ITS OWN MODE: in High-pass they shape nothing (the brickwall has one shape),
	// so — the honesty rule — they are not offered as if they did.
	bool anyNotch = false;
	bool anyActive = false;
	for (int k = 0; k < hrm::kPartialCount; ++k) {
		const bool highpass = host_.fieldAt(device_, static_cast<size_t>(hrm::partialParam(k, hrm::PartialField::Mode))) >= 0.5f;
		const bool active = host_.fieldAt(device_, static_cast<size_t>(hrm::partialParam(k, hrm::PartialField::Active))) >= 0.5f;
		Detail& d = detail_[static_cast<size_t>(k)];
		for (size_t i = 0; i < d.knobs.size() && i < d.knobParams.size(); ++i) {
			const hrm::Param p = d.knobParams[i];
			if (p == hrm::partialParam(k, hrm::PartialField::Width) || p == hrm::partialParam(k, hrm::PartialField::Depth)) {
				d.knobs[i]->setEnabled(!highpass);
			}
		}
		if (active) {
			anyActive = true;
			anyNotch = anyNotch || !highpass;
		}
	}
	// ENV DEPTH SCALES NOTCH DEPTH, so it has something to act on only while a partial runs a notch.
	if (depthEnv_ != nullptr) {
		depthEnv_->setEnabled(!anyActive || anyNotch);
	}
}

bool HarmExpanded::updateContent() {
	// THE FIELDS, NOT A CONTROL — `HarmCard::updateContent`'s rule: a tick must not construct widgets.
	std::vector<float> now = signature();
	if (now == drawnSignature_) {
		return false;
	}
	drawnSignature_ = std::move(now);
	followState();
	if (strip_ != nullptr) {
		strip_->refresh();
	}
	if (envelopeView_ != nullptr) {
		envelopeView_->refresh();
	}
	if (scope_ != nullptr) {
		scope_->redraw();
	}
	redraw();
	return true;
}

void HarmExpanded::resized() {
	const Layout l = layout();
	strip_->setBounds(l.strip.x(), l.strip.y(), l.strip.width(), l.strip.height());
	scope_->setBounds(l.scope.x(), l.scope.y(), l.scope.width(), l.scope.height());
	for (Detail& d : detail_) {
		d.row->setBounds(l.harmonic.x(), l.harmonic.y(), l.harmonic.width(), l.harmonic.height());
		d.mode->setBounds(l.mode.x(), l.mode.y(), l.mode.width(), l.mode.height());
		for (size_t i = 0; i < d.knobs.size(); ++i) {
			d.knobs[i]->setBounds(l.knobRow.x() + l.knobCell * static_cast<float>(i), l.knobRow.y(), l.knobCell,
			                      l.knobRow.height());
		}
	}
	envelopeView_->setBounds(l.envelope.x(), l.envelope.y(), l.envelope.width(), l.envelope.height());
	for (size_t i = 0; i < envelopeBoxes_.size() && i < l.boxes.size(); ++i) {
		envelopeBoxes_[i]->setBounds(l.boxes[i].x(), l.boxes[i].y(), l.boxes[i].width(), l.boxes[i].height());
	}
	porta_->setBounds(l.porta.x(), l.porta.y(), l.porta.width(), l.porta.height());
	poly_->setBounds(l.poly.x(), l.poly.y(), l.poly.width(), l.poly.height());
	depthEnv_->setBounds(l.depthEnv.x(), l.depthEnv.y(), l.depthEnv.width(), l.depthEnv.height());

	// THE CAPTIONS, written once here and read by `draw` and by the cases.
	captions_.clear();
	captions_.push_back({ "PARTIALS", l.partialsCaption });
	captions_.push_back({ "SPECTRAL", l.spectralCaption });
	captions_.push_back({ "SPATIAL", l.spatialCaption });
	captions_.push_back({ "ENVELOPE", l.envelopeCaption });
	captions_.push_back({ "MASTER", l.masterCaption });
}

std::vector<hrm::Param> HarmExpanded::addressedParamsForTest() const {
	// EVERY CONTROL THE WINDOW OFFERS, once each: the strip's Level and Active for all four partials, each partial's ratio, five
	// knobs and Mode, the envelope's six, and the device's Porta, Poly and Env Depth. `Enabled` IS THE WINDOW HEADER'S chip
	// (`ExpandedPanel::enableBounds`), and Mix and wet Gain are the slot's furniture.
	std::vector<hrm::Param> out;
	for (int k = 0; k < hrm::kPartialCount; ++k) {
		out.push_back(hrm::partialParam(k, hrm::PartialField::Level));
		out.push_back(hrm::partialParam(k, hrm::PartialField::Active));
		out.push_back(hrm::partialParam(k, hrm::PartialField::Harmonic));
		out.push_back(hrm::partialParam(k, hrm::PartialField::Mode));
		for (const hrm::Param p : detail_[static_cast<size_t>(k)].knobParams) {
			out.push_back(p);
		}
	}
	for (const EnvelopeEntry& entry : kEnvelopeEntries) {
		out.push_back(entry.param);
	}
	out.push_back(hrm::Param::Porta);
	out.push_back(hrm::Param::Poly);
	out.push_back(hrm::Param::DepthFollowsEnv);
	return out;
}

void HarmExpanded::draw(visage::Canvas& canvas) {
	// NO GROUND OF ITS OWN. The overlay's panel is already a raised surface and a second rectangle on
	// top of it would be a card inside a card. The knobs, the row and the scope draw themselves.


	// WHERE SPECTRAL'S KNOBS END AND SPATIAL'S BEGIN: Fine, Width and Depth set the pitch and the shape of the notch (spectral);
	// only Phase and Spread are about the stereo image. A hairline between the two cells says so.
	{
		const Layout lay = layout();
		const float x = lay.knobRow.x() + lay.knobCell * static_cast<float>(kSpectralKnobs);
		canvas.setColor(withAlpha(kLine, 0.55f));
		canvas.fill(x, lay.spatialCaption.y(), 1.0f, lay.knobRow.bottom() - lay.spatialCaption.y());
	}

	// THE GROUP CAPTIONS. READ FROM `captions_` RATHER THAN RECOMPUTED, so the caption is written where `resized`
	// put it. Two places computing one geometry is the seam this lane has produced three times in a week.
	for (const Caption& caption : captions_) {
		canvas.setColor(withAlpha(kMuted, 0.75f));
		drawTracked(canvas, caption.text, font(kKnobLabelSize), kKnobLabelTracking, caption.bounds.x(),
		            caption.bounds.y(), caption.bounds.height());
		// A HAIRLINE ACROSS THE REST OF THE ROW, so the caption reads as a section head rather than
		// as a stray word above a knob.
		const float textWidth = trackedWidth(font(kKnobLabelSize), caption.text, kKnobLabelTracking);
		const float ruleLeft = caption.bounds.x() + textWidth + 8.0f;
		if (caption.bounds.right() > ruleLeft) {
			canvas.setColor(withAlpha(kLine, 0.55f));
			canvas.fill(ruleLeft, caption.bounds.y() + caption.bounds.height() * 0.5f, caption.bounds.right() - ruleLeft, 1.0f);
		}
	}
}

} // namespace feathers::ui
