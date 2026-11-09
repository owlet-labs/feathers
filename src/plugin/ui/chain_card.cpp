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

#include "plugin/ui/chain_card.h"

#include "backplane/inner_routes.h"
#include "plugin/device_registry.h"
#include "plugin/ui/theme.h"
#include "rack/rack_state.h"

#include <clap/stream.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <span>

namespace feathers::ui {

namespace {

namespace cmp = feathers::composite;

/// THE TEN KNOBS, IN CELL ORDER: the eight macros, then Mix and Gain.
constexpr std::array<cmp::Param, ChainCard::kKnobCount> kKnobs {
	cmp::Param::Macro1, cmp::Param::Macro2, cmp::Param::Macro3, cmp::Param::Macro4,
	cmp::Param::Macro5, cmp::Param::Macro6, cmp::Param::Macro7, cmp::Param::Macro8,
	cmp::Param::Mix,    cmp::Param::Gain,
};
constexpr std::array<const char*, ChainCard::kKnobCount> kCaptions {
	"M1", "M2", "M3", "M4", "M5", "M6", "M7", "M8", "Mix", "Gain",
};

/// A `clap_istream` OVER THE BYTES THE HOST HOLDS, so `rack::parseRack` can read a body without a file
/// and without the running instance. The same shape `rack_device.cpp` reads its own body through.
struct SpanSource {
	std::span<const uint8_t> bytes;
	size_t at = 0;
	static int64_t CLAP_ABI read(const clap_istream* stream, void* buffer, uint64_t size) {
		auto* self = static_cast<SpanSource*>(stream->ctx);
		const size_t left = self->bytes.size() - self->at;
		const size_t n = static_cast<size_t>(std::min<uint64_t>(size, left));
		if (n > 0) {
			std::memcpy(buffer, self->bytes.data() + self->at, n);
			self->at += n;
		}
		return static_cast<int64_t>(n);
	}
};

} // namespace

const device::Descriptor& ChainCard::rackDescriptor() {
	const device::Descriptor* d = deviceForTag(cmp::kTraits.tag);
	return d != nullptr ? *d : kDevices[0];
}

ChainCard::ChainCard(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
                     Notifier* notifier)
    : host_(host), device_(id), notifier_(notifier) {
	(void)registry;
	controls_.resize(static_cast<size_t>(cmp::Param::kCount));
	for (size_t i = 0; i < macroCaption_.size(); ++i) {
		macroCaption_[i] = kCaptions[i];
	}
	for (size_t i = 0; i < kKnobs.size(); ++i) {
		const bool isMacro = i < macroCaption_.size();
		auto knob = std::make_unique<Knob>(control(kKnobs[i]), isMacro ? macroCaption_[i].c_str() : kCaptions[i],
		                                   notifier_);
		knob->setFaceDiameter(kKnobDiameter);
		// A MACRO'S CAPTION IS A NAME (backlog OT): as typed, shrunk to fit.
		knob->setCaptionIsName(isMacro);
		addChild(knob.get());
		knobs_.push_back(std::move(knob));
	}
	(void)refreshSummary();
	(void)refreshMacroNames();

	onMouseDown() = [this](const visage::MouseEvent& e) {
		noteCardPress(e);
		if (slotMenuAt(e.position)) {
			pressSlotMenu();
		}
		else if (const auto nowOn = enablePressAt(e.position)) {
			if (notifier_ != nullptr) {
				notifier_->say(*nowOn ? "Chain on" : "Chain off");
			}
		}
	};
}

DeviceControl& ChainCard::control(cmp::Param p) const {
	const size_t i = static_cast<size_t>(p);
	if (controls_[i] == nullptr) {
		controls_[i] = std::make_unique<DeviceControl>(host_, device_, rackDescriptor(), i, mod_, history_);
	}
	return *controls_[i];
}

device::Modulatable* ChainCard::enableModulatable() {
	return &control(cmp::Param::Enabled);
}

bool ChainCard::headEnabled() const {
	return control(cmp::Param::Enabled).value() >= 0.5;
}

void ChainCard::setModulation(ModState* mod, EditHistory* history) {
	mod_ = mod;
	history_ = history;
	for (auto& knob : knobs_) {
		knob->setModulation(mod, history);
	}
	for (size_t i = 0; i < static_cast<size_t>(cmp::Param::kCount); ++i) {
		control(static_cast<cmp::Param>(i)).setModulation(mod, history);
	}
}

std::vector<cmp::Param> ChainCard::addressedParamsForTest() const {
	return std::vector<cmp::Param>(kKnobs.begin(), kKnobs.end());
}

float ChainCard::openHeight() {
	const float rows = static_cast<float>((kKnobCount + kKnobsPerRow - 1) / kKnobsPerRow);
	return kSlotHeadHeight + kRowGap + rows * (Knob::kHeight + kRowGap) + kPad;
}

float ChainCard::laneHeightAt(float) const {
	return folded() ? foldedHeight() : openHeight();
}

float ChainCard::laneExtent(float cross) const {
	if (folded()) {
		return kCardHeadHeight;
	}
	return FxCard::laneExtent(cross);
}

visage::Bounds ChainCard::headBounds() const {
	return ownFrame().head;
}

visage::Bounds ChainCard::cellBounds(size_t index) const {
	const float cell = (bodyWidth() - 2.0f * kPad) / static_cast<float>(kKnobsPerRow);
	const size_t row = index / kKnobsPerRow;
	const size_t col = index % kKnobsPerRow;
	return { bodyLeft() + kPad + cell * static_cast<float>(col),
		     kSlotHeadHeight + kRowGap + static_cast<float>(row) * (Knob::kHeight + kRowGap), cell,
		     Knob::kHeight };
}

void ChainCard::layOut() {
	for (size_t i = 0; i < knobs_.size(); ++i) {
		const visage::Bounds where = cellBounds(i);
		knobs_[i]->setBounds(where.x(), where.y(), where.width(), where.height());
	}
}

void ChainCard::resized() {
	layOut();
}

void ChainCard::foldedChanged() {
	for (auto& knob : knobs_) {
		knob->setVisible(!folded());
	}
}

bool ChainCard::refreshSummary() {
	// RE-READ ONLY WHEN THE HOST STORED A NEW BODY. The generation is the host's own count of stores; a
	// body that has never been stored reads 0, and a card that has read once at 0 does not read again.
	const uint64_t generation = host_.containerBodyGenerationOf(device_);
	if (summaryRead_ && generation == summaryGeneration_) {
		return false;
	}
	summaryRead_ = true;
	summaryGeneration_ = generation;

	std::string next;
	size_t held = 0;
	const std::span<const uint8_t> body = host_.containerBodyOf(device_);
	if (!body.empty()) {
		SpanSource source { body, 0 };
		const clap_istream stream { &source, &SpanSource::read };
		rack::RackPatch patch;
		if (rack::parseRack(&stream, &patch)) {
			// THE NAME A DEVICE DECLARES, by its tag — through the registry, which is where a tag becomes a
			// device for every host. An unknown tag is shown as its four letters rather than dropped, so a
			// body written by a build with a device this one lacks still says what it holds.
			for (const rack::RackPatch::Device& d : patch.device) {
				const device::Descriptor* desc = deviceForTag(d.tag.data());
				if (!next.empty()) {
					next += " \xE2\x80\xBA "; // ›
				}
				// **THE WHOLE TAG, NOT FOUR BYTES OF IT.** This is the label for a device THIS BUILD
				// CANNOT RESOLVE, so the tag is all there is to show — and truncating it to four
				// showed "SCAT" for SCATTER (now SKLITTER) once `device::kTagBytes` widened. The patch's tag is
				// NUL-terminated (`rack::patchTagOf`), so the plain constructor is the right one.
				next += desc != nullptr && desc->traits.name != nullptr ? desc->traits.name
				                                                       : std::string(d.tag.data());
				++held;
			}
		}
	}
	if (held == 0) {
		next = "empty";
	}
	const bool changed = next != summary_ || held != heldCount_;
	summary_ = std::move(next);
	heldCount_ = held;
	return changed;
}

bool ChainCard::refreshMacroNames() {
	// THE RUNNING CHAIN'S NAMES, asked by id every time: the surface is a contemporaneous handle, never kept. A rename
	// made in the chain's window, a patch load and a chain preset load all move the chain's own record, and this
	// reads that record — the same one the window's macro slots read.
	const device::Descriptor* desc = host_.descriptorOf(device_);
	void* surface = host_.containerSurfaceOf(device_);
	bool changed = false;
	for (size_t i = 0; i < macroCaption_.size(); ++i) {
		std::string name = backplane::innerMacroLabelOf(desc, surface, i);
		if (name.empty()) {
			name = kCaptions[i];
		}
		if (name != macroCaption_[i]) {
			macroCaption_[i] = std::move(name);
			// The knob holds a pointer into this string; an assignment that reused the buffer leaves the pointer
			// equal and `setLabel` would see no change, so the repaint is asked for outright (`MacroSlot::refresh`).
			knobs_[i]->setLabel(macroCaption_[i].c_str());
			knobs_[i]->redraw();
			changed = true;
		}
	}
	return changed;
}

bool ChainCard::updateContent() {
	const bool renamed = refreshMacroNames();
	return refreshSummary() || renamed;
}

void ChainCard::drawHeadExtras(visage::Canvas& canvas) {
	drawSlotMenu(canvas, kMuted);
}

std::string ChainCard::headCaption() const {
	// THE SUMMARY IS THE HEAD (D6): what the chain holds, right-aligned before the head's marks. It
	// was drawn into a box that ran from x = 0 to the slot menu, so it sat under the enable square
	// and, open, under the expand mark too ("empty" crossed both). `headLayout` places it now,
	// between the name and the first mark, and drops it before it can crowd either.
	return summary_;
}

void ChainCard::drawContent(visage::Canvas&) {
	// The knobs are children and draw themselves; the frame and head are the base's.
}

} // namespace feathers::ui
