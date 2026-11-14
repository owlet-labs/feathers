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

#include "plugin/ui/pattern_expanded.h"
#include "plugin/ui/controls.h"
#include "plugin/ui/mod_route_menu.h"
#include "plugin/ui/mod_state.h"

#include "device/format_value.h"
#include "mod/arp/pattern_device.h"
#include "plugin/ui/edge_pan.h"
#include "plugin/ui/like_glyph.h"
#include "plugin/ui/pattern_likes_log.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/value_box.h"
#include "plugin/ui/switch_chip.h"
#include "plugin/ui/theme.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string_view>
#include <cmath>

namespace feathers::ui {

namespace {
namespace pt = feathers::mod::arp::pat;
namespace pn = feathers::mod::arp::pattern;
namespace gn = feathers::mod::arp::genes;
namespace likes = feathers::mod::arp::likes;
using pt::Field;

/// A TERRAIN CELL'S CYCLE IS SIXTEEN STEPS, whatever the pattern's Length — enough to read density and motion, and
/// the cost is per cell (MEASURED by the window's cost case: see `lastBuildMsForTest`).
constexpr size_t kCellSteps = 16;

float clamp01(float v) {
	return std::clamp(v, 0.0f, 1.0f);
}

uint32_t mix(uint32_t a, uint32_t b, float t) {
	const auto ch = [&](int shift) {
		const float x = static_cast<float>((a >> shift) & 0xFFu);
		const float y = static_cast<float>((b >> shift) & 0xFFu);
		return static_cast<uint32_t>(std::lround(x + (y - x) * t)) & 0xFFu;
	};
	return (ch(16) << 16) | (ch(8) << 8) | ch(0);
}

uint32_t scale(uint32_t c, float k) {
	const auto ch = [&](int shift) {
		return static_cast<uint32_t>(std::lround(std::min(255.0f, static_cast<float>((c >> shift) & 0xFFu) * k))) & 0xFFu;
	};
	return (ch(16) << 16) | (ch(8) << 8) | ch(0);
}

} // namespace

PatternFeatures featuresOf(const pn::PatternCycle& cycle, gn::Mode mode) noexcept {
	PatternFeatures f;
	if (cycle.length == 0 || !cycle.playable) {
		return f;
	}
	size_t notes = 0;
	double keySum = 0.0;
	double moveSum = 0.0;
	int moves = 0;
	int last = -1;
	size_t up = 0;
	size_t acid = 0;
	size_t tones = 0;
	int changes = 0;
	int lastDegree = cycle.steps[0].degree;
	for (size_t k = 0; k < cycle.length; ++k) {
		const pn::PatternStep& st = cycle.steps[k];
		if (st.degree != lastDegree) {
			++changes;
			lastDegree = st.degree;
		}
		if (st.kind != pn::PatternStep::Kind::Note) {
			continue;
		}
		++notes;
		const int key = cycle.keyAt(k, 0);
		keySum += key;
		if (last >= 0) {
			moveSum += std::abs(key - last);
			++moves;
		}
		last = key;
		up += st.octave > 0 ? 1 : 0;
		acid += (st.accent ? 1 : 0) + (st.slide ? 1 : 0);
		tones += static_cast<size_t>(st.count);
	}
	const auto n = static_cast<float>(cycle.length);
	f.axisB = clamp01(static_cast<float>(notes) / n);
	if (notes == 0) {
		return f;
	}
	const auto nn = static_cast<float>(notes);
	switch (mode) {
	case gn::Mode::Chords:
		f.axisA = clamp01(static_cast<float>(changes) / n * 2.0f);
		f.glow = clamp01((static_cast<float>(tones) / nn - 1.0f) / 6.0f);
		break;
	case gn::Mode::TbMono:
	case gn::Mode::TbGlide:
		f.axisA = clamp01(static_cast<float>(up) / nn * 2.0f);
		f.glow = clamp01(static_cast<float>(acid) / (2.0f * nn) * 2.0f);
		break;
	default:
		// REGISTER: the mean note against the held C4, ±two octaves; MOTION: the mean interval against a fifth.
		f.axisA = clamp01(static_cast<float>(keySum / notes - 60.0) / 48.0f + 0.5f);
		f.glow = moves > 0 ? clamp01(static_cast<float>(moveSum / moves) / 7.0f) : 0.0f;
		break;
	}
	return f;
}

uint32_t paletteOf(gn::Mode mode, const PatternFeatures& f) noexcept {
	// FOUR CORNERS PER FAMILY, (axisA, axisB) = (0,0) (1,0) (0,1) (1,1).
	static constexpr uint32_t kLine[4] = { 0x1A2350, 0x3D2A7A, 0x1F6F8B, 0x9FD8F0 };  // navy, violet, teal, ice
	static constexpr uint32_t kChords[4] = { 0x4A1520, 0x8A4A12, 0xA0365A, 0xF2C14E }; // maroon, amber, rose, gold
	static constexpr uint32_t kTb[4] = { 0x0E2A18, 0x1F6B5A, 0x5A8A12, 0xD8F04A };     // forest, teal, moss, acid
	static constexpr uint32_t kDrum[4] = { 0x2A1A12, 0x6B3A1E, 0x5A5F6B, 0xE8E2D0 };   // umber, rust, steel, bone
	const uint32_t* c = gn::decodesDrum(mode) ? kDrum : (mode == gn::Mode::Chords ? kChords : (gn::isTb(mode) ? kTb : kLine));
	const uint32_t bottom = mix(c[0], c[1], f.axisA);
	const uint32_t top = mix(c[2], c[3], f.axisA);
	return scale(mix(bottom, top, f.axisB), 0.45f + 0.75f * f.glow);
}

//------------------------------------------------------------------------------------------------//
// The terrain                                                                                    //
//------------------------------------------------------------------------------------------------//

PatternTerrain::PatternTerrain(backplane::DeviceHost& host, DeviceInstanceId id, ModState* mod, EditHistory* history)
    : host_(host), device_(id), mod_(mod), history_(history) {
	placeX_ = std::make_unique<DeviceControl>(host_, device_, pt::kDescriptor, pt::at(0, Field::PlaceX), mod_, history_);
	placeY_ = std::make_unique<DeviceControl>(host_, device_, pt::kDescriptor, pt::at(0, Field::PlaceY), mod_, history_);
	onMouseDown() = [this](const visage::MouseEvent& e) {
		// ARMED, ON THE PUCK: a route, not a move. Alt, or a press off the puck, moves it as ever.
		const device::ModDisplay dx = placeX_->display();
		const device::ModDisplay dy = placeY_->display();
		const bool mayX = !e.isAltDown() && dx.isDestination && dx.routable;
		const bool mayY = !e.isAltDown() && dy.isDestination && dy.routable;
		const visage::Point puck = puckPosition();
		if ((mayX || mayY) && std::hypot(e.position.x - puck.x, e.position.y - puck.y) <= kPuckReach) {
			if (e.repeatClickCount() == 2) {
				(void)placeX_->dropRoute(); // A DOUBLE-CLICK UNROUTES BOTH, as a knob's unroutes its one
				(void)placeY_->dropRoute();
				redraw();
				return;
			}
			routing_ = true;
			routeFrom_ = e.position;
			routeMayX_ = mayX;
			routeMayY_ = mayY;
			routeX_ = false;
			routeY_ = false;
			return;
		}
		dragging_ = true;
		dragged_ = false;
		pointer_ = e.position;
		edgeSteppedAt_ = std::chrono::steady_clock::now();
		placeX_->beginGesture();
		placeY_->beginGesture();
		moveToPointer();
	};
	onMouseDrag() = [this](const visage::MouseEvent& e) {
		if (routing_) {
			// EACH AXIS ROUTED ONLY ONCE THE DRAG MOVES ALONG IT, seeded as a knob seeds; up is more for Y.
			const auto axis = [&](DeviceControl& c, bool may, bool& started, float& anchor, float travel) {
				if (!may) {
					return;
				}
				if (!started) {
					if (std::fabs(travel) < kRouteDeadZone) {
						return;
					}
					started = true;
					c.beginAmountGesture();
					const float was = c.display().amount;
					anchor = was != 0.0f ? was : kRouteSeedAmount;
				}
				c.setAmount(std::clamp(anchor + travel / kRouteDragRange, -device::kMaxAmount, device::kMaxAmount));
			};
			axis(*placeX_, routeMayX_, routeX_, anchorX_, e.position.x - routeFrom_.x);
			axis(*placeY_, routeMayY_, routeY_, anchorY_, routeFrom_.y - e.position.y);
			redraw();
			return;
		}
		if (dragging_) {
			pointer_ = e.position;
			dragged_ = true;
			moveToPointer();
		}
	};
	onMouseUp() = [this](const visage::MouseEvent&) {
		if (routing_) {
			routing_ = false;
			if (routeX_) {
				placeX_->endAmountGesture();
			}
			if (routeY_) {
				placeY_->endAmountGesture();
			}
			redraw();
			return;
		}
		if (dragging_) {
			placeX_->endGesture();
			placeY_->endGesture();
			dragging_ = false;
		}
	};
	installTooltipHover(*this, *this);
}

void PatternTerrain::offeredControls(std::vector<Offered>& out) const {
	// A PLOT'S DRAG, as SKLITTER's perform pad offers its X and Y: the puck writes them as a side road (`ControlFace`'s
	// `Gesture`). It draws no modulation and takes no route; that is a design question, not this road's (2026-11-07).
	out.push_back({ placeX_.get(), Road::Gesture });
	out.push_back({ placeY_.get(), Road::Gesture });
}

ControlFace::ModLook PatternTerrain::modLookFor(const device::Modulatable& c) const {
	if (&c != placeX_.get() && &c != placeY_.get()) {
		return {};
	}
	const device::ModDisplay d = c.display();
	if (!d.isDestination) {
		return {};
	}
	return { true, d.armed && d.routable, !d.armed && (d.amount != 0.0f || d.routedElsewhere) };
}

void PatternTerrain::setModulation(ModState* mod, EditHistory* history) {
	mod_ = mod;
	history_ = history;
	setFocus(focus_); // the puck's controls, rebuilt over the state
	redraw();
}

visage::Point PatternTerrain::puckPosition() const {
	const auto w = static_cast<float>(width());
	const auto h = static_cast<float>(height());
	const double half = kHalfSpan / zoom_;
	return { static_cast<float>((placeX_->value() - centreX_) / (2.0 * half) + 0.5) * w,
		     h - static_cast<float>((placeY_->value() - centreY_) / (2.0 * half) + 0.5) * h };
}

void PatternTerrain::refreshWatch() {
	ModState* state = routingStateOf(*placeX_);
	if (state == watched_) {
		return;
	}
	watched_ = state;
	watch_ = state != nullptr ? state->watch(this) : Watch {};
}

void PatternTerrain::setFocus(size_t pattern) {
	focus_ = std::min(pattern, pt::kPatterns - 1);
	placeX_ = std::make_unique<DeviceControl>(host_, device_, pt::kDescriptor, pt::at(focus_, Field::PlaceX), mod_, history_);
	placeY_ = std::make_unique<DeviceControl>(host_, device_, pt::kDescriptor, pt::at(focus_, Field::PlaceY), mod_, history_);
}

void PatternTerrain::setZoom(double zoom) {
	zoom_ = std::clamp(zoom, 0.125, 64.0);
}

bool PatternTerrain::mouseWheel(const visage::MouseEvent& e) {
	if (e.wheel_delta_y == 0.0f) {
		return false;
	}
	setZoom(e.wheel_delta_y > 0.0f ? zoom_ * 1.25 : zoom_ / 1.25);
	(void)poll();
	return true;
}

const std::vector<float>& PatternTerrain::rowNow() const {
	std::vector<float>& row = rowBuf_;
	row.resize(pt::kParamCount);
	const size_t present = host_.fieldsPresentAt(device_);
	for (size_t i = 0; i < row.size(); ++i) {
		row[i] = i < present ? host_.fieldAt(device_, i) : static_cast<float>(pt::kParamDescs[i].def);
	}
	return row;
}

std::array<double, 2> PatternTerrain::placeAt(double ux, double uy) const {
	const double half = kHalfSpan / zoom_;
	return { centreX_ + (ux * 2.0 - 1.0) * half, centreY_ + (uy * 2.0 - 1.0) * half };
}

void PatternTerrain::moveTo(double ux, double uy) {
	const std::array<double, 2> at = placeAt(std::clamp(ux, 0.0, 1.0), std::clamp(uy, 0.0, 1.0));
	placeX_->setValue(std::clamp(at[0], -pt::kPlaceReach, pt::kPlaceReach));
	placeY_->setValue(std::clamp(at[1], -pt::kPlaceReach, pt::kPlaceReach));
}

void PatternTerrain::moveToPointer() {
	const auto w = static_cast<double>(std::max(1.0f, static_cast<float>(width())));
	const auto h = static_cast<double>(std::max(1.0f, static_cast<float>(height())));
	const double inset = edge_pan::kStarInsetPixels;
	const double px = std::clamp(static_cast<double>(pointer_.x), inset, std::max(inset, w - inset));
	const double py = std::clamp(static_cast<double>(pointer_.y), inset, std::max(inset, h - inset));
	moveTo(px / w, 1.0 - py / h);
}

bool PatternTerrain::edgeScroll(double seconds) {
	// ONLY A DRAG SCROLLS: a still press near the edge puts the puck there, it does not pan.
	if (!dragging_ || !dragged_ || seconds <= 0.0 || width() <= 1.0f || height() <= 1.0f) {
		return false;
	}
	const auto [vx, vy] = edge_pan::velocity(pointer_.x, pointer_.y, width(), height());
	if (vx == 0.0 && vy == 0.0) {
		return false;
	}
	// IN TRAVEL UNITS, the axes the map is drawn in — so it is the same under any Rotate (which turns the decode, not
	// the map's axes), and one view is the zoomed view.
	const double view = 2.0 * kHalfSpan / zoom_;
	centreX_ += vx * seconds * view;
	centreY_ += vy * seconds * view;
	build(rowNow(), false);
	// THE PUCK STAYS UNDER THE HAND while the ground slides — so the place walks with the view.
	moveToPointer();
	redraw();
	return true;
}

bool PatternTerrain::poll() {
	// THE EDGE PAN, on TIME while a hand is on the map — so a hand that dwells at the edge keeps going. The step is
	// capped, as the pad caps it, so a stalled frame cannot fling.
	if (dragging_) {
		const auto now = std::chrono::steady_clock::now();
		const double dt = std::min(std::chrono::duration<double>(now - edgeSteppedAt_).count(), 0.05);
		edgeSteppedAt_ = now;
		(void)edgeScroll(dt);
	}
	const std::vector<float>& row = rowNow();
	// THE KEY: the focused pattern's own block, the globals, the zoom — and NOT its place, which the view is centred
	// on only when the map is rebuilt (a drag inside the map moves the puck, not the map).
	std::vector<float>& key = keyBuf_;
	key.assign(row.begin(), row.begin() + static_cast<std::ptrdiff_t>(pt::kGlobalCount));
	key.insert(key.end(), row.begin() + static_cast<std::ptrdiff_t>(pt::at(focus_, Field::Enable)),
	           row.begin() + static_cast<std::ptrdiff_t>(pt::at(focus_, Field::Enable) + pt::kFieldCount));
	key[pt::kGlobalCount + static_cast<size_t>(Field::PlaceX)] = 0.0f;
	key[pt::kGlobalCount + static_cast<size_t>(Field::PlaceY)] = 0.0f;
	key[pt::at(pt::Global::Selector)] = 0.0f;
	// THE PUCK, as the controls hold it: a move repaints (a host's write, an undo), never rebuilds.
	const std::array<double, 2> puckKey { placeX_->value(), placeY_->value() };
	const bool puckMoved = puckKey != puckKey_;
	puckKey_ = puckKey;
	// THE LIKES ARE AN INPUT TOO: a new like re-brightens the map.
	std::vector<float>& liked = likedBuf_;
	liked.assign(likes::kBlockWords + pt::kHandWords, 0.0f);
	(void)host_.blockAt(device_, pt::kLikesBlock, std::span<float>(liked.data(), likes::kBlockWords));
	// AND THE HAND'S BLOCK (the banked turns and the pin switches) is an input as well.
	(void)host_.blockAt(device_, 1, std::span<float>(liked.data() + likes::kBlockWords, pt::kHandWords));
	if (key == key_ && zoom_ == keyZoom_ && focus_ == keyFocus_ && liked == likesKey_) {
		if (puckMoved) {
			redraw();
		}
		return puckMoved;
	}
	likesKey_ = liked;
	key_ = key;
	keyZoom_ = zoom_;
	keyFocus_ = focus_;
	build(row);
	redraw();
	return true;
}

pt::HandSlot PatternTerrain::handNow() const {
	std::vector<float> block(pt::kHandWords);
	const pt::HandState state = host_.blockAt(device_, 1, block) ? pt::handStateOf(block.data(), block.size()) : pt::HandState {};
	return state[focus_];
}

mod::arp::likes::SlotLikes PatternTerrain::likesNow() const {
	std::vector<float> block(likes::kBlockWords);
	likes::AllLikes all {};
	if (host_.blockAt(device_, pt::kLikesBlock, block) && likes::read(block.data(), block.size(), &all)) {
		return all[focus_];
	}
	return {};
}

mod::arp::likes::Fingerprint PatternTerrain::fingerprintAt(const pt::Settings& s, double x, double y) const {
	pt::PatternSettings c = s.patterns[focus_];
	c.placeX = static_cast<float>(x);
	c.placeY = static_cast<float>(y);
	c.length = kCellSteps;
	pt::Settings cell = s;
	cell.patterns[focus_] = c;
	std::array<bool, gn::kPinCount> on {};
	on = gn::kPinOnByDefault; // the switches a fresh pattern has (Ratchet and Approach opt in)
	pn::Recipe r = pt::recipeFor(cell, focus_, on);
	r.voice = false;
	mod::arp::HeldChord held;
	held.press(60, 0, 0.8f, 1);
	pn::PatternCycle cycle;
	pn::build(pt::PatternDevice::placeOf(c, handNow().residue), r, held, cycle);
	return likes::fingerprintOf(cycle, c.mode, 60);
}

bool PatternTerrain::like() {
	const std::vector<float> row = rowNow();
	const pt::Settings s = pt::settingsOf(row.data(), row.size());
	const pt::PatternSettings& q = s.patterns[focus_];
	std::vector<float> block(likes::kBlockWords);
	likes::AllLikes all {};
	if (!(host_.blockAt(device_, pt::kLikesBlock, block) && likes::read(block.data(), block.size(), &all))) {
		all = likes::AllLikes {};
	}
	likes::SlotLikes& slot = all[focus_];
	const likes::SlotLikes before = slot;
	if (slot.count == likes::kMaxLikes) {
		std::copy(slot.like.begin() + 1, slot.like.end(), slot.like.begin());
		--slot.count;
	}
	likes::Like& l = slot.like[slot.count++];
	l = likes::Like { static_cast<float>(q.zone), q.zoneOffset, static_cast<float>(q.gamma), q.placeX, q.placeY,
		              0.0f, 0.0f, q.rotate, fingerprintAt(s, q.placeX, q.placeY), {} }; // (the orbit's two, kept at 0 in the block)
	// THE FULL POINT, decoded where the fingerprint was heard (the place, on the hand's banked turns).
	pt::PatternSettings heardAt = q;
	l.point = pt::pointOf(pt::PatternDevice::placeOf(heardAt, handNow().residue));
	likes::write(all, block.data());
	const bool ok = host_.setBlock(device_, pt::kLikesBlock, block);
	// AND THE LOG (pattern_likes_log.h): a like that reached the block is a line; one that did not is not.
	if (ok) {
		(void)patternlog::logLike(static_cast<uint64_t>(device_), focus_, before, slot);
	}
	// THE HEART RETURNS AT ONCE: the map's "more like this" rebuild (a few ms) waits for the next frame's poll, and the
	// likes' warp is fitted in the background by the device (device/background.h), never here.
	redraw();
	return ok;
}

double PatternTerrain::scoreAt(double ux, double uy) const {
	const std::vector<float> row = rowNow();
	const pt::Settings s = pt::settingsOf(row.data(), row.size());
	const std::array<double, 2> at = placeAt(ux, uy);
	return likes::score(likesNow(), fingerprintAt(s, at[0], at[1]));
}

void PatternTerrain::build(const std::vector<float>& row, bool recentre) {
	const auto t0 = std::chrono::steady_clock::now();
	pt::Settings s = pt::settingsOf(row.data(), row.size());
	pt::PatternSettings q = s.patterns[focus_];
	if (recentre) {
		centreX_ = q.placeX;
		centreY_ = q.placeY;
	}
	q.length = kCellSteps;
	mod::arp::HeldChord held;
	held.press(60, 0, 0.8f, 1);
	std::array<bool, gn::kPinCount> on {};
	on = gn::kPinOnByDefault; // the switches a fresh pattern has (Ratchet and Approach opt in)
	pt::Settings cell = s;
	pn::PatternCycle cycle;
	const likes::SlotLikes slotLikes = likesNow();
	// THE PATTERN'S OWN BANKED TURNS (Rotate's fold), so the map under the puck is the pattern the device plays there.
	const pt::HandSlot hand = handNow();
	for (size_t y = 0; y < kHigh; ++y) {
		for (size_t x = 0; x < kWide; ++x) {
			// SAMPLED AT CELL CENTRES, rows bottom-up (`FieldActivityMap`'s two rules).
			const std::array<double, 2> at = placeAt((static_cast<double>(x) + 0.5) / kWide, (static_cast<double>(y) + 0.5) / kHigh);
			pt::PatternSettings c = q;
			c.placeX = static_cast<float>(at[0]);
			c.placeY = static_cast<float>(at[1]);
			cell.patterns[focus_] = c;
			pn::Recipe r = pt::recipeFor(cell, focus_, on);
			r.voice = false;
			pn::build(pt::PatternDevice::placeOf(c, hand.residue), r, held, cycle);
			base_[y * kWide + x] = paletteOf(q.mode, featuresOf(cycle, q.mode));
			// "MORE LIKE THIS": a place whose pattern resembles what this slot liked is drawn toward white.
			const double liked = slotLikes.count > 0 ? likes::score(slotLikes, likes::fingerprintOf(cycle, q.mode, 60)) : 0.0;
			cells_[y * kWide + x] = mix(base_[y * kWide + x], 0xFFFFFFu, static_cast<float>(0.55 * liked));
		}
	}
	++builds_;
	lastBuildMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void PatternTerrain::draw(visage::Canvas& canvas) {
	const auto w = static_cast<float>(width());
	const auto h = static_cast<float>(height());
	const float cw = w / static_cast<float>(kWide);
	const float ch = h / static_cast<float>(kHigh);
	for (size_t y = 0; y < kHigh; ++y) {
		for (size_t x = 0; x < kWide; ++x) {
			canvas.setColor(0xFF000000u | cells_[y * kWide + x]);
			canvas.fill(cw * static_cast<float>(x), h - ch * static_cast<float>(y + 1), cw + 0.5f, ch + 0.5f);
		}
	}
	// THE PUCK: where the focused pattern stands now (its place).
	const double half = kHalfSpan / zoom_;
	const auto toX = [&](double x) { return static_cast<float>((x - centreX_) / (2.0 * half) + 0.5) * w; };
	const auto toY = [&](double y) { return h - static_cast<float>((y - centreY_) / (2.0 * half) + 0.5) * h; };
	const float px = toX(placeX_->value());
	const float py = toY(placeY_->value());
	// THE LIKES, as hearts, before the puck so the puck stays on top. Clipped to the map, never faded.
	likeMarksDrawn_ = 0;
	canvas.setColor(withAlpha(kLikeHue, 0.92f));
	for (const pt::PlaneMark& mark : likeMarks()) {
		const float mx = toX(mark.x);
		const float my = toY(mark.y);
		if (!mark.solved || !std::isfinite(mx) || !std::isfinite(my) || mx < 0.0f || mx > w || my < 0.0f || my > h) {
			continue;
		}
		drawHeart(canvas, mx, my, kLikeMarkSize);
		++likeMarksDrawn_;
	}
	// THE PUCK'S MODULATION: armed and routable, a glow and a ring in the modulation colour; routed and not
	// armed, the ring with a tick for each routed axis — right for X, up for Y.
	refreshWatch();
	const ModLook lx = modLookFor(*placeX_);
	const ModLook ly = modLookFor(*placeY_);
	constexpr float kModRing = 9.0f;
	if (lx.armedGlow || ly.armedGlow) {
		canvas.setColor(withAlpha(kModulation, 0.35f));
		canvas.circle(px - kModRing - 2.0f, py - kModRing - 2.0f, 2.0f * (kModRing + 2.0f));
		canvas.setColor(kModulation);
		canvas.ring(px - kModRing, py - kModRing, 2.0f * kModRing, 1.5f);
	}
	else if (lx.routedMark || ly.routedMark) {
		canvas.setColor(withAlpha(kModulation, 0.9f));
		canvas.ring(px - kModRing, py - kModRing, 2.0f * kModRing, 1.0f);
		if (lx.routedMark) {
			canvas.segment(px + kModRing, py, px + kModRing + 4.0f, py, 1.5f, true);
		}
		if (ly.routedMark) {
			canvas.segment(px, py - kModRing, px, py - kModRing - 4.0f, 1.5f, true);
		}
	}
	canvas.setColor(kInk);
	canvas.ring(px - 6.0f, py - 6.0f, 12.0f, 2.0f);
}

std::vector<pt::PlaneMark> PatternTerrain::likeMarks() const {
	const likes::SlotLikes slot = likesNow();
	std::vector<pt::PlaneMark> out;
	if (slot.count == 0) {
		return out;
	}
	const std::vector<float> row = rowNow();
	const pt::PatternSettings q = pt::settingsOf(row.data(), row.size()).patterns[focus_];
	const pt::HandSlot hand = handNow();
	for (size_t i = 0; i < slot.count; ++i) {
		out.push_back(pt::markOn(q, hand.residue, slot.like[i].point));
	}
	return out;
}

PinSwitch::PinSwitch(backplane::DeviceHost& host, DeviceInstanceId id, size_t pattern, size_t pin)
    : host_(host), device_(id), pattern_(pattern), pin_(pin) {}

const device::ParamDesc& PinSwitch::desc() const {
	static constexpr const char* kNames[] = { "Off", "On" };
	static const device::ParamDesc kDesc { .key = "pattern_pin_switch", .name = "Pin", .min = 0.0, .max = 1.0, .def = 1.0,
		                                   .stepped = true, .unit = device::Unit::Choice, .names = kNames,
		                                   .tooltip = "Off: this gene at its neutral; on, the amount scales the terrain's value" };
	return kDesc;
}

double PinSwitch::value() const {
	std::vector<float> block(pt::kHandWords);
	const pt::HandState state = host_.blockAt(device_, 1, block) ? pt::handStateOf(block.data(), block.size()) : pt::HandState {};
	return state[pattern_].pinOn[pin_] ? 1.0 : 0.0;
}

void PinSwitch::setValue(double value) {
	std::vector<float> block(pt::kHandWords);
	pt::HandState state = host_.blockAt(device_, 1, block) ? pt::handStateOf(block.data(), block.size()) : pt::HandState {};
	state[pattern_].pinOn[pin_] = value >= 0.5;
	pt::writeHandState(state, block.data());
	(void)host_.setBlock(device_, 1, block);
}

LikeButton::LikeButton() {
	onMouseDown() = [this](const visage::MouseEvent&) {
		if (onLike) {
			onLike();
		}
	};
	installTooltipHover(*this, *this);
}

void LikeButton::draw(visage::Canvas& canvas) {
	const auto w = static_cast<float>(width());
	const auto h = static_cast<float>(height());
	canvas.setColor(kSurfaceRaised);
	canvas.roundedRectangle(0.0f, 0.0f, w, h, 3.0f);
	canvas.setColor(kAccent);
	canvas.text("\u2665", font(kKnobReadoutSize), visage::Font::kCenter, 0.0f, 0.0f, w, h);
}

//------------------------------------------------------------------------------------------------//
// The window                                                                                     //
//------------------------------------------------------------------------------------------------//

//------------------------------------------------------------------------------------------------//
// The step inspector                                                                             //
//------------------------------------------------------------------------------------------------//

namespace {
constexpr const char* kOffOn[] = { "Off", "On" };
constexpr device::ParamDesc switchDesc(const char* key, const char* name, const char* tooltip) {
	return { .key = key, .name = name, .min = 0.0, .max = 1.0, .def = 0.0, .stepped = true, .unit = device::Unit::Choice,
		     .names = kOffOn, .tooltip = tooltip };
}
constexpr device::ParamDesc kStepDescs[] = {
	switchDesc("pattern_step_note", "Note", "This step plays a note (on) or rests (off)"),
	{ .key = "pattern_step_pitch", .name = "Pitch", .min = 0.0, .max = 127.0, .def = 60.0, .stepped = true,
	  .unit = device::Unit::Note, .names = {}, .tooltip = "The step's pitch, in the pattern's scale (the lane shows it over a held C)" },
	{ .key = "pattern_step_velocity", .name = "Velocity", .min = 0.0, .max = 1.0, .def = 0.8, .stepped = false,
	  .unit = device::Unit::Percent, .names = {}, .tooltip = "How hard the step's note is struck" },
	{ .key = "pattern_step_gate", .name = "Gate", .min = 0.05, .max = 16.0, .def = 0.5, .stepped = false,
	  .unit = device::Unit::Number, .names = {}, .tooltip = "How long the step's note lasts, in steps" },
	switchDesc("pattern_step_accent", "Accent", "The step is accented"),
	switchDesc("pattern_step_slide", "Slide", "The step's note slides into the next"),
	switchDesc("pattern_step_tie", "Tie", "The note before holds through this step"),
	switchDesc("pattern_step_hit", "Hit", "This lane hits on this step"),
	switchDesc("pattern_step_hit_accent", "Accent", "This lane's hit on this step is accented"),
	// A CELL'S OWN LEVEL (a base velocity or modulation value per step, beside the accent switch): Drum's velocity of this lane's hit, Modulation's value from this step (held until its next). Accent
	// boosts on top of either.
	{ .key = "pattern_step_cell_velocity", .name = "Velocity", .min = 0.0, .max = 1.0, .def = 0.8, .stepped = false,
	  .unit = device::Unit::Percent, .names = {}, .tooltip = "How hard this lane hits on this step. Accent adds on top" },
	{ .key = "pattern_step_cell_value", .name = "Value", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = false,
	  .unit = device::Unit::Percent, .names = {},
	  .tooltip = "This lane's value from this step on (it holds until the lane's next value). Accent adds on top" },
};
static_assert(std::size(kStepDescs) == static_cast<size_t>(StepControl::What::kCount));
/// MODULATION'S VALUE IN A BIPOLAR TAB: the stored 0..1 shown as -1..1, as the lane's source publishes it.
constexpr device::ParamDesc kCellValueBipolar { .key = "pattern_step_cell_value", .name = "Value", .min = -1.0, .max = 1.0,
	                                             .def = -1.0, .stepped = false, .unit = device::Unit::Percent, .names = {},
	                                             .tooltip = "This lane's value from this step on (it holds until the lane's next value). Accent adds on top",
	                                             .centred = true };
using What = StepControl::What;
} // namespace

const device::ParamDesc& StepControl::desc() const {
	if (what_ == What::CellValue && lane_.modBipolar()) {
		return kCellValueBipolar;
	}
	return kStepDescs[static_cast<size_t>(what_)];
}

double StepControl::value() const {
	const pn::PatternStep* st = lane_.selectedStepNow();
	if (st == nullptr) {
		return desc().def;
	}
	const auto bit = static_cast<uint8_t>(1u << lane_.selectedLane());
	switch (what_) {
	case What::Note: return st->kind == pn::PatternStep::Kind::Note ? 1.0 : 0.0;
	case What::Pitch: return static_cast<double>(lane_.selectedKey());
	case What::Velocity: return static_cast<double>(st->velocity);
	case What::Gate: return static_cast<double>(st->gate);
	case What::Accent: return st->accent ? 1.0 : 0.0;
	case What::Slide: return st->slide ? 1.0 : 0.0;
	case What::Tie: return st->kind == pn::PatternStep::Kind::Tie ? 1.0 : 0.0;
	case What::Hit: return (st->drumHits & bit) != 0u ? 1.0 : 0.0;
	case What::HitAccent: return (st->drumAccent & bit) != 0u ? 1.0 : 0.0;
	case What::CellVelocity: return lane_.selectedCellLevel();
	case What::CellValue: return lane_.modBipolar() ? 2.0 * lane_.selectedCellLevel() - 1.0 : lane_.selectedCellLevel();
	default: return 0.0;
	}
}

void StepControl::setValue(double value) {
	const int step = lane_.selectedStep();
	if (step < 0) {
		return;
	}
	const bool on = value >= 0.5;
	const auto bit = static_cast<uint8_t>(1u << lane_.selectedLane());
	const int from = lane_.selectedKey();
	const pn::PatternCycle& terrain = lane_.terrainForTest();
	const int fallback = lane_.defaultKeyAt(static_cast<size_t>(step));
	const int pitch = what_ == What::Pitch ? lane_.snapKey(static_cast<int>(std::lround(value)), from) : from;
	lane_.editSelected([&](pn::StepEdit& ed) {
		switch (what_) {
		case What::Note:
			if (!on) {
				ed.kind = pn::PatternStep::Kind::Rest;
			}
			else if (ed.kind != pn::PatternStep::Kind::Note) {
				ed.kind = pn::PatternStep::Kind::Note;
				pn::setEditKey(ed, terrain, fallback);
			}
			break;
		case What::Pitch:
			ed.kind = pn::PatternStep::Kind::Note;
			pn::setEditKey(ed, terrain, pitch);
			break;
		case What::Velocity: ed.velocity = std::clamp(static_cast<float>(value), 0.0f, 1.0f); break;
		case What::Gate: ed.gate = std::clamp(static_cast<float>(value), 0.05f, 16.0f); break;
		case What::Accent: ed.accent = on; break;
		case What::Slide: ed.slide = on; break;
		case What::Tie: ed.kind = on ? pn::PatternStep::Kind::Tie : pn::PatternStep::Kind::Rest; break;
		case What::Hit:
			ed.drumHits = static_cast<uint8_t>(on ? (ed.drumHits | bit) : (ed.drumHits & ~bit));
			ed.drumAccent = static_cast<uint8_t>(ed.drumAccent & ed.drumHits);
			break;
		case What::HitAccent:
			ed.drumAccent = static_cast<uint8_t>(on ? (ed.drumAccent | bit) : (ed.drumAccent & ~bit));
			ed.drumHits = static_cast<uint8_t>(ed.drumHits | (ed.drumAccent & bit)); // an accent is on a hit
			break;
		case What::CellVelocity:
		case What::CellValue: {
			// STORED 0..1 (a Bipolar tab's -1..1 read back), held to the block's 16-bit step so it round-trips exactly.
			const double v = what_ == What::CellValue && lane_.modBipolar() ? 0.5 * (value + 1.0) : value;
			const size_t l = std::min(lane_.selectedLane(), pn::kDrumLanes - 1);
			ed.cellLevel[l] = pt::quantizeLevel(static_cast<float>(v));
			ed.cellSet = static_cast<uint8_t>(ed.cellSet | bit);
			break;
		}
		default: break;
		}
	});
}

std::string StepControl::text() const {
	if (what_ == What::Gate) {
		char buf[32];
		std::snprintf(buf, sizeof buf, "%.2f steps", value());
		return buf;
	}
	return device::formatValue(desc(), value());
}

StepInspector::StepInspector(PatternLane& lane) : lane_(lane) {
	for (size_t i = 0; i < controls_.size(); ++i) {
		controls_[i] = std::make_unique<StepControl>(lane_, static_cast<What>(i));
		const What w = static_cast<What>(i);
		std::unique_ptr<visage::Frame> widget;
		if (w == What::Velocity || w == What::Gate || w == What::CellVelocity || w == What::CellValue) {
			widget = std::make_unique<FaderPill>(*controls_[i], kStepDescs[i].name);
		}
		else if (w == What::Pitch) {
			auto box = std::make_unique<ValueBox>(*controls_[i], "Pitch");
			box->setCompact(true);
			widget = std::move(box);
		}
		else {
			widget = makeModulatedChip(*controls_[i], kStepDescs[i].name);
		}
		widgets_[i] = widget.get();
		addChild(widget.get());
		owned_.push_back(std::move(widget));
	}
	installTooltipHover(*this, *this);
	refresh();
}

void StepInspector::refresh() {
	const pn::PatternStep* st = lane_.selectedStepNow();
	const bool drum = lane_.drumForTest();
	const bool note = st != nullptr && st->kind == pn::PatternStep::Kind::Note;
	for (size_t i = 0; i < widgets_.size(); ++i) {
		const What w = static_cast<What>(i);
		bool shown = false;
		if (st != nullptr) {
			if (drum) {
				// THE CELL'S LEVEL: Drum's velocity where the lane hits; Modulation's value at any cell.
				const auto bit = static_cast<uint8_t>(1u << std::min(lane_.selectedLane(), pn::kDrumLanes - 1));
				const bool mod = lane_.modulationForTest();
				shown = w == What::Hit || w == What::HitAccent || (mod && w == What::CellValue)
				        || (!mod && w == What::CellVelocity && (st->drumHits & bit) != 0u);
			}
			else {
				shown = w == What::Note || w == What::Tie
				        || (note && (w == What::Pitch || w == What::Velocity || w == What::Gate || w == What::Accent
				                     || w == What::Slide));
			}
		}
		widgets_[i]->setVisible(shown);
		if (auto* pill = dynamic_cast<FaderPill*>(widgets_[i])) {
			pill->setReadout(controls_[i]->text());
		}
		widgets_[i]->redraw();
	}
	label_.clear();
	if (st != nullptr) {
		static constexpr const char* kLanes[pn::kDrumLanes] = { "Kick", "Snare", "Closed Hat", "Open Hat", "Clap",
			                                                    "Low Tom", "High Tom", "Ride" };
		label_ = "Step " + std::to_string(lane_.selectedStep() + 1);
		if (drum) {
			const size_t l = std::min(lane_.selectedLane(), pn::kDrumLanes - 1);
			label_ += std::string(" · ") + (lane_.modulationForTest() ? pn::kModLaneNames[l] : kLanes[l]);
		}
		else if (st->ratchet > 1) {
			label_ += " · ×" + std::to_string(st->ratchet); // a ratchet the terrain chose (not a hand's edit)
		}
	}
	layOut();
	redraw();
}

void StepInspector::resized() {
	layOut();
}

void StepInspector::layOut() {
	const auto w = static_cast<float>(width());
	const float labelW = 92.0f;
	const float gap = 4.0f;
	// THE FIRST ROW: the label, the switches, the pitch box.
	float x = labelW;
	const float chipW = 52.0f;
	for (const What c : { What::Note, What::Tie, What::Accent, What::Slide, What::Hit, What::HitAccent }) {
		visage::Frame* f = widgets_[static_cast<size_t>(c)];
		if (!f->isVisible()) {
			continue;
		}
		f->setBounds(x, 2.0f, chipW, SwitchChip::kHeight);
		x += chipW + gap;
	}
	widgets_[static_cast<size_t>(What::Pitch)]->setBounds(x, 2.0f, std::max(0.0f, std::min(96.0f, w - x)), ValueBox::kCompactHeight);
	// THE SECOND: the two amounts, half each.
	const float half = std::max(0.0f, (w - gap) * 0.5f);
	const float y = 2.0f + SwitchChip::kHeight + gap;
	widgets_[static_cast<size_t>(What::Velocity)]->setBounds(0.0f, y, half, PatternExpanded::kPillHeight);
	widgets_[static_cast<size_t>(What::CellVelocity)]->setBounds(0.0f, y, half, PatternExpanded::kPillHeight);
	widgets_[static_cast<size_t>(What::CellValue)]->setBounds(0.0f, y, half, PatternExpanded::kPillHeight);
	widgets_[static_cast<size_t>(What::Gate)]->setBounds(half + gap, y, half, PatternExpanded::kPillHeight);
}

void StepInspector::draw(visage::Canvas& canvas) {
	canvas.setColor(label_.empty() ? kMuted : kInk);
	const char* text = label_.empty() ? kHint : label_.c_str();
	canvas.text(text, font(kKnobLabelSize), visage::Font::kLeft, 2.0f, 2.0f, label_.empty() ? static_cast<float>(width()) : 88.0f,
	            SwitchChip::kHeight);
}

namespace {
/// A SWITCH IS AN OFF/ON CHOICE (a chip); a two-way choice of other names (Keys: Transpose/Pool) is a list, a box.
bool isSwitch(const device::ParamDesc& d) {
	return d.unit == device::Unit::Choice && d.names.size() == 2 && d.names[0] != nullptr && d.names[1] != nullptr
	       && std::string_view(d.names[0]) == "Off" && std::string_view(d.names[1]) == "On";
}
} // namespace

PatternExpanded::PatternExpanded(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id, Notifier* notifier)
    : host_(host), device_(id), notifier_(notifier) {
	(void)registry;
	controls_.resize(pt::kParamCount);
	tabs_ = std::make_unique<PatternTabs>();
	tabs_->onFocus = [this](size_t p) { setFocus(p); };
	tabs_->onMenu = [this](size_t p, visage::Point at) {
		showPatternTabMenu(host_, device_, p, history_, *tabs_, at, [this, p] { (void)exportMidi(p); });
	};
	addChild(tabs_.get());
	for (size_t p = 0; p < pt::kPatterns; ++p) {
		chips_[2 * p] = makeModulatedChip(control(pt::at(p, Field::Enable)), "E");
		chips_[2 * p + 1] = makeModulatedChip(control(pt::at(p, Field::Solo)), "S");
		chips_[2 * p + 1]->setPressOverride([this, p](const visage::MouseEvent& e) {
			if (pressPatternSolo(host_, device_, p, e.isMainModifier(), history_)) {
				setFocus(p); // SOLO FOCUSES ITS TAB
			}
			return true;
		});
		addChild(chips_[2 * p].get());
		addChild(chips_[2 * p + 1].get());
		cycleRepeats_[p] = std::make_unique<ValueBox>(control(pt::at(p, Field::Repeats)), "\u00d7"); // "× 2": how many times through
		cycleRepeats_[p]->setCompact(true);
		cycleNext_[p] = std::make_unique<ValueBox>(control(pt::at(p, Field::Next)), "");
		cycleNext_[p]->setCompact(true);
		addChild(cycleRepeats_[p].get());
		addChild(cycleNext_[p].get());
		cycleSpan_[p] = std::make_unique<ValueBox>(control(pt::at(p, Field::Span)), "");
		cycleSpan_[p]->setCompact(true);
		addChild(cycleSpan_[p].get());
		fillChips_[p] = makeModulatedChip(control(pt::at(p, Field::Fill)), "F");
		addChild(fillChips_[p].get());
		goChips_[p] = makeModulatedChip(control(pt::at(p, Field::FillTrigger)), "Go");
		goChips_[p]->setBehaviour(SwitchChip::Behaviour::Momentary);
		addChild(goChips_[p].get());
		midiGrips_[p] = std::make_unique<MidiGrip>(host_, device_, p);
		addChild(midiGrips_[p].get());
	}
	terrain_ = std::make_unique<PatternTerrain>(host, id, nullptr, nullptr);
	addChild(terrain_.get());
	lane_ = std::make_unique<PatternLane>(host, id);
	// THE WINDOW'S LANE IS EDITABLE BY HAND (design §15-bis); the card's is a view.
	lane_->setEditable(true, nullptr);
	addChild(lane_.get());
	// THE STEP INSPECTOR, beside the lane: the selected step's settings.
	inspector_ = std::make_unique<StepInspector>(*lane_);
	addChild(inspector_.get());
	lane_->onSelect = [this] { inspector_->refresh(); };
	heart_ = std::make_unique<LikeButton>();
	heart_->onLike = [this] { (void)terrain_->like(); };
	addChild(heart_.get());

	// THE DEVICE'S OWN. The selector stays a knob (it reads better as one); Humanize is SKLITTER's zone cell — the feel's
	// names on a ZoneSelect, its Amount the box beside them (`ModeValueCell`, the groove's precedent: two params, one look);
	// two-state choices are chips; other choices compact boxes; the rest a slider bank.
	selector_ = std::make_unique<Knob>(control(pt::at(pt::Global::Selector)), "Pattern", notifier_);
	addChild(selector_.get());
	{
		const size_t mode = pt::at(pt::Global::HumanizeMode);
		std::vector<ZoneSelect::ZoneName> names;
		for (const char* n : pt::kParamDescs[mode].names) {
			names.push_back({ n == nullptr ? "" : n, {} });
		}
		humanize_ = std::make_unique<ModeValueCell>(control(mode), "Humanize", std::move(names),
		                                            [this](size_t param) { return live(param); });
		// THE AMOUNT ONLY (the card's reason: Seed is its own quantity, seated with the boxes).
		humanize_->addFollower(pt::at(pt::Global::HumanizeAmount),
		                       std::make_unique<ValueBox>(control(pt::at(pt::Global::HumanizeAmount)), ""));
		// FREE'S PHASE in the Free feels (design §12-bis); its Fine is seated with the globals' pills.
		humanize_->addFollower(pt::at(pt::Global::FreePhase),
		                       std::make_unique<ValueBox>(control(pt::at(pt::Global::FreePhase)), ""));
		addChild(humanize_.get());
	}
	for (size_t i = 0; i < pt::kGlobalCount; ++i) {
		const auto g = static_cast<pt::Global>(i);
		if (g == pt::Global::Enabled || g == pt::Global::Selector || g == pt::Global::HumanizeMode
		    || g == pt::Global::HumanizeAmount || g == pt::Global::FreePhase) {
			continue; // the header's chip; the knob; the zone cell and what it holds
		}
		const device::ParamDesc& d = pt::kParamDescs[i];
		const char* caption = d.shortName != nullptr ? d.shortName : d.name;
		if (g == pt::Global::Run) {
			// RUN, EVERY CHOICE ON THE FACE (Note mode replaced the Only w/ Transport switch): a segmented
			// row, so which of Always / Transport / Note is chosen is seen before it is chosen (the multivalue rule).
			auto seg = std::make_unique<SegmentedControl>(control(i), registry, std::span<const char* const>(d.names));
			globalWide_.push_back({ i, seg.get(), SwitchChip::kHeight });
			addChild(seg.get());
			globalWidgets_.push_back(std::move(seg));
			continue;
		}
		if (isSwitch(d)) {
			auto chip = makeModulatedChip(control(i), caption);
			globalChips_.push_back({ i, chip.get(), SwitchChip::kHeight });
			addChild(chip.get());
			globalWidgets_.push_back(std::move(chip));
		}
		else if (d.unit == device::Unit::Choice || d.stepped || d.digitBox) { // (a digit box: Wobble, unbounded)
			auto box = std::make_unique<ValueBox>(control(i), caption);
			box->setCompact(true);
			globalBoxes_.push_back({ i, box.get(), ValueBox::kCompactHeight });
			addChild(box.get());
			globalWidgets_.push_back(std::move(box));
		}
		else {
			auto pill = std::make_unique<FaderPill>(control(i), caption);
			globalBank_.push_back({ i, pill.get(), kPillHeight });
			addChild(pill.get());
			globalWidgets_.push_back(std::move(pill));
		}
	}
	buildPatternWidgets();
	buildPills();
	buildDrumBank();
	setFocus(0);
}

void PatternExpanded::buildDrumBank() {
	for (auto& c : drumChips_) {
		removeChild(c.get());
	}
	for (auto& b : drumNotes_) {
		removeChild(b.get());
	}
	drumChips_.clear();
	drumNotes_.clear();
	// THE LANE FIELDS (design §11-ter): a chip for the lane's on/off, a box for its note — DAW-automatable fields.
	for (size_t l = 0; l < pn::kDrumLanes; ++l) {
		drumChips_.push_back(makeModulatedChip(control(pt::drumOnAt(focus_, l)), pn::kDrumLaneShort[l]));
		addChild(drumChips_.back().get());
		drumNotes_.push_back(std::make_unique<ValueBox>(control(pt::drumNoteAt(focus_, l)), ""));
		drumNotes_.back()->setCompact(true);
		addChild(drumNotes_.back().get());
	}
}

PatternExpanded::~PatternExpanded() = default;

std::string PatternExpanded::title() const {
	return pt::kTraits.name;
}

DeviceControl& PatternExpanded::control(size_t index) const {
	if (controls_[index] == nullptr) {
		controls_[index] = std::make_unique<DeviceControl>(host_, device_, pt::kDescriptor, index, mod_, history_);
	}
	return *controls_[index];
}

bool PatternExpanded::live(size_t index) const {
	// DRUM HIDES THE PITCH BOXES its keys never read (`pt::drumHides`, as the card asks).
	if (index >= pt::kGlobalCount && index < pt::kParamCount) {
		const size_t p = (index - pt::kGlobalCount) / pt::kFieldCount;
		const long m = std::lround(host_.fieldAt(device_, pt::at(p, Field::Mode)));
		if (pt::drumHides(index, static_cast<pt::Mode>(std::clamp<long>(m, 0, static_cast<long>(pt::Mode::kCount) - 1)))) {
			return false;
		}
	}
	// THE NOTE RANGE FOLDS NO DRUM AND NO MODULATION (design §7.12): hidden while no pitched pattern is in range.
	if (index == pt::at(pt::Global::NoteLow) || index == pt::at(pt::Global::NoteHigh)) {
		return pt::noteRangeLive([this](size_t i) { return host_.fieldAt(device_, i); });
	}
	// TRANSPOSE MOVES NO DRUM (design §7.7): hidden while every pattern in range is a Drum.
	if (index == pt::at(pt::Global::TransposeSteps) || index == pt::at(pt::Global::TransposeOctaves)) {
		return pt::transposeLive([this](size_t i) { return host_.fieldAt(device_, i); });
	}
	// THE SAME GATE THE CARD AND EVERY GENERIC SURFACE READ (`ParamDesc::gateControl`), followed up its chain.
	size_t at = index;
	for (size_t hops = 0; hops < pt::kParamCount && at < pt::kParamCount; ++hops) {
		const device::ParamDesc& d = pt::kParamDescs[at];
		if (d.gateControl < 0) {
			return true;
		}
		const auto gate = static_cast<size_t>(d.gateControl);
		if (!device::relevantFor(d, host_.fieldAt(device_, gate))) {
			return false;
		}
		at = gate;
	}
	return true;
}

void PatternExpanded::buildPatternWidgets() {
	for (auto& w : patternWidgets_) {
		removeChild(w.get());
	}
	patternWidgets_.clear();
	patternBoxes_.clear();
	patternChips_.clear();
	patternBank_.clear();
	for (auto* old : std::initializer_list<visage::Frame*> { rotate_.get(), zone_.get(), zoneOffset_.get(), gamma_.get() }) {
		if (old != nullptr) {
			removeChild(old);
		}
	}
	const auto box = [&](Field f, const char* caption) {
		auto b = std::make_unique<ValueBox>(control(pt::at(focus_, f)), caption);
		b->setCompact(true);
		addChild(b.get());
		return b;
	};
	// THE TERRAIN'S OWN, lying in it (made after the terrain, so they draw over the map they move).
	rotate_ = std::make_unique<GammaBox>(control(pt::at(focus_, Field::Rotate)), "Rotate");
	rotate_->setDragSpan(1.0); // the Phaser's and the Shaper's: one unit of Rotate a full drag
	addChild(rotate_.get());
	zone_ = box(Field::Zone, "Zone");
	zoneOffset_ = box(Field::ZoneOffset, "Fine"); // the zone's fine half (Zone Offset); "Offset" is the global step offset
	gamma_ = std::make_unique<GammaBox>(control(pt::at(focus_, Field::Gamma)), "Gamma");
	addChild(gamma_.get());
	// THE PATTERN'S OWN, beside the terrain, by what each control is.
	const auto seat = [&](size_t i) {
		const device::ParamDesc& d = pt::kParamDescs[i];
		const char* caption = d.shortName != nullptr ? d.shortName : d.name;
		if (isSwitch(d)) {
			auto chip = makeModulatedChip(control(i), caption);
			patternChips_.push_back({ i, chip.get(), SwitchChip::kHeight });
			addChild(chip.get());
			patternWidgets_.push_back(std::move(chip));
		}
		else if (d.unit == device::Unit::Choice) {
			auto b = std::make_unique<ValueBox>(control(i), caption);
			b->setCompact(true);
			patternBoxes_.push_back({ i, b.get(), ValueBox::kCompactHeight });
			addChild(b.get());
			patternWidgets_.push_back(std::move(b));
		}
		else {
			auto pill = std::make_unique<FaderPill>(control(i), caption);
			patternBank_.push_back({ i, pill.get(), kPillHeight });
			addChild(pill.get());
			patternWidgets_.push_back(std::move(pill));
		}
	};
	for (size_t k = 0; k < pt::kFieldCount; ++k) {
		const auto f = static_cast<Field>(k);
		if (f == Field::Enable || f == Field::Solo || f == Field::PlaceX || f == Field::PlaceY || f == Field::Zone
		    || f == Field::ZoneOffset || f == Field::Gamma || f == Field::Rotate
		    || k >= static_cast<size_t>(Field::Pin0)) {
			continue; // the tabs, the puck, the terrain's own, the pins' bank, Drum's lane bank
		}
		seat(pt::at(focus_, f));
	}
	// MODULATION'S LANES (design §7.10): fields past the pins too, each seated by what it is; shown in Modulation alone.
	for (const Field f : { Field::ModSmooth, Field::ModTime, Field::ModPolarity, Field::ModNeutral }) {
		seat(pt::at(focus_, f));
	}
	// THE PATTERN'S OWN TRANSPOSE (design §7.7), with its boxes — fields past the pins, so the loop above leaves them.
	for (const Field f : { Field::TransposeSteps, Field::TransposeOctaves }) {
		const size_t i = pt::at(focus_, f);
		const device::ParamDesc& d = pt::kParamDescs[i];
		auto b = std::make_unique<ValueBox>(control(i), d.shortName != nullptr ? d.shortName : d.name);
		b->setCompact(true);
		patternBoxes_.push_back({ i, b.get(), ValueBox::kCompactHeight });
		addChild(b.get());
		patternWidgets_.push_back(std::move(b));
	}
	modulationBound(mod_, history_);
}

void PatternExpanded::buildPills() {
	for (auto& pill : pills_) {
		removeChild(pill.get());
	}
	pills_.clear();
	pinSwitches_.clear();
	pinAmounts_.clear();
	for (size_t i = 0; i < gn::kPinCount; ++i) {
		pinAmounts_.push_back(std::make_unique<DeviceControl>(host_, device_, pt::kDescriptor,
		                                                      pt::pinAt(focus_, static_cast<gn::Pin>(i)), mod_, history_));
		pinSwitches_.push_back(std::make_unique<PinSwitch>(host_, device_, focus_, i));
		pills_.push_back(std::make_unique<FaderPill>(*pinAmounts_.back(), *pinSwitches_.back(), gn::kPinNames[i]));
		addChild(pills_.back().get());
	}
}

void PatternExpanded::modulationBound(ModState* mod, EditHistory* history) {
	mod_ = mod;
	history_ = history;
	lane_->setModState(mod);
	terrain_->setModulation(mod, history);
	lane_->setEditable(true, history);
	for (auto& c : controls_) {
		if (c != nullptr) {
			c->setModulation(mod, history);
		}
	}
	for (auto& c : pinAmounts_) {
		c->setModulation(mod, history);
	}
	selector_->setModulation(mod, history);
	// **EVERY SOURCE'S HANDLE** (design §7.10): the card shows the open tab's, the window all of them. No state, no
	// handles (`ArpExpanded`'s rule).
	for (size_t n = 0; n < arms_.size(); ++n) {
		if (arms_[n] != nullptr) {
			removeChild(arms_[n].get());
			arms_[n].reset();
		}
		if (mod != nullptr) {
			arms_[n] = std::make_unique<SourceArm>(*mod, backplane::Source::output(device_, n), pt::kOutputs[n].name);
			addChild(arms_[n].get());
		}
	}
	layOut();
}

visage::Frame* PatternExpanded::widgetForTest(size_t param) const {
	for (const auto* seats : { &patternBoxes_, &patternChips_, &patternBank_, &globalBoxes_, &globalWide_, &globalChips_, &globalBank_ }) {
		for (const Seat& s : *seats) {
			if (s.param == param) {
				return s.widget;
			}
		}
	}
	return nullptr;
}

void PatternExpanded::setFocus(size_t pattern) {
	focus_ = std::min(pattern, pt::kPatterns - 1);
	tabs_->setFocus(focus_);
	terrain_->setFocus(focus_);
	lane_->setFocus(focus_);
	if (laidFocus_ != focus_) {
		buildPatternWidgets();
		buildPills();
		buildDrumBank();
	}
	(void)terrain_->poll();
	(void)lane_->poll();
	layOut();
	redraw();
}

std::vector<uint8_t> PatternExpanded::liveSet() const {
	std::vector<uint8_t> out(pt::kParamCount + 1);
	for (size_t i = 0; i < pt::kParamCount; ++i) {
		out[i] = live(i) ? 1u : 0u;
	}
	out[pt::kParamCount] = static_cast<uint8_t>(std::lround(host_.fieldAt(device_, pt::at(focus_, Field::Mode))));
	return out;
}

float PatternExpanded::flow(std::vector<Seat>& seats, float x, float y, float w, float cell, bool stretch) {
	const auto perRow = static_cast<size_t>(std::max(1.0f, std::floor((w + kPillGap) / (cell + kPillGap))));
	const float cw = stretch ? (w - kPillGap * static_cast<float>(perRow - 1)) / static_cast<float>(perRow) : std::min(cell, w);
	size_t n = 0;
	float rowHeight = 0.0f;
	float top = y;
	for (Seat& s : seats) {
		const bool shown = live(s.param);
		s.widget->setVisible(shown);
		if (!shown) {
			continue;
		}
		const size_t col = n % perRow;
		if (col == 0 && n > 0) {
			top += rowHeight + kPillGap;
			rowHeight = 0.0f;
		}
		s.widget->setBounds(x + static_cast<float>(col) * (cw + kPillGap), top, cw, s.height);
		rowHeight = std::max(rowHeight, s.height);
		++n;
	}
	return n == 0 ? y : top + rowHeight + kGap;
}

void PatternExpanded::layOut() {
	laidLive_ = liveSet();
	laidFocus_ = focus_;
	const float w = std::max(0.0f, static_cast<float>(width()) - 2.0f * kPad);
	// THE TAB STRIP, every tab's E and S on it — and in Cycle a second row: each tab's Repeats and Next.
	const bool cycling = live(pt::at(0, Field::Next));
	const float stripH = kTabsHeight + (cycling ? kCycleRow : 0.0f);
	tabs_->setBounds(kPad, kPad, w, stripH);
	const float cell = w / static_cast<float>(pt::kPatterns);
	// THE CHIPS AT EACH TAB'S RIGHT, chip-sized: a tab is its number, not two wide buttons.
	const float chip = std::clamp((cell - kTabsHeight - 6.0f) / 2.0f, 0.0f, 26.0f);
	for (size_t p = 0; p < pt::kPatterns; ++p) {
		const float x = kPad + cell * static_cast<float>(p + 1) - 2.0f * chip - 8.0f;
		const float cy = kPad + 0.5f * (kTabsHeight - SwitchChip::kHeight);
		chips_[2 * p]->setBounds(x + 2.0f, cy, chip, SwitchChip::kHeight);
		chips_[2 * p + 1]->setBounds(x + 4.0f + chip, cy, chip, SwitchChip::kHeight);
		// F (a fill) left of E, and Go (its trigger) left of F while it is one.
		fillChips_[p]->setBounds(x - chip, cy, chip, SwitchChip::kHeight);
		// GO A LITTLE WIDER THAN A CHIP where the tab has room: routed, its ring sits at the right end and must clear the
		// caption (the capture of 2026-11-08 showed "GoO" at a chip's width). Never over the MIDI grip at the tab's left.
		const float gripRight = kPad + cell * static_cast<float>(p) + PatternTabs::kNumberRow + 16.0f + 2.0f;
		const float goRight = x - chip - 2.0f;
		const float goW = std::clamp(goRight - gripRight, 0.0f, chip + 10.0f);
		goChips_[p]->setBounds(goRight - goW, cy, goW, SwitchChip::kHeight);
		goChips_[p]->setVisible(live(pt::at(p, Field::FillTrigger)));
		// THE MIDI GRIP, just right of the tab's number.
		midiGrips_[p]->setBounds(kPad + cell * static_cast<float>(p) + PatternTabs::kNumberRow, cy, 16.0f, SwitchChip::kHeight);
		// THE CYCLE CELL: "3×" then the Next box, inside the tab under its number and chips.
		const float cx = kPad + cell * static_cast<float>(p) + 6.0f;
		const float cw = std::max(0.0f, cell - 12.0f);
		const float repW = std::min(40.0f, cw * 0.32f);
		const float ry = kPad + kTabsHeight + 0.5f * (kCycleRow - ValueBox::kCompactHeight) - 2.0f;
		cycleRepeats_[p]->setBounds(cx, ry, repW, ValueBox::kCompactHeight);
		const float spanW = std::min(58.0f, cw * 0.3f);
		const float nextW = std::max(0.0f, cw - repW - spanW - 2.0f * kPillGap);
		cycleNext_[p]->setBounds(cx + repW + kPillGap, ry, nextW, ValueBox::kCompactHeight);
		cycleSpan_[p]->setBounds(cx + repW + kPillGap + nextW + kPillGap, ry, spanW, ValueBox::kCompactHeight);
		cycleSpan_[p]->setVisible(live(pt::at(p, Field::Span)));
		cycleRepeats_[p]->setVisible(live(pt::at(p, Field::Repeats)));
		cycleNext_[p]->setVisible(live(pt::at(p, Field::Next)));
	}
	// THE PANEL: the terrain at the left, its own controls lying in it.
	const float panelTop = kPad + stripH;
	const float x0 = kPad + kGap;
	const float iw = std::max(0.0f, w - 2.0f * kGap);
	float y = panelTop + kGap;
	const float tw = std::min(kTerrainWidth, iw * 0.55f);
	terrain_->setBounds(x0, y, tw, kTerrainHeight);
	heart_->setBounds(x0 + kOverlayInset, y + kOverlayInset, 22.0f, 18.0f);
	rotate_->setBounds(x0 + tw - kRotateWidth - kOverlayInset, y + kOverlayInset, kRotateWidth, rotate_->naturalHeight());
	{
		// ALONG ITS FOOT: where you are — Zone, its fine half, Gamma.
		const float bh = ValueBox::kCompactHeight;
		const float by = y + kTerrainHeight - kOverlayInset - bh;
		const float bw = std::min(kOverlayBoxWidth, (tw - 2.0f * kOverlayInset - 2.0f * kPillGap) / 3.0f);
		float bx = x0 + kOverlayInset;
		for (visage::Frame* f : std::initializer_list<visage::Frame*> { zone_.get(), zoneOffset_.get(), gamma_.get() }) {
			f->setBounds(bx, by, bw, bh);
			bx += bw + kPillGap;
		}
	}
	// BESIDE IT: the lane, then the pattern's choices, its chip, and its slider bank.
	const float rx = x0 + tw + kGap;
	const float rw = std::max(0.0f, x0 + iw - rx);
	lane_->setBounds(rx, y, rw, kLaneHeight);
	inspector_->setBounds(rx, y + kLaneHeight + kGap, rw, StepInspector::kHeight);
	float ry = y + kLaneHeight + kGap + StepInspector::kHeight + kGap;
	ry = flow(patternBoxes_, rx, ry, rw, 96.0f);
	ry = flow(patternChips_, rx, ry, rw, 96.0f);
	ry = flow(patternBank_, rx, ry, rw, 120.0f);
	// DRUM'S LANE BANK, in Drum and Modulation alone (the honesty rule): two lanes a row, each its chip and (Drum) its note.
	{
		const long focusMode = std::lround(host_.fieldAt(device_, pt::at(focus_, Field::Mode)));
		const bool drum = focusMode == static_cast<long>(pt::Mode::Drum) || focusMode == static_cast<long>(pt::Mode::Modulation);
		const bool notes = focusMode == static_cast<long>(pt::Mode::Drum);
		const float half = (rw - kPillGap) * 0.5f;
		const float chipW = std::min(54.0f, half * 0.45f);
		for (size_t l = 0; l < drumChips_.size(); ++l) {
			drumChips_[l]->setVisible(drum);
			drumChips_[l]->setLabel(notes ? pn::kDrumLaneShort[l] : pn::kModLaneNames[l]); // Modulation's lanes are Mod 1–8
			// AND ITS TOOLTIP: the field's declared one names the kit lane; in Modulation it is Mod N, and what off means there.
			control(pt::drumOnAt(focus_, l)).setTooltipOverride(notes ? std::string {} : "Whether " + std::string(pn::kModLaneNames[l]) + " moves. Off rests it at 0, so whatever it modulates is left as it is");
			drumNotes_[l]->setVisible(notes);
			const float x = rx + static_cast<float>(l % 2) * (half + kPillGap);
			const float yy = ry + static_cast<float>(l / 2) * (ValueBox::kCompactHeight + kPillGap);
			drumChips_[l]->setBounds(x, yy + 0.5f * (ValueBox::kCompactHeight - SwitchChip::kHeight), chipW, SwitchChip::kHeight);
			drumNotes_[l]->setBounds(x + chipW + 2.0f, yy, std::max(0.0f, half - chipW - 2.0f), ValueBox::kCompactHeight);
		}
		if (drum) {
			ry += static_cast<float>(pn::kDrumLanes / 2) * (ValueBox::kCompactHeight + kPillGap) + kGap;
		}
	}
	y = std::max(y + kTerrainHeight, ry) + kGap;
	// THE PINS' BANK, under both — the ones the focused pattern's Mode reads (design §5: the others are not drawn).
	const auto mode = static_cast<gn::Mode>(std::clamp(static_cast<int>(std::lround(host_.fieldAt(device_, pt::at(focus_, Field::Mode)))), 0,
	                                                     static_cast<int>(gn::Mode::kCount) - 1));
	const float pinW = (iw - kPillGap * static_cast<float>(kPillsPerRow - 1)) / static_cast<float>(kPillsPerRow);
	size_t shown = 0;
	for (size_t i = 0; i < pills_.size(); ++i) {
		const bool read = gn::pinRead(mode, static_cast<gn::Pin>(i));
		pills_[i]->setVisible(read);
		if (!read) {
			continue;
		}
		const size_t row = shown / kPillsPerRow;
		const size_t col = shown % kPillsPerRow;
		pills_[i]->setBounds(x0 + static_cast<float>(col) * (pinW + kPillGap),
		                     y + static_cast<float>(row) * (kPillHeight + kPillGap), pinW, kPillHeight);
		++shown;
	}
	const size_t pinRows = (shown + kPillsPerRow - 1) / kPillsPerRow;
	y += static_cast<float>(pinRows) * (kPillHeight + kPillGap) + kGap;
	panel_ = { kPad, panelTop, w, y - panelTop };
	// THE DEVICE'S OWN, under the panel: the selector and Humanize's cell, then the boxes, the chips and the bank.
	y += kGap;
	const float knobW = 84.0f;
	selector_->setBounds(kPad, y, knobW, Knob::columnHeight(Knob::kEngineRailDiameter));
	selector_->setFaceDiameter(Knob::kEngineRailDiameter);
	selector_->setVisible(live(pt::at(pt::Global::Selector))); // Cycle does not read it (the honesty rule)
	humanize_->setFaceDiameter(Knob::kEngineRailDiameter);
	humanize_->setBounds(kPad + knobW + kGap, y, std::max(humanize_->minimumWidth(), 120.0f),
	                     Knob::columnHeight(Knob::kEngineRailDiameter));
	(void)humanize_->refresh();
	const float gx = kPad + knobW + kGap + std::max(humanize_->minimumWidth(), 120.0f) + kGap;
	const float gw = std::max(0.0f, kPad + w - gx);
	float gy = flow(globalBoxes_, gx, y, gw, 110.0f, false);
	gy = flow(globalWide_, gx, gy, gw, 230.0f, false);
	gy = flow(globalChips_, gx, gy, gw, 110.0f, false);
	gy = flow(globalBank_, gx, gy, gw, 170.0f, false);
	// THE SOURCES under the device's own — the same walk every host lays a card's chips by — those `windowOutputsFor` shows: the
	// seven, every tab's gate, and the open tab's Mod 1–8 while it is in Modulation.
	shownOutputs_ = windowShown();
	const float sy = std::max(y + Knob::columnHeight(Knob::kEngineRailDiameter), gy) + kGap;
	const size_t rows = outputChipRowsFor(pt::kDescriptor, w, {}, shownOutputs_);
	sourceStrip_ = { kPad, sy, w, static_cast<float>(rows) * kCardOutputStripHeight };
	const std::vector<visage::Bounds> chips = outputChipBoundsFor(pt::kDescriptor, sourceStrip_, {}, shownOutputs_);
	for (size_t n = 0; n < arms_.size(); ++n) {
		if (arms_[n] != nullptr && n < chips.size()) {
			arms_[n]->setBounds(chips[n].x(), chips[n].y(), chips[n].width(), chips[n].height());
			arms_[n]->setVisible(chips[n].width() > 0.0f);
		}
	}
	refreshReadouts();
}

void PatternExpanded::refreshReadouts() {
	// A SLIDER BANK READS ITS VALUES: each amount-only pill prints its control's text at its right end.
	for (const auto* bank : { &patternBank_, &globalBank_ }) {
		for (const Seat& s : *bank) {
			if (auto* pill = dynamic_cast<FaderPill*>(s.widget)) {
				pill->setReadout(control(s.param).text());
			}
		}
	}
}

visage::Point PatternExpanded::minimumSize() const {
	const float knobs = Knob::columnHeight(Knob::kEngineRailDiameter);
	const float pins = 3.0f * (kPillHeight + kPillGap);
	const float h = 2.0f * kPad + kTabsHeight + kCycleRow + kGap + kTerrainHeight + StepInspector::kHeight + kGap + pins + kGap + kGap + knobs + 3.0f * kGap
	                + 2.0f * (kPillHeight + kPillGap) + kGap + kSourceRows * kCardOutputStripHeight;
	return { 2.0f * kPad + kTerrainWidth + kGap + 380.0f, h };
}

void PatternExpanded::resized() {
	layOut();
}

void PatternExpanded::draw(visage::Canvas& canvas) {
	PatternTabs::drawPanel(canvas, panel_.x(), panel_.y(), panel_.width(), panel_.height());
}

bool PatternExpanded::updateContent() {
	// GO'S PENDING LOOK: the fill the DEVICE says is armed (published beside its playhead), until it fires.
	{
		float level = 0.0f;
		const size_t armed = host_.playheadLevelAt(device_, level) ? pt::PatternDevice::armedFillOfLevel(level) : pt::kPatterns;
		for (size_t p = 0; p < goChips_.size(); ++p) {
			if (goChips_[p] != nullptr) {
				goChips_[p]->setPending(p == armed);
			}
		}
	}
	// F ASLEEP WHILE ITS TAB'S E IS OFF (a fill tab does not join the cycle while E is off): the
	// walk never picks it, Go still fires it. The chip says so — half-lit — and its tooltip says why.
	for (size_t p = 0; p < fillChips_.size(); ++p) {
		if (fillChips_[p] == nullptr) {
			continue;
		}
		const bool fill = host_.fieldAt(device_, pt::at(p, Field::Fill)) >= 0.5f;
		const bool enabled = host_.fieldAt(device_, pt::at(p, Field::Enable)) >= 0.5f;
		const bool asleep = fill && !enabled;
		if (asleep != fillChips_[p]->dormant()) {
			fillChips_[p]->setDormant(asleep);
			control(pt::at(p, Field::Fill))
			    .setTooltipOverride(asleep ? "This tab is a fill, but its E is off: the cycle never lands on it. Go "
			                                 "still fires it. Turn E on for the cycle to pick it too."
			                               : "");
		}
	}
	const bool turned = foldRotateIfTurned(host_, device_);
	// THE HAND'S EDITS ARE THE STANCE'S: a moved base place (or Mode) clears them.
	const bool cleared = clearStaleEdits(host_, device_);
	// THE HONESTY RULE: a gate or the focused Mode moved, so what is shown moves with it.
	bool relaid = false;
	if (windowShown() != shownOutputs_) {
		layOut(); // the open tab's Mode moved: its Mod 1–8 come or go
		relaid = true;
	}
	if (liveSet() != laidLive_) {
		layOut();
		relaid = true;
	}
	refreshReadouts();
	// THE PLAYING DOT, as the card's: on Cycle the device's playhead names the tab (the row cannot); else the selector's.
	if (std::lround(host_.fieldAt(device_, pt::at(pt::Global::PlayMode))) == static_cast<long>(pt::PlayMode::Cycle)) {
		uint32_t ordinal = mod::Playhead::kIdle;
		float phase = 0.0f;
		const bool have = host_.playheadAt(device_, ordinal, phase) && ordinal != mod::Playhead::kIdle;
		tabs_->setPlaying(have ? std::min<size_t>(ordinal >> pt::PatternDevice::kPlayheadStepBits, pt::kPatterns) : pt::kPatterns);
	}
	else {
		std::vector<float> row(pt::kParamCount);
		const size_t present = host_.fieldsPresentAt(device_);
		for (size_t i = 0; i < row.size(); ++i) {
			row[i] = i < present ? host_.modulatedFieldAt(device_, i) : static_cast<float>(pt::kParamDescs[i].def);
		}
		tabs_->setPlaying(pt::selected(pt::settingsOf(row.data(), row.size())));
	}
	const bool map = terrain_->poll();
	return lane_->poll() || map || turned || relaid || cleared;
}

} // namespace feathers::ui

namespace feathers::ui {

namespace pt = feathers::mod::arp::pat;

mod::arp::pat::MidiSource midiSourceOf(const backplane::DeviceHost& host, DeviceInstanceId id) {
	pt::MidiSource src;
	src.row.resize(pt::kParamCount);
	const size_t present = host.fieldsPresentAt(id);
	for (size_t i = 0; i < src.row.size(); ++i) {
		src.row[i] = i < present ? host.fieldAt(id, i) : static_cast<float>(pt::kParamDescs[i].def);
	}
	std::vector<float> block(pt::kHandWords);
	if (host.blockAt(id, 1, block)) {
		src.hand = block;
	}
	block.assign(pt::kEditsWords, 0.0f);
	if (host.blockAt(id, pt::kEditsBlock, block)) {
		src.edits = block;
	}
	block.assign(mod::arp::kPlayWords, 0.0f);
	if (host.blockAt(id, 0, block)) {
		src.play = block;
	}
	return src;
}

double midiTempoOf(const backplane::DeviceHost& host) {
	const double spb = host.secondsPerBeat();
	return spb > 0.0 && std::isfinite(spb) ? 60.0 / spb : 120.0;
}

MidiGrip::MidiGrip(backplane::DeviceHost& host, DeviceInstanceId id, size_t pattern)
    : host_(host), device_(id), pattern_(pattern) {
	installTooltipHover(*this, *this);
}

std::filesystem::path MidiGrip::directoryFor(const char* xdgCache, const char* home, bool apple) {
	const bool haveHome = home != nullptr && *home != '\0';
	// macOS: the platform's own per-user cache, not an XDG one (normally unset there).
	if (apple && haveHome) {
		return std::filesystem::path(home) / "Library" / "Caches" / "Feathers" / "MIDI";
	}
	std::filesystem::path base;
	if (!apple && xdgCache != nullptr && *xdgCache != '\0') {
		base = xdgCache;
	}
	else if (!apple && haveHome) {
		base = std::filesystem::path(home) / ".cache";
	}
	else {
		std::error_code ec;
		base = std::filesystem::temp_directory_path(ec);
	}
	return base / "feathers" / "midi";
}

std::filesystem::path MidiGrip::directory() {
#if defined(__APPLE__)
	constexpr bool kApple = true;
#else
	constexpr bool kApple = false;
#endif
	return directoryFor(std::getenv("XDG_CACHE_HOME"), std::getenv("HOME"), kApple);
}

std::string MidiGrip::startDragDropSource() {
	// WRITTEN AT THE PRESS (visage asks for the file then): the pattern as it plays now, at the song's tempo.
	const std::vector<uint8_t> bytes = pt::patternMidi(midiSourceOf(host_, device_), pattern_, midiTempoOf(host_));
	std::error_code ec;
	const std::filesystem::path dir = directory();
	std::filesystem::create_directories(dir, ec);
	const std::filesystem::path path = dir / pt::patternMidiName(pattern_);
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	out.close();
	lastPath_ = out ? path.string() : std::string();
	return lastPath_;
}

void MidiGrip::draw(visage::Canvas& canvas) {
	// A GRIP: three short bars, the "drag me" mark, in the muted ink.
	const auto w = static_cast<float>(width());
	const auto h = static_cast<float>(height());
	canvas.setColor(kMuted);
	for (int i = 0; i < 3; ++i) {
		canvas.fill(w * 0.25f, h * (0.3f + 0.2f * static_cast<float>(i)), w * 0.5f, 1.5f);
	}
}

bool PatternExpanded::exportMidi(size_t pattern) {
	// THE SAME BYTES THE GRIP DRAGS, written where the hand says (the OS's own save dialog).
	FileDialogOptions options;
	options.mode = FileDialogOptions::Mode::SaveFile;
	options.title = "Export MIDI";
	options.defaultName = pt::patternMidiName(pattern);
	options.filters = { FileFilter { "MIDI", { "mid" } } };
	options.parent = nativeWindowOf(this);
	const std::vector<uint8_t> bytes = pt::patternMidi(midiSourceOf(host_, device_), pattern, midiTempoOf(host_));
	return exportDialog_.open(options, [bytes](const FileDialogResult& result) {
		if (!result.chosen()) {
			return;
		}
		std::ofstream out(result.path, std::ios::binary | std::ios::trunc);
		out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	});
}

} // namespace feathers::ui

namespace feathers::ui {

uint64_t PatternExpanded::windowShown() const {
	const long mode = std::lround(host_.fieldAt(device_, mod::arp::pat::at(focus_, mod::arp::pat::Field::Mode)));
	return mod::arp::pat::windowOutputsFor(focus_, mode == static_cast<long>(mod::arp::pat::Mode::Modulation));
}

} // namespace feathers::ui
