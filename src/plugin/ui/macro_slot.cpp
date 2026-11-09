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

#include "plugin/ui/macro_slot.h"

#include "plugin/ui/menu_anchor.h"    // `showMenuAt`, the one road a dropdown opens by
#include "plugin/ui/mod_route_menu.h" // `appendHostRows` — the pad's host rows (T11)

#include <algorithm>

namespace feathers::ui {

namespace {

[[nodiscard]] SwitchChip::Behaviour behaviourOf(device::MacroType type) {
	return type == device::MacroType::Momentary ? SwitchChip::Behaviour::Momentary : SwitchChip::Behaviour::Latch;
}

} // namespace

MacroSlot::MacroSlot(device::Modulatable& control, std::string shortCaption, Naming naming, MacroMenuHooks hooks,
                     Notifier* notifier)
    : shortCaption_(std::move(shortCaption)), naming_(std::move(naming)), hooks_(std::move(hooks)),
      caption_(shortCaption_), self_(std::make_shared<MacroSlot*>(this)) {
	// **THE SLOT'S OWN TWO VERBS, OVER THE OWNER'S.** Rename is the slot's entirely — the field is
	// here. A type pick is the owner's write followed by the slot re-reading it, so the control changes
	// in the same instant the menu closes rather than on the next tick. Both go through the weak handle:
	// a menu holds a COPY of these and can outlive the row (`self_`). A verb the owner left empty stays
	// empty, so `complete()` still tells the truth about what the owner gave.
	// **THE SLOT ITSELF TAKES NO PRESS; ITS CONTROLS DO.** The knob and the pad each cover the column,
	// so today nothing falls through — but a slot that took presses would swallow any point a future
	// layout leaves uncovered, rather than let it reach the panel beneath as the arrow row under it does.
	setIgnoresMouseEvents(true, true);

	const std::weak_ptr<MacroSlot*> weak = self_;
	hooks_.beginRename = [weak] {
		if (const auto slot = weak.lock()) {
			(*slot)->beginRename();
		}
	};
	if (hooks_.writeType) {
		hooks_.writeType = [weak, write = hooks_.writeType](device::MacroType type) {
			write(type);
			if (const auto slot = weak.lock()) {
				(*slot)->syncType();
			}
		};
	}

	knob_ = std::make_unique<Knob>(control, caption_.c_str(), notifier);
	knob_->setMacroMenu(hooks_);
	// A MACRO'S CAPTION IS A NAME THE USER TYPED (backlog OT): as typed, shrunk to fit, cut only below the floor.
	knob_->setCaptionIsName(true);
	addChild(knob_.get());

	chip_ = std::make_unique<SwitchChip>(control, caption_);
	// **A PAD IN THE FACE, NOT A STRIP** (by design, `SwitchChip::Presentation`): the button takes
	// the knob's place, caption and reading included, so the row keeps one rhythm.
	chip_->setPresentation(SwitchChip::Presentation::Pad);
	chip_->setCaptionIsName(true);
	// THE SAME MENU ON THE CHIP, on the right button the chip already reserves for it — a macro turned
	// into a latch must still be able to turn back. **AND THE SAME HOST ROWS AS THE KNOB'S** (host slots T11:
	// a nested rack's macro is its device's control, exposable as a pad too), after the macro rows and a break
	// (`appendHostRows`), each pick guarded by this slot's token — the popup may outlive the row. A header
	// macro is already a host parameter and answers `Unavailable`, so its pad gets its macro rows alone.
	control_ = &control;
	chip_->setPopup([this] {
		(void)showMenuAt(padMenu(), *chip_, { 0.0f, 0.0f, chip_->width(), chip_->height() });
	});
	addChild(chip_.get());

	// **THE ENCODER A RELATIVE MACRO BECOMES (backlog PQ)** — dormant like the chip until the type asks for it. Its
	// turns go to whatever the owner handed `setRelativeHooks`, read at the moment of the turn, so the hooks can
	// arrive after construction. The menu is the same one, so the type can be switched back.
	encoder_ = std::make_unique<EncoderFace>(
	    caption_.c_str(),
	    EncoderFace::Hooks {
	        .begin = [this] { if (relative_.begin) { relative_.begin(); } },
	        .turn = [this](double delta) { if (relative_.turn) { relative_.turn(delta); } },
	        .end = [this] { if (relative_.end) { relative_.end(); } },
	    },
	    hooks_);
	encoder_->setVisible(false);
	addChild(encoder_.get());

	// **THE FIELD IS HIDDEN, NEVER DESTROYED, FROM ITS OWN CALLBACKS** (`text_field.h`): visage's frames
	// are beneath both on the stack. The write comes first so the host's listener, which may call
	// `refresh()` on this slot, finds the field still open — harmless either way, since refresh does
	// not touch it.
	field_ = std::make_unique<TextField>();
	field_->onCommit([this](std::string text) {
		if (naming_.rename) {
			naming_.rename(text);
		}
		field_->setVisible(false);
		refresh();
	});
	field_->onCancel([this] { field_->setVisible(false); });
	field_->setVisible(false);
	addChild(field_.get());

	refresh();
}

MacroSlot::~MacroSlot() {
	// **NOTHING CALLS BACK INTO A SLOT THAT HAS STARTED DYING.** In this order:
	// 1. The weak handle goes, so a menu still open on screen — it holds a copy of `hooks_` — finds no
	//    slot behind its Rename or its type pick.
	// 2. The field and the chip are told to call nobody. The field already calls nobody when it is
	//    destroyed mid-edit, and says so; the popup is the chip's only road back here. Cleared anyway,
	//    because members die in reverse order and a callback that reached `field_` or `chip_` after
	//    either was gone would be the use-after-free.
	// 3. The members then die with nothing wired to this object. The chip's destructor may still WRITE
	//    — a held momentary lets go through `control` — which is the owner's control and outlives us.
	self_.reset();
	field_->onCommit(nullptr);
	field_->onCancel(nullptr);
	chip_->setPopup(nullptr);
}

visage::PopupMenu MacroSlot::padMenu() {
	visage::PopupMenu menu = macroMenu(hooks_);
	const std::weak_ptr<MacroSlot*> weak = self_;
	(void)appendHostRows(menu, *control_, std::weak_ptr<void>(self_), [weak](const std::string&) {
		if (const auto slot = weak.lock()) {
			(*slot)->chip().redraw();
			(*slot)->knob().redraw();
		}
	});
	return menu;
}

void MacroSlot::setFaceDiameter(float diameter) {
	knob_->setFaceDiameter(diameter);
	chip_->setFaceDiameter(diameter);
	encoder_->setFaceDiameter(diameter);
	resized();
}

void MacroSlot::setModulation(ModState* state, EditHistory* history) {
	knob_->setModulation(state, history);
}

void MacroSlot::refresh() {
	// THE NAME FIRST, then the type, so a restore that changed both re-seats a control already carrying
	// its new caption.
	std::string caption = naming_.renamed && naming_.renamed() && naming_.name ? naming_.name() : shortCaption_;
	if (caption != caption_) {
		caption_ = std::move(caption);
		// The knob holds a pointer into `caption_`; hand it the current one and redraw, because an
		// assignment that reused the buffer leaves the pointer equal and `setLabel` would see no change.
		knob_->setLabel(caption_.c_str());
		knob_->redraw();
		chip_->setLabel(caption_);
		encoder_->setCaption(caption_.c_str());
	}
	// **THE NAME A HOVER SAYS** when the caption is "M1" or a typed name cut to its column (long macro names in the
	// main Feathers macros hit this): the effective name, the user's or the host's.
	std::string full = naming_.name ? naming_.name() : caption_;
	knob_->setFullName(full);
	encoder_->setFullName(full);
	chip_->setFullName(std::move(full));
	applyType(hooks_.readType ? hooks_.readType() : device::MacroType::Knob);
}

bool MacroSlot::syncType() {
	const device::MacroType type = hooks_.readType ? hooks_.readType() : device::MacroType::Knob;
	if (type == type_ && knob_->isVisible() == showsKnob(type) && encoder_->isVisible() == showsEncoder(type)) {
		return false;
	}
	applyType(type);
	return true;
}

void MacroSlot::applyType(device::MacroType type) {
	type_ = type;
	const bool knob = showsKnob(type);
	const bool encoder = showsEncoder(type);
	knob_->setVisible(knob);
	encoder_->setVisible(encoder);
	if (knob || encoder) {
		// HIDING THE CHIP ITSELF is what lets a held momentary go (`SwitchChip::visibilityChanged`).
		chip_->setVisible(false);
	}
	else {
		const SwitchChip::Behaviour behaviour = behaviourOf(type);
		if (chip_->behaviour() != behaviour) {
			// A MOMENTARY TURNED LATCH WHILE HELD: its release would still arrive and write min, but the
			// hand is now on a latch and the latch's press is over. Let go now, as the switch away did.
			chip_->cancelHold();
			chip_->setBehaviour(behaviour);
		}
		chip_->setVisible(true);
	}
	placeField();
	redraw();
}

void MacroSlot::resized() {
	// THE KNOB AND THE PAD BOTH TAKE THE WHOLE COLUMN, exactly as the knob did when the row held bare
	// knobs: each draws its face at the top and its caption and reading under it, at the same offsets.
	knob_->setBounds(0.0f, 0.0f, width(), height());
	chip_->setBounds(0.0f, 0.0f, width(), height());
	encoder_->setBounds(0.0f, 0.0f, width(), height());
	placeField();
}

void MacroSlot::placeField() {
	// **OVER WHERE THE NAME IS READ** — the caption line under the face, for every type now that a pad
	// prints its caption where a knob does — grown to the field's height about that line's centre so the
	// typed name sits where the printed one did.
	const float lineCentre = knob_->faceDiameter() + Knob::kGap + Knob::kLabelHeight * 0.5f;
	const float h = SwitchChip::kHeight;
	field_->setBounds(0.0f, std::max(0.0f, lineCentre - h * 0.5f), width(), h);
}

void MacroSlot::beginRename() {
	if (!isVisible()) {
		return; // a field opened in a slot nobody can see would hold the keyboard invisibly
	}
	placeField();
	field_->setVisible(true);
	field_->begin(naming_.name ? naming_.name() : caption_);
}

void MacroSlot::closeField() {
	// HIDING THE FIELD ITSELF is what cancels it (`TextField::visibilityChanged`); its onCancel then
	// hides it again, which finds nothing to do.
	if (field_->isVisible()) {
		field_->setVisible(false);
	}
}

void MacroSlot::visibilityChanged() {
	if (isVisible()) {
		return;
	}
	// **visage TELLS ONLY THE FRAME WHOSE OWN FLAG MOVED.** A hidden slot's children keep reading as
	// visible, so neither the field nor the chip hears this — the slot acts for both: an edit nobody
	// can see is abandoned, and a momentary nobody can see or release lets go.
	closeField();
	chip_->cancelHold();
}

} // namespace feathers::ui
