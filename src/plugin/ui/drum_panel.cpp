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

#include "plugin/ui/drum_panel.h"

#include "dsp/field_override.h"
#include "plugin/character_params.h"
#include "plugin/sound_travel.h"
#include "plugin/ui/theme.h"
#include "plugin/sound_hues.h"
#include "plugin/ui/celestial.h"
#include "rack/rack_bridge.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numbers>
#include <optional>
#include <utility>
#include <string>

namespace feathers::ui {

namespace {

constexpr float kPad = 12.0f;
constexpr float kPictureHeight = 230.0f;
constexpr float kRowHeight = 30.0f;
constexpr float kPadCellWidth = 72.0f;
constexpr float kPadCellHeight = 48.0f;
constexpr float kPadGap = 6.0f;
/// A layer's terrain pad: square, at the body's left.
constexpr float kVectorPadSize = 148.0f;
/// The zone cells' face in the kit window's list rows.
constexpr float kZoneFace = 22.0f;
// THE PANEL IS EXACTLY ONE ROW OF EIGHT PADS WIDE, header's kMinimumWidth, and no wider (maximumSize).
static_assert(DrumPanel::kMinimumWidth == 2.0f * kPad + 8.0f * kPadCellWidth + 7.0f * kPadGap);

const ParamDesc& descOf(ParamId id) { return *findParam(static_cast<clap_id>(id)); }

/// THE PAD FX RAIL'S WORDS AND ITS CAST: the surface it follows IS the pad's chain (the parallel rack's `chainItself`).
constexpr const char* kPadLaneLabel = "PAD FX";
constexpr const char* kPadLaneLine = "after the pad, before the kit's mix";
constexpr const char* kPadLanePasses = "this pad's hits pass through untouched";
rack::InnerChain* padChainItself(void* surface) { return static_cast<rack::InnerChain*>(surface); }

uint32_t alphaOf(uint32_t colour, float alpha) {
	const auto a = static_cast<uint32_t>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f);
	return (colour & 0x00ffffffu) | (a << 24);
}

} // namespace

//---------//
// DrumTab //
//---------//

DrumTab::DrumTab(std::function<void()> toggle) : toggle_(std::move(toggle)) {
	onMouseDown() = [this](const visage::MouseEvent&) {
		if (toggle_) {
			toggle_();
		}
	};
}

void DrumTab::draw(visage::Canvas& canvas) {
	canvas.setColor(open_ ? kAccent : kMuted);
	canvas.text("KIT", font(kControlSize), visage::Font::kCenter, 0.0f, 0.0f, width(), height());
	if (open_) {
		canvas.fill(width() * 0.5f - 8.0f, height() - 2.0f, 16.0f, 2.0f);
	}
}

void DrumTab::setOpen(bool open) {
	if (open != open_) {
		open_ = open;
		redraw();
	}
}

//------------------//
// The settled pitch //
//------------------//

const device::ParamDesc& PadSoloControl::desc() const {
	static const device::ParamDesc d { "solo", "Solo", 0.0, 1.0, 0.0, true, device::Unit::Number, {}, false, false,
		                               "Hear this pad alone: the preview the sound cards' S marks, so every other pad is silent while it is on. "
		                               "Not saved with the patch." };
	return d;
}

void PadSoloControl::setValue(double value) {
	if (value >= 0.5) {
		bridge_.setPreview(slot_);
	}
	else if (bridge_.preview().active() && bridge_.preview().slot == slot_) {
		bridge_.clearPreview();
	}
	if (changed_) {
		changed_();
	}
}

PadPlayButton::PadPlayButton(ParamBridge& bridge, size_t slot) : bridge_(bridge), slot_(slot) {
	onMouseDown() = [this](const visage::MouseEvent&) { press(); };
	onMouseUp() = [this](const visage::MouseEvent&) { release(); };
	installTooltipHover(*this, *this);
}

PadPlayButton::~PadPlayButton() { release(); }

const char* PadPlayButton::tooltipAt(visage::Point) const {
	return "Strike this pad without selecting it: held while the button is held, as a key on the pad's note. Not saved and not modulated.";
}

void PadPlayButton::press() {
	if (held_) {
		return;
	}
	heldKey_ = static_cast<int>(std::lround(bridge_.value(static_cast<clap_id>(drumParam(slot_, DrumControl::Note)))));
	held_ = bridge_.postNote(heldKey_, true, kVelocity); // a full ring drops the press (and counts it); no off is owed then
	redraw();
}

void PadPlayButton::release() {
	if (!held_) {
		return;
	}
	held_ = false;
	(void)bridge_.postNote(heldKey_, false, 0.0f);
	redraw();
}

void PadPlayButton::draw(visage::Canvas& canvas) {
	canvas.setColor(held_ ? kAccent : kSurfaceRaised);
	canvas.roundedRectangle(0.0f, 0.0f, width(), height(), 4.0f);
	canvas.setColor(alphaOf(kLine, 0.8f));
	canvas.roundedRectangleBorder(0.0f, 0.0f, width(), height(), 4.0f, 1.0f);
	canvas.setColor(held_ ? kVoid : kInk);
	canvas.triangleRight(width() * 0.5f - 2.0f, height() * 0.5f - 4.0f, 4.0f);
}

//------------//
// LayerTabs  //
//------------//

LayerTabs::LayerTabs(std::function<void(size_t)> choose) : choose_(std::move(choose)) {
	onMouseDown() = [this](const visage::MouseEvent& e) {
		const int tab = tabAt(e.position);
		if (tab >= 0 && choose_) {
			choose_(static_cast<size_t>(tab));
		}
	};
}

int LayerTabs::tabAt(visage::Point p) const {
	if (p.y < 0.0f || p.y > height() || p.x < 0.0f || p.x >= 2.0f * kTabWidth) {
		return -1;
	}
	return p.x < kTabWidth ? 0 : 1;
}

void LayerTabs::setShown(size_t layer) {
	if (layer != shown_) {
		shown_ = layer;
		redraw();
	}
}

void LayerTabs::draw(visage::Canvas& canvas) {
	for (size_t i = 0; i < 2; ++i) {
		const float x = static_cast<float>(i) * kTabWidth;
		const bool lit = i == shown_;
		canvas.setColor(lit ? kSurfaceRaised : kSurface);
		canvas.roundedRectangle(x + 1.0f, 0.0f, kTabWidth - 2.0f, height(), 4.0f);
		canvas.setColor(lit ? kAccent : kMuted);
		canvas.text(i == 0 ? "Layer 1" : "Layer 2", font(kControlSize), visage::Font::kCenter, x, 0.0f, kTabWidth, height());
		if (lit) {
			canvas.fill(x + 10.0f, height() - 2.0f, kTabWidth - 20.0f, 2.0f);
		}
	}
}

//-----------//
// DrumPanel //
//-----------//

DrumPanel::DrumPanel(ParamBridge& bridge, WidgetRegistry& registry, SoundSelection& selection, Notifier* notifier,
                     FrameWatchers* previewWatchers)
    : bridge_(bridge), previewWatchers_(previewWatchers), selection_(selection) {
	mode_ = std::make_unique<SegmentedControl>(bridge, registry, descOf(ParamId::DrumMode), kDrumModeNames, notifier);
	pitchedKindControl_ = std::make_unique<ParamControl>(bridge, &descOf(ParamId::DrumPitchedKind));
	pitchedKind_ = std::make_unique<ValueBox>(*pitchedKindControl_, "Kind");
	seedControl_ = std::make_unique<ParamControl>(bridge, &descOf(ParamId::DrumSeed));
	seed_ = std::make_unique<ValueBox>(*seedControl_, "Seed");
	watches_.push_back(registry.watchToken(static_cast<clap_id>(ParamId::DrumPitchedKind), pitchedKind_.get()));
	watches_.push_back(registry.watchToken(static_cast<clap_id>(ParamId::DrumSeed), seed_.get()));
	watches_.push_back(registry.watchToken(static_cast<clap_id>(ParamId::DrumMode), this));
	if (previewWatchers_ != nullptr) {
		watches_.push_back(previewWatchers_->watch(this));
	}
	noRotate_ = std::make_unique<ParamControl>(bridge, nullptr);
	tabs_ = std::make_unique<LayerTabs>([this](size_t layer) { chooseLayer(layer); });
	addChild(&picture_);
	addChild(tabs_.get());
	addChild(mode_.get());
	addChild(pitchedKind_.get());
	addChild(seed_.get());

	// ONE ParamControl PER WIDGET (the spectrum panel's pattern), every one handed the modulation state when the panel is bound.
	const auto control = [&](std::unique_ptr<ParamControl>& out, ParamId id, visage::Frame* watcher) {
		const ParamDesc& d = descOf(id);
		out = std::make_unique<ParamControl>(bridge, &d);
		allControls_.push_back(out.get());
		if (watcher != nullptr) {
			watches_.push_back(registry.watchToken(static_cast<clap_id>(d.id), watcher));
		}
	};
	const auto box = [&](std::unique_ptr<ParamControl>& out, std::unique_ptr<ValueBox>& widget, ParamId id, const char* label) {
		control(out, id, nullptr);
		widget = std::make_unique<ValueBox>(*out, label);
		watches_.push_back(registry.watchToken(static_cast<clap_id>(id), widget.get()));
		addChild(widget.get());
	};
	const auto pill = [&](std::unique_ptr<ParamControl>& out, std::unique_ptr<FaderPill>& widget, ParamId id, const char* caption) {
		control(out, id, nullptr);
		widget = std::make_unique<FaderPill>(*out, caption);
		watches_.push_back(registry.watchToken(static_cast<clap_id>(id), widget.get()));
		addChild(widget.get());
	};
	const auto segmented = [&](std::unique_ptr<SegmentedControl>& out, ParamId id, const auto& names) { // (a span keeps a reference to them)
		out = std::make_unique<SegmentedControl>(bridge, registry, descOf(id), names, notifier);
		addChild(out.get());
	};

	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		PadSet& set = pads_[slot];
		// THE PAD-LEVEL CONTROLS: Note, Choke, Voices, Trigger, Pan, Bend, Release (Level is the sound card's, and layer one's Level handle).
		box(set.noteControl, set.note, drumParam(slot, DrumControl::Note), "Note");
		box(set.chokeControl, set.choke, drumParam(slot, DrumControl::Choke), "Choke");
		segmented(set.voices, drumParam(slot, DrumControl::Voices), kDrumVoicesNames);
		segmented(set.trigger, drumParam(slot, DrumControl::Trigger), kDrumTriggerNames);
		pill(set.panControl, set.pan, drumParam(slot, DrumControl::Pan), "Pan");
		pill(set.releaseControl, set.release, drumParam(slot, DrumControl::Release), "Release");
		pill(set.bendControl, set.bend, drumParam(slot, DrumControl::Bend), "Bend");
		// MUTE is the pad's own Mute; SOLO the preview of this pad alone (the cards' S), which the synth already honours.
		control(set.muteControl, drumParam(slot, DrumControl::Mute), nullptr);
		set.mute = std::make_unique<SwitchChip>(*set.muteControl, "Mute");
		watches_.push_back(registry.watchToken(static_cast<clap_id>(drumParam(slot, DrumControl::Mute)), set.mute.get()));
		addChild(set.mute.get());
		set.soloControl = std::make_unique<PadSoloControl>(bridge, slot, [this] {
			if (previewWatchers_ != nullptr) {
				previewWatchers_->notify();
			}
		});
		set.solo = std::make_unique<SwitchChip>(*set.soloControl, "Solo");
		addChild(set.solo.get());
		set.play = std::make_unique<PadPlayButton>(bridge, slot);
		addChild(set.play.get());
		// The pad's kind and Trigger decide what is drawn, so a move of either re-lays the panel.
		watches_.push_back(registry.watchToken(static_cast<clap_id>(drumParam(slot, DrumControl::Kind)), this));
		watches_.push_back(registry.watchToken(static_cast<clap_id>(drumParam(slot, DrumControl::Trigger)), this));
		watches_.push_back(registry.watchToken(static_cast<clap_id>(drumParam(slot, DrumControl::LayerKind)), this));

		for (size_t layer = 0; layer < 2; ++layer) {
			LayerSet& L = set.layer[layer];
			const auto id = [&](DrumControl c) { return drumLayerParam(slot, layer, c); };
			// THE HANDLES' CONTROLS: the picture writes them for the shown layer, and watches every layer's, so a host move
			// redraws it and each of them is reached by a hand (the reachability guard asks the registry).
			const auto handle = [&](std::unique_ptr<ParamControl>& out, DrumControl c) { control(out, id(c), &picture_); };
			handle(L.tune, DrumControl::Tune);
			handle(L.sweepTime, DrumControl::SweepTime);
			handle(L.decay, DrumControl::Decay);
			handle(L.drive, DrumControl::Drive);
			handle(L.driveTone, DrumControl::DriveTone);
			handle(L.wireDecay, DrumControl::WireDecay);
			handle(L.clickDecay, DrumControl::ClickDecay);
			handle(L.bellStart, DrumControl::BellStart);
			handle(L.bellEnd, DrumControl::BellEnd);
			handle(L.bellTime, DrumControl::BellTime);
			handle(L.bell2Hz, DrumControl::Bell2Hz);
			handle(L.bell2Time, DrumControl::Bell2Time);
			handle(L.hold, DrumControl::KickHold);
			handle(L.dip, DrumControl::KickDip);
			handle(L.dipTime, DrumControl::KickDipTime);
			handle(L.clickSweep, DrumControl::ClickSweep);
			handle(L.clickSweepTime, DrumControl::ClickSweepTime);
			handle(L.startPhase, DrumControl::StartPhase);
			handle(L.spread, DrumControl::Spread);
			handle(L.clapQ, DrumControl::ClapQ);
			handle(L.clapHighPass, DrumControl::ClapHighPass);
			// THE LEVEL HANDLE: the pad's Level on layer one, the layer's own Level (on top of the pad's) on layer two.
			control(L.level, layer == 0 ? drumParam(slot, DrumControl::Level) : drumParam(slot, DrumControl::LayerLevel), &picture_);
			// THE KIND: the pad's own (a kind of the six), or the layer's, whose first choice is Off.
			box(L.kindControl, L.kind, drumLayerKindParam(slot, layer), "Kind");
			box(L.countControl, L.count, id(DrumControl::Count), "Claps");
			segmented(L.driveCurve, id(DrumControl::DriveCurve), kDrumDriveCurveNames);
			segmented(L.pitchMode, id(DrumControl::PitchMode), kDrumPitchModeNames);
			pill(L.tuneControl, L.tunePill, id(DrumControl::Tune), "Tune");
			L.tunePill->setWholeSteps(true); // semitones, as the readout and the picture's handle
			pill(L.velocityNoiseControl, L.velocityNoise, id(DrumControl::VelocityNoise), "Vel Noise");
			pill(L.widthControl, L.width, id(DrumControl::Width), "Width");
			if (layer == 0) {
				// LAYER ONE'S LEVEL IS THE PAD'S Level, the param the sound card's fader drives, so the two stay one control.
				pill(L.levelPillControl, L.levelPill, drumParam(slot, DrumControl::Level), "Level");
			}
			if (layer == 1) {
				pill(L.levelPillControl, L.levelPill, drumParam(slot, DrumControl::LayerLevel), "Level");
				pill(L.delayControl, L.delay, drumParam(slot, DrumControl::LayerDelay), "Delay");
				pill(L.highPassControl, L.highPass, drumParam(slot, DrumControl::LayerHighPass), "HP");
			}
			// THE SEVEN FIELD SCALERS of this layer (Pitch .. Metal), each drawn only where its kind reads it.
			static constexpr std::array<DrumControl, 7> kFields { DrumControl::FieldPitch, DrumControl::FieldLength, DrumControl::FieldSweep,
				                                                  DrumControl::FieldTone,  DrumControl::FieldNoise,  DrumControl::FieldSnap,
				                                                  DrumControl::FieldMetal };
			static constexpr std::array<const char*, 7> kFieldCaptions { "Pitch", "Length", "Sweep", "Tone", "Noise", "Snap", "Metal" };
			for (size_t f = 0; f < kFields.size(); ++f) {
				pill(L.fieldControl[f], L.field[f], id(kFields[f]), kFieldCaptions[f]);
			}
			// THE ZONE CONTROLS (drum proposal §14): one coordinate each, its zone named in a dropdown, in the list row's layout.
			const auto zone = [&](std::unique_ptr<ParamControl>& out, std::unique_ptr<ZoneCoordCell>& cell, DrumControl c,
			                      const char* caption, auto names) {
				control(out, id(c), nullptr);
				std::vector<ZoneSelect::ZoneName> zoneNames;
				for (const char* n : names) {
					zoneNames.push_back({ n, "" });
				}
				cell = std::make_unique<ZoneCoordCell>(*out, caption, std::move(zoneNames), ZoneSelect::kCoordinatePerLap);
				cell->setRowLayout(true);
				watches_.push_back(registry.watchToken(static_cast<clap_id>(id(c)), cell.get()));
				addChild(cell.get());
			};
			zone(L.velocityZoneControl, L.velocityZone, DrumControl::VelocityZone, "Velocity", drum::kVelocityZoneNames);
			zone(L.wiresControl, L.wires, DrumControl::Wires, "Wires", drum::kWiresZoneNames);
			zone(L.bellsControl, L.bells, DrumControl::Bells, "Bells", drum::kBellsZoneNames);
			// The Bells zone decides whether the second bell is linked, which is what its handle writes, so a move re-lays.
			watches_.push_back(registry.watchToken(static_cast<clap_id>(id(DrumControl::Bells)), this));
			watches_.push_back(registry.watchToken(static_cast<clap_id>(id(DrumControl::PitchMode)), this));
			// THE LAYER'S VECTOR: its terrain pad, and Lock beside it.
			control(L.lockControl, id(DrumControl::Lock), nullptr);
			L.lock = std::make_unique<SwitchChip>(*L.lockControl, "Lock");
			watches_.push_back(registry.watchToken(static_cast<clap_id>(id(DrumControl::Lock)), L.lock.get()));
			addChild(L.lock.get());
			// THE VECTOR'S CONTROLS FIRST: the pad reads its row as it is built.
			control(L.offsetXControl, id(DrumControl::OffsetX), nullptr);
			control(L.offsetYControl, id(DrumControl::OffsetY), nullptr);
			L.vectorContext.slot = static_cast<uint8_t>(slot);
			L.vectorContext.layer = static_cast<uint8_t>(layer);
			L.vectorPad = std::make_unique<DrumVectorPad>(
			    [this, slot, layer](size_t index, float value) {
				    LayerSet& l = pads_[slot].layer[layer];
				    (index == 0 ? l.offsetXControl : l.offsetYControl)->setValue(static_cast<double>(value));
			    },
			    [this, slot, layer](size_t field) {
				    const LayerSet& l = pads_[slot].layer[layer];
				    return static_cast<float>((field == 0 ? l.offsetXControl : l.offsetYControl)->value());
			    },
			    *noRotate_);
			watches_.push_back(registry.watchToken(static_cast<clap_id>(id(DrumControl::OffsetX)), L.vectorPad.get()));
			watches_.push_back(registry.watchToken(static_cast<clap_id>(id(DrumControl::OffsetY)), L.vectorPad.get()));
			L.vectorPad->context = [this, slot, layer]() -> const DrumVectorContext& { return pads_[slot].layer[layer].vectorContext; };
			// ONE DRAG OF THE PAD IS ONE UNDO STEP, as the picture's two-way handles are: both vector parameters in one group.
			L.vectorPad->onHoldBegan = [this, slot, layer] {
				LayerSet& l = pads_[slot].layer[layer];
				l.vectorGroup = l.offsetXControl->recordsInto();
				if (l.vectorGroup != nullptr) {
					l.vectorGroup->beginGroup("Move layer");
				}
				l.offsetXControl->beginGesture();
				l.offsetYControl->beginGesture();
			};
			L.vectorPad->onHoldEnded = [this, slot, layer] {
				LayerSet& l = pads_[slot].layer[layer];
				l.offsetXControl->endGesture();
				l.offsetYControl->endGesture();
				if (l.vectorGroup != nullptr) {
					l.vectorGroup->endGroup();
					l.vectorGroup = nullptr;
				}
			};
			addChild(L.vectorPad.get());
			// THE SETTLED PITCH, over this layer's Tune: its own text, a hand on it.
			L.settledControl = std::make_unique<SettledPitchControl>(*L.tuneControl, [this, slot, layer] { return settledOf(slot, layer); });
			L.readout = std::make_unique<PitchReadout>(*L.settledControl);
			addChild(L.readout.get());
		}
	}
	// THE PAD FX RAIL (drum proposal §17 P3): the focused pad's lane, hanging off the right side as SKLITTER's inner chain
	// does. Built with the panel, CLOSED (SKLITTER's rule: a chain is not the instrument, and a pad's tile wears a dot while
	// its lane holds anything); one click opens it.
	lane_ = std::make_unique<FxLaneRail>(FxLaneRail::Spec { kPadLaneLabel, kPadLaneLine, &padChainItself }, registry, notifier);
	addChild(lane_.get());
	lane_->onRailWidthChanged([this]() {
		resized();
		redraw();
		minimumSizeChanged();
	});
	(void)followLane();
	selectionWatch_ = selection_.watch(this);
	onMouseDown() = [this](const visage::MouseEvent& e) {
		if (!kitMode()) {
			return;
		}
		for (size_t pad = 0; pad < kMaxSounds; ++pad) {
			if (padBounds(pad).contains(e.position)) {
				selection_.select(pad);
				sync();
				redraw();
				return;
			}
		}
	};
	sync();
}

DrumPanel::~DrumPanel() = default;

std::string DrumPanel::headerTitle() const {
	if (!kitMode()) {
		return "Drum · Pitched";
	}
	const size_t pad = std::min(selection_.slot(), kMaxSounds - 1);
	// THE PAD AND ITS KIND NOW (not the lane it started as): a pad switched to a Snare says Snare.
	const auto kind = static_cast<size_t>(kindOf(pad));
	return std::string("Drum Kit · ") + static_cast<char>('A' + pad) + " · " + kDrumKindNames[std::min(kind, kDrumKindNames.size() - 1)];
}

void DrumPanel::modulationBound(ModState* mod, EditHistory* history) {
	for (ParamControl* c : allControls_) {
		c->setModulation(mod, history);
	}
}

bool DrumPanel::kitMode() const {
	return steppedIndex(bridge_.value(static_cast<clap_id>(ParamId::DrumMode)), kDrumModeNames.size())
	       == static_cast<size_t>(DrumMode::Kit);
}

size_t DrumPanel::shownPad() const { return kitMode() ? std::min(selection_.slot(), kMaxSounds - 1) : 0; }

drum::Kind DrumPanel::kindOf(size_t pad) const {
	const clap_id id = kitMode() ? static_cast<clap_id>(drumParam(pad, DrumControl::Kind))
	                             : static_cast<clap_id>(ParamId::DrumPitchedKind);
	return static_cast<drum::Kind>(steppedIndex(bridge_.value(id), kDrumKindNames.size()));
}

std::optional<drum::Kind> DrumPanel::layerKindOf(size_t pad, size_t layer) const {
	if (layer == 0) {
		return kindOf(pad);
	}
	// LAYER TWO'S KINDS ARE Off, THEN Kick..Clap in Kind's own order, so index - 1 is the Kind.
	const size_t index = steppedIndex(bridge_.value(static_cast<clap_id>(drumParam(pad, DrumControl::LayerKind))), kDrumLayerKindNames.size());
	if (!kitMode() || index == 0) {
		return std::nullopt;
	}
	return static_cast<drum::Kind>(index - 1);
}

Character DrumPanel::characterNow() const {
	// THE PAD WHERE IT REALLY STANDS (the honesty rule: what you see is what the pad plays): the character the synth builds, by
	// WaveDisplay's road — modulated values, the played travel (drift and pulls), the planes, residue, gamma and the likes'
	// warp. Measured cost: seven triangle reads a sound.
	const auto read = [this](ParamId id) { return bridge_.modulated(static_cast<clap_id>(id)); };
	Character c = characterFromParams(read, OscType::Drum, bridge_.tuning(), bridge_.puckVelocity(),
	                                  playedTravel(bridge_.soundTravel(), bridge_.travelPulls()), bridge_.soundOrientations(),
	                                  bridge_.soundResidue(), bridge_.soundGamma(), bridge_.preview(), EarSide::Head);
	bool any = false;
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		likeTable_[slot] = bridge_.likeEngineOf(slot) == static_cast<uint8_t>(OscType::Drum) ? bridge_.likeWarpOf(slot) : LikeWarp {};
		any = any || !likeTable_[slot].identity();
	}
	if (any) {
		c.likes = { &likeTable_, bridge_.likeSerial() + 1 };
	}
	return c;
}

drum::Fields DrumPanel::fieldsFrom(size_t pad, size_t layer, const DrumCharacter& built) const {
	if (layer == 0) {
		return built.pads[built.kit ? pad : 0];
	}
	return built.kit ? built.layers[pad] : drum::Fields {};
}

drum::Controls DrumPanel::controlsOf(size_t pad, size_t layer) const {
	// THE VOICE'S MAPPING (Synth::fillDrumKit's `fillControls`): the layer's own controls over its real fields.
	const auto v = [&](DrumControl c) { return bridge_.modulated(static_cast<clap_id>(drumLayerParam(pad, layer, c))); };
	const bool kit = kitMode();
	drum::Controls c;
	c.decaySeconds = std::exp2(v(DrumControl::Decay));
	c.sweepSeconds = std::exp2(v(DrumControl::SweepTime));
	c.drive = v(DrumControl::Drive);
	c.count = static_cast<int>(std::lround(v(DrumControl::Count)));
	c.spreadSeconds = std::exp2(v(DrumControl::Spread));
	c.pitchRangeSemitones = kit ? 3.0 : 0.5;
	c.velocityZone = v(DrumControl::VelocityZone);
	c.wires = v(DrumControl::Wires);
	c.bells = v(DrumControl::Bells);
	c.wireDecaySeconds = std::exp2(v(DrumControl::WireDecay));
	c.clickDecaySeconds = std::exp2(v(DrumControl::ClickDecay));
	c.bellStartHz = std::exp2(v(DrumControl::BellStart));
	c.bellEndHz = std::exp2(v(DrumControl::BellEnd));
	c.bellSeconds = std::exp2(v(DrumControl::BellTime));
	c.kickHoldSeconds = v(DrumControl::KickHold) / 1000.0;
	c.kickDip = v(DrumControl::KickDip);
	c.kickDipSeconds = v(DrumControl::KickDipTime) / 1000.0;
	c.clickSweepOctaves = v(DrumControl::ClickSweep);
	c.clickSweepSeconds = std::exp2(v(DrumControl::ClickSweepTime));
	c.startPhaseDegrees = v(DrumControl::StartPhase);
	c.driveCurve = static_cast<int>(steppedIndex(v(DrumControl::DriveCurve), kDrumDriveCurveNames.size()));
	c.driveTone = v(DrumControl::DriveTone);
	c.clapQ = v(DrumControl::ClapQ);
	c.clapHighPassHz = std::exp2(v(DrumControl::ClapHighPass));
	c.bell2Hz = std::exp2(v(DrumControl::Bell2Hz));
	c.bell2Seconds = std::exp2(v(DrumControl::Bell2Time));
	c.velocityNoise = v(DrumControl::VelocityNoise);
	c.width = v(DrumControl::Width);
	// THE TERRAIN'S PITCH MODE: Kit mode alone (Pitched mode keeps the +-0.5 semitone fine pitch as it always was).
	c.pitchMode = kit ? static_cast<int>(steppedIndex(v(DrumControl::PitchMode), kDrumPitchModeNames.size())) : 0;
	return c;
}

namespace {
/// THE KIT-WIDE TRANSPOSE in semitones, as the synth sums it (Synth::fillDrumKit): the Engine rail's Oct, St, Coarse and Fine.
double kitTransposeOf(const ParamBridge& bridge) {
	const auto v = [&](ParamId id) { return bridge.modulated(static_cast<clap_id>(id)); };
	return 12.0 * v(ParamId::TransposeOct) + v(ParamId::TransposeSemi) + v(ParamId::Coarse) + v(ParamId::FineTune) / 100.0;
}
} // namespace

drum::Laws DrumPanel::lawsFrom(size_t pad, size_t layer, const DrumCharacter& built) const {
	const drum::Kind k = layerKindOf(pad, layer).value_or(kindOf(pad));
	const double tune = bridge_.modulated(static_cast<clap_id>(drumLayerParam(pad, layer, DrumControl::Tune)));
	// THE RESTING PITCH: a kit's kind's own (the kit's transpose in), or a C3 in Pitched mode; then Tune.
	const double base = kitMode() ? drum::restingBaseHz(k, tune, kitTransposeOf(bridge_)) : 130.8127826502993 * std::exp2(tune / 12.0);
	return drum::lawsFor(k, fieldsFrom(pad, layer, built), controlsOf(pad, layer), base, 0.8);
}

drum::SettledPitch DrumPanel::settledFrom(size_t pad, size_t layer, const DrumCharacter& built) const {
	const std::optional<drum::Kind> k = layerKindOf(pad, layer);
	if (!k || !kitMode()) {
		return {};
	}
	const double tune = bridge_.modulated(static_cast<clap_id>(drumLayerParam(pad, layer, DrumControl::Tune)));
	return drum::settledPitch(*k, fieldsFrom(pad, layer, built), controlsOf(pad, layer), drum::restingBaseHz(*k, tune, kitTransposeOf(bridge_)));
}

drum::SettledPitch DrumPanel::settledOf(size_t pad, size_t layer) const {
	return settledFrom(pad, layer, buildDrumFor(characterNow()));
}

void DrumPanel::refreshVectorContext(size_t pad, size_t layer, const Character& character) {
	DrumVectorContext& ctx = pads_[pad].layer[layer].vectorContext;
	ctx.character = character;
	ctx.slot = static_cast<uint8_t>(pad);
	ctx.layer = static_cast<uint8_t>(layer);
	ctx.kind = layerKindOf(pad, layer).value_or(kindOf(pad));
}

visage::Bounds DrumPanel::padBounds(size_t pad) const {
	// ONE ROW OF EIGHT, A to H left to right (the kit window's layout, by design): the panel is exactly as wide as
	// the row, and the pad's controls sit under it.
	const size_t col = pad % kMaxSounds;
	const float top = kPad + kPictureHeight + kPad + kRowHeight + kPad;
	return { kPad + static_cast<float>(col) * (kPadCellWidth + kPadGap), top,
		     kPadCellWidth, kPadCellHeight };
}

void DrumPanel::chooseLayer(size_t layer) {
	layer = std::min<size_t>(layer, 1);
	if (layer == layerShown_) {
		return;
	}
	layerShown_ = layer;
	tabs_->setShown(layer);
	shownPad_ = kMaxSounds; // lay out again for the other tab
	sync();
	redraw();
}

bool DrumPanel::sync() {
	const bool kit = kitMode();
	const size_t pad = shownPad();
	const size_t layer = kit ? layerShown_ : 0; // Pitched mode has layer one alone
	const std::optional<drum::Kind> shownKindOpt = layerKindOf(pad, layer);
	const bool layerOn = shownKindOpt.has_value();
	const drum::Kind kind = shownKindOpt.value_or(kindOf(pad));
	const drum::ControlUse uses = drum::kindControls(kind);
	const drum::FieldUse reads = drum::kindReads(kind);
	const bool gate = steppedIndex(bridge_.value(static_cast<clap_id>(drumParam(pad, DrumControl::Trigger))),
	                               kDrumTriggerNames.size())
	                  == 1;
	PadSet& mine = pads_[pad];
	LayerSet& L = mine.layer[layer];
	const Character character = layerOn ? characterNow() : Character {};
	const DrumCharacter built = layerOn ? buildDrumFor(character) : DrumCharacter {};
	const drum::SettledPitch settled = layerOn ? settledFrom(pad, layer, built) : drum::SettledPitch {};
	HitPicture::Controls handles;
	if (layerOn) {
		handles.tune = L.tune.get();
		handles.decay = L.decay.get();
		handles.level = L.level.get();
		handles.sweepTime = uses.sweepTime ? L.sweepTime.get() : nullptr;
		handles.drive = uses.drive ? L.drive.get() : nullptr;
		handles.driveTone = uses.drive ? L.driveTone.get() : nullptr;
		if (uses.snareEnvelopes) {
			handles.wireDecay = L.wireDecay.get();
			handles.clickDecay = L.clickDecay.get();
		}
		if (uses.bell) {
			handles.bellStart = L.bellStart.get();
			handles.bellEnd = L.bellEnd.get();
			handles.bellTime = L.bellTime.get();
			handles.bell2 = L.bell2Hz.get();
			handles.bell2Time = L.bell2Time.get();
		}
		if (uses.kickShape) {
			handles.hold = L.hold.get();
			handles.dip = L.dip.get();
			handles.dipTime = L.dipTime.get();
			handles.clickSweep = L.clickSweep.get();
			handles.clickSweepTime = L.clickSweepTime.get();
			handles.startPhase = L.startPhase.get();
		}
		if (uses.spread) {
			handles.spread = L.spread.get();
		}
		if (uses.clapFilter) {
			handles.clapHighPass = L.clapHighPass.get();
			handles.clapQ = L.clapQ.get();
		}
		const double levelDb = bridge_.modulated(static_cast<clap_id>(layer == 0 ? drumParam(pad, DrumControl::Level)
		                                                                          : drumParam(pad, DrumControl::LayerLevel)));
		picture_.show(kind, lawsFrom(pad, layer, built),
		              std::exp2(bridge_.modulated(static_cast<clap_id>(drumLayerParam(pad, layer, DrumControl::Decay)))), levelDb,
		              handles);
		refreshVectorContext(pad, layer, character);
		L.readout->refresh(drum_pitch::readoutText(settled));
	}
	else {
		picture_.show(kind, drum::Laws {}, 0.5, 0.0, handles);
	}
	if (pad == shownPad_ && layer == shownLayer_ && kit == shownKit_ && kind == shownKind_ && layerOn == shownLayerOn_
	    && gate == shownGate_ && settled.defined == shownSettled_) {
		return false;
	}
	shownPad_ = pad;
	shownLayer_ = layer;
	shownKit_ = kit;
	shownKind_ = kind;
	shownLayerOn_ = layerOn;
	shownGate_ = gate;
	shownSettled_ = settled.defined;
	padsShown_ = kit;
	const auto show = [](auto* frame, bool shown) {
		if (frame->isVisible() != shown) {
			frame->setVisible(shown);
		}
		// A HIDDEN CONTROL IS A DEAD ONE TOO, not merely undrawn (the Explore surround's rule), where the widget can say so.
		if constexpr (requires { frame->setEnabled(shown); }) {
			frame->setEnabled(shown);
		}
	};
	show(pitchedKind_.get(), !kit);
	show(tabs_.get(), kit);
	tabs_->setShown(layerShown_);
	show(&picture_, layerOn);
	// THE LAYOUT: the picture with its handles across the top; under it the engine row (Mode, the Pitched Kind, Seed); then
	// the pads, the pad-level controls, the layer tabs, and the shown tab's body.
	// THE CONTENT IS THE PAD ROW'S WIDTH; the pad fx rail hangs to its right (Kit mode alone, drum proposal §17 P3).
	const float rail = railWidthNow();
	show(lane_.get(), kit);
	lane_->setBounds(std::max(0.0f, width() - rail), 0.0f, rail, height());
	const float w = width() > 0.0f ? std::max(0.0f, width() - rail) : kMinimumWidth;
	picture_.setBounds(kPad, kPad, w - 2.0f * kPad, kPictureHeight);
	const float rowY = kPad + kPictureHeight + kPad;
	mode_->setBounds(kPad, rowY, 150.0f, 24.0f);
	pitchedKind_->setBounds(kPad + 160.0f, rowY - 4.0f, 90.0f, ValueBox::kHeight);
	seed_->setBounds(w - kPad - 80.0f, rowY - 4.0f, 80.0f, ValueBox::kHeight);

	// A ROW-WRAPPING FLOW: items in a line, each `rowH` tall and centred in it, wrapping when the next would pass the right edge.
	struct Flow {
		float x0, xMax, x, y, rowH, gap;
		void place(visage::Frame* f, float width, float height) {
			if (x > x0 && x + width > xMax) {
				x = x0;
				y += rowH + 2.0f;
			}
			f->setBounds(x, y + (rowH - height) * 0.5f, width, height);
			x += width + gap;
		}
		[[nodiscard]] float bottom() const { return y + rowH; }
	};
	const float padLevelY = kit ? padBounds(0).bottom() + kPad : rowY + kRowHeight + kPad;
	float bodyBottom = padLevelY;
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		PadSet& set = pads_[slot];
		const bool own = slot == pad;
		// EVERY CONTROL OF A PAD THAT IS NOT THE SHOWN ONE IS HIDDEN (and dead), both layers'.
		for (size_t l = 0; l < 2; ++l) {
			LayerSet& s = set.layer[l];
			if (own && l == layer) {
				continue;
			}
			for (visage::Frame* f : std::initializer_list<visage::Frame*> {
			         s.kind.get(), s.count.get(), s.driveCurve.get(), s.pitchMode.get(), s.velocityZone.get(), s.wires.get(),
			         s.bells.get(), s.tunePill.get(), s.velocityNoise.get(), s.width.get(), s.levelPill.get(), s.delay.get(),
			         s.highPass.get(), s.lock.get(), s.vectorPad.get(), s.readout.get() }) {
				if (f != nullptr) {
					show(f, false);
				}
			}
			for (auto& f : s.field) {
				show(f.get(), false);
			}
		}
		// EVERY PAD'S TILE WEARS ITS OWN PLAY BUTTON (bottom right, clear of the FX mark and the choke text): it strikes that pad without
		// selecting it, and a press ended by the mode switching lets go.
		show(set.play.get(), kit);
		if (kit) {
			const visage::Bounds tile = padBounds(slot);
			set.play->setBounds(tile.right() - PadPlayButton::kWidth - 3.0f, tile.bottom() - PadPlayButton::kHeight - 3.0f,
			                    PadPlayButton::kWidth, PadPlayButton::kHeight);
		}
		else {
			set.play->release();
		}
		if (!own) {
			for (visage::Frame* f : std::initializer_list<visage::Frame*> { set.note.get(), set.choke.get(), set.voices.get(),
			                                                                set.trigger.get(), set.pan.get(), set.release.get(),
			                                                                set.bend.get(), set.mute.get(), set.solo.get() }) {
				show(f, false);
			}
			continue;
		}
		// THE PAD-LEVEL CONTROLS, one flowing line (two when a Gate pad adds its Release): Note, Choke, Voices (Kit mode), Trigger,
		// Pan, Bend (Kit), Release (Gate).
		Flow flow { kPad, w - kPad, kPad, padLevelY, ValueBox::kHeight, 6.0f };
		const auto seat = [&](visage::Frame* f, bool shown, float width, float height) {
			show(f, shown);
			if (shown) {
				flow.place(f, width, height);
			}
		};
		seat(set.note.get(), kit, 52.0f, ValueBox::kHeight);
		seat(set.choke.get(), kit, 52.0f, ValueBox::kHeight);
		seat(set.voices.get(), kit, 96.0f, 22.0f);
		seat(set.trigger.get(), true, 128.0f, 22.0f);
		seat(set.pan.get(), true, 92.0f, 22.0f);
		seat(set.bend.get(), kit, 92.0f, 22.0f); // the pitch bend's share (decision 8): a kit pad's alone; Pitched mode follows the voice
		seat(set.release.get(), gate, 92.0f, 22.0f);
		seat(set.mute.get(), kit, 54.0f, SwitchChip::kHeight); // the pad's own Mute and the preview of it alone (kit pads only)
		seat(set.solo.get(), kit, 54.0f, SwitchChip::kHeight);
		float y = flow.bottom() + 10.0f;
		LayerSet& s = set.layer[layer];
		// THE LAYER TABS, and beside them the shown layer's Kind (layer two's with Off) and, on layer two, Level, Delay and High Pass.
		if (kit) {
			tabs_->setBounds(kPad, y, 2.0f * LayerTabs::kTabWidth, LayerTabs::kHeight);
			Flow top { 2.0f * LayerTabs::kTabWidth + kPad + 12.0f, w - kPad, 0.0f, y - 2.0f, ValueBox::kHeight, 6.0f };
			top.x = top.x0;
			show(s.kind.get(), true);
			top.place(s.kind.get(), 86.0f, ValueBox::kHeight);
			for (visage::Frame* f : std::initializer_list<visage::Frame*> { s.levelPill.get(), s.delay.get(), s.highPass.get() }) {
				if (f != nullptr) {
					show(f, layerOn);
					if (layerOn) {
						top.place(f, 88.0f, 22.0f);
					}
				}
			}
			y += LayerTabs::kHeight + 12.0f;
		}
		else {
			show(s.kind.get(), false);
		}
		bodyBottom = y;
		if (!layerOn) {
			// AN OFF LAYER HAS A KIND AND NOTHING ELSE: no vector, no pitch, nothing to hear.
			for (visage::Frame* f : std::initializer_list<visage::Frame*> { s.count.get(), s.driveCurve.get(), s.pitchMode.get(),
			                                                                s.velocityZone.get(), s.wires.get(), s.bells.get(),
			                                                                s.tunePill.get(), s.velocityNoise.get(), s.width.get(), s.lock.get(),
			                                                                s.vectorPad.get(), s.readout.get() }) {
				if (f != nullptr) {
					show(f, false);
				}
			}
			for (auto& f : s.field) {
				show(f.get(), false);
			}
			continue;
		}
		// THE TAB'S BODY: the terrain pad and its Lock at the left (Kit mode), everything else flowing at the right.
		float x0 = kPad;
		if (kit) {
			show(s.vectorPad.get(), true);
			s.vectorPad->setBounds(kPad, y, kVectorPadSize, kVectorPadSize);
			show(s.lock.get(), true);
			s.lock->setBounds(kPad, y + kVectorPadSize + 6.0f, 60.0f, SwitchChip::kHeight);
			x0 = kPad + kVectorPadSize + 12.0f;
		}
		else {
			show(s.vectorPad.get(), false);
			show(s.lock.get(), false);
		}
		Flow body { x0, w - kPad, x0, y, 28.0f, 6.0f };
		const auto put = [&](visage::Frame* f, bool shown, float width, float height) {
			show(f, shown);
			if (shown) {
				body.place(f, width, height);
			}
		};
		put(s.tunePill.get(), true, 92.0f, 22.0f);
		put(s.readout.get(), kit && settled.defined, 188.0f, 24.0f);
		put(s.pitchMode.get(), kit && reads.pitch, 196.0f, 22.0f);
		put(s.driveCurve.get(), uses.drive, 184.0f, 22.0f);
		put(s.count.get(), uses.count, 52.0f, ValueBox::kHeight);
		put(s.velocityNoise.get(), uses.velNoise, 92.0f, 22.0f);
		put(s.width.get(), uses.width, 92.0f, 22.0f);
		// THE SEVEN FIELD SCALERS of this layer (Kit mode: the Explore surround holds the kit's trims, and Pitched mode's own seven).
		const std::array<bool, 7> fieldShown { reads.pitch, reads.length, reads.sweep, reads.tone, reads.noise, reads.snap, reads.metal };
		for (size_t f = 0; f < s.field.size(); ++f) {
			put(s.field[f].get(), kit && fieldShown[f], 92.0f, 22.0f);
		}
		// THE ZONE ROWS (drum proposal §14): Velocity on every kind, Wires and Bells on a Snare, one to a line.
		const float zoneH = Knob::rowHeight(kZoneFace);
		float zy = body.bottom() + 6.0f;
		for (const auto& [cell, shown] : { std::pair { s.velocityZone.get(), true }, std::pair { s.wires.get(), uses.wireContour },
		                                   std::pair { s.bells.get(), uses.bell } }) {
			show(cell, shown);
			cell->setFaceDiameter(kZoneFace);
			if (shown) {
				cell->setBounds(x0, zy, w - kPad - x0, zoneH);
				zy += zoneH + 2.0f;
			}
		}
		bodyBottom = std::max(zy, kit ? y + kVectorPadSize + 6.0f + SwitchChip::kHeight : zy);
	}
	// THE PICTURE'S PLACE WHEN LAYER TWO IS OFF is said in words (draw): the content ends where the controls end.
	contentBottom_ = bodyBottom;
	redraw();
	return true;
}

void DrumPanel::resized() {
	shownPad_ = kMaxSounds; // lay out again at the new size
	sync();
}

bool DrumPanel::updateContent() {
	bool moved = sync();
	// THE SOLO CHIPS FOLLOW THE PREVIEW, which moves from the sound cards too and is no parameter any watch would carry.
	const Preview preview = bridge_.preview();
	const std::optional<size_t> solo = preview.active() ? std::optional<size_t> { preview.slot } : std::nullopt;
	if (solo != soloSeen_) {
		soloSeen_ = solo;
		for (PadSet& set : pads_) {
			set.solo->redraw();
		}
		moved = true;
	}
	// THE PAD FX RAIL, RE-ASKED EVERY TICK (the house habit), its cards advanced, its add menu opened once a fresh lane's
	// column exists.
	moved = followLane() || moved;
	moved = lane_->updateCards() > 0 || moved;
	if (pendingAddMenu_ && lane_->column() != nullptr) {
		pendingAddMenu_ = false;
		lane_->column()->showAddMenu();
		moved = true;
	}
	const bool lit = updatePulses(static_cast<int64_t>(visage::time::milliseconds()));
	return moved || lit;
}

void DrumPanel::draw(visage::Canvas& canvas) {
	canvas.setColor(kSurface);
	canvas.fill(0.0f, 0.0f, std::max(0.0f, width() - railWidthNow()), height()); // the rail paints its own
	if (!kitMode()) {
		return;
	}
	const size_t selected = std::min(selection_.slot(), kMaxSounds - 1);
	if (!shownLayerOn_) {
		// AN OFF LAYER HAS NO PICTURE: said in words where the picture would be.
		canvas.setColor(kVoid);
		canvas.roundedRectangle(kPad, kPad, std::max(0.0f, width() - railWidthNow()) - 2.0f * kPad, kPictureHeight, 6.0f);
		canvas.setColor(kMuted);
		canvas.text("Layer 2 is off. Choose its Kind to strike a second hit with this pad.", font(kControlSize), visage::Font::kCenter,
		            kPad, kPad, std::max(0.0f, width() - railWidthNow()) - 2.0f * kPad, kPictureHeight);
	}
	else {
		// THE LAYER'S VECTOR, in numbers beside its Lock under the terrain pad.
		const LayerSet& L = pads_[selected].layer[layerShown_];
		if (L.vectorPad != nullptr && L.vectorPad->isVisible()) {
			char text[48] = {};
			std::snprintf(text, sizeof(text), "x %+.2f   y %+.2f", L.offsetXControl->value(), L.offsetYControl->value());
			canvas.setColor(kMuted);
			canvas.text(text, font(kKnobLabelSize), visage::Font::kLeft, L.lock->right() + 8.0f, L.lock->y(), 110.0f, L.lock->height());
		}
	}
	for (size_t pad = 0; pad < kMaxSounds; ++pad) {
		const visage::Bounds b = padBounds(pad);
		const uint32_t hue = padHue(pad);
		const bool muted = std::lround(bridge_.value(static_cast<clap_id>(drumParam(pad, DrumControl::Mute)))) != 0;
		canvas.setColor(kSurfaceRaised);
		canvas.roundedRectangle(b.x(), b.y(), b.width(), b.height(), 6.0f);
		// THE HIT PULSE: the pad's sound hue washed over it, as bright as the hit, falling over kPulseMs.
		if (const float lit = pulseOf(pad); lit > 0.0f) {
			canvas.setColor(alphaOf(hue, 0.45f * lit));
			canvas.roundedRectangle(b.x(), b.y(), b.width(), b.height(), 6.0f);
		}
		// THE PAD WEARS ITS SOUND'S COLOUR (pad <-> sound at a glance): the border in the hue the sound card's dot
		// and the star wear, brighter when selected.
		canvas.setColor(pad == selected ? hue : alphaOf(hue, 0.45f));
		canvas.roundedRectangleBorder(b.x(), b.y(), b.width(), b.height(), 6.0f, pad == selected ? 2.0f : 1.0f);
		canvas.setColor(alphaOf(hue, muted ? 0.35f : 1.0f));
		canvas.circle(b.x() + 6.0f, b.y() + 7.0f, 6.0f);
		canvas.setColor(muted ? kMuted : kInk);
		const auto kindIndex = std::min(static_cast<size_t>(kindOf(pad)), kDrumKindNames.size() - 1);
		const std::string name = std::string(1, static_cast<char>('A' + pad)) + " " + kDrumKindNames[kindIndex];
		canvas.text(name, font(kKnobLabelSize), visage::Font::kCenter, b.x(), b.y() + 4.0f, b.width(), 14.0f);
		canvas.setColor(kMuted);
		const auto note = std::lround(bridge_.value(static_cast<clap_id>(drumParam(pad, DrumControl::Note))));
		const std::string sub = "note " + std::to_string(note) + (muted ? "  ·  muted" : "");
		canvas.text(sub, font(kKnobLabelSize), visage::Font::kCenter, b.x(), b.y() + 22.0f, b.width(), 14.0f);
		const size_t choke = steppedIndex(bridge_.value(static_cast<clap_id>(drumParam(pad, DrumControl::Choke))),
		                                  kDrumChokeNames.size());
		if (choke != 0) {
			canvas.setColor(kModulation);
			canvas.text(std::string("ch ") + kDrumChokeNames[choke], font(kKnobLabelSize), visage::Font::kCenter, b.x() + 24.0f,
			            b.y() + b.height() - 14.0f, 26.0f, 12.0f); // between the FX mark and the play button
		}
		// THE FX DOT (drum proposal §17 P3): this pad's lane holds a device, so its hits go through it.
		if (padHasFx(pad)) {
			canvas.setColor(kModulation);
			canvas.text("FX", font(kKnobLabelSize), visage::Font::kLeft, b.x() + 5.0f, b.y() + b.height() - 14.0f, 20.0f, 12.0f);
		}
	}
}

uint32_t DrumPanel::padHue(size_t pad) const {
	return kSeedHues[hueIndexOf(bridge_.soundHues(), std::min(pad, kMaxSounds - 1))];
}

bool DrumPanel::updatePulses(int64_t nowMs) {
	const PadHits hits = bridge_.padHits();
	bool lit = false;
	for (size_t pad = 0; pad < kMaxSounds; ++pad) {
		if (hits.count[pad] != pulseSeen_[pad]) {
			pulseSeen_[pad] = hits.count[pad];
			pulseLevel_[pad] = std::clamp(hits.velocity[pad], 0.0f, 1.0f);
			pulseAt_[pad] = nowMs;
		}
		lit = lit || pulseLevel_[pad] > 0.0f;
	}
	pulseNow_ = nowMs;
	if (lit) {
		redraw();
	}
	for (size_t pad = 0; pad < kMaxSounds; ++pad) {
		if (static_cast<float>(nowMs - pulseAt_[pad]) >= kPulseMs) {
			pulseLevel_[pad] = 0.0f; // spent: no more frames owed for it
		}
	}
	return lit;
}

float DrumPanel::pulseOf(size_t pad) const {
	const float age = static_cast<float>(pulseNow_ - pulseAt_[pad]);
	if (pulseLevel_[pad] <= 0.0f || age >= kPulseMs) {
		return 0.0f;
	}
	if (reducedMotionRequested()) {
		return pulseLevel_[pad]; // lit, still, then out
	}
	return pulseLevel_[pad] * std::exp(-3.0f * age / kPulseMs);
}

void DrumPanel::clickPadForTest(size_t pad) {
	if (kitMode() && pad < kMaxSounds) {
		selection_.select(pad);
		sync();
	}
}

visage::Frame* DrumPanel::controlForTest(size_t pad, DrumControl c, size_t layer) const {
	const PadSet& set = pads_[pad];
	const LayerSet& L = set.layer[std::min<size_t>(layer, 1)];
	switch (c) {
	// THE PAD'S OWN.
	case DrumControl::Note: return set.note.get();
	case DrumControl::Choke: return set.choke.get();
	case DrumControl::Voices: return set.voices.get();
	case DrumControl::Trigger: return set.trigger.get();
	case DrumControl::Pan: return set.pan.get();
	case DrumControl::Release: return set.release.get();
	case DrumControl::Bend: return set.bend.get();
	// THE LAYER'S: the kind box (layer one's is the pad's Kind), and every control a hit reads, each layer's own copy.
	case DrumControl::Kind: return set.layer[0].kind.get();
	case DrumControl::LayerKind: return set.layer[1].kind.get();
	case DrumControl::Level: return set.layer[0].levelPill.get(); // the pad's own Level, as the card's fader
	case DrumControl::LayerLevel: return set.layer[1].levelPill.get();
	case DrumControl::LayerDelay: return set.layer[1].delay.get();
	case DrumControl::LayerHighPass: return set.layer[1].highPass.get();
	case DrumControl::Count: return L.count.get();
	case DrumControl::Tune: return L.tunePill.get();
	case DrumControl::DriveCurve: return L.driveCurve.get();
	case DrumControl::PitchMode: return L.pitchMode.get();
	case DrumControl::VelocityNoise: return L.velocityNoise.get();
	case DrumControl::Width: return L.width.get();
	case DrumControl::Lock: return L.lock.get();
	case DrumControl::OffsetX:
	case DrumControl::OffsetY: return L.vectorPad.get();
	case DrumControl::VelocityZone: return L.velocityZone.get();
	case DrumControl::Wires: return L.wires.get();
	case DrumControl::Bells: return L.bells.get();
	case DrumControl::FieldPitch: return L.field[0].get();
	case DrumControl::FieldLength: return L.field[1].get();
	case DrumControl::FieldSweep: return L.field[2].get();
	case DrumControl::FieldTone: return L.field[3].get();
	case DrumControl::FieldNoise: return L.field[4].get();
	case DrumControl::FieldSnap: return L.field[5].get();
	case DrumControl::FieldMetal: return L.field[6].get();
	default: return nullptr;
	}
}

visage::Frame* DrumPanel::fieldPillForTest(size_t pad, size_t layer, size_t field) const {
	return pads_[pad].layer[std::min<size_t>(layer, 1)].field[std::min<size_t>(field, 6)].get();
}

bool DrumPanel::drawnForTest(size_t pad, DrumControl c, size_t layer) const {
	// A frame is drawn when it is visible AND its pad is the shown one (a hidden pad's frames are hidden too, but the pad's kit
	// structure answers for itself).
	if (visage::Frame* f = controlForTest(pad, c, layer); f != nullptr) {
		return f->isVisible();
	}
	const bool selected = shownPad() == pad && (kitMode() ? layerShown_ : 0) == layer;
	using H = HitPicture::Handle;
	switch (c) {
	// THE PAD'S LEVEL is its sound card's, and layer one's Level handle on the picture.
	case DrumControl::Level: return selected && picture_.handleShown(H::Level);
	case DrumControl::SweepTime: return selected && picture_.handleShown(H::SweepTime);
	case DrumControl::Decay: return selected && picture_.handleShown(H::Decay);
	case DrumControl::Drive: return selected && picture_.handleShown(H::Drive);
	case DrumControl::WireDecay: return selected && picture_.handleShown(H::WireDecay);
	case DrumControl::ClickDecay: return selected && picture_.handleShown(H::ClickDecay);
	case DrumControl::BellStart: return selected && picture_.handleShown(H::BellStart);
	case DrumControl::BellEnd:
	case DrumControl::BellTime: return selected && picture_.handleShown(H::BellEnd);
	case DrumControl::Bell2Hz:
		// THE SECOND BELL'S HZ IS REACHABLE ONLY WHILE FREE (the Bells zone's Knock); linked, its ratio is the place's Metal.
		return selected && picture_.handleShown(H::Bell2) && !picture_.lawsForTest().bell2Linked;
	case DrumControl::Bell2Time: return selected && picture_.handleShown(H::Bell2);
	case DrumControl::DriveTone: return selected && picture_.handleShown(H::Drive);
	case DrumControl::StartPhase: return selected && picture_.handleShown(H::StartPhase);
	case DrumControl::Spread: return selected && picture_.handleShown(H::Spread);
	case DrumControl::ClapQ:
	case DrumControl::ClapHighPass: return selected && picture_.handleShown(H::ClapBand);
	case DrumControl::KickHold: return selected && picture_.handleShown(H::Hold);
	case DrumControl::KickDip:
	case DrumControl::KickDipTime: return selected && picture_.handleShown(H::Dip);
	case DrumControl::ClickSweep:
	case DrumControl::ClickSweepTime: return selected && picture_.handleShown(H::ClickSweep);
	default: return false;
	}
}

} // namespace feathers::ui

namespace feathers::ui {

// ---- THE PAD FX RAIL (drum proposal §17 P3) ----------------------------------------------------------------------------------

float DrumPanel::railWidthNow() const {
	return kitMode() ? lane_->railWidth() : 0.0f;
}

visage::Point DrumPanel::minimumSize() const {
	visage::Point p { kMinimumWidth + railWidthNow(), kMinimumHeight };
	if (kitMode() && !lane_->collapsed()) {
		p.y = std::max(p.y, lane_->openMinimumHeight());
	}
	return p;
}

void DrumPanel::setPresentedOn(SurfaceLayers* host) { lane_->setPresentedOn(host); }

bool DrumPanel::padHasFx(size_t pad) const {
	const rack::InnerChain* lane = bridge_.drumPadLane(pad);
	return lane != nullptr && lane->bridge() != nullptr && lane->bridge()->chainLength(0) > 0;
}

bool DrumPanel::followLane() {
	const bool kit = kitMode();
	const size_t pad = std::min(selection_.slot(), kMaxSounds - 1);
	rack::InnerChain* chain = kit ? bridge_.drumPadLane(pad) : nullptr;
	bool moved = lane_->follow(chain);
	// A PHANTOM EXACTLY WHILE THE FOCUSED PAD HAS NO LANE: its (+) builds one and opens its add menu once its column exists.
	const bool wantPhantom = kit && chain == nullptr;
	if (wantPhantom != phantomShown_) {
		phantomShown_ = wantPhantom;
		if (wantPhantom) {
			lane_->setPhantom(kPadLanePasses, [this] {
				if (bridge_.drumPadLane(std::min(selection_.slot(), kMaxSounds - 1), true) != nullptr) {
					pendingAddMenu_ = true;
				}
			});
		}
		else {
			lane_->setPhantom({}, {});
		}
		moved = true;
	}
	return moved;
}

} // namespace feathers::ui
