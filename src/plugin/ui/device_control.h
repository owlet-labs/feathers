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

#pragma once

#include "backplane/device_host.h"
#include "device/modulatable.h"
#include "plugin/ui/edit_history.h"
#include "plugin/ui/mod_readout.h"
#include "plugin/ui/mod_route_menu.h"
#include "plugin/ui/mod_state.h"

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// ONE CONTROL OF ONE DOCKED DEVICE, addressed the way a swallowed device has to be addressed — and
// through a host it does not know the type of.
//
// `FxControl` WITH `ParamBridge` REPLACED BY `backplane::DeviceHost`, and nothing else changed. Every
// rule that adapter established is preserved here deliberately rather than re-derived, because each
// one was paid for:
//
//   - A CONTROL IS ADDRESSED BY ITS INDEX in the device's own declarations, never by a host
//     identifier. device.h's permanent rule.
//   - AN ABSENT FIELD IS THE DECLARED DEFAULT, not zero — device_row.h's rule, which matters here
//     because a card may be looking at a patch written by a build that declared fewer controls.
//   - A GESTURE ON THIS SIDE IS NOT A HOST GESTURE, so this road records its own undo step. A host
//     parameter has a DAW behind it that owns undo; a swallowed control has nobody.
//   - `modulated` READS AS THE BASE, and that is a stated gap rather than an oversight — see
//     `display()`.

namespace feathers::ui {

class DeviceControl : public device::Modulatable, public RoutedControl, public RecordsEdits, public RouteReadoutSubject {
public:
	/// `mod` may be null — a control never handed the panel's state never reports itself armed.
	/// `history` is optional and is used for one thing: the undo step this road has to record itself.
	///
	/// **ADDRESSED BY THE DEVICE'S ID** (kill-lanes step 2) — never a slot or a lane. The host alone knows
	/// where it keeps the device; a control that moves with its device's card needs nothing re-seated.
	DeviceControl(backplane::DeviceHost& host, DeviceInstanceId device, const device::Descriptor& desc,
	              size_t control, ModState* mod = nullptr, EditHistory* history = nullptr)
	    : host_(host), device_(device), desc_(&desc), control_(control), mod_(mod), history_(history) {}
	/// **A LANE OR A SLOT IS NOT AN ID, AND A `size_t` WOULD NARROW INTO ONE WITHOUT A WORD** —
	/// `DeviceInstanceId` is a `uint32_t`. Deleted, so a card still holding its old address fails to
	/// compile rather than addressing whichever device wears that number as an id.
	DeviceControl(backplane::DeviceHost&, size_t, const device::Descriptor&, size_t, ModState* = nullptr,
	              EditHistory* = nullptr) = delete;

	void setModulation(ModState* mod, EditHistory* history) {
		mod_ = mod;
		history_ = history;
	}

	const device::ParamDesc& desc() const override { return desc_->params[control_]; }

	double value() const override {
		if (control_ >= desc_->params.size()) {
			return 0.0;
		}
		// ABSENT IS THE DECLARED DEFAULT, NEVER ZERO — see the note above.
		return control_ < host_.fieldsPresentAt(device_)
		           ? static_cast<double>(host_.fieldAt(device_, control_))
		           : desc_->params[control_].def;
	}

	void setValue(double value) override {
		host_.setField(device_, control_, static_cast<float>(value));
	}
	void releaseValue(double value) override { host_.releaseField(device_, control_, static_cast<float>(value)); }

	/// **THIS CONTROL AS A ROUTE'S DESTINATION** — its device and its own control index, which is all a
	/// destination is (kill-lanes step 2). Whether it may be one at all is `isDestination()`.
	backplane::Destination destination() const noexcept { return backplane::Destination::control(device_, control_); }

	/// WHETHER THIS CONTROL IS A MODULATION DESTINATION — its own declaration says, asked of the device
	/// actually at this id rather than of a type.
	bool isDestination() const noexcept {
		const device::Descriptor* d = host_.descriptorOf(device_);
		return d != nullptr && control_ < d->params.size() && d->params[control_].modulatable;
	}

	device::ModDisplay display() const override;
	/// THE DEVICE'S POLY SWITCH WHERE NO VOICE REACHES — inside a Rack (backlog NO): the host says why.
	/// AND WHERE THE ROW DOES NOT LET IT ACT (`Traits::perVoiceRequires`, backlog OP): the device says why.
	/// A surface's own tooltip for this control (`Modulatable::tooltipOverride`); empty restores the declared one.
	void setTooltipOverride(std::string text) { tooltipOverride_ = std::move(text); }
	const char* tooltipOverride() const override { return tooltipOverride_.empty() ? nullptr : tooltipOverride_.c_str(); }
	const char* unavailable() const override {
		if (static_cast<int>(control_) != desc_->traits.perVoiceControl) {
			return nullptr;
		}
		if (const char* why = host_.perVoiceUnavailable(); why != nullptr) {
			return why;
		}
		const int req = desc_->traits.perVoiceRequires;
		if (req >= 0 && static_cast<size_t>(req) < desc_->params.size()) {
			const auto c = static_cast<size_t>(req);
			const float v = c < host_.fieldsPresentAt(device_) ? host_.fieldAt(device_, c)
			                                                   : static_cast<float>(desc_->params[c].def);
			if (v < 0.5f) {
				return desc_->traits.perVoiceRequiresWhy != nullptr ? desc_->traits.perVoiceRequiresWhy
				                                                    : "Poly does nothing in this mode";
			}
		}
		return nullptr;
	}
	/// See `device::Modulatable::amountFor` — the per-source question `display()` cannot answer.
	/// Asked of the HOST, the same seam `display()` uses, so a rack device answers about its own
	/// editor rather than about Feathers' tables.
	float amountFor(device::Source source) const override {
		// THE SAME GUARD `display()` APPLIES: a control that is not a destination has no route to report.
		return isDestination() ? host_.routeAmount(source, destination()) : 0.0f;
	}
	void setAmount(float amount) override;
	std::string text() const override;
	/// **THE SAME PRINT AT A VALUE THE CONTROL IS NOT (YET) AT** — the device's hook first, then the unit.
	/// `text()` is this at `value()`. A `ValueBox` formats values it is only considering (its digit-drag
	/// parse asks about `at`, not about the stored value), so a box that prints through the device's hook
	/// needs the hook at an arbitrary value, not only at the current one (backlog IG, Groove's readout).
	[[nodiscard]] std::string textAt(double at) const;

	/// **TYPE-IN (backlog LS): THE DECLARED UNIT, UNLESS THE DEVICE PRINTS THIS CONTROL ITS OWN WAY AND THE
	/// UNIT PARSER CANNOT READ THAT BACK** — then a refusal, never a silently wrong value. Decided over a
	/// sweep of the whole range in the device's CURRENT state (a hook's words follow its mode: SKLITTER's
	/// Rate is a note length in one and a slice count in another), not at the one value showing.
	[[nodiscard]] std::optional<std::string> editText() const override;
	/// The device's own `TextOps::parse` first (the Rack's `Chain 2`), then the declared unit.
	[[nodiscard]] std::optional<double> parseText(std::string_view text, double shown) const override;
	/// Whether this control can be typed into — `editText`'s refusal, for a reader with no value in hand.
	[[nodiscard]] bool typeable() const;
	/// See `RecordsEdits`: the history `endGesture` records into.
	[[nodiscard]] EditHistory* recordsInto() const override { return history_; }

	/// **A CONTROL DESTROYED MID-DRAG STILL CLOSES ITS HOST GESTURE** (T6b review) — a card rebuilt, or the
	/// editor closed, while a hand is down. The host would otherwise hold a gesture open on the slot until
	/// the next Begin there.
	~DeviceControl() override { endHostGesture(); }
	DeviceControl(const DeviceControl&) = delete;
	DeviceControl& operator=(const DeviceControl&) = delete;

	/// THE HOST SLOTS (T7), asked of the host by this control's device and index — the host owns the pool,
	/// and a host with none answers `Unavailable`.
	[[nodiscard]] device::HostSlotStatus hostSlot() const override { return host_.hostSlotOf(device_, control_); }
	bool exposeToHost(std::string* displaced) override { return host_.exposeToHost(device_, control_, displaced); }
	bool unpinFromHost() override { return host_.unpinFromHost(device_, control_); }
	[[nodiscard]] uint64_t hostSlotGeneration() const noexcept override { return host_.hostSlotGeneration(); }

	void beginGesture() override;
	void endGesture() override;
	/// The host's Begin/End only (`Modulatable::beginHostGesture`), through `DeviceHost::begin/
	/// endControlGesture`, at most once each: `beginGesture` and `endGesture` call these too.
	void beginHostGesture() override;
	void endHostGesture() override;
	void beginAmountGesture() override;
	void endAmountGesture() override;

	/// See `ui::RoutedControl` — how the shared route menu finds the panel state without every widget
	/// having to be handed it.
	ModState* routingState() const override { return mod_; }

	DeviceInstanceId device() const noexcept { return device_; }
	size_t control() const noexcept { return control_; }

	/// **THE AS-MODULATED READOUT** (backlog PU; `mod_readout.h`) — this control as a route's destination: its host,
	/// its endpoint, and what it would receive from `source` at the route's amount, printed through `textAt`.
	[[nodiscard]] const void* readoutIdentity() const noexcept override { return host_.routeIdentity(); }
	[[nodiscard]] device::Destination readoutDestination() const noexcept override { return destination(); }
	[[nodiscard]] std::optional<ModReadout> routeReadout(device::Source source) override;
	void readoutEnded() override { host_.unwatchRoute(); }

private:
	/// THE THREE READS A DEVICE'S TEXT HOOK IS ALLOWED — see `device::TextView`. `host` is this control.
	static float viewField(const void* host, size_t control);
	static size_t viewReadingCount(const void* host);
	static float viewReading(const void* host, size_t index);

	backplane::DeviceHost& host_;
	DeviceInstanceId device_ = kNoInstance;
	const device::Descriptor* desc_ = nullptr;
	size_t control_ = 0;
	std::string tooltipOverride_;
	ModState* mod_ = nullptr;
	EditHistory* history_ = nullptr;
	double valueBefore_ = 0.0;
	float amountBefore_ = 0.0f;
	/// A host Begin has gone out and its End has not (`beginHostGesture`).
	bool hostGesture_ = false;
};

/// ONE OF A DOCKED DEVICE'S VALUES, CHANGED — the undo record for the value road.
///
/// `FxValueEdit` over a `DeviceHost` instead of a `ParamBridge`. It writes by the SAME ROAD
/// `DeviceControl::setValue` takes, so an undone value is coerced against the control's own
/// declaration exactly as the value that got recorded was.
class DeviceValueEdit : public EditRecord {
public:
	DeviceValueEdit(backplane::DeviceHost& host, DeviceInstanceId device, size_t control, double before,
	                double after)
	    : host_(host), device_(device), control_(control), before_(before), after_(after) {}

	void undo() const override { write(before_); }
	void redo() const override { write(after_); }
	const char* verb() const override { return "Set"; }

private:
	/// **AN UNDO IS A HAND** (integration review): an explicit act, so it wins over an unmirrored host write
	/// exactly as a drag does — and the host records it as one gesture, Begin/Value/End, rather than a bare
	/// Value outside any. The gesture is opened UNCONDITIONALLY, not through `ControlGestureScope`, whose
	/// "unchanged" test would skip exactly the case that matters: an undo back to the value the record still
	/// holds while the host's newer one sits unmirrored on the bus. (When the bus has not moved either, the
	/// owner sends nothing between the Begin and the End: an empty gesture, which a host ignores.)
	void write(double value) const {
		host_.beginControlGesture(device_, control_);
		host_.setField(device_, control_, static_cast<float>(value));
		host_.endControlGesture(device_, control_);
	}

	backplane::DeviceHost& host_;
	/// BY ID, so an undo after the device was dragged elsewhere still lands on it.
	DeviceInstanceId device_ = kNoInstance;
	size_t control_ = 0;
	double before_ = 0.0;
	double after_ = 0.0;
};

/// ONE ROUTE AMOUNT, CHANGED — the undo record for the amount road.
///
/// A RECORD OVER VALUES AND THE HOST, capturing no widget — so pressing undo after the panel has been
/// torn down puts the amount back rather than reaching into a dead surface. That is what
/// `edit_history.h` asks of any new author, and it is why this can live beside the control rather
/// than inside the stack.
///
/// IT TELLS THE PANEL, exactly as `setAmount` does. An undone amount that moved the ring and left the
/// route list a gesture behind would be the same defect this road already had once, arriving through
/// the history instead of through the hand.
class DeviceRouteEdit : public EditRecord {
public:
	DeviceRouteEdit(backplane::DeviceHost& host, ModState& mod, device::Source source,
	                device::Destination destination, float before, float after)
	    : host_(host), mod_(mod), source_(source), destination_(destination), before_(before),
	      after_(after) {}

	void undo() const override { write(before_); }
	void redo() const override { write(after_); }
	const char* verb() const override { return "Route"; }

private:
	void write(float amount) const {
		host_.setRouteAmount(source_, destination_, amount);
		mod_.touched();
	}

	backplane::DeviceHost& host_;
	ModState& mod_;
	device::Source source_ {};
	device::Destination destination_ {};
	float before_ = 0.0f;
	float after_ = 0.0f;
};


/// ONE FIELD OF A DEVICE'S ROW, BEFORE AND AFTER — the unit a compound step is made of.
struct DeviceFieldChange {
	size_t control = 0;
	float before = 0.0f;
	float after = 0.0f;
};

/// **A WHOLE VERB'S WORTH OF FIELDS, AS ONE UNDO STEP.** The drawn device's Flatten, Reset and two Copies
/// each move up to 120 fields, and each was up to 120 separate `DeviceValueEdit`s until this existed; a pad's
/// drag (Shaper, Phaser, Automod) moves its whole folded travel at once.
///
/// A RECORD OVER VALUES AND THE HOST, capturing no widget — `edit_history.h`'s standing requirement of any new
/// author, so undoing after the panel has been torn down puts the fields back rather than reaching into a dead
/// surface. It writes by `host_.setField`, the same road `DeviceControl::setValue` and `DeviceValueEdit` take.
///
/// It lived in `drawn_expanded.cpp` until a second device wanted it (backlog JB, the pads), which that file
/// said would be the day it moved here.
class DeviceFieldsEdit final : public EditRecord {
public:
	DeviceFieldsEdit(backplane::DeviceHost& host, DeviceInstanceId id, std::vector<DeviceFieldChange> changes,
	                 const char* verb)
	    : host_(host), device_(id), changes_(std::move(changes)), verb_(verb) {}

	// ONE HOST GESTURE PER UNDO OR REDO, per control it moves (host slots, T6b review).
	void undo() const override {
		backplane::ControlGestureScope gesture(host_, device_);
		for (const DeviceFieldChange& c : changes_) {
			gesture.setField(c.control, c.before);
		}
	}
	void redo() const override {
		backplane::ControlGestureScope gesture(host_, device_);
		for (const DeviceFieldChange& c : changes_) {
			gesture.setField(c.control, c.after);
		}
	}
	/// A STRING LITERAL FROM THE CALL SITE, which is what `EditRecord::verb`'s `const char*` already asks of
	/// every other record in the editor.
	const char* verb() const override { return verb_; }
	// NO `slot()` OVERRIDE: `EditRecord::slot` names a SOUND for a surface to point itself at, and this edit
	// is about a device.

private:
	backplane::DeviceHost& host_;
	DeviceInstanceId device_ = kNoInstance;
	std::vector<DeviceFieldChange> changes_;
	const char* verb_ = "Edit";
};

/// **A RUN OF A DEVICE'S FIELDS, HELD FROM PRESS TO RELEASE AS ONE UNDO STEP** — for a surface that writes
/// fields directly rather than through a `DeviceControl` (a pad's folded travel). `begin` reads the run, `end`
/// reads it again and records what moved; a gesture that moved nothing records nothing, and an `end` with no
/// `begin` does nothing.
class DeviceFieldsGesture {
public:
	void begin(const backplane::DeviceHost& host, DeviceInstanceId id, size_t first, size_t count) {
		device_ = id;
		first_ = first;
		before_.resize(count);
		for (size_t i = 0; i < count; ++i) {
			before_[i] = host.fieldAt(id, first + i);
		}
		open_ = true;
	}

	/// True if it recorded a step.
	bool end(backplane::DeviceHost& host, EditHistory* history, const char* verb) {
		if (!open_) {
			return false;
		}
		open_ = false;
		std::vector<DeviceFieldChange> changes;
		for (size_t i = 0; i < before_.size(); ++i) {
			const float after = host.fieldAt(device_, first_ + i);
			if (after != before_[i]) {
				changes.push_back(DeviceFieldChange { first_ + i, before_[i], after });
			}
		}
		if (history == nullptr || changes.empty()) {
			return false;
		}
		history->record(std::make_unique<DeviceFieldsEdit>(host, device_, std::move(changes), verb));
		return true;
	}

private:
	DeviceInstanceId device_ = kNoInstance;
	size_t first_ = 0;
	std::vector<float> before_;
	bool open_ = false;
};

} // namespace feathers::ui
