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



#include "plugin/ui/eroder_flow.h"

#include "plugin/ui/modulated_row.h"
#include "plugin/ui/theme.h"

#include <algorithm>
#include <cctype>

namespace feathers::ui {

namespace ero = fx::eroder;

namespace {

constexpr float kLabelTracking = 0.0f;
constexpr float kLabelSize = 9.0f;
constexpr float kMargin = 12.0f;
constexpr float kBoxHeight = 20.0f;
constexpr float kStackedHeight = 18.0f;
constexpr float kStackedOffset = 11.0f;
constexpr float kSumDiameter = 16.0f;
constexpr float kPadding = 14.0f;

std::string lower(const char* s) {
	std::string out = s;
	for (char& c : out) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return out;
}

} // namespace

EroderFlow::Path EroderFlow::pathFor(const ero::Row& row) {
	const auto at = [&](ero::Param p) { return row[static_cast<size_t>(p)]; };
	const int modeAt = std::clamp(static_cast<int>(at(ero::Param::Mode) + 0.5f), 0, static_cast<int>(ero::Mode::kCount) - 1);
	const auto mode = static_cast<ero::Mode>(modeAt);
	const bool emphasised = at(ero::Param::EmphAmount) != 0.0f;
	const bool inLoop = at(ero::Param::EmphInLoop) >= 0.5f;
	const bool comb = mode == ero::Mode::Comb;
	const bool inner = comb || at(ero::Param::Resonance) != 0.0f;
	const bool outer = at(ero::Param::Loop) > 0.0f;
	const bool driven = at(ero::Param::FbTap) >= 0.5f;

	Path path;
	const auto add = [&](std::string label, bool lit, bool beneath = false) {
		path.stages.push_back({ std::move(label), lit, beneath });
		return path.stages.size() - 1;
	};
	const size_t input = add("in", true);
	// THE INPUT DRIVE, right after the input: the Sub Thru road and the dry leave the line before it.
	add("gain", at(ero::Param::Input) != 0.0f);
	if (!inLoop) {
		add("emph", emphasised);
	}
	const size_t sum = add("\xE2\x8A\x95", inner || outer);
	if (inLoop) {
		add("emph", emphasised);
	}
	add("smear", at(ero::Param::Smear) > 0.0f);
	size_t engineEnd = 0;
	if (mode == ero::Mode::Etch) {
		add("A", at(ero::Param::SlotAAmount) > 0.0f && at(ero::Param::SlotAMix) > 0.0f);
		const bool parallel = at(ero::Param::SlotRoute) >= 0.5f;
		engineEnd = add("B", at(ero::Param::SlotBAmount) > 0.0f && at(ero::Param::SlotBMix) > 0.0f, parallel);
	}
	else {
		engineEnd = add(lower(ero::kModeNames[modeAt]), true);
	}
	const size_t drive = add("drive", at(ero::Param::Drive) > 0.0f);
	// THE DC STAGE, right after the Drive: lit while it runs (the block is On and some stage that makes an offset is running).
	add("dc", ero::dcStageRuns(ero::stateOf(row, row.size()).settings));
	add("restore", emphasised && at(ero::Param::EmphRestore) >= 0.5f);
	// THE LIVE FILTER: semitones from the note while Track is on in an engine that has it, else HP Wet's hertz.
	const bool filtering = at(ero::Param::Track) >= 0.5f && !comb ? at(ero::Param::HpWetSemis) > ero::kHpWetSemisMin + 1.0e-3f
	                                                              : static_cast<double>(at(ero::Param::HpWet)) > ero::kHpWetLog2Min;
	const size_t hp = add("hp", filtering);
	add("mix", true);
	// SUB THRU GIVES BACK, AFTER HP WET, WHAT HP WET TOOK OF THE INPUT (`Eroder::processStereo`): nothing without a filter,
	// and in the Comb nothing while the dry is whole (Blend off) — the window's enable rule.
	const bool crossfades = !comb || at(ero::Param::Blend) >= 0.5f;
	path.bypass = { input, hp, at(ero::Param::SubThru) >= 0.5f && filtering && crossfades };
	path.returns.push_back({ driven ? drive : engineEnd, sum, inner, false });
	path.returns.push_back({ drive, sum, outer, true });
	return path;
}

EroderFlow::EroderFlow(backplane::DeviceHost& host, DeviceInstanceId id) : host_(host), device_(id) {
	// THE FRAME IS NOT A CONTROL: presses pass through to the chips seated in it.
	setIgnoresMouseEvents(true, true);
	(void)update();
}

void EroderFlow::seat(visage::Frame& inLoop, visage::Frame& tap, visage::Frame& restore, visage::Frame& subThru, visage::Frame& dc) {
	inLoop_ = &inLoop;
	tap_ = &tap;
	restore_ = &restore;
	subThru_ = &subThru;
	dc_ = &dc;
	for (visage::Frame* f : { inLoop_, tap_, restore_, subThru_, dc_ }) {
		addChild(f);
	}
	resized();
}

std::vector<std::string> EroderFlow::labelsForTest() const {
	std::vector<std::string> out;
	for (const Stage& s : path_.stages) {
		out.push_back(s.label);
	}
	return out;
}

EroderFlow::Anchor EroderFlow::anchorForTest(Chip chip) const { return anchors_[static_cast<size_t>(chip)]; }
EroderFlow::Rect EroderFlow::chipRectForTest(Chip chip) const { return chipRects_[static_cast<size_t>(chip)]; }

float EroderFlow::labelWidth(const std::string& text) const {
	return trackedWidth(font(kLabelSize), text, kLabelTracking);
}

size_t EroderFlow::indexOf(const char* label) const {
	for (size_t i = 0; i < path_.stages.size(); ++i) {
		if (path_.stages[i].label == label) {
			return i;
		}
	}
	return 0;
}

bool EroderFlow::update() {
	ero::Row row {};
	(void)readModulatedRow(host_, device_, ero::kDescriptor, row);
	Path next = pathFor(row);
	if (computed_ && next == path_) {
		return false;
	}
	path_ = std::move(next);
	computed_ = true;
	++recomputes_;
	resized();
	redraw();
	return true;
}

float EroderFlow::returnFromX(const Return& r) const {
	const Rect& b = boxes_[r.from];
	return b.x + b.w * 0.5f + (r.outer ? 5.0f : -5.0f);
}

float EroderFlow::returnToX(const Return& r) const {
	const Rect& b = boxes_[r.to];
	return b.x + b.w * 0.5f + (r.outer ? -4.0f : 4.0f);
}

float EroderFlow::bypassToX() const {
	// WHERE THE LOWS ARE GIVEN BACK: the arrow between HP Wet and what follows it.
	const size_t hp = path_.bypass.to;
	const Rect& b = boxes_[hp];
	const float next = hp + 1 < boxes_.size() ? boxes_[hp + 1].x : b.x + b.w + 20.0f;
	return (b.x + b.w + next) * 0.5f;
}

void EroderFlow::measure() {
	// COLUMNS: a stage beneath shares the column of the one before it.
	columns_.clear();
	std::vector<float> widths(path_.stages.size(), 0.0f);
	for (size_t i = 0; i < path_.stages.size(); ++i) {
		const bool sum = path_.stages[i].label == "\xE2\x8A\x95";
		widths[i] = sum ? kSumDiameter : std::max(24.0f, labelWidth(path_.stages[i].label) + kPadding);
		if (path_.stages[i].beneath && !columns_.empty()) {
			columns_.back().push_back(i);
		}
		else {
			columns_.push_back({ i });
		}
	}
	std::vector<float> colWidth;
	float total = 0.0f;
	for (const auto& col : columns_) {
		float w = 0.0f;
		for (const size_t i : col) {
			w = std::max(w, widths[i]);
		}
		colWidth.push_back(w);
		total += w;
	}
	const float n = static_cast<float>(columns_.size());
	const float spare = static_cast<float>(width()) - 2.0f * kMargin - total;
	const float arrow = std::clamp(n > 1.0f ? spare / (n - 1.0f) : 0.0f, 14.0f, 40.0f);
	float x = (static_cast<float>(width()) - total - arrow * (n - 1.0f)) * 0.5f;
	boxes_.assign(path_.stages.size(), {});
	for (size_t c = 0; c < columns_.size(); ++c) {
		const auto& col = columns_[c];
		for (size_t k = 0; k < col.size(); ++k) {
			const size_t i = col[k];
			const bool stacked = col.size() > 1;
			const bool sum = path_.stages[i].label == "\xE2\x8A\x95";
			const float h = sum ? kSumDiameter : stacked ? kStackedHeight : kBoxHeight;
			const float cy = kMainY + (stacked ? (k == 0 ? -kStackedOffset : kStackedOffset) : 0.0f);
			const float w = sum ? kSumDiameter : colWidth[c];
			boxes_[i] = { x + (colWidth[c] - w) * 0.5f, cy - h * 0.5f, w, h };
		}
		x += colWidth[c] + arrow;
	}
}

void EroderFlow::resized() {
	measure();
	const size_t sum = indexOf("\xE2\x8A\x95");
	const size_t emph = indexOf("emph");
	const size_t restore = indexOf("restore");
	const auto centreX = [&](size_t i) { return boxes_[i].x + boxes_[i].w * 0.5f; };
	const float w = static_cast<float>(width());
	const auto put = [&](Chip chip, float cx, float cy) {
		const float x = std::clamp(cx - kChipWidth * 0.5f, 4.0f, std::max(4.0f, w - 4.0f - kChipWidth));
		chipRects_[static_cast<size_t>(chip)] = { x, cy - kChipHeight * 0.5f, kChipWidth, kChipHeight };
	};
	// IN LOOP: above the emphasis and the sum, between the Sub Thru road and the line.
	const float inLoopX = (centreX(emph) + centreX(sum)) * 0.5f;
	put(Chip::InLoop, inLoopX, 30.0f);
	anchors_[static_cast<size_t>(Chip::InLoop)] = { emph, inLoopX, 30.0f };
	// TAP: on the inner return, left of where it leaves, and clear of the arrow into the sum.
	const Return& inner = path_.returns[0];
	const float leaveX = returnFromX(inner);
	const float tapX = std::max(leaveX - 48.0f, boxes_[sum].x + boxes_[sum].w * 0.5f + 4.0f + 6.0f + kChipWidth * 0.5f);
	put(Chip::Tap, tapX, kInnerY);
	anchors_[static_cast<size_t>(Chip::Tap)] = { inner.from, tapX, kInnerY };
	// RESTORE: under its own stage.
	put(Chip::Restore, centreX(restore), 78.0f);
	anchors_[static_cast<size_t>(Chip::Restore)] = { restore, centreX(restore), boxes_[restore].y + boxes_[restore].h };
	// DC: above its own stage (the one post high-pass the switch turns on and off), clear of the Sub Thru road along the top.
	const size_t dcStage = indexOf("dc");
	put(Chip::Dc, centreX(dcStage), 30.0f);
	anchors_[static_cast<size_t>(Chip::Dc)] = { dcStage, centreX(dcStage), 30.0f };
	// SUB THRU: on the road, half way along it.
	const float subX = (centreX(path_.bypass.from) + bypassToX()) * 0.5f;
	put(Chip::SubThru, subX, kBypassY);
	anchors_[static_cast<size_t>(Chip::SubThru)] = { path_.bypass.from, subX, kBypassY };
	if (inLoop_ == nullptr) {
		return;
	}
	visage::Frame* frames[5] = { inLoop_, tap_, restore_, subThru_, dc_ };
	for (size_t i = 0; i < 5; ++i) {
		const Rect& r = chipRects_[i];
		frames[i]->setBounds(r.x, r.y, r.w, r.h);
	}
}

void EroderFlow::draw(visage::Canvas& canvas) {
	const float w = static_cast<float>(width());
	const float h = static_cast<float>(height());
	canvas.setColor(kSurface);
	canvas.roundedRectangle(0.0f, 0.0f, w, h, 5.0f);
	canvas.setColor(withAlpha(kLine, 0.7f));
	canvas.roundedRectangleBorder(0.5f, 0.5f, w - 1.0f, h - 1.0f, 5.0f, 1.0f);
	if (boxes_.size() != path_.stages.size()) {
		return;
	}
	const visage::Font& face = font(kLabelSize);
	const float cy = kMainY;
	events_.clear();
	const auto line = [&](float x0, float y0, float x1, float y1, float t = 1.2f) {
		canvas.segment(x0, y0, x1, y1, t, true);
		if (recording_) {
			events_.push_back({ false, x0, y0, x1, y1 });
		}
	};
	const auto head = [&](float x, float y, float dx, float dy) {
		// AN ARROWHEAD AT (x, y) POINTING ALONG (dx, dy), a unit axis.
		line(x - 4.0f * dx - 2.5f * dy, y - 4.0f * dy + 2.5f * dx, x, y);
		line(x - 4.0f * dx + 2.5f * dy, y - 4.0f * dy - 2.5f * dx, x, y);
	};
	const auto roadColour = [&](bool lit, uint32_t on) { return lit ? withAlpha(on, 0.95f) : withAlpha(kMuted, 0.3f); };

	// THE MAIN LINE: an arrow from each column to the next, split and joined where a column holds two.
	for (size_t c = 0; c + 1 < columns_.size(); ++c) {
		const auto& from = columns_[c];
		const auto& to = columns_[c + 1];
		const Rect& a = boxes_[from.front()];
		const Rect& b = boxes_[to.front()];
		const float x0 = a.x + a.w;
		const float x1 = b.x;
		bool lit = true;
		for (const size_t i : from) {
			lit = lit && path_.stages[i].lit;
		}
		for (const size_t i : to) {
			lit = lit && path_.stages[i].lit;
		}
		canvas.setColor(lit ? withAlpha(kInk, 0.75f) : withAlpha(kMuted, 0.35f));
		if (from.size() == 1 && to.size() == 1) {
			line(x0, cy, x1, cy);
			head(x1, cy, 1.0f, 0.0f);
		}
		else {
			const float mid = (x0 + x1) * 0.5f;
			if (to.size() > 1) {
				line(x0, cy, mid, cy);
				line(mid, boxes_[to.front()].y + boxes_[to.front()].h * 0.5f, mid, boxes_[to.back()].y + boxes_[to.back()].h * 0.5f);
				for (const size_t i : to) {
					const float y = boxes_[i].y + boxes_[i].h * 0.5f;
					line(mid, y, x1, y);
					head(x1, y, 1.0f, 0.0f);
				}
			}
			else {
				line(mid, cy, x1, cy);
				head(x1, cy, 1.0f, 0.0f);
				line(mid, boxes_[from.front()].y + boxes_[from.front()].h * 0.5f, mid, boxes_[from.back()].y + boxes_[from.back()].h * 0.5f);
				for (const size_t i : from) {
					const float y = boxes_[i].y + boxes_[i].h * 0.5f;
					line(x0, y, mid, y);
				}
			}
		}
	}

	// THE SUB THRU ROAD OVER THE LINE: off the input, along the top, and down into the arrow after HP Wet.
	{
		const Bypass& b = path_.bypass;
		const Rect& from = boxes_[b.from];
		const float xFrom = from.x + from.w * 0.5f;
		const float xTo = bypassToX();
		canvas.setColor(roadColour(b.lit, kValue));
		line(xFrom, from.y, xFrom, kBypassY);
		line(xFrom, kBypassY, xTo, kBypassY);
		line(xTo, kBypassY, xTo, cy - 1.0f);
		head(xTo, cy - 1.0f, 0.0f, 1.0f);
	}

	// THE RETURNS UNDER THE LINE: down from the stage they leave, back along, and up into the sum.
	for (const Return& r : path_.returns) {
		const Rect& from = boxes_[r.from];
		const float y = r.outer ? kOuterY : kInnerY;
		const float xFrom = returnFromX(r);
		const float xTo = returnToX(r);
		const Rect& sum = boxes_[r.to];
		const float yTo = sum.y + sum.h - 1.0f;
		canvas.setColor(roadColour(r.lit, kAccent));
		line(xFrom, from.y + from.h, xFrom, y);
		line(xFrom, y, xTo, y);
		line(xTo, y, xTo, yTo);
		head(xTo, yTo, 0.0f, -1.0f);
		canvas.setColor(r.lit ? kAccent : withAlpha(kMuted, 0.5f));
		canvas.text(r.outer ? "loop" : "reso", face, visage::Font::kLeft, xTo + 8.0f, y + (r.outer ? 1.0f : 2.0f), 36.0f, 11.0f);
	}

	// THE CHIPS' PLATES ARE BACKED WITH THE PICTURE'S OWN GROUND, after every line and before the chips themselves: a road that
	// passes under a chip (Sub Thru's, the Tap's) must not show through it.
	for (const Rect& r : chipRects_) {
		canvas.setColor(kSurface);
		canvas.roundedRectangle(r.x, r.y, r.w, r.h, 3.0f);
		if (recording_) {
			events_.push_back({ true, r.x, r.y, r.x + r.w, r.y + r.h });
		}
	}

	// THE STAGES: a rounded box and its label; the sum a circle with a plus. EVERY BOX IS FILLED OPAQUE BEFORE IT IS STROKED.
	for (size_t i = 0; i < path_.stages.size(); ++i) {
		const Stage& s = path_.stages[i];
		const Rect& r = boxes_[i];
		const bool sum = s.label == "\xE2\x8A\x95";
		const uint32_t edge = s.lit ? withAlpha(kInk, 0.8f) : withAlpha(kMuted, 0.35f);
		if (sum) {
			canvas.setColor(kSurfaceRaised);
			canvas.circle(r.x, r.y, r.w);
			canvas.setColor(edge);
			canvas.ring(r.x, r.y, r.w, 1.2f);
			line(r.x + 4.0f, r.y + r.h * 0.5f, r.x + r.w - 4.0f, r.y + r.h * 0.5f, 1.2f);
			line(r.x + r.w * 0.5f, r.y + 4.0f, r.x + r.w * 0.5f, r.y + r.h - 4.0f, 1.2f);
			continue;
		}
		canvas.setColor(s.lit ? kSurfaceRaised : kSurface);
		canvas.roundedRectangle(r.x, r.y, r.w, r.h, 4.0f);
		canvas.setColor(edge);
		canvas.roundedRectangleBorder(r.x + 0.5f, r.y + 0.5f, r.w - 1.0f, r.h - 1.0f, 4.0f, 1.0f);
		canvas.setColor(s.lit ? kInk : withAlpha(kMuted, 0.55f));
		canvas.text(s.label, face, visage::Font::kCenter, r.x, r.y + (r.h - 11.0f) * 0.5f, r.w, 11.0f);
	}
}

} // namespace feathers::ui
