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


#include "plugin/ui/spectral_expanded.h"

#include <algorithm>
#include <span>

namespace feathers::ui {

namespace sp = fx::spectral;

namespace {
/// A full ValueBox in the header strip, or a compact one in a panel's head; its Choice gets its menu from the descriptor alone.
std::unique_ptr<ValueBox> box(device::Modulatable& control, const char* label, bool compact) {
	auto b = std::make_unique<ValueBox>(control, label);
	b->setCompact(compact);
	return b;
}
} // namespace

SpectralExpanded::SpectralExpanded(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id, Notifier* notifier)
    : SpectralWindowSource(host, registry, id), SpectrumPanel(deviceSource, registry, notifier, true) {
	const auto item = [](std::unique_ptr<visage::Frame> frame, float width, float height = 0.0f) {
		LeadItem i;
		i.frame = std::move(frame);
		i.width = width;
		i.height = height;
		return i;
	};
	// THE HEADER STRIP, above the plot: Resolution (its three in a menu), Key Track and Poly, Offset and Mix. Every control the
	// card's box has, in one line, so the plot keeps the row the box used to take.
	auto resolution = box(deviceSource.deviceControl(sp::Param::Resolution), "Resolution", false);
	auto keyTrack = std::make_unique<SwitchChip>(deviceSource.deviceControl(sp::Param::KeyTrack), "Key");
	auto poly = std::make_unique<SwitchChip>(deviceSource.deviceControl(sp::Param::Poly), "Poly");
	auto offset = box(deviceSource.deviceControl(sp::Param::KeyOffset), "Offset", false);
	// THE DEVICE PRINTS IT (its TextOps): the pitch Offset sets with Key Track off, semitones with it on — as the card's box.
	offset->setReadoutFormatter([this](double v) { return deviceSource.deviceControl(sp::Param::KeyOffset).textAt(v); });
	// TWELVE SEMITONES A FULL DRAG (Z7c): at the default span, the whole -36..+36, it was 0.51 st/px — "really twitchy".
	offset->setDragSpan(kOffsetDragSpan);
	auto mix = box(deviceSource.deviceControl(sp::Param::Mix), "Mix", false);
	resolution_ = resolution.get();
	keyTrack_ = keyTrack.get();
	poly_ = poly.get();
	offset_ = offset.get();
	mix_ = mix.get();
	std::vector<LeadItem> strip;
	strip.push_back(item(std::move(resolution), kResolutionWidth));
	strip.push_back(item(std::move(keyTrack), kChipWidth, SwitchChip::kHeight));
	strip.push_back(item(std::move(poly), kChipWidth, SwitchChip::kHeight));
	strip.push_back(item(std::move(offset), kBoxWidth));
	strip.push_back(item(std::move(mix), kBoxWidth));
	addHeaderStrip(std::move(strip));

	// THE VOCODER'S PANEL (design §10.3), first in the row: its switch, the Carrier and Stereo menus, Freeze and the "sidechain
	// silent" badge in its head; three stacks — Shape, Envelope, Range — and Depth as the one full knob. Hidden under Poly (the
	// per-voice road hears no sidechain).
	auto vocode = std::make_unique<SwitchChip>(deviceSource.deviceControl(sp::Param::VocodeOn), "Vocode");
	auto carrier = box(deviceSource.deviceControl(sp::Param::VocodeCarrier), "Carrier", true);
	auto freeze = std::make_unique<SwitchChip>(deviceSource.deviceControl(sp::Param::VocodeFreeze), "Freeze");
	auto stereo = box(deviceSource.deviceControl(sp::Param::VocodeStereo), "Stereo", true);
	auto badge = std::make_unique<SidechainBadge>();
	vocode_ = vocode.get();
	carrier_ = carrier.get();
	freeze_ = freeze.get();
	stereo_ = stereo.get();
	badge_ = badge.get();
	std::vector<LeadItem> vhead;
	vhead.push_back(item(std::move(vocode), kVocodeChipWidth));
	vhead.push_back(item(std::move(carrier), kHeadBoxWidth));
	vhead.push_back(item(std::move(freeze), kVocodeChipWidth));
	vhead.push_back(item(std::move(stereo), kHeadBoxWidth));
	vhead.push_back(item(std::move(badge), kBadgeWidth));
	std::vector<std::unique_ptr<PillStack>> vstacks;
	vstacks.push_back(makeStack({ sp::Param::VocodeBands, sp::Param::VocodeFlatten, sp::Param::VocodeSibilance },
	                            { "Bands", "Flatten", "Sibilance" }, "Shape"));
	vstacks.push_back(makeStack({ sp::Param::VocodeAttack, sp::Param::VocodeRelease, sp::Param::VocodeGate },
	                            { "Attack", "Release", "Gate" }, "Envelope"));
	vstacks.push_back(makeStack({ sp::Param::VocodeLow, sp::Param::VocodeHigh, sp::Param::VocodeBandwidth },
	                            { "Low", "High", "Bandwidth" }, "Range"));
	for (auto& s : vstacks) {
		vocodeStacks_.push_back(s.get());
	}
	std::vector<std::unique_ptr<Knob>> vknobs;
	vknobs.push_back(std::make_unique<Knob>(deviceSource.deviceControl(sp::Param::VocodeDepth), "Depth", notifier));
	depth_ = vknobs.back().get();
	// ZONE AND WIDTHS: THE HOUSE'S MODE CELLS (Z7: they were not zone-knob widgets; OWLTT's Vibe and Character are
	// the model), after Depth — OWLTT's `addCoord`: the dropdown is the MODE (the named behaviours, then Free, each with its line),
	// the box the value that mode reads. A named mode's amount is fenced 0..1 from Even; Free's coordinate is unbounded ("just keep
	// turning"); Even reads neither, so its box rests muted. At the row's knob face, so the cells line up with Depth.
	const auto modeCell = [this](sp::Param mode, const char* caption, std::span<const char* const> modeNames,
	                             std::span<const char* const> descriptions) {
		std::vector<ZoneSelect::ZoneName> names;
		for (size_t i = 0; i < modeNames.size(); ++i) {
			names.push_back({ modeNames[i], i < descriptions.size() ? descriptions[i] : "" });
		}
		auto cell = std::make_unique<ModeValueCell>(deviceSource.deviceControl(mode), caption, std::move(names), [this](size_t param) {
			if (param >= sp::kParamCount) {
				return false;
			}
			const device::ParamDesc& p = sp::kParamDescs[param];
			const sp::Settings s = deviceSource.settings();
			return device::relevantFor(p, p.gateControl == sp::kZoneControl ? s.vocodeZone : s.vocodeWidthZone);
		});
		for (size_t j = 0; j < sp::kParamCount; ++j) {
			if (sp::kParamDescs[j].gateControl == static_cast<int16_t>(mode)) {
				cell->addFollower(j, std::make_unique<ValueBox>(deviceSource.deviceControl(static_cast<sp::Param>(j)), ""));
			}
		}
		cell->setFaceDiameter(Knob::kEngineRailDiameter);
		cell->refresh();
		return cell;
	};
	auto zone = modeCell(sp::Param::VocodeZone, "Spacing", sp::kZoneNames, sp::kZoneDescriptions);
	auto widthZone = modeCell(sp::Param::VocodeWidthZone, "Widths", sp::kWidthZoneNames, sp::kWidthZoneDescriptions);
	zone_ = zone.get();
	widthZone_ = widthZone.get();
	std::vector<LeadItem> vcells;
	const float zoneWidth = std::max(zone->minimumWidth(), kModeCellWidth);
	const float widthsWidth = std::max(widthZone->minimumWidth(), kModeCellWidth);
	vcells.push_back(item(std::move(zone), zoneWidth));
	vcells.push_back(item(std::move(widthZone), widthsWidth));
	addLeadingPanel("Vocode", std::move(vhead), std::move(vknobs), std::move(vstacks), std::move(vcells));

	// GENERALISED PARTIALS (design §11), second: its switch in its head; THE MODE CELL (backlog OH/OI: the
	// dropdown the cull scheme, the box the count N, as Spacing and Widths are), then Depth, which stays its own knob.
	auto partials = std::make_unique<SwitchChip>(deviceSource.deviceControl(sp::Param::PartialsOn), "Partials");
	partials_ = partials.get();
	std::vector<LeadItem> phead;
	phead.push_back(item(std::move(partials), kVocodeChipWidth));
	std::vector<ZoneSelect::ZoneName> schemes;
	for (size_t i = 0; i < std::size(sp::kPartialsModeNames); ++i) {
		schemes.push_back({ sp::kPartialsModeNames[i], sp::kPartialsModeDescriptions[i] });
	}
	auto partialsMode = std::make_unique<ModeValueCell>(deviceSource.deviceControl(sp::Param::PartialsMode), "Partials",
	                                                    std::move(schemes), [this](size_t param) {
		                                                    if (param >= sp::kParamCount) {
			                                                    return false;
		                                                    }
		                                                    const device::ParamDesc& p = sp::kParamDescs[param];
		                                                    return device::relevantFor(p, deviceSource.settings().partialsMode);
	                                                    });
	// ITS BOXES: the count (Peaks, Harmonics) and Centers' set, each shown while its scheme reads it (the gates).
	for (const sp::Param p : { sp::Param::PartialsCount, sp::Param::PartialsSet }) {
		partialsMode->addFollower(static_cast<size_t>(p), std::make_unique<ValueBox>(deviceSource.deviceControl(p), ""));
	}
	partialsMode->setFaceDiameter(Knob::kEngineRailDiameter);
	partialsMode->refresh();
	partialsMode_ = partialsMode.get();
	std::vector<std::unique_ptr<Knob>> pknobs;
	pknobs.push_back(std::make_unique<Knob>(deviceSource.deviceControl(sp::Param::PartialsDepth), "Depth", notifier));
	partialsDepth_ = pknobs.back().get();
	std::vector<LeadItem> pcells;
	const float schemeWidth = std::max(partialsMode->minimumWidth(), kModeCellWidth);
	pcells.push_back(item(std::move(partialsMode), schemeWidth));
	addLeadingPanel("Partials", std::move(phead), std::move(pknobs), {}, std::move(pcells));
}

std::unique_ptr<PillStack> SpectralExpanded::makeStack(const std::vector<sp::Param>& params, const std::vector<const char*>& captions,
                                                       const char* title) {
	std::array<PillStack::Spec, PillStack::kCount> specs {};
	for (size_t k = 0; k < params.size() && k < specs.size(); ++k) {
		specs[k] = { &deviceSource.deviceControl(params[k]), nullptr, captions[k] };
	}
	auto stack = params.size() == PillStack::kCount
	                 ? std::make_unique<PillStack>(std::move(specs), title)
	                 : std::make_unique<PillStack>(std::array<PillStack::Spec, 2> { specs[0], specs[1] }, title);
	for (size_t k = 0; k < params.size(); ++k) {
		pills_.emplace_back(params[k], &stack->pill(k));
	}
	return stack;
}

bool SpectralExpanded::updateContent() {
	if (!isVisible()) {
		return false;
	}
	// THE STRIP GREYS WITH THE DEVICE, as the operators' panels do (the panel's sync, master = Enabled).
	const bool on = deviceSource.master();
	resolution_->setEnabled(on);
	keyTrack_->setEnabled(on);
	poly_->setEnabled(on);
	offset_->setEnabled(on);
	mix_->setEnabled(on);
	// THE VOCODER'S PANEL: out of the row under Poly; its controls live while it is on; the badge while nothing sounds there.
	const sp::Settings s = deviceSource.settings();
	setLeadingPanelHidden(0, s.poly);
	vocode_->setEnabled(on);
	carrier_->setEnabled(on && s.vocodeOn);
	freeze_->setEnabled(on && s.vocodeOn);
	stereo_->setEnabled(on && s.vocodeOn);
	zone_->setEnabled(on && s.vocodeOn);
	widthZone_->setEnabled(on && s.vocodeOn);
	// A MODE MOVED: the box of the value it reads shows (`ModeValueCell::refresh`); none in Even.
	bool modes = zone_->refresh();
	modes = widthZone_->refresh() || modes;
	for (PillStack* stack : vocodeStacks_) {
		stack->setActive(on && s.vocodeOn);
	}
	depth_->setActive(on && s.vocodeOn);
	badge_->setSilent(s.vocodeOn && !s.poly && !deviceSource.sidechainSounding());
	partials_->setEnabled(on);
	partialsMode_->setEnabled(on && s.partialsOn);
	modes = partialsMode_->refresh() || modes;
	partialsDepth_->setActive(on && s.partialsOn);
	return sync(0, true) || modes;
}

void SpectralExpanded::modulationBound(ModState* mod, EditHistory* history) {
	SpectrumPanel::modulationBound(mod, history);
	depth_->setModulation(mod, history);
}

visage::Frame* SpectralExpanded::boxWidgetForTest(sp::Param p) const {
	switch (p) {
	case sp::Param::Resolution: return resolution_;
	case sp::Param::KeyTrack: return keyTrack_;
	case sp::Param::Poly: return poly_;
	case sp::Param::KeyOffset: return offset_;
	case sp::Param::Mix: return mix_;
	case sp::Param::VocodeOn: return vocode_;
	case sp::Param::VocodeCarrier: return carrier_;
	case sp::Param::VocodeFreeze: return freeze_;
	case sp::Param::VocodeStereo: return stereo_;
	case sp::Param::VocodeDepth: return depth_;
	case sp::Param::VocodeZone: return zone_;
	case sp::Param::VocodeZoneAmount:
	case sp::Param::VocodeZoneFree: return zone_->boxFor(static_cast<size_t>(p));
	case sp::Param::VocodeWidthZone: return widthZone_;
	case sp::Param::VocodeWidthAmount:
	case sp::Param::VocodeWidthFree: return widthZone_->boxFor(static_cast<size_t>(p));
	case sp::Param::PartialsOn: return partials_;
	case sp::Param::PartialsMode: return partialsMode_;
	case sp::Param::PartialsCount:
	case sp::Param::PartialsSet: return partialsMode_->boxFor(static_cast<size_t>(p));
	case sp::Param::PartialsDepth: return partialsDepth_;
	default: break;
	}
	for (const auto& [param, pill] : pills_) {
		if (param == p) {
			return pill;
		}
	}
	return nullptr;
}

} // namespace feathers::ui
