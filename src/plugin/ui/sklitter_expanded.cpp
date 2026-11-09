/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#include "plugin/ui/sklitter_expanded.h"

#include "fx/sklitter/sklitter.h"
#include "plugin/device_preset.h"
#include "rack/inner_chain.h"
#include "plugin/ui/sklitter_card.h"

#include "fx/sklitter/perform.h"
#include "fx/sklitter/sklitter_types.h"
#include "plugin/ui/generic_card.h" // GenericCard::zoneNamesFor — the one decode of a zoned coordinate
#include "plugin/ui/theme.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace feathers::ui {

namespace sc = feathers::fx::sklitter;

//--------------------------------------------------------------------------------------------------//
// RAIL 1 — THE PERFORM CIRCLE.                                                                       //
//--------------------------------------------------------------------------------------------------//

CircleRail::CircleRail(device::Modulatable& perfX, device::Modulatable& perfY,
                       std::array<std::unique_ptr<ZoneCoordCell>, sc::kCorners> corners)
    : Rail("PERFORM", Side::Right) {
	pad_ = std::make_unique<PerformPad>(perfX, perfY);
	addChild(pad_.get());
	for (size_t i = 0; i < sc::kCorners; ++i) {
		if (corners[i] == nullptr) {
			continue;
		}
		corners_[i] = corners[i].get();
		addChild(corners[i].get());
		owned_[i] = std::move(corners[i]);
	}
	// THE TOGGLE IS THE RAIL'S OWN VERB AND THE ONLY ONE IT HAS. Everything else in here is a child
	// frame that answers for itself — which is why this is `toggleHit` rather than the fold-down
	// class's whole-header rule: a rail's chrome is a 20px button in a 34px column, and the column
	// under it is the content.
	onMouseDown() = [this](const visage::MouseEvent& e) {
		if (toggleHit(e.position)) {
			toggleCollapsed();
			return;
		}
		// AND THE ANCHOR BUTTON (backlog HT), the rail's second verb: a press does nothing while it is dimmed.
		if (!collapsed() && anchorBounds().contains(e.position) && anchorEnabled() && anchorPress_) {
			anchorPress_();
			redraw();
		}
	};
}

void CircleRail::setAnchor(std::function<bool()> enabled, std::function<void()> press) {
	anchorEnabled_ = std::move(enabled);
	anchorPress_ = std::move(press);
}

visage::Bounds CircleRail::anchorBounds() const {
	// THE RIGHT END OF THE HEADING ROW, which the label leaves empty.
	const visage::Bounds heading = headingBounds();
	const float w = std::min(kAnchorWidth, heading.width());
	return { heading.right() - w, heading.y(), w, heading.height() };
}

visage::Bounds CircleRail::headingBounds() const {
	// PAST THE TOGGLE, which on this side shares the heading's end of the head row. `Rail` works out
	// where that is, because which end the toggle sits at is `Rail`'s business and not this rail's —
	// the fx lane's first render drew "P⟩ST-FX" by not asking.
	const float left = railHeadingLeft(kPadX);
	return { left, kPadTop, std::max(0.0f, static_cast<float>(width()) - left - kPadX), kHeadingHeight };
}

uint32_t CircleRail::fillGround(visage::Canvas& canvas, float wide, float tall, uint32_t colour) {
	canvas.setColor(colour);
	canvas.rectangle(0.0f, 0.0f, wide, tall);
	return colour;
}

void CircleRail::draw(visage::Canvas& canvas) {
	// **THE GROUND BEHIND THE CIRCLE IS FEATHERS' PERFORM GROUND (2026-09-20)**, the same colour as
	// Feathers' Perform background.
	//
	// **WHICH COLOUR THAT IS, READ RATHER THAN NAMED.** `PerformStage::draw` paints NO ground of its
	// own — it draws sounds, sky, wake, trail, tour, rim, reticle and label, and nothing under any of
	// them — so "Feathers' perform background" is whatever the editor paints behind it, which is
	// `Gui::Impl::drawBackground`'s `canvas.setColor(kVoid); canvas.fill(0, 0, w, h)`. `theme.h` has
	// `kVoid = tokens::kVoidPerform`, a GENERATED token, so it is the mock's own `--void-perform`
	// (`#010204`) rather than a hand-picked dark. The mock agrees from the other end: `drawBlendView`,
	// the function that draws that circle, clips to the disk and fills it `TH.voidP` — the same token.
	// Two independent readings, one answer.
	//
	// **SO THIS RAIL IS THE ONE EXCEPTION TO "the lane's own ground".** `FxLane` and `FxLaneRail` keep
	// `kSurfaceRaised`, because they hold CARDS and a card needs a surface to sit on. This rail holds an
	// aperture, and an aperture wants the void behind it.
	//
	// THE HAIRLINE IS STILL THE LANE'S, and still on the INBOARD edge for the same reason: the rule
	// separates the rail from what it sits beside rather than from the window. It matters more here
	// than it did — against `kVoid` the rail no longer announces its own edge by being lighter.
	ground_ = fillGround(canvas, static_cast<float>(width()), static_cast<float>(height()), kGround);
	canvas.setColor(kLine);
	canvas.fill(0.0f, 0.0f, 1.0f, static_cast<float>(height()));

	// CLOSED IS A WHOLE DRAWING: the label goes upright, the disk and the cells are not drawn at all
	// (they are hidden frames), and what is left is the strip and the way back out of it.
	if (collapsed()) {
		drawRailChrome(canvas);
		return;
	}
	const visage::Bounds heading = headingBounds();
	canvas.setColor(kMuted);
	drawTracked(canvas, railLabel(), font(kControlSize), kControlTracking, heading.x(), heading.y(),
	            heading.height());
	// THE ANCHOR BUTTON: outlined, in the accent while a press would change something and dimmed while it would not.
	if (anchorPress_) {
		const visage::Bounds b = anchorBounds();
		const bool live = anchorEnabled();
		canvas.setColor(live ? kAccent : kDisabled);
		canvas.roundedRectangleBorder(b.x(), b.y(), b.width(), b.height(), 3.0f, 1.0f);
		canvas.text("ANCHOR", font(kControlSize), visage::Font::kCenter, b.x(), b.y(), b.width(), b.height());
	}
	drawRailChrome(canvas);
	// The disk and the four cells are CHILD FRAMES and paint themselves.
}

void CircleRail::resized() {
	if (collapsed()) {
		return; // `collapsedChanged` has hidden them; a hidden frame's bounds say nothing
	}
	const auto wide = static_cast<float>(width());
	if (wide <= 0.0f) {
		return;
	}
	// THE DISK IS CENTRED ACROSS THE RAIL, under the heading, with a square TUCKED AT EACH CORNER of the
	// block — at the corners of the circle so they stack more efficiently, now with the
	// room to do it properly: at 392 the corner offset is 114 rather than 4, so the cells genuinely nest
	// beside the circle instead of sitting in rows above and below it.
	//
	// **THE DISK IS CENTRED IN THE ROOM** under the heading and above the output band. At the panel's
	// own ask that room is square (`kBlockHeight` = `kInner`) and this draws what it always drew.
	//
	// **AND CENTRED DOWN THE RAIL'S REMAINING HEIGHT.** A rail is as tall as the panel and the room is
	// at least `kBlockHeight`; top-aligning it would leave the circle hanging from the heading with the whole
	// column empty beneath it. `kOpenMinimumHeight` is what keeps the centring from ever being negative.
	//
	// **AND IT IS SIZED FROM THE ROOM THE RAIL WAS GIVEN, NOT FROM `expandedWidth()` (2026-09-20).**
	// When the window is enlarged, the whole Perform circle scales with it, as large as the window
	// allows. `SklitterExpanded::resized` hands this rail the panel's whole surplus width, and the
	// panel's height is the rail's height already — so `layout()` reads both rooms and
	// everything below falls out of it, exactly as it used to fall out of `kInner`. The constants are
	// the floor: at the smallest panel the room IS `kBlockHeight` square and this draws what it drew
	// before. `rail.h`'s ban is on `expandedWidth()` measuring, and that function still answers 392 cold.
	//
	// **AND SINCE BACKLOG IR THE ROOM NEED NOT BE SQUARE.** `layoutFor` sizes the disk from the whole
	// room under the heading and above the output band, and places the cells where that room has space.
	const Layout at = layout();
	const float cx = wide * 0.5f;
	const float top = kPadTop + kHeadingHeight + kGap;
	// **THE OUTPUT BAND COMES OUT OF THE ROOM THE DISK IS CENTRED IN, NOT ONLY THE ROOM IT IS SIZED
	// FROM.** `downFor` already took it out of the size; the centre is the middle of that same room, so
	// the circle cannot centre over the strip. **AND IT IS THE BAND THIS WIDTH NEEDS** — `downFor` reads
	// `outputBandFor(wide)`, so a rail narrow enough for a third row of chips centres the disk in the
	// room the taller band leaves. A room floored above what the rail has overflows at the bottom.
	const float cy = top + at.down * 0.5f;
	pad_->setBounds(cx - at.disk * 0.5f, cy - at.disk * 0.5f, at.disk, at.disk);

	const auto place = [&](sc::Corner c, float x, float y) {
		if (corners_[c] != nullptr) {
			corners_[c]->setBounds(x, y, kCellSide, kCellSide);
		}
	};
	// **EACH CELL IS AT ITS OWN WAYPOINT'S CORNER, AND THAT IS A FACT RATHER THAN A CONVENTION.**
	//
	// The first draft of this moved the cells and left the waypoints on the cardinals, which made a
	// corner touch TWO waypoints and "which one is this" something a reader had to learn. The intent
	// is that the cardinals themselves move 45 degrees to match the box locations. So the
	// waypoints rotated 45° to meet the cells (`fx::sklitter::kWaypointAxis`), and the pairing is now
	// the picture: `kCfg1` is the top-left rim point with the top-left square beside it, then clockwise.
	//
	// **THE TWO DECLARATIONS ARE JOINED BY A CASE, NOT BY THIS COMMENT** — *"each Cfg cell is at its own
	// waypoint's corner"* reads `perform_pad::waypointPoint` for the direction and the cell's bounds for
	// the position, so a rotation applied to one and not the other is a red rather than a drawing nobody
	// checks.
	//
	// **EACH CELL'S INNER CORNER IS `(dx, dy)` OUT FROM THE DISK'S CENTRE, INTO ITS OWN QUADRANT** —
	// `layoutFor` says how far on each axis (backlog IR). Clamped to the rail so a panel laid out
	// narrower than this rail asked for puts its cells inside the frame rather than off the end of it.
	const float left = std::max(0.0f, cx - at.dx - kCellSide);
	const float right = std::min(wide - kCellSide, cx + at.dx);
	const float highRow = cy - at.dy - kCellSide;
	const float lowRow = cy + at.dy;
	place(sc::kCfg1, left, highRow);
	place(sc::kCfg2, right, highRow);
	place(sc::kCfg3, right, lowRow);
	place(sc::kCfg4, left, lowRow);

	layOutOutputArms();
}

CircleRail::Layout CircleRail::layoutFor(float railWidth, float railHeight) {
	Layout at;
	at.across = acrossFor(railWidth);
	at.down = downFor(railWidth, railHeight);
	// THE FURTHEST A CELL CAN GO, which is the room's corner: its inner corner is this far out on each
	// axis. Both are positive because the room is floored at `kBlockFloor` > 2·`kCellSide`.
	const float roomX = at.across * 0.5f - kCellSide;
	const float roomY = at.down * 0.5f - kCellSide;
	const float diagonal = 2.0f * (std::hypot(roomX, roomY) - kCornerClear);
	at.disk = std::max(0.0f, std::min({ at.across, at.down, diagonal }));	// THE CELLS: on the 45° ray at the grab reach plus the air, clamped to the room. When an axis
	// clamps, the other axis carries the cell out until the corner is exactly that far from the centre.
	const float reach = at.disk * 0.5f + kCornerClear;
	const float along = reach / kRoot2;
	at.dx = along;
	at.dy = along;
	if (along > roomX) {
		at.dx = roomX;
		at.dy = std::min(roomY, std::sqrt(std::max(0.0f, reach * reach - roomX * roomX)));
	} else if (along > roomY) {
		at.dy = roomY;
		at.dx = std::min(roomX, std::sqrt(std::max(0.0f, reach * reach - roomY * roomY)));
	}
	return at;
}

visage::Bounds CircleRail::outputStripBounds() const {
	// **THE BOTTOM OF THE PANE, FULL WIDTH INSIDE THE PADDING.**
	// Anchored to the rail's own bottom rather than to the block's, so it stays put while the disk
	// grows — which is what makes the strip a fixture of the pane and the circle the thing that scales.
	//
	// **AS TALL AS THE ROWS THIS WIDTH NEEDS (2026-09-22).** A fixed two rows was the truncation seen:
	// the placing walk hides any row past the strip's height, so at a yielded rail the strip
	// has to be as tall as the walk's own count or the tail of the outputs vanishes.
	const auto wide = static_cast<float>(width());
	const float tall = outputStripHeightFor(wide);
	return { kPadX, std::max(0.0f, static_cast<float>(height()) - kPadTop - tall),
		     std::max(0.0f, wide - 2.0f * kPadX), tall };
}

size_t CircleRail::outputRowsFor(float railWidth) {
	// **THE PLACING WALK IN COUNTING MODE** (`outputChipRows`: same widths, same greedy wrap, nothing
	// emitted), over the strip width `outputStripBounds()` will hand it — so what this reserves and
	// what `layOutOutputArms` places are one computation and cannot disagree about where a row breaks.
	// Floored at the design width's count so widening never takes a row away; see `kMinOutputRows`.
	const float strip = std::max(0.0f, railWidth - 2.0f * kPadX);
	return std::max(kMinOutputRows, outputChipRowsFor(sc::kDescriptor, strip));
}

float CircleRail::outputStripHeightFor(float railWidth) {
	return static_cast<float>(outputRowsFor(railWidth)) * kCardOutputStripHeight;
}

float CircleRail::outputBandFor(float railWidth) {
	return kGap + outputStripHeightFor(railWidth);
}

void CircleRail::setOutputSources(ModState* mod, std::function<DeviceInstanceId()> device) {
	outputDevice_ = std::move(device);
	if (mod == nullptr || outputDevice_ == nullptr) {
		// **NO ARMS WITHOUT A STATE TO ARM AGAINST** — `FxLane` states the same rule. A chip over
		// nothing is a control that cannot do what its arrow says, which is the honesty rule exactly.
		for (std::unique_ptr<SourceArm>& arm : outputArms_) {
			if (arm != nullptr) {
				removeChild(arm.get());
				arm.reset();
			}
		}
		return;
	}
	for (size_t n = 0; n < sc::kPerformOutputs; ++n) {
		if (outputArms_[n] != nullptr) {
			continue; // one panel, one device, for the panel's whole life — nothing to rebuild for
		}
		const char* name = sc::kOutputs[n].name;
		outputArms_[n] = std::make_unique<SourceArm>(*mod, backplane::Source::dormantOutput(n),
		                                             name != nullptr ? name : "");
		// **THE WAY TO ASK, NOT THE ANSWER** — `FxLane::ensureOutputArm`'s contract. The panel's device
		// does not move under it the way a lane SLOT's does, so this is belt and braces rather than a
		// fix; it costs one indirection and it means this widget cannot be the one that remembers.
		outputArms_[n]->setSourceProvider([this, n] {
			const DeviceInstanceId id = outputDevice_ ? outputDevice_() : kNoInstance;
			return id != kNoInstance ? backplane::Source::output(id, n)
			                         : backplane::Source::dormantOutput(n);
		});
		addChild(outputArms_[n].get());
		// HIDDEN IF THE RAIL IS SHUT, BEFORE IT IS EVER DRAWN — `Rail::collapsedChanged` only runs on a
		// toggle, so an arm built into an already-closed rail would draw across the 34px strip.
		outputArms_[n]->setVisible(!collapsed());
	}
	layOutOutputArms();
	redraw();
}

void CircleRail::layOutOutputArms() {
	// **THE SAME WALK EVERY OTHER HOST USES** (`outputChipBoundsFor`), so what this rail reserves and
	// what the modulator strip, the fx lane and the rack editor reserve are one computation of one
	// font. It WRAPS, so a chip that does not fit across goes on the next row instead of off the edge.
	//
	// **AND IT HIDES A ROW THAT DOES NOT FIT DOWN — WHICH IS WHY THE STRIP IS AS TALL AS THE WALK'S
	// OWN COUNT.** This comment used to say "nothing here hides a chip that does not fit". True at the
	// design width and false at a yielded one: the walk drops any row past `strip.height()` (the card's
	// design rule), and with two rows fixed a 266 rail lost `Perf Speed` and then the Weights — some of
	// Perform's mod outputs were truncated. `outputStripBounds()` now reserves
	// `outputRowsFor(width())` rows, the count from this same walk, so the hiding branch is
	// unreachable here BY CONSTRUCTION rather than by luck at one width. The case *"the output strip
	// grows by whole rows as the circle yields"* asserts all seven placed at the floor.
	const std::vector<visage::Bounds> boxes =
	    outputChipBoundsFor(sc::kDescriptor, outputStripBounds());
	for (size_t n = 0; n < outputArms_.size() && n < boxes.size(); ++n) {
		if (outputArms_[n] == nullptr) {
			continue;
		}
		outputArms_[n]->setBounds(boxes[n].x(), boxes[n].y(), boxes[n].width(), boxes[n].height());
	}
}

//--------------------------------------------------------------------------------------------------//
// THE PANEL.                                                                                         //
//--------------------------------------------------------------------------------------------------//

SklitterExpanded::SklitterExpanded(backplane::DeviceHost& host, WidgetRegistry& registry,
                                   DeviceInstanceId id, const device::Descriptor& desc,
                                   Notifier* notifier)
    : GenericExpanded(host, registry, id, desc, notifier), chainPin_(host) {
	// **THE PRESETS: FEATHERS' OWN BROWSER, THIS DEVICE'S BANK.** "Init" is the device's declared defaults
	// and both lanes empty (`initDevicePreset`) — a factory entry that is exactly what it says, the
	// honesty rule the editor's own "Init" was rewritten under. The user bank is read off disk now so
	// the first draw lists it; selecting nothing on open means opening the panel changes no sound.
	presets_ = std::make_unique<PresetBrowser>(std::vector<std::string> { "Init" },
	                                           std::vector<PresetData> { PresetData { initDevicePreset(desc) } },
	                                           devicePresetBank(desc), PresetBrowser::kPanelStyle);
	presets_->setBounds(0.0f, 0.0f, presets_->barWidth(), PresetBrowser::kPanelStyle.height);
	presets_->loadBankFromDisk();
	presetProvider_ = std::make_unique<DevicePresetProvider>(host, desc, id, kPresetKeeps, notifier);
	presets_->setProvider(presetProvider_.get());
	presets_->setNotifier(notifier);
	presets_->onSelected([this](int) { redrawAll(); });
	// THE PANEL OPENS ON THE PRESET THIS DEVICE CAME FROM, and names every later one into the patch (backlog QC).
	bindPresetIdentity(
	    *presets_, [&host, id] { return host.devicePreset(id); },
	    [&host, id](const PresetIdentity& identity) { host.setDevicePreset(id, identity); });

	// **THE FOUR CORNER PARAMS, NAMED BY ENUMERATOR RATHER THAN COUNTED FROM ONE.** `sklitter_types.cpp`
	// makes the same call at the same four fields and says why: a loop over `Param` values would make the
	// N/E/S/W order an assumption instead of something written down, and the two declarations that have
	// to agree about it live in different headers.
	cornerParams_[sc::kCfg1] = static_cast<size_t>(sc::Param::Cfg1);
	cornerParams_[sc::kCfg2] = static_cast<size_t>(sc::Param::Cfg2);
	cornerParams_[sc::kCfg3] = static_cast<size_t>(sc::Param::Cfg3);
	cornerParams_[sc::kCfg4] = static_cast<size_t>(sc::Param::Cfg4);

	// **A SECOND WIDGET OVER ONE FIELD, AND IT IS NOT A SECOND COPY OF THE VALUE.** Both this cell and
	// the grid's own hold a `DeviceControl` over the same control index, which reads the host on every
	// draw — there is no stored value to fall out of step. Only ONE of the two is ever visible
	// (`drawnBySubclass`), so what the duplication buys is that folding the circle puts the control
	// back on the grid without rebuilding anything.
	std::array<std::unique_ptr<ZoneCoordCell>, sc::kCorners> corners;
	for (size_t i = 0; i < sc::kCorners; ++i) {
		const size_t p = cornerParams_[i];
		if (p >= desc.params.size()) {
			continue; // a build whose descriptor is shorter than this file expects draws no corner
		}
		auto cell = std::make_unique<ZoneCoordCell>(controlFor(p), desc.params[p].name == nullptr
		                                                              ? ""
		                                                              : desc.params[p].name,
		                                            GenericCard::zoneNamesFor(desc.params[p]),
		                                            ZoneSelect::kCoordinatePerLap);
		// **THE SQUARE LAYOUT, WHICH IS ALSO WHAT DROPS THE CAPTION.** The phase/dropdown boxes are
		// square with two rows, and the N/E/S/W Cfg labels are gone. Those are one
		// change rather than two — the caption is the row the square does not have, and dropping it is
		// what lets the cell be 72px across instead of 92 (`ZoneCoordCell::Layout::Square`).
		cell->setLayout(ZoneCoordCell::Layout::Square);
		corners[i] = std::move(cell);
	}

	circle_ = std::make_unique<CircleRail>(controlFor(static_cast<size_t>(sc::Param::PerfX)),
	                                       controlFor(static_cast<size_t>(sc::Param::PerfY)),
	                                       std::move(corners));
	// THE ANCHOR (backlog HT): lit while the HAND's puck is off the centre — the base fields, not the modulated ones.
	circle_->setAnchor(
	    [this] {
		    sc::Settings base;
		    base.perfX = ownHost().fieldAt(watchedDevice(), static_cast<size_t>(sc::Param::PerfX));
		    base.perfY = ownHost().fieldAt(watchedDevice(), static_cast<size_t>(sc::Param::PerfY));
		    return sc::anchorWouldMove(base);
	    },
	    [this] { anchor(); });
	grainFx_ = std::make_unique<FxLaneRail>(FxLaneRail::Lane::Grain, registry, notifier);
	postFx_ = std::make_unique<FxLaneRail>(FxLaneRail::Lane::Post, registry, notifier);
	// **BOUND NOW RATHER THAN ON THE FIRST TICK.** A panel opened over a device that is already running
	// has its chain available at construction, and a column that waited for a tick would draw the
	// "not running" line for one frame on every open — which reads as a failure rather than as a wait.
	//
	// **ONE HANDLE, ASKED ONCE, GIVEN TO BOTH.** `containerSurfaceOf` files one pointer per instance;
	// each rail casts it through its own lane's accessor. Asking the host twice would be two reads of
	// one fact, and the second could differ from the first for no reason either rail could see.
	void* const held = host.containerSurfaceOf(id);
	(void)grainFx_->follow(held);
	(void)postFx_->follow(held);
	chainPin_.hold(held); // backlog JX: what the columns were built over stays until they let go
	for (Rail* rail : { static_cast<Rail*>(circle_.get()), static_cast<Rail*>(grainFx_.get()),
		                static_cast<Rail*>(postFx_.get()) }) {
		addChild(rail);
		// **TOLD, NOT ASSUMED — `rail.h`'s rule, and the whole of the coupling between a rail and
		// whoever owns its column.** A rail's width is one term in an arithmetic that belongs to its
		// owner; in the editor that owner is `gui.cpp`, and here it is this panel. A rail that set its
		// own bounds would be a second place deciding where the grid ends.
		rail->onRailWidthChanged([this]() {
			// THIS PANEL'S OWN COLUMNS FIRST — the rails move against the right edge and the band
			// shortens to what is left.
			resized();
			redraw();
			// **AND THE HOST IS TOLD, BECAUSE THE PANEL'S MINIMUM JUST MOVED.** `minimumSize()` is read
			// live by `ExpandedOverlay::panelSizeOf`, but LIVE IS NOT AUTOMATIC: something has to ask.
			// Without this line a rail that opened was laid out inside the old rectangle and clipped —
			// which is exactly what `widget-shot` was working around with a hand-written
			// `overlay->resized()` after setting them.
			minimumSizeChanged();
		});
	}

	// **AND THE BODY FOLDS TOO, WHICH IS THE THIRD OF THREE.** The main body is collapsible in
	// the window as well. Declared here rather than inherited, because a panel
	// with nothing beside its grid would fold into a strip holding one word — see
	// `GenericExpanded::setBodyCollapsible`. This panel qualifies on both counts the seam asks for:
	// there is something else to look at, and `bodyTopInset()` is 65px, well clear of the 26 the
	// chevron needs above the first row of cells.
	setBodyCollapsible();
}

namespace {

/// **EVERY FX RAIL THE PANEL HAS, so the four forwarders below cannot reach one and miss the other.**
/// Written as a walk rather than as two lines each, because two lines each is exactly how a third rail
/// would arrive half-wired — and this panel has already had a fix reach one surface and stop three
/// separate times (density, the hard cut, the redraw).
template <typename Fn> void forEachFxRail(FxLaneRail* grain, FxLaneRail* post, Fn&& fn) {
	for (FxLaneRail* rail : { grain, post }) {
		if (rail != nullptr) {
			fn(*rail);
		}
	}
}

} // namespace

void SklitterExpanded::modulationBound(ModState* mod, EditHistory* history) {
	GenericExpanded::modulationBound(mod, history);
	anchorHistory_ = history;
	// **BOTH LANES FOLLOW THE SAME OUTER STATE, AND ONLY THE OUTER IS SHARED.** The outer source is the
	// one thing a control in either chain can genuinely route from (`kPostChainAxes.coreSources =
	// kPerformOutputs`); a local arm inside one rail stays in that rail's own `ModState` and lights
	// that lane only, because a route runs through one `InnerChain`'s authority.
	// **AND EACH LANE IS TOLD WHOSE OUTPUTS IT PUBLISHES INWARD** — the holder is this panel's device,
	// which only this panel knows, and it is asked live for `setOutputSources`' reason (trap T9).
	forEachFxRail(grainFx_.get(), postFx_.get(), [&](FxLaneRail& rail) {
		rail.setOuterModulation(mod, history, [this] { return watchedDevice(); });
	});
	// **AND THE PERFORM PANE'S OWN OUTPUT CHIPS GET THE OUTER STATE DIRECTLY, NOT A PARENTED ONE.**
	// SKLITTER's mod outputs, once only on the card, are duplicated in the Perform pane.
	// The source these arm is SKLITTER's OWN output, which lives in the outer chain
	// exactly as the card's does — so this is the same `ModState` the card's chips use (`gui.cpp` hands
	// `&impl_->mod` to the fx lane and to every expanded view), and that is what makes the two chips
	// two views of ONE arm rather than two arms that can disagree.
	if (circle_ != nullptr) {
		circle_->setOutputSources(mod, [this] { return watchedDevice(); });
		// THE SAME HISTORY THE GRID'S CONTROLS RECORD INTO, so the pad can make its two of them one step.
		circle_->pad().setHistory(history);
	}
}

void SklitterExpanded::setPresentedOn(SurfaceLayers* host) {
	GenericExpanded::setPresentedOn(host);
	forEachFxRail(grainFx_.get(), postFx_.get(), [&](FxLaneRail& rail) { rail.setPresentedOn(host); });
}

void SklitterExpanded::onInnerNestChanged() {
	GenericExpanded::onInnerNestChanged();
	// **ONE NEST, BOTH RAILS.** See the rail's banner: the nest is the HOLDER's and the holder is one
	// device, so there is one answer. Two frames is not two nests.
	forEachFxRail(grainFx_.get(), postFx_.get(),
	              [this](FxLaneRail& rail) { rail.setNest(innerNest()); });
}

PerformPad& SklitterExpanded::padForTest() { return circle_->pad(); }

void SklitterExpanded::anchor() {
	// THE HAND'S SETTINGS — every base field as the record holds it, and the anchored push past them. Not the resolved
	// (modulated) values: HT's recommendation, so the routes keep swinging around the new home as they did the old.
	backplane::DeviceHost& host = ownHost();
	const DeviceInstanceId id = watchedDevice();
	sc::Row row {};
	for (size_t i = 0; i < row.size(); ++i) {
		row[i] = host.fieldAt(id, i);
	}
	sc::Settings base = sc::stateOf(row);
	for (size_t i = 0; i < sc::kAnchorFloats; ++i) {
		base.anchor[i] = host.fieldAt(id, sc::kAnchorFirst + i);
	}
	if (!sc::anchorWouldMove(base)) {
		return;
	}
	const sc::AnchorPress press = sc::anchorPressOf(base);
	// ONE UNDO STEP for the puck and the anchored push together: an undo that put back one without the other would
	// change the sound.
	std::vector<DeviceFieldChange> changes;
	changes.push_back({ static_cast<size_t>(sc::Param::PerfX), base.perfX, press.perfX });
	changes.push_back({ static_cast<size_t>(sc::Param::PerfY), base.perfY, press.perfY });
	for (size_t i = 0; i < sc::kAnchorFloats; ++i) {
		if (press.anchor[i] != base.anchor[i]) {
			changes.push_back({ sc::kAnchorFirst + i, base.anchor[i], press.anchor[i] });
		}
	}
	auto edit = std::make_unique<DeviceFieldsEdit>(host, id, std::move(changes), "anchor");
	edit->redo();
	if (anchorHistory_ != nullptr) {
		anchorHistory_->record(std::move(edit));
	}
	redraw();
}

const std::array<ZoneCoordCell*, fx::sklitter::kCorners>& SklitterExpanded::cornerCellsForTest() const {
	return circle_->corners();
}

float SklitterExpanded::bodyTopInset() const {
	// **THE BAND AND NOTHING ELSE.** The rails are beside the grid, not above it, so they take no body
	// height at all — the arithmetic they DO take part in is `minimumSize`'s width and `railsWidth()`.
	return kBandHeight + kBandGap;
}

float SklitterExpanded::bodyRightInset() const {
	// WHAT IS NOT THE BODY: the two rails down the right-hand edge. The base places the body's own rail
	// against this, so the body's chevron sits where the body stops rather than where the panel does.
	//
	// **PLUS THE SURPLUS, BECAUSE THE SURPLUS IS THE CIRCLE'S NOW.** `GenericExpanded::resized` gives the
	// body `width() - bodyRightInset()`; without this term the body would keep the room the circle has
	// been given and the two would be laid out on top of each other. One number, read here and in
	// `resized()`, so the body's right edge and the circle's left edge cannot disagree.
	// **AND IT IS THE RAILS AS LAID OUT, NOT AS ASKED** — `railsLaidOutWidth()` is what `resized()`
	// actually spends, so the body's right edge and the circle's left edge are one number in both
	// directions: the circle takes the surplus and gives back the deficit, and the body's inset follows
	// it either way.
	return railsLaidOutWidth();
}

float SklitterExpanded::widthDeficit() const {
	// **HOW MUCH LESS ROOM THE PANEL WAS GIVEN THAN IT ASKED FOR — the exact twin of `surplusWidth()`,
	// and by construction only one of the two is ever non-zero.**
	//
	// **A MONOTONE CLAMP, NOT A PREDICATE, AND THAT IS THE WHOLE DESIGN.** Written as "shrink the
	// circle if it clips" this would be a feedback loop — it clips, so the circle shrinks, so it no
	// longer clips, so it grows — flip-flopping every layout pass, which on a repaint timer is a
	// visible shimmer rather than a subtle bug. **`desired` is `minimumSize().x`, which sums the rails'
	// BASE widths** (`railWidth()` answers `expandedWidth()`, and that is a constant), so the deficit
	// cannot depend on the yield it causes and cannot cycle. **There is no `if (clipping)` anywhere in
	// this file and there must not be.**
	//
	// **AND IT IS READ IN `resized()`'s WORLD, WHICH IS WHY NO NEW SEAM WAS NEEDED.** `rail.h` forbids
	// `expandedWidth()` measuring a frame, because `gui.cpp` reads `railWidth()` *while placing* the
	// rail. Nothing forbids the PANEL reading its own `width()` during layout — the header above
	// `layoutFor` already states that distinction as ASK vs GIVEN. So `expandedWidth()` and
	// `minimumSize()` stay pure constants and the whole elasticity lives in the layout pass.
	return std::max(0.0f, minimumSize().x - static_cast<float>(width()));
}

float SklitterExpanded::surplusWidth() const {
	// **AGAINST THIS PANEL'S OWN MINIMUM, which is the whole panel's ask and not the rails'.** The grid's
	// answer plus both rails is what `ExpandedOverlay::panelSizeOf` fits a hand-sized panel to, so
	// anything past it is room nobody claimed. A panel the layer CLIPPED is narrower than its minimum
	// and the max floors that at zero.
	//
	// **AND IT IS NOT A TERM IN `minimumSize()`, WHICH WOULD BE A LOOP.** The panel is sized from its
	// minimum; a minimum that grew with the width it was given would grow again on the next pass and
	// never settle. The ask stays constant and only the layout spends the difference.
	if (circle_ == nullptr || circle_->collapsed()) {
		return 0.0f;
	}
	return std::max(0.0f, static_cast<float>(width()) - minimumSize().x);
}

float SklitterExpanded::circleWidth() const {
	if (circle_ == nullptr) {
		return 0.0f;
	}
	const float base = circle_->railWidth();
	if (circle_->collapsed()) {
		return base; // a 34px strip has nothing to give and nothing to spend
	}
	// **THE CIRCLE ABSORBS THE SURPLUS AND YIELDS THE DEFICIT, DOWN TO A FLOOR.** The circle shrinks
	// iff the window would otherwise clip off the screen — not merely when both lanes are open — so
	// the trigger is the room, not the rail count, and a third rail would need no new rule.
	//
	// **THIS IS ONE RULE, NOT A SPECIAL CASE.** The circle was already the elastic element — it took
	// every spare pixel while the other rails kept their plain `railWidth()`. It now gives as well as
	// takes, which is the symmetric statement of what it already did. The earlier rule still holds
	// UPWARD: the whole Perform circle scales as large as the window allows — it still takes
	// all the surplus and still grows without bound. What is new is that it yields when the panel's own
	// minimum cannot otherwise fit. The second rule refines the first rather than reversing it.
	//
	// `surplusWidth()` and `widthDeficit()` cannot both be non-zero, so this is one term either way.
	return std::max(CircleRail::kYieldFloorWidth, base + surplusWidth() - widthDeficit());
}

float SklitterExpanded::railsLaidOutWidth() const {
	// The sum `resized()` actually spends: the circle at its yielded width, the other two at their ask.
	return circleWidth() + (grainFx_ != nullptr ? grainFx_->railWidth() : 0.0f)
	       + (postFx_ != nullptr ? postFx_->railWidth() : 0.0f);
}

float SklitterExpanded::railsWidth() const {
	// A rail that has not been built yet costs nothing — this is reached from the base's constructor
	// through `minimumSize` before the members exist, which is the one place a null is real.
	// **ALL THREE, AND THE SECOND FX RAIL IS THE WHOLE COST OF THE SPLIT.** Two open fx lanes ask for
	// 600px where one asked for 300; closed they ask for 34 each. This is the sum `minimumSize` adds to
	// the grid's answer and the sum `resized` spends, so there is one place that arithmetic lives.
	return (circle_ != nullptr ? circle_->railWidth() : 0.0f)
	       + (grainFx_ != nullptr ? grainFx_->railWidth() : 0.0f)
	       + (postFx_ != nullptr ? postFx_->railWidth() : 0.0f);
}

bool SklitterExpanded::drawnBySubclass(size_t index) const {
	if (circle_ == nullptr || circle_->collapsed()) {
		return false; // closed: the corner cells are hidden and the grid seats them again
	}
	for (const size_t p : cornerParams_) {
		if (p == index) {
			return true;
		}
	}
	return false;
}

visage::Point SklitterExpanded::minimumSize() const {
	// **THE GRID'S OWN ANSWER PLUS THE RAILS, AND THAT SUM IS WHY THE WINDOW GROWS.** Chosen
	// over the alternative (compress the grid): the panel is sized from this on every layout pass
	// (`ExpandedOverlay::panelSizeOf`), so a rail that opens makes the window wider and a rail that
	// closes hands the width straight back. The grid keeps its knobs at full size throughout.
	visage::Point p = GenericExpanded::minimumSize();
	p.x += railsWidth();
	// AND DOWN: a rail is as tall as the panel, so the circle's block is a FLOOR on the height rather
	// than a term in it. A grid with few live cells is shorter than a 132px disk with a cell row above
	// and below it, and a panel that believed the grid would clip the circle.
	if (circle_ != nullptr && !circle_->collapsed()) {
		p.y = std::max(p.y, CircleRail::kOpenMinimumHeight);
	}
	// **AND AN FX RAIL'S OWN FLOOR, FOR THE SAME REASON AND FOUND THE SAME WAY.** While the fx rail held
	// three lines of text it needed no height; holding CARDS, it does — photographed with the body
	// collapsed and the circle closed, where the panel came out short enough to cut a filter card off
	// mid-plot. A MAX rather than a sum, and that is what makes the SECOND rail free in height: both
	// lanes are floors on ONE height, and a panel tall enough for either is tall enough for both. It is
	// only in WIDTH that the split costs anything.
	if ((grainFx_ != nullptr && !grainFx_->collapsed())
	    || (postFx_ != nullptr && !postFx_->collapsed())) {
		p.y = std::max(p.y, FxLaneRail::kOpenMinimumHeight);
	}
	return p;
}

visage::Point SklitterExpanded::maximumSize() const {
	if (circle_ != nullptr && !circle_->collapsed()) {
		return { kNoCap, kNoCap };
	}
	const visage::Point capped = GenericExpanded::maximumSize(); // this panel's minimum, plus a pad
	const bool laneOpen = (grainFx_ != nullptr && !grainFx_->collapsed())
	                      || (postFx_ != nullptr && !postFx_->collapsed());
	return { capped.x, laneOpen ? kNoCap : capped.y };
}

void SklitterExpanded::resized() {
	// THE GRID FIRST, which lays out every cell it owns and hides the ones it does not — including the
	// four corners while the circle is open. It runs left from `kPad` in fixed-width columns and knows
	// nothing about the rails; `minimumSize` is what keeps the two from meeting.
	GenericExpanded::resized();
	if (circle_ == nullptr || grainFx_ == nullptr || postFx_ == nullptr) {
		return;
	}

	// **FROM THE RIGHT EDGE INWARD, OUTERMOST FIRST — so left to right the panel reads
	// grid | PERFORM | GRAIN FX | POST FX.** Grain FX sits to the left of Post FX.
	// **AND THAT ORDER IS THE SIGNAL'S**: a chosen grain is diverted into the grain lane, rejoins the
	// wet, and the post lane runs on the sum — so reading left to right is reading downstream. The
	// stacked arrangement this replaces drew the two lanes the other way up and had to say the order in
	// words; the layout says it now.
	//
	// **THE LOOP IS WRITTEN BACKWARDS FROM THE READING ORDER ON PURPOSE**, because placing from the
	// right edge is what lets a closed rail hand its width to whatever is inboard of it without anyone
	// re-deriving the sum.
	//
	// A CLOSED RAIL STILL GETS ITS 34px STRIP, which is the whole of "minimised, never gone": the
	// toggle that closed it has to be there to open it. `railWidth()` is the one place that arithmetic
	// lives and `railsWidth()` sums the same numbers, so the band's width and the rails' left edges
	// cannot disagree.
	//
	// **AND THE CIRCLE IS THE ONE THAT TAKES ANY SURPLUS** — see `surplusWidth()` for why all of it and
	// why not the other three. `railWidth()` is still what each fx rail and every closed strip get, so
	// the only rail handed a different number is the circle.
	float right = static_cast<float>(width());
	for (FxLaneRail* rail : { postFx_.get(), grainFx_.get() }) {
		const float wide = rail->railWidth();
		right -= wide;
		rail->setBounds(right, 0.0f, wide, static_cast<float>(height()));
	}
	{
		const float wide = circleWidth();
		right -= wide;
		circle_->setBounds(right, 0.0f, wide, static_cast<float>(height()));
	}
}

bool SklitterExpanded::updateContent() {
	// THE GRID'S OWN ANSWER FIRST — a mode change re-lays the cells out, and that is a repaint too.
	bool moved = GenericExpanded::updateContent();

	// **THE CHAIN, RE-ASKED BY ID EVERY TICK.** The handle the host files for a container is a
	// contemporaneous one, so this is where a rebuilt instance (a body load — bolt-on-chains trap T9)
	// is noticed and the column is rebuilt over the new bridge rather than left over the old.
	//
	// **ASKED ONCE FOR BOTH RAILS.** One handle per instance, so a second `containerSurfaceOf` would be
	// a second read of one fact — and if the two reads ever disagreed, one rail would rebuild and the
	// other would not, with nothing on screen to say which.
	void* const held = ownHost().containerSurfaceOf(deviceId());
	forEachFxRail(grainFx_.get(), postFx_.get(), [&](FxLaneRail& rail) {
		if (rail.follow(held)) {
			moved = true;
		}
		// AND THE CARDS ADVANCED — a device inside the chain whose card draws a live reading (a meter,
		// an analyser) moves only because something asks it to. The count is produced BY the asking.
		if (rail.updateCards() > 0) {
			moved = true;
		}
	});
	// HELD NOW, THE OLD LET GO — after both rails dropped whatever they had built over it (backlog JX).
	chainPin_.hold(held);

	// **WHAT THE TWO STRIPS DRAW, COMPARED RATHER THAN ASSUMED.** A panel that redrew every tick would
	// repaint at the host's timer rate whether or not anything had changed. Same comparison the card
	// makes, over the same decode, so the two surfaces cannot disagree about when the picture moved.
	const int step = sklitterPlayingStep(ownHost(), deviceId());
	const float phase = sklitterStepPhase(ownHost(), deviceId());
	const float fill = ownHost().readingAt(deviceId(), static_cast<size_t>(sc::Reading::Fill));
	// AND THE BAR COUNT — the RULING can move while the chunks do not; see `SklitterCard::updateContent`.
	const int bars = sklitterSeqBars(ownHost(), deviceId());
	const sc::GrooveFeel groove = sklitterGroove(ownHost(), deviceId()); // backlog IG: the grid can move alone
	const float repeat = sklitterRepeatLength(ownHost(), deviceId()); // Repeat's face can move alone too
	// backlog MJ: the record light can change while nothing else on the band does.
	const int recording = sklitterRecording(ownHost(), deviceId()) ? 1 : 0;
	const std::vector<SklitterChunk> now = sklitterChunks(ownHost(), deviceId());

	// **THE SPARKS ARE TICKED HERE AND THEIR ANSWER IS KEPT OUT OF `moved` — backlog HR.** The pad
	// repaints ITSELF (`PerformPad::updateSparks` calls its own `redraw()`), so folding this into
	// `moved` would make the whole panel — the grid, both fx rails and every card on them — repaint at
	// 60 Hz for the lifetime of one flying spark. It goes into the RETURN instead, because the tick
	// report's count is "how many device frames were asked for" and one was.
	//
	// **AND THE LEVEL IS READ HERE RATHER THAN BY THE PAD**, which holds no device handle by design.
	// Zero while nothing is published, bypassed, or at Mix 0 — so the pad asks for nothing at rest,
	// which is the property this whole layer was required to keep.
	// **ONE READ FOR THE PAIR**, because the two levels are a partition only while they came from one
	// publish — `SklitterLevels` argues it, and splitting the read here would put the fault back.
	const SklitterLevels levels = sklitterLevels(ownHost(), deviceId());
	const PerformPad::SparkEvents events { levels.onsets, levels.repeats, levels.repeatPeriod };
	const bool sparks =
	    circle_->pad().updateSparks(levels.wet, levels.grain, levels.haveOnsets ? &events : nullptr);
	if (step != drawnStep_ || phase != drawnPhase_ || fill != drawnFill_ || bars != drawnSeqBars_
	    || groove != drawnGroove_ || repeat != drawnRepeat_ || recording != drawnRecording_
	    || now.size() != drawn_.size()) {
		moved = true;
	}
	else {
		for (size_t i = 0; i < now.size(); ++i) {
			if (now[i].source != drawn_[i].source || now[i].known != drawn_[i].known
			    || now[i].reversed != drawn_[i].reversed || now[i].pitchedUp != drawn_[i].pitchedUp
			    || now[i].dry != drawn_[i].dry || now[i].span != drawn_[i].span
			    || now[i].covered != drawn_[i].covered || now[i].ratchet != drawn_[i].ratchet
			    || now[i].piece != drawn_[i].piece) {
				moved = true;
				break;
			}
		}
	}
	if (!moved) {
		return sparks;
	}
	drawnStep_ = step;
	drawnPhase_ = phase;
	drawnFill_ = fill;
	drawnSeqBars_ = bars;
	drawnGroove_ = groove;
	drawnRepeat_ = repeat;
	drawnRecording_ = recording;
	drawn_ = now;
	redraw();
	return true;
}

void SklitterExpanded::drawBodyExtras(visage::Canvas& canvas) {
	const float left = kPad;
	// **LESS WHAT THE RAILS TAKE.** The band is the left column's own heading, and a strip drawn at the
	// panel's full width would run under both rails — which is not a clip the rails can be blamed for,
	// since children paint AFTER their parent and the rails would simply cover it.
	//
	// **AND LESS THE COLUMN THE BODY'S OWN CHEVRON SITS IN.** The toggle that folds the body is drawn at
	// the body's inboard edge, in this band's own row; a strip run to the full width would have the
	// chevron on top of the chunk colours, where a 1.6px muted stroke is not reliably visible. This is
	// the one place in the panel that pays for the affordance — the cells start below it.
	const float wide = std::max(0.0f, static_cast<float>(width()) - 2.0f * kPad - bodyRightInset()
	                                      - bodyToggleReserve());
	if (wide <= 0.0f) {
		return;
	}
	const std::vector<SklitterChunk> picture = sklitterChunks(ownHost(), deviceId());
	const int playing = sklitterPlayingStep(ownHost(), deviceId());

	const visage::Font& face = font(kKnobLabelSize);

	// **THE PERFORM HEADER WAS DRAWN HERE AND IS THE RAIL'S NOW (2026-09-19).** It was a rectangle this
	// panel drew and hit-tested itself, and two of its three defects were exactly what a shared base
	// prevents: it sat below `if (picture.empty()) { return; }` so it never drew on a device that had
	// published nothing, and its chevron was `\xe2\x96\xbe` — a character this atlas has no glyph for,
	// which rendered as the word with nothing after it. Both were PHOTOGRAPHED rather than reasoned
	// about. `Rail` draws its own chrome in both states and the panel no longer has a second rectangle
	// to keep in step with anything.

	const auto strip = [&](float top, const char* caption) {
		canvas.setColor(kMuted);
		canvas.text(caption, face, visage::Font::kLeft, left, top, wide, kLabelHeight);
		canvas.setColor(kSurfaceRaised);
		canvas.roundedRectangle(left, top + kLabelHeight, wide, kStripHeight, 3.0f);
	};

	// **ONE STRIP — the BUFFER strip above it is gone (backlog EY); see `sklitter_expanded.h`.**
	const float lowStrip = kPad;
	strip(lowStrip, "PLAYED");
	// **THE RECORD LIGHT (backlog MJ)** — the card's, at the right end of the strip's caption row, where nothing else draws.
	drawSklitterRecordLight(canvas, left + wide - kSklitterRecordLightRadius - 1.0f, lowStrip + 0.5f * kLabelHeight,
	                        sklitterRecording(ownHost(), deviceId()));

	if (picture.empty()) {
		return;
	}
	const int seqBars = sklitterSeqBars(ownHost(), deviceId());
	// THE SWUNG GRID (backlog IG) — the card's edges, from the same function.
	const std::vector<float> edges =
	    sklitterStepEdges(sklitterGroove(ownHost(), deviceId()), picture.size(), seqBars);

	// **RULED INTO ITS BARS.** The band spans the whole sequence, because a playhead spanning only one
	// bar hid the four-bar periodicity these sequences can have, and a sixty-four-chunk strip with no
	// grid is unreadable as four bars. Under the chunks, so a rule never competes with a modifier mark.
	drawSklitterBarRules(canvas, left, lowStrip + kLabelHeight + 2.0f, wide, kStripHeight - 4.0f,
	                    picture.size(), seqBars);

	// **REPEAT'S FACE** — the card's, at strip size; see `SklitterRepeatFace`.
	if (const float repeatLength = sklitterRepeatLength(ownHost(), deviceId()); repeatLength > 0.0f) {
		const SklitterRepeatFace face =
		    sklitterRepeatFace(picture, sklitterGroove(ownHost(), deviceId()), seqBars, repeatLength, playing);
		drawSklitterRepeatFace(canvas, left, lowStrip + kLabelHeight + 4.0f, wide, kStripHeight - 8.0f, picture,
		                       face, &font(kKnobLabelSize));
		if (playing >= 0 && static_cast<size_t>(playing) < picture.size()) {
			canvas.setColor(kInk);
			const float headX = left + edges[static_cast<size_t>(playing)] * wide;
			canvas.fill(headX, lowStrip + kLabelHeight + 2.0f, 1.5f, kStripHeight - 4.0f);
			drawSklitterHeadLabel(canvas, font(kKnobLabelSize), sklitterHeadRatchetLabel(picture, playing), headX,
			                      left, wide,
			                      lowStrip + kLabelHeight + 2.0f, kHeadLabelHeight);
		}
		return;
	}

	// **THE STRIP IS THE READING ORDER**, which is the card's band at panel size, modifiers and all.
	// See `sklitter_card.h` for why each mark is the mark it is.
	drawSklitterChunkFace(canvas, picture, edges, left, lowStrip + kLabelHeight + 4.0f, wide, kStripHeight - 8.0f,
	                      lowStrip + kLabelHeight + 1.0f, false, &face);
	if (playing >= 0 && static_cast<size_t>(playing) < picture.size()) {
		canvas.setColor(kInk);
		const float headX = left + edges[static_cast<size_t>(playing)] * wide;
		canvas.fill(headX, lowStrip + kLabelHeight + 2.0f, 1.5f, kStripHeight - 4.0f);
		// the playing cell's heard ratchet, as the card's head carries it
		drawSklitterHeadLabel(canvas, face, sklitterHeadRatchetLabel(picture, playing), headX, left, wide,
		                      lowStrip + kLabelHeight + 2.0f, kHeadLabelHeight);
	}
}

} // namespace feathers::ui
