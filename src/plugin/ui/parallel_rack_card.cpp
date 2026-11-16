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

#include "plugin/ui/parallel_rack_card.h"

#include "plugin/ui/theme.h"
#include "plugin/ui/rack_mode_views.h"
#include "rack/rack_bridge.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace feathers::ui {

namespace pr = feathers::rack::parallel;

namespace {

constexpr const char* kOffOn[] = { "Off", "On" };

/// THE LINE AFTER THE LAST CHAIN ROW THE CARD HAS ROOM FOR: how many more the window holds. Words, not a control.
class OverflowLine final : public visage::Frame {
public:
	std::string text;
	void draw(visage::Canvas& canvas) override {
		canvas.setColor(kMuted);
		canvas.text(text, font(kKnobLabelSize), visage::Font::kLeft, 0.0f, 0.0f, static_cast<float>(width()),
		            static_cast<float>(height()));
	}
};

constexpr const char* kLevelTip = "This chain's level into the rack's sum, unity at twelve o'clock. Chains run in "
                                  "parallel, each hearing the rack's input, and are summed into the rack.";
constexpr const char* kMuteTip = "Silence this chain in the rack's sum; it keeps running";
constexpr const char* kSoloTip = "Hear only the soloed chains in the rack's sum";
// A PHANTOM'S (no chain yet: nothing to route to until the first edit makes one).
constexpr device::ParamDesc kLevelDesc { "chain_level", "Chain Level", 0.0, 1.0, composite::kFaderUnity, false,
	                                     device::Unit::Fader, {}, false, false, kLevelTip };
constexpr device::ParamDesc kMuteDesc { "chain_mute", "Chain Mute", 0.0, 1.0, 0.0, true, device::Unit::Choice, kOffOn,
	                                    false, false, kMuteTip };
constexpr device::ParamDesc kSoloDesc { "chain_solo", "Chain Solo", 0.0, 1.0, 0.0, true, device::Unit::Choice, kOffOn,
	                                    false, false, kSoloTip };
// A CHAIN WITH A SLOT (backlog NO item 2): a destination — its routes are its slot param's.
constexpr device::ParamDesc kLevelSlotDesc { "chain_level", "Chain Level", 0.0, 1.0, composite::kFaderUnity, false,
	                                         device::Unit::Fader, {}, true, false, kLevelTip };
constexpr device::ParamDesc kMuteSlotDesc { "chain_mute", "Chain Mute", 0.0, 1.0, 0.0, true, device::Unit::Choice,
	                                        kOffOn, true, false, kMuteTip };
constexpr device::ParamDesc kSoloSlotDesc { "chain_solo", "Chain Solo", 0.0, 1.0, 0.0, true, device::Unit::Choice,
	                                        kOffOn, true, false, kSoloTip };
// A CHAIN PAST THE 32ND: no slot, so not a destination — and the tooltip says why (the honesty rule).
constexpr device::ParamDesc kLevelNoSlotDesc { "chain_level", "Chain Level", 0.0, 1.0, composite::kFaderUnity, false,
	                                           device::Unit::Fader, {}, false, false, pr::kNoSlotWhy };
constexpr device::ParamDesc kMuteNoSlotDesc { "chain_mute", "Chain Mute", 0.0, 1.0, 0.0, true, device::Unit::Choice,
	                                          kOffOn, false, false, pr::kNoSlotWhy };
constexpr device::ParamDesc kSoloNoSlotDesc { "chain_solo", "Chain Solo", 0.0, 1.0, 0.0, true, device::Unit::Choice,
	                                          kOffOn, false, false, pr::kNoSlotWhy };

/// THE KEY'S CHOICE: "Any", then the 128 notes by name. A value is the key + 1, so the stored Any (-1) is the Choice's 0.
struct KeyNames {
	std::array<std::array<char, 8>, 129> text {};
	std::array<const char*, 129> names {};
	KeyNames() {
		static constexpr const char* kPitch[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
		std::snprintf(text[0].data(), text[0].size(), "Any");
		for (int k = 0; k < 128; ++k) {
			std::snprintf(text[static_cast<size_t>(k) + 1].data(), 8, "%s%d", kPitch[k % 12], k / 12 - 1);
		}
		for (size_t i = 0; i < names.size(); ++i) {
			names[i] = text[i].data();
		}
	}
};

[[nodiscard]] const device::ParamDesc& keyDesc() {
	static const KeyNames table;
	static const device::ParamDesc desc { "chain_key", "Chain Key", 0.0, 128.0, 0.0, true, device::Unit::Choice,
		                                  std::span<const char* const>(table.names), false, false,
		                                  "The note this chain answers: only that key's notes enter its devices. Any takes every note. "
		                                  "Every note still passes on through the rack" };
	return desc;
}

/// THE CHOKE GROUP'S CHOICE: None, then 1..8. A value is the group itself.
constexpr const char* kChokeNames[] = { "None", "1", "2", "3", "4", "5", "6", "7", "8" };
constexpr device::ParamDesc kChokeDesc { "chain_choke", "Chain Choke", 0.0, 8.0, 0.0, true, device::Unit::Choice, kChokeNames, false,
	                                     false, "A note this chain takes cuts off every other chain in the same group, as a closed hat "
	                                            "stops an open one. None takes part in no group" };

[[nodiscard]] pr::SlotControl slotControlOf(ChainControl::Kind kind) noexcept {
	switch (kind) {
	case ChainControl::Kind::Level: return pr::SlotControl::Level;
	case ChainControl::Kind::Mute: return pr::SlotControl::Mute;
	case ChainControl::Kind::Solo: return pr::SlotControl::Solo;
	case ChainControl::Kind::Key:
	case ChainControl::Kind::Choke: break;
	}
	return pr::SlotControl::Level;
}

} // namespace

//------------------------------------------------------------------------------------------------//
// ChainControl                                                                                     //
//------------------------------------------------------------------------------------------------//

pr::Rack* ChainControl::rack() const { return pr::rackOf(host_.containerSurfaceOf(rack_)); }

uint8_t ChainControl::slot() const {
	const pr::Rack* r = rack();
	const size_t at = r != nullptr ? r->indexOfChain(chain_) : SIZE_MAX;
	return at != SIZE_MAX ? r->slotOf(at) : pr::kNoChainSlot;
}

const device::ParamDesc& ChainControl::desc() const {
	const pr::Rack* r = rack();
	const bool real = r != nullptr && r->indexOfChain(chain_) != SIZE_MAX;
	const bool slotted = real && slot() < pr::kChainSlots;
	switch (kind_) {
	case Kind::Level: return !real ? kLevelDesc : slotted ? kLevelSlotDesc : kLevelNoSlotDesc;
	case Kind::Mute: return !real ? kMuteDesc : slotted ? kMuteSlotDesc : kMuteNoSlotDesc;
	case Kind::Solo: return !real ? kSoloDesc : slotted ? kSoloSlotDesc : kSoloNoSlotDesc;
	case Kind::Key: return keyDesc();
	case Kind::Choke: return kChokeDesc;
	}
	return kLevelDesc;
}

device::HostSlotStatus ChainControl::hostSlot() const {
	const DeviceControl* c = slotControl();
	return c != nullptr ? c->hostSlot() : device::HostSlotStatus {};
}

bool ChainControl::exposeToHost(std::string* displaced) {
	DeviceControl* c = slotControl();
	return c != nullptr && c->exposeToHost(displaced);
}

bool ChainControl::unpinFromHost() {
	DeviceControl* c = slotControl();
	return c != nullptr && c->unpinFromHost();
}

DeviceControl* ChainControl::slotControl() const {
	if (kind_ == Kind::Key || kind_ == Kind::Choke) {
		return nullptr; // neither is a destination
	}
	const uint8_t s = slot();
	if (s >= pr::kChainSlots) {
		return nullptr;
	}
	if (slotControl_ == nullptr || slotControlFor_ != s) {
		slotControl_ = std::make_unique<DeviceControl>(host_, rack_, pr::kDescriptor, pr::slotParam(s, slotControlOf(kind_)),
		                                               mod_, history_);
		slotControlFor_ = s;
	}
	return slotControl_.get();
}

device::ModDisplay ChainControl::display() const {
	DeviceControl* c = slotControl();
	if (c == nullptr) {
		device::ModDisplay out;
		out.base = value();
		out.modulated = out.base;
		out.modulatedR = out.modulated;
		return out;
	}
	return c->display();
}

float ChainControl::amountFor(device::Source source) const {
	const DeviceControl* c = slotControl();
	return c != nullptr ? c->amountFor(source) : 0.0f;
}

void ChainControl::setAmount(float amount) {
	if (DeviceControl* c = slotControl(); c != nullptr) {
		c->setAmount(amount);
	}
}

void ChainControl::beginAmountGesture() {
	if (DeviceControl* c = slotControl(); c != nullptr) {
		c->beginAmountGesture();
	}
}

void ChainControl::endAmountGesture() {
	if (DeviceControl* c = slotControl(); c != nullptr) {
		c->endAmountGesture();
	}
}

bool ChainControl::phantom() const {
	const pr::Rack* r = rack();
	return materialise_ && (r == nullptr || r->indexOfChain(chain_) == SIZE_MAX);
}

double ChainControl::value() const {
	const pr::Rack* r = rack();
	const size_t at = r != nullptr ? r->indexOfChain(chain_) : SIZE_MAX;
	if (at == SIZE_MAX) {
		return std::isnan(pending_) ? desc().def : pending_; // a phantom: its default, or what a drag holds
	}
	switch (kind_) {
	case Kind::Level: return r->level(at);
	case Kind::Mute: return r->muted(at) ? 1.0 : 0.0;
	case Kind::Solo: return r->soloed(at) ? 1.0 : 0.0;
	case Kind::Key: return static_cast<double>(r->chainKey(at) + 1);
	case Kind::Choke: return static_cast<double>(r->chainChoke(at));
	}
	return 0.0;
}

bool ChainControl::peak(float& out) const {
	const pr::Rack* r = rack();
	const size_t at = r != nullptr ? r->indexOfChain(chain_) : SIZE_MAX;
	return at != SIZE_MAX && r->chainPeak(at, out);
}

void ChainControl::apply(double value) {
	pr::Rack* r = rack();
	size_t at = r != nullptr ? r->indexOfChain(chain_) : SIZE_MAX;
	if (at == SIZE_MAX) {
		// A PHANTOM'S FIRST EDIT MAKES ITS CHAIN (§2a) — at once outside a gesture, at the gesture's end inside one.
		if (!materialise_ || r == nullptr) {
			return;
		}
		if (gesture_) {
			pending_ = value;
			return;
		}
		chain_ = materialise_();
		at = r->indexOfChain(chain_);
		if (at == SIZE_MAX) {
			return;
		}
	}
	switch (kind_) {
	case Kind::Level: r->setLevel(at, static_cast<float>(value)); break;
	case Kind::Mute: r->setMuted(at, value >= 0.5); break;
	case Kind::Solo: r->setSoloed(at, value >= 0.5); break;
	case Kind::Key: r->setChainKey(at, static_cast<int>(std::lround(value)) - 1); break;
	case Kind::Choke: r->setChainChoke(at, static_cast<int>(std::lround(value))); break;
	}
}

namespace {

/// ONE CHAIN CONTROL'S MOVE, AS AN UNDO STEP — replayed through a fresh `ChainControl`, which re-asks the host.
class ChainControlEdit final : public EditRecord {
public:
	ChainControlEdit(backplane::DeviceHost& host, DeviceInstanceId rack, uint16_t chain, ChainControl::Kind kind,
	                 double before, double after)
	    : host_(host), rack_(rack), chain_(chain), kind_(kind), before_(before), after_(after) {}
	void undo() const override { ChainControl(host_, rack_, chain_, kind_).apply(before_); }
	void redo() const override { ChainControl(host_, rack_, chain_, kind_).apply(after_); }
	const char* verb() const override {
		switch (kind_) {
		case ChainControl::Kind::Level: return "Chain level";
		case ChainControl::Kind::Mute: return "Chain mute";
		case ChainControl::Kind::Solo: return "Chain solo";
		case ChainControl::Kind::Key: return "Chain key";
		case ChainControl::Kind::Choke: return "Chain choke group";
		}
		return "Chain";
	}

private:
	backplane::DeviceHost& host_;
	DeviceInstanceId rack_;
	uint16_t chain_;
	ChainControl::Kind kind_;
	double before_;
	double after_;
};

} // namespace

void ChainControl::record(double before, double after) {
	if (history_ != nullptr && before != after) {
		history_->record(std::make_unique<ChainControlEdit>(host_, rack_, chain_, kind_, before, after));
	}
}

void ChainControl::setValue(double value) {
	const double before = this->value();
	apply(value);
	if (!gesture_) {
		record(before, this->value());
	}
}

void ChainControl::beginGesture() {
	gesture_ = true;
	before_ = value();
	// THE HOST'S HALF (no undo step: `record` makes ours), so a DAW recording automation on its slot sees the drag.
	if (DeviceControl* c = slotControl(); c != nullptr) {
		hostGesture_ = c;
		c->beginHostGesture();
	}
}

void ChainControl::endGesture() {
	if (gesture_) {
		gesture_ = false;
		if (!std::isnan(pending_)) {
			const double held = pending_;
			pending_ = std::numeric_limits<double>::quiet_NaN();
			apply(held); // the phantom's chain, made now, at the value the hand left it
		}
		record(before_, value());
		if (hostGesture_ != nullptr && hostGesture_ == slotControl_.get()) {
			hostGesture_->endHostGesture();
		}
		hostGesture_ = nullptr;
	}
}

ChainPresenceEdit::ChainPresenceEdit(backplane::DeviceHost& host, DeviceInstanceId rack, size_t index,
                                     pr::Rack::ChainSnapshot chain, bool added)
    : host_(host), rack_(rack), index_(index), chain_(std::move(chain)), added_(added) {
	if (!added_) {
		capture(chain_.slot);
	}
}

void ChainPresenceEdit::capture(uint8_t slot) const {
	routes_.clear();
	if (slot >= pr::kChainSlots) {
		return;
	}
	for (size_t c = 0; c < pr::kSlotControls; ++c) {
		const auto which = static_cast<pr::SlotControl>(c);
		const backplane::Destination dest = backplane::Destination::control(rack_, pr::slotParam(slot, which));
		std::vector<backplane::Source> sources;
		host_.forEachRouteInto(dest, &sources, [](void* ctx, backplane::Source s) {
			static_cast<std::vector<backplane::Source>*>(ctx)->push_back(s);
		});
		for (const backplane::Source& s : sources) {
			const float amount = host_.routeAmount(s, dest);
			if (amount != 0.0f) {
				routes_.push_back({ s, which, amount, host_.routeTransfer(s, dest) });
			}
		}
	}
}

void ChainPresenceEdit::put() const {
	pr::Rack* r = pr::rackOf(host_.containerSurfaceOf(rack_));
	if (r == nullptr) {
		return;
	}
	const size_t at = r->insertChain(index_, chain_);
	if (at == SIZE_MAX) {
		return;
	}
	// ITS ROUTES BACK, into whichever slot it took (its own, or another while its own awaits the host's prune).
	const uint8_t slot = r->slotOf(at);
	if (slot >= pr::kChainSlots) {
		return; // past the last slot: nowhere to put them
	}
	for (const SlotRoute& route : routes_) {
		const backplane::Destination dest = backplane::Destination::control(rack_, pr::slotParam(slot, route.which));
		host_.setRouteAmount(route.source, dest, route.amount);
		if (route.transfer != device::Transfer::Lin && host_.supportsRouteTransfers()) {
			(void)host_.setRouteTransfer(route.source, dest, route.transfer);
		}
	}
}

void ChainPresenceEdit::take() const {
	if (pr::Rack* r = pr::rackOf(host_.containerSurfaceOf(rack_)); r != nullptr) {
		if (const size_t at = r->indexOfChain(chain_.id); at != SIZE_MAX) {
			capture(r->slotOf(at)); // as they stand now: the host prunes them at its next service beat
			(void)r->removeChain(at);
		}
	}
}

std::string ChainControl::text() const {
	const double v = value();
	if (kind_ == Kind::Choke) {
		return kChokeNames[static_cast<size_t>(std::clamp(std::lround(v), 0L, 8L))];
	}
	if (kind_ == Kind::Key) {
		return keyDesc().names[static_cast<size_t>(std::clamp(std::lround(v), 0L, 128L))];
	}
	if (kind_ != Kind::Level) {
		return v >= 0.5 ? "On" : "Off";
	}
	const float g = pr::faderGain(static_cast<float>(v));
	if (g <= 0.0f) {
		return "-inf dB";
	}
	char buf[24];
	std::snprintf(buf, sizeof buf, "%+.1f dB", 20.0 * std::log10(static_cast<double>(g)));
	return buf;
}

std::string chainSummary(const rack::InnerChain* chain) {
	const rack::RackBridge* bridge = chain != nullptr ? chain->bridge() : nullptr;
	std::string out;
	if (bridge != nullptr) {
		for (size_t p = 0; p < bridge->chainLength(); ++p) {
			const device::Descriptor* d = bridge->descriptorOf(bridge->deviceAtPosition(p));
			if (d == nullptr) {
				continue;
			}
			if (!out.empty()) {
				out += " \xE2\x80\xBA "; // ›
			}
			out += d->traits.name;
		}
	}
	return out.empty() ? std::string("empty") : out;
}

//------------------------------------------------------------------------------------------------//
// ChainFader                                                                                       //
//------------------------------------------------------------------------------------------------//

ChainFader::ChainFader(ChainControl& control) : control_(control) {
	installTooltipHover(*this, *this);
	onMouseDown() = [this](const visage::MouseEvent& e) {
		if (e.repeatClickCount() >= 2) {
			// A DOUBLE-CLICK IS UNITY, one undo step.
			control_.beginGesture();
			control_.setValue(control_.desc().def);
			control_.endGesture();
			redraw();
			return;
		}
		startY_ = e.position.y;
		startValue_ = control_.value();
		dragging_ = false;
	};
	onMouseDrag() = [this](const visage::MouseEvent& e) {
		const float dy = startY_ - e.position.y; // up raises
		if (!dragging_) {
			if (std::fabs(dy) <= 3.0f) {
				return; // inside the slop a press is still a click
			}
			dragging_ = true;
			control_.beginGesture();
		}
		const device::ParamDesc& d = control_.desc();
		control_.setValue(std::clamp(startValue_ + static_cast<double>(dy / kSpan) * (d.max - d.min), d.min, d.max));
		redraw();
	};
	onMouseUp() = [this](const visage::MouseEvent&) {
		if (dragging_) {
			dragging_ = false;
			control_.endGesture();
			redraw();
		}
	};
}

void ChainFader::draw(visage::Canvas& canvas) {
	const float w = static_cast<float>(width());
	const float h = static_cast<float>(height());
	const device::ParamDesc& d = control_.desc();
	const float norm = static_cast<float>(std::clamp((control_.value() - d.min) / (d.max - d.min), 0.0, 1.0));
	canvas.setColor(withAlpha(kSurfaceRaised, 0.9f));
	canvas.roundedRectangle(0.0f, 0.0f, w, h, 3.0f);
	if (norm > 0.0f) {
		// THE FILL IS THE KNOB'S VALUE COLOUR, from the foot up.
		const float fill = std::max(norm * h, 6.0f);
		canvas.setColor(withAlpha(kValue, 0.85f));
		canvas.roundedRectangle(0.0f, h - fill, w, fill, 3.0f);
	}
	// UNITY, ticked across the track.
	const double unity = (d.def - d.min) / (d.max - d.min);
	const float uy = h - static_cast<float>(unity) * h;
	canvas.setColor(withAlpha(kVoid, 0.55f));
	canvas.segment(2.0f, uy, w - 2.0f, uy, 1.0f, true);
	canvas.setColor(withAlpha(kLine, 0.6f));
	canvas.roundedRectangleBorder(0.0f, 0.0f, w, h, 3.0f, 1.0f);
}

//------------------------------------------------------------------------------------------------//
// ChainStrip                                                                                       //
//------------------------------------------------------------------------------------------------//

ChainStrip::ChainStrip(backplane::DeviceHost& host, DeviceInstanceId rack, uint16_t chain, std::string caption,
                       std::function<void(uint16_t)> onRemove)
    : level_(host, rack, chain, ChainControl::Kind::Level), muted_(host, rack, chain, ChainControl::Kind::Mute),
      soloed_(host, rack, chain, ChainControl::Kind::Solo), keyed_(host, rack, chain, ChainControl::Kind::Key),
      choked_(host, rack, chain, ChainControl::Kind::Choke), onRemove_(std::move(onRemove)) {
	pill_ = std::make_unique<FaderPill>(level_, std::move(caption));
	fader_ = std::make_unique<ChainFader>(level_);
	mute_ = std::make_unique<SwitchChip>(muted_, "M");
	solo_ = std::make_unique<SwitchChip>(soloed_, "S");
	keyBox_ = std::make_unique<ValueBox>(keyed_, "");
	keyBox_->setCompact(true); // one row: the strip is 20 high
	chokeBox_ = std::make_unique<ValueBox>(choked_, "");
	chokeBox_->setCompact(true);
	// A CHAIN'S MUTE AND SOLO ARE DESTINATIONS (backlog NO item 2): they glow and take a route as the level's pill does.
	mute_->setShowsModulation(true);
	solo_->setShowsModulation(true);
	meter_ = std::make_unique<CardOutputMeter>();
	addChild(pill_.get());
	addChild(fader_.get());
	addChild(mute_.get());
	addChild(solo_.get());
	addChild(keyBox_.get());
	addChild(chokeBox_.get());
	addChild(meter_.get());
	fader_->setVisible(false); // the column's; `resized` shows it when the strip is a column
	meter_->setVisible(true);  // THE CARD'S STRIPS AND THE WINDOW'S BOTH METER THEIR CHAIN
	(void)refresh();
	installTooltipHover(*this, *this);
	onMouseDown() = [this](const visage::MouseEvent& e) {
		if (onRemove_ && removeBounds_.contains(e.position)) {
			onRemove_(level_.chain());
		}
	};
}

ChainStrip::~ChainStrip() = default;

float ChainStrip::summaryTextWidthForTest() const {
	const visage::Font& face = font(kKnobLabelSize);
	return face.stringWidth(std::u32string(summary_.begin(), summary_.end()));
}

void ChainStrip::setSummary(std::string summary) {
	if (summary != summary_) {
		summary_ = std::move(summary);
		resized();
		redraw();
	}
}

void ChainStrip::setLabel(std::string label) {
	if (label != label_) {
		label_ = std::move(label);
		redraw();
	}
}

bool ChainStrip::refresh() {
	bool moved = false;
	if (const std::string readout = column_ ? std::string() : level_.text(); readout != pill_->readout()) {
		pill_->setReadout(readout);
		moved = true;
	}
	float peak = 0.0f;
	const CardMeterReading now = level_.peak(peak) ? cardMeterReadingFor(peak) : CardMeterReading {};
	reading_ = now;
	// THE MARKS IN WHOLE PIXELS (`cardMeterMarksFor`): a reading that moves less than a pixel draws the same marks and asks
	// for no frame.
	moved = meter_->setMarks(cardMeterMarksFor(meterTrack_.height(), true, now)) || moved;
	if (column_) {
		fader_->redraw(); // the fader reads the control when it draws; a load or an undo moved it without a gesture
	}
	return moved;
}

void ChainStrip::resized() {
	const float w = static_cast<float>(width());
	const float h = static_cast<float>(height());
	column_ = (onRemove_ || inRail_) && w < kColumnBelow;
	pill_->setVisible(!column_);
	fader_->setVisible(column_);
	if (column_) {
		// THE CHANNEL STRIP: label, M, S, then the fader and the meter SIDE BY SIDE in what is left, the × at the foot.
		const float cx = std::round((w - kChipWidth) * 0.5f);
		const float chipH = SwitchChip::kHeight;
		float y = kLabelHeight;
		mute_->setBounds(cx, y + (kColumnRow - chipH) * 0.5f, kChipWidth, chipH);
		y += kColumnRow;
		solo_->setBounds(cx, y + (kColumnRow - chipH) * 0.5f, kChipWidth, chipH);
		y += kColumnRow;
		const float keyW = std::min(kKeyWidth, w - 2.0f);
		keyBox_->setVisible(true);
		keyBox_->setBounds(std::round((w - keyW) * 0.5f), y + (kColumnRow - ValueBox::kCompactHeight) * 0.5f, keyW, ValueBox::kCompactHeight);
		y += kColumnRow;
		chokeBox_->setVisible(true);
		chokeBox_->setBounds(std::round((w - keyW) * 0.5f), y + (kColumnRow - ValueBox::kCompactHeight) * 0.5f, keyW, ValueBox::kCompactHeight);
		y += kColumnRow + kGap;
		removeBounds_ = { cx, std::max(y, h - kColumnRow), kChipWidth, kColumnRow };
		const float bottom = removeBounds_.y() - kGap;
		const float height = std::max(0.0f, bottom - y);
		const float pair = kFaderWidth + kGap + kMeterWidth;
		const float fx = std::round((w - pair) * 0.5f);
		fader_->setBounds(fx, y, kFaderWidth, height);
		meterTrack_ = { fx + kFaderWidth + kGap, y, kMeterWidth, height };
	}
	else {
		float x = summary_.empty() ? 0.0f : std::round(w * kSummaryWidth) + kGap;
		float right = w;
		if (onRemove_) {
			removeBounds_ = { right - kChipWidth, 0.0f, kChipWidth, h };
			right -= kChipWidth + kGap;
		}
		else {
			removeBounds_ = {};
		}
		meterTrack_ = { right - kMeterWidth, 1.0f, kMeterWidth, std::max(0.0f, h - 2.0f) };
		right -= kMeterWidth + kGap;
		const float chipY = std::max(0.0f, (h - SwitchChip::kHeight) * 0.5f);
		solo_->setBounds(right - kChipWidth, chipY, kChipWidth, SwitchChip::kHeight);
		right -= kChipWidth + kGap;
		mute_->setBounds(right - kChipWidth, chipY, kChipWidth, SwitchChip::kHeight);
		right -= kChipWidth + kGap;
// THE KEY BOX ONLY WHERE THE PILL KEEPS ITS CAPTION: a card's narrow strip leaves it to the window's rail (the column has it).
		chokeBox_->setVisible(false); // the window's column carries the choke group
		const bool roomForKey = right - kKeyWidth - kGap - x >= kPillMinWidth;
		keyBox_->setVisible(roomForKey);
		if (roomForKey) {
			keyBox_->setBounds(right - kKeyWidth, std::max(0.0f, (h - ValueBox::kCompactHeight) * 0.5f), kKeyWidth, ValueBox::kCompactHeight);
			right -= kKeyWidth + kGap;
		}
		pill_->setBounds(x, 0.0f, std::max(0.0f, right - x), h);
	}
	meter_->setBounds(meterTrack_);
	(void)refresh(); // the readout belongs to the layout; the marks to the track's length
}

void ChainStrip::draw(visage::Canvas& canvas) {
	if (!summary_.empty()) {
		canvas.setColor(kMuted);
		canvas.text(summary_, font(kKnobLabelSize), visage::Font::kLeft, 0.0f, 0.0f, std::round(width() * kSummaryWidth),
		            height());
	}
	if (column_ && !label_.empty()) {
		canvas.setColor(kMuted);
		canvas.text(label_, font(kKnobLabelSize), visage::Font::kCenter, 0.0f, 0.0f, static_cast<float>(width()),
		            kLabelHeight);
	}
	// THE METER'S TRACK, dim, under the one-pixel marks the meter frame draws over it.
	canvas.setColor(withAlpha(kLine, 0.5f));
	canvas.rectangle(meterTrack_.x(), meterTrack_.y(), meterTrack_.width(), meterTrack_.height());
	if (onRemove_) {
		canvas.setColor(kMuted);
		canvas.text("\xC3\x97", font(kKnobLabelSize), visage::Font::kCenter, removeBounds_.x(), removeBounds_.y(),
		            removeBounds_.width(), removeBounds_.height()); // ×
	}
}

//------------------------------------------------------------------------------------------------//
// ParallelRackCard                                                                                 //
//------------------------------------------------------------------------------------------------//

ParallelRackCard::ParallelRackCard(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
                                   Notifier* notifier)
    : host_(host), device_(id), notifier_(notifier) {
	controls_.resize(static_cast<size_t>(pr::Param::kCount));
	model_ = std::make_unique<RackViewAdapter>(host_, device_,
	                                           RackViewAdapter::Controls { &control(pr::Param::Playing), &control(pr::Param::Mode),
	                                                                       &control(pr::Param::Position), &control(pr::Param::Gain) });
	mix_ = std::make_unique<Knob>(control(pr::Param::Mix), "Mix", notifier_);
	gain_ = std::make_unique<Knob>(control(pr::Param::Gain), "Gain", notifier_);
	// STAGE 2: THE MODE AS A DROPDOWN (a ValueBox over a Choice opens its list — never a cycling chip), and the
	// POSITION Select and Blend read, hidden in Parallel.
	modeBox_ = std::make_unique<ValueBox>(control(pr::Param::Mode), "Mode");
	addChild(modeBox_.get());
	viewSeg_ = std::make_unique<SegmentedControl>(control(pr::Param::View), registry, pr::kViewNames, notifier_);
	playingBox_ = std::make_unique<ValueBox>(control(pr::Param::Playing), "Playing");
	viewPairControl_ = std::make_unique<RackViewPairControl>(control(pr::Param::View));
	viewPair_ = std::make_unique<SegmentedControl>(*viewPairControl_, registry, RackViewPairControl::kNames, notifier_);
	addChild(viewSeg_.get());
	addChild(viewPair_.get());
	addChild(playingBox_.get());
	position_ = std::make_unique<Knob>(control(pr::Param::Position), "Position", notifier_);
	for (Knob* k : { mix_.get(), gain_.get(), position_.get() }) {
		k->setFaceDiameter(kKnobDiameter);
		addChild(k);
	}
	// WHAT A CHAIN THE MODE GIVES NO ROLE DOES (§2a): both choices on the face (step 4: no chip for a choice).
	unused_ = std::make_unique<SegmentedControl>(control(pr::Param::Unused), registry, pr::kUnusedFaceNames, notifier_);
	addChild(unused_.get());
	// BANDS (step 3): the count and the filtering are lists (dropdowns), the slope two states (a chip, lit at LR2), the
	// corners knobs — each drawn only in Bands, a corner only below the band count.
	bandsBox_ = std::make_unique<ValueBox>(control(pr::Param::BandCount), "Bands");
	filteringBox_ = std::make_unique<ValueBox>(control(pr::Param::Filtering), "Filtering");
	slope_ = std::make_unique<SegmentedControl>(control(pr::Param::Slope), registry, pr::kSlopeFaceNames, notifier_);
	for (visage::Frame* f : { static_cast<visage::Frame*>(bandsBox_.get()), static_cast<visage::Frame*>(filteringBox_.get()),
	                          static_cast<visage::Frame*>(slope_.get()) }) {
		addChild(f);
	}
	// THE CORNERS' KNOBS AND THE KEY TRACK SWITCH ARE THE WINDOW'S (with them the card was too tall in a
	// horizontal chain). The card's spectrum strip still carries every corner, and a drag on it writes the cut.
	gated_ = { { position_.get(), pr::Param::Position }, { unused_.get(), pr::Param::Unused }, { playingBox_.get(), pr::Param::Playing },
	           // THE VIEW CHOICE ITSELF (the three-way or Strips | Pads) is shown and hidden by `showForMode` below: folded away, and Tabs
	           // offered only to eight chains.
	           { bandsBox_.get(), pr::Param::BandCount }, { filteringBox_.get(), pr::Param::Filtering },
	           { slope_.get(), pr::Param::Slope } };
	overflow_ = std::make_unique<OverflowLine>();
	addChild(overflow_.get());
	overflow_->setVisible(false);
	// STEP 4'S VIEWS (§7, §7a): the spectrum strip above the chains in Bands, Mid/Side and Left/Right; the position bar
	// in Select and Blend.
	spectrum_ = std::make_unique<RackSpectrumStrip>(
	    host_, device_,
	    std::array<DeviceControl*, 4> { &control(pr::Param::Cut1), &control(pr::Param::Cut2), &control(pr::Param::Cut3),
	                                    &control(pr::Param::Cut4) },
	    control(pr::Param::Mode), control(pr::Param::BandCount));
	addChild(spectrum_.get());
	positionBar_ = std::make_unique<RackPositionBar>(host_, device_, control(pr::Param::Position), control(pr::Param::Mode));
	addChild(positionBar_.get());
	gated_.push_back({ positionBar_.get(), pr::Param::Position });
	// THE TABS AND PADS VIEWS: a bar of the chains' names, or the pads, with what is in front of the viewed chain and its arrow. Hidden
	// until the View asks for them (Strips draws exactly as it always did).
	tabs_ = std::make_unique<RackTabBar>(*model_, RackFlow::TopDown);
	pads_ = std::make_unique<RackPadsPanel>(*model_, pulse_, RackFlow::TopDown);
	front_ = std::make_unique<RackFrontColumn>(host_, device_, registry, notifier_);
	chainArrow_ = std::make_unique<RackDrawerArrow>(
	    RackFlow::TopDown, "Open the whole chain here, or close it again",
	    [this] { return model_->expanded(model_->viewedChain()); },
	    [this](bool open) {
		    model_->setExpanded(model_->viewedChain(), open);
		    (void)updateContent();
	    });
	for (visage::Frame* f : { static_cast<visage::Frame*>(tabs_.get()), static_cast<visage::Frame*>(pads_.get()),
	                          static_cast<visage::Frame*>(front_.get()), static_cast<visage::Frame*>(chainArrow_.get()) }) {
		addChild(f);
		f->setVisible(false);
	}
	(void)showForMode();
	(void)follow();

	onMouseDown() = [this](const visage::MouseEvent& e) {
		noteCardPress(e);
		if (slotMenuAt(e.position)) {
			pressSlotMenu();
		}
		else if (const auto nowOn = enablePressAt(e.position)) {
			if (notifier_ != nullptr) {
				notifier_->say(*nowOn ? "Rack on" : "Rack off");
			}
		}
	};
}

pr::Rack* ParallelRackCard::rack() const { return pr::rackOf(host_.containerSurfaceOf(device_)); }

DeviceControl& ParallelRackCard::control(pr::Param p) const {
	const auto i = static_cast<size_t>(p);
	if (controls_[i] == nullptr) {
		controls_[i] = std::make_unique<DeviceControl>(host_, device_, pr::kDescriptor, i);
	}
	return *controls_[i];
}

device::Modulatable* ParallelRackCard::enableModulatable() { return &control(pr::Param::Enabled); }

bool ParallelRackCard::headEnabled() const { return control(pr::Param::Enabled).value() >= 0.5; }

void ParallelRackCard::setModulation(ModState* mod, EditHistory* history) {
	mix_->setModulation(mod, history);
	gain_->setModulation(mod, history);
	position_->setModulation(mod, history);
	for (size_t i = 0; i < static_cast<size_t>(pr::Param::kCount); ++i) {
		control(static_cast<pr::Param>(i)).setModulation(mod, history);
	}
	history_ = history;
	mod_ = mod;
	for (auto& strip : strips_) {
		strip->setModulation(mod, history);
	}
	front_->setOuterModulation(mod, history, [this] { return device_; });
}

std::vector<pr::Param> ParallelRackCard::addressedParamsForTest() const {
	// THE CORNERS BY A GESTURE: dragged on the spectrum strip (asserted by value in "...a drag writes the cut").
	return { pr::Param::Mode,  pr::Param::Position, pr::Param::Unused, pr::Param::View, pr::Param::Playing, pr::Param::BandCount, pr::Param::Filtering,
		     pr::Param::Slope, pr::Param::Cut1,     pr::Param::Cut2,   pr::Param::Cut3,      pr::Param::Cut4,
		     pr::Param::Mix,   pr::Param::Gain };
}

float ParallelRackCard::laneHeightAt(float) const {
	if (folded()) {
		return kSlotHeadHeight;
	}
	return kSlotHeadHeight + kRowGap + bodyHeight() + kPad;
}

ParallelRackCard::Plan ParallelRackCard::plan() const {
	Plan p;
	p.mode = static_cast<pr::Mode>(std::lround(control(pr::Param::Mode).value()));
	p.bands = p.mode == pr::Mode::Bands;
	p.spectrum = (pr::kSpectrumModes & pr::kModeBit(p.mode)) != 0;
	p.bar = p.mode == pr::Mode::Select || p.mode == pr::Mode::Blend;
	const size_t roles = pr::roleCount(p.mode, strips_.size(), pr::bandCountOf(control(pr::Param::BandCount).value()));
	p.missing = (pr::kRoleModes & pr::kModeBit(p.mode)) != 0 && roles > strips_.size() ? roles - strips_.size() : 0;
	p.strips = strips_.size();
	p.view = model_->view();
	if (p.view != RackViewKind::Strips) {
		// TABS AND PADS lay out no chain rows, bands, spectrum or position bar (the window has them): what is in front of the viewed chain
		// is one tile, or all of them once it is open (as many as the unit has room for, in a horizontal lane).
		p.bands = p.spectrum = p.bar = false;
		p.missing = p.rows = p.overflow = 0;
		// WHAT IS IN FRONT is the viewed chain's REAL cards (`RackFrontColumn`): as tall as they are at the card's content width.
		p.frontExtent = static_cast<int>(std::lround(frontExtentFor(horizontalLane() ? lanesCross() : bodyWidth() - 2.0f * kPad)));
		p.arrow = front_->deviceCount() > 1;
		p.playingRow = pr::controlShown(pr::Param::Playing, control(pr::Param::Mode).value(), control(pr::Param::BandCount).value());
		return p;
	}
	// THE ROWS THE UNIT HAS ROOM FOR: what is left of `kCardUnitHeight` under everything else, a row per chain — and when
	// that is not all of them, one row fewer, for the line that says how many the window holds.
	const float row = ChainStrip::kHeight + kRowGap;
	const float rest = kCardUnitHeight - (kSlotHeadHeight + kRowGap + fixedRowsHeight(p) + Knob::kHeight + kPad);
	const auto fit = static_cast<size_t>(std::max(0.0f, std::floor(rest / row)));
	const size_t total = p.strips + p.missing;
	p.rows = total <= fit ? total : (fit > 0 ? fit - 1 : 0);
	p.overflow = total - p.rows;
	return p;
}

float ParallelRackCard::fixedRowsHeight(const Plan& p) {
	float h = 2.0f * (ValueBox::kHeight + kRowGap); // the mode row and the view row
	if (p.bands) {
		h += ValueBox::kHeight + kRowGap;
	}
	if (p.spectrum) {
		h += RackSpectrumStrip::kCardHeight + kRowGap;
	}
	if (p.bar) {
		h += RackPositionBar::kHeight + kRowGap;
	}
	return h;
}

std::string ParallelRackCard::overflowTextForTest() const {
	return static_cast<const OverflowLine*>(overflow_.get())->text;
}

float ParallelRackCard::viewHeight(const Plan& p) const {
	float h = 0.0f;
	if (p.view == RackViewKind::Tabs) {
		h += RackTabBar::kThickness;
	}
	else {
		h += pads_->naturalSize().y;
	}
	h += kRowGap;
	h += static_cast<float>(p.frontExtent) + kRowGap;
	if (p.arrow) {
		h += RackDrawerArrow::kSize + kRowGap;
	}
	h += (p.playingRow ? 3.0f : 2.0f) * (ValueBox::kHeight + kRowGap);
	return h;
}

float ParallelRackCard::bodyHeight() const {
	const Plan p = plan();
	if (p.view != RackViewKind::Strips) {
		return viewHeight(p);
	}
	const size_t rows = p.rows + (p.overflow > 0 ? 1 : 0);
	return fixedRowsHeight(p) + static_cast<float>(rows) * (ChainStrip::kHeight + kRowGap) + Knob::kHeight;
}

float ParallelRackCard::laneExtent(float cross) const {
	if (folded()) {
		return kCardHeadHeight;
	}
	// A PADS CARD IN A HORIZONTAL LANE needs its panel's width and a column beside it for what is in front and the mode and view.
	if (horizontalLane() && model_->view() != RackViewKind::Strips) {
		// LEFT TO RIGHT the card is as wide as what is in front: the spine, the bar or the pads panel, the mode and view column, the cards
		// (the first alone while the chain is closed, all of it open), their arrow and the padding. Never sized to the largest chain.
		const float lead = model_->view() == RackViewKind::Tabs ? RackTabBar::kThickness : pads_->naturalSize().x;
		const float front = frontExtentFor(std::max(0.0f, cross - 2.0f * kPad));
		const float arrow = front_->deviceCount() > 1 ? RackDrawerArrow::kSize + kRowGap : 0.0f;
		return kCardHeadHeight + lead + kRowGap + kLeftColumnWidth + kRowGap + front + arrow + kPad;
	}
	return FxCard::laneExtent(cross);
}

void ParallelRackCard::layOut() {
	const Plan p = plan();
	laidOut_ = p;
	float left = bodyLeft() + kPad;
	float wide = std::max(0.0f, bodyWidth() - 2.0f * kPad);
	float y = kSlotHeadHeight + kRowGap;
	// THE VIEW'S OWN PIECES: the bar (across the top, or down the side in a horizontal lane) or the pads panel, then what is in front of the
	// viewed chain and its arrow, all ABOVE the mode and view rows. Strips places none of them.
	const bool strips = p.view == RackViewKind::Strips;
	const RackFlow flow = horizontalLane() ? RackFlow::LeftRight : RackFlow::TopDown;
	tabs_->setFlow(flow);
	pads_->setFlow(flow);
	chainArrow_->setFlow(flow);
	const bool open = !folded();
	tabs_->setVisible(open && p.view == RackViewKind::Tabs);
	pads_->setVisible(open && p.view == RackViewKind::Pads);
	front_->setVisible(open && !strips);
	chainArrow_->setVisible(open && !strips && p.arrow);
	mix_->setVisible(open && strips);
	gain_->setVisible(open && strips);
	if (!strips) {
		front_->setHorizontal(horizontalLane());
		if (!horizontalLane()) {
			float top = y;
			if (p.view == RackViewKind::Tabs) {
				tabs_->setBounds(bodyLeft(), kSlotHeadHeight, bodyWidth(), RackTabBar::kThickness);
				top = kSlotHeadHeight + RackTabBar::kThickness + kRowGap;
			}
			else {
				const visage::Point n = pads_->naturalSize();
				pads_->setBounds(bodyLeft(), y, bodyWidth(), n.y);
				top = y + n.y + kRowGap;
			}
			// THE CARDS IN FRONT run down a column as wide as the card's content (the hairline and the cap sit in the padding beside it).
			const auto stack = static_cast<float>(p.frontExtent);
			front_->setBounds(left, top, wide, stack);
			top += stack + kRowGap;
			if (p.arrow) {
				chainArrow_->setBounds(left, top, wide, RackDrawerArrow::kSize);
				top += RackDrawerArrow::kSize + kRowGap;
			}
			y = top;
		}
		else {
			// LEFT TO RIGHT: the bar (or the pads panel) at the left, then a column of the mode and view rows, then the cards in front running
			// right with the card's height as their cross extent, and the arrow after the last of them.
			float x = bodyLeft();
			const float h = static_cast<float>(height());
			if (p.view == RackViewKind::Tabs) {
				tabs_->setBounds(x, 0.0f, RackTabBar::kThickness, h);
				x += RackTabBar::kThickness + kRowGap;
			}
			else {
				const visage::Point n = pads_->naturalSize();
				pads_->setBounds(x, 0.0f, n.x, std::min(n.y, h));
				x += n.x + kRowGap;
			}
			left = x;
			wide = kLeftColumnWidth;
			const float frontLeft = x + kLeftColumnWidth + kRowGap;
			front_->setBounds(frontLeft, kPad, static_cast<float>(p.frontExtent), std::max(0.0f, h - 2.0f * kPad));
			chainArrow_->setBounds(frontLeft + static_cast<float>(p.frontExtent) + kRowGap, 0.0f, RackDrawerArrow::kSize, h);
			y = kRowGap + kPad * 0.0f;
		}
	}
	// THE MODE ON ITS OWN ROW ABOVE THE CHAINS: it decides what the strips below mean.
	const float modeWide = std::min(wide, kModeWidth);
	modeBox_->setBounds(left, y, modeWide, ValueBox::kHeight);
	unused_->setBounds(left + modeWide + kRowGap, y + (ValueBox::kHeight - kSegmentHeight) * 0.5f,
	                   std::max(0.0f, wide - modeWide - kRowGap), kSegmentHeight);
	y += ValueBox::kHeight + kRowGap;
	// THE VIEW (no audio effect) on a row of its own, and Select's playing chain beside it where Select is the mode.
	if (!strips) {
		// TABS AND PADS: the view across the card's whole width (its three words need it), Select's playing chain on the row under it.
		viewSeg_->setBounds(left, y + (ValueBox::kHeight - kSegmentHeight) * 0.5f, wide, kSegmentHeight);
		viewPair_->setBounds(left, y + (ValueBox::kHeight - kSegmentHeight) * 0.5f, wide, kSegmentHeight);
		y += ValueBox::kHeight + kRowGap;
		playingBox_->setBounds(left, y, wide, ValueBox::kHeight);
		y += p.playingRow ? ValueBox::kHeight + kRowGap : 0.0f;
	}
	else {
		const float viewWide = std::round(wide * 0.62f);
		viewSeg_->setBounds(left, y + (ValueBox::kHeight - kSegmentHeight) * 0.5f, viewWide - kRowGap, kSegmentHeight);
		viewPair_->setBounds(left, y + (ValueBox::kHeight - kSegmentHeight) * 0.5f, viewWide - kRowGap, kSegmentHeight);
		playingBox_->setBounds(left + viewWide, y, std::max(0.0f, wide - viewWide), ValueBox::kHeight);
		y += ValueBox::kHeight + kRowGap;
	}
	if (p.bands) {
		// THE SLOPE'S TWO LABELS NEED THE ROOM: the count narrow, the filtering middling, the slope the rest.
		const float countW = std::round(wide * 0.22f);
		const float filterW = std::round(wide * 0.30f);
		bandsBox_->setBounds(left, y, countW - kRowGap, ValueBox::kHeight);
		filteringBox_->setBounds(left + countW, y, filterW - kRowGap, ValueBox::kHeight);
		slope_->setBounds(left + countW + filterW, y + (ValueBox::kHeight - kSegmentHeight) * 0.5f, wide - countW - filterW,
		                  kSegmentHeight);
		y += ValueBox::kHeight + kRowGap;
	}
	if (p.spectrum) {
		spectrum_->setBounds(left, y, wide, RackSpectrumStrip::kCardHeight);
		y += RackSpectrumStrip::kCardHeight + kRowGap;
	}
	if (p.bar) {
		positionBar_->setBounds(left, y, wide, RackPositionBar::kHeight);
		y += RackPositionBar::kHeight + kRowGap;
	}
	// THE CHAIN ROWS THE UNIT HOLDS, strips then phantoms; the rest are the window's, counted on the line after them.
	size_t row = 0;
	const auto place = [&](ChainStrip& strip) {
		const bool fits = row < p.rows;
		strip.setVisible(fits && !folded());
		if (fits) {
			strip.setBounds(left, y, wide, ChainStrip::kHeight);
			y += ChainStrip::kHeight + kRowGap;
		}
		++row;
	};
	for (auto& strip : strips_) {
		place(*strip);
	}
	// A ROLE NO CHAIN FILLS (§2a): a PHANTOM strip, an empty chain's face — role, level, M and S — whose first edit makes
	// the chain (`ChainControl::setMaterialise`).
	for (auto& phantom : phantoms_) {
		place(*phantom);
	}
	auto* line = static_cast<OverflowLine*>(overflow_.get());
	line->text = p.overflow > 0 ? "+" + std::to_string(p.overflow) + (p.overflow == 1 ? " more chain" : " more chains")
	                                  + " in the window"
	                            : std::string {};
	overflow_->setVisible(p.overflow > 0 && !folded());
	if (p.overflow > 0) {
		overflow_->setBounds(left, y, wide, ChainStrip::kHeight);
		y += ChainStrip::kHeight + kRowGap;
	}
	// POSITION, MIX, GAIN: the position's cell stays where it is when it is hidden, so Mix and Gain never jump.
	const float cell = wide / 3.0f;
	position_->setBounds(left, y, cell, Knob::kHeight);
	mix_->setBounds(left + cell, y, cell, Knob::kHeight);
	gain_->setBounds(left + 2.0f * cell, y, cell, Knob::kHeight);
	redraw();
}

uint16_t materialiseRole(backplane::DeviceHost& host, DeviceInstanceId rackId, size_t role, EditHistory* history) {
	// **ONLY ON A HAND'S EDIT, NEVER ON A MODE CHANGE** (§2a). Two reasons a mode change does not make the chains its
	// roles ask for: an empty chain in Parallel is one more copy of the input, so chains left behind would change the
	// rack's gain on the way back to Parallel; and a mode change may come from automation, where making chains is not
	// something the audio thread may do. So a missing role is a phantom until a hand touches it.
	//
	// THE ROLES ARE THE FIRST CHAINS, IN ORDER, so a phantom for band 5 over two chains is made by adding chains 3, 4 and
	// 5 — the two between are empty chains, exactly what their phantoms were — each one undo step.
	pr::Rack* r = pr::rackOf(host.containerSurfaceOf(rackId));
	if (r == nullptr) {
		return 0xFFFF;
	}
	while (r->chainCount() <= role) {
		const size_t at = r->addChain();
		if (at == SIZE_MAX) {
			return 0xFFFF;
		}
		if (history != nullptr) {
			history->record(std::make_unique<ChainPresenceEdit>(host, rackId, at, r->snapshotChain(at), true));
		}
	}
	// NOT `updateContent` here: the strip whose control is calling is still on the stack. The next tick rebuilds.
	return r->chainIdAt(role);
}

uint16_t ParallelRackCard::materialiseChain(size_t role) { return materialiseRole(host_, device_, role, history_); }

std::string ParallelRackCard::roleName(pr::Mode mode, size_t role) {
	switch (mode) {
	case pr::Mode::MidSide: return role == 0 ? "Mid" : "Side";
	case pr::Mode::LeftRight: return role == 0 ? "Left" : "Right";
	case pr::Mode::Bands: return "Band " + std::to_string(role + 1);
	default: return {};
	}
}

std::string ParallelRackCard::placeholderText(pr::Mode mode, size_t role) {
	switch (mode) {
	case pr::Mode::MidSide: return std::string("empty, passes the ") + (role == 0 ? "mid" : "side");
	case pr::Mode::LeftRight: return std::string("empty, passes the ") + (role == 0 ? "left" : "right");
	case pr::Mode::Bands: return "empty, passes its band";
	default: return {};
	}
}

void ParallelRackCard::resized() {
	layOut();
	placedExtent_ = laneHeightAt(static_cast<float>(width()));
}

void ParallelRackCard::foldedChanged() {
	const bool shown = !folded();
	mix_->setVisible(shown);
	gain_->setVisible(shown);
	modeBox_->setVisible(shown);
	(void)showForMode();
	spectrum_->setVisible(shown && laidOut_.spectrum);
	layOut(); // the chain rows and the overflow line follow the fold, each only where the unit has room for it
}

bool ParallelRackCard::follow() {
	pr::Rack* r = rack();
	std::vector<uint16_t> ids;
	if (r != nullptr) {
		for (size_t i = 0; i < r->chainCount(); ++i) {
			ids.push_back(r->chainIdAt(i));
		}
	}
	bool moved = false;
	if (ids != ids_) {
		for (auto& strip : strips_) {
			removeChild(strip.get());
		}
		strips_.clear();
		for (size_t i = 0; i < ids.size(); ++i) {
			auto strip = std::make_unique<ChainStrip>(host_, device_, ids[i], "Level");
			strip->setModulation(mod_, history_);
			strip->setVisible(!folded());
			addChild(strip.get());
			strips_.push_back(std::move(strip));
		}
		ids_ = std::move(ids);
		layOut();
		moved = true;
	}
	for (size_t i = 0; i < strips_.size(); ++i) {
		// THE ROLE THE MODE GIVES THIS CHAIN (§2a), or that it has none here.
		const Plan pl = plan();
		const size_t roles = pr::roleCount(pl.mode, strips_.size(), pr::bandCountOf(control(pr::Param::BandCount).value()));
		std::string role = roleName(pl.mode, i);
		if ((pr::kRoleModes & pr::kModeBit(pl.mode)) != 0 && i >= roles) {
			role = std::string("not used by ") + pr::kModeNames[static_cast<size_t>(pl.mode)];
		}
		std::string summary = std::to_string(i + 1) + "  " + (role.empty() ? "" : role + " · ")
		                      + chainSummary(r != nullptr ? r->chainAt(i) : nullptr);
		if (summary != strips_[i]->summaryForTest()) {
			strips_[i]->setSummary(std::move(summary));
			moved = true;
		}
		moved = strips_[i]->refresh() || moved;
	}
	// THE PHANTOMS: one per role no chain fills, each the face of an empty chain, numbered as the chain it would be.
	const Plan pl = plan();
	if (phantoms_.size() != pl.missing) {
		for (auto& phantom : phantoms_) {
			removeChild(phantom.get());
		}
		phantoms_.clear();
		for (size_t m = 0; m < pl.missing; ++m) {
			auto phantom = std::make_unique<ChainStrip>(host_, device_, uint16_t { 0xFFFF }, "Level");
			const size_t role = strips_.size() + m;
			phantom->setMaterialise([this, role] { return materialiseChain(role); });
			phantom->setModulation(mod_, history_);
			phantom->setVisible(!folded());
			addChild(phantom.get());
			phantoms_.push_back(std::move(phantom));
		}
		layOut();
		moved = true;
	}
	for (size_t m = 0; m < phantoms_.size(); ++m) {
		const size_t role = strips_.size() + m;
		// SHORT, SO IT FITS THE ROW: the role and "empty"; what passes is the hover's (`setSummaryTooltip`).
		std::string summary = std::to_string(role + 1) + "  " + roleName(pl.mode, role) + " \xC2\xB7 empty";
		phantoms_[m]->setSummaryTooltip(placeholderText(pl.mode, role));
		if (summary != phantoms_[m]->summaryForTest()) {
			phantoms_[m]->setSummary(std::move(summary));
			moved = true;
		}
		moved = phantoms_[m]->refresh() || moved;
	}
	return moved;
}

bool ParallelRackCard::showForMode() {
	const double mode = control(pr::Param::Mode).value();
	const double bands = control(pr::Param::BandCount).value();
	bool changed = false;
	if (!(plan() == laidOut_)) {
		layOut();
		changed = true;
	}
	// TABS IS OFFERED ONLY TO A RACK OF EIGHT CHAINS OR FEWER (hidden, not greyed): past it the choice is Strips | Pads, except while the view
	// IS Tabs (a load, or a ninth chain added under it), which the three-way can still show.
	const bool tabsOffered = model_->chainCount() <= kMaxRackTabs || model_->view() == RackViewKind::Tabs;
	if (viewSeg_->isVisible() != (!folded() && tabsOffered) || viewPair_->isVisible() != (!folded() && !tabsOffered)) {
		viewSeg_->setVisible(!folded() && tabsOffered);
		viewPair_->setVisible(!folded() && !tabsOffered);
		changed = true;
	}
	const bool spectrumShown = !folded() && laidOut_.spectrum;
	if (spectrum_->isVisible() != spectrumShown) {
		spectrum_->setVisible(spectrumShown);
		changed = true;
	}
	for (const auto& [frame, param] : gated_) {
		// TABS AND PADS show the rack's Playing (Select) and nothing else of what the mode adds; the window has the rest.
		const bool shown = !folded() && pr::controlShown(param, mode, bands)
		                   && (laidOut_.view == RackViewKind::Strips || frame == playingBox_.get());
		if (frame->isVisible() != shown) {
			frame->setVisible(shown);
			changed = true;
		}
	}
	return changed;
}

float ParallelRackCard::lanesCross() const {
	// THE CROSS EXTENT OF A HORIZONTAL LANE IS THE CARD'S HEIGHT less the padding the cards in front keep clear of the edge.
	const float h = height() > 0 ? static_cast<float>(height()) : kCardUnitHeight;
	return std::max(0.0f, h - 2.0f * kPad);
}

float ParallelRackCard::frontExtentFor(float cross) const {
	return front_->naturalExtent(cross);
}

uint32_t ParallelRackCard::frontTint() const {
	const uint32_t tint = instanceTintFor(device_, nest());
	return tint != 0 ? instanceEdgeColor(tint) : kAccent;
}

bool ParallelRackCard::followView() {
	if (model_->view() == RackViewKind::Strips || folded()) {
		return false;
	}
	bool moved = false;
	const size_t viewed = model_->viewedChain();
	// THE REAL CARDS OF THE VIEWED CHAIN: bound by the chain's id and bridge (a tab switch rebinds), advanced like any lane's.
	front_->setHorizontal(horizontalLane());
	front_->setNest(DeviceNest::under(nest(), device_));
	moved = front_->follow(viewed, model_->expanded(viewed)) || moved;
	moved = front_->updateCards() > 0 || moved;
	tabs_->refresh();
	chainArrow_->refresh();
	if (model_->view() == RackViewKind::Pads) {
		pads_->refresh();
		// THE STRIKE PULSE: one look a tick; no frame is told when nothing was struck (zero repaints at rest).
		moved = pulse_.poll(*model_) > 0 || moved;
	}
	return moved;
}

bool ParallelRackCard::updateContent() {
	// THE VIEW FIRST: the cards in front are bound before the plan asks how tall they are.
	bool moved = followView();
	const bool shown = showForMode();
	moved = follow() || shown || moved;
	if (!(plan() == laidOut_)) {
		layOut(); // a chain added or removed moves the phantoms
		moved = true;
	}
	moved = spectrum_->refresh() || moved;
	moved = (positionBar_->isVisible() && positionBar_->refresh()) || moved;
	// **A ROW PER CHAIN, AND THE HOST TOLD.** `bodyHeight()` already counts the rows; what was missing is anybody asking
	// for it again. Chains arrive through the body, so the lane kept the height it placed the card at and the Mix and Gain
	// knobs, laid out under the new rows, were drawn off the card's foot. Recorded before the call, so it fires once.
	const float extent = laneHeightAt(static_cast<float>(width()));
	if (placedExtent_ >= 0.0f && std::fabs(extent - placedExtent_) > 0.5f) {
		placedExtent_ = extent;
		extentChanged();
		moved = true;
	}
	if (moved) {
		redraw();
	}
	return moved;
}

void ParallelRackCard::drawHeadExtras(visage::Canvas& canvas) { drawSlotMenu(canvas, kMuted); }

void ParallelRackCard::setPresentedOn(SurfaceLayers* host) {
	presenter_ = host;
	front_->setPresentedOn(host);
}

void ParallelRackCard::drawContent(visage::Canvas& canvas) {
	// THE GROUP HAIRLINE: ONE line in the RACK's colour down the edge of the cards in front, in the padding the card already has (the member
	// cards are as wide as its content, so it adds no cross-flow space), and a thin END CAP after the last card shown.
	if (folded() || !front_->isVisible() || model_->view() == RackViewKind::Strips) {
		return;
	}
	canvas.setColor(frontTint());
	if (!horizontalLane()) {
		const float x = static_cast<float>(front_->x()) - kPad * 0.5f;
		const float top = static_cast<float>(front_->y());
		const float bottom = top + static_cast<float>(front_->height());
		canvas.fill(x, top, 1.0f, bottom - top + 2.0f);
		canvas.fill(x, bottom + 1.0f, static_cast<float>(front_->x() + front_->width()) - x, 2.0f);
	}
	else {
		// ALONG THE TOP EDGE of the cards (in the padding above them), the cap standing after the last one.
		const float y = static_cast<float>(front_->y()) - kPad * 0.5f;
		const float left = static_cast<float>(front_->x());
		const float right = left + static_cast<float>(front_->width());
		canvas.fill(left, y, right - left + 2.0f, 1.0f);
		canvas.fill(right + 1.0f, y, 2.0f, static_cast<float>(front_->y() + front_->height()) - y);
	}
}

std::string ParallelRackCard::headCaption() const {
	const size_t n = strips_.size();
	return std::to_string(n) + (n == 1 ? " chain" : " chains");
}

} // namespace feathers::ui
