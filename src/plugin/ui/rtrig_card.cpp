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

#include "plugin/ui/rtrig_card.h"

#include "plugin/ui/plot_grid.h"

#include <algorithm>
#include <cmath>

namespace feathers::ui {

namespace rt = feathers::mod::rtrig;
using P = rt::Param;

namespace {

[[nodiscard]] constexpr size_t at(P p) {
	return static_cast<size_t>(p);
}

/// THE BOXES, with their captions. Jitter and Cluster share a cell: only one is live at a time.
struct BoxSpec {
	P param;
	const char* caption;
};
constexpr std::array<BoxSpec, 6> kBoxes { {
	{ P::Density, "Density" },
	{ P::Sync, "Sync" },
	{ P::Jitter, "Jitter" },
	{ P::Cluster, "Cluster" },
	{ P::Seed, "Seed" },
	{ P::Decay, "Decay" },
} };

/// THE SIZE PLOT'S SHORT NAMES — "Exponential" does not fit a third of a half card.
constexpr const char* kHitSizeShort[] = { "Fixed", "Uniform", "Exp" };
static_assert(std::size(kHitSizeShort) == std::size(rt::kHitSizeNames));

/// HOW FAR THE GAP PLOT RUNS, in mean intervals.
constexpr double kGapSpan = 3.0;

void drawFrame(visage::Canvas& canvas, visage::Bounds box) {
	canvas.setColor(kVoid);
	canvas.roundedRectangle(box.x(), box.y(), box.width(), box.height(), 4.0f);
	canvas.setColor(withAlpha(kLine, 0.3f));
	canvas.roundedRectangleBorder(box.x(), box.y(), box.width(), box.height(), 4.0f, 1.0f);
}

void drawCaption(visage::Canvas& canvas, visage::Bounds box, const char* text) {
	canvas.setColor(kMuted);
	canvas.text(text, font(kKnobLabelSize), visage::Font::kLeft, box.x() + 4.0f, box.y() + 1.0f, box.width(), 12.0f);
}

} // namespace

RandomTriggerCard::RandomTriggerCard(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
                                     Notifier* notifier, ModState* mod, EditHistory* history)
    : host_(host), device_(id), notifier_(notifier), mod_(mod), history_(history) {
	timing_ = std::make_unique<SegmentedControl>(control(P::Timing), registry, rt::kTimingNames, notifier_);
	hitSize_ = std::make_unique<SegmentedControl>(control(P::HitSize), registry, kHitSizeShort, notifier_);
	seedMode_ = std::make_unique<SegmentedControl>(control(P::SeedMode), registry, rt::kSeedModeNames, notifier_);
	for (SegmentedControl* s : { timing_.get(), hitSize_.get(), seedMode_.get() }) {
		addChild(s);
	}
	// TWO STATES EACH, SO CHIPS.
	polarity_ = std::make_unique<SwitchChip>(control(P::Polarity), "±");
	poly_ = std::make_unique<SwitchChip>(control(P::Poly), "Poly");
	addChild(polarity_.get());
	addChild(poly_.get());
	// THE TRIG PAD (backlog MV): the device's Trig control, and the fire count it lights from.
	trigPad_ = std::make_unique<TrigPad>(control(P::Trig), [this](uint32_t& fires) { return host_.trigFiresAt(device_, fires); });
	addChild(trigPad_.get());
	if (mod_ != nullptr) {
		watches_.push_back(mod_->watch(trigPad_.get())); // its ring is the mapping signal
	}
	for (const BoxSpec& b : kBoxes) {
		auto box = std::make_unique<ValueBox>(control(b.param), b.caption);
		addChild(box.get());
		boxes_[at(b.param)] = std::move(box);
	}

	onMouseDown() = [this](const visage::MouseEvent& e) {
		noteCardPress(e);
		if (slotMenuAt(e.position)) {
			pressSlotMenu();
			return;
		}
		(void)enablePressAt(e.position);
	};
	layOut();
}

RandomTriggerCard::~RandomTriggerCard() = default;

DeviceControl& RandomTriggerCard::control(P p) const {
	auto& c = controls_[at(p)];
	if (c == nullptr) {
		c = std::make_unique<DeviceControl>(host_, device_, rt::kDescriptor, at(p), mod_, history_);
	}
	return *c;
}

device::Modulatable* RandomTriggerCard::enableModulatable() {
	return &control(P::Enabled);
}

void RandomTriggerCard::setModulation(ModState* mod, EditHistory* history) {
	mod_ = mod;
	history_ = history;
	for (size_t i = 0; i < controls_.size(); ++i) {
		control(static_cast<P>(i)).setModulation(mod, history);
	}
	watches_.clear();
	if (mod_ != nullptr && trigPad_ != nullptr) {
		watches_.push_back(mod_->watch(trigPad_.get()));
	}
}

bool RandomTriggerCard::isLive(P p) const {
	return rt::liveIn(p, [this](size_t i) { return static_cast<double>(host_.fieldAt(device_, i)); });
}

uint32_t RandomTriggerCard::layoutKey() const {
	uint32_t key = 0;
	for (const int16_t gate : { rt::kSyncControl, rt::kTimingControl, rt::kSeedModeControl }) {
		const auto v = static_cast<uint32_t>(std::lround(host_.fieldAt(device_, static_cast<size_t>(gate))));
		key = key * 32u + (v & 31u);
	}
	return key;
}

float RandomTriggerCard::openHeight(size_t outputRows) {
	return kCardHeadHeight + bodyHeight() + kCardOutputStripHeight * static_cast<float>(outputRows);
}

float RandomTriggerCard::laneHeightAt(float cardWidth) const {
	return folded() ? kCardHeadHeight : kCardHeadHeight + bodyHeight() + outputStripHeightAt(cardWidth);
}

visage::Bounds RandomTriggerCard::timelineBounds() const {
	// THE TRIG PAD'S COLUMN FIRST, then the timeline it fires into.
	const float left = bodyLeft() + kPadX + kTrigColumn;
	const float inner = std::max(0.0f, bodyWidth() - 2.0f * kPadX - kTrigColumn);
	return { left, bodyTopEdge() + kGap, inner, kTimelineHeight };
}

visage::Bounds RandomTriggerCard::gapBounds() const {
	const float left = bodyLeft() + kPadX;
	const float inner = std::max(0.0f, bodyWidth() - 2.0f * kPadX);
	const float half = std::max(0.0f, (inner - kGap) * 0.5f);
	return { left, timelineBounds().bottom() + kGap, half, kDistHeight };
}

visage::Bounds RandomTriggerCard::sizeBounds() const {
	const visage::Bounds g = gapBounds();
	return { g.right() + kGap, g.y(), g.width(), kDistHeight };
}

void RandomTriggerCard::resized() {
	layOut();
}

void RandomTriggerCard::foldedChanged() {
	layOut();
}

void RandomTriggerCard::layOut() {
	laidOutKey_ = layoutKey();
	const bool open = !folded();
	const visage::Bounds gap = gapBounds();
	const visage::Bounds size = sizeBounds();
	const float left = gap.x();
	const float width = size.right() - gap.x();
	const float cell = width / 3.0f;
	const float boxH = ValueBox::kHeight;

	// UNDER THE DISTRIBUTIONS, THE CHOICES THAT SHAPE THEM — every entry on the face, each WIDE ENOUGH TO READ. They sat
	// half a row each, under their own plots, and three labels in half a row ran together ("eguliPoissCluster"): the
	// timing a whole row and the sizes the next (two cells were measured too narrow for "Uniform"). Polarity on the size
	// plot's corner, where the sizes it mirrors are drawn, and Poly on the timing plot's, which the row it left freed.
	float y = gap.bottom() + kGap;
	timing_->setBounds(left, y, width, kSwitchHeight);
	y += kSwitchHeight + kGap;
	hitSize_->setBounds(left, y, width, kSwitchHeight);
	poly_->setBounds(gap.right() - kChipWidth - 2.0f, gap.y() + 2.0f, kChipWidth, SwitchChip::kHeight);
	poly_->setVisible(open);
	polarity_->setBounds(size.right() - kChipWidth - 2.0f, size.y() + 2.0f, kChipWidth, SwitchChip::kHeight);
	timing_->setVisible(open);
	hitSize_->setVisible(open);
	polarity_->setVisible(open);
	y += kSwitchHeight + kGap;

	const auto box = [&](P p, float x, float yy, float w) {
		ValueBox& b = *boxes_[at(p)];
		b.setBounds(x, yy, w, boxH);
		b.setVisible(open && isLive(p));
	};
	// THE RATE, ITS SYNC, AND THE TIMING'S OWN AMOUNT.
	box(P::Density, left, y, cell - kGap);
	box(P::Sync, left + cell, y, cell - kGap);
	box(P::Jitter, left + 2.0f * cell, y, cell);
	box(P::Cluster, left + 2.0f * cell, y, cell);
	y += boxH + kGap;

	// THE SEED AND THE DECAY OUTPUT'S TIME — the seed's two states a cell and a half, which "Stable" needs.
	const float switchInset = 0.5f * (boxH - kSwitchHeight);
	seedMode_->setBounds(left, y + switchInset, 1.5f * cell - kGap, kSwitchHeight);
	seedMode_->setVisible(open);
	box(P::Seed, left + 1.5f * cell, y, 0.5f * cell - kGap);
	box(P::Decay, left + 2.0f * cell, y, cell);
	const visage::Bounds line = timelineBounds();
	trigPad_->setBounds(bodyLeft() + kPadX, line.y() + (line.height() - TrigPad::kDiameter) * 0.5f, TrigPad::kDiameter,
	                    TrigPad::kDiameter);
	trigPad_->setVisible(open);
	redraw();
}

bool RandomTriggerCard::sample() {
	uint32_t hits = 0;
	float phase = 0.0f;
	if (!host_.playheadAt(device_, hits, phase)) {
		return false;
	}
	float decay = 0.0f;
	(void)host_.playheadLevelAt(device_, decay);
	const float size = 2.0f * phase - 1.0f;
	// THE FIRST READING ONLY SETS THE COUNT: hits made before the card existed are not this frame's.
	const uint32_t arrived = haveCount_ ? (hits - lastHits_) & 0x7FFFFFFFu : 0u;
	haveCount_ = true;
	lastHits_ = hits;
	// THE DECAY'S SIGN IS ITS HIT'S: the magnitude is published, the newest size carries the sign.
	Column c { arrived, size, size < 0.0f ? -decay : decay };
	// EVERY HIT SINCE THE LAST FRAME, by its place in the count; the newest size alone where the host keeps none.
	if (arrived > 0) {
		std::array<device::RecentEvent, device::kRecentEvents> recent {};
		const size_t n = host_.recentEventsAt(device_, recent.data(), recent.size());
		for (size_t i = 0; i < n && c.sized < c.sizes.size(); ++i) {
			if (((hits - recent[i].ordinal) & 0x7FFFFFFFu) >= arrived) {
				break; // a hit an earlier frame drew
			}
			c.sizes[c.sized++] = recent[i].value;
		}
		if (c.sized == 0) {
			c.sizes[c.sized++] = size;
		}
	}
	quiet_ = (c.hits == 0 && c.decay == 0.0f) ? quiet_ + 1 : 0;
	ring_[head_] = c;
	head_ = (head_ + 1) % kTimelineCapacity;
	count_ = std::min(count_ + 1, kTimelineCapacity);
	return true;
}

RandomTriggerCard::Column RandomTriggerCard::timelineAtForTest(size_t back) const noexcept {
	if (back >= count_) {
		return {};
	}
	return ring_[(head_ + kTimelineCapacity - 1 - back) % kTimelineCapacity];
}

bool RandomTriggerCard::updateContent() {
	bool changed = trigPad_->poll();
	if (layoutKey() != laidOutKey_) {
		layOut();
		changed = true;
	}
	// THE DISTRIBUTIONS REPAINT WHEN WHAT SHAPES THEM MOVES.
	const auto q = [this](P p) { return static_cast<uint64_t>(std::lround(255.0f * host_.modulatedFieldAt(device_, at(p)))); };
	const uint64_t shapeKey = (q(P::Timing) << 48) ^ (q(P::Jitter) << 32) ^ (q(P::Cluster) << 16) ^ (q(P::HitSize) << 8)
	                          ^ q(P::Polarity);
	if (shapeKey != drawnShapeKey_) {
		drawnShapeKey_ = shapeKey;
		changed = true;
	}
	// ONE COLUMN A FRAME; a repaint only while the timeline holds something that moves.
	if (sample()) {
		const auto shown = static_cast<size_t>(std::max(0.0f, timelineBounds().width()));
		if (quiet_ <= std::min(shown, count_) + 1) {
			changed = true;
		}
	}
	if (changed) {
		redraw();
	}
	return changed;
}

void RandomTriggerCard::drawContent(visage::Canvas& canvas) {
	if (folded()) {
		return;
	}
	drawTimeline(canvas, timelineBounds());
	drawGap(canvas, gapBounds());
	drawSize(canvas, sizeBounds());
}

void RandomTriggerCard::drawTimeline(visage::Canvas& canvas, visage::Bounds box) const {
	drawFrame(canvas, box);
	plot::drawGrid(canvas, box);
	// BIPOLAR UNDER RANDOM POLARITY: the baseline in the middle, stems up and down. Otherwise along the foot.
	const bool bipolar = host_.fieldAt(device_, at(P::Polarity)) >= 0.5f;
	constexpr float kInset = 5.0f;
	const float base = bipolar ? box.y() + 0.5f * box.height() : box.bottom() - kInset;
	const float reach = bipolar ? 0.5f * box.height() - kInset : box.height() - 2.0f * kInset;
	const auto yOf = [&](float v) { return base - std::clamp(v, -1.0f, 1.0f) * reach; };
	canvas.setColor(withAlpha(kLine, 0.5f));
	canvas.segment(box.x() + 2.0f, base, box.right() - 2.0f, base, 1.0f, false);

	const size_t shown = std::min(count_, static_cast<size_t>(std::max(0.0f, box.width() - 4.0f)));
	const float newestX = box.right() - 3.0f;
	// THE DECAY OUTPUT, behind the stems.
	canvas.setColor(withAlpha(kModulation, 0.55f));
	for (size_t back = 1; back < shown; ++back) {
		const float x0 = newestX - static_cast<float>(back);
		canvas.segment(x0, yOf(timelineAtForTest(back).decay), x0 + 1.0f, yOf(timelineAtForTest(back - 1).decay), 1.2f,
		               true);
	}
	// THE HITS, each as tall as its size, with a dot on the tip.
	canvas.setColor(kAccent);
	for (size_t back = 0; back < shown; ++back) {
		const Column c = timelineAtForTest(back);
		if (c.hits == 0) {
			continue;
		}
		const float x = newestX - static_cast<float>(back);
		for (uint32_t k = 0; k < c.sized; ++k) {
			canvas.segment(x, base, x, yOf(c.sizes[k]), 1.5f, true);
			canvas.circle(x - 2.0f, yOf(c.sizes[k]) - 2.0f, 4.0f);
		}
	}
}

void RandomTriggerCard::drawGap(visage::Canvas& canvas, visage::Bounds box) const {
	drawFrame(canvas, box);
	drawCaption(canvas, box, "When");
	const auto timing = static_cast<dsp::EventTiming>(
	    std::clamp(static_cast<int>(std::lround(host_.fieldAt(device_, at(P::Timing)))), 0, 2));
	const float jitter = host_.modulatedFieldAt(device_, at(P::Jitter));
	const float cluster = host_.modulatedFieldAt(device_, at(P::Cluster));
	constexpr float kInset = 4.0f;
	const float foot = box.bottom() - kInset;
	const float tall = box.height() - 14.0f - kInset;
	const float x0 = box.x() + kInset;
	const float w = box.width() - 2.0f * kInset;
	const int steps = std::max(2, static_cast<int>(w));
	// NORMALISED TO ITS OWN PEAK over the span drawn: the SHAPE is what the card shows, not a scale.
	double peak = 0.0;
	for (int i = 0; i <= steps; ++i) {
		peak = std::max(peak, rt::gapDensity(timing, jitter, cluster, kGapSpan * i / steps));
	}
	// THE MEAN, a faint mark at one interval, which every timing shares.
	canvas.setColor(withAlpha(kLine, 0.5f));
	const float meanX = x0 + w / static_cast<float>(kGapSpan);
	canvas.segment(meanX, foot, meanX, foot - tall, 1.0f, false);
	canvas.setColor(kModulation);
	if (timing == dsp::EventTiming::Regular && jitter <= 0.0f) {
		canvas.segment(meanX, foot, meanX, foot - tall, 2.0f, true); // a metronome: every gap the same
		return;
	}
	if (!(peak > 0.0)) {
		return;
	}
	float px = x0;
	float py = foot - static_cast<float>(rt::gapDensity(timing, jitter, cluster, 0.0) / peak) * tall;
	for (int i = 1; i <= steps; ++i) {
		const double x = kGapSpan * i / steps;
		const float nx = x0 + w * static_cast<float>(i) / static_cast<float>(steps);
		const float ny = foot - static_cast<float>(rt::gapDensity(timing, jitter, cluster, x) / peak) * tall;
		canvas.segment(px, py, nx, ny, 1.5f, true);
		px = nx;
		py = ny;
	}
}

void RandomTriggerCard::drawSize(visage::Canvas& canvas, visage::Bounds box) const {
	drawFrame(canvas, box);
	drawCaption(canvas, box, "Size");
	const auto size = static_cast<dsp::HitSize>(
	    std::clamp(static_cast<int>(std::lround(host_.fieldAt(device_, at(P::HitSize)))), 0, 2));
	const bool bipolar = host_.fieldAt(device_, at(P::Polarity)) >= 0.5f;
	constexpr float kInset = 4.0f;
	const float foot = box.bottom() - kInset;
	const float tall = box.height() - 14.0f - kInset;
	const float x0 = box.x() + kInset;
	const float w = box.width() - 2.0f * kInset;
	// THE X AXIS IS THE HIT'S VALUE: 0..1, or -1..1 under random polarity, where each half holds half the hits.
	const float zeroX = bipolar ? x0 + 0.5f * w : x0;
	const float unit = bipolar ? 0.5f * w : w;
	const float halve = bipolar ? 0.5f : 1.0f;
	canvas.setColor(withAlpha(kLine, 0.5f));
	canvas.segment(zeroX, foot, zeroX, foot - tall, 1.0f, false);
	canvas.setColor(kModulation);
	const double peak = size == dsp::HitSize::Exponential ? 1.0 / dsp::RandomEvents::kExponentialMean : 1.0;
	const int steps = std::max(2, static_cast<int>(unit));
	for (const float sign : { 1.0f, -1.0f }) {
		if (sign < 0.0f && !bipolar) {
			break;
		}
		if (size != dsp::HitSize::Fixed) {
			float px = zeroX;
			float py = foot - halve * static_cast<float>(rt::sizeDensity(size, 1e-6) / peak) * tall;
			for (int i = 1; i < steps; ++i) {
				const double x = static_cast<double>(i) / steps;
				const float nx = zeroX + sign * unit * static_cast<float>(x);
				const float ny = foot - halve * static_cast<float>(rt::sizeDensity(size, x) / peak) * tall;
				canvas.segment(px, py, nx, ny, 1.5f, true);
				px = nx;
				py = ny;
			}
		}
		// THE MASS AT FULL SIZE, a stem as tall as its share of the hits.
		const double mass = rt::sizeMassAtOne(size);
		if (mass > 0.0) {
			const float sx = zeroX + sign * unit;
			canvas.segment(sx, foot, sx, foot - halve * static_cast<float>(mass) * tall, 2.0f, true);
		}
	}
}

bool RandomTriggerCard::headSourceFocused() const {
	if (mod_ == nullptr) {
		return false;
	}
	for (size_t out = 0; out < static_cast<size_t>(rt::Output::kCount); ++out) {
		if (mod_->source() == backplane::Source::output(device_, out)) {
			return true;
		}
	}
	return false;
}

bool RandomTriggerCard::headArmed() const {
	return headSourceFocused() && mod_->armed();
}

std::vector<P> RandomTriggerCard::addressedParamsForTest() const {
	std::vector<P> out;
	const auto add = [&](const device::Modulatable* c) {
		if (const auto* road = dynamic_cast<const DeviceControl*>(c); road != nullptr && road->device() == device_) {
			out.push_back(static_cast<P>(road->control()));
		}
	};
	for (const BoxSpec& b : kBoxes) {
		add(&boxes_[at(b.param)]->control());
	}
	add(&control(P::Timing));
	add(&control(P::HitSize));
	add(&control(P::SeedMode));
	add(&polarity_->control());
	add(&poly_->control());
	add(&trigPad_->control());
	add(&control(P::Enabled));
	return out;
}

visage::Frame* RandomTriggerCard::widgetForTest(P p) const {
	if (boxes_[at(p)] != nullptr) {
		return boxes_[at(p)].get();
	}
	switch (p) {
	case P::Timing: return timing_.get();
	case P::HitSize: return hitSize_.get();
	case P::SeedMode: return seedMode_.get();
	case P::Polarity: return polarity_.get();
	case P::Poly: return poly_.get();
	case P::Trig: return trigPad_.get();
	default: return nullptr;
	}
}

} // namespace feathers::ui
