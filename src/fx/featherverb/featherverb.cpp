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

#include "fx/featherverb/featherverb.h"

#include "character/phi_triangle.hpp"
#include "fx/conv/ir_file.h"
#include "fx/conv/ir_ref.h"
#include "fx/conv/test_ir.h"
#include "mod/mseg/mseg_geometry.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <cstdio>
#include <new>

namespace feathers::fx::featherverb {

namespace {

const device::ParamDesc* descOf(Param param) {
	const auto index = static_cast<size_t>(param);
	if (index >= std::size(kParamDescs)) {
		return nullptr;
	}
	return &kParamDescs[index];
}

float coerce(const device::ParamDesc& desc, float value) {
	if (!std::isfinite(value)) {
		return static_cast<float>(desc.def);
	}
	const float clamped = device::coerceControl(desc, value);
	return desc.stepped ? static_cast<float>(std::lround(clamped)) : clamped;
}

int32_t zoneOfUnit(float x) noexcept {
	const float c = std::clamp(std::isfinite(x) ? x : 0.0f, 0.0f, 1.0f);
	return static_cast<int32_t>(std::lround(c * static_cast<float>(kZoneMax)));
}
/// ONE COUNTER FOR EVERY INSTANCE, so two devices' sources never share a serial (the ABA rule).
uint64_t nextSourceSerial() {
	static std::atomic<uint64_t> serial { 0 };
	return serial.fetch_add(1, std::memory_order_relaxed) + 1;
}


/// A Feathers sample, already at the reference's scale, to the q31 the core reads. Saturating: a
/// Feathers signal 42 dB over full scale is past the reference's mix too, where it would have saturated.
int32_t toQ31(double x) noexcept {
	if (!(x == x)) {
		return 0;
	}
	constexpr double kMax = static_cast<double>(std::numeric_limits<int32_t>::max());
	constexpr double kMin = static_cast<double>(std::numeric_limits<int32_t>::min());
	return static_cast<int32_t>(std::lrint(std::clamp(x, kMin, kMax)));
}

} // namespace

//--------------------------------------------------------------------------------------------------//
// THE CODEC.                                                                                         //
//--------------------------------------------------------------------------------------------------//

Row rowOf(const State& state) {
	Row row {};
	const Settings& s = state.settings;
	const auto set = [&row](Param p, float v) { row[static_cast<size_t>(p)] = v; };
	set(Param::Enabled, s.enabled ? 1.0f : 0.0f);
	set(Param::Mix, s.mix);
	set(Param::Room, s.room);
	set(Param::Damping, s.damping);
	set(Param::Width, s.width);
	set(Param::Hpf, s.hpfLog2Hz);
	set(Param::Lpf, s.lpfLog2Hz);
	set(Param::Matrix, s.matrix);
	set(Param::SizeMode, static_cast<float>(s.sizeMode));
	set(Param::Size, s.size);
	set(Param::Decay, s.decay);
	set(Param::Predelay, s.predelay);
	set(Param::Character, s.character);
	set(Param::Oversample, s.oversample ? 1.0f : 0.0f);
	set(Param::ConvMode, static_cast<float>(s.convMode));
	set(Param::ConvBlend, s.convBlend);
	set(Param::ConvPredelay, s.convPredelay);
	set(Param::ConvFeedback, s.convFeedback);
	set(Param::InputGain, s.inputGainDb);
	return row;
}

State stateOf(const Row& row, size_t fieldsPresent) {
	State out {};
	const auto read = [&](Param p) {
		const device::ParamDesc* d = descOf(p);
		if (d == nullptr) {
			return 0.0f;
		}
		return static_cast<size_t>(p) < fieldsPresent ? coerce(*d, row[static_cast<size_t>(p)])
		                                              : static_cast<float>(d->def);
	};
	Settings& s = out.settings;
	s.enabled = read(Param::Enabled) >= 0.5f;
	s.mix = read(Param::Mix);
	s.room = read(Param::Room);
	s.damping = read(Param::Damping);
	s.width = read(Param::Width);
	s.hpfLog2Hz = read(Param::Hpf);
	s.lpfLog2Hz = read(Param::Lpf);
	s.matrix = read(Param::Matrix);
	s.sizeMode = std::clamp(static_cast<int>(std::lround(read(Param::SizeMode))), 0, kModes - 1);
	s.size = read(Param::Size);
	s.decay = read(Param::Decay);
	s.predelay = read(Param::Predelay);
	s.character = read(Param::Character);
	s.oversample = read(Param::Oversample) >= 0.5f;
	s.convMode = std::clamp(static_cast<int>(std::lround(read(Param::ConvMode))), 0, kConvModes - 1);
	s.convBlend = read(Param::ConvBlend);
	s.convPredelay = read(Param::ConvPredelay);
	s.convFeedback = read(Param::ConvFeedback);
	s.inputGainDb = read(Param::InputGain);
	return out;
}

namespace {
/// ELEVEN PHI TRIANGLES, OFFSET 0, so every one is 0 at coordinate 0 and the core is the firmware there.
/// Distinct powers of phi, so no two ever realign: the coordinate never repeats.
constexpr std::array<owlet::dsp::phi::PhiTriConfig, kCharacterTriangles> kCharacterBank = {{
    {owlet::dsp::phi::kPhi033, 1.0f, 0.0f, true},
    {owlet::dsp::phi::kPhi067, 1.0f, 0.0f, true},
    {owlet::dsp::phi::kPhi100, 1.0f, 0.0f, true},
    {owlet::dsp::phi::kPhi150, 1.0f, 0.0f, true},
    {owlet::dsp::phi::kPhi200, 1.0f, 0.0f, true},
    {owlet::dsp::phi::kPhi250, 1.0f, 0.0f, true},
    // WIDTH, every mode.
    {owlet::dsp::phi::kPhi125, 1.0f, 0.0f, true},
    // THE POCKETS: UNIPOLAR and LOW DUTY, so each is 0 for most of its cycle (and at 0).
    {owlet::dsp::phi::kPhi175, 0.22f, 0.0f, false},
    {owlet::dsp::phi::kPhi225, 0.18f, 0.0f, false},
    // OWL'S REGISTER AND FOCUS (the other modes do not read these two yet).
    {owlet::dsp::phi::kPhi275, 1.0f, 0.0f, true},
    {owlet::dsp::phi::kPhi300, 1.0f, 0.0f, true},
}};
} // namespace

static_assert(kCharacterTriangles == 11, "featherverb_types.h declares the bank as eleven");
static_assert(conv::kIrGeneratedRooms == static_cast<uint32_t>(conv::kGeneratedRooms),
              "a body names only the rooms this build makes");
std::array<float, kCharacterTriangles> characterBankOf(float character) noexcept {
	const double phase = std::isfinite(character) ? static_cast<double>(character) : 0.0;
	return owlet::dsp::phi::evalTriangleBank<kCharacterTriangles>(phase, 1.0f, kCharacterBank);
}

int32_t zone1Of(const Settings& s) noexcept { return zoneOfUnit(s.matrix); }
int32_t zone3Of(const Settings& s) noexcept { return zoneOfUnit(s.decay); }
int32_t zone2Of(const Settings& s) noexcept {
	const int mode = std::clamp(s.sizeMode, 0, kModes - 1);
	const float c = std::clamp(std::isfinite(s.size) ? s.size : 0.0f, 0.0f, 1.0f);
	return kModeZone[mode] * kZoneWidth + static_cast<int32_t>(std::lround(c * static_cast<float>(kZoneWidth - 1)));
}

//--------------------------------------------------------------------------------------------------//
// THE DEVICE.                                                                                        //
//--------------------------------------------------------------------------------------------------//

void Featherverb::prepare(double sampleRate) {
	sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
	// THE ONE ALLOCATION: 78.9 kB at 44.1 kHz, proportionally more at higher rates — and sized for TWICE the rate,
	// so Oversample (backlog JE) turns on at the next buffer boundary with nothing to allocate.
	buffer_.assign(Core::bufferFloatsFor(2.0 * sampleRate_), 0.0f);
	osFadeStep_ = static_cast<float>(1.0 / (kOversampleFadeSeconds * sampleRate_));
	{
		// THE WET'S CEILING (`dsp::LookaheadLimiter`): its lookahead a fixed time, so a fixed count at this rate.
		const auto lookahead = static_cast<size_t>(std::max(1L, std::lround(kWetLookaheadSeconds * sampleRate_)));
		limiter_.prepare(sampleRate_, lookahead);
		limiter_.setParams({ .ceiling = kWetCeiling, .lookahead = lookahead, .releaseSeconds = kWetLimiterReleaseSeconds, .link = true });
	}
	// INPUT AND THE METER (backlog LA): the gain's glide and the readings' hold, at this rate.
	inputGain_.prepare(sampleRate_);
	inPeak_.prepare(sampleRate_);
	wetPeak_.prepare(sampleRate_);
	limiterGr_.prepare(sampleRate_);
	aurora_.prepare(sampleRate_);
	mixCoeff_ = static_cast<float>(1.0 - std::exp(-1.0 / (kMixSmoothingSeconds * sampleRate_)));
	convCoeff_ = static_cast<float>(1.0 - std::exp(-1.0 / (kConvGainSeconds * sampleRate_)));
	// THE CONVOLUTION, sized for the longest IR a file may bring, and its IR made at this rate: the file's,
	// read again (a rate change resamples it afresh), or the generated room. It allocates: prepare is the place.
	conv_.prepare(static_cast<size_t>(std::ceil(kConvMaxIrSeconds * sampleRate_)));
	swapStep_ = static_cast<float>(1.0 / (kConvSwapSeconds * sampleRate_));
	convOutStep_ = static_cast<float>(1.0 / (kConvGainSeconds * sampleRate_));
	{
		size_t ring = 1;
		while (ring < static_cast<size_t>(std::ceil(kConvPredelayMaxSeconds * sampleRate_)) + 512) {
			ring *= 2;
		}
		preRing_.assign(ring, 0.0f);
		fbRing_.assign(ring, 0.0f);
		exciteRing_.assign(1024, 0.0f); // a power of two past kExciteLagFrames + a chunk
		convPredelayCoeff_ = static_cast<float>(1.0 - std::exp(-1.0 / (kConvPredelayGlideSeconds * sampleRate_)));
	}
	// The source again at this rate — the file read again, or the room made again — shaped as it was, or
	// nothing if the file has gone.
	if (readSource()) {
		install(shapedKernel());
	}
	else {
		whole_ = {};
		sourceSerial_ = nextSourceSerial();
		install(std::make_unique<conv::ConvKernel>());
	}
	gateFrames_ = static_cast<int64_t>(std::llround(kGateSeconds * sampleRate_));
	reset();
}

void Featherverb::reset() noexcept {
	// **LAZY, AND THAT IS WHAT MAKES IT "AS IF JUST SWITCHED ON".** The setters are order-dependent
	// (plan, trap 2), so a core rebuilt here under the settings of the moment and then handed new ones
	// before its first sample would have arrived at them by a different road than a device switched on
	// with them: diffs applied one setter at a time, against the song-load order. `test_fx_devices`'
	// reset sweep does exactly that and caught it. So reset only marks the core stale; the first
	// buffer boundary rebuilds it from whatever the settings are THEN — the reference's `setModel`
	// (construct, allocate) and `getReverbParamsFromSong` (every setter, in its order).
	rebuildPending_ = true;
	gridPos_ = 0;
	// And the gate SHUT, as at the reference's power-on (`timeThereWasLastSomeReverb` starts 12 s stale):
	// the first frame that sends anything opens it.
	running_ = false;
	quietFrames_ = 0;
	// THE CONVOLVER, AS IF JUST SWITCHED ON, now (it does not wait on the reverb's grid: in Conv Only the
	// reverb never runs). Its routing is PLACED at the next call, once the settings are the ones it runs on.
	conv_.reset();
	convQuiet_ = UINT64_MAX / 2;
	convPlace_ = true;
	aurora_.reset();
	std::fill(preRing_.begin(), preRing_.end(), 0.0f);
	std::fill(fbRing_.begin(), fbRing_.end(), 0.0f);
	std::fill(exciteRing_.begin(), exciteRing_.end(), 0.0f);
	ringPos_ = 0;
	exciteT_ = 0;
	// A swap in progress is abandoned, not lost: a waiting kernel is still waiting, and the convolver is
	// silent now, so the next chunk takes it with no dip.
	swapPhase_ = 0;
	swapGain_ = 1.0f;
	// THE WET LIMITER: nothing in its lookahead, the gain at 1.
	limiter_.reset();
	// INPUT PLACED at the next call, not glided up from wherever it was; the meter empty.
	inputGain_.reset();
	inPeak_.reset();
	wetPeak_.reset();
	limiterGr_.reset();
}

void Featherverb::publish() noexcept {
	if (sink_.publish != nullptr) {
		// `Reading`'s declared order — the enum is the contract with the card.
		const float values[static_cast<size_t>(Reading::kCount)] = { inPeak_.reading(), wetPeak_.reading(),
			                                                         limiterGr_.reading() };
		sink_.publish(sink_.context, values, static_cast<size_t>(Reading::kCount));
	}
}

//--------------------------------------------------------------------------------------------------//
// THE IMPULSE RESPONSE: MESSAGE THREAD                                                               //
//--------------------------------------------------------------------------------------------------//

namespace {

std::unique_ptr<conv::ConvKernel> kernelOf(const std::vector<float>& left, const std::vector<float>& right, bool lite) {
	auto k = std::make_unique<conv::ConvKernel>();
	k->build(left.data(), right.data(), std::min(left.size(), right.size()), lite);
	return k;
}

} // namespace

Featherverb::~Featherverb() {
	collect();
	delete pending_.exchange(nullptr);
	delete runningKernel_;
	runningKernel_ = nullptr;
}

void Featherverb::collect() noexcept {
	for (auto& slot : retired_) {
		delete slot.exchange(nullptr, std::memory_order_acq_rel);
	}
}

void Featherverb::post(std::unique_ptr<conv::ConvKernel> kernel) {
	collect();
	// A KERNEL STILL WAITING is one the audio thread never took, so it is this thread's to free: the
	// exchange hands it back to exactly one owner.
	delete pending_.exchange(kernel.release(), std::memory_order_acq_rel);
}

void Featherverb::install(std::unique_ptr<conv::ConvKernel> kernel) {
	collect();
	delete pending_.exchange(nullptr, std::memory_order_acq_rel);
	delete runningKernel_;
	runningKernel_ = kernel.release();
	(void)conv_.setKernel(runningKernel_);
	convQuiet_ = UINT64_MAX / 2;
	swapPhase_ = 0;
	swapGain_ = 1.0f;
}

conv::LoadedIr Featherverb::generatedWhole(double sampleRate, int which) {
	conv::GeneratedIr g = conv::generateRoom(which, sampleRate);
	conv::LoadedIr ir;
	ir.ok = true;
	ir.sampleRate = sampleRate;
	ir.fileSampleRate = sampleRate;
	ir.fileChannels = 2;
	ir.left = std::move(g.left);
	ir.right = std::move(g.right);
	return ir;
}


std::vector<float> Featherverb::envelopeOf(const conv::IrShape& shape) {
	const bool flat = shape.envCount == 1 && shape.envStart == 1.0f && shape.envSegment[0][1] == 1.0f;
	if (flat) {
		return {};
	}
	// THE MSEG'S OWN SHAPE, evaluated in the x its editor draws (`mseg::levelAtX`), so the gain applied is
	// the line drawn over the span.
	const mod::mseg::Settings m = envelopeSettingsOf(shape);
	std::vector<float> table(kEnvelopePoints);
	for (size_t i = 0; i < table.size(); ++i) {
		table[i] = mod::mseg::levelAtX(m, static_cast<float>(i) / static_cast<float>(table.size() - 1));
	}
	return table;
}

mod::mseg::Settings Featherverb::envelopeSettingsOf(const conv::IrShape& shape) {
	mod::mseg::Settings m;
	m.startLevel = shape.envStart;
	m.count = std::clamp<size_t>(shape.envCount, 1, conv::kIrEnvSegments);
	for (size_t i = 0; i < conv::kIrEnvSegments && i < mod::mseg::kMaxSegments; ++i) {
		m.segment[i].share = shape.envSegment[i][0];
		m.segment[i].level = shape.envSegment[i][1];
		m.segment[i].shape = shape.envSegment[i][2];
	}
	m.bipolar = false;
	return m;
}

conv::IrShape Featherverb::shapeOfEnvelope(conv::IrShape shape, const mod::mseg::Settings& m) {
	shape.envStart = std::clamp(m.startLevel, 0.0f, 1.0f);
	shape.envCount = static_cast<uint32_t>(std::clamp<size_t>(m.count, 1, conv::kIrEnvSegments));
	for (size_t i = 0; i < conv::kIrEnvSegments && i < mod::mseg::kMaxSegments; ++i) {
		shape.envSegment[i] = { m.segment[i].share, std::clamp(m.segment[i].level, 0.0f, 1.0f),
			                    std::clamp(m.segment[i].shape, -1.0f, 1.0f) };
	}
	return shape;
}

conv::LoadedIr Featherverb::shaped(const conv::IrShape& shape) const {
	return conv::shapeIr(whole_, shape.begin, shape.end, shape.stretch, envelopeOf(shape), kConvMaxIrSeconds);
}

std::unique_ptr<conv::ConvKernel> Featherverb::shapedKernel() {
	const conv::LoadedIr span = shaped(shape_);
	if (!span.ok) {
		irError_ = span.error;
		irTruncated_ = false;
		return std::make_unique<conv::ConvKernel>(); // runs nothing
	}
	irError_.clear();
	irTruncated_ = span.truncated || whole_.truncated;
	return kernelOf(span.left, span.right, shape_.lite);
}

bool Featherverb::readWhole(const std::string& path) {
	conv::LoadedIr whole = conv::loadIrFile(path, sampleRate_);
	if (!whole.ok) {
		irError_ = whole.error;
		return false;
	}
	whole_ = std::move(whole);
	sourceSerial_ = nextSourceSerial();
	return true;
}

bool Featherverb::readSource() {
	if (irPath_.empty()) {
		whole_ = generatedWhole(sampleRate_, generated_);
		sourceSerial_ = nextSourceSerial();
		return true;
	}
	return readWhole(irPath_);
}

bool Featherverb::loadIr(const std::string& path) {
	conv::LoadedIr whole = conv::loadIrFile(path, sampleRate_);
	if (!whole.ok) {
		irError_ = whole.error;
		return false; // the old IR, its source and its shape all kept
	}
	// A NEW FILE STARTS WHOLE: the markers go back to its ends. The stretch and the envelope stay — they
	// are how this device shapes a room, not facts about the last file.
	conv::IrShape shape = shape_;
	shape.begin = 0.0f;
	shape.end = 1.0f;
	std::swap(whole_, whole);
	const conv::LoadedIr span = shaped(shape);
	if (!span.ok) {
		std::swap(whole_, whole);
		irError_ = span.error;
		return false;
	}
	irPath_ = path;
	sourceSerial_ = nextSourceSerial();
	shape_ = shape;
	irError_.clear();
	irTruncated_ = span.truncated || whole_.truncated;
	post(kernelOf(span.left, span.right, shape.lite));
	return true;
}

void Featherverb::useGeneratedIr(int which) {
	generated_ = std::clamp(which, 0, conv::kGeneratedRooms - 1);
	irPath_.clear();
	shape_.begin = 0.0f;
	shape_.end = 1.0f;
	whole_ = generatedWhole(sampleRate_, generated_);
	sourceSerial_ = nextSourceSerial();
	post(shapedKernel());
}

bool Featherverb::setIrShape(const conv::IrShape& wanted) {
	// Checked by the same rules a patch's body is, so a hand cannot set what a load would refuse.
	conv::IrRef probe;
	probe.shape = wanted;
	const std::vector<uint8_t> bytes = conv::encodeIrRef(probe);
	if (!conv::decodeIrRef(bytes.data(), bytes.size(), probe)) {
		return false;
	}
	if (wanted == shape_) {
		return true;
	}
	if (!whole_.ok) {
		return false; // a file that has gone: nothing to shape
	}
	const conv::LoadedIr span = shaped(wanted);
	if (!span.ok) {
		return false; // a silent span: the shape stays as it was
	}
	shape_ = wanted;
	irTruncated_ = span.truncated || whole_.truncated;
	post(kernelOf(span.left, span.right, wanted.lite));
	return true;
}

bool Featherverb::setIrTrim(conv::IrTrim trim) {
	conv::IrShape s = shape_;
	s.begin = trim.begin;
	s.end = trim.end;
	return setIrShape(s);
}

bool Featherverb::setIrEnvelope(const mod::mseg::Settings& envelope) {
	return setIrShape(shapeOfEnvelope(shape_, envelope));
}

void Featherverb::saveBody(std::vector<uint8_t>& out) const {
	// ALWAYS WRITTEN, the generated room included: an EMPTY body would send a host back to the body it
	// stored at load (`rack::patchOfRecord` falls back to it), which is the file this device left.
	conv::IrRef ref;
	ref.shape = shape_;
	ref.generated = static_cast<uint8_t>(generated_);
	if (!irPath_.empty()) {
		ref.kind = conv::IrRef::Kind::File;
		ref.path = irPath_;
	}
	out = conv::encodeIrRef(ref);
}

bool Featherverb::loadBody(const uint8_t* data, size_t bytes) {
	// **EMPTY IS THE DEVICE'S OWN START** — the generated Room, a flat shape — as a fresh Featherverb has it: what a
	// device preset's "Init" carries (`initDevicePreset` writes no body). A body that does not decode is refused.
	conv::IrRef ref;
	if (bytes != 0 && !conv::decodeIrRef(data, bytes, ref)) {
		return false; // all or nothing: as it was
	}
	shape_ = ref.shape;
	// **A FILE THAT HAS GONE KEEPS ITS NAME AND ITS SHAPE.** The patch still names them, so saving again
	// does not lose them, and the convolution runs nothing rather than a room the patch never asked for.
	// `irError` says so.
	irPath_ = ref.kind == conv::IrRef::Kind::File ? ref.path : std::string();
	if (ref.kind != conv::IrRef::Kind::File) {
		generated_ = ref.generated;
	}
	if (readSource()) {
		post(shapedKernel());
	}
	else {
		whole_ = {};
		sourceSerial_ = nextSourceSerial();
		irTruncated_ = false;
		post(std::make_unique<conv::ConvKernel>());
	}
	return true;
}

//--------------------------------------------------------------------------------------------------//
// THE IMPULSE RESPONSE: AUDIO THREAD                                                                 //
//--------------------------------------------------------------------------------------------------//

void Featherverb::killConv() noexcept {
	conv_.reset();
	std::fill(preRing_.begin(), preRing_.end(), 0.0f);
	std::fill(fbRing_.begin(), fbRing_.end(), 0.0f);
	std::fill(exciteRing_.begin(), exciteRing_.end(), 0.0f);
	convQuiet_ = UINT64_MAX / 2;
	convFeedbackNow_ = 0.0f;
	convDead_ = true;
}

bool Featherverb::swapNow() noexcept {
	conv::ConvKernel* next = pending_.exchange(nullptr, std::memory_order_acq_rel);
	if (next == nullptr) {
		return false;
	}
	conv::ConvKernel* old = runningKernel_;
	if (conv_.setKernel(next)) {
		runningKernel_ = next;
	}
	else {
		old = next; // longer than the capacity: cannot happen (files are cut to it), and is put down unrun
	}
	convQuiet_ = UINT64_MAX / 2; // the convolver was just cleared: silent until it is fed
	if (old != nullptr) {
		for (auto& slot : retired_) {
			conv::ConvKernel* empty = nullptr;
			if (slot.compare_exchange_strong(empty, old, std::memory_order_acq_rel)) {
				return true;
			}
		}
		// Unreachable: `swapBegin` begins nothing without an empty slot, and only this thread fills them.
	}
	return true;
}

void Featherverb::swapBegin() noexcept {
	if (swapPhase_ == 1) {
		if (swapGain_ <= 0.0f) {
			(void)swapNow();
			swapPhase_ = 2;
		}
		return;
	}
	if (swapPhase_ != 0 || pending_.load(std::memory_order_acquire) == nullptr) {
		return;
	}
	bool slotFree = false;
	for (const auto& slot : retired_) {
		slotFree = slotFree || slot.load(std::memory_order_acquire) == nullptr;
	}
	if (!slotFree) {
		return; // the old IR plays on until the message thread has been back
	}
	// SILENT, NOTHING TO DIP: the convolver holds only zeros (never fed, or fed zeros for longer than its
	// IR), so it is taken at once — which is also every swap before audio has run.
	if (convQuiet_ >= conv_.irFrames() + conv::Convolver::kBlock) {
		(void)swapNow();
		return;
	}
	swapPhase_ = 1;
}

void Featherverb::swapRamp(float* convL, float* convR, int n) noexcept {
	if (swapPhase_ == 0) {
		return;
	}
	for (int k = 0; k < n; ++k) {
		swapGain_ = swapPhase_ == 1 ? std::max(0.0f, swapGain_ - swapStep_) : std::min(1.0f, swapGain_ + swapStep_);
		convL[k] *= swapGain_;
		convR[k] *= swapGain_;
	}
	if (swapPhase_ == 2 && swapGain_ >= 1.0f) {
		swapPhase_ = 0;
	}
}

void Featherverb::rebuild() noexcept {
	// AT THE RATE THE SWITCH ASKS FOR (backlog JE), OVER A CLEARED BUFFER. The core does not clear the buffer it is
	// handed, and the firmware's `setModel` allocates a fresh one: without this a reset's rebuild left the old tail in
	// the lines to come back, and a change of rate would read one layout's audio through the other's offsets.
	coreOs_ = target_.oversample ? 2 : 1;
	std::fill(buffer_.begin(), buffer_.end(), 0.0f);
	core_ = Core(sampleRate_ * static_cast<double>(coreOs_), buffer_.data());
	core_.setOwlProbeForTest(owlProbe_);
	// OWL'S COMPRESSOR, ALWAYS (I like owl comp. we can make it always on and drop the button): a
	// peak compressor on Owl's loop audio, so a hit cannot run it into the ceiling (`Core::setOwlCompressor`). The core
	// keeps it OFF by default, so the parity cases that drive the core directly still compare against the firmware.
	core_.setOwlCompressor(owlCompressor_);
	// AND THE MODULATION RAMPED, NOT STEPPED (`Core::setSmoothModulation`): Sky's attack clicks and Owl's tail noise.
	core_.setSmoothModulation(smoothModulation_);
	upIn_.reset();
	upInj_.reset();
	downL_.reset();
	downR_.reset();
	osFade_ = 1.0f; // the new core is empty: it starts from silence of its own, with nothing to fade in
	// PREDELAY IS ONLY A DELAY here; its firmware coupling to the envelope and to Owl's
	// attack is Character's (`Core::setPredelayDecoupled`).
	core_.setPredelayDecoupled(true);
	applyChanges(true);
	rebuildPending_ = false;
}

void Featherverb::applyChanges(bool all) noexcept {
	const Settings& t = target_;
	const Settings& a = applied_;
	// `getReverbParamsFromSong`'s order: room, LPF, damping, width. Then HPF, which that function
	// does not set (a menu does), then the four Featherverb-only settings in the order it sets them.
	if (all || t.room != a.room) {
		core_.setRoomSize(t.room);
	}
	if (all || t.lpfLog2Hz != a.lpfLog2Hz) {
		// THE CORNER IN HZ (backlog LB), at this core's rate; or the firmware's knob, for the parity case.
		if (firmwareLpf_ >= 0.0f) {
			core_.setLPF(firmwareLpf_);
			core_.setLpfHz(0.0f);
		}
		else {
			core_.setLpfHz(std::exp2(t.lpfLog2Hz));
		}
	}
	if (all || t.damping != a.damping) {
		core_.setDamping(t.damping);
	}
	if (all || t.width != a.width) {
		core_.setWidth(t.width);
	}
	if (all || t.hpfLog2Hz != a.hpfLog2Hz) {
		if (firmwareHpf_ >= 0.0f) {
			core_.setHPF(firmwareHpf_);
			core_.setHpfHz(0.0f);
		}
		else {
			core_.setHpfHz(std::exp2(t.hpfLog2Hz));
		}
	}
	if (all || zone1Of(t) != zone1Of(a)) {
		core_.setZone1(zone1Of(t));
	}
	if (all || zone2Of(t) != zone2Of(a)) {
		core_.setZone2(zone2Of(t));
	}
	if (all || zone3Of(t) != zone3Of(a)) {
		core_.setZone3(zone3Of(t));
	}
	if (all || t.character != a.character) {
		core_.setCharacter(characterBankOf(t.character));
	}
	if (all || t.predelay != a.predelay) {
		core_.setPredelay(t.predelay);
	}
	// NO DRY−. The firmware's DRY− (subtract the dry from the return, +3 dB into the send) is a send's
	// tool; on an insert it is what Mix already does (redundant with mix knob). The core
	// keeps the setter, at the firmware's default, off.
	if (all) {
		core_.setReturnLevels(kUnpannedLevel, kUnpannedLevel);
	}
	applied_ = t;
}

void Featherverb::processStereo(float* left, float* right, int frames) noexcept {
	if (left == nullptr || right == nullptr || frames <= 0 || buffer_.empty()) {
		return;
	}
	if (!target_.enabled) {
		// OFF: nothing goes in and nothing comes out, and the meter says so rather than holding its last bars.
		for (int k = 0; k < frames; ++k) {
			inPeak_.push(0.0f);
			wetPeak_.push(0.0f);
			limiterGr_.push(0.0f);
		}
		publish();
		return;
	}

	constexpr int kChunk = 256;
	static_assert(kChunk <= static_cast<int>(kOversampleMaxFrames), "the resamplers take a chunk at a time");
	int32_t in[kChunk];
	int32_t inj[kChunk];
	int32_t outL[kChunk];
	int32_t outR[kChunk];
	constexpr float kFromFirmware = static_cast<float>(kReturnGain / kFirmwareFullScale);
	// (The reference buffer's length is read INSIDE the loop, after a pending rebuild: see below.)
	const float mixTarget = target_.mix;
	// INPUT (backlog LA): dB to a linear gain on the send, ramped (`dyn::LinearRamp`; its first `set` after a reset
	// places it).
	{
		const float db = std::clamp(std::isfinite(target_.inputGainDb) ? target_.inputGainDb : 0.0f,
		                            static_cast<float>(kInputGainMinDb), static_cast<float>(kInputGainMaxDb));
		inputGain_.set(std::pow(10.0f, db / 20.0f));
	}
	float inGain[kChunk];
	float limiterGain[kChunk];

	// THE SEND: the mono mid, at the reference's scale.
	const auto send = [](float l, float r, float gain = 1.0f) {
		return toQ31(0.5 * (static_cast<double>(l) + static_cast<double>(r)) * kFirmwareFullScale * kSendGain * gain);
	};
	// DRY AGAINST THE WET, over `n` frames.
	const auto mixOut = [&](float* l, float* r, const float* wetL, const float* wetR, int n) {
		for (int k = 0; k < n; ++k) {
			glideTo(mixNow_, mixTarget, mixCoeff_);
			const float dryL = l[k];
			const float dryR = r[k];
			// NO CEILING HERE. There was a 0 dBFS clamp (asked for as a safety), and it was the clipping
			// heard: "its not high frequency artifacts, its plain old clipping". A float chain carries a
			// pluck past 1.0 legitimately, the wet follows it, and the clamp squared it off, where the reference,
			// with its headroom, never does. The core keeps the reference's own ceiling (16x its full scale).
			// RIGHT BEFORE LEFT, so the mono road (both pointers the same) keeps the left channel.
			r[k] = dryR * (1.0f - mixNow_) + wetR[k] * mixNow_;
			l[k] = dryL * (1.0f - mixNow_) + wetL[k] * mixNow_;
		}
	};

	// THE CONVOLUTION'S ROUTING (backlog JD): the three gains' targets for this call.
	const ConvGains want = convGainsOf(target_);
	// PLACED AT THE FIRST CALL AFTER A RESET, not at the reverb's rebuild: in Conv Only the reverb never
	// runs, and MEASURED, a Mix placed there started at its default and glided (the output came out dry).
	if (convPlace_) {
		mixNow_ = target_.mix;
		convGains_ = want;
		sendNow_ = want.send;
		// The convolver's own predelay PLACED, not glided up from zero; its feedback starts from none.
		{
			const float p = std::clamp(std::isfinite(target_.convPredelay) ? target_.convPredelay : 0.0f, 0.0f, 1.0f);
			convPredelayNow_ = static_cast<float>(kConvPredelayMaxSeconds * sampleRate_) * p * p;
			convFeedbackNow_ = 0.0f;
		}
		// AND THE CONVOLUTION'S OWN OUTPUT PLACED: fully in, or — in Off — already gone, so a device switched
		// on in Off never runs a sample of it.
		convDead_ = target_.convMode == kConvOff;
		convOut_ = convDead_ ? 0.0f : 1.0f;
		// OWL'S TRIM PLACED, not glided in from 1.
		owlTrim_ = owlTrimOn_ && target_.sizeMode == kOwlMenu ? std::pow(10.0f, owlDecayTrimDb(target_.decay) / 20.0f) : 1.0f;
		convPlace_ = false;
	}
	// **CONV ONLY SKIPS THE REVERB**, once its fade-out has finished: its state freezes, as the gate
	// freezes it, and costs nothing.
	const bool reverbWanted = !(want.reverb == 0.0f && want.series == 0.0f && convGains_.reverb == 0.0f
	                            && convGains_.series == 0.0f);
	float revL[kChunk];
	float revR[kChunk];
	float convIn[kChunk];
	float convL[kChunk];
	float convR[kChunk];
	float revGain[kChunk];
	float convHeard[kChunk];
	float convExcite[kChunk];
	float wetL[kChunk];
	float wetR[kChunk];

	// **THE EXCITER'S ROAD INTO THE REVERB** (backlog JD): the convolution, run after the reverb in each chunk,
	// writes its injected share into `exciteRing_` at the running sample count; the reverb reads it
	// `kExciteLagFrames` behind, which is always a sample already written (no chunk is longer). At the send's
	// scale, so the room drives the reverb as the dry input would.
	const size_t exciteMask = exciteRing_.size() - 1;
	const auto exciteAt = [&](int k) -> int32_t {
		const float v = exciteRing_[(exciteT_ + static_cast<uint64_t>(k) - kExciteLagFrames) & exciteMask];
		return v == 0.0f ? 0 : toQ31(static_cast<double>(v) * kFirmwareFullScale * kSendGain);
	};

	int done = 0;
	while (done < frames) {
		float* l = left + done;
		float* r = right + done;
		int n = 0;
		// INPUT'S GAIN for every frame this chunk may take, from a copy of the ramp: the chunk's length is decided
		// below (a gate, a buffer boundary), and the ramp itself moves on by exactly the frames it took, at the end.
		{
			dyn::LinearRamp ahead = inputGain_;
			const int span = std::min(frames - done, kChunk);
			for (int k = 0; k < span; ++k) {
				inGain[k] = ahead.next();
			}
		}
		bool haveReverb = false;
		bool startAfter = false;

		// ---- THE REVERB ------------------------------------------------------------------------ //
		if (!reverbWanted) {
			running_ = false;
			n = std::min(frames - done, kChunk);
		}
		else if (!running_) {
			// **THE GATE IS SHUT** (see `running_`): the reverb is not running and its state is frozen, as
			// the reference's is. Frames pass with no reverb until the first one that sends anything, and A
			// BUFFER STARTS THERE — a point fixed by the audio, not by where the host cut its blocks.
			const int span = std::min(frames - done, kChunk);
			int first = span;
			for (int k = 0; k < span; ++k) {
				if (send(l[k], r[k], inGain[k]) != 0 || exciteAt(k) != 0) { // the room, injected, opens it too
					first = k;
					break;
				}
			}
			if (first == 0) {
				running_ = true;
				quietFrames_ = 0;
				gridPos_ = 0;
				// A RATE CHANGE WHILE SHUT needs no fade: the tail had already fallen past the gate.
				if ((target_.oversample ? 2 : 1) != coreOs_) {
					rebuildPending_ = true;
				}
				continue;
			}
			n = first;
			startAfter = first < span;
		}
		else {
			// A BUFFER BOUNDARY: the only place a setting may reach the core, and where its per-buffer
			// dynamics step.
			if (gridPos_ == 0) {
				// A TURN OF OVERSAMPLE rebuilds here once the reverb has faded out (`osFade_`, below).
				if (rebuildPending_ || ((target_.oversample ? 2 : 1) != coreOs_ && osFade_ <= 0.0f)) {
					rebuild();
				}
				else {
					applyChanges(false);
				}
				core_.beginBuffer(static_cast<size_t>(core_.bufferFrames()));
				sumAbsL_ = 0.0;
				sumAbsR_ = 0.0;
			}
			// **READ HERE, AFTER THE REBUILD, NEVER CACHED ABOVE THE LOOP.** A lazy rebuild (after `prepare`
			// at a new rate) swaps in a core with a different buffer length; a length cached at the top of
			// the call ended a buffer at the OLD length, left `gridPos_` past the NEW one, and the next call
			// computed a NEGATIVE `take` — `run` then read and wrote past the stack arrays. Found under ASan,
			// 2026-09-24, by a lane-lifecycle stress with a rate change downwards.
			const int bufferFrames = core_.bufferFrames() / coreOs_; // in HOST frames (the core's are twice, oversampled)
			n = std::min({ frames - done, bufferFrames - gridPos_, kChunk });
			for (int k = 0; k < n; ++k) {
				glideTo(sendNow_, want.send, convCoeff_);
				in[k] = send(l[k], r[k], sendNow_ * inGain[k]);
				inj[k] = exciteAt(k);
				quietFrames_ = in[k] != 0 || inj[k] != 0 ? 0 : quietFrames_ + 1;
				outL[k] = 0;
				outR[k] = 0;
			}
			// THE EXCITER'S INJECTION only while the convolution lives: a dead one passes nothing, and the core is
			// then the firmware to the bit.
			if (target_.sizeMode == kAuroraMenu) {
				// AURORA, IN THE CORE'S PLACE: the same int32 road, send, return and injection, so the gate, the
				// Mix and the convolution work on it unchanged. Its controls are the device's, read each chunk.
				Aurora::Params ap;
				ap.size = target_.size;
				ap.decay = target_.decay;
				ap.shift = target_.matrix; // rising, a quarter hertz to twelve (`Aurora::Params::shift`)
				ap.diffusion = target_.room;
				ap.damping = target_.damping;
				ap.width = target_.width;
				ap.hpfHz = std::exp2(target_.hpfLog2Hz);
				ap.lpfHz = std::exp2(target_.lpfLog2Hz);
				ap.predelay = target_.predelay;
				ap.character = characterBankOf(target_.character);
				aurora_.set(ap);
				aurora_.run(in, outL, outR, static_cast<size_t>(n), convDead_ ? nullptr : inj);
			}
			else if (coreOs_ == 2) {
				// **OVERSAMPLED** (backlog JE): the send and the injection up to twice the rate, the core there,
				// its return back down (`featherverb_oversample.h`).
				int32_t in2[2 * kChunk];
				int32_t inj2[2 * kChunk];
				int32_t outL2[2 * kChunk];
				int32_t outR2[2 * kChunk];
				const auto m = static_cast<size_t>(n);
				upIn_.run(in, in2, m);
				if (!convDead_) {
					upInj_.run(inj, inj2, m);
				}
				std::fill_n(outL2, 2 * m, 0);
				std::fill_n(outR2, 2 * m, 0);
				core_.run(in2, outL2, outR2, 2 * m, convDead_ ? nullptr : inj2);
				downL_.run(outL2, outL, m);
				downR_.run(outR2, outR, m);
			}
			else {
				core_.run(in, outL, outR, static_cast<size_t>(n), convDead_ ? nullptr : inj);
			}
			for (int k = 0; k < n; ++k) {
				sumAbsL_ += std::fabs(static_cast<double>(outL[k]));
				sumAbsR_ += std::fabs(static_cast<double>(outR[k]));
			}
			haveReverb = true;
		}
		for (int k = 0; k < n; ++k) {
			revL[k] = haveReverb ? static_cast<float>(outL[k]) * kFromFirmware : 0.0f;
			revR[k] = haveReverb ? static_cast<float>(outR[k]) * kFromFirmware : 0.0f;
		}
		// A TURN OF OVERSAMPLE, FADED: down to nothing while the running core is at the wrong rate (the rebuild above
		// waits for it), and back up if the switch returns first. At rest the gain is exactly 1 and this is a no-op.
		if (haveReverb && (osFade_ < 1.0f || (target_.oversample ? 2 : 1) != coreOs_)) {
			const bool out = (target_.oversample ? 2 : 1) != coreOs_;
			for (int k = 0; k < n; ++k) {
				osFade_ = out ? std::max(0.0f, osFade_ - osFadeStep_) : std::min(1.0f, osFade_ + osFadeStep_);
				revL[k] *= osFade_;
				revR[k] *= osFade_;
			}
		}
		// OWL'S TRIM ALONG DECAY (`owlDecayTrimDb`): a pure level on its return, gliding when Decay moves. 1 elsewhere.
		if (haveReverb) {
			const float want = owlTrimOn_ && target_.sizeMode == kOwlMenu ? std::pow(10.0f, owlDecayTrimDb(target_.decay) / 20.0f) : 1.0f;
			if (owlTrim_ != 1.0f || want != 1.0f) {
				for (int k = 0; k < n; ++k) {
					glideTo(owlTrim_, want, convCoeff_);
					revL[k] *= owlTrim_;
					revR[k] *= owlTrim_;
				}
			}
		}

		// ---- THE CONVOLUTION ------------------------------------------------------------------- //
		// ONE CONVOLVER, FED A MIX: `input · gC + reverb · gS`. Convolution is linear, so Parallel's
		// conv(input) and Series' conv(reverb) are one engine, and a change of mode is a glide of three
		// gains rather than a switch between paths — which is what keeps it from clicking.
		bool anyIn = false;
		for (int k = 0; k < n; ++k) {
			convGains_.glide(want, convCoeff_);
			const float mid = 0.5f * (l[k] + r[k]) * inGain[k];
			const float revMid = 0.5f * (revL[k] + revR[k]);
			convIn[k] = convGains_.input * mid + convGains_.series * revMid;
			revGain[k] = convGains_.reverb;
			convHeard[k] = convGains_.audible;
			convExcite[k] = convGains_.excite;
			convL[k] = 0.0f;
			convR[k] = 0.0f;
		}
		// **OFF, AND FADED: THE CONVOLUTION DOES NOT EXIST.** No ring, no convolver, no loop — only a waiting
		// IR is taken, which costs a pointer. verify it kills all tails and really bypasses the conv.
		const bool convOn = target_.convMode != kConvOff;
		if (convDead_ && !convOn) {
			swapBegin(); // convL/convR stay the zeros the loop above wrote
		}
		else {
		convDead_ = false;
		// ---- THE CONVOLVER'S OWN PREDELAY AND FEEDBACK (convolver specific predelay, feedback) -- //
		// The input waits `convPredelay` in a ring; the convolution's mid comes back round through a second
		// ring, as far behind as the predelay but never less than a chunk (so the loop reads only output
		// already made), scaled by the IR's own peak gain so 100% is the edge of ringing forever, through a
		// soft saturation. At zero predelay and zero feedback this is the input, exactly.
		const size_t chunkStart = ringPos_;
		{
			const float p = std::clamp(std::isfinite(target_.convPredelay) ? target_.convPredelay : 0.0f, 0.0f, 1.0f);
			const float pdTarget = static_cast<float>(kConvPredelayMaxSeconds * sampleRate_) * p * p;
			const float fb = std::clamp(std::isfinite(target_.convFeedback) ? target_.convFeedback : 0.0f, 0.0f, 1.0f);

			// SCALED BY THE IR'S AVERAGE GAIN, NOT ITS PEAK (conv feedback doesnt seem to do much). MEASURED: every
			// generated room's peak is ~7.5x (17.5 dB) its average, so a loop held under the peak came back 18 dB down per
			// lap at 100% — inaudible. Now the average lap gain is `kConvFeedbackMax · fb^1.5`: the low half repeats and
			// decays; above ~40% the loudest frequencies pass unity and bloom, bounded by the loop's soft saturation and
			// smeared by the loop's slow wobble so they do not settle on one pitch.
			const float rms = runningKernel_ != nullptr ? runningKernel_->rmsGain() : 0.0f;
			const float fbTarget = rms > 1.0e-9f ? static_cast<float>(kConvFeedbackMax) * fb * std::sqrt(fb) / rms : 0.0f;
			const size_t mask = preRing_.size() - 1;
			const auto readRing = [mask](const std::vector<float>& ring, size_t pos, float delay) {
				const float at = static_cast<float>(pos) - delay + static_cast<float>(ring.size());
				const auto i0 = static_cast<size_t>(at);
				const float frac = at - static_cast<float>(i0);
				const float a = ring[i0 & mask];
				return frac == 0.0f ? a : a + frac * (ring[(i0 + 1) & mask] - a);
			};
			for (int k = 0; k < n; ++k) {
				convPredelayNow_ += convPredelayCoeff_ * (pdTarget - convPredelayNow_);
				glideTo(convFeedbackNow_, fbTarget, convCoeff_);
				preRing_[ringPos_] = convIn[k];
				float u = convPredelayNow_ < 1.0e-6f ? convIn[k] : readRing(preRing_, ringPos_, convPredelayNow_);
				if (convFeedbackNow_ != 0.0f) {
					// THE LOOP'S SLOW WOBBLE: ±0.4% of its length at ~0.2 Hz, so a bloom choruses rather than drones.
					const float wob = 1.0f + 0.004f * static_cast<float>(std::sin(6.283185307179586 * loopWobble_));
					loopWobble_ += 0.2 / sampleRate_;
					loopWobble_ -= std::floor(loopWobble_);
					const float loop = std::max(convPredelayNow_, static_cast<float>(kChunk)) * wob;
					u += kConvLoopCeiling * std::tanh(convFeedbackNow_ * readRing(fbRing_, ringPos_, loop) / kConvLoopCeiling);
				}
				convIn[k] = u;
				anyIn = anyIn || u != 0.0f;
				ringPos_ = (ringPos_ + 1) & mask;
			}
		}
		// **OFF COSTS NOTHING, AND NEITHER DOES SILENCE.** Once the convolver's input has been zero for
		// longer than the IR (plus a block), everything it holds is zero and running it would add zeros:
		// it is skipped, which is exactly a reset, so resuming is exact.
		// A NEW IR, if one waits (`loadIr`): taken here, between chunks, under a dip. BEFORE this chunk's
		// input marks the convolver fed: whether there is anything to dip is a fact about what it HOLDS.
		swapBegin();
		if (anyIn) {
			convQuiet_ = 0;
		}
		if (anyIn || convQuiet_ < conv_.irFrames() + conv::Convolver::kBlock) {
			conv_.process(convIn, convL, convR, static_cast<size_t>(n));
			if (!anyIn) {
				convQuiet_ += static_cast<uint64_t>(n);
			}
		}
		swapRamp(convL, convR, n);
		// THE LOOP'S RETURN: this chunk's convolution, where the feedback ring will read it a chunk or more on. **FLOORED AT
		// -140 dBFS (backlog MC)**, as the exciter's road below is: a loop under unity decays geometrically and never
		// ARRIVES at zero, and MEASURED on the factory preset machine_elf_book_of_dreams it carried 1e-14 down to 1e-37 for
		// ninety seconds after the output fell exactly silent, every lap re-feeding the convolver, so `holdsTail` kept a
		// host awake for it. Only the loop's road is floored; the convolution that is heard is not.
		constexpr float kLoopFloor = 1.0e-7f;
		const auto floored = [](float v) { return std::fabs(v) < kLoopFloor ? 0.0f : v; };
		for (int k = 0; k < n; ++k) {
			fbRing_[(chunkStart + static_cast<size_t>(k)) & (fbRing_.size() - 1)] = floored(0.5f * (convL[k] + convR[k]));
		}
		// THE OUTPUT'S OWN FADE, after the loop's return (so the loop goes on hearing itself while it fades,
		// and is then cleared whole rather than cut mid-lap). Linear, `kConvGainSeconds` end to end.
		for (int k = 0; k < n; ++k) {
			convOut_ = convOn ? std::min(1.0f, convOut_ + convOutStep_) : std::max(0.0f, convOut_ - convOutStep_);
			convL[k] *= convOut_;
			convR[k] *= convOut_;
		}
		// THE EXCITER'S SHARE, into the ring the reverb reads a chunk from now (post-fade, so Off takes it too).
		for (int k = 0; k < n; ++k) {
			exciteRing_[(exciteT_ + static_cast<uint64_t>(k)) & exciteMask] = floored(convExcite[k] * 0.5f * (convL[k] + convR[k]));
		}
		if (!convOn && convOut_ == 0.0f) {
			killConv();
		}
		}
		for (int k = 0; k < n; ++k) {
			wetL[k] = revGain[k] * revL[k] + convHeard[k] * convL[k];
			wetR[k] = revGain[k] * revR[k] + convHeard[k] * convR[k];
		}
		// THE WET'S SAFETY CEILING (`kWetCeiling`, `dsp/lookahead_limiter.h`): a stereo-linked LOOKAHEAD limiter, the wet
		// delayed by `kWetLookaheadSeconds` and the gain ramped across it, so no sample passes the ceiling and none
		// is squared off on the way (backlog LD). Below the ceiling it is a pure delay. The delay runs whether or not
		// the limiter does, so a test hearing the wet unlimited hears it on the same timing.
		limiter_.process(wetL, wetR, n, wetLimiter_, limiterGain);

		// ---- THE METER (backlog LA): what went in, after Input; the wet; what the limiter took ---------- //
		// Before the mix writes over `l` and `r`. The ramp moves on here by the frames this chunk took.
		for (int k = 0; k < n; ++k) {
			inputGain_.next();
			inPeak_.push(std::fabs(0.5f * (l[k] + r[k])) * inGain[k]);
			wetPeak_.push(std::max(std::fabs(wetL[k]), std::fabs(wetR[k])));
			limiterGr_.push(limiterGain[k] < 1.0f ? -20.0f * std::log10(std::max(limiterGain[k], 1.0e-6f)) : 0.0f);
		}

		// ---- THE MIX --------------------------------------------------------------------------- //
		mixOut(l, r, wetL, wetR, n);
		done += n;
		exciteT_ += static_cast<uint64_t>(n);

		if (haveReverb) {
			gridPos_ += n;
			const int bufferFrames = core_.bufferFrames() / coreOs_;
			if (gridPos_ == bufferFrames) {
				core_.endBuffer(static_cast<size_t>(core_.bufferFrames())); // the core's own frames
				gridPos_ = 0;
				// THE REFERENCE'S GATE, once per buffer as it evaluates it: off when nothing has been sent for
				// 12 s AND the level has fallen to e^9 (`audio_engine.cpp`: `reverbOn`).
				const double meanAbs = std::max(sumAbsL_, sumAbsR_) / static_cast<double>(bufferFrames);
				if (quietFrames_ >= gateFrames_ && !(std::log(meanAbs + 1e-24) > kGateLogLevel)) {
					running_ = false;
				}
			}
		}
		if (startAfter) {
			running_ = true;
			quietFrames_ = 0;
			gridPos_ = 0;
		}
	}
	publish();
}

bool Featherverb::holdsTail() const noexcept {
	// OFF, nothing goes in or comes out; at Mix 0 held at 0, the wet is not heard (Owl at Mix 0 would otherwise keep a
	// host awake for ever on a tail nobody hears).
	if (!target_.enabled || buffer_.empty() || (target_.mix == 0.0f && mixNow_ == 0.0f)) {
		return false;
	}
	// THE REVERB: the reference's gate is the device's own "still holding" — it opens on the first sample sent and
	// shuts only 12 s after the last one, once the level is under e^9, so it covers the predelay's gap.
	if (running_) {
		return true;
	}
	if (convDead_) {
		return false; // Off and faded: `killConv` cleared the convolver and every ring
	}
	// THE CONVOLUTION: its own quiet count (from its predelayed input), to where `processStereo` stops running it...
	if (convQuiet_ < conv_.irFrames() + conv::Convolver::kBlock) {
		return true;
	}
	// ...and what that count cannot see: the wet's lookahead (1.5 ms behind the convolution), the predelay (up to
	// 0.5 s), the loop's return, and the exciter's road into the reverb. Scanned exactly; asked only after an exactly
	// silent block.
	const auto holds = [](const std::vector<float>& ring) {
		for (const float v : ring) {
			if (v != 0.0f) {
				return true;
			}
		}
		return false;
	};
	return limiter_.holdsSound() || holds(preRing_) || holds(fbRing_) || holds(exciteRing_);
}

void Featherverb::processMono(float* mono, int frames) noexcept {
	if (mono == nullptr || frames <= 0) {
		return;
	}
	processStereo(mono, mono, frames);
}

//--------------------------------------------------------------------------------------------------//
// THE OPS TABLE.                                                                                     //
//--------------------------------------------------------------------------------------------------//

namespace {
Featherverb& asVerb(void* storage) { return *static_cast<Featherverb*>(storage); }
} // namespace

const device::AudioOps kAudioOps {
    .stateBytes = sizeof(Featherverb),
    .stateAlign = alignof(Featherverb),
    .construct = [](void* storage) { new (storage) Featherverb(); },
    .destruct = [](void* storage) { asVerb(storage).~Featherverb(); },
    .prepare = [](void* storage, double sampleRate, int) { asVerb(storage).prepare(sampleRate); },
    .reset = [](void* storage) { asVerb(storage).reset(); },
    .setFields =
        [](void* storage, const float* fields, size_t fieldsPresent) {
	        Row row {};
	        const size_t take = std::min(fieldsPresent, row.size());
	        for (size_t i = 0; i < take; ++i) {
		        row[i] = fields[i];
	        }
	        asVerb(storage).setTarget(stateOf(row, take).settings);
        },
    .processStereo =
        [](void* storage, float* left, float* right, int frames) {
	        asVerb(storage).processStereo(left, right, frames);
        },
    .processMono = [](void* storage, float* mono, int frames) { asVerb(storage).processMono(mono, frames); },
    .setReadingsSink = [](void* storage, device::ReadingsSink sink) { asVerb(storage).setReadingsSink(sink); },
    .holdsTail = [](const void* storage) { return static_cast<const Featherverb*>(storage)->holdsTail(); },
};

const device::BodyOps kBodyOps {
    .saveState =
        [](const void* storage, const device::ByteSink& sink) {
	        std::vector<uint8_t> bytes;
	        static_cast<const Featherverb*>(storage)->saveBody(bytes);
	        sink(bytes.data(), bytes.size());
	        return true;
        },
    .loadState = [](void* storage, const uint8_t* data, size_t bytes) { return asVerb(storage).loadBody(data, bytes); },
    .maxBodyBytes = conv::kIrRefMaxBytes,
};

} // namespace feathers::fx::featherverb

namespace feathers::fx::featherverb {

ConvGains convGainsOf(const Settings& s) noexcept {
	const float b = std::clamp(std::isfinite(s.convBlend) ? s.convBlend : 0.5f, 0.0f, 1.0f);
	switch (s.convMode) {
	case kConvParallel: return {1.0f - b, b, 0.0f, 0.0f, 1.0f, 1.0f};
	case kConvSeries: return {1.0f - b, 0.0f, b, 0.0f, 1.0f, 1.0f};
	case kConvOnly: return {0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f};
	case kConvExciter: return {1.0f, 1.0f, 0.0f, b, 0.0f, 1.0f - b};
	default: return {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f};
	}
}

} // namespace feathers::fx::featherverb
