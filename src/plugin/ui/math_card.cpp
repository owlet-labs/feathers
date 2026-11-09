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

#include "plugin/ui/math_card.h"

#include "plugin/ui/menu_anchor.h"
#include "plugin/ui/notify_tray.h"
#include "plugin/ui/theme.h"
#include "plugin/ui/transfer_plot.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <limits>

namespace feathers::ui {

namespace {

namespace mth = feathers::mod::math;

[[nodiscard]] constexpr size_t at(mth::Param p) { return static_cast<size_t>(p); }

} // namespace
//-------------------//
// MathTransferGlyph //
//-------------------//

MathTransferGlyph::MathTransferGlyph(DeviceControl& map, DeviceControl& periods, DeviceControl& exponent,
                                     const DeviceControl& bipolar, Notifier* notifier)
    : map_(map), periods_(periods), exponent_(exponent), bipolar_(bipolar), notifier_(notifier) {
	onMouseDown() = [this](const visage::MouseEvent& e) {
		// A CURVE WITH NO COEFFICIENT IS ITS MENU ON THE PRESS, as the route chip's is; one with a coefficient
		// waits to see whether the press moves (`transfer_plot.h`'s threshold).
		DeviceControl* control = coefficientControl();
		if (control == nullptr) {
			drag_.reset();
			if (onMenu) {
				onMenu();
			}
			return;
		}
		drag_ = Drag { e.position.y, transferCode(), control, false };
	};
	onMouseDrag() = [this](const visage::MouseEvent& e) {
		if (!drag_) {
			return;
		}
		const float up = drag_->anchorY - e.position.y;
		if (!drag_->moved && std::abs(up) < kTransferDragThreshold) {
			return;
		}
		if (!drag_->moved) {
			// ONE GESTURE FROM HERE TO THE RELEASE — `DeviceControl::endGesture` records it as one undo step (none if
			// it came back to where it started).
			drag_->moved = true;
			drag_->control->beginGesture();
		}
		const double next = static_cast<double>(device::transferCoefficient(transferDragTarget(drag_->start, up)));
		if (next != drag_->control->value()) {
			drag_->control->setValue(next);
		}
		redraw();
	};
	onMouseUp() = [this](const visage::MouseEvent&) {
		if (!drag_) {
			return;
		}
		const Drag drag = *drag_;
		drag_.reset();
		if (!drag.moved) {
			if (onMenu) {
				onMenu(); // A CLICK: the menu, as the chip opens it.
			}
			return;
		}
		drag.control->endGesture();
		if (notifier_ != nullptr && transferCode() != drag.start) {
			notifier_->say("Transfer " + readout());
		}
		redraw();
	};
	// LAST, after every `=` above — see `installTooltipHover`: the hover reaches the card's tooltip layer.
	installTooltipHover(*this, *this);
}

mth::Map MathTransferGlyph::map() const {
	return static_cast<mth::Map>(
	    std::clamp(static_cast<long>(std::lround(map_.value())), 0L, static_cast<long>(mth::Map::kCount) - 1));
}

DeviceControl* MathTransferGlyph::coefficientControl() const {
	switch (map()) {
	case mth::Map::Log:
	case mth::Map::Exp: return &exponent_;
	case mth::Map::Phi: return &periods_;
	default: return nullptr;
	}
}

device::Transfer MathTransferGlyph::transferCode() const {
	switch (map()) {
	case mth::Map::Log: return device::transferWithCoefficient(device::Transfer::Log, static_cast<float>(exponent_.value()));
	case mth::Map::Exp: return device::transferWithCoefficient(device::Transfer::Exp, static_cast<float>(exponent_.value()));
	case mth::Map::Phi: return device::transferWithCoefficient(device::Transfer::Phi, static_cast<float>(periods_.value()));
	default: return device::Transfer::Lin;
	}
}

visage::Bounds MathTransferGlyph::plotBounds() const {
	// THE FACE'S BAND, Amount's beside it, less the caption's margin each side and a few pixels inside the box.
	const float inset = 3.0f;
	const float x = Knob::kCaptionMargin + inset;
	return { x, inset, std::max(0.0f, width() - 2.0f * x), std::max(0.0f, MathCard::kAmountDiameter - 2.0f * inset) };
}

std::vector<visage::Point> MathTransferGlyph::plotPoints() const {
	const mth::Map m = map();
	const auto octaves = static_cast<float>(periods_.value());
	const auto exponent = static_cast<float>(exponent_.value());
	const visage::Bounds box = plotBounds();
	return transferPlotPoints(box, bipolar_.value() >= 0.5, transferPlotSteps(m == mth::Map::Phi, box.width()),
	                          [m, octaves, exponent](float x) { return mth::mapCurve(m, x, octaves, exponent); });
}

std::string MathTransferGlyph::readout() const {
	const mth::Map m = map();
	char text[32] = {};
	switch (m) {
	case mth::Map::Log:
	case mth::Map::Exp:
		std::snprintf(text, sizeof(text), "%s %.2f", mth::kMapNames[static_cast<size_t>(m)], exponent_.value());
		break;
	case mth::Map::Phi: {
		// THE ROUTE CHIP'S PRECISION: a tenth below ten periods, whole ones above.
		const double periods = std::exp2(std::clamp(periods_.value(), 0.0, static_cast<double>(device::kPhiOctavesMax)));
		std::snprintf(text, sizeof(text), periods < 10.0 ? "Phi %.1f" : "Phi %.0f", periods);
		break;
	}
	default: std::snprintf(text, sizeof(text), "%s", mth::kMapNames[static_cast<size_t>(m)]); break;
	}
	return text;
}

void MathTransferGlyph::draw(visage::Canvas& canvas) {
	// THE BOX THE DROPDOWN HAD, so the glyph reads as a control and not as a picture.
	canvas.setColor(kSurface);
	canvas.roundedRectangle(Knob::kCaptionMargin, 0.0f, std::max(0.0f, width() - 2.0f * Knob::kCaptionMargin),
	                        MathCard::kAmountDiameter, 3.0f);
	strokeTransferPlot(canvas, plotBounds(), plotPoints(), map() == mth::Map::Lin);
	canvas.setColor(kMuted);
	const float caption = MathCard::kAmountDiameter + Knob::kGap;
	Knob::drawColumnCaption(canvas, Knob::columnCaption("Transfer", width()), width(), caption);
	canvas.setColor(dragging() ? kModulation : kMuted);
	canvas.text(readout(), font(kKnobReadoutSize), visage::Font::kCenter, 0.0f,
	            caption + Knob::kLabelHeight + Knob::kLineGap, width(), Knob::kReadoutHeight);
}


MathCard::MathCard(backplane::DeviceHost& host, DeviceInstanceId id, Notifier* notifier, ModState* mod,
                   EditHistory* history)
    : host_(host), device_(id), notifier_(notifier), mod_(mod), history_(history) {
	drawn_.fill(std::numeric_limits<double>::quiet_NaN());
	// CAPTIONS ARE THE SHORT FORMS — the head already says MATH, and "A" under a knob reads as the
	// operand it is.
	a_ = std::make_unique<Knob>(control(at(mth::Param::A)), "A", notifier_);
	a_->setFaceDiameter(kFaceDiameter);
	op_ = std::make_unique<ValueBox>(control(at(mth::Param::Operator)), "");
	op_->setCompact(true);
	b_ = std::make_unique<Knob>(control(at(mth::Param::B)), "B", notifier_);
	b_->setFaceDiameter(kFaceDiameter);
	amount_ = std::make_unique<Knob>(control(at(mth::Param::Amount)), "Amount", notifier_);
	amount_->setFaceDiameter(kAmountDiameter);
	bipolar_ = std::make_unique<SwitchChip>(control(at(mth::Param::Bipolar)), "Bipolar");
	transfer_ = std::make_unique<MathTransferGlyph>(control(at(mth::Param::Map)), control(at(mth::Param::Periods)),
	                                                control(at(mth::Param::Exponent)),
	                                                control(at(mth::Param::Bipolar)), notifier_);
	// THE GLYPH'S CLICK IS THE CARD'S GUARDED MENU, placed against the glyph's box.
	transfer_->onMenu = [this] {
		showMenuAt(transferMenu(), *transfer_, { 0.0f, 0.0f, transfer_->width(), kAmountDiameter });
	};

	// **THE BOX'S OWN LIST, WITH THE PICK GUARDED** — see `guardedMenuFor`. `onPicker` takes the press
	// before the box's own menu would, so the unguarded one is never shown from this card.
	op_->onPicker([this] { showMenuAt(guardedMenuFor(*op_), *op_, { 0.0f, 0.0f, op_->width(), op_->height() }); });

	for (Knob* knob : { a_.get(), b_.get(), amount_.get() }) {
		knob->setModulation(mod_, history_);
	}
	// ROW ORDER, which is also the order a screen reader or a tab would meet them.
	addChild(a_.get());
	addChild(op_.get());
	addChild(b_.get());
	addChild(amount_.get());
	addChild(bipolar_.get());
	addChild(transfer_.get());
	(void)applyGates();

	onMouseDown() = [this](const visage::MouseEvent& e) {
		// THE PRESS IS THE CARD'S TO HAND BACK — `MacrosCard`'s note: the head's marks, the fold and the
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

MathCard::~MathCard() {
	// **NOTHING THAT OUTLIVES THE CARD REACHES IT.** A menu still open on the root frame holds a copy of
	// the guarded pick; with the token gone it finds no card behind it. The members then die in reverse
	// order: widgets before the controls they are built on.
	alive_.reset();
}

DeviceControl& MathCard::control(size_t index) const {
	if (controls_[index] == nullptr) {
		controls_[index] = std::make_unique<DeviceControl>(host_, device_, mth::kDescriptor, index, mod_, history_);
	}
	return *controls_[index];
}

visage::PopupMenu MathCard::guardedMenuFor(ValueBox& box) {
	// **THE BOX BUILDS THE LIST AND OWNS THE WRITE** — its rows, its X on the current entry, and its pick
	// bracketed as one gesture (`ValueBox::choiceMenu`). This only puts the token in front of that pick:
	// the box's own callback reaches the box, and the box dies with the card.
	visage::PopupMenu menu = box.choiceMenu();
	const std::weak_ptr<void> alive = alive_;
	const visage::CallbackList<void(int)> pick = menu.onSelection();
	menu.onSelection() = [alive, pick](int id) {
		if (!alive.expired()) {
			pick.callback(id);
		}
	};
	return menu;
}

device::Modulatable* MathCard::enableModulatable() {
	return &control(at(mth::Param::Enabled));
}

void MathCard::setModulation(ModState* mod, EditHistory* history) {
	// **THE CONTROL NEEDS IT, NOT ONLY THE KNOB** — `GenericCard::setModulation`'s note: a knob asks its
	// control whether it is routable, and the control answers from its own `mod_`.
	mod_ = mod;
	history_ = history;
	for (size_t i = 0; i < controls_.size(); ++i) {
		control(i).setModulation(mod, history);
	}
	for (Knob* knob : { a_.get(), b_.get(), amount_.get() }) {
		knob->setModulation(mod, history);
	}
}

std::vector<mth::Param> MathCard::addressedParamsForTest() const {
	// **ASKED OF THE WIDGETS' OWN CONTROLS, NOT LISTED** — a list written here would go on answering the
	// six after a widget was rebuilt over the wrong control. In row order.
	std::vector<mth::Param> out;
	const auto add = [&](const device::Modulatable* c) {
		if (const auto* road = dynamic_cast<const DeviceControl*>(c); road != nullptr && road->device() == device_) {
			out.push_back(static_cast<mth::Param>(road->control()));
		}
	};
	add(a_->control());
	add(&op_->control());
	add(b_->control());
	add(amount_->control());
	add(&bipolar_->control());
	// THE GLYPH: the map, and the coefficient the current map reads (the one its drag writes).
	add(&transfer_->mapControl());
	if (const DeviceControl* coefficient = transfer_->coefficientControl(); coefficient != nullptr) {
		add(coefficient);
	}
	return out;
}

float MathCard::openHeight(size_t outputRows) {
	return kCardHeadHeight + bodyHeight() + kCardOutputStripHeight * static_cast<float>(outputRows);
}

float MathCard::laneHeightAt(float cardWidth) const {
	return folded() ? kCardHeadHeight : kCardHeadHeight + bodyHeight() + outputStripHeightAt(cardWidth);
}

void MathCard::resized() {
	// UNDER A TOP HEAD OR BESIDE A LEFT SPINE ALIKE (`bodyLeft`, `bodyTopEdge`), so the rack's card and
	// the strip's are one layout at two widths.
	const float left = bodyLeft() + kPadX;
	const float inner = std::max(0.0f, bodyWidth() - 2.0f * kPadX);

	// ROW ONE — A, THE OPERATOR, B. The two knobs share what the operator and its gaps leave; the
	// operator is centred on the knob FACES rather than on the columns, so it reads as the sign between
	// two dials and not as a third control with its own caption row.
	const float top = bodyTopEdge() + kGap;
	const float knobWidth = std::max(0.0f, (inner - kOperatorWidth - 2.0f * kGap) * 0.5f);
	a_->setBounds(left, top, knobWidth, firstRowHeight());
	const float faceCentre = top + kFaceDiameter * 0.5f;
	op_->setBounds(left + knobWidth + kGap, faceCentre - ValueBox::kHeight * 0.5f, kOperatorWidth, ValueBox::kHeight);
	b_->setBounds(left + knobWidth + kGap + kOperatorWidth + kGap, top, knobWidth, firstRowHeight());

	// ROW TWO — THIRDS, IN EVERY STATE. Each widget at its own natural height, so the chip is not stretched into a
	// slab (SKLITTER's Engage, `GenericCard::rowHeightFor`'s note), and each level with Amount's FACE: the chip on
	// the face's centre line, the glyph's plot in the face's band with its caption and reading on the knob's lines.
	// The coefficient is the glyph's drag now (backlog PY), so no box joins the row under Phi/Log/Exp and it never
	// changes shape under the hand.
	const float second = top + firstRowHeight() + kGap;
	const float third = std::max(0.0f, (inner - 2.0f * kGap) / 3.0f);
	amount_->setBounds(left, second, third, secondRowHeight());
	bipolar_->setBounds(left + third + kGap, second + (kAmountDiameter - SwitchChip::kHeight) * 0.5f, third,
	                    SwitchChip::kHeight);
	transfer_->setBounds(left + 2.0f * (third + kGap), second, third, secondRowHeight());
}

bool MathCard::applyGates() {
	const auto rounded = [this](mth::Param p) { return static_cast<long>(std::lround(control(at(p)).value())); };
	const bool comparison = mth::isComparison(static_cast<mth::Op>(std::clamp(
	    rounded(mth::Param::Operator), 0L, static_cast<long>(mth::Op::kCount) - 1)));
	if (bipolar_->enabled() == comparison) {
		bipolar_->setEnabled(!comparison);
		return true;
	}
	return false;
}

visage::PopupMenu MathCard::transferMenu() {
	// EVERY MAP, NAMED AND MARKED (the house mark, `addChoiceRow`), so the choices are seen before one is applied.
	DeviceControl& map = control(at(mth::Param::Map));
	const auto current = static_cast<long>(std::lround(map.value()));
	visage::PopupMenu menu;
	for (size_t i = 0; i < static_cast<size_t>(mth::Map::kCount); ++i) {
		(void)addChoiceRow(menu, static_cast<int>(i), mth::kMapNames[i], static_cast<long>(i) == current);
	}
	// THE PICK IS ONE GESTURE (one undo step through `DeviceControl::endGesture`), and reaches no freed card.
	const std::weak_ptr<void> alive = alive_;
	menu.onSelection() = [this, alive](int id) {
		if (alive.expired() || id < 0 || id >= static_cast<int>(mth::Map::kCount)) {
			return;
		}
		DeviceControl& road = control(at(mth::Param::Map));
		road.beginGesture();
		road.setValue(static_cast<double>(id));
		road.endGesture();
		transfer_->redraw();
	};
	return menu;
}

void MathCard::foldedChanged() {
	for (visage::Frame* child : std::initializer_list<visage::Frame*> { a_.get(), op_.get(), b_.get(), amount_.get(),
	                                                                     bipolar_.get(), transfer_.get() }) {
		child->setVisible(!folded());
	}
	(void)applyGates();
}

bool MathCard::updateContent() {
	// **POLLED, BECAUSE NOTHING TELLS A CARD ITS PARAMS MOVED** — an undo, a patch load, host automation
	// (`MacrosCard::updateContent`'s note). Six reads a frame; a widget repaints only when its own moved.
	const auto sync = [this](mth::Param p, visage::Frame& widget) {
		const double now = control(at(p)).value();
		double& was = drawn_[at(p)];
		if (now == was) {
			return false;
		}
		was = now;
		widget.redraw();
		return true;
	};
	bool told = false;
	told = sync(mth::Param::A, *a_) || told;
	told = sync(mth::Param::B, *b_) || told;
	told = sync(mth::Param::Operator, *op_) || told;
	told = sync(mth::Param::Map, *transfer_) || told;
	told = sync(mth::Param::Amount, *amount_) || told;
	// THE GLYPH PLOTS THE MAP AT ITS COEFFICIENT OVER THE RANGE BIPOLAR SETS, so any of the three repaints it.
	if (sync(mth::Param::Bipolar, *bipolar_)) {
		transfer_->redraw();
		told = true;
	}
	told = sync(mth::Param::Periods, *transfer_) || told;
	told = sync(mth::Param::Exponent, *transfer_) || told;
	told = applyGates() || told;
	return told;
}

bool MathCard::headSourceFocused() const {
	return mod_ != nullptr && mod_->source() == backplane::Source::output(device_, 0);
}

bool MathCard::headArmed() const {
	return headSourceFocused() && mod_->armed();
}

} // namespace feathers::ui
