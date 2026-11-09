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

#pragma once

#include "device/macro_type.h"
#include "device/modulatable.h"
#include "plugin/ui/encoder_face.h"
#include "plugin/ui/frame_watchers.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/macro_menu.h"
#include "plugin/ui/switch_chip.h"
#include "plugin/ui/text_field.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <visage/ui.h>

// **ONE MACRO, AS WHATEVER CONTROL ITS TYPE SAYS IT IS — AND THE PLACE ITS NAME IS TYPED.**
//
// A macro is a knob, a latch or a momentary (`device/macro_type.h`), and the type is patch state a
// right-click changes under the hand. So the slot holds ALL of them from the start — a `Knob`, a
// `SwitchChip` and the `TextField` Rename opens — and the type only picks which one is shown: the
// dormant-widget discipline, and the only shape in which a type switch cannot race a construction.
//
// **A SWITCH OF TYPE WRITES NOTHING.** The value is the macro's, not the control's: a knob at 70%
// switched to a latch is a lit latch, and switched back is the knob at 70%. The one exception is
// deliberate and belongs to the chip — a momentary HELD when it stops being shown lets go
// (`SwitchChip::cancelHold`), because a macro stuck at max behind a control nobody can see or release
// is a stuck note.
//
// **HOST-AGNOSTIC ON PURPOSE.** Everything the slot knows about where the macro lives arrives as a
// function — the name, whether it is a rename, the rename verb, and the menu's hooks — so Feathers'
// header row, the rack's row and the MACROS card can all seat one over their own bridge.

namespace feathers::ui {

class Notifier;

class MacroSlot : public visage::Frame {
public:
	/// **WHAT A MACRO IS CALLED, AND THE VERB THAT CHANGES IT** — the host's three, asked afresh on
	/// every `refresh()`, never copied into the slot as a name it would then have to keep in step.
	struct Naming {
		/// The effective name — the user's, or the host's default ("Macro 1").
		std::function<std::string()> name;
		/// Whether there is a name of the user's. The caption is the short one ("M1") when there is not:
		/// a 54px column cannot hold "Macro 1" and the row's heading already says what these are.
		std::function<bool()> renamed;
		/// THE RENAME — what Enter in the field hands its text to. The host normalises and clamps.
		/// **IT MUST NOT DESTROY THE SLOT SYNCHRONOUSLY**: it runs inside the field's commit callback, with
		/// the field's and visage's frames beneath it on the stack, and the slot hides the field and
		/// refreshes its caption after it returns. A host that rebuilds its row on a rename (the rack's
		/// may) defers the rebuild to its next tick.
		std::function<void(const std::string&)> rename;
	};

	/// `control` is BORROWED and outlives the slot — the owner holds it, as it held it for the bare
	/// knob this replaces; the chip's destructor may write through it (a held momentary letting go).
	/// `hooks` are the owner's menu verbs; the slot keeps them but answers `beginRename` itself and
	/// re-reads its type after `writeType`, so a pick in the menu re-seats the control at once.
	/// `notifier` may be null, as a knob's may.
	MacroSlot(device::Modulatable& control, std::string shortCaption, Naming naming, MacroMenuHooks hooks,
	          Notifier* notifier = nullptr);
	/// **CALLS NOBODY ON THE WAY OUT.** See the definition for the order, which is the point.
	~MacroSlot() override;

	/// As tall as the knob column it replaces, so a row laid out for knobs does not move.
	static constexpr float columnHeight(float faceDiameter) { return Knob::columnHeight(faceDiameter); }

	/// The knob's face size, and the pad's: a button macro is a pad the size of the face.
	void setFaceDiameter(float diameter);
	/// Handed to the knob — a macro is never a destination, but the knob redraws with the arm.
	void setModulation(ModState* state, EditHistory* history);
	/// Keep a subscription alive for as long as the slot. The owner subscribes BOTH controls to the
	/// macro's parameter, because the one on show has to redraw when the host automates it.
	void holdWatch(Watch watch) { held_.push_back(std::move(watch)); }

	/// **RE-READ THE TYPE AND THE NAME** and re-seat the control and the captions to match. What the
	/// owner calls when either may have moved without the slot's own menu moving it — a rename heard
	/// from the host, a patch restore. Writes nothing to the macro.
	void refresh();
	/// **THE TYPE ALONE, AND ONLY IF IT MOVED** — the cheap half of `refresh`, cheap enough to ask on
	/// every editor tick (no string is built). True when the control was re-seated. It is what catches
	/// a patch restore that changes a type and not a name, which no listener reports.
	bool syncType();

	/// **OPEN THE NAME FOR EDITING** — the field over the caption, holding the current name, with the
	/// keyboard. What the menu's "Rename…" does. Nothing while the slot is hidden.
	void beginRename();
	[[nodiscard]] bool renaming() const noexcept { return field_->editing(); }

	/// The caption on show — the user's name, or the short one.
	[[nodiscard]] const std::string& caption() const noexcept { return caption_; }
	/// The type the control on show was seated for.
	[[nodiscard]] device::MacroType type() const noexcept { return type_; }

	[[nodiscard]] Knob& knob() const noexcept { return *knob_; }
	[[nodiscard]] SwitchChip& chip() const noexcept { return *chip_; }
	/// THE ENDLESS ENCODER a Relative macro shows (backlog PQ) — built with the slot, shown only for Relative.
	[[nodiscard]] EncoderFace& encoder() const noexcept { return *encoder_; }
	/// **WHAT A TURN OF THE ENCODER DOES** — the owner's, handed in once. A host that never hands these has no
	/// relative road, and a Relative type there shows the knob rather than an encoder that would do nothing.
	void setRelativeHooks(EncoderFace::Hooks hooks) {
		relative_ = std::move(hooks);
		syncType();
	}
	[[nodiscard]] TextField& field() const noexcept { return *field_; }
	/// The hooks as the slot hands them to both controls' menu — the owner's, with the slot's own
	/// `beginRename` and the re-seat after `writeType`.
	[[nodiscard]] const MacroMenuHooks& menuHooks() const noexcept { return hooks_; }
	/// **THE PAD'S RIGHT-CLICK** — the macro rows, then (behind a break) the host rows of the control underneath,
	/// as the knob's menu has them (host slots T11: a nested rack's macro is exposable as a pad too). Every
	/// pick guarded by this slot's token. What the chip's popup opens; public so a case can read it.
	[[nodiscard]] visage::PopupMenu padMenu();
	/// The owner's naming functions, as handed in — what the field's commit calls.
	[[nodiscard]] const Naming& naming() const noexcept { return naming_; }

	void resized() override;
	void visibilityChanged() override;

private:
	void applyType(device::MacroType type);
	/// Which control a type shows. A Relative macro on a host with no relative road (no hooks) shows the knob.
	[[nodiscard]] bool showsEncoder(device::MacroType type) const noexcept {
		return type == device::MacroType::Relative && static_cast<bool>(relative_.turn);
	}
	[[nodiscard]] bool showsKnob(device::MacroType type) const noexcept {
		return type == device::MacroType::Knob || (type == device::MacroType::Relative && !relative_.turn);
	}
	void placeField();
	void closeField();

	std::string shortCaption_;
	Naming naming_;
	MacroMenuHooks hooks_;
	/// THE CAPTION BOTH CONTROLS PRINT. The knob holds a `const char*` into it (`Knob::label`), so it
	/// is a member and outlives the knob.
	std::string caption_;
	device::MacroType type_ = device::MacroType::Knob;
	/// **THE SLOT, FOR ANYTHING THAT MAY OUTLIVE IT.** A menu copies its hooks when it opens and is
	/// parented to the top frame, so a pick can arrive after the row that opened it was rebuilt (the
	/// rack's does). The slot's own hooks capture a weak handle to this and do nothing once it is gone.
	std::shared_ptr<MacroSlot*> self_;
	/// The control both faces drive — the owner's, outliving this slot.
	device::Modulatable* control_ = nullptr;
	std::unique_ptr<Knob> knob_;
	std::unique_ptr<SwitchChip> chip_;
	std::unique_ptr<EncoderFace> encoder_;
	EncoderFace::Hooks relative_;
	std::unique_ptr<TextField> field_;
	std::vector<Watch> held_;
};

} // namespace feathers::ui
