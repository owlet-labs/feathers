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
#include "plugin/ui/edit_history.h"
#include "plugin/ui/mod_state.h"

// ONE AMOUNT CHANGED, FOR THE EDITOR'S ONE HISTORY.
//
// SPLIT OUT OF mod_state.h, WHICH IS NOW THE CARD FLOOR'S AND NOT THE PLUGIN'S. `ModState` answers
// which source is armed and needs nothing but a frame list and a count; this needs a HOST
// (`backplane::DeviceHost` — Feathers' bridge behind its adapter, or the rack's, rack roadmap §7f),
// because an undone amount has to be written back somewhere. Keeping the two in one header made
// every widget that asked "am I armed?" pay for the bridge, the parameter table and the CLAP SDK.
//
namespace feathers {
class ParamBridge; // `RouteEdit` names it; the knobs that record one include the bridge themselves
}

namespace feathers::ui {

/// A RECORD OVER VALUES AND THE BRIDGE, which is what edit_history.h asks of a new author: no widget is
/// captured, so pressing undo after the panel has been torn down puts the amount back rather than
/// reaching into a dead surface. It is also why this type lives here rather than in edit_history.h —
/// that file was written so a verb it has never heard of can be recorded by whoever wrote the verb.
///
/// **OVER WHICHEVER HOST WROTE THE AMOUNT** — Feathers' knobs write through `ParamBridge` directly and
/// the route list through the host contract (rack roadmap §7f); both spell the three calls the same
/// way, so one record serves both. `RouteEdit` is the bridge's; the list names the contract's.
template <typename Host>
class RouteEditOver : public EditRecord {
public:
	/// BY ENDPOINT (kill-lanes step 2) — so an undo after the device was dragged elsewhere still names it.
	RouteEditOver(Host& host, ModState& mod, backplane::Source source, backplane::Destination destination,
	              float before, float after)
	    : host_(host), mod_(mod), source_(source), destination_(destination), before_(before),
	      after_(after) {}

	void undo() const override { write(before_); }
	void redo() const override { write(after_); }
	const char* verb() const override { return "Route"; }

private:
	void write(float amount) const {
		host_.beginRouteGesture();
		host_.setRouteAmount(source_, destination_, amount);
		host_.endRouteGesture();
		// AND SAY SO, WHICH THIS DID NOT. An amount is deliberately not a parameter, so the registry
		// cannot repaint the surfaces that draw one — and the state channel does not reach them
		// either: `stateWatchers` holds the stages and the fx cards, while the knobs' rings, the mod
		// panel's route list and the rack cards are on THIS list (knob.cpp's setModulation,
		// mod_panel.cpp's constructor). `Modulatable::setAmount` is the live convention and every
		// implementation of it ends here; this record wrote through the bridge directly and told
		// nobody, so undoing a route restored the value the engine hears and left the picture behind.
		//
		// THE ModState ARRIVES AS A REFERENCE for the reason the bridge does — see the class note:
		// no widget is captured, so pressing undo after the panel has been torn down still puts the
		// amount back and still tells whatever is watching now.
		mod_.touched();
	}

	Host& host_;
	ModState& mod_;
	backplane::Source source_ {};
	backplane::Destination destination_ {};
	float before_ = 0.0f;
	float after_ = 0.0f;
};

using RouteEdit = RouteEditOver<ParamBridge>;

/// **THE SAME STEP FOR A ROUTE THAT LIVES INSIDE A CHAIN** (rack roadmap §7): the source is this host's,
/// the destination is the chain's, and the write goes through the chain's own gate
/// (`ParamBridge::setInnerRouteAmount`). Told the same way, for the same reason.
class InnerRouteEdit : public EditRecord {
public:
	InnerRouteEdit(backplane::DeviceHost& host,ModState& mod, backplane::Source source, DeviceInstanceId chain,
	               backplane::Destination inner, float before, float after)
	    : host_(host), mod_(mod), source_(source), chain_(chain), inner_(inner), before_(before), after_(after) {}

	void undo() const override { write(before_); }
	void redo() const override { write(after_); }
	const char* verb() const override { return "Route"; }

private:
	void write(float amount) const {
		(void)host_.setInnerRouteAmount(source_, chain_, inner_, amount);
		mod_.touched();
	}

	backplane::DeviceHost& host_;
	ModState& mod_;
	backplane::Source source_;
	DeviceInstanceId chain_ = kNoInstance;
	backplane::Destination inner_;
	float before_ = 0.0f;
	float after_ = 0.0f;
};

/// **A ROUTE'S CURVE CHANGED** (the transfer chips) — one step per pick from the chip's
/// menu, own route or a chain's inner one (`chain != kNoInstance`). Over the contract and values only, like
/// the amount's step, so an undo after the list has gone still reaches the route.
class RouteTransferEdit : public EditRecord {
public:
	RouteTransferEdit(backplane::DeviceHost& host, ModState& mod, backplane::Source source, DeviceInstanceId chain,
	                  backplane::Destination destination, backplane::Transfer before, backplane::Transfer after)
	    : host_(host), mod_(mod), source_(source), chain_(chain), destination_(destination), before_(before),
	      after_(after) {}

	void undo() const override { write(before_); }
	void redo() const override { write(after_); }
	const char* verb() const override { return "Route curve"; }

private:
	void write(backplane::Transfer t) const {
		if (chain_ == kNoInstance) {
			(void)host_.setRouteTransfer(source_, destination_, t);
		}
		else {
			(void)host_.setInnerRouteTransfer(source_, chain_, destination_, t);
		}
		mod_.touched();
	}

	backplane::DeviceHost& host_;
	ModState& mod_;
	backplane::Source source_;
	DeviceInstanceId chain_ = kNoInstance;
	backplane::Destination destination_;
	backplane::Transfer before_ = backplane::Transfer::Lin;
	backplane::Transfer after_ = backplane::Transfer::Lin;
};

} // namespace feathers::ui
