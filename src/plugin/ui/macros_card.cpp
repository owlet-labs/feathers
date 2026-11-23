/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
 * even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not,
 * see <https://www.gnu.org/licenses/>.
 */

#include "plugin/ui/macros_card.h"

#include "plugin/ui/stored_edit.h"
#include "plugin/ui/theme.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace feathers::ui {

namespace {

namespace mac = feathers::mod::macros;

/// A STORED `Type n` AS THE TYPE IT NAMES — rounded and clamped, so a value between two steps (an old
/// patch, a host that wrote one) reads as the nearest type rather than as nothing.
[[nodiscard]] device::MacroType typeOf(double stored) {
	const long rounded = std::lround(stored);
	const long last = static_cast<long>(device::kAbsoluteMacroTypeCount) - 1; // no relative road here (backlog PQ)
	return static_cast<device::MacroType>(std::clamp(rounded, 0L, last));
}

} // namespace

std::string MacrosCard::shortCaption(size_t n) {
	// **"M1", NOT "Macro 1".** The card's head already says MACROS, and four columns of a 216px card are
	// 49.5px each in Feathers' strip (narrower in the rack, whose head is a spine on the left): MEASURED,
	// "Macro 1" in that column draws as "MACR..." (`Knob::columnCaption`; the first MACROS case pins it).
	// "M1" is the form the Feathers header row and the chain card print for a macro in a narrow column,
	// so a hand reads it the same way wherever a macro is. A slot whose output has a label prints the
	// label instead — cut the same way when it is long, and whole in the field that edits it.
	return "M" + std::to_string(n + 1);
}

std::string MacrosCard::captionOf(size_t n) const {
	return shape_.columns == 1 ? "Macro " + std::to_string(n + 1) : shortCaption(n);
}

MacrosCard::MacrosCard(backplane::DeviceHost& host, DeviceInstanceId id, Notifier* notifier, ModState* mod,
                       EditHistory* history, const MacrosShape& shape)
    : host_(host), device_(id), notifier_(notifier), mod_(mod), history_(history), shape_(shape),
      controls_(shape.descriptor->params.size()), views_(shape.count), slots_(shape.count),
      drawnValue_(shape.count, -1.0f), drawnPolarity_(shape.count, -1.0f) {
	for (size_t n = 0; n < shape_.count; ++n) {
		// THE POLARITY IS ASKED OF `Bipolar n` ON EVERY READ, never copied: an undo or a load moves it.
		views_[n] = std::make_unique<BipolarMacro>(control(mac::macroField(n)), [this, n] {
			return control(mac::bipolarField(shape_.count, n)).value() >= 0.5;
		});
		slots_[n] = std::make_unique<MacroSlot>(*views_[n], captionOf(n), namingFor(n), hooksFor(n), notifier_);
		slots_[n]->setFaceDiameter(shape_.faceDiameter);
		// **A DESTINATION, UNLIKE A HEADER MACRO** — the knob takes a route while a source is armed and
		// draws the ring of whatever rides `Macro n`.
		slots_[n]->setModulation(mod_, history_);
		addChild(slots_[n].get());
	}
	// **A NAME HEARD FROM THE HOST RELABELS ITS SLOT** — the slot's own field (harmless: its caption is
	// already right) and each label a patch load moved.
	labelSubscription_ = host_.subscribeOutputLabel([this](DeviceInstanceId device, size_t output) {
		if (device == device_ && output < shape_.count && slots_[output] != nullptr) {
			slots_[output]->refresh();
		}
	});

	onMouseDown() = [this](const visage::MouseEvent& e) {
		// THE PRESS IS THE CARD'S TO HAND BACK — `LfoCard`'s note: the head's marks, the fold and the
		// lane's key target all ride `noteCardPress`.
		noteCardPress(e);
		// **THE SLOT MENU, WHEN A HOST BINDS ONE** — Feathers' fx lane, which holds modulators since 2026-10-04, and a
		// chain's lane (`onSlotMenu`). Every audio card makes this call after `noteCardPress`; the modulator cards did
		// not, so in a lane their dots were drawn and dead (the head-verb sweep found all five). The strip binds no
		// `onSlotMenu` — it opens its menu from the head mark — so there this is a no-op.
		if (slotMenuAt(e.position)) {
			pressSlotMenu();
			return;
		}
		(void)enablePressAt(e.position);
	};
}

MacrosCard::~MacrosCard() {
	// **NOTHING THAT OUTLIVES THE CARD REACHES IT.** The token first, so a menu still open on the root
	// frame — it holds a copy of the hooks — finds no card behind its picks, and a rename committed late
	// writes no label. Then the subscription, which captures `this` and would refresh dying slots. The
	// members then die in reverse order: slots before views before controls.
	alive_.reset();
	labelSubscription_.reset();
}

DeviceControl& MacrosCard::control(size_t index) const {
	if (controls_[index] == nullptr) {
		controls_[index] = std::make_unique<DeviceControl>(host_, device_, *shape_.descriptor, index, mod_, history_);
	}
	return *controls_[index];
}

MacroMenuHooks MacrosCard::hooksFor(size_t n) {
	// **THE DEVICE'S OWN PARAMS, WRITTEN AS GESTURES** — begin, set, end — so a pick in the menu is one
	// undo step and reaches the host's automation the way a hand's edit does. A reader after the card
	// died answers the default; a writer does nothing (`alive_`).
	const std::weak_ptr<void> alive = alive_;
	return {
		.readBipolar = [this, n, alive] {
			return !alive.expired() && control(mac::bipolarField(shape_.count, n)).value() >= 0.5;
		},
		.writeBipolar =
		    [this, n, alive](bool bipolar) {
			    if (alive.expired()) {
				    return;
			    }
			    writeAsGesture(mac::bipolarField(shape_.count, n), bipolar ? 1.0 : 0.0);
			    // THE KNOB READS THROUGH THE NEW POLARITY — its arc, its readout and its rest move now.
			    slots_[n]->knob().redraw();
			    slots_[n]->chip().redraw();
		    },
		.readType =
		    [this, n, alive] {
			    return alive.expired() ? device::MacroType::Knob : typeOf(control(mac::typeField(shape_.count, n)).value());
		    },
		.writeType =
		    [this, n, alive](device::MacroType type) {
			    if (alive.expired()) {
				    return;
			    }
			    writeAsGesture(mac::typeField(shape_.count, n), static_cast<double>(static_cast<int>(type)));
		    },
		// THE SLOT'S OWN: `MacroSlot` answers Rename with its field, through its own weak handle.
		.beginRename = [] {},
	};
}

MacroSlot::Naming MacrosCard::namingFor(size_t n) {
	// **OUTPUT n's LABEL ON THIS INSTANCE** — the host's (`DeviceHost::outputLabel`), so the name typed
	// here is the one every route list prints ("Macros 2 · Wobble"). The token for the hooks' reason.
	// NOTHING THE RENAME REACHES DESTROYS THIS CARD: the hosts re-lay their output chips on a relabel and
	// rebuild no card for one (`MacroSlot::Naming::rename`'s rule).
	const std::weak_ptr<void> alive = alive_;
	return {
		.name = [this, n, alive] {
			return alive.expired() ? device::defaultMacroName(n) : host_.outputLabel(device_, n);
		},
		.renamed = [this, n, alive] { return !alive.expired() && host_.outputRelabelled(device_, n); },
		.rename =
		    [this, n, alive](const std::string& name) {
			    if (alive.expired()) {
				    return;
			    }
			    // ONE UNDO STEP (backlog JB), written back through the host's verb by device id and output — the
			    // label as the host reads it, which the verb normalises the same way going back.
			    backplane::DeviceHost& host = host_;
			    const DeviceInstanceId device = device_;
			    (void)writeStoredAsEdit<std::string>(
			        history_, [&host, device, n] { return host.outputLabel(device, n); },
			        [&host, device, n](const std::string& label) { host.setOutputLabel(device, n, label); }, name,
			        "Rename");
		    },
	};
}

void MacrosCard::writeAsGesture(size_t index, double value) {
	DeviceControl& c = control(index);
	c.beginGesture();
	c.setValue(value);
	c.endGesture();
}

device::Modulatable* MacrosCard::enableModulatable() {
	return &control(mac::enableField(shape_.count));
}

void MacrosCard::setModulation(ModState* mod, EditHistory* history) {
	mod_ = mod;
	history_ = history;
	for (size_t i = 0; i < controls_.size(); ++i) {
		control(i).setModulation(mod, history);
	}
	for (const std::unique_ptr<MacroSlot>& slot : slots_) {
		slot->setModulation(mod, history);
	}
}

std::vector<mac::Param> MacrosCard::addressedParamsForTest() const {
	std::vector<mac::Param> out;
	for (size_t n = 0; n < shape_.count; ++n) {
		out.push_back(static_cast<mac::Param>(mac::macroField(n)));
		out.push_back(static_cast<mac::Param>(mac::bipolarField(shape_.count, n)));
		out.push_back(static_cast<mac::Param>(mac::typeField(shape_.count, n)));
	}
	return out;
}

float MacrosCard::openHeight(size_t outputRows, const MacrosShape& shape) {
	return kCardHeadHeight + bodyHeight(shape) + kCardOutputStripHeight * static_cast<float>(outputRows);
}

float MacrosCard::laneHeightAt(float cardWidth) const {
	return folded() ? kCardHeadHeight : kCardHeadHeight + bodyHeight(shape_) + outputStripHeightAt(cardWidth);
}

visage::Bounds MacrosCard::slotBounds(size_t n) const {
	// FOUR EQUAL COLUMNS ACROSS THE BODY, under a top head or beside a left spine alike (`bodyLeft`,
	// `bodyTopEdge`), each as tall as a knob column at the header row's face.
	const size_t col = n % shape_.columns;
	const size_t row = n / shape_.columns;
	const float rowHeight = MacroSlot::columnHeight(shape_.faceDiameter);
	const float top = bodyTopEdge() + kGap + static_cast<float>(row) * (rowHeight + kGap);
	if (shape_.columns > 1) {
		// **A KNOB'S COLUMN IS ITS OUTPUT CHIP'S CELL** (`layoutOutputGrid`, the grid road the device's `outputGridColumns` asks for): the same
		// equal cells with the same padding and gap, and the slot is the cell widened by half a gap each side, so a knob and the chip under it
		// share one centre and the chips read as the knobs' own captions.
		const float usable = std::max(0.0f, bodyWidth() - 2.0f * kCardOutputChipPadX);
		const float cols = static_cast<float>(shape_.columns);
		const float cell = std::max(0.0f, (usable - (cols - 1.0f) * kCardOutputChipGap) / cols);
		const float left = bodyLeft() + kCardOutputChipPadX + static_cast<float>(col) * (cell + kCardOutputChipGap);
		return { left - 0.5f * kCardOutputChipGap, top, cell + kCardOutputChipGap, rowHeight };
	}
	const float cell = std::max(0.0f, bodyWidth() - 2.0f * kPadX) / static_cast<float>(shape_.columns);
	return { bodyLeft() + kPadX + cell * static_cast<float>(col), top, cell, rowHeight };
}

void MacrosCard::resized() {
	for (size_t n = 0; n < shape_.count; ++n) {
		const visage::Bounds b = slotBounds(n);
		slots_[n]->setBounds(b.x(), b.y(), b.width(), b.height());
	}
}

void MacrosCard::foldedChanged() {
	for (const std::unique_ptr<MacroSlot>& slot : slots_) {
		slot->setVisible(!folded());
	}
}

bool MacrosCard::updateContent() {
	// **POLLED, BECAUSE NOTHING TELLS A CARD ITS PARAMS MOVED** — an undo, a patch load, host automation
	// (`GenericCard::updateContent`'s note). Eight floats and four type reads a frame; a slot repaints
	// only when one of its own moved. `syncType` is the slot's cheap half of `refresh` for exactly this.
	bool told = false;
	for (size_t n = 0; n < shape_.count; ++n) {
		told = slots_[n]->syncType() || told;
		const auto value = static_cast<float>(control(mac::macroField(n)).value());
		const auto polarity = static_cast<float>(control(mac::bipolarField(shape_.count, n)).value());
		if (value != drawnValue_[n] || polarity != drawnPolarity_[n]) {
			drawnValue_[n] = value;
			drawnPolarity_[n] = polarity;
			slots_[n]->knob().redraw();
			slots_[n]->chip().redraw();
			told = true;
		}
	}
	return told;
}

bool MacrosCard::headSourceFocused() const {
	if (mod_ == nullptr) {
		return false;
	}
	for (size_t out = 0; out < shape_.count; ++out) {
		if (mod_->source() == backplane::Source::output(device_, out)) {
			return true;
		}
	}
	return false;
}

bool MacrosCard::headArmed() const {
	return headSourceFocused() && mod_ != nullptr && mod_->armed();
}

void MacrosCard::offeredControls(std::vector<Offered>& out) const {
	FxCard::offeredControls(out);
	// EACH MACRO'S TYPE AND POLARITY, chosen from the slot's menu (`writeAsGesture`): every choice shown before it applies.
	for (size_t n = 0; n < shape_.count; ++n) {
		out.push_back({ &control(mac::typeField(shape_.count, n)), Road::Face });
		out.push_back({ &control(mac::bipolarField(shape_.count, n)), Road::Face });
	}
}

} // namespace feathers::ui
