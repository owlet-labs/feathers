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

#pragma once

#include "plugin/ui/fader_pill.h"
#include "plugin/ui/knob.h"

#include <array>
#include <memory>
#include <string>
#include <visage/ui.h>

namespace feathers::ui {

/// **THREE FADER PILLS IN A KNOB'S CELL, WITH ONE VALUE READOUT** (backlog IL).
///
/// A pill is much smaller than a regular knob, so three fit vertically, with a value readout, in the
/// size of one knob. So this is a LAYOUT and nothing else:
/// three `FaderPill`s, each keeping every gesture, tooltip, modulation surface and undo it has on its own, and
/// under them the line a knob column prints its reading on.
///
/// **THE READOUT SAYS WHICH PILL IT IS READING** — `Width 62%`, the pill's caption and its control's own
/// `text()`. It reads the HOVERED pill, else the one last touched; before either, the stack's title, dim, so the
/// line names the group rather than picking a pill nobody asked about. One line for three controls is only
/// honest while it says whose number it is.
///
/// It is not a `ControlFace`: it writes no control. The pills are, and a walk of the tree finds them under it.
class PillStack : public visage::Frame {
public:
	static constexpr size_t kCount = 3;
	static constexpr float kPillGap = 2.0f;
	/// The readout's line — a knob column's reading line, and the gap a column sets between its lines.
	static constexpr float kReadoutHeight = Knob::kReadoutHeight;
	static constexpr float kReadoutGap = Knob::kLineGap;
	/// **THE CELL: A KNOB COLUMN AT THE ENGINE RAIL'S FACE**, the knob the cards' grids hold (`OwlttCard`'s
	/// three). The pills take what the readout leaves: 16 px each at this height.
	static constexpr float kHeight = Knob::columnHeight(Knob::kEngineRailDiameter);
	/// The narrowest cell the pills' captions and the widest readout this repo's pills print fit in.
	static constexpr float kMinWidth = 56.0f;

	/// One pill's two controls and its caption — `FaderPill`'s constructor, per pill. A null `power` is an AMOUNT-ONLY pill
	/// (`FaderPill`'s second constructor): no click, greyed by `setActive` with its section.
	struct Spec {
		device::Modulatable* amount = nullptr;
		device::Modulatable* power = nullptr;
		std::string caption;
	};

	/// Builds the three pills itself, top to bottom, so it knows each one's amount without asking the pill.
	PillStack(std::array<Spec, kCount> specs, std::string title);
	/// **THE TWO-PILL FORM** (Vowel and Partials have two amounts): the first two pills of a three-pill cell,
	/// at the same places, so a row of mixed stacks lines up; the third slot is empty.
	PillStack(std::array<Spec, 2> specs, std::string title);
	/// How many pills: 3, or 2 in the two-pill form.
	[[nodiscard]] size_t count() const noexcept { return count_; }
	/// GREY ALL OF THEM with their section (`FaderPill::setActive`).
	void setActive(bool active);

	[[nodiscard]] FaderPill& pill(size_t i) const { return *pills_[i]; }
	[[nodiscard]] const std::string& title() const noexcept { return title_; }
	/// Where pill `i` sits, in this frame's coordinates — the same arithmetic `resized` lays them out by.
	[[nodiscard]] visage::Bounds pillBounds(size_t i) const;
	/// The readout's rectangle, under the pills.
	[[nodiscard]] visage::Bounds readoutBounds() const;

	/// WHICH PILL THE READOUT READS: the hovered one, else the last touched, else -1 (the title).
	[[nodiscard]] int shown() const noexcept { return hovered_ >= 0 ? hovered_ : touched_; }
	/// WHAT THE READOUT PRINTS NOW — `"<caption> <value>"`, or the title when no pill is shown.
	[[nodiscard]] std::string readoutText() const;

	/// **REPAINT IF THE READOUT'S TEXT MOVED** — a value can move under it with no hand on it (automation, undo,
	/// a preset), so the owning card calls this from its `updateContent`. Calls `redraw()` itself and reports
	/// that it did: the report is not the delivery (`SklitterCard::updateContent`'s note). The first call after
	/// construction always repaints, since nothing has been asked for yet.
	bool refresh();

	void draw(visage::Canvas& canvas) override;
	void resized() override;

private:
	void add(size_t i, Spec spec);
	void watchPill(size_t i);

	std::array<std::unique_ptr<FaderPill>, kCount> pills_;
	/// Each pill's amount — what the readout prints.
	std::array<const device::Modulatable*, kCount> amounts_ {};
	size_t count_ = kCount;
	std::string title_;
	int hovered_ = -1;
	int touched_ = -1;
	/// The text the last repaint was asked for, so `refresh` can tell a moved value from a still one.
	std::string asked_;
};

} // namespace feathers::ui
