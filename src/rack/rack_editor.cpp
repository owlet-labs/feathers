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

#include "rack/rack_editor.h"

#include "plugin/ui/expanded_views.h"
#include "plugin/ui/widget_registry.h"
#include "plugin/ui/device_menu.h"
#include "plugin/ui/emitters_front.h"
#include "plugin/ui/menu_anchor.h"
#include "rack/device_browser.h"

#include "plugin/ui/drop_gap.h"
#include "plugin/ui/theme.h"
#include "plugin/device_registry.h"
#include "rack/rack_bridge.h"
#include "composite/composite_types.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace feathers::rack {

namespace {

constexpr float kPad = 12.0f;
constexpr float kMacroDiameter = 34.0f;
/// THE ROOM EACH SIDE OF A SITE'S CENTRE (backlog NZ): a partition is a rule in the middle of it.
constexpr float kPartitionGap = 20.0f;

/// **A SEGMENT THAT DOES NOT RUN, SAID OVER IT** (backlog NZ, D4): the veil a disabled card wears, the reason in words,
/// and it takes the hand — a press on it reaches nothing beneath, which is the refusal.
class SegmentVeil final : public visage::Frame {
public:
	SegmentVeil() {
		setOnTop(true);
		setVisible(false);
		onMouseDown() = [](const visage::MouseEvent&) {};
	}
	void draw(visage::Canvas& canvas) override {
		canvas.setColor(ui::kDisabledVeil);
		canvas.fill(0.0f, 0.0f, static_cast<float>(width()), static_cast<float>(height()));
		// WRAPPED TO THE VEIL'S WIDTH, not left to one clipped line: the segment of an empty PRE is a few dozen pixels wide
		// and the reason ("This device has no PRE: …") was cut off on screen.
		const std::vector<std::string> rows = lines();
		const float lineHeight = kFontSize * 1.3f;
		float y = std::max(0.0f, (static_cast<float>(height()) - lineHeight * static_cast<float>(rows.size())) * 0.5f);
		canvas.setColor(ui::kInk);
		for (const std::string& row : rows) {
			canvas.text(row, ui::font(kFontSize), visage::Font::kCenter, kInset, y, width() - kInset * 2.0f, lineHeight);
			y += lineHeight;
		}
	}
	/// THE REASON BROKEN AT WORD BOUNDARIES so that each row is no wider than the veil less its insets (a single word
	/// wider than that stands alone on its row).
	std::vector<std::string> lines() const {
		const visage::Font face = ui::font(kFontSize);
		const float room = std::max(1.0f, static_cast<float>(width()) - kInset * 2.0f);
		std::vector<std::string> rows;
		std::string row;
		size_t at = 0;
		while (at < why.size()) {
			size_t end = why.find(' ', at);
			end = end == std::string::npos ? why.size() : end;
			const std::string word = why.substr(at, end - at);
			const std::string joined = row.empty() ? word : row + " " + word;
			if (!row.empty() && ui::textWidth(joined, face) > room) {
				rows.push_back(row);
				row = word;
			}
			else {
				row = joined;
			}
			at = end + 1;
		}
		if (!row.empty()) {
			rows.push_back(row);
		}
		return rows;
	}
	static constexpr float kFontSize = 12.0f;
	static constexpr float kInset = 8.0f;
	std::string why;
};

/// THE MACRO COLUMN'S METRICS: a knob cell at the macro face size, the gap between rows, the H-line under Mix and Gain.
constexpr float kMacroKnobHeight = ui::Knob::columnHeight(kMacroDiameter);
constexpr float kColumnGap = 4.0f;
constexpr float kRuleHeight = 1.0f;

// THE DESIGN HEIGHT HOLDS ONE UNIT OF THE LANE — the lane's top pad, one card
// unit and its foot pad — AND THE MACROS COLUMN BESIDE IT (the macros and a scrolling route list), which is what fixes it at 466.
static_assert(static_cast<float>(RackEditor::kHeight) >= kPad + ui::kCardUnitHeight + kPad);

/// **THE MACROS COLUMN'S RAIL** (backlog QN): the Engine rail's fold, for the rack's left column. It holds no widget — the
/// column's widgets are the editor's, which it shows and hides (`RackEditor::showColumn`) — so it is the chrome alone: the
/// toggle in the column's top-right corner while open, and the 34 px strip with the upright label while closed.
class MacrosRail final : public ui::Rail {
public:
	MacrosRail() : Rail("MACROS", Side::Left) {
		onMouseDown() = [this](const visage::MouseEvent& e) {
			if (toggleHit(e.position) && toggleCollapsed() && handToggled) {
				handToggled(); // the hand's fold says so; a restore does not
			}
		};
	}
	/// Told after a toggle the HAND made — the editor's notice. `setCollapsed` (a restore) tells nobody.
	std::function<void()> handToggled;
	/// The column, its pads included: what the backdrop covers.
	float expandedWidth() const override { return RackEditor::kLaneLeft - kPad * 0.5f; }
	void draw(visage::Canvas& canvas) override {
		if (collapsed()) {
			canvas.setColor(ui::kSurface);
			canvas.fill(0.0f, 0.0f, static_cast<float>(width()), static_cast<float>(height()));
		}
		// OPEN, THE FRAME IS THE TOGGLE'S CORNER and the chrome is the toggle; FOLDED, the strip names itself upright.
		drawRailChrome(canvas);
	}
};

} // namespace

/// THE MACRO COLUMN'S GROUND — see `RackEditor::columnBackdrop_`. Deaf, so a press meant for a control reaches it; it
/// draws the edge between the column and the lane and the H-line under Mix and Gain, which belong to the column.
class ColumnBackdrop final : public visage::Frame {
public:
	ColumnBackdrop() { setIgnoresMouseEvents(true, false); }
	/// The H-line, in THIS frame's coordinates; empty draws none.
	void setRule(visage::Bounds rule) {
		if (!(rule == rule_)) {
			rule_ = rule;
			redraw();
		}
	}
	void draw(visage::Canvas& canvas) override {
		canvas.setColor(ui::kSurface);
		canvas.fill(0.0f, 0.0f, width(), height());
		canvas.setColor(ui::kLine);
		canvas.fill(width() - 1.0f, 0.0f, 1.0f, height());
		if (rule_.width() > 0.0f) {
			canvas.fill(rule_.x(), rule_.y(), rule_.width(), rule_.height());
		}
	}

private:
	visage::Bounds rule_ {};
};

MacroControl::MacroControl(RackBridge& engine, size_t index, const char* name)
    : engine_(engine), index_(index) {
	desc_.name = name;
	desc_.min = 0.0;
	desc_.max = 1.0;
	desc_.def = 0.0;
	desc_.stepped = false;
	desc_.unit = device::Unit::Percent;
	// NOT MODULATABLE, because nothing may be routed AT a macro — see the header.
	desc_.modulatable = false;
}

double MacroControl::value() const {
	return static_cast<double>(engine_.macro(index_));
}

void MacroControl::setValue(double value) {
	engine_.setMacroByHand(index_, static_cast<float>(value)); // and the host hears it (backlog NG)
}

void MacroControl::beginGesture() {
	engine_.beginMacroGesture(index_);
}

void MacroControl::endGesture() {
	engine_.endMacroGesture(index_);
}

std::string MacroControl::text() const {
	return ui::formatValue(desc_, value());
}

device::ModDisplay MacroControl::display() const {
	device::ModDisplay out;
	out.isDestination = false;
	out.base = value();
	out.modulated = out.base;
	return out;
}

RackEditor::RackEditor(RackBridge& engine, ui::ModState& mod, ui::EditHistory& history)
    : engine_(engine), mod_(mod), history_(history) {
	// WHAT THIS EDITOR CAN PRESENT, told to the host the cards will ask. A card decides whether to
	// draw its way in by asking `engine_.hasExpandedViewFor`, and the engine — UI-free by rule — has
	// no table of its own; the one it answers from is the same `makeExpandedView` reads at press time
	// (the `onExpand` arm in `syncCards`), so the button and the panel cannot disagree.
	engine_.setExpandedViewQuery(&ui::hasExpandedView);
	// **DELETE AND BACKSPACE REMOVE THE FOCUSED DEVICE.** Both,
	// because which one a keyboard calls "delete" is a platform argument nobody should have to
	// win. `KPDelete` is the numpad's and costs nothing to accept alongside them.
	//
	// A KEY NOBODY WANTS IS NOT SWALLOWED — `false` hands it on, the rule `ExpandedOverlay`'s Esc
	// handler already had to learn: a layer that eats a key for the editor's whole life is a bug that
	// only shows up when a second road wants it.
	setAcceptsKeystrokes(true);
	onKeyPress() = [this](const visage::KeyEvent& e) {
		// UNDO AND REDO (backlog JB C10): main modifier + Z, + Shift + Z or + Y — Feathers' chord — walk this editor's
		// history, which in a nested window forwards to the outermost host's: one history per plugin.
		if (e.isMainModifier() && !e.isAltDown()) {
			if (e.keyCode() == visage::KeyCode::Z || (e.keyCode() == visage::KeyCode::Y && !e.isShiftDown())) {
				if (e.keyCode() == visage::KeyCode::Y || e.isShiftDown()) {
					history_.redo();
				}
				else {
					history_.undo();
				}
				return true;
			}
		}
		// THE SELECTION'S KEYS FIRST (backlog OA): Ctrl/Cmd + A C X V D G, and Delete over two or more — POST's while it
		// holds the selection.
		const std::vector<DeviceInstanceId> before = editOrder();
		if (keySelection().key(e)) {
			afterVerb(before);
			return true;
		}
		const visage::KeyCode code = e.keyCode();
		if (code != visage::KeyCode::Delete && code != visage::KeyCode::Backspace
		    && code != visage::KeyCode::KPBackspace) {
			return false;
		}
		const CardEntry* focused = entryOf(focusedDevice_);
		if (focused == nullptr || focused->card == nullptr) {
			return false;
		}
		removeDevice(focusedDevice_);
		return true;
	};
	columnBackdrop_ = std::make_unique<ColumnBackdrop>();
	addChild(columnBackdrop_.get());
	// THE LANE'S SCROLL BAR (backlog NE): a drag asks for an absolute scroll, which goes the wheel's road.
	laneScrollBar_ = std::make_unique<LaneScrollBar>();
	laneScrollBar_->onScroll = [this](float to) { (void)scrollBy(to - scroll_); };
	addChild(laneScrollBar_.get());
	static const char* const kNames[] = { "Macro 1", "Macro 2", "Macro 3", "Macro 4",
		                                  "Macro 5", "Macro 6", "Macro 7", "Macro 8" };
	const size_t count = engine_.axes().coreSources;
	for (size_t i = 0; i < count; ++i) {
		macros_.push_back(std::make_unique<MacroControl>(
		    engine_, i, i < std::size(kNames) ? kNames[i] : "Macro"));
		// THROUGH ITS POLARITY (backlog EG): the knob reads the macro as the bridge's mode says, and the
		// mode switch is the knob's own menu; the verb is the bridge's, so a case flips it without one.
		macroViews_.push_back(std::make_unique<ui::BipolarMacro>(
		    *macros_.back(), [this, i] { return engine_.macroBipolar(i); }));
		// A MACRO IS ARMED FROM HERE, which is how a hand points one at a control: the strip's arrow in
		// Feathers, and for now the knob itself. It is handed the panel state so it can report armed;
		// its own `display()` refuses to be a destination regardless.
		auto slot = makeMacroSlot(*macroViews_.back(), i, &mod_, &history_);
		addChild(slot.get());
		macroSlots_.push_back(std::move(slot));
		// A MACRO IS ONE OF THE RACK'S OWN SOURCES — a host end, at its index (kill-lanes step 2).
		auto arm = std::make_unique<SourceArm>(mod_, backplane::Source::core(i));
		addChild(arm.get());
		macroArms_.push_back(std::move(arm));
	}
	// **A NAME HEARD FROM THE BRIDGE RELABELS ITS SLOT**, looked up by index at the call: the row it
	// finds may be one `setOuterDevice` built after this was subscribed. Ended first in the destructor.
	renameSubscription_ = engine_.subscribeMacroRename([this](size_t macro) {
		if (macro < macroSlots_.size() && macroSlots_[macro] != nullptr) {
			macroSlots_[macro]->refresh();
		}
		// THE ROUTE LIST'S HEAD NAMES THE ARMED SOURCE through `setSourceNamer`, which reads the name too.
		if (routes_ != nullptr) {
			routes_->redraw();
		}
	});
	// **A RELABELLED OUTPUT RENAMES ITS ARM** — the arms are kept across a relabel (rebuilt only when the
	// chain changes), so the word goes to the one arm and the lane is laid out again at its new width.
	// The route list's head reads the label through the namer too. Ended first in the destructor.
	labelSubscription_ = engine_.subscribeOutputLabel([this](DeviceInstanceId device, size_t output) {
		for (OutputArm& entry : outputArms_) {
			if (entry.arm != nullptr && entry.device == device && entry.output == output) {
				entry.arm->setLabel(engine_.outputLabel(device, output));
			}
		}
		resized();
		redraw();
		if (routes_ != nullptr) {
			routes_->redraw();
		}
	});
	// **WHAT SOURCES THIS RACK HAS, FOR THE INBOUND-ROUTE MENU** — its macros, then every output of every
	// device in the chain, by endpoint. The menu walked `0..sourceCount` while a source was a flat index;
	// an endpoint has no count, so the editor that walks the devices says what exists (`ModState::
	// setSourceLister`). Asked live: the chain changes under it.
	mod_.setSourceLister([this] {
		std::vector<backplane::Source> out;
		for (size_t i = 0; i < engine_.axes().coreSources; ++i) {
			out.push_back(backplane::Source::core(i));
		}
		for (size_t position = 0; position < engine_.chainLength(0); ++position) {
			const DeviceInstanceId device = engine_.deviceAtPosition(position);
			if (const device::Descriptor* d = engine_.descriptorOf(device); d != nullptr) {
				for (size_t output = 0; output < d->outputCount(); ++output) {
					out.push_back(backplane::Source::output(device, output));
				}
			}
		}
		return out;
	});
	// **AND WHAT TO CALL THEM** — the route list's head names the armed source through this (rack roadmap
	// §7f), so the head and the macro's own caption agree: "Macro 3" or the name it was given (the
	// bridge's, which the slot reads too), or a modulator by its name and its
	// place in the chain, with the output's name when it has more than one.
	mod_.setSourceNamer([this](backplane::Source source) -> std::string {
		if (!source.isDevice()) {
			return engine_.macroName(source.index);
		}
		const device::Descriptor* d = engine_.descriptorOf(source.device);
		if (d == nullptr) {
			return "Source"; // an id nothing holds names nothing
		}
		std::string name = d->traits.name;
		if (const size_t position = engine_.positionOf(source.device); position != RackBridge::kNoSlot) {
			name += " ";
			name += std::to_string(position + 1);
		}
		if (d->outputCount() > 1 && source.index < d->outputCount()) {
			name += " · ";
			name += engine_.outputLabel(source.device, source.index); // as this instance calls it
		}
		return name;
	});
	// OVER EVERYTHING, AND DEAF: it is a readout, not a control, and a click meant for a knob
	// underneath must reach the knob. The mock says the same thing with `pointer-events:none`.
	// THE `(+)`, AT THE END OF THE STACK — the one gesture that makes this usable by hand. A press opens
	// Feathers' add menu against the slot (backlog KP).
	addRow_ = std::make_unique<AddDeviceRow>();
	addRow_->onHeightChanged = [this] {
		resized();
		redraw();
	};
	addRow_->onPress = [this] {
		// THE (+) IS AN APPEND. An "Add before" that was chosen and then abandoned must not turn the next
		// (+) into an insert.
		insertAnchor_.reset();
		showAddMenu(*addRow_, { 0.0f, 0.0f, static_cast<float>(addRow_->width()), AddDeviceRow::kBarHeight });
	};
	addChild(addRow_.get());

	// THE WHEEL SCROLLS THE STACK. Cards are children, so a wheel over one reaches here by bubbling.
	onMouseWheel() = [this](const visage::MouseEvent& e) {
		// BOTH AXES DRIVE THE ONE SCROLL, because the chain runs left-to-right and most wheels only go
		// up and down. A plain wheel scrolls the chain — which is what Bitwig's own device chain does,
		// and this rack lives inside one — while a trackpad's horizontal gesture reaches it directly.
		//
		// SUMMED RATHER THAN PICKED, so a diagonal gesture does something continuous instead of
		// snapping to whichever axis happened to be larger this frame.
		//
		// **THE HORIZONTAL SIGN IS THE OPPOSITE OF THE VERTICAL ONE, AND THAT IS NOT A TYPO.** A wheel's
		// y is reported positive when it rolls AWAY, so scrolling the chain forward wants `-y`; a
		// trackpad's x is reported positive when the fingers move RIGHT, and moving the fingers right
		// should carry the chain right, so it wants `+x`. The two axes describe the gesture in
		// opposite conventions and negating both treated them as one.
		//
		// **THERE IS NO OS HOOK FOR THIS THROUGH VISAGE**, which was the next question:
		// `WindowEventHandler::handleMouseWheel` passes the platform's deltas through untouched and
		// carries no inversion flag. On Linux libinput applies the user's natural-scroll preference in
		// the DRIVER, so what arrives here is already corrected for their setting; macOS exposes
		// `isDirectionInvertedFromDevice` on `NSEvent` and visage does not surface it. So the sign is
		// ours, and this one is set from a report on real hardware rather than from a headless test —
		// **no venue we own can feel a trackpad.**
		const float delta = -e.wheel_delta_y + e.wheel_delta_x;
		// ANSWERED ONLY IF THE CHAIN MOVED. A wheel this editor cannot use — the chain is already at
		// that end, or fits — bubbles to whatever holds it, which inside a canvas card is the
		// `MountedFrame` scrolling the OTHER axis (visage walks the parents until one answers). This
		// answered every wheel, so a chain panel taller than its card could never be scrolled to its
		// cards' feet:
		return scrollBy(delta * 40.0f);
	};

	// THE ARMED SOURCE'S ROUTE LIST, in the macro row's right column (rack roadmap §7f) — Feathers'
	// Engine rail holds the same widget. Before the tray, which is over everything.
	routes_ = std::make_unique<ui::RouteList>(engine_, mod_, history_, nullptr);
	addChild(routes_.get());

	// THE MACROS COLUMN'S RAIL (backlog QN): its toggle folds the column to a strip, as the Engine rail does. The column's
	// widgets are hidden, not destroyed — a macro keeps its value, a route keeps its amount, the host keeps automating both.
	auto rail = std::make_unique<MacrosRail>();
	rail->handToggled = [this] {
		if (notify_ != nullptr) {
			notify_->say(macrosCollapsed() ? "Macros minimised" : "Macros shown");
		}
	};
	macrosRail_ = std::move(rail);
	// OPENED THE WAY THE PATCH LEFT IT, from the first frame (before the width hook, so nothing is written back or said); a
	// later load is followed on the beat (`syncMacros`).
	(void)macrosRail_->setCollapsed(engine_.macrosCollapsed());
	addChild(macrosRail_.get());
	macrosRail_->onRailWidthChanged([this] {
		const bool folded = macrosRail_->collapsed();
		engine_.setMacrosCollapsed(folded); // PATCH STATE (QN): saved with the rack, as the canvas's fold is
		showColumn(!folded);
		resized();
		redraw();
		if (onMacrosCollapsedChanged) {
			onMacrosCollapsedChanged(folded);
		}
	});

	// **THE SELECTION OVER THIS EDITOR'S OWN CHAIN** (backlog OA, D1): which cards are selected, the verbs over them,
	// the lasso from the bare lane, and the overlay that draws the lasso and the "N selected" pill above the cards.
	// Under the tray and the surfaces, like everything else here.
	ui::LaneSelection::Hooks selectionHooks;
	selectionHooks.order = [this] { return chainOrder(); };
	selectionHooks.changed = [this] {
		exclusiveSelection(*selection_);
		applySelection();
	};
	selectionHooks.cardBounds = [this](DeviceInstanceId id) -> std::optional<visage::Bounds> {
		const CardEntry* entry = entryOf(id);
		if (entry == nullptr || entry->card == nullptr || !entry->card->isVisible()) {
			return std::nullopt;
		}
		return entry->card->bounds();
	};
	selection_ = std::make_unique<ui::LaneSelection>(engine_, history_, nullptr, std::move(selectionHooks));
	selectionOverlay_ = std::make_unique<ui::SelectionOverlay>(*selection_);
	addChild(selectionOverlay_.get());
	// THE LASSO STARTS ON THE BARE LANE: a press that reaches this frame itself hit no card, no column and no
	// canvas (each is a child that takes its own presses). A click with no drag clears the selection.
	// A SITE'S POST IS LASSOED BY THE SAME GESTURE (backlog OA), and the lasso keeps to the chain it touched first.
	onMouseDown() = [this](const visage::MouseEvent& e) {
		lassoChain_ = LassoChain::None;
		selection_->lassoBegin(e.position, e.isShiftDown());
		if (postSelection_ != nullptr) {
			postSelection_->lassoBegin(e.position, e.isShiftDown());
		}
	};
	const auto lassoTo = [this](visage::Point at) {
		lassoing_ = true;
		selection_->lassoMove(at);
		if (postSelection_ != nullptr) {
			postSelection_->lassoMove(at);
		}
		lassoing_ = false;
		const bool pre = !selection_->selection().empty();
		const bool post = postSelection_ != nullptr && !postSelection_->selection().empty();
		if (lassoChain_ == LassoChain::None && (pre || post)) {
			lassoChain_ = pre ? LassoChain::Pre : LassoChain::Post;
		}
		if (lassoChain_ == LassoChain::Pre && post) {
			postSelection_->clear();
		}
		else if (lassoChain_ == LassoChain::Post && pre) {
			selection_->clear();
		}
	};
	onMouseDrag() = [lassoTo](const visage::MouseEvent& e) { lassoTo(e.position); };
	onMouseUp() = [this, lassoTo](const visage::MouseEvent& e) {
		lassoTo(e.position);
		selection_->lassoEnd();
		if (postSelection_ != nullptr) {
			postSelection_->lassoEnd();
		}
	};

	// THE EDITOR'S TOOLTIP LAYER, under the notifier (`raiseTopLayers`) — see `tips_`.
	addChild(&tips_);
	tips_.watch(*this);
	notify_ = std::make_unique<ui::NotifyTray>();
	notify_->setIgnoresMouseEvents(true, false);
	addChild(notify_.get());

	// THE CANVAS, BESIDE THE LANE — a region of this layout, so it is a child here and not of the
	// surface host, and it sits UNDER the notifier and the surfaces like everything else in the editor.
	canvas_ = std::make_unique<ui::ExpandedCanvas>(ui::ExpandedCanvas::Edge::Top);
	// NEVER MORE THAN THE LANE BELOW IT — see `ExpandedCanvas::kMaxShare`.
	canvas_->setMaxExtent(static_cast<float>(kHeight) * ui::ExpandedCanvas::kMaxShare);
	// ITS FOLD AND ITS PARTITION ARE PATCH STATE (`RackBridge::setCanvasView`), and both change this
	// editor's preferred size; a drag step only re-lays out, the ask waits for the release.
	canvas_->onCollapsedChanged = [this] {
		// OPENING INTO A WINDOW THAT ALREADY HAS ROOM TAKES THAT ROOM — see `windowRoomHeight`.
		if (!canvas_->collapsed() && windowRoomHeight) {
			(void)canvas_->fitOpenExtentTo(static_cast<float>(windowRoomHeight()) - static_cast<float>(kHeight));
		}
		writeCanvasView();
		resized();
		redraw();
		if (onCanvasExtentChanged) {
			onCanvasExtentChanged();
		}
	};
	canvas_->onExtentDrag = [this](visage::Point pointer) {
		// THE HOST FIRST, which may put the partition under the pointer physically (`RackGui`); then
		// this editor lays out whatever extent stands.
		if (onCanvasDrag) {
			onCanvasDrag(pointer);
		}
		resized();
		redraw();
	};
	canvas_->onExtentCommit = [this] {
		writeCanvasView();
		if (onCanvasExtentChanged) {
			onCanvasExtentChanged();
		}
	};
	addChild(canvas_.get());

	// THE SURFACE HOST, ADDED LAST SO IT DRAWS OVER EVERYTHING — including the notifier. A takeover
	// that let the tray show through would be showing a message about a rack the player cannot see.
	surfaces_ = std::make_unique<ui::SurfaceLayers>();
	surfaces_->setCanvas(canvas_.get());
	// THE LAYERS FORGET THEIR CONTENT ON DISMISSAL — but only through the `ModState` they are given,
	// and nothing had given the overlay one. The same dangling-watcher fault as the cards above, one
	// layer up: a panel's knobs register on arming and its dismissal would have left them in the list.
	// One call for every layer, so the canvas cannot be the one that was forgotten.
	surfaces_->setModulationState(&mod_);
	addChild(surfaces_.get());
	selection_->setEscapeStack(&surfaces_->escapeStack());
	surfaces_->takeover().onDepthChanged = [this] {
		redraw();
	};

	syncCards();
}

RackEditor::~RackEditor() {
	// THE MACRO MENU'S HOOKS GO QUIET BEFORE ANYTHING THEY READ DOES (see `alive_`).
	alive_.reset();
	// AND NO NAME IS HEARD INTO A ROW THAT IS GOING: the bridge outlives this editor.
	renameSubscription_.reset();
	labelSubscription_.reset();
	// A PANEL THIS EDITOR PRESENTED ON AN OUTER HOST OUTLIVES ITS OWN LAYERS, and reads a device of a
	// bridge that is going with this editor — so it is taken down first. Local ids are the layers'
	// own and go with them either way.
	if (surfaces_ != nullptr) {
		const std::vector<ui::SurfaceId> up = expanded_;
		for (const ui::SurfaceId id : up) {
			surfaces_->dismiss(id);
		}
	}
	// THE LISTER AND THE NAMER CAPTURE `this` — a knob outliving this editor would call into a dead one
	// the first time somebody right-clicked it. `ModPanel` takes its own off for the same reason.
	mod_.setSourceLister(nullptr);
	mod_.setSourceNamer(nullptr);
}

void RackEditor::pruneExpanded() {
	// WHAT THE LAYERS NO LONGER HOLD IS NO LONGER OURS. Asked rather than told which: a dismissal's
	// callback is installed before the id exists.
	std::erase_if(expanded_, [this](ui::SurfaceId id) { return !surfaces_->holds(id); });
}

void RackEditor::writeCanvasView() {
	if (canvas_ != nullptr && canvas_->isVisible()) {
		engine_.setCanvasView({ canvas_->openExtent(), canvas_->collapsed() });
	}
}

bool RackEditor::syncCanvas() {
	// A NESTED EDITOR HAS NO CANVAS TO SYNC, and must not write the inner patch's view from a frame
	// that is hidden.
	if (canvas_ == nullptr || !canvas_->isVisible()) {
		return false;
	}
	// NOT WHILE A HAND IS ON THE PARTITION: the patch holds the last release's extent, and applying it on
	// every beat mid-drag snaps the partition back under the hand.
	if (canvas_->draggingExtent()) {
		return false;
	}
	const CanvasView view = engine_.canvasView();
	// AN EXTENT OF ZERO IS A PATCH THAT NEVER SET THE CANVAS: the frame keeps its default for both the
	// partition and the fold.
	if (view.extent <= 0.0f) {
		return false;
	}
	bool changed = false;
	if (canvas_->setOpenExtent(view.extent)) {
		changed = true;
	}
	if (view.collapsed != canvas_->collapsed()) {
		canvas_->setCollapsed(view.collapsed); // re-lays out and tells the host on its own road
		return true;
	}
	if (changed) {
		resized();
		redraw();
		if (onCanvasExtentChanged) {
			onCanvasExtentChanged();
		}
	}
	return changed;
}

void RackEditor::setOuterLayers(ui::SurfaceLayers* outer) {
	surfaces_->setOuterLayers(outer);
	selection_->setEscapeStack(&surfaces_->escapeStack()); // a nested chain's entry goes on the outermost stack
	if (postSelection_ != nullptr) {
		postSelection_->setEscapeStack(&surfaces_->escapeStack());
	}
	// NO CANVAS INSIDE A PANEL: a nested editor's panels go to the outermost host, and a strip that
	// offered to open one here would be offering a canvas inside a canvas.
	canvas_->setVisible(outer == nullptr);
	resized();
}

void RackEditor::setNest(ui::DeviceNest nest) {
	if (nest == nest_) {
		return;
	}
	nest_ = nest;
	// **THE CARDS ARE RE-STAMPED RATHER THAN REBUILT.** A nest is not a reason to destroy a card — it
	// changes only what colour the head wears and what key a panel this card opens is filed under. A
	// rebuild here would also drop a live press, which `syncCards` goes to some trouble not to do.
	for (CardEntry& entry : cards_) {
		if (entry.card != nullptr) {
			entry.card->setNest(nest_);
		}
	}
	redraw();
}

bool RackEditor::nestingFull() const noexcept {
	// `nest_.depth` is how many chains deep this editor already is, so a container added here holds a
	// chain at `depth + 1`, and the deepest whose CONTENTS survive a save is `kMaxUsefulDepth`. Written
	// against that constant rather than `kMaxDepth - 1`, because two call sites re-deriving one
	// subtraction is how they drift — and the off-by-one is real: the loader tests `depth + 1 >=
	// kMaxDepth`, so the chain AT `kMaxDepth` comes back empty rather than not at all.
	// AND THE MODEL'S OWN DEPTH (2026-10-31, `RackBridge::nestingDepth`): the deeper of the two answers, so this surface
	// never offers what the record would refuse. One gate, `fitsNesting`, which the loader's test defines.
	return !fitsNesting(std::max(static_cast<size_t>(nest_.depth), engine_.nestingDepth()), 1);
}

void RackEditor::setOuterDevice(backplane::DeviceHost* host, DeviceInstanceId device, ui::ModState* mod,
                                ui::EditHistory* history) {
	const device::Descriptor* desc = host != nullptr ? host->descriptorOf(device) : nullptr;
	// THE ROW IS REBUILT EITHER WAY: the knobs are children and are on watcher lists, so they go out
	// through their own destructors (which unwatch) before the new ones are made.
	for (auto& slot : macroSlots_) {
		removeChild(slot.get());
	}
	macroSlots_.clear();
	for (auto& knob : outerLevelKnobs_) {
		removeChild(knob.get());
	}
	outerLevelKnobs_.clear(); // before the controls they read
	outerLevels_.clear();
	outerMacros_.clear();
	macroViews_.clear(); // after the slots that read them, before the controls they read
	macroDrawn_.clear();
	const size_t count = macros_.size();
	const size_t firstMacro = static_cast<size_t>(composite::Param::Macro1);
	const bool outer = desc != nullptr && desc->params.size() >= firstMacro + count;
	for (size_t i = 0; i < count; ++i) {
		std::unique_ptr<ui::MacroSlot> slot;
		// **THE SAME POLARITY VIEW ON BOTH ROWS (backlog EG).** Nested, the macro is the chain device's
		// parameter — a destination the OUTER host resolves by reading its routes — and the chain hands
		// the resolved 0..1 to this rack as a plain macro value (`rack_device.cpp`), where the mode maps
		// it. So the mode is this rack's either way, the slot reads through it either way, and only the
		// control underneath differs: the outer device's parameter, or the rack's own macro. THE TYPE AND
		// THE NAME ARE THIS RACK'S TOO, by design (`macroMenuHooks`, `macroNaming`): a pad on the outer
		// row writes the outer parameter, and what makes it a pad is the nested rack's own patch.
		if (outer) {
			outerMacros_.push_back(std::make_unique<ui::DeviceControl>(*host, device, *desc, firstMacro + i, mod, history));
			macroViews_.push_back(std::make_unique<ui::BipolarMacro>(
			    *outerMacros_.back(), [this, i] { return engine_.macroBipolar(i); }));
			slot = makeMacroSlot(*macroViews_.back(), i, mod, history);
		}
		else {
			macroViews_.push_back(std::make_unique<ui::BipolarMacro>(
			    *macros_[i], [this, i] { return engine_.macroBipolar(i); }));
			slot = makeMacroSlot(*macroViews_.back(), i, &mod_, &history_);
		}
		addChild(slot.get());
		macroSlots_.push_back(std::move(slot));
	}
	// MIX AND GAIN AFTER THE MACROS, NESTED — the chain's card has them, so its window does too (a device's window
	// is strictly a superset of its card). Knobs as on the card, captioned as the card captions them.
	if (outer && desc->params.size() > static_cast<size_t>(composite::Param::Gain)) {
		constexpr std::array<std::pair<composite::Param, const char*>, 2> kLevels {
			{ { composite::Param::Mix, "Mix" }, { composite::Param::Gain, "Gain" } }
		};
		for (const auto& [param, caption] : kLevels) {
			outerLevels_.push_back(
			    std::make_unique<ui::DeviceControl>(*host, device, *desc, static_cast<size_t>(param), mod, history));
			auto knob = std::make_unique<ui::Knob>(*outerLevels_.back(), caption, notify_.get());
			knob->setFaceDiameter(kMacroDiameter);
			addChild(knob.get());
			outerLevelKnobs_.push_back(std::move(knob));
		}
	}
	columnDirty_ = true; // the rebuilt slots and Mix/Gain were added after the cards
	// THE ARMS AND THE TOP LAYERS KEEP THEIR PLACE ABOVE THE ROW.
	raiseTopLayers();
	resized();
	redraw();
}

void RackEditor::raiseTopLayers() {
	// **ONLY IF THEY ARE NOT ALREADY ON TOP, AND THAT IS NOT AN OPTIMISATION — IT IS THE FIX FOR A BUG
	// WAS HIT.** 2026-08-31: clicking the knob appeared to assign 100%, but dragging
	// did not adjust the depth (it did work in the card).
	//
	// `RackGui::tick` calls `syncCards` on every beat, and this used to `removeChild`/`addChild`
	// unconditionally at the end of it. `Frame::removeChild` calls `notifyRemoveFromHierarchy`, and
	// visage's window event handler clears `mouse_down_frame_` for a frame that leaves the tree — so
	// every beat threw away the frame a drag was being delivered to.
	//
	// THE ASYMMETRY IN THE REPORT IS THE PROOF: a CARD is a direct child of this editor and was never
	// re-added, so dragging worked there. A POPUP's knobs live under `surfaces_`, which was removed
	// sixty times a second. The press landed — one event, delivered before the next beat — and every
	// drag after it went nowhere.
	//
	// SO THE RAISE IS IDEMPOTENT NOW. Re-adding a frame that is already last is a no-op in every
	// visible sense and a dropped gesture in the one that matters.
	// **THE MACRO COLUMN GOES OVER THE CARDS WHEN ONE WAS ADDED** (`columnDirty_`): the lane scrolls sideways beneath it, so
	// its ground, door, Mix/Gain, macros and arms must come after every card in sibling order. Only then — a frame
	// re-added every beat would drop the drag it was being delivered (the note above). Everything after it is re-raised
	// with it, so the notifier and the surface host stay on top.
	const bool raiseColumn = columnDirty_;
	columnDirty_ = false;
	if (raiseColumn) {
		const auto raise = [this](visage::Frame* frame) {
			if (frame != nullptr) {
				removeChild(frame);
				addChild(frame);
			}
		};
		raise(columnBackdrop_.get());
		raise(laneScrollBar_.get()); // over the cards, as the column is
		raise(settingsDoor_.get());
		raise(undoRedo_.get());
		raise(presets_);
		raise(routes_.get()); // the Macros column's list stands over the column's ground
		for (auto& knob : outerLevelKnobs_) {
			raise(knob.get());
		}
		for (auto& slot : macroSlots_) {
			raise(slot.get());
		}
		for (auto& arm : macroArms_) {
			raise(arm.get());
		}
		raise(macrosRail_.get()); // the Macros toggle (QN) over the column's ground, and its folded strip over the cards
		raise(&tips_); // a tooltip over everything the column holds
	}
	const int notifyAt = notify_ != nullptr ? indexOfChild(notify_.get()) : -1;
	const int surfacesAt = surfaces_ != nullptr ? indexOfChild(surfaces_.get()) : -1;
	const int overlayAt = selectionOverlay_ != nullptr ? indexOfChild(selectionOverlay_.get()) : -1;
	const int last = static_cast<int>(children().size()) - 1;
	if (!raiseColumn && surfacesAt == last && (notify_ == nullptr || notifyAt == last - 1)
	    && (selectionOverlay_ == nullptr || overlayAt == last - 2)) {
		return;
	}
	++raises_;
	// ORDER MATTERS BETWEEN THESE AS WELL: the selection's overlay over the cards (backlog OA), the notifier over
	// it, and the surface host above both. A takeover covers the whole editor, and a message drawn over a browser
	// would be a message about a rack the player cannot see — which is the same reasoning that put the surface
	// host after the notifier when they were first built.
	if (selectionOverlay_ != nullptr) {
		removeChild(selectionOverlay_.get());
		addChild(selectionOverlay_.get());
	}
	if (notify_ != nullptr) {
		removeChild(notify_.get());
		addChild(notify_.get());
	}
	if (surfaces_ != nullptr) {
		removeChild(surfaces_.get());
		addChild(surfaces_.get());
	}
}

std::vector<const device::Descriptor*> RackEditor::offer() const {
	std::vector<const device::Descriptor*> offered = offeredDevices(kDevices);
	if (offerFilter) {
		std::erase_if(offered, [this](const device::Descriptor* d) { return !offerFilter(*d); });
	}
	return offered;
}

void RackEditor::showAddMenu(visage::Frame& source, const visage::Bounds& anchor) {
	// THE OFFER IS THE RACK'S — every device it can run, modulators included — FILED THE WAY FEATHERS'
	// FX LANE FILES ITS OWN (`device_menu.h`), so the two plugins have one add menu and not two taxonomies.
	const std::vector<const device::Descriptor*> offered = offer();
	if (offered.empty()) {
		return;
	}
	visage::PopupMenu menu;
	// Every row is live: the rack grows (unbounded devices step 6), so its menu is never greyed for room.
	ui::addCategorisedDevices(menu, offered, [](size_t) { return true; });
	menu.onSelection() = [this, offered](int id) {
		if (id >= 0 && static_cast<size_t>(id) < offered.size()) {
			addDeviceByTag(offered[static_cast<size_t>(id)]->traits.tag);
		}
	};
	// A MENU THAT CLOSED WITHOUT A PICK ENDS AN "Add before" TOO, so the next (+) is an append.
	menu.onCancel() = [this] {
		insertAnchor_.reset();
		addToPost_ = false;
	};
	ui::showMenuAt(std::move(menu), source, anchor);
}

DeviceInstanceId RackEditor::addDeviceByTag(const char* tag) {
	// AN INSERT IF "ADD BEFORE" SET AN ANCHOR, AN APPEND OTHERWISE — one pick road for the (+) and "Add
	// before…", which open the same menu, so the anchor is state here rather than a captured argument. The
	// anchor is the card's DEVICE and the bridge resolves where it stands at apply time (backlog CR).
	std::optional<DeviceInstanceId> anchor = insertAnchor_;
	insertAnchor_.reset();
	// WHICH LANE THE PICK LANDS IN: the anchor's, or POST's for its own (+) (backlog NZ), or this editor's.
	RackBridge& lane = anchor.has_value() ? laneOf(*anchor) : (addToPost_ && tail_.post != nullptr ? *tail_.post : engine_);
	addToPost_ = false;
	// ARPS SENT TO FRONT OF CHAIN: a (+) pick of a note-emitting device goes in front of the lane's
	// first device while the setting is on. An "Add before…" already named its place, so it is left alone.
	if (ui::pickGoesToFront(engine_.descriptorFor(tag), anchor.has_value(), lane.chainLength(0))) {
		anchor = lane.deviceAtPosition(0);
	}
	// **THE NESTING BOUND, ON THE ONE PICK ROAD.** Every pick from the add menu and every direct call
	// lands here, so one check covers all of them — which is the same reason the anchor is
	// state here rather than a captured argument.
	//
	// **WITHOUT THIS THE GUARANTEE IS ONLY HALF TRUE.** `ChainColumn` has refused a container past
	// `kMaxUsefulDepth` since that bound was set, but a rack device added inside a rack chain went through
	// HERE and was never asked how deep it was — so the rack plugin could still build a patch that does
	// not reopen, which is exactly the thing the bound exists to make unreachable.
	const device::Descriptor* wanted = engine_.descriptorFor(tag);
	if (wanted != nullptr && wanted->container != nullptr && nestingFull()) {
		// SAID ON THE TRAY, WHICH IS HOW THIS EDITOR ALREADY WORDS A REFUSAL AT A BOUND ("The rack is
		// full", below). The chain column draws the same sentence as a menu row instead, because that is
		// how ITS surface refuses — one constant, two voices, `rack::kNestingFullMessage`.
		notify_->say(kNestingFullMessage);
		return kNoInstance;
	}
	const DeviceInstanceId added =
	    anchor.has_value() ? lane.addDeviceBefore(*anchor, tag) : lane.addDevice(tag);
	if (added == kNoInstance) {
		// A REFUSAL SAYS SO. Silently doing nothing is the honesty rule's failure case: the click looked
		// like it worked. It is not "the rack is full" any more (unbounded devices step 6): a rack is never
		// full, so a refusal here is an anchor that vanished between the menu and the pick, or a device the
		// rack could not build.
		notify_->say(anchor.has_value() ? "That device is gone; nothing added" : "That device could not be added");
		return kNoInstance;
	}
	// AS ONE UNDO STEP (backlog JB).
	const DeviceInstanceId ids[] { added };
	selectionOfLane(lane).verbs().recordAdded(ids, "Add");
	syncCards();
	return added;
}

ui::LaneSelection& RackEditor::selectionOfLane(RackBridge& lane) {
	return &lane == tail_.post && postSelection_ != nullptr ? *postSelection_ : *selection_;
}

DeviceInstanceId RackEditor::insertDeviceBeforeForTest(DeviceInstanceId anchor, const char* tag) {
	// THE VERB THE MENU'S "Add before…" ENDS IN, minus the two popups: the anchor set, then
	// the pick. Returns the new device's id, or kNoInstance.
	insertAnchor_ = anchor;
	return addDeviceByTag(tag);
}

size_t RackEditor::outputsOf(DeviceInstanceId device) const {
	const device::Descriptor* d = engine_.descriptorOf(device);
	return d != nullptr ? d->outputCount() : 0;
}

RackEditor::CardEntry* RackEditor::entryOf(DeviceInstanceId device) noexcept {
	if (device == kNoInstance) {
		return nullptr;
	}
	for (std::vector<CardEntry>* lane : { &cards_, &postCards_ }) {
		for (CardEntry& entry : *lane) {
			if (entry.device == device) {
				return &entry;
			}
		}
	}
	return nullptr;
}

const RackEditor::CardEntry* RackEditor::entryOf(DeviceInstanceId device) const noexcept {
	return const_cast<RackEditor*>(this)->entryOf(device);
}

ui::FxCard* RackEditor::cardOfDeviceForTest(DeviceInstanceId device) const noexcept {
	const CardEntry* entry = entryOf(device);
	return entry != nullptr ? entry->card.get() : nullptr;
}

std::vector<ui::FxCard*> RackEditor::cardsForTest() const {
	std::vector<ui::FxCard*> out;
	for (size_t position = 0; position < engine_.chainLength(0); ++position) {
		if (ui::FxCard* card = cardOfDeviceForTest(engine_.deviceAtPosition(position)); card != nullptr) {
			out.push_back(card);
		}
	}
	return out;
}

size_t RackEditor::updateCards() {
	// THE INLINE CARDS FIRST. Each answers whether it asked for a frame; the count is produced BY the
	// work rather than standing beside it, which is what makes it unfakeable — see `FxCard::update`.
	size_t told = 0;
	for (const CardEntry& entry : cards_) {
		if (entry.card != nullptr && entry.card->update()) {
			++told;
		}
	}
	// AND THE OPEN PANEL, WHICH IS A SEPARATE WIDGET OVER THE SAME SLOT. `Gui::Impl::tick()` says the
	// same sentence about Feathers' overlay: an open OWLTT panel builds its own graph, so a road that
	// advanced only the lane would leave the panel's analyser dead while the card's moved. That is
	// precisely the half that was seen — the panel moved only when the mouse gave it an event. THROUGH THE
	// LAYERS, which own the mount that carries the tick — the same line Feathers' tick has.
	if (surfaces_ != nullptr && surfaces_->updateContent()) {
		++told;
	}
	// THE EMPTY CANVAS'S SKY, uncounted: not a device.
	if (canvas_ != nullptr) {
		(void)canvas_->animateBackdrop();
	}
	return told;
}

void RackEditor::syncMacros() {
	// THE MACROS COLUMN FOLLOWS THE PATCH (backlog QN), on this beat like the macros: a load (or a fresh instance's first
	// beat) folds or unfolds it as saved. Through the rail's own `setCollapsed`, so no notice is said; writing it back to
	// the bridge is then a no-op.
	if (macrosRail_ != nullptr && engine_.macrosCollapsed() != macrosRail_->collapsed()) {
		(void)macrosRail_->setCollapsed(engine_.macrosCollapsed());
	}
	macroDrawn_.resize(macroSlots_.size(), -1.0f);
	for (size_t i = 0; i < macroSlots_.size(); ++i) {
		// A TYPE A PATCH LOAD MOVED, first, so the value below redraws the control now on show.
		macroResyncs_ += macroSlots_[i]->syncType() ? 1u : 0u;
		const float now = engine_.macro(i);
		if (now != macroDrawn_[i]) {
			macroDrawn_[i] = now;
			redrawMacroKnob(i);
			++macroRedraws_;
		}
	}
}

void RackEditor::removeDevice(DeviceInstanceId device) {
	const CardEntry* entry = entryOf(device);
	if (entry == nullptr || entry->card == nullptr) {
		return;
	}
	// THE PANEL FIRST. An expanded view open on this device would otherwise outlive the device it is
	// showing — the hazard the `onExpand` arm records as one `removeDevice` away.
	surfaces_->overlay().dismiss();
	// AS ONE UNDO STEP (backlog JB): the lane's own verbs, which put the device back whole — routes, labels, host slots.
	const DeviceInstanceId ids[] { device };
	if (selectionOfLane(laneOf(device)).verbs().remove(ids) == 0) {
		laneOf(device).removeDevice(device); // a device the verbs could not lift goes as before, unrecorded
	}
	// **AND FOCUS DOES NOT SURVIVE THE DEVICE.** An id kept across its own removal names nothing — inert,
	// but an undo could bring it back, and a device that came back focused would be a key's target the
	// hand never chose again.
	focusDevice(kNoInstance);
	syncCards();
	resized();
}

void RackEditor::focusDevice(DeviceInstanceId device) {
	focusedDevice_ = device;
	// THE FOCUS IS A SELECTION OF ONE (backlog OA, D1) — the flag a card draws is the selection's.
	if (device == kNoInstance) {
		selection_->clear();
		if (postSelection_ != nullptr) {
			postSelection_->clear();
		}
	}
	else {
		const std::array<DeviceInstanceId, 1> one { device };
		selectionOf(device).select(one);
	}
}

std::vector<DeviceInstanceId> RackEditor::chainOrder() const {
	std::vector<DeviceInstanceId> out;
	for (size_t p = 0; p < engine_.chainLength(0); ++p) {
		out.push_back(engine_.deviceAtPosition(p));
	}
	return out;
}

std::vector<DeviceInstanceId> RackEditor::postOrder() const {
	std::vector<DeviceInstanceId> out;
	if (tail_.post != nullptr) {
		for (size_t p = 0; p < tail_.post->chainLength(0); ++p) {
			out.push_back(tail_.post->deviceAtPosition(p));
		}
	}
	return out;
}

std::vector<DeviceInstanceId> RackEditor::editOrder() const {
	std::vector<DeviceInstanceId> out = chainOrder();
	const std::vector<DeviceInstanceId> post = postOrder();
	out.insert(out.end(), post.begin(), post.end());
	return out;
}

ui::LaneSelection& RackEditor::selectionOf(DeviceInstanceId device) {
	return postSelection_ != nullptr && &laneOf(device) == tail_.post ? *postSelection_ : *selection_;
}

ui::LaneSelection& RackEditor::keySelection() {
	return postSelection_ != nullptr && !postSelection_->selection().empty() ? *postSelection_ : *selection_;
}

void RackEditor::exclusiveSelection(ui::LaneSelection& changed) {
	// A LASSO SETTLES THIS ITSELF (it runs over both chains at once), and an empty selection clears nothing.
	if (lassoing_ || postSelection_ == nullptr || changed.selection().empty()) {
		return;
	}
	ui::LaneSelection& other = &changed == selection_.get() ? *postSelection_ : *selection_;
	if (!other.selection().empty()) {
		other.clear(); // its `changed` sees an empty selection, so it clears nothing back
	}
}

void RackEditor::applySelection() {
	const ui::CardSelection& s = selection_->selection();
	for (CardEntry& entry : cards_) {
		if (entry.card != nullptr) {
			entry.card->setFocused(s.contains(entry.device));
		}
	}
	// POST'S CARDS DRAW POST'S SELECTION (backlog OA), and the key's target is the selection that holds anything.
	const ui::CardSelection* post = postSelection_ != nullptr ? &postSelection_->selection() : nullptr;
	for (CardEntry& entry : postCards_) {
		if (entry.card != nullptr) {
			entry.card->setFocused(post != nullptr && post->contains(entry.device));
		}
	}
	const ui::CardSelection& k = keySelection().selection();
	// THE KEY'S TARGET FOLLOWS: Delete on one card is the editor's own road, and it removes the anchor.
	focusedDevice_ = k.size() == 1 ? k.anchor() : (k.contains(focusedDevice_) ? focusedDevice_ : k.anchor());
	if (selectionOverlay_ != nullptr) {
		selectionOverlay_->redraw();
	}
}

void RackEditor::afterVerb(const std::vector<DeviceInstanceId>& before) {
	// A PANEL OPEN ON A DEVICE A VERB TOOK AWAY would outlive it — the hazard `removeDevice` already guards.
	// Over every chain this editor draws: a POST verb can take a device away as well (backlog OA).
	const std::vector<DeviceInstanceId> now = editOrder();
	for (const DeviceInstanceId id : before) {
		if (std::find(now.begin(), now.end(), id) == now.end()) {
			surfaces_->overlay().dismiss();
			break;
		}
	}
	syncCards();
	selection_->prune();
	if (postSelection_ != nullptr) {
		postSelection_->prune();
	}
	applySelection();
	resized();
}

ui::MacroMenuHooks RackEditor::macroMenuHooks(size_t index) {
	// THE VERBS ARE THE BRIDGE'S, so a case flips a mode without a menu. The editor repaints because
	// every knob reading this macro's routes draws a different reach; the macro's own knob because it
	// reads through the new polarity. **THE KNOB BY INDEX, NOT BY POINTER:** the menu holds a copy of
	// these hooks until a pick, and `setOuterDevice` may rebuild the row under an open menu — a knob
	// captured here would be a freed one by then.
	//
	// **AND THE EDITOR ONLY WHILE IT LIVES** (`alive_`): the menu can outlive the whole editor too, so
	// every hook asks the token first — a reader answers the default, a writer does nothing. `ModPanel`
	// needs no such token: it dies only with the root frame, and the popup with it.
	//
	// ON THE OUTER-DEVICE ROW TOO these go to `engine_`, this rack: type, polarity and name are the
	// nested rack's own patch, as the polarity already was; only the value is the outer device's param.
	const std::weak_ptr<void> alive = alive_;
	return {
		.readBipolar = [this, index, alive] { return !alive.expired() && engine_.macroBipolar(index); },
		.writeBipolar =
		    [this, index, alive](bool bipolar) {
			    if (alive.expired()) {
				    return;
			    }
			    engine_.setMacroBipolar(index, bipolar);
			    redrawMacroKnob(index);
			    redraw();
		    },
		.readType =
		    [this, index, alive] { return alive.expired() ? device::MacroType::Knob : engine_.macroType(index); },
		.writeType =
		    [this, index, alive](device::MacroType type) {
			    if (alive.expired()) {
				    return;
			    }
			    engine_.setMacroType(index, type);
			    redrawMacroKnob(index);
			    redraw();
		    },
		// THE SLOT'S OWN: `MacroSlot` answers Rename with its field, through its own weak handle.
		.beginRename = [] {},
	};
}

ui::MacroSlot::Naming RackEditor::macroNaming(size_t index) {
	// **THE HOOKS' TOKEN, FOR THE HOOKS' REASON** (`alive_`). The field that calls `rename` dies with its
	// slot, which dies with this editor, so no road reaches here after death today — but these are
	// functions a slot keeps and copies may be taken, and "cannot be called late" is a claim about every
	// future caller, where "does nothing when called late" is one this function can keep by itself.
	// ON THE OUTER ROW TOO these are `engine_`'s, this rack's: the name is the nested rack's own patch.
	const std::weak_ptr<void> alive = alive_;
	return {
		.name = [this, index, alive] { return alive.expired() ? device::defaultMacroName(index) : engine_.macroName(index); },
		.renamed = [this, index, alive] { return !alive.expired() && engine_.macroRenamed(index); },
		// NOTHING HERE REBUILDS THE ROW: the bridge's listener only `refresh()`es the slot, so the field's
		// commit callback returns into a slot that is still there (`MacroSlot::Naming::rename`).
		.rename =
		    [this, index, alive](const std::string& name) {
			    if (alive.expired()) {
				    return;
			    }
			    engine_.setMacroName(index, name);
		    },
	};
}

std::unique_ptr<ui::MacroSlot> RackEditor::makeMacroSlot(device::Modulatable& control, size_t index,
                                                         ui::ModState* mod, ui::EditHistory* history) {
	// THE CAPTION IS "Macro 1" IN FULL — what this row printed before it held slots — until the macro is
	// given a name; the column is wide enough for it here, unlike the Feathers header's "M1".
	auto slot = std::make_unique<ui::MacroSlot>(control, device::defaultMacroName(index), macroNaming(index),
	                                            macroMenuHooks(index), nullptr);
	slot->setFaceDiameter(kMacroDiameter);
	slot->setModulation(mod, history);
	return slot;
}

void RackEditor::redrawMacroKnob(size_t index) {
	// BOTH CONTROLS: the one on show is the one that matters, and a redraw of a hidden frame is free.
	if (index < macroSlots_.size() && macroSlots_[index] != nullptr) {
		macroSlots_[index]->knob().redraw();
		macroSlots_[index]->chip().redraw();
	}
}

visage::Bounds RackEditor::gapBounds(size_t gap) const {
	// **THE LIVE CARDS, IN ORDER** — the same list `dropTargetAt` measures spans over, so a gap index
	// means the same thing to both. Counting over `cards_` including the empty slots would make the
	// two disagree the moment a rack has a hole in it.
	// **IN CHAIN ORDER, LIKE THE LAYOUT.** `cards_` is keyed by device, and the bumper marks a gap
	// between cards as DRAWN — so walking the store would put the bar between the wrong two the moment
	// a rack has been reordered, which is precisely when somebody is looking at it.
	// **IN THE CHAIN THE GAP COUNTS IN** (backlog NZ): a site's POST, or this editor's own.
	const std::vector<const ui::FxCard*> live = liveCards(dropIntoPost_);
	constexpr float thickness = 2.0f;
	const float laneTop = canvasExtent() + kPad;
	const float laneHeight =
	    std::max(0.0f, static_cast<float>(height()) - laneTop - kPad);
	// AN EMPTY SEGMENT OF A SITE IS STILL A PLACE TO DROP: the bar stands where its first card would.
	if (live.empty() && tail_.outer != nullptr && gap == 0) {
		const float x = dropIntoPost_ ? partitionX_[1] + kPartitionGap * 0.5f : kPad - scroll_;
		return { x - thickness * 0.5f, laneTop, thickness, laneHeight };
	}
	if (gap > live.size() || live.empty()) {
		return {};
	}
	// A BAR IN THE GAP BEFORE CARD `gap`, or after the last one. `kPad * 0.5f` centres it in the space
	// between two cards rather than butting it against one, which would read as belonging to that card.
	const float x = gap < live.size()
	                    ? static_cast<float>(live[gap]->x()) - kPad * 0.5f - thickness * 0.5f
	                    : static_cast<float>(live.back()->x() + live.back()->width()) + kPad * 0.5f
	                          - thickness * 0.5f;
	return { x, laneTop, thickness, laneHeight };
}

visage::Bounds RackEditor::cardBoundsForTest(DeviceInstanceId device) const {
	const CardEntry* entry = entryOf(device);
	return entry != nullptr && entry->card != nullptr ? entry->card->bounds() : visage::Bounds {};
}

bool RackEditor::tickDrag() {
	if (!dragDevice_.has_value()) {
		return false;
	}
	const float w = static_cast<float>(width());
	// INSIDE THE LANE ONLY — the same rectangle `dropTargetAt` answers in; outside it is the cancel.
	if (dragPoint_.x < 0.0f || dragPoint_.x >= w || dragPoint_.y < canvasExtent()
	    || dragPoint_.y >= static_cast<float>(height())) {
		return false;
	}
	float step = 0.0f;
	if (dragPoint_.x < kEdgeScrollBand) {
		step = -kEdgeScrollStep;
	}
	else if (dragPoint_.x >= w - kEdgeScrollBand) {
		step = kEdgeScrollStep;
	}
	else {
		return false;
	}
	if (!scrollBy(step)) {
		return false; // already at that end
	}
	// THE DROP TARGET FOLLOWS THE HAND: the cards moved under a still pointer.
	setDropTarget(dropTargetAt(dragPoint_));
	redraw();
	return true;
}

std::vector<const ui::FxCard*> RackEditor::liveCards(bool post) const {
	std::vector<const ui::FxCard*> live;
	RackBridge* lane = post ? tail_.post : &engine_;
	if (lane == nullptr) {
		return live;
	}
	for (size_t position = 0; position < lane->chainLength(); ++position) {
		if (const CardEntry* entry = entryOf(lane->deviceAtPosition(position)); entry != nullptr && entry->card != nullptr) {
			live.push_back(entry->card.get());
		}
	}
	return live;
}

std::optional<RackEditor::DropTarget> RackEditor::dropTargetAt(visage::Point position) const {
	// OUTSIDE THE LANE IS THE CANCEL, and past either end is the END of the chain rather than nothing
	// — the hand aimed past the list because that is where it wanted to go, which a gap can say and a
	// card index cannot. The fx lane's rule, transposed.
	if (position.x < 0.0f || position.x >= static_cast<float>(width()) || position.y < canvasExtent()
	    || position.y >= static_cast<float>(height())) {
		return std::nullopt;
	}
	// **IN A SITE, THE PARTITIONS SAY WHICH CHAIN** (backlog NZ): left of the first is PRE (this editor's own), right of
	// the second is POST, and between them is the device itself, which is no place to drop. A veiled PRE takes nothing.
	bool post = false;
	if (tail_.outer != nullptr) {
		if (position.x >= partitionX_[1] && tail_.post != nullptr) {
			post = true;
		}
		else if (position.x > partitionX_[0] || !preAvailable_) {
			return std::nullopt;
		}
	}
	// **SPANS ALONG X, BECAUSE THIS LANE FLOWS SIDEWAYS.** `ui::gapAlong` is the fx lane's arithmetic
	// with an honest name — it was `gapAtY`, and the name was the only vertical thing about it.
	// **SPANS IN CHAIN ORDER.** A gap index counts over the cards as DRAWN, and `rowForGap` turns it
	// into a chain POSITION — so the spans must be gathered the way the layout places them. `cards_`
	// is keyed by device, and walking it would number the gaps by when devices were ADDED: correct
	// until the first reorder, then off by however far things have moved.
	std::vector<ui::RowSpan> spans;
	for (const ui::FxCard* card : liveCards(post)) {
		spans.push_back({ static_cast<float>(card->x()), static_cast<float>(card->x() + card->width()) });
	}
	if (spans.empty()) {
		// AN EMPTY SEGMENT OF A SITE takes a drop at its start; an empty chain of its own has nothing to reorder.
		return tail_.outer != nullptr ? std::optional<DropTarget> { DropTarget { post, 0 } } : std::nullopt;
	}
	return DropTarget { post, ui::gapAlong(position.x, spans) };
}

void RackEditor::cardDragForTest(DeviceInstanceId device, ui::FxCard::DragPhase phase, visage::Point inCard) {
	// THE CONVERSION IS THE LANE'S, and it is why the card reports in its OWN coordinates: only the
	// lane knows where a card sits in it, and a card's x moves with the scroll.
	const CardEntry* entry = entryOf(device);
	if (entry == nullptr || entry->card == nullptr) {
		return;
	}
	const ui::FxCard& card = *entry->card;
	const visage::Point inLane { static_cast<float>(card.x()) + inCard.x,
		                         static_cast<float>(card.y()) + inCard.y };

	switch (phase) {
	case ui::FxCard::DragPhase::Begin:
		dragDevice_ = device;
		dragPoint_ = inLane;
		break;
	case ui::FxCard::DragPhase::Cancel:
		dragDevice_.reset();
		dropGap_.reset();
		dropIntoPost_ = false;
		redraw();
		break;
	case ui::FxCard::DragPhase::Move:
		if (!dragDevice_.has_value()) {
			return;
		}
		dragPoint_ = inLane;
		setDropTarget(dropTargetAt(inLane));
		redraw();
		break;
	case ui::FxCard::DragPhase::End: {
		const std::optional<DeviceInstanceId> from = dragDevice_;
		const std::optional<size_t> gap = dropGap_;
		const bool intoPost = dropIntoPost_;
		dragDevice_.reset();
		dropGap_.reset();
		dropIntoPost_ = false;
		if (from.has_value() && gap.has_value()) {
			// **THE TWO GAPS EITHER SIDE OF THE DRAGGED CARD ANSWER NOTHING.** A drop where the card
			// already is moves nothing rather than moving by zero — see `rowForGap`, which exists to
			// get exactly this off-by-one right and is shared with the fx lane rather than restated.
			//
			// **EVERYTHING `rowForGap` SEES IS A ROW, AND A ROW IS NOT A SLOT.** The gap was counted
			// over the LIVE cards — `dropTargetAt` skips the empty slots, and so does the bumper — but the
			// dragged card's SLOT index was handed in as its row, and its answer was handed to
			// `moveDevice` as a slot. In a dense rack the two agree and nothing shows. Once a hole sits
			// before the dragged card they do not: the slot index is at or past the row count,
			// `rowForGap` refuses it, and the release moves nothing while the bumper — counted
			// correctly — goes on saying it will. Holes are permanent now that a re-add goes
			// to the END rather than filling one, which is why this arrived with churn and not before.
			//
			// **AND STEP 2 DELETED THE TRANSLATION RATHER THAN FIXING IT AGAIN.** The paragraph above
			// describes a bug whose CAUSE was that two indexings had to be kept in step by hand: a
			// row counted over live cards, and a slot index that was also a position. The fix was a
			// `slotOf` table mapping one to the other in both directions.
			//
			// The graph owns the order now, so a gap index and a chain position are the same number
			// by construction — `dropTargetAt` counts over the cards in DRAW order, and draw order IS
			// chain order. The only translation left is the dragged card's DEVICE to its position,
			// which is one call.
			const size_t fromPosition = engine_.positionOf(*from);
			const bool fromPost = tail_.post != nullptr && &laneOf(*from) == tail_.post;
			// **A SITE'S POST, OR ACROSS THE CENTRE** (backlog NZ): one undo step each, through the verbs.
			if (fromPost || intoPost) {
				commitSiteDrop(*from, fromPost, intoPost, *gap);
			}
			// **A SELECTED CARD CARRIES THE SELECTION** (backlog OA, D1): the whole block lands at the gap, in order,
			// as one undo step. The gap counts over the whole chain, which is what the block move counts in.
			// **AND A CARD ALONE IS A BLOCK OF ONE** — the same verb, so it is one undo step too. It went through
			// `engine_.moveDevice`, which records nothing, so a plain chain's drag was the one edit undo could not reach.
			// `rowForGap` still answers whether the drop moves anything: the two gaps beside the card move nothing.
			else if (fromPosition != RackBridge::kNoSlot
			         && (selection_->movesBlock(*from)
			             || ui::rowForGap(fromPosition, *gap, engine_.chainLength(0)).has_value())) {
				const std::vector<DeviceInstanceId> before = chainOrder();
				const std::vector<DeviceInstanceId> ids = selection_->movesBlock(*from)
				                                              ? selection_->selection().inOrder(before)
				                                              : std::vector<DeviceInstanceId> { *from };
				if (selection_->verbs().moveBlock(ids, *gap)) {
					afterVerb(before);
				}
			}
		}
		redraw();
		break;
	}
	}
}

void RackEditor::commitSiteDrop(DeviceInstanceId from, bool fromPost, bool intoPost, size_t gap) {
	if (postSelection_ == nullptr || tail_.post == nullptr) {
		return;
	}
	const std::vector<DeviceInstanceId> before = editOrder();
	// A SELECTED CARD CARRIES ITS SELECTION, in POST as in PRE (backlog OA): the selection is its own chain's.
	ui::LaneSelection& held = fromPost ? *postSelection_ : *selection_;
	std::vector<DeviceInstanceId> ids { from };
	if (held.movesBlock(from)) {
		ids = held.selection().inOrder(fromPost ? postOrder() : chainOrder());
	}
	if (fromPost && intoPost) {
		// WITHIN POST: its block move, the same verb PRE's selection drag is — one undo step in this history.
		if (postSelection_->verbs().moveBlock(ids, gap)) {
			afterVerb(before);
		}
		return;
	}
	// ACROSS THE CENTRE: out of one lane's body and into the other's, under the same ids (the site's lanes share one
	// counter). Into a veiled PRE nothing lands: `dropTargetAt` names no gap there.
	ui::FragmentVerbs& verbs = held.verbs();
	rack::FragmentSite& to = intoPost ? static_cast<rack::FragmentSite&>(*tail_.post) : static_cast<rack::FragmentSite&>(engine_);
	if (!verbs.moveInto(to, ids, gap).empty()) {
		afterVerb(before);
	}
}

void RackEditor::showSlotMenu(DeviceInstanceId device) {
	const CardEntry* entry = entryOf(device);
	if (entry == nullptr || entry->card == nullptr) {
		return;
	}
	// A MENU RATHER THAN A BARE REMOVE BUTTON, and the menu is the confirmation — the fx lane's
	// reasoning applies unchanged here: a remove is destructive, there is no undo behind it, and two
	// deliberate presses at two different places cannot happen by accident.
	visage::PopupMenu menu;
	// "ADD BEFORE" FIRST AND REMOVE LAST — the fx lane's order (backlog CR). The card is its DEVICE's,
	// so the anchor's identity is the argument as bound; greyed when full.
	auto& addBefore = menu.addOption(1, "Add before…");
	addBefore.enable(true); // the rack grows (unbounded devices step 6)
	menu.addOption(0, "Remove");
	// **AND THE SELECTION'S VERBS** (backlog OA): Group into Chain / Rack, Copy, Cut, Paste after, Duplicate, and
	// Remove N — over the selection when it holds this card, else this card alone. A POST card's are POST's (backlog OA).
	selectionOf(device).addVerbs(menu, kVerbIds, device);
	// **THROUGH `removeSlot`, WHICH IS THE POINT OF `removeSlot` EXISTING.** This inlined the same
	// four lines and was missing one of them — the focus reset — so a device removed from the MENU
	// left `focusedSlot_` naming a slot that no longer holds anything. The commit that introduced
	// `removeSlot` SAID it was "the menu's verb and the keyboard's, one implementation" and it was
	// not: the edit that was supposed to redirect this call site silently did not apply, and the
	// claim went into a commit message unchecked.
	menu.onSelection() = [this, device](int id) {
		if (id >= kVerbIds) {
			(void)slotVerbForTest(device, static_cast<ui::FragmentVerb>(id - kVerbIds));
		}
		else if (id == 0) {
			removeDevice(device);
		}
		else if (id == 1) {
			// THE ANCHOR IS THE CARD'S DEVICE, SET NOW; the pick lands through `addDeviceByTag`. The add menu
			// opens at the same dots, as the fx lane's does (backlog KP).
			insertAnchor_ = device;
			if (const CardEntry* at = entryOf(device); at != nullptr && at->card != nullptr) {
				showAddMenu(*at->card, at->card->slotMenuBounds());
			}
		}
	};
	// AT THE DOTS, which is where the hand is — read off the card rather than recomputed, so the menu
	// cannot open away from the mark that summoned it.
	const ui::FxCard& card = *entry->card;
	const visage::Bounds dots = card.slotMenuBounds();
	// Through the one road every other menu takes, so it is placed — and kept inside the window — by the
	// same rules.
	ui::showMenuAt(std::move(menu), *this,
	               { card.x() + dots.x(), card.y() + dots.y(), dots.width(), dots.height() });
}

bool RackEditor::slotVerbForTest(DeviceInstanceId device, ui::FragmentVerb verb) {
	const std::vector<DeviceInstanceId> before = editOrder();
	if (!selectionOf(device).chooseVerb(kVerbIds + static_cast<int>(verb), kVerbIds, device)) {
		return false;
	}
	afterVerb(before);
	return true;
}

bool RackEditor::syncLane(RackBridge& lane, std::vector<CardEntry>& cards, std::vector<OutputArm>& arms, ui::ModState& mod,
                          ui::EditHistory& history, bool own) {
	// COMPARE BEFORE BUILDING. A card is a frame in a tree and rebuilding one under a live press is
	// how the fx lane once reordered a chain on a click that never moved — see FxCard's drag anchor.
	// So this only builds where a device has actually arrived or changed, and a reorder builds nothing:
	// a card is keyed by its DEVICE, not by where the device stands.
	bool changed = false;
	// A DEVICE THIS BUILD DOES NOT HAVE: the record holds a tag the catalogue cannot find. It was drawn
	// as nothing at all here; it is "device missing" now, the one card every host
	// draws for it.
	const auto missingTagOf = [&lane](DeviceInstanceId device, const device::Descriptor* desc) {
		return desc == nullptr ? lane.heldTagOf(device) : device::Tag {};
	};
	// FIRST, EVERY CARD WHOSE DEVICE HAS LEFT THE CHAIN OR BECOME ANOTHER, RETIRED.
	for (auto it = cards.begin(); it != cards.end();) {
		const device::Descriptor* desc = lane.descriptorOf(it->device);
		const bool held = lane.positionOf(it->device) != RackBridge::kNoSlot;
		if (held && it->desc == desc && it->missing == missingTagOf(it->device, desc)) {
			++it;
			continue;
		}
		if (it->card != nullptr) {
			// **FORGET IT BEFORE DESTROYING IT.** a crash.
			//
			// `Knob::setModulation` puts EVERY knob on `ModState`'s watcher list so the glow follows
			// arming, and that list holds RAW FRAME POINTERS. Destroying a card left them dangling, and
			// the next `arm()` calls `watchers_.notify()`, which walks them. The crash is not in the
			// arrow; the arrow is merely the first thing that reads the list after a card has died.
			//
			// FOUR PLACES IN FEATHERS ALREADY DO THIS — `mod_panel.cpp`, `fx_lane.cpp`,
			// `expanded_overlay.cpp`, `windowed_expanded.cpp` — and the rack, which builds and destroys
			// cards exactly as they do, did not. `forgetFrameTree` exists for this and its comment says
			// why no owner can be trusted to remember: the modulation list is joined by CHILDREN THE
			// OWNER NEVER SEES.
			//
			// THE PARAMETER LIST IS NULL AND THAT IS A DECLARATION: a rack card's knobs are built
			// through the `device::Modulatable` constructor, which deliberately does not register, so
			// there is nothing of ours on `registry_`'s list to forget.
			ui::forgetFrameTree(it->card.get(), nullptr, nullptr, &mod.watchers());
			removeChild(it->card.get());
		}
		it = cards.erase(it);
		changed = true;
	}
	// THEN A CARD FOR EVERY DEVICE THE CHAIN HOLDS THAT HAS NONE.
	for (size_t position = 0; position < lane.chainLength(0); ++position) {
		const DeviceInstanceId i = lane.deviceAtPosition(position);
		if (i == kNoInstance || entryOf(i) != nullptr) {
			continue;
		}
		const device::Descriptor* desc = lane.descriptorOf(i);
		const device::Tag missing = missingTagOf(i, desc);
		std::unique_ptr<ui::FxCard> card;
		if (desc != nullptr) {
			// THE DEVICE'S OWN CARD IF IT HAS ONE ON THE SEAM, the generic grid otherwise. the rack built `GenericCard` for
			// everything, because every custom card needed a `ParamBridge` this host does not have.
			//
			// THE SAME SELECTION FEATHERS USES, from `ui::makeCustomCard`, so the two hosts cannot
			// disagree about which UI a device gets — which would make one device look like two.
			card = ui::makeCustomCard(lane, registry_, i, *desc, nullptr, &mod, &history);
			if (card == nullptr) {
				card = std::make_unique<ui::GenericCard>(lane, i, *desc, nullptr);
			}
		}
		else if (missing[0] != '\0') {
			card = std::make_unique<ui::MissingDeviceCard>(std::string_view(missing.data(), missing.size()));
		}
		if (card != nullptr) {
			card->setModulation(&mod, &history);
			// **FOLDED AS THE PATCH LEFT IT, BEFORE THE HEAD MARKS ARE TAKEN FROM IT.** The fold is the
			// device's and the bridge holds it (`RackBridge::deviceFolded`), so a card rebuilt for a device
			// that was folded — after a reorder, a load, or a reopened editor — is folded again rather than
			// springing open. Order matters by one line: `setHeadMarks` below reads `card->folded()`.
			card->setFolded(lane.deviceFolded(i));
			// **THIS LANE OFFERS A FOLD, AND DID NOT BEFORE.** the rack set no head marks at all, so no card in it had a fold to press.
			// Feathers' strip offers a fold and a slot menu; the rack offers the fold, because removing
			// and reordering here are the (+) row's and the drag's jobs rather than a per-card menu's.
			// THE `(-)` RULE IS RETIRED, so what is left to say here is the folded STATE the frame draws the head in.
			card->setHeadMarks({ false, card->folded() });
			// WHICH CHAIN THIS CARD'S DEVICE IS IN — its own tint, and the key of any panel it opens.
			// The rack's own is the default; a nested editor's is one level deeper (`setNest`).
			card->setNest(nest_);
			// **THE SLOT MENU, BOUND AT LAST.** Every card draws three dots; nothing in the rack ever
			// bound `onSlotMenu`, so the mark was painted and pressed and did nothing — which is what
			// was being seen. `removeDevice` is the one slot verb this engine has (there is no
			// reorder here yet; the comment at `rack_engine.cpp:553` is where that is recorded), so
			// the menu holds exactly that and does not offer what it cannot do.
			card->onSlotMenu = [this, i] { showSlotMenu(i); };
			// **THE SELECTION'S TWO ROADS** (backlog OA, D1; `LaneSelection`'s banner says why there are two): the focus
			// road for any press on the card, a knob's included, and the card's own press with the modifier held.
			// POST's cards (a site's tail) select in POST's own selection, over POST's bridge (backlog OA). Asked when the
			// press arrives: the selection is rebuilt with the tail.
			if (own || &lane == tail_.post) {
				card->onCardPressed = [this, i] { selectionOf(i).focusPress(i); };
				card->onCardSelectPress = [this, i](ui::CardSelection::Press kind) { selectionOf(i).cardPress(i, kind); };
				card->onCardClicked = [this, i] { selectionOf(i).cardClick(i); };
			}
			else {
				card->onCardPressed = [this, i] { focusedDevice_ = i; };
			}
			// **THE CARD'S HEIGHT MOVED — RE-PLACE THE LANE.** `resized()` does both halves;
			// `redraw()` does neither, which is what a card that folded into its old rectangle was
			// doing.
			//
			// **THIS HOST NEVER INSTALLED IT, and that is the third responsibility this family of
			// hosts has been found missing.** `FxLane` has had it since collapsing
			// audio effects left the card size unchanged; `RackEditor` and
			// `ChainColumn` both have the `laneExtent`-summing stacking that makes it work and
			// neither wired the trigger. So in the standalone rack a fold has been leaving the card
			// at its old size, and the gated-control resize that now drives the same hook would
			// have done nothing here either.
			card->onFoldChanged = [this] {
				resized();
				redraw();
			};
			// **REORDER BY DRAG — the rack had none.** The gesture itself is the card's and was already built for Feathers; what
			// the rack lacked was a lane to report it to.
			// A SITE'S POST IS DRAGGED TOO (backlog NZ): within POST, and across the centre into PRE.
			if (own || &lane == tail_.post) {
				card->onCardDrag = [this, i](ui::FxCard::DragPhase phase, visage::Point inCard) {
					cardDragForTest(i, phase, inCard);
				};
			}
			// **A FOLD IS THE DEVICE'S AND TRAVELS IN THE PATCH**.
			// The card's own `folded_` is what it DRAWS from; the bridge is what it IS, so a card rebuilt —
			// which this editor does whenever a device changes — comes back folded as the hand left it,
			// and a save carries it. The rack had no memory of a fold at all before this.
			card->onHeadFold = [this, &lane, c = card.get(), device = i] {
				c->setFolded(!c->folded());
				c->setHeadMarks({ false, c->folded() });
				lane.setDeviceFolded(device, c->folded());
				resized();
			};
			// THE EXPAND VERB, WIRED TO THE OVERLAY — the first `SpansGesture` caller this host has had.
			//
			// WHAT IT PRESENTS IS A SECOND `GenericCard` FOR THE SAME SLOT, IN THE VERTICAL AXIS. That is
			// not a trick: vertical is the uncapped direction — `kMaxFlowCells` exists because a horizontal
			// card competes for the window's width, and a panel does not — so the same card, turned, shows
			// the device's whole control set. Two cards over one slot is fine because a `DeviceControl` is a
			// view onto the host, not state: both read and write the same device.
			//
			// AND IT IS AN OVERLAY RATHER THAN A TAKEOVER because a hand must be able to arm a macro and
			// then click a control ON THIS PANEL. `SurfaceNeed::SpansGesture` is that requirement, and the
			// rack answering it with a takeover would put the mod panel off-screen mid-gesture.
			// A PRESS ON THE HEAD RAISES THIS DEVICE'S OPEN PANEL, as in Feathers' lane.
			card->onHeadPressed = [this, c = card.get()] { (void)surfaces_->focusPanelFor(c->device(), c->nest()); };
			// **THE SITE CHIP** (backlog NR): the device gets its PRE/POST chains on first use, and their panel opens
			// keyed by the site — a panel of its own, beside the device's, closed by a second press like it.
			card->onSite = [this, &lane, &mod, &history, device = i] {
				const DeviceInstanceId key = siteKeyOf(device);
				if (surfaces_->dismissPanelFor(key, nest_)) {
					return;
				}
				if (!lane.openSite(device)) {
					// A SITE IS A CHAIN BELOW THIS ONE: refused at the depth gate, and said so.
					if (!fitsNesting(lane.nestingDepth(), 1)) {
						notify_->say(kNestingFullMessage);
					}
					return;
				}
				std::unique_ptr<visage::Frame> content =
				    ui::makeExpandedView(lane, nullptr, registry_, key, ui::siteViewDescriptor(), nullptr);
				if (content == nullptr) {
					return;
				}
				if (auto* view = dynamic_cast<ui::ExpandedView*>(content.get())) {
					view->setModulation(&mod, &history);
					view->setNest(nest_);
				}
				const ui::SurfaceId presented =
				    surfaces_->present(std::move(content), ui::SurfaceNeed::SpansGesture,
				                       ui::siteViewDescriptor().traits.name, [this] { pruneExpanded(); });
				if (presented != ui::kNoSurface) {
					expanded_.push_back(presented);
				}
			};
			card->onExpand = [this, &lane, &mod, &history, device = i] {
				// A PRESS WHILE THIS DEVICE'S PANEL IS UP CLOSES IT, matched on the
				// device and this editor's nest — the key `setNest` below files the panel under.
				if (surfaces_->dismissPanelFor(device, nest_)) {
					return;
				}
				// THE DESCRIPTOR IS RE-READ AT PRESS TIME, not captured: a lambda holding one would be
				// one `removeDevice` away from opening a panel for a device that has gone.
				const device::Descriptor* desc = lane.descriptorOf(device);
				if (desc == nullptr) {
					return;
				}

				// THE DEVICE'S OWN PANEL IF THIS HOST CAN BUILD ONE. the rack opened a vertical `GenericCard` for everything, because every
				// custom panel took a `ParamBridge` the rack does not have.
				//
				// **NULLPTR HERE MEANS "NOT ON THIS HOST YET", NOT "NO PANEL".** `makeExpandedView`
				// takes the bridge as a pointer and the rack passes none, so a panel that still needs
				// the parameter road declines rather than crashing — and the generic fallback below is
				// what the player gets meanwhile.
				std::unique_ptr<visage::Frame> content =
				    ui::makeExpandedView(lane, nullptr, registry_, device, *desc, nullptr);
				// HOW THE PANEL WILL BE ADVANCED IS THE MOUNT'S BUSINESS NOW (`SurfaceLayers::mount`
				// decides where the type is still known); what is decided here is only the modulation
				// state, which the seam cannot carry.
				if (content != nullptr) {
					if (auto* view = dynamic_cast<ui::ExpandedView*>(content.get())) {
						view->setModulation(&mod, &history);
						// AND WHICH CHAIN IT IS ABOUT, which is half of the key the overlay files it
						// under — see `setNest`. Without it a nested editor's panel and an outer one's
						// share a key whenever the ids coincide, and the second retires the first.
						view->setNest(nest_);
					}
					// NOT SIZED HERE. The panel knows how big it needs to be and `MountedFrame` asks
					// it; a number chosen at the call site is the fixed minimum that made every popout
					// the same size and scrolled the real one inside it.
				}
				else {
					// THE GENERIC PANEL: the same card turned VERTICAL, which is the uncapped axis, so
					// it shows the device's whole control set rather than the three columns the
					// horizontal lane allows.
					auto panel = std::make_unique<ui::GenericCard>(lane, device, *desc, nullptr);
					panel->setLaneAxis(ui::FxCard::LaneAxis::Vertical);
					panel->setModulation(&mod, &history);
					panel->setNest(nest_);
					panel->setBounds(0, 0, 480, static_cast<int>(panel->laneExtent(480.0f)));
					content = std::move(panel);
				}

				const ui::SurfaceId presented = surfaces_->present(
				    std::move(content), ui::SurfaceNeed::SpansGesture, desc->traits.name, [this] { pruneExpanded(); });
				if (presented != ui::kNoSurface) {
					expanded_.push_back(presented);
				}
			};
			addChild(card.get());
			columnDirty_ = true;
		}
		// RECORDED EVEN WITH NO CARD (a device with neither a descriptor nor a tag), so it is not
		// "arriving" again on every sync.
		cards.push_back(CardEntry { i, desc, missing, std::move(card) });
		changed = true;
	}
	// ONE ARM PER DEVICE OUTPUT, REBUILT WITH THE CARDS THEY SIT ON.
	//
	// **THE ENGINE COULD ALWAYS ROUTE THESE AND THE UI COULD NEVER ARM THEM.** Everything was in place
	// except a widget, so a modulator in the rack was a device you could add, tick, and not use.
	//
	// REBUILT WHOLESALE WHEN THE CHAIN CHANGES rather than patched. An arm names its source by ENDPOINT
	// — the device's id and its output (kill-lanes step 2) — so it cannot come to arm another device's
	// output; it held a source INDEX computed from a lane before, which could. Cheap because it happens
	// only when `changed`.
	if (changed) {
		for (OutputArm& entry : arms) {
			if (entry.arm != nullptr) {
				// OFF THE WATCHER LIST BEFORE IT DIES. `SourceArm`'s constructor calls `mod.watch(this)`
				// and that list holds raw frame pointers — the same road that crashed on the arrow, and
				// the reason `~Knob` now unwatches itself.
				mod.watchers().unwatch(entry.arm.get());
				removeChild(entry.arm.get());
			}
		}
		arms.clear();
		for (size_t position = 0; position < lane.chainLength(0); ++position) {
			const DeviceInstanceId i = lane.deviceAtPosition(position);
			const device::Descriptor* d = lane.descriptorOf(i);
			if (d == nullptr) {
				continue;
			}
			for (size_t out = 0; out < d->outputCount(); ++out) {
				OutputArm entry;
				entry.device = i;
				entry.output = out;
				// LABELLED, exactly as Feathers' strip labels its chips. A device publishing two
				// outputs draws two identical glyphs and nothing but the name says which is which.
				// BY THE LABEL THIS INSTANCE GIVES THE OUTPUT (`RackBridge::outputLabel`) — the declared
				// name until a hand relabels it; later relabels arrive by `labelSubscription_`.
				entry.arm = std::make_unique<SourceArm>(mod, backplane::Source::output(i, out),
				                                        lane.outputLabel(i, out));
				addChild(entry.arm.get());
				arms.push_back(std::move(entry));
			}
		}
	}

	return changed;
}

void RackEditor::syncCards() {
	bool changed = syncLane(engine_, cards_, outputArms_, mod_, history_, true);
	if (changed) {
		columnDirty_ = true; // backlog OC: a new output arm reflows the macro column
	}
	changed = syncTail() || changed;
	if (changed) {
		// A CARD BUILT OR GONE: the selection drops what the chain no longer holds (an undo, a host load), and every
		// card — a new one included — draws the selection's flag (backlog OA).
		selection_->prune();
		if (postSelection_ != nullptr) {
			postSelection_->prune();
		}
		applySelection();
		resized();
		redraw();
	}

	// AND THE TOP LAYERS BACK ON TOP, because the cards above were added after them.
	raiseTopLayers();
}

RackBridge& RackEditor::laneOf(DeviceInstanceId device) noexcept {
	if (tail_.post != nullptr) {
		for (const CardEntry& entry : postCards_) {
			if (entry.device == device) {
				return *tail_.post;
			}
		}
	}
	return engine_;
}

bool RackEditor::syncTail() {
	if (tail_.outer == nullptr || tail_.post == nullptr || tail_.centreMod == nullptr || tail_.postMod == nullptr) {
		return false;
	}
	bool changed = false;
	// THE CENTRE: THE DEVICE ITSELF, BUILT OVER THE HOST IT RUNS IN — the same selection the host's own lane makes, so it
	// is the same card over the same instance, and a control here and the one on the lane are one control.
	const device::Descriptor* desc = tail_.outer->descriptorOf(tail_.centre);
	if (centreDesc_ != desc || (desc != nullptr && centreCard_ == nullptr)) {
		if (centreCard_ != nullptr) {
			ui::forgetFrameTree(centreCard_.get(), nullptr, nullptr, &tail_.centreMod->watchers());
			removeChild(centreCard_.get());
			centreCard_.reset();
		}
		centreDesc_ = desc;
		if (desc != nullptr) {
			centreCard_ = ui::makeCustomCard(*tail_.outer, registry_, tail_.centre, *desc, nullptr, tail_.centreMod,
			                                 tail_.centreHistory);
			if (centreCard_ == nullptr) {
				centreCard_ = std::make_unique<ui::GenericCard>(*tail_.outer, tail_.centre, *desc, nullptr);
			}
			centreCard_->setModulation(tail_.centreMod, tail_.centreHistory);
			// NO HEAD MARKS, NO DRAG, NO SITE CHIP: the centre is where the site sits, not a device of this chain.
			centreCard_->setHeadMarks({ false, centreCard_->folded() });
			centreCard_->onFoldChanged = [this] {
				resized();
				redraw();
			};
			addChild(centreCard_.get());
		}
		changed = true;
	}
	changed = syncLane(*tail_.post, postCards_, postArms_, *tail_.postMod, history_, false) || changed;
	return changed;
}

void RackEditor::setSiteTail(const SiteTail& tail) {
	// WHAT THE LAST TAIL BUILT GOES FIRST, off the watcher lists it joined.
	if (centreCard_ != nullptr) {
		if (tail_.centreMod != nullptr) {
			ui::forgetFrameTree(centreCard_.get(), nullptr, nullptr, &tail_.centreMod->watchers());
		}
		removeChild(centreCard_.get());
		centreCard_.reset();
	}
	centreDesc_ = nullptr;
	for (CardEntry& entry : postCards_) {
		if (entry.card != nullptr) {
			if (tail_.postMod != nullptr) {
				ui::forgetFrameTree(entry.card.get(), nullptr, nullptr, &tail_.postMod->watchers());
			}
			removeChild(entry.card.get());
		}
	}
	postCards_.clear();
	for (OutputArm& entry : postArms_) {
		if (entry.arm != nullptr) {
			if (tail_.postMod != nullptr) {
				tail_.postMod->watchers().unwatch(entry.arm.get());
			}
			removeChild(entry.arm.get());
		}
	}
	postArms_.clear();
	tail_ = tail;
	// POST'S SELECTION AND VERBS, over its own bridge and recording in this editor's history (where POST's other edits
	// go). The overlay lets go of the old one first: it draws POST's lasso and pill (backlog OA).
	if (selectionOverlay_ != nullptr) {
		selectionOverlay_->setSecond(nullptr);
	}
	postSelection_.reset();
	if (tail_.post != nullptr) {
		ui::LaneSelection::Hooks hooks;
		hooks.order = [this] { return postOrder(); };
		hooks.changed = [this] {
			if (postSelection_ != nullptr) {
				exclusiveSelection(*postSelection_);
			}
			applySelection();
		};
		hooks.cardBounds = [this](DeviceInstanceId id) -> std::optional<visage::Bounds> {
			for (const CardEntry& entry : postCards_) {
				if (entry.device == id && entry.card != nullptr && entry.card->isVisible()) {
					return entry.card->bounds();
				}
			}
			return std::nullopt;
		};
		postSelection_ = std::make_unique<ui::LaneSelection>(*tail_.post, history_, notify_.get(), std::move(hooks));
		postSelection_->setEscapeStack(&surfaces_->escapeStack());
		if (selectionOverlay_ != nullptr) {
			selectionOverlay_->setSecond(postSelection_.get());
		}
	}
	if (tail_.outer != nullptr && postAddRow_ == nullptr) {
		// POST'S OWN (+), AT THE END OF THE WHOLE CHAIN: an append to POST, through the one pick road.
		postAddRow_ = std::make_unique<AddDeviceRow>();
		postAddRow_->onHeightChanged = [this] {
			resized();
			redraw();
		};
		postAddRow_->onPress = [this] {
			insertAnchor_.reset();
			addToPost_ = true;
			showAddMenu(*postAddRow_, { 0.0f, 0.0f, static_cast<float>(postAddRow_->width()), AddDeviceRow::kBarHeight });
		};
		addChild(postAddRow_.get());
	}
	else if (tail_.outer == nullptr && postAddRow_ != nullptr) {
		removeChild(postAddRow_.get());
		postAddRow_.reset();
	}
	syncFurniture();
	syncCards();
}

void RackEditor::setPreAvailable(bool available, std::string why) {
	preAvailable_ = available;
	if (preVeil_ == nullptr) {
		preVeil_ = std::make_unique<SegmentVeil>();
		addChild(preVeil_.get());
	}
	static_cast<SegmentVeil*>(preVeil_.get())->why = std::move(why);
	raiseTopLayers();
	resized();
	redraw();
}

std::vector<std::string> RackEditor::preVeilLinesForTest() const {
	return preVeil_ != nullptr ? static_cast<const SegmentVeil*>(preVeil_.get())->lines() : std::vector<std::string> {};
}

visage::Bounds RackEditor::preVeilBoundsForTest() const {
	return preVeil_ != nullptr ? preVeil_->bounds() : visage::Bounds {};
}

std::vector<ui::FxCard*> RackEditor::postCardsForTest() const {
	std::vector<ui::FxCard*> out;
	if (tail_.post == nullptr) {
		return out;
	}
	for (size_t position = 0; position < tail_.post->chainLength(0); ++position) {
		for (const CardEntry& entry : postCards_) {
			if (entry.device == tail_.post->deviceAtPosition(position) && entry.card != nullptr) {
				out.push_back(entry.card.get());
			}
		}
	}
	return out;
}

void RackEditor::syncFurniture() {
	for (auto& knob : outerLevelKnobs_) {
		removeChild(knob.get());
	}
	outerLevelKnobs_.clear(); // before the controls they read
	outerLevels_.clear();
	const device::Descriptor* desc = tail_.outer != nullptr ? tail_.outer->descriptorOf(tail_.centre) : nullptr;
	if (desc == nullptr) {
		resized();
		return;
	}
	// **THE SAME TWO KNOBS A CHAIN CONTAINER'S PANEL CARRIES**, in the same place, captioned the same — mapped onto the
	// device's DECLARED furniture roles (backlog NR's D1/D2): the slot blends a plain crossfade, a device-owned mix is
	// the device's own, and either way the control is the device's, so this knob and the one on its card are one
	// control. A device that declares neither has none here, which is NR's open item for the no-Mix devices.
	const std::array<std::pair<int, const char*>, 2> roles { { { desc->traits.mixControl, "Mix" },
		                                                      { desc->traits.wetGainControl, "Gain" } } };
	for (const auto& [control, caption] : roles) {
		if (control < 0 || static_cast<size_t>(control) >= desc->params.size()) {
			continue;
		}
		outerLevels_.push_back(std::make_unique<ui::DeviceControl>(*tail_.outer, tail_.centre, *desc,
		                                                           static_cast<size_t>(control), tail_.centreMod,
		                                                           tail_.centreHistory));
		auto knob = std::make_unique<ui::Knob>(*outerLevels_.back(), caption, notify_.get());
		knob->setFaceDiameter(kMacroDiameter);
		addChild(knob.get());
		outerLevelKnobs_.push_back(std::move(knob));
	}
	raiseTopLayers();
	resized();
	redraw();
}

float RackEditor::placeLane(RackBridge& lane, std::vector<OutputArm>& laneArms, float x, float laneTop, float laneHeight) {
	for (size_t position = 0; position < lane.chainLength(0); ++position) {
		const DeviceInstanceId ci = lane.deviceAtPosition(position);
		CardEntry* held = entryOf(ci);
		if (held == nullptr || held->card == nullptr) {
			continue;
		}
		std::unique_ptr<ui::FxCard>& card = held->card;
		card->setLaneAxis(ui::FxCard::LaneAxis::Horizontal);
		// **EVERY CARD IS ONE UNIT TALL.** So the lane GRANTS `kCardUnitHeight` rather than asking each card what it
		// wants — the card pads inside it or scrolls its body, and neither is the lane's business.
		//
		// CAPPED BY THE LANE, because a unit taller than the window would put the strip's foot off
		// screen. Measured heights and why the unit is what it is: `ui::kCardUnitHeight`.
		const float cardHeight = std::min(ui::kCardUnitHeight, laneHeight);
		const float cardWidth = card->laneExtent(cardHeight);
		card->setBounds(x, laneTop, cardWidth, cardHeight);

		// **THE OUTPUT ARMS SIT IN THE STRIP ALONG THE CARD'S BOTTOM EDGE**, which `ui::cardFrameOf`
		// reserves — the design rule that a device's outputs belong at the foot of its card, made a
		// contract rather than a convention: the frame takes the strip out of the body, so a card
		// cannot draw over it and a host cannot put the arms anywhere else.
		//
		// They are children of the EDITOR and not of the card, deliberately: a card is rebuilt
		// whenever its device changes and an arm on the watcher list must not be rebuilt with it,
		// which is the crash `forgetFrameTree` exists for.
		//
		// **THE FRAME IS THE CARD'S OWN, AND THIS ASKED FOR SOMEBODY ELSE'S.** It called `cardFrameOf`
		// with neither the head edge nor the row count — so it got a TOP head on a card whose head is
		// its LEFT spine, which put the strip across the spine, and one row for a device with seven
		// chips. `FxCard::ownFrame` is the card's answer to both; offset by the card's origin because
		// an arm is the EDITOR's child and the frame is in the card's coordinates.
		//
		// **AND THEY WRAP, BY THE WALK ALL FOUR HOSTS NOW SHARE** — `ui::layOutCardOutputArms`, which
		// is that offset arithmetic, `outputChipBoundsFor`, and the fold / visible / does-it-fit tests
		// in one place. This laid ONE row, marching leftward with no wrap and no clamp, so a device
		// publishing more chips than fit drew the rest off the left edge of its own card — Feathers'
		// strip's exact bug, still standing in this lane after the
		// strip's was fixed. **The card's height absorbs it**: `cardFrameOf` takes the strip out of the
		// BODY, the lane grants a fixed unit height, and a card's body already scrolls
		// (`FxCard::maxBodyScroll` counts the strip), so a taller foot costs body room rather than
		// pushing the card past the lane.
		//
		// **THE FIT TEST IS LOAD-BEARING AND NOT BELT-AND-BRACES, WHICH IS WORTH STATING BECAUSE THE
		// CAMERA SAYS OTHERWISE, AND THIS LANE IS THE ONE THAT CAN REACH IT**: its card height is
		// `min(kCardUnitHeight, laneHeight)` rather than whatever the content wants, so a short enough
		// lane clamps the strip below the rows the chips need. Removing the test changes no pixel —
		// `layoutOutputChips` never emits the bad geometry, so a chip shown without a rectangle is a
		// zero-size frame that draws nothing, and two renders with and without it came back
		// byte-identical. What it changes is every NON-pixel answer: `isVisible()` is what a hit test
		// consults and what any "how many outputs is this card showing" reader counts — the honesty
		// question itself. A frame that claims to be visible while occupying nothing lies to all three.
		const device::Descriptor* desc = lane.descriptorOf(ci);
		// **INDEXED BY OUTPUT, NOT IN THE ORDER THE ARMS HAPPEN TO SIT IN.** `laneArms` is one flat
		// list across every device, so this gathers THIS device's arms into output order — which is the
		// order the chips come back in, and the only order the two can be matched by.
		//
		// **SIZED TO WHAT IS THERE AS WELL AS TO WHAT IS DECLARED**, so an arm the descriptor no longer
		// has an output for is still HANDED to the walk and hidden by it. Dropping it from the vector
		// instead would leave it wherever it last was, shown — which is what the loop this replaced did
		// NOT do, because every arm it saw got a `setVisible` whether a chip came back for it or not.
		size_t armCount = desc != nullptr ? desc->outputCount() : 0;
		for (const OutputArm& entry : laneArms) {
			if (entry.arm != nullptr && entry.device == ci) {
				armCount = std::max(armCount, entry.output + 1);
			}
		}
		std::vector<ui::SourceArm*> arms(armCount, nullptr);
		for (OutputArm& entry : laneArms) {
			if (entry.arm != nullptr && entry.device == ci) {
				arms[entry.output] = entry.arm.get();
			}
		}
		ui::layOutCardOutputArms(*card, arms);
		x += cardWidth + kPad;
	}
	return x;
}

void RackEditor::resized() {
	// THE CANVAS IS THE BAND ACROSS THE TOP, and everything else starts under it: the macro row, the
	// lane, the rule between them. One number, `top`, is what a canvas that opens or closes moves.
	const float w = static_cast<float>(width());
	const float top = canvasExtent();
	if (canvas_ != nullptr) {
		canvas_->setBounds(0, 0, width(), static_cast<int>(top));
	}

	// THE WHOLE EDITOR, and it is placed FIRST so that an early return added below it later cannot
	// leave a presented surface unplaced. It is invisible while the stack is empty.
	if (selectionOverlay_ != nullptr) {
		selectionOverlay_->setBounds(0, 0, width(), height()); // the lasso and the pill, over the whole editor
		selectionOverlay_->setPillLeft(laneLeft());              // the pill at the lane's foot, not over the column
	}
	if (surfaces_ != nullptr) {
		surfaces_->setBounds(0, 0, width(), height());
	}

	// THE MACROS COLUMN AT THE LEFT (it was "the inspector"; QN): the settings door, Mix and Gain (nested), the H-line, the macros two across and four tall, and
	// UNDER THEM the armed source's route list (rack roadmap §7f), which takes what height is left and scrolls by rows.
	const float columnEnd = layOutColumn(top);
	if (routes_ != nullptr) {
		const float listTop = columnEnd + kPad * 0.5f;
		const float room = std::max(ui::RouteList::kHeadingHeight + ui::RouteList::kRowHeight,
		                            static_cast<float>(height()) - listTop - kPad * 0.5f);
		routes_->setBounds(kPad, listTop, kMacroColumnWidth, room);
	}

	if (notify_ != nullptr) {
		notify_->setBounds(0, 0, static_cast<int>(w), static_cast<int>(height()));
	}

	// Then the cards, stacked, and the (+) after them. Each reports its own height, which is what lets a card fold without
	// this layout knowing what folding is.
	// THE STACK IS OFFSET BY THE SCROLL; the macro row above is NOT — it is the surface's fixed head,
	// so a player reaching for a macro does not have to find it first, and the arrows stay reachable
	// while looking at a control far down the stack.
	// THE CHAIN RUNS LEFT TO RIGHT.
	//
	// WHAT MAKES THIS A LAYOUT SWAP RATHER THAN A REWRITE: every card answers `laneExtent(cross)`,
	// and the only thing that changed is WHICH dimension is the cross. Vertically the lane hands a
	// width and receives a height; here it hands a height and receives a width. The accumulate, the
	// scroll and the content measure are the same three lines with x and y exchanged.
	//
	// THE CARDS TAKE THE FULL LANE HEIGHT, which is what makes the bound real: a card can no longer
	// grow to fit its knobs, so a device with more than fit has to put the rest somewhere else. That
	// is the expanded view, and it is why this direction makes card+expand load-bearing rather than
	// desirable — OWLTT's generic card wants ~837px against the ~460 this lane has.
	const float laneTop = top + kPad;
	const float laneHeight = std::max(0.0f, static_cast<float>(height()) - laneTop - kPad);

	float x = laneLeft() - scroll_; // after the Macros column, open or folded (QN)
	// **LAID OUT IN CHAIN ORDER, WHICH IS NOT STORE ORDER.**
	//
	// `cards_` is keyed by DEVICE — a card belongs to a device, so a reorder neither creates nor
	// destroys one (it was indexed by the device's lane until kill-lanes step 2, for the same reason).
	// But the store is not where the device SITS, and this loop is about where things sit. Walking
	// `cards_` would draw the rack in the order devices arrived: correct before any reorder, and silently
	// wrong after one, with the audio in the order the graph says and the picture in the order the store
	// happens to hold — the "silent wrong-order bug rather than a crash" `graph.h` warns about.
	x = placeLane(engine_, outputArms_, x, laneTop, laneHeight);
	if (addRow_ != nullptr) {
		// THE (+) IS THE LAST THING IN THE CHAIN, which horizontally means the RIGHTMOST rather than
		// the bottom. It keeps its own extent — it is not an `FxCard` and has no knobs to pack.
		//
		// **ONE UNIT TALL, NOT THE LANE'S HEIGHT.**
		// It took the full height so that "the end of the chain reads as part of it" — which was the
		// right instinct and the wrong size: a full-height (+) beside unit-tall cards reads as a
		// column rule rather than as the next slot. Same rule as the cards, from the same constant.
		addRow_->setLaneAxis(ui::FxCard::LaneAxis::Horizontal);
		const float addHeight = std::min(ui::kCardUnitHeight, laneHeight);
		const float addWidth = addRow_->laneExtent(addHeight);
		addRow_->setBounds(x, laneTop, addWidth, addHeight);
		x += addWidth;
	}
	// **THE SITE'S CENTRE AND ITS POST** (backlog NZ): a partition, the device outlined (drawn in `draw`), a partition,
	// then POST's cards and its own (+), all on the one scroll. PRE's (+) stays where every chain's is, at the end of
	// the segment it appends to.
	if (tail_.outer != nullptr) {
		const float segmentStart = kPad - scroll_;
		x += kPartitionGap;
		partitionX_[0] = x - kPartitionGap * 0.5f;
		if (preVeil_ != nullptr) {
			preVeil_->setVisible(!preAvailable_);
			preVeil_->setBounds(segmentStart, laneTop, std::max(0.0f, partitionX_[0] - kPad * 0.5f - segmentStart),
			                    std::min(ui::kCardUnitHeight, laneHeight));
		}
		if (addRow_ != nullptr) {
			addRow_->setVisible(preAvailable_); // no (+) into a segment that does not run
		}
		if (centreCard_ != nullptr) {
			centreCard_->setLaneAxis(ui::FxCard::LaneAxis::Horizontal);
			const float cardHeight = std::min(ui::kCardUnitHeight, laneHeight);
			const float cardWidth = centreCard_->laneExtent(cardHeight);
			centreCard_->setBounds(x, laneTop, cardWidth, cardHeight);
			x += cardWidth;
		}
		x += kPartitionGap;
		partitionX_[1] = x - kPartitionGap * 0.5f;
		if (tail_.post != nullptr) {
			x = placeLane(*tail_.post, postArms_, x, laneTop, laneHeight);
		}
		if (postAddRow_ != nullptr) {
			postAddRow_->setLaneAxis(ui::FxCard::LaneAxis::Horizontal);
			const float addHeight = std::min(ui::kCardUnitHeight, laneHeight);
			const float addWidth = postAddRow_->laneExtent(addHeight);
			postAddRow_->setBounds(x, laneTop, addWidth, addHeight);
			x += addWidth;
		}
	}

	// HOW FAR THERE IS LEFT TO GO, recomputed from what was actually placed rather than from a guess
	// about how tall a card is. A card that folds changes this and says nothing; asking the layout is
	// the only answer that cannot go stale.
	// HOW FAR THERE IS LEFT TO GO, along the flow axis — measured from what was actually placed rather
	// than guessed from a card size, for the reason the vertical version gave: a card that folds
	// changes this and says nothing, so asking the layout is the only answer that cannot go stale.
	const float contentRight = x + scroll_;
	const float room = w - kPad;
	maxScroll_ = std::max(0.0f, contentRight - room);
	if (scroll_ > maxScroll_) {
		scroll_ = maxScroll_;
	}
	// THE SCROLL BAR IN THE PAD UNDER THE LANE (backlog NE), across the lane's view — set unconditionally, so a scale
	// change re-places it at the new scale (CLAUDE.md: a scale change moves nothing logical).
	if (laneScrollBar_ != nullptr) {
		const float view = std::max(0.0f, room - laneLeft());
		const float barY = static_cast<float>(height()) - kPad + (kPad - LaneScrollBar::kHeight) * 0.5f;
		laneScrollBar_->setBounds(laneLeft(), barY, view, LaneScrollBar::kHeight);
		laneScrollBar_->set(view, maxScroll_, scroll_);
	}
}

visage::Bounds RackEditor::columnBackdropForTest() const noexcept {
	return columnBackdrop_ != nullptr ? columnBackdrop_->bounds() : visage::Bounds {};
}

visage::Frame* RackEditor::columnBackdropFrameForTest() const noexcept { return columnBackdrop_.get(); }

float RackEditor::columnNeeded(bool levels, bool door) noexcept {
	// THE COLUMN'S HEIGHT, as `layOutColumn` stacks it: the pad, the door, the chain's Mix and Gain and the rule under
	// them, four macro rows (knob and arm), the pad. With neither door nor Mix/Gain, the Macros toggle's own row first (QN).
	float y = kPad + (!levels && !door ? kMacrosHeadHeight : 0.0f);
	if (door) {
		y += ui::Knob::kHeight * 0.5f + kColumnGap;
	}
	if (levels) {
		y += kMacroKnobHeight + kColumnGap + kRuleHeight + kColumnGap;
	}
	const float rows = static_cast<float>((8 + kMacroColumns - 1) / kMacroColumns);
	y += rows * (kMacroKnobHeight + kColumnGap);
	return y + kPad - kColumnGap;
}

float RackEditor::layOutColumn(float top) {
	const float x0 = kPad;
	const float columnHeight = std::max(0.0f, static_cast<float>(height()) - top);
	// THE RAIL (backlog QN): while open, just its TOGGLE's corner at the column's top right (the frame no larger than the
	// toggle, so it takes no press meant for the door, the presets or a macro); while folded, the whole strip, when nothing
	// else of the column is laid out. The column keeps its height: the first row leaves the corner free (`kMacrosToggleRoom`).
	const bool headless = settingsDoor_ == nullptr && presets_ == nullptr && outerLevelKnobs_.empty();
	if (macrosRail_ != nullptr) {
		const bool folded = macrosRail_->collapsed();
		if (folded) {
			macrosRail_->setBounds(0.0f, top, macrosRail_->railWidth(), columnHeight);
		}
		else {
			const float corner = kMacrosToggleRoom;
			macrosRail_->setBounds(kLaneLeft - kPad * 0.5f - corner, top, corner, kMacrosHeadHeight);
		}
		showColumn(!folded); // every layout: a widget the column gains while folded is hidden with the rest
		if (folded) {
			columnRule_ = {}; // the H-line under Mix and Gain is the open column's
			return top + kMacrosHeadHeight;
		}
	}
	// A COLUMN WITH NO DOOR ROW AND NO MIX/GAIN ROW opens on the macros, whose second cell's arm would stand under the
	// toggle: that one shape gives the toggle a row of its own.
	float y = top + kPad + (headless ? kMacrosHeadHeight : 0.0f);
	if (columnBackdrop_ != nullptr) {
		columnBackdrop_->setBounds(0.0f, top, kLaneLeft - kPad * 0.5f, columnHeight);
	}
	if (settingsDoor_ != nullptr || presets_ != nullptr) {
		const float row = ui::Knob::kHeight * 0.5f;
		if (settingsDoor_ != nullptr) {
			settingsDoor_->setBounds(x0, y, kSettingsDoorWidth, row);
		}
		// UNDO/REDO AT THE ROW'S RIGHT END (backlog JB); the presets take what is between.
		// LEFT OF THE MACROS TOGGLE'S CORNER (QN), which stands at the column's top right.
		float right = std::min(x0 + kMacroColumnWidth, kLaneLeft - kPad * 0.5f - kMacrosToggleRoom - kColumnGap);
		if (undoRedo_ != nullptr) {
			right -= UndoRedoButtons::kWidth;
			undoRedo_->setBounds(right, y, UndoRedoButtons::kWidth, row);
			right -= kColumnGap;
		}
		if (presets_ != nullptr) {
			// BESIDE THE DOOR, centred on its row, and no wider than the column it heads.
			const float left = x0 + (settingsDoor_ != nullptr ? kSettingsDoorWidth + kColumnGap : 0.0f);
			const float h = static_cast<float>(presets_->height());
			presets_->setBounds(left, y + std::max(0.0f, (row - h) * 0.5f),
			                    std::max(0.0f, std::min(static_cast<float>(presets_->width()), right - left)), h);
		}
		y += row + kColumnGap;
	}
	columnRule_ = {};
	if (columnBackdrop_ != nullptr) {
		columnBackdrop_->setRule({});
	}
	if (!outerLevelKnobs_.empty()) {
		for (size_t i = 0; i < outerLevelKnobs_.size(); ++i) {
			outerLevelKnobs_[i]->setBounds(x0 + kMacroCellWidth * static_cast<float>(i), y, kMacroKnobWidth, kMacroKnobHeight);
		}
		y += kMacroKnobHeight + kColumnGap;
		columnRule_ = { x0, y, kMacroColumnWidth, kRuleHeight };
		if (columnBackdrop_ != nullptr) {
			columnBackdrop_->setRule({ x0, y - top, kMacroColumnWidth, kRuleHeight });
		}
		y += kRuleHeight + kColumnGap;
	}
	for (size_t i = 0; i < macroSlots_.size(); ++i) {
		const float cx = x0 + kMacroCellWidth * static_cast<float>(i % kMacroColumns);
		const float cy = y + static_cast<float>(i / kMacroColumns) * (kMacroKnobHeight + kColumnGap);
		// THE KNOB GETS THE WHOLE CELL, not just its face — the BOUNDS are what the label is drawn inside, and giving them
		// the face's 34px clipped every label to "IACRO".
		macroSlots_[i]->setBounds(cx, cy, kMacroKnobWidth, kMacroKnobHeight);
		if (i < macroArms_.size()) {
			// THE ARM BESIDE ITS KNOB, level with the dial's face (not under the label), in the cell's other 44 px.
			macroArms_[i]->setBounds(cx + kMacroKnobWidth, cy + (kMacroDiameter - SourceArm::kHeight) * 0.5f,
			                         kMacroCellWidth - kMacroKnobWidth - kColumnGap, SourceArm::kHeight);
		}
	}
	return y + std::ceil(static_cast<float>(macroSlots_.size()) / static_cast<float>(kMacroColumns))
	                 * (kMacroKnobHeight + kColumnGap);
}


void RackEditor::setSettingsDoor(std::function<void()> open) {
	if (settingsDoor_ == nullptr) {
		settingsDoor_ = std::make_unique<ui::LibraDoor>();
		addChild(settingsDoor_.get());
		// AND UNDO/REDO ON THE SAME ROW (backlog JB): the door's row is the rack plugin's own head.
		undoRedo_ = std::make_unique<UndoRedoButtons>(history_);
		addChild(undoRedo_.get());
		columnDirty_ = true;
	}
	// UNDER THE NOTIFIER AND THE SURFACE HOST, like every other child: a panel presented over the editor must
	// cover the door too.
	raiseTopLayers();
	settingsDoor_->onClick = std::move(open);
	resized();
}

void RackEditor::setPresetBrowser(visage::Frame* browser) {
	if (presets_ == browser) {
		return;
	}
	if (presets_ != nullptr) {
		removeChild(presets_);
	}
	presets_ = browser;
	if (presets_ != nullptr) {
		addChild(presets_);
		columnDirty_ = true;
	}
	raiseTopLayers(); // under the notifier and the surface host, as the door is
	resized();
}

//------------------------------------------------------------------------------------------------//
// UndoRedoButtons (backlog JB)                                                                     //
//------------------------------------------------------------------------------------------------//

UndoRedoButtons::UndoRedoButtons(ui::EditHistory& history) : history_(history) {
	watch_ = history_.watch(this); // lit and dim as the history moves
	onMouseDown() = [this](const visage::MouseEvent& e) {
		if (undoBounds().contains(e.position)) {
			history_.undo();
		}
		else if (redoBounds().contains(e.position)) {
			history_.redo();
		}
		redraw();
	};
	// LAST, after every `=` above (`installTooltipHover`'s hazard).
	ui::installTooltipHover(*this, *this);
}

const char* UndoRedoButtons::tooltipAt(visage::Point local) const {
	if (undoBounds().contains(local)) {
		return "Undo the last edit (Ctrl/Cmd+Z)";
	}
	if (redoBounds().contains(local)) {
		return "Redo the edit just undone (Ctrl/Cmd+Shift+Z)";
	}
	return nullptr;
}

void UndoRedoButtons::draw(visage::Canvas& canvas) {
	const auto glyph = [&](visage::Bounds b, bool live, bool redo) {
		canvas.setColor(ui::withAlpha(ui::kLine, live ? 0.9f : 0.35f));
		canvas.roundedRectangleBorder(b.x(), b.y(), b.width(), b.height(), 3.0f, 1.0f);
		// A HOOKED ARROW, drawn: a short arc of segments and a head, mirrored for redo.
		canvas.setColor(ui::withAlpha(ui::kInk, live ? 0.9f : 0.3f));
		const float cx = b.x() + b.width() * 0.5f;
		const float cy = b.y() + b.height() * 0.5f;
		const float r = std::min(b.width(), b.height()) * 0.28f;
		const float s = redo ? -1.0f : 1.0f;
		canvas.segment(cx + s * r, cy + r * 0.6f, cx + s * r, cy - r * 0.2f, 1.2f, true);
		canvas.segment(cx + s * r, cy - r * 0.2f, cx - s * r * 0.6f, cy - r * 0.2f, 1.2f, true);
		canvas.segment(cx - s * r * 0.6f, cy - r * 0.2f, cx - s * r * 0.1f, cy - r * 0.7f, 1.2f, true);
		canvas.segment(cx - s * r * 0.6f, cy - r * 0.2f, cx - s * r * 0.1f, cy + r * 0.3f, 1.2f, true);
	};
	glyph(undoBounds(), history_.canUndo(), false);
	glyph(redoBounds(), history_.canRedo(), true);
}

//------------------------------------------------------------------------------------------------//
// LaneScrollBar (backlog NE)                                                                       //
//------------------------------------------------------------------------------------------------//

LaneScrollBar::Thumb LaneScrollBar::thumbFor(float track, float view, float maxScroll, float scroll) noexcept {
	if (track <= 0.0f || view <= 0.0f || maxScroll <= 0.0f) {
		return { 0.0f, track };
	}
	const float width = std::clamp(track * view / (view + maxScroll), std::min(kMinThumb, track), track);
	const float travel = track - width;
	return { travel * std::clamp(scroll / maxScroll, 0.0f, 1.0f), width };
}

float LaneScrollBar::scrollFor(float track, float view, float maxScroll, float thumbX) noexcept {
	const Thumb t = thumbFor(track, view, maxScroll, 0.0f);
	const float travel = track - t.width;
	return travel > 0.0f ? maxScroll * std::clamp(thumbX / travel, 0.0f, 1.0f) : 0.0f;
}

LaneScrollBar::LaneScrollBar() {
	setVisible(false);
	onMouseDown() = [this](const visage::MouseEvent& e) {
		const Thumb t = thumb();
		// ON THE THUMB, it is grabbed where it was pressed; ON THE TRACK, its centre jumps there first.
		grab_ = e.position.x >= t.x && e.position.x < t.x + t.width ? e.position.x - t.x : t.width * 0.5f;
		dragging_ = true;
		if (onScroll) {
			onScroll(scrollFor(static_cast<float>(width()), view_, maxScroll_, e.position.x - grab_));
		}
	};
	onMouseDrag() = [this](const visage::MouseEvent& e) {
		if (dragging_ && onScroll) {
			onScroll(scrollFor(static_cast<float>(width()), view_, maxScroll_, e.position.x - grab_));
		}
	};
	onMouseUp() = [this](const visage::MouseEvent&) { dragging_ = false; };
}

void LaneScrollBar::set(float view, float maxScroll, float scroll) {
	const bool show = maxScroll > 0.0f && view > 0.0f;
	if (view == view_ && maxScroll == maxScroll_ && scroll == scroll_ && isVisible() == show) {
		return;
	}
	view_ = view;
	maxScroll_ = maxScroll;
	scroll_ = scroll;
	setVisible(show); // NOTHING TO SCROLL, NOTHING DRAWN — `drawBodyScrollBar`'s "if applicable"
	redraw();
}

void LaneScrollBar::draw(visage::Canvas& canvas) {
	const float w = static_cast<float>(width());
	const float h = static_cast<float>(height());
	const Thumb t = thumb();
	canvas.setColor(ui::withAlpha(ui::kLine, 0.5f));
	canvas.roundedRectangle(0.0f, 0.0f, w, h, h * 0.5f);
	canvas.setColor(ui::withAlpha(ui::kMuted, dragging_ ? 1.0f : 0.85f));
	canvas.roundedRectangle(t.x, 0.0f, t.width, h, h * 0.5f);
}

bool RackEditor::scrollBy(float delta) {
	const float was = scroll_;

	// CLAMP AGAINST A FRESHLY MEASURED BOUND, NOT THE ONE FROM LAST TIME.
	//
	// `maxScroll_` is computed BY `resized()` from what was actually placed, so before any layout has
	// run with the current contents it is stale — and a clamp against a stale bound stops short. A
	// case scrolling to the end landed at 818 against a real maximum of 1536, which as a hand is "the
	// wheel stops before the bottom" and is indistinguishable from having reached it.
	//
	// So: move, re-measure, clamp, and re-measure once more if the clamp moved anything. Two passes
	// terminate because the second clamp can only reduce.
	scroll_ = std::max(0.0f, scroll_ + delta);
	resized();
	scroll_ = std::clamp(scroll_, 0.0f, maxScroll_);
	resized();

	if (scroll_ != was) {
		redraw();
	}
	return scroll_ != was;
}

void RackEditor::draw(visage::Canvas& canvas) {
	canvas.setColor(ui::kSurface);
	canvas.fill(0, 0, width(), height());

	// THE BUMPER, if a card is being dragged. Drawn by the LANE because a gap belongs to the list
	// rather than to either card beside it — and drawn EARLY, under everything, so a card sliding past
	// does not have to be told about it. The rack had the gap arithmetic and drew nothing with it, so
	// the drag committed correctly and looked like nothing was happening.
	//
	// (It is painted after the background fill below, which is why this sits where it does rather than
	// first: the fill would cover it.)

	if (dragDevice_.has_value() && dropGap_.has_value()) {
		const visage::Bounds bumper = gapBounds(*dropGap_);
		if (bumper.height() > 0.0f) {
			canvas.setColor(ui::kValue);
			canvas.roundedRectangle(bumper.x(), bumper.y(), bumper.width(), bumper.height(), 1.0f);
		}
	}

	// **A SITE'S PARTITIONS AND ITS CENTRE** (backlog NZ): a rule each side of the device, and the device outlined — the
	// one card in this chain that is not the chain's.
	if (tail_.outer != nullptr) {
		const float laneTop = canvasExtent() + kPad; // OC: no band above the lane any more
		const float laneHeight = std::min(ui::kCardUnitHeight, std::max(0.0f, static_cast<float>(height()) - laneTop - kPad));
		canvas.setColor(ui::kLine);
		for (const float px : partitionX_) {
			canvas.rectangle(px - 0.5f, laneTop, 1.0f, laneHeight);
		}
		if (centreCard_ != nullptr) {
			constexpr float kOutline = 3.0f;
			canvas.setColor(ui::kAccent);
			canvas.roundedRectangleBorder(centreCard_->x() - kOutline, centreCard_->y() - kOutline,
			                              centreCard_->width() + 2.0f * kOutline, centreCard_->height() + 2.0f * kOutline,
			                              6.0f, 2.0f);
		}
	}

	// **NO CHROME DRAWN HERE ANY MORE.** This lane briefly painted the frame for cards that did not
	// paint their own, which was the fix for cards with no border or frame — and it kept two
	// conventions alive rather than closing them. Every card now frames itself through
	// `ui::drawCardFrame`, so a card looks the same in this lane as it does in Feathers' strip
	// because it is the same drawing, not two that agree.


	if (tail_.outer == nullptr && std::all_of(cards_.begin(), cards_.end(), [](const CardEntry& c) { return c.card == nullptr; })) {
		// AN EMPTY RACK SAYS SO. A blank panel is indistinguishable from a broken one, and the first
		// thing anyone sees is an empty rack.
		canvas.setColor(ui::kMuted);
		canvas.text("No devices", ui::font(14.0f), visage::Font::kCenter, static_cast<int>(laneLeft()),
		            static_cast<int>(canvasExtent() + kPad + 40.0f), static_cast<int>(width() - laneLeft()), 20);
	}
}

} // namespace feathers::rack

namespace feathers::rack {

//------------------------------------------------------------------------------------------------//
// THE MACROS COLUMN'S FOLD (backlog QN)                                                           //
//------------------------------------------------------------------------------------------------//

float RackEditor::laneLeft() const {
	return macrosCollapsed() ? ui::Rail::kCollapsedWidth + kPad * 0.5f : kLaneLeft;
}

bool RackEditor::macrosCollapsed() const { return macrosRail_ != nullptr && macrosRail_->collapsed(); }

bool RackEditor::setMacrosCollapsed(bool collapsed) {
	return macrosRail_ != nullptr && macrosRail_->setCollapsed(collapsed);
}

ui::Rail& RackEditor::macrosRailForTest() const { return *macrosRail_; }

void RackEditor::showColumn(bool shown) {
	const auto show = [shown](visage::Frame* f) {
		if (f != nullptr && f->isVisible() != shown) {
			f->setVisible(shown);
		}
	};
	show(settingsDoor_.get());
	show(presets_);
	show(undoRedo_.get());
	show(columnBackdrop_.get());
	show(routes_.get());
	for (auto& k : outerLevelKnobs_) {
		show(k.get());
	}
	for (auto& s : macroSlots_) {
		show(s.get());
	}
	for (auto& a : macroArms_) {
		show(a.get());
	}
}

} // namespace feathers::rack
