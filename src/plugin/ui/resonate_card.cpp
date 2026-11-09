/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#include "plugin/ui/resonate_card.h"

#include "plugin/ui/theme.h"

#include <algorithm>

namespace feathers::ui {

namespace rs = fx::resonate;

namespace {
/// WHERE THE BODY STARTS UNDER THE HEAD — `ClipCard`'s.
constexpr float kBodyTop = FxCard::kSlotHeadHeight + 4.0f;
} // namespace

ResonateCard::ResonateCard(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
                           Notifier* notifier)
    : host_(host), device_(id), notifier_(notifier) {
	face_ = std::make_unique<ResonateFace>(ResonateFace::Surface::Card, host_, device_, registry, notifier_, *this);
	// THE HEAD'S OCCUPANTS — `ClipCard`'s, word for word.
	onMouseDown() = [this](const visage::MouseEvent& e) {
		noteCardPress(e);
		if (slotMenuAt(e.position)) {
			pressSlotMenu();
		}
		else if (const auto nowOn = enablePressAt(e.position)) {
			if (notifier_ != nullptr) {
				notifier_->say(*nowOn ? "Resonate on" : "Resonate off");
			}
		}
	};
}

device::Modulatable* ResonateCard::enableModulatable() {
	return &face_->control(rs::Param::Enabled);
}

bool ResonateCard::headEnabled() const {
	return face_->control(rs::Param::Enabled).value() >= 0.5;
}

void ResonateCard::setModulation(ModState* mod, EditHistory* history) {
	face_->setModulation(mod, history);
}

float ResonateCard::openHeightAt(float cardWidth) const {
	const float spine = headEdge() == CardHeadEdge::Left ? kCardHeadHeight : 0.0f;
	const float inner = std::max(0.0f, cardWidth - spine - 2.0f * kPad);
	return kBodyTop + ResonateFace::cardHeight(inner) + kPad;
}

float ResonateCard::laneHeightAt(float cardWidth) const {
	return folded() ? foldedHeight() : openHeightAt(cardWidth);
}

float ResonateCard::laneExtent(float cross) const {
	if (folded()) {
		return kCardHeadHeight;
	}
	return FxCard::laneExtent(cross);
}

void ResonateCard::layOut() {
	// `setBounds` UNCONDITIONALLY, every time (CLAUDE.md's DPI rule) — the face does nothing else.
	face_->layOutCard(bodyLeft() + kPad, kBodyTop, std::max(0.0f, bodyWidth() - 2.0f * kPad), !folded());
}

void ResonateCard::resized() { layOut(); }

void ResonateCard::foldedChanged() { layOut(); }

void ResonateCard::drawHeadExtras(visage::Canvas& canvas) {
	// **THE SLOT MENU IS DRAWN HERE** — see `ClipCard::drawHeadExtras`.
	drawSlotMenu(canvas, kMuted);
}

bool ResonateCard::updateContent() {
	const ResonateFace::Changes c = face_->refresh();
	// A GATE MOVED: re-lay out, and redraw the body for the captions that moved with it — the delivery,
	// not only the report (`CompCard::updateContent`). The ring view and the meter redraw themselves.
	if (c.gates) {
		layOut();
		redraw();
	}
	return c.any();
}

void ResonateCard::drawContent(visage::Canvas& canvas) {
	if (folded()) {
		return;
	}
	face_->drawCaptions(canvas);
}

std::vector<rs::Param> ResonateCard::addressedParamsForTest() const {
	std::vector<rs::Param> out = face_->addressedParams();
	out.push_back(rs::Param::Enabled);
	return out;
}

} // namespace feathers::ui
