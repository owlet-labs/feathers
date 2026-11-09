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

#pragma once

#include "fx/conv/convolver.h"
#include "fx/conv/ir_file.h"
#include "fx/conv/ir_ref.h"
#include "fx/conv/test_ir.h"
#include "dsp/lookahead_limiter.h"
#include "device/registry.h"
#include "fx/dyn/meter_ramp.h"
#include "mod/mseg/mseg_types.h"
#include "fx/featherverb/featherverb_aurora.h"
#include "fx/featherverb/featherverb_core.h"
#include "fx/featherverb/featherverb_oversample.h"
#include "fx/featherverb/featherverb_types.h"

#include <cstddef>
#include <cmath>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace feathers::fx::featherverb {

/// **A ONE-POLE GLIDE THAT ARRIVES.** `g += coeff · (t − g)` alone STALLS short of its target in float: once the
/// step is smaller than half an ulp of `g`, adding it changes nothing. MEASURED 2026-09-24 with the 20 ms
/// routing coefficient (~0.001): the gains stuck about 3e-5 from home, inside no 1e-6 snap — so after Conv had
/// been on, Off left the convolver's input gain at ~3e-5, its input never went exactly silent, and the
/// convolver ran at full cost forever with Conv on Off (plugin has become very cpu hungry even with
/// con apparently switched off). So it snaps when a step no longer moves it, as well as when it is close.
inline void glideTo(float& g, float t, float coeff) noexcept {
	const float next = g + coeff * (t - g);
	g = (next == g || std::fabs(t - next) < 1.0e-6f) ? t : next;
}

/// **THE CONVOLUTION'S THREE ROUTING GAINS** (backlog JD): the reverb's share of the wet, and how much of
/// the input and of the reverb the one convolver is fed. A mode is a set of targets (`convGainsOf`); the
/// gains glide to them over `kConvGainSeconds`, so a change of mode never switches a path.
struct ConvGains {
	float reverb = 1.0f;  ///< the reverb's share of the wet
	float input = 0.0f;   ///< the convolver fed the input
	float series = 0.0f;  ///< the convolver fed the reverb
	float excite = 0.0f;  ///< the convolution INJECTED into the reverb's loop (Exciter)
	float audible = 1.0f; ///< the convolution heard in the wet (not in Exciter)
	float send = 1.0f;    ///< the reverb's own dry send (Exciter trades it for the room)
	void glide(const ConvGains& to, float coeff) noexcept {
		const auto step = [coeff](float& g, float t) { glideTo(g, t, coeff); };
		step(reverb, to.reverb);
		step(input, to.input);
		step(series, to.series);
		step(excite, to.excite);
		step(audible, to.audible);
		step(send, to.send);
	}
};
/// Off: the reverb. Parallel: the reverb against conv(input). Series: against conv(reverb). Conv Only:
/// conv(input). Exciter: the reverb, driven by the dry send against conv(input) injected into its loop.
[[nodiscard]] ConvGains convGainsOf(const Settings& s) noexcept;

/// **THE DEVICE AROUND THE CORE.** Three jobs, each one the reference does somewhere else:
/// 1. **THE BUFFER GRID.** The core steps its LFO, its Owl servo and its envelopes once per hardware
///    buffer. Here a buffer is a fixed `Core::bufferFrames()` (128 at 44.1 kHz), anchored to the
///    stream and carried across host calls (`beginBuffer` / `run` / `endBuffer`). So the host's block
///    size never changes the sound.
/// 2. **THE SETTINGS, IN THE REFERENCE'S ORDER, ONLY WHEN THEY CHANGE, ONLY BETWEEN BUFFERS.** The
///    setters are order-dependent (plan, trap 2): a changed control calls its own setter, as turning
///    that knob on the hardware does, and several at once go in `getReverbParamsFromSong`'s order.
/// 3. **THE SEND AND THE RETURN** at the reference's levels (`featherverb_types.h`, the level contract),
///    and `Mix`.
class Featherverb {
public:
	Featherverb() = default;
	~Featherverb();
	Featherverb(const Featherverb&) = delete;
	Featherverb& operator=(const Featherverb&) = delete;

	/// ALLOCATE: the core's one buffer at this rate, and the convolution's IR made at it (the file's,
	/// read again, or the generated room). Activate-time only.
	void prepare(double sampleRate);

	//----------------------------------------------------------------------------------------------//
	// THE IMPULSE RESPONSE (backlog JD). MESSAGE THREAD, every one of these.                         //
	//----------------------------------------------------------------------------------------------//

	/// **READ AN IR FROM DISK AND HAND IT TO THE RUNNING CONVOLVER.** The file is decoded, resampled and
	/// partitioned here; the audio thread takes the finished kernel by one pointer at its next chunk,
	/// dipping the convolution for `kConvSwapSeconds` around the swap so it does not click, and the
	/// reverb runs on untouched. False (the old IR kept, `irError` saying why) for a file it cannot use.
	/// The path is remembered either way only on success.
	bool loadIr(const std::string& path);
	/// Back to the generated room (`conv::generateTestIr`).
	void useGeneratedIr(int which = conv::kGeneratedRoom);
	/// Which generated room runs while `irPath` is empty (`conv::generateRoom`).
	[[nodiscard]] int generatedRoom() const noexcept { return generated_; }
	/// The file the IR came from; empty for the generated room.
	[[nodiscard]] const std::string& irPath() const noexcept { return irPath_; }
	/// Why the last load failed, or empty. A patch naming a file that has gone keeps its path (so saving
	/// it again does not lose it) and runs no IR, and this says so.
	[[nodiscard]] const std::string& irError() const noexcept { return irError_; }
	/// Whether the span between the markers was longer than `kConvMaxIrSeconds` (or the file longer than
	/// `conv::kIrMaxFileSeconds`) and was cut.
	[[nodiscard]] bool irTruncated() const noexcept { return irTruncated_; }

	/// **THE IR'S SHAPE** (`conv::IrShape`): the markers, the stretch and the envelope — every control that
	/// rebuilds the kernel rather than moving a parameter. A change re-cuts the source held in memory and
	/// hands the new kernel over as `loadIr` does, under the same dip. NOT PARAMETERS: each rebuilds the
	/// spectra, so none can be modulated at audio rate, and all are saved in the body beside the filename.
	/// False (nothing changed) for a shape a patch's body could not hold, or a silent span. The generated
	/// room is shaped exactly as a file is.
	bool setIrShape(const conv::IrShape& shape);
	[[nodiscard]] const conv::IrShape& irShape() const noexcept { return shape_; }
	/// **THE MARKERS** (in the impulse view, choosing the begin and end points at which to truncate),
	/// as fractions of the source.
	bool setIrTrim(conv::IrTrim trim);
	[[nodiscard]] conv::IrTrim irTrim() const noexcept { return { shape_.begin, shape_.end }; }
	/// **THE ENVELOPE, AS THE MSEG'S OWN SHAPE** (an mseg style env which draws over the active part
	/// of the impulse--reusing parts of our mseg code), so the MSEG's geometry edits it: only the start
	/// level and the segments are read; sustain, loop and window mean nothing here.
	bool setIrEnvelope(const mod::mseg::Settings& envelope);
	[[nodiscard]] mod::mseg::Settings irEnvelope() const { return envelopeSettingsOf(shape_); }
	/// The envelope half of a shape as the MSEG's settings, and back — for a view previewing a gesture.
	static mod::mseg::Settings envelopeSettingsOf(const conv::IrShape& shape);
	static conv::IrShape shapeOfEnvelope(conv::IrShape shape, const mod::mseg::Settings& envelope);
	/// The whole source at this rate (the file, or the generated room), for drawing it.
	[[nodiscard]] const conv::LoadedIr& irWhole() const noexcept { return whole_; }
	/// **WHICH SOURCE `irWhole` IS**, as a serial no other source in any instance has had — so a view caching
	/// what it drew from the source keys on this, never on the vector's address (CLAUDE.md: an address is
	/// only an identity while the thing at it cannot be replaced). Moves when a file loads, the room is
	/// made again, or a rate change reads the source afresh; the shape moving does not move it.
	[[nodiscard]] uint64_t sourceSerial() const noexcept { return sourceSerial_; }

	/// THE BODY (`device::BodyOps`, format `conv::IrRef`): which IR, as a filename.
	void saveBody(std::vector<uint8_t>& out) const;
	bool loadBody(const uint8_t* data, size_t bytes);

	/// AS IF JUST SWITCHED ON: a fresh core, the current settings applied in the song-load order, the
	/// grid back to the start of a buffer, and Mix placed where it is asked to be.
	void reset() noexcept;

	void setTarget(const Settings& settings) noexcept { target_ = settings; }
	[[nodiscard]] const Settings& target() const noexcept { return target_; }

	void processStereo(float* left, float* right, int frames) noexcept;
	/// DECLARED: the stereo road's LEFT channel, for which `processStereo` writes right before left.
	void processMono(float* mono, int frames) noexcept;

	//----------------------------------------------------------------------------------------------//
	// TEST SEAMS                                                                                     //
	//----------------------------------------------------------------------------------------------//
	[[nodiscard]] const Core& coreForTest() const noexcept { return core_; }
	/// OWL PROBES (`Core::OwlProbe`), kept across the core's rebuilds.
	/// THE FIRMWARE'S OWL, for a case that measures against it (the device always runs the compressor).
	/// THE FIRMWARE'S STEPPED MODULATION, for a case that measures against it (the device ramps it).
	void setSmoothModulationForTest(bool on) noexcept {
		smoothModulation_ = on;
		core_.setSmoothModulation(on);
	}
	void setOwlCompressorForTest(bool on) noexcept {
		owlCompressor_ = on;
		core_.setOwlCompressor(on);
	}
	void setOwlProbeForTest(const Core::OwlProbe& p) noexcept {
		owlProbe_ = p;
		core_.setOwlProbeForTest(p);
	}
	[[nodiscard]] const Aurora& auroraForTest() const noexcept { return aurora_; }
	/// WHAT THE CORE IS RUNNING ON — the settings last pushed into it, as opposed to the target.
	[[nodiscard]] const Settings& appliedForTest() const noexcept { return applied_; }
	[[nodiscard]] int gridPositionForTest() const noexcept { return gridPos_; }
	[[nodiscard]] double sampleRateForTest() const noexcept { return sampleRate_; }
	[[nodiscard]] bool runningForTest() const noexcept { return running_; }
	/// **WHETHER THE REVERB OR THE CONVOLUTION STILL HOLDS SOUND TO COME** — `AudioOps::holdsTail` (backlog MC): a
	/// predelay (50 ms in the reverb, 0.5 s in the convolver) and the convolver's loop each put exact zeroes out while
	/// they hold a sample.
	[[nodiscard]] bool holdsTail() const noexcept;
	/// THE RUNNING CORE'S FACTOR (1, or 2 when oversampled), and the fade that carries a change of it.
	[[nodiscard]] int coreOversampleForTest() const noexcept { return coreOs_; }
	[[nodiscard]] float oversampleFadeForTest() const noexcept { return osFade_; }
	/// THE OWL TRIM'S GAIN NOW.
	[[nodiscard]] float owlTrimForTest() const noexcept { return owlTrim_; }
	/// THE WET LIMITER (`dsp/lookahead_limiter.h`): its gain on the last sample out, and its lookahead in samples —
	/// the delay the wet carries against the dry, at every setting, whether or not the limiter is acting.
	[[nodiscard]] float limiterGainForTest() const noexcept { return limiter_.gainForTest(0); }
	[[nodiscard]] const dsp::LookaheadLimiter& wetLimiterForTest() const noexcept { return limiter_; }
	/// WITHOUT THE WET CEILING, for a case that measures what reaches it (the device always runs it). The wet's
	/// lookahead DELAY stays: only the gain is lifted.
	void setWetLimiterForTest(bool on) noexcept { wetLimiter_ = on; }
	/// THE WET LIMITER'S LEVEL EVENTS since construction, in frames (backlog LD): `limited`, any gain under 1;
	/// `attack`, the envelope jumping up to a new held peak.
	using WetLimiterEvents = dsp::LookaheadLimiter::Events;
	[[nodiscard]] const WetLimiterEvents& wetLimiterEventsForTest() const noexcept { return limiter_.eventsForTest(); }
	/// WITHOUT OWL'S TRIM, for the case that fits it.
	void setOwlTrimForTest(bool on) noexcept { owlTrimOn_ = on; }
	/// THE HOST'S EAR FOR THE METER (`device::AudioOps::setReadingsSink`).
	void setReadingsSink(device::ReadingsSink sink) noexcept { sink_ = sink; }
	/// THE METER'S READINGS NOW, in `Reading`'s order — what the sink was last handed.
	[[nodiscard]] float readingForTest(Reading r) const noexcept {
		switch (r) {
		case Reading::InPeak: return inPeak_.reading();
		case Reading::WetPeak: return wetPeak_.reading();
		case Reading::LimiterGr: return limiterGr_.reading();
		default: return 0.0f;
		}
	}
	/// THE INPUT GAIN, LINEAR, as applied to the last frame sent.
	[[nodiscard]] float inputGainForTest() const noexcept { return inputGain_.value(); }
	/// **THE FIRMWARE'S FILTER KNOBS** (0..1 on its coefficients), in place of the device's Hz (backlog LB), for the case
	/// claiming the device IS the firmware's reverb: no corner in Hz lands on the firmware's float coefficient to the bit.
	/// Negative returns to Hz. Applied with the rest of the settings at the next change (`applyChanges`).
	void setFirmwareFiltersForTest(float hpfKnob, float lpfKnob) noexcept {
		firmwareHpf_ = hpfKnob;
		firmwareLpf_ = lpfKnob;
	}
	[[nodiscard]] const conv::Convolver& convolverForTest() const noexcept { return conv_; }
	[[nodiscard]] const ConvGains& convGainsForTest() const noexcept { return convGains_; }
	/// Whether a kernel is waiting for the audio thread to take it.
	[[nodiscard]] bool irPendingForTest() const noexcept { return pending_.load() != nullptr; }
	/// The swap's dip gain, 1 when no swap is in progress.
	[[nodiscard]] float swapGainForTest() const noexcept { return swapGain_; }
	/// Whether the convolution has faded out in Off and been cleared — nothing of it runs.
	[[nodiscard]] bool convDeadForTest() const noexcept { return convDead_; }
	[[nodiscard]] float convOutForTest() const noexcept { return convOut_; }

private:
	/// MESSAGE THREAD: the generated room at this rate, as a source to shape.
	static conv::LoadedIr generatedWhole(double sampleRate, int which);
	/// The envelope as gains over the span (`kEnvelopePoints` of them), or empty when it is flat at 1.
	static std::vector<float> envelopeOf(const conv::IrShape& shape);
	static constexpr size_t kEnvelopePoints = 513;
	/// MESSAGE THREAD: read the whole file into `whole_` at this rate; false (with `irError_`) if it cannot.
	bool readWhole(const std::string& path);
	/// MESSAGE THREAD: the source again — the file, or the generated room — into `whole_`.
	bool readSource();
	/// `whole_` under a shape.
	[[nodiscard]] conv::LoadedIr shaped(const conv::IrShape& shape) const;
	/// MESSAGE THREAD: a kernel for `whole_` under `shape_`; one that runs nothing, with `irError_`, for a
	/// silent span.
	std::unique_ptr<conv::ConvKernel> shapedKernel();
	/// MESSAGE THREAD: hand a finished kernel over — taken at the audio thread's next chunk.
	void post(std::unique_ptr<conv::ConvKernel> kernel);
	/// MESSAGE THREAD: free what the audio thread has put down.
	void collect() noexcept;
	/// MESSAGE THREAD, AUDIO STOPPED (prepare, destruction): run this kernel now, freeing everything else.
	void install(std::unique_ptr<conv::ConvKernel> kernel);
	/// AUDIO THREAD, BEFORE A CHUNK OF CONVOLUTION: begin a swap if a kernel waits, or make it if the dip
	/// has reached the bottom (or the convolver is silent, when there is nothing to dip).
	void swapBegin() noexcept;
	/// AUDIO THREAD, AFTER IT: the dip's gain over the chunk's convolution.
	void swapRamp(float* convL, float* convR, int n) noexcept;
	/// AUDIO THREAD: run the waiting kernel now, putting the old one down. False if nothing waits.
	bool swapNow() noexcept;
	/// A FRESH CORE with every setting applied in the song-load order — what `reset` defers to the next
	/// buffer boundary.
	void rebuild() noexcept;
	/// Push every control that differs from what the core last saw, in the song-load order.
	void applyChanges(bool all) noexcept;

	std::vector<float> buffer_;
	Core core_ {};
	/// AURORA, run in the core's place while the Size Mode names it (`kAuroraMenu`); the core idles meanwhile.
	Aurora aurora_ {};
	double sampleRate_ = 48000.0;

	Settings target_ {};
	Settings applied_ {};
	/// FRAMES INTO THE CURRENT FIRMWARE BUFFER. 0 means "at a boundary": the next frame begins one.
	int gridPos_ = 0;
	/// SET BY `reset`, CLEARED BY `rebuild` at the next buffer boundary.
	bool rebuildPending_ = true;
	/// **OVERSAMPLING** (backlog JE): the running core's factor, its resamplers, and the fade a change of it takes —
	/// the reverb out over `kOversampleFadeSeconds`, the core rebuilt at the next boundary at the new rate (so a
	/// turn clears the tail, without a click), and back in.
	Core::OwlProbe owlProbe_ {};
	bool owlCompressor_ = true;
	bool smoothModulation_ = true;
	/// OWL'S TRIM, as a gain gliding to `owlDecayTrimDb` (1 in every other kind).
	float owlTrim_ = 1.0f;
	/// THE WET'S SAFETY CEILING, with its lookahead (`kWetLookaheadSeconds`). Sized at `prepare`.
	dsp::LookaheadLimiter limiter_ {};
	bool wetLimiter_ = true;
	bool owlTrimOn_ = true;
	float firmwareHpf_ = -1.0f;
	float firmwareLpf_ = -1.0f;
	/// **INPUT** (backlog LA): the send's gain, linear, ramped; and the meter's three held readings (`Reading`),
	/// published every call to the host's sink.
	dyn::LinearRamp inputGain_ {};
	dyn::HeldPeak inPeak_ {};
	dyn::HeldPeak wetPeak_ {};
	dyn::HeldPeak limiterGr_ {};
	device::ReadingsSink sink_ {};
	void publish() noexcept;
	int coreOs_ = 1;
	HalfbandUp upIn_ {};
	HalfbandUp upInj_ {};
	HalfbandDown downL_ {};
	HalfbandDown downR_ {};
	float osFade_ = 1.0f;
	float osFadeStep_ = 0.0f;
	/// THE REFERENCE'S GATE (`kGateSeconds`): whether the core is running, frames since the send was last
	/// non-zero, the gate in frames, and this buffer's return level so far.
	bool running_ = false;
	int64_t quietFrames_ = 0;
	int64_t gateFrames_ = 0;
	double sumAbsL_ = 0.0;
	double sumAbsR_ = 0.0;

	float mixNow_ = 0.3f;
	float mixCoeff_ = 0.0f;

	/// THE CONVOLUTION (backlog JD): one engine, its IR generated at prepare (`conv::generateTestIr`) until
	/// IRs can be loaded; the routing gains; and how long its input has been silent.
	conv::Convolver conv_;
	ConvGains convGains_ {};
	float convCoeff_ = 0.0f;
	uint64_t convQuiet_ = UINT64_MAX / 2;
	bool convPlace_ = true;
	/// THE CONVOLVER'S OWN PREDELAY AND FEEDBACK: the input's ring and the loop's ring (one write head, a
	/// power of two long), the predelay in frames as it glides, and the loop's gain as it glides.
	std::vector<float> preRing_;
	std::vector<float> fbRing_;
	size_t ringPos_ = 0;
	float convPredelayNow_ = 0.0f;
	float convPredelayCoeff_ = 0.0f;
	float convFeedbackNow_ = 0.0f;
	double loopWobble_ = 0.0;
	/// THE EXCITER: the convolution's injected share, in a ring the reverb reads `kExciteLagFrames` behind the running
	/// sample count; and the reverb's own dry send, gliding (Exciter trades it for the room).
	std::vector<float> exciteRing_;
	uint64_t exciteT_ = 0;
	float sendNow_ = 1.0f;
	/// **OFF KILLS THE TAIL** (verify it kills all tails and really bypasses the conv when you select it
	/// off): the convolution's own output gain, ramped linearly over `kConvGainSeconds` toward 0 in Off and 1
	/// otherwise; and whether it has reached 0 in Off and been cleared, so nothing of the convolution runs.
	float convOut_ = 0.0f;
	float convOutStep_ = 0.0f;
	bool convDead_ = true;
	/// AUDIO THREAD: the convolver, both rings and the loop forgotten — what Off leaves behind.
	void killConv() noexcept;

	/// **THE KERNELS, AND WHO OWNS WHICH.** `runningKernel_` is the audio thread's once audio runs. `pending_`
	/// carries a new one over (the message thread stores, the audio thread takes by exchange, so exactly
	/// one of them owns it). `retired_` carries the old one back: the audio thread never frees, it puts
	/// the kernel it stopped running into an empty slot, and the message thread frees it at its next
	/// call. **THE AUDIO THREAD DOES NOT BEGIN A SWAP UNLESS A SLOT IS EMPTY**, so a message thread that
	/// has not been back cannot leave the convolution muted — the old IR simply keeps playing.
	static constexpr int kRetiredSlots = 4;
	conv::ConvKernel* runningKernel_ = nullptr;
	std::atomic<conv::ConvKernel*> pending_ { nullptr };
	std::atomic<conv::ConvKernel*> retired_[kRetiredSlots] {};
	/// The dip: 1 → 0 while the old kernel fades out, the swap, 0 → 1 as the new one fades in.
	float swapGain_ = 1.0f;
	float swapStep_ = 0.0f;
	int swapPhase_ = 0; // 0 idle, 1 fading out, 2 fading in

	std::string irPath_;
	std::string irError_;
	bool irTruncated_ = false;
	/// THE WHOLE FILE at this rate, held so a marker's move re-cuts it without the disk. Empty for the
	/// generated room.
	conv::LoadedIr whole_;
	conv::IrShape shape_;
	uint64_t sourceSerial_ = 0;
	int generated_ = conv::kGeneratedRoom;
};

} // namespace feathers::fx::featherverb
