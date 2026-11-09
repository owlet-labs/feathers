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

#include "plugin/ui/eq_controls.h"

#include <algorithm>
#include <cmath>

namespace feathers::ui {

namespace eq = fx::eq;

namespace {

// Not a param (it has no key in the params' ledger); the `ValueBox` needs a declaration to print from and
// to build its menu from.
constexpr device::ParamDesc kChannelDesc { .key = "channel", .name = "Channel", .min = 0.0,
	                                       .max = static_cast<double>(eq::Channel::kCount) - 1.0, .def = 0.0,
	                                       .stepped = true, .unit = device::Unit::Choice, .names = eq::kChannelNames,
	                                       .tooltip = "Where the band acts: on left and right alike, on the mid "
	                                                  "(what the two sides share) or on the side (how they differ)" };

} // namespace

EqChannelControl::EqChannelControl(backplane::DeviceHost& host, DeviceInstanceId id, int band)
    : host_(host), device_(id), field_(eq::extraIndex(eq::channelField(band))) {}

const device::ParamDesc& EqChannelControl::desc() const {
	return kChannelDesc;
}

double EqChannelControl::value() const {
	const float stored = field_ < host_.fieldsPresentAt(device_) ? host_.fieldAt(device_, field_) : 0.0f;
	return static_cast<double>(eq::channelOf(stored));
}

void EqChannelControl::setValue(double value) {
	host_.setField(device_, field_, static_cast<float>(eq::channelOf(static_cast<float>(value))));
}

void EqChannelControl::beginGesture() {
	valueBefore_ = value();
}

void EqChannelControl::endGesture() {
	const double after = value();
	if (history_ != nullptr && after != valueBefore_) {
		history_->record(std::make_unique<DeviceValueEdit>(host_, device_, field_, valueBefore_, after));
	}
}

std::string EqChannelControl::text() const {
	return eq::kChannelNames[static_cast<size_t>(value())];
}

EqControls::EqControls(visage::Frame& parent, backplane::DeviceHost& host, DeviceInstanceId id, ControlFn control,
                       Notifier* notifier, float knobDiameter, bool withPivotBox)
    : host_(host), device_(id), control_(std::move(control)), diameter_(knobDiameter) {
	const auto knob = [&](size_t param, const char* label) {
		auto k = std::make_unique<Knob>(control_(param), label, notifier);
		k->setFaceDiameter(diameter_);
		parent.addChild(k.get());
		return k;
	};
	const auto box = [&](size_t param, const char* label) {
		auto b = std::make_unique<ValueBox>(control_(param), label);
		b->setCompact(true);
		parent.addChild(b.get());
		return b;
	};
	for (int b = 0; b < eq::kBandCount; ++b) {
		BandWidgets& w = bands_[static_cast<size_t>(b)];
		w.on = std::make_unique<SwitchChip>(control_(eq::bandIndex(b, eq::BandField::On)), "On");
		parent.addChild(w.on.get());
		w.type = box(eq::bandIndex(b, eq::BandField::Type), "Type");
		w.slope = box(eq::bandIndex(b, eq::BandField::Slope), "Slope");
		w.freq = knob(eq::bandIndex(b, eq::BandField::Freq), "Freq");
		w.gain = knob(eq::bandIndex(b, eq::BandField::Gain), "Gain");
		w.q = knob(eq::bandIndex(b, eq::BandField::Q), "Q");
		w.channelControl = std::make_unique<EqChannelControl>(host_, device_, b);
		// NO LABEL: the value ("Stereo", "Mid", "Side") names itself, and the strip has no room for both.
		w.channel = std::make_unique<ValueBox>(*w.channelControl, "");
		w.channel->setCompact(true);
		parent.addChild(w.channel.get());
	}
	offset_ = knob(eq::paramIndex(eq::Param::Offset), "Offset");
	amount_ = knob(eq::paramIndex(eq::Param::Amount), "Amount");
	tilt_ = knob(eq::paramIndex(eq::Param::Tilt), "Tilt");
	output_ = knob(eq::paramIndex(eq::Param::Output), "Output");
	// PHASE: two states, so a SwitchChip — lit is Linear. PANEL ONLY, like the pivot box: a fifth cell in the
	// card's globals row cut every knob's caption short (photographed), so the card names Linear on its plot.
	if (withPivotBox) {
		phase_ = std::make_unique<SwitchChip>(control_(eq::paramIndex(eq::Param::Phase)), "Linear");
		parent.addChild(phase_.get());
		// KEY TRACK (backlog MQ): two states, a SwitchChip. Panel only, for Phase's reason; the card names it on its plot.
		keyTrack_ = std::make_unique<SwitchChip>(control_(eq::paramIndex(eq::Param::KeyTrack)), "Key");
		parent.addChild(keyTrack_.get());
	}
	if (withPivotBox) {
		pivot_ = box(eq::paramIndex(eq::Param::TiltPivot), "Pivot");
	}
}

float EqControls::field(size_t i) const {
	if (i < host_.fieldsPresentAt(device_)) {
		return host_.fieldAt(device_, i);
	}
	return static_cast<float>(eq::kParamDescs[i].def);
}

bool EqControls::anyBandActing() const {
	for (int b = 0; b < eq::kBandCount; ++b) {
		if (field(eq::bandIndex(b, eq::BandField::On)) < 0.5f) {
			continue;
		}
		const auto type = static_cast<eq::BandType>(std::lround(field(eq::bandIndex(b, eq::BandField::Type))));
		if (!eq::hasGain(type) || field(eq::bandIndex(b, eq::BandField::Gain)) != 0.0f) {
			return true;
		}
	}
	return false;
}

bool EqControls::anyGainActing() const {
	if (field(eq::paramIndex(eq::Param::Tilt)) != 0.0f) {
		return true;
	}
	for (int b = 0; b < eq::kBandCount; ++b) {
		const auto type = static_cast<eq::BandType>(std::lround(field(eq::bandIndex(b, eq::BandField::Type))));
		if (field(eq::bandIndex(b, eq::BandField::On)) >= 0.5f && eq::hasGain(type)
		    && field(eq::bandIndex(b, eq::BandField::Gain)) != 0.0f) {
			return true;
		}
	}
	return false;
}

std::vector<int> EqControls::honestyKey(int selected) const {
	const int b = std::clamp(selected, 0, eq::kBandCount - 1);
	return { b,
		     field(eq::bandIndex(b, eq::BandField::On)) >= 0.5f ? 1 : 0,
		     static_cast<int>(std::lround(field(eq::bandIndex(b, eq::BandField::Type)))),
		     static_cast<int>(std::lround(field(eq::bandIndex(b, eq::BandField::Slope)))),
		     field(eq::paramIndex(eq::Param::Tilt)) != 0.0f ? 1 : 0,
		     anyBandActing() ? 1 : 0,
		     anyGainActing() ? 1 : 0 };
}

void EqControls::layOut(visage::Bounds strip, visage::Bounds globals, int selected, bool open) {
	const int sel = std::clamp(selected, 0, eq::kBandCount - 1);
	offCaption_ = {};
	for (int b = 0; b < eq::kBandCount; ++b) {
		BandWidgets& w = bands_[static_cast<size_t>(b)];
		for (visage::Frame* f : { static_cast<visage::Frame*>(w.on.get()), static_cast<visage::Frame*>(w.type.get()),
		                          static_cast<visage::Frame*>(w.slope.get()), static_cast<visage::Frame*>(w.freq.get()),
		                          static_cast<visage::Frame*>(w.gain.get()), static_cast<visage::Frame*>(w.q.get()),
		                          static_cast<visage::Frame*>(w.channel.get()) }) {
			f->setVisible(false);
		}
	}
	const float x = strip.x();
	const float y = strip.y();
	BandWidgets& w = bands_[static_cast<size_t>(sel)];
	const bool on = field(eq::bandIndex(sel, eq::BandField::On)) >= 0.5f;
	const auto type = static_cast<eq::BandType>(std::lround(field(eq::bandIndex(sel, eq::BandField::Type))));
	const bool cut = type == eq::BandType::LowCut || type == eq::BandType::HighCut;
	const float chipW = 40.0f;
	w.on->setBounds(x, y, chipW, kSwitchRow);
	w.on->setVisible(open);
	if (!on) {
		// OFF: nothing else about this band changes the sound, so nothing else is offered.
		offCaption_ = { x + chipW + kGap, y, std::max(0.0f, strip.width() - chipW - kGap), kSwitchRow };
	}
	else {
		// On | Type | Slope (a cut only), and the Channel box: at the switch row's end, or — for a cut, whose
		// Type and Slope need the whole row — in the knob row's third cell, which a cut leaves empty (no Gain).
		const float rest = strip.width() - chipW - kGap - (cut ? 0.0f : kChannelWidth + kGap);
		const float boxW = cut ? (rest - kGap) * 0.5f : rest;
		const float boxY = y + (kSwitchRow - ValueBox::kCompactHeight) * 0.5f;
		w.type->setBounds(x + chipW + kGap, boxY, boxW, ValueBox::kCompactHeight);
		w.type->setVisible(open);
		if (cut) {
			w.slope->setBounds(x + chipW + kGap + boxW + kGap, boxY, boxW, ValueBox::kCompactHeight);
			w.slope->setVisible(open);
		}
		// The knob row: Freq, Gain (gain types only), Q — closed up like COMP's gated knobs.
		std::vector<Knob*> row { w.freq.get() };
		if (eq::hasGain(type)) {
			row.push_back(w.gain.get());
		}
		row.push_back(w.q.get());
		const float cell = strip.width() / 3.0f;
		const float ky = y + kSwitchRow + kGap;
		for (size_t i = 0; i < row.size(); ++i) {
			row[i]->setBounds(x + cell * static_cast<float>(i), ky, cell, rowHeight());
			row[i]->setVisible(open);
		}
		if (cut) {
			const float cw = std::min(kChannelWidth, cell);
			w.channel->setBounds(x + 2.0f * cell + (cell - cw) * 0.5f, ky + (rowHeight() - ValueBox::kCompactHeight) * 0.5f,
			                     cw, ValueBox::kCompactHeight);
		}
		else {
			w.channel->setBounds(strip.right() - kChannelWidth, boxY, kChannelWidth, ValueBox::kCompactHeight);
		}
		w.channel->setVisible(open);
		// A 6 dB cut is one first-order section: its Q changes nothing.
		const bool sixDb = cut && std::lround(field(eq::bandIndex(sel, eq::BandField::Slope))) == 0;
		w.q->setActive(!sixDb);
	}

	// THE GLOBALS ROW.
	std::vector<visage::Frame*> globalsRow { offset_.get(), amount_.get(), tilt_.get() };
	const bool tilted = field(eq::paramIndex(eq::Param::Tilt)) != 0.0f;
	if (pivot_ != nullptr) {
		globalsRow.push_back(pivot_.get());
	}
	globalsRow.push_back(output_.get());
	if (phase_ != nullptr) {
		globalsRow.push_back(phase_.get());
	}
	if (keyTrack_ != nullptr) {
		globalsRow.push_back(keyTrack_.get());
	}
	const float cell = globals.width() / static_cast<float>(globalsRow.size());
	for (size_t i = 0; i < globalsRow.size(); ++i) {
		visage::Frame* f = globalsRow[i];
		const float gx = globals.x() + cell * static_cast<float>(i);
		if (f == pivot_.get()) {
			f->setBounds(gx + 2.0f, globals.y() + (rowHeight() - ValueBox::kCompactHeight) * 0.5f, cell - 4.0f,
			             ValueBox::kCompactHeight);
			f->setVisible(open && tilted);
		}
		else if ((phase_ != nullptr && f == phase_.get()) || (keyTrack_ != nullptr && f == keyTrack_.get())) {
			const float w = std::min(cell - 4.0f, 48.0f);
			f->setBounds(gx + (cell - w) * 0.5f, globals.y() + (rowHeight() - kSwitchRow) * 0.5f, w, kSwitchRow);
			f->setVisible(open);
		}
		else {
			f->setBounds(gx, globals.y(), cell, rowHeight());
			f->setVisible(open);
		}
	}
	offset_->setActive(anyBandActing());
	amount_->setActive(anyGainActing());
}

void EqControls::setModulation(ModState* mod, EditHistory* history) {
	for (BandWidgets& w : bands_) {
		w.freq->setModulation(mod, history);
		w.gain->setModulation(mod, history);
		w.q->setModulation(mod, history);
		w.channelControl->setHistory(history);
	}
	for (Knob* k : { offset_.get(), amount_.get(), tilt_.get(), output_.get() }) {
		k->setModulation(mod, history);
	}
}

std::vector<size_t> EqControls::addressedParams() const {
	std::vector<size_t> out;
	for (int b = 0; b < eq::kBandCount; ++b) {
		for (int f = 0; f < static_cast<int>(eq::BandField::kCount); ++f) {
			out.push_back(eq::bandIndex(b, static_cast<eq::BandField>(f)));
		}
	}
	for (const eq::Param p : { eq::Param::Offset, eq::Param::Amount, eq::Param::Tilt, eq::Param::Output }) {
		out.push_back(eq::paramIndex(p));
	}
	if (pivot_ != nullptr) {
		out.push_back(eq::paramIndex(eq::Param::TiltPivot));
	}
	if (phase_ != nullptr) {
		out.push_back(eq::paramIndex(eq::Param::Phase));
	}
	if (keyTrack_ != nullptr) {
		out.push_back(eq::paramIndex(eq::Param::KeyTrack));
	}
	return out;
}

Knob* EqControls::knobForTest(size_t param) const {
	for (Knob* k : { offset_.get(), amount_.get(), tilt_.get(), output_.get() }) {
		if (k->control() == &control_(param)) {
			return k;
		}
	}
	for (const BandWidgets& w : bands_) {
		for (Knob* k : { w.freq.get(), w.gain.get(), w.q.get() }) {
			if (k->control() == &control_(param)) {
				return k;
			}
		}
	}
	return nullptr;
}

ValueBox* EqControls::boxForTest(size_t param) const {
	if (pivot_ != nullptr && param == eq::paramIndex(eq::Param::TiltPivot)) {
		return pivot_.get();
	}
	if (param < eq::kGlobalCount) {
		return nullptr;
	}
	const size_t band = (param - eq::kGlobalCount) / eq::kBandFields;
	const auto f = static_cast<eq::BandField>((param - eq::kGlobalCount) % eq::kBandFields);
	const BandWidgets& w = bands_[band];
	return f == eq::BandField::Type ? w.type.get() : f == eq::BandField::Slope ? w.slope.get() : nullptr;
}

SwitchChip* EqControls::chipForTest(size_t param) const {
	if (param == eq::paramIndex(eq::Param::Phase)) {
		return phase_.get();
	}
	if (param == eq::paramIndex(eq::Param::KeyTrack)) {
		return keyTrack_.get();
	}
	if (param < eq::kGlobalCount) {
		return nullptr;
	}
	const size_t band = (param - eq::kGlobalCount) / eq::kBandFields;
	const auto f = static_cast<eq::BandField>((param - eq::kGlobalCount) % eq::kBandFields);
	return f == eq::BandField::On ? bands_[band].on.get() : nullptr;
}

} // namespace feathers::ui
