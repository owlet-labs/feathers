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

#include <clap/clap.h>

#include <cstdint>

// A CLAP EXTENSION THAT PLACES A DEVICE IN THE FX LANE AND SETS ITS CONTROLS, so a test host can put
// the editor in a state and photograph it.
//
// WHY IT HAS TO EXIST. A device's controls are SWALLOWED — they are not CLAP parameters, so
// `clap_plugin_params` cannot reach one, and guihost's `--set` addresses the host parameters only.
// The consequence, until this: no device surface had ever been rendered by anything. Six devices
// ship a card each and every one of them had been looked at by human eyes alone.
//
// PRESENT IN EVERY BUILD, NOT GATED ON FEATHERS_DIAGNOSTIC, and that is the whole point rather than
// an oversight. A diagnostic-only seam means the build we photograph is not the build we ship, so a
// clean picture would be evidence about a binary nobody runs — "render geometry before believing
// it" defeated at one remove. graphics_utils.h makes the same argument about the full-redraw flag:
// "a fix that can be turned off by accident is not a fix."
//
// THE VERSION IS IN THE ID. A host that does not ask for this exact string never sees it, and the
// day the string means something to somebody else it stops being ours to change quietly.
//
// **/2 (2026-09-25, macro A9) appended `rename_macro`.** Bumped rather than appended under /1, because a
// caller built against the longer struct reading a /1 plugin would call through a pointer past its end.
//
// **/3 (2026-09-25, host slots T6a) appended the seven slot entries** (`touch_slot` .. `replace_with_filter`), for /2's
// reason. An entry naming a road a plugin does not have fills it with a function that refuses (the standalone
// rack, which owns a pool since T10, refuses `undo_remove`, the two preset roads and `replace_with_filter`).
//
// **/4 (2026-10-03, the engine-restart cases) appended `device_body`, `edit_device_body` and `add_mod_device`**,
// and `CLAP_FEATHERS_TEST_STRIP`: a position with that bit set names the modulator strip's slot, for every entry
// that takes a position. /3's reason for the bump.
//
// **/5 (2026-10-29, the arp) appended `delivered_note`**: the notes the voices were handed and their samples, so a
// case reads an arp's steps by value through a whole plugin instance (a save and a load into a fresh one).
//
// **/6 (2026-11-04, the Pattern's hand edits) appended `set_device_block`**: a device's hand block written whole, on the
// road the editor's own write takes (`DeviceHost::setBlock` over the bridge), so a case can make an edit as the window
// makes it and read what the voices are handed.
//
// **/7 (2026-11-06, backlog QC) appended `set_preset_identity`, `preset_identity` and `preset_face`**: which preset the patch
// came from, set on the road the editor's browser takes, read back from the patch, and read off an open editor's browser —
// so a case can hold the name through an engine restart and a fresh instance by value.
//
// **/8 (2026-11-07, backlog QC) appended `set_device_preset` and `device_preset`**: which preset a DEVICE's panel came from,
// by position, on the road the panel's browser takes (`DeviceHost::setDevicePreset`). The rack plugin answers /7's three
// with its own browser's name (`RackBridge::topPreset`).
//
// **/9 (2026-11-07, variant-seamless) appended `voice_reading`**: one voice's pitch, oscillator phase and edge level BY
// VALUE, so a case can say a held note kept its phase and reached the new pitch across a whole-plugin preset load (a
// variant chip) — claims no rendered buffer can carry (CLAUDE.md: assert the value).
//
// **/10 (2026-11-08) appended `release_control`**: a strip control written as a MOMENTARY'S RELEASE (`DeviceHost::releaseField`),
// the road a press quicker than a block takes. Null in the rack.
//
// **/11 (2026-11-09) appended `param_index_of` and `param_info_builds`**: the index the plugin's clap-helpers base is told
// for a param id (`getParamIndexForParamId`, which the base asks on every value/text call) and how many `clap_param_info`
// the plugin has built, so a case can say BY VALUE that the lookup agrees with a `get_info` walk and costs no walk.
static constexpr char CLAP_EXT_FEATHERS_TEST_CHAIN[] = "com.owletrecords.feathers.test-chain/12";

/// What `voice_reading` reads (`Synth::voicePitchForTest`, `voicePhaseForTest`, `voiceEdgeLevelForTest`).
enum clap_feathers_voice_reading : uint32_t {
	CLAP_FEATHERS_VOICE_PITCH = 0, ///< semitones
	CLAP_FEATHERS_VOICE_PHASE = 1, ///< the oscillator's accumulator, cycles 0..1
	CLAP_FEATHERS_VOICE_LEVEL = 2, ///< the voice edge's level, 0..1 (0: not sounding)
	CLAP_FEATHERS_VOICE_STAGE = 3, ///< `Voice::Stage` as a number: 0 Idle, 1 Attack, 2 Sustain, 3 Release, 4 Stealing
	CLAP_FEATHERS_VOICE_KEY = 4,   ///< the key the voice manager last said this voice plays
	CLAP_FEATHERS_VOICE_NOTE_ID = 5, ///< and its note id (-1 for none)
};

/// **WHAT A SOAK READS, BY VALUE** (`synth_reading`, /11): counts of everything in the engine that could grow over a
/// long session (CPU was seen to creep up after playing Pattern in TB-Glide mode and switching engines).
/// Each is a count at the instant of the read, between blocks.
enum clap_feathers_synth_reading : uint32_t {
	CLAP_FEATHERS_SYNTH_MANAGED_VOICES = 0,  ///< the voice manager's live count (`Synth::managedVoiceCount`)
	CLAP_FEATHERS_SYNTH_SOUNDING_VOICES = 1, ///< voices making sound, tails and steal fades included
	CLAP_FEATHERS_SYNTH_ATTACK = 2,          ///< voices in each stage (`Voice::Stage`)
	CLAP_FEATHERS_SYNTH_SUSTAIN = 3,
	CLAP_FEATHERS_SYNTH_RELEASE = 4,
	CLAP_FEATHERS_SYNTH_STEALING = 5,
	CLAP_FEATHERS_SYNTH_KEYS_DOWN = 6,       ///< keys the voices have been told are down (`heldNotes_`)
	CLAP_FEATHERS_SYNTH_LANE_INVENTED = 7,   ///< invented notes the lane holds sounding downstream
	CLAP_FEATHERS_SYNTH_LANE_MOD_RINGS = 8,  ///< modulation rings in use on the lane's engine
	CLAP_FEATHERS_SYNTH_LANE_RETIRED = 9,    ///< retired device storage awaiting the message thread
	CLAP_FEATHERS_SYNTH_LANE_PENDING = 10,   ///< pending field writes on the lane's engine
	CLAP_FEATHERS_SYNTH_PATTERN_HELD = 11,     ///< keys the (first) Pattern on the lane holds (`HeldChord::size`)
	CLAP_FEATHERS_SYNTH_PATTERN_SOUNDING = 12, ///< notes it is sounding downstream
	CLAP_FEATHERS_SYNTH_PATTERN_LOWEST = 13,   ///< the lowest key it holds, -1 for none
	CLAP_FEATHERS_SYNTH_PATTERN_NEWEST = 14,   ///< the key it holds that was pressed last, -1 for none
	CLAP_FEATHERS_SYNTH_READING_COUNT
};

/// A POSITION WITH THIS BIT SET NAMES THE MODULATOR STRIP'S SLOT (`position & ~bit`), not the lane's.
static constexpr uint32_t CLAP_FEATHERS_TEST_STRIP = 0x10000u;

extern "C" {

// MAIN THREAD ONLY, EVERY ONE. They go through GuiBridge on the roads the editor's own widgets take,
// which are main-thread roads; the audio thread reads the result through the atomics the bridge
// publishes. Said here rather than left to be inferred, because a seam that is quietly callable from
// the audio thread is a rule waiting to be broken by whoever picks it up next.
struct clap_plugin_feathers_test_chain {
	/// APPEND A DEVICE TO THE LANE BY ITS OWN TAG. Returns the slot it took, or UINT32_MAX if the
	/// lane refused it — an unknown tag, a device that cannot render audio, or a full lane.
	///
	/// A TAG RATHER THAN AN INDEX INTO kDevices, following GuiBridge::addFxDevice: an index is a
	/// position in a table that may be reordered, and the tag is what a saved row stores.
	uint32_t (*add_device)(const clap_plugin* plugin, const char* tag);

	/// SET ONE CONTROL OF THE DEVICE IN `slot`, BY THE CONTROL'S NAME. False if the slot is empty,
	/// the device is one this build cannot name, or no control of it is called `control`.
	///
	/// BY NAME AND NOT BY CONTROL INDEX, which is a deliberate departure from the bridge road
	/// underneath — `GuiBridge::setFxDeviceControl` takes an index, and this resolves the name
	/// against the PLUGIN's own registry before calling it. The reason is that the caller is a
	/// separate binary with its own compiled-in copy of kDevices. Passing an index would make the two
	/// copies agree silently or disagree silently, and a disagreement writes a REAL control that is
	/// simply the wrong one — the quiet, convincing failure this project keeps paying for. A name
	/// that the plugin cannot find is refused and reported instead. It is guihost's own rule at the
	/// door: "a hard-coded id that silently moves is a run that proves nothing about the parameter it
	/// claims to be moving."
	///
	/// THE WRITE ITSELF STILL TAKES THE KNOB'S ROAD, coerced by the control's own declaration, so
	/// this cannot set a value a knob could not. That is a condition on the seam and not an
	/// implementation detail: if the tool could reach past the UI, the photograph would stop being of
	/// the product.
	bool (*set_control)(const clap_plugin* plugin, uint32_t slot, const char* control, double value);

	/// WHAT DEVICES THIS BUILD WILL LET YOU PLACE -- walk `index` upward from 0 until it returns
	/// false, writing each device's tag into `tag_out`. False also for a null or zero-sized buffer.
	///
	/// THE HOST CANNOT KNOW THIS ANY OTHER WAY, and that is the whole reason it is here. A host that
	/// hard-codes the tags is a host somebody must edit the day a device is added -- and the first
	/// six surfaces were photographed exactly that way, with the tags read out of `*_types.h` by
	/// hand. This is the road `--set` already takes for host parameters: learn the names by ASKING,
	/// knowing nothing of our headers.
	///
	/// ONLY DEVICES THE LANE WILL ACCEPT (`maySitInLane`: audio devices, modulators and note devices, since
	/// 2026-10-04). A device the lane refuses would hand a caller a tag that is guaranteed to be
	/// rejected, so a host looping over this list would abort on a device that was never placeable. An enumeration whose entries the next call refuses is worse
	/// than none: it looks authoritative and is wrong.
	bool (*available_device)(const clap_plugin* plugin, uint32_t index, char* tag_out,
	                         uint32_t tag_capacity);

	/// HOW MANY DEVICES ARE IN THE LANE.
	///
	/// THE NON-VACUITY GUARD, and it is the reason this is three functions and not two. A photograph
	/// of an empty lane looks exactly like a photograph of a seeded one to anything that is not
	/// looking at the pixels, so without this a run that placed nothing reports the same success as a
	/// run that worked. Sweeps in this repo have photographed a device they never placed; the guard
	/// belongs to the tool for the same reason it belongs to a case.
	uint32_t (*device_count)(const clap_plugin* plugin);

	/// NAME MACRO `index` — the rename the macro's own caption commits, through the same bridge call, so
	/// a venue can put a name in front of the DAW without an editor. An empty name is the default
	/// ("Macro N"). False for an index past the plugin's last macro or a null name.
	///
	/// WHY A VENUE NEEDS IT: a macro's name is NOT a parameter, and it is the one thing about a
	/// parameter a host re-reads on its own (`CLAP_PARAM_RESCAN_INFO`). Without this road the only way to
	/// test that the host is told was an editor on a display.
	bool (*rename_macro)(const clap_plugin* plugin, uint32_t index, const char* name);

	/// **THE HOST SLOTS (backlog JU, T6a) — a venue drives the pool the way T7's "Expose to host" and T9's
	/// touch will**, because until they exist nothing in the product maps a control and the host surface would
	/// be untestable. Both name the lane device at `position` and its control by NAME (`set_control`'s rule),
	/// and return the slot index (0-based), or UINT32_MAX when nothing could be mapped. Neither sends the host
	/// anything itself: each asks for a main-thread visit (`request_callback`), and the rename reaches the host
	/// from THAT, once however many slots moved.
	uint32_t (*touch_slot)(const clap_plugin* plugin, uint32_t position, const char* control);
	uint32_t (*pin_slot)(const clap_plugin* plugin, uint32_t position, const char* control);

	/// REMOVE THE LANE DEVICE AT `position` as its card's ✕ does, holding it and its routes for one
	/// `undo_remove`, which puts it back under the same id as the editor's undo does. False when there is
	/// nothing to remove or nothing held.
	bool (*remove_device)(const clap_plugin* plugin, uint32_t position);
	bool (*undo_remove)(const clap_plugin* plugin);

	/// THE PRESET BROWSER'S TWO ROADS — the whole plugin to and from bytes as a PRESET, which is not the host's
	/// `state` road (a preset keeps the project's host slots, plan §0.8). False for a refused load.
	bool (*capture_preset)(const clap_plugin* plugin, const clap_ostream* stream);
	bool (*load_preset)(const clap_plugin* plugin, const clap_istream* stream);

	/// STAMP A FILTER ONTO THE LANE DEVICE AT `position`, UNDER ITS ID — the filter card's settings road
	/// (`ParamBridge::setFilterSettings`) aimed at a position holding another kind of device, which the record
	/// takes as a REPLACEMENT in place: a different tag under the same id, with no removal for anything to see.
	/// The one lane road that does that, and so the one a venue needs to show the host slots reconcile it.
	/// False when there is no device there.
	bool (*replace_with_filter)(const clap_plugin* plugin, uint32_t position);

	/// **THE HOST SLOTS' WRITE ROADS (T6b) — values a venue must ASSERT, not infer from a render.**
	///
	/// `slot_state`: slot `slot`'s pool state as its `hostslots::SlotState` number (Empty 0, Default 1,
	/// Recent 2, Pinned 3, Anchored 4), or UINT32_MAX past the pool.
	uint32_t (*slot_state)(const clap_plugin* plugin, uint32_t slot);
	/// `engine_control`: the lane ENGINE's base and resolved value (where modulation left it this block) of the
	/// lane device at `position`'s control called `control` — what the device actually ran with. Only with no
	/// block in flight. False when the engine does not run the device or it declares no such control.
	bool (*engine_control)(const clap_plugin* plugin, uint32_t position, const char* control, double* base,
	                       double* resolved);
	/// `knob_value`: what that control's knob reads (`DeviceControl::value`, over the bridge's record).
	bool (*knob_value)(const clap_plugin* plugin, uint32_t position, const char* control, double* value);
	/// `drag_control`: A HAND ON THE KNOB — one `DeviceControl` gesture, as a widget drives it: `beginGesture`,
	/// `setValue` for each of `values`, `endGesture`, recording its undo step into a history the plugin keeps
	/// for this seam. `undo` undoes that history's last step, through the same `DeviceValueEdit` the editor's
	/// undo runs. False when there is no such control / nothing to undo.
	bool (*drag_control)(const clap_plugin* plugin, uint32_t position, const char* control, const double* values,
	                     uint32_t count);
	bool (*undo)(const clap_plugin* plugin);
	/// `reset_control`: a DOUBLE-CLICK on a `ValueBox` over that control — the box's own reset road.
	/// `abandon_drag`: a hand takes the control and moves it to `value`, and the control is DESTROYED with
	/// the hand still down (a card rebuilt mid-drag). False when there is no such control.
	bool (*reset_control)(const clap_plugin* plugin, uint32_t position, const char* control);
	bool (*abandon_drag)(const clap_plugin* plugin, uint32_t position, const char* control, double value);

	/// **A DEVICE'S BODY — the state no row can hold** (a Featherverb's IR, a chain's contents). `device_body`
	/// writes what a save would write for the lane device at `position`: the RUNNING instance's body when one is
	/// on file, the stored bytes otherwise. `edit_device_body` is an edit in the device's own editor: `data` is
	/// loaded into the RUNNING instance (its body loader, on the handle the bridge filed), so — as with a real
	/// edit — it lives only there until a save asks for it. False when there is no such device or body.
	bool (*device_body)(const clap_plugin* plugin, uint32_t position, const clap_ostream* out);
	bool (*edit_device_body)(const clap_plugin* plugin, uint32_t position, const void* data, uint64_t size);
	/// APPEND A MODULATOR TO THE STRIP by its tag, as the editor's strip (+) does; its slot, or UINT32_MAX.
	uint32_t (*add_mod_device)(const clap_plugin* plugin, const char* tag);
	/// **THE LONGEST BLOCK THE LANE'S ENGINE WAS PREPARED WITH** (backlog GO) — what `activate`'s `max_frames_count`
	/// became on its way to every device's `AudioOps::prepare`. Read while inactive.
	uint32_t (*max_frames)(const clap_plugin* plugin);
	/// **HOW MANY TIMES THAT ENGINE HAS BEEN PREPARED** — every prepare rebuilds every device's state, so a reactivation
	/// that must leave the devices alone (a same-rate shrink, backlog GO) is asserted as this count not moving.
	uint64_t (*prepare_count)(const clap_plugin* plugin);
	/// **WHAT THE LANE DEVICE AT `position` LAST PUBLISHED** on the generic readings channel, as its card reads it: reading `index`
	/// into `out`. False when the device published fewer than `index + 1`.
	bool (*device_reading)(const clap_plugin* plugin, uint32_t position, uint32_t index, float* out);
	/// **A ROUTE BETWEEN TWO HOST ENDS** (backlog OJ) — `Source::core(source)` to `Destination::core(destination)` at
	/// `amount`, through the bridge's own `setRouteAmount` (the ring's road); false if the record does not hold it
	/// after (the rule refused it). Zero removes.
	bool (*set_core_route)(const clap_plugin* plugin, uint32_t source, uint32_t destination, double amount);
	/// **WHAT THE ENGINE ADOPTED** — how many routes its staged snapshot holds, the amount it holds for one pair (0 for
	/// none), and how many publishes it refused for want of room (`ParamSet::routeRoomRefusals`, plus the lane
	/// engine's). Read between blocks.
	uint32_t (*engine_route_count)(const clap_plugin* plugin);
	double (*engine_core_route_amount)(const clap_plugin* plugin, uint32_t source, uint32_t destination);
	uint32_t (*route_room_refusals)(const clap_plugin* plugin);
	/// **A ROUTE FROM A STRIP MODULATOR'S OUTPUT TO A DEVICE CONTROL** (the standing benchmark, backlog OR) — `stripSlot`'s output
	/// `output` to the control called `control` of the device at `position` (lane, or strip with CLAP_FEATHERS_TEST_STRIP), at
	/// `amount`, through the bridge's `setRouteAmount`; a `control` of `#N` names HOST destination N (a voice parameter). False if refused.
	bool (*set_device_route)(const clap_plugin* plugin, uint32_t stripSlot, uint32_t output, uint32_t position,
	                         const char* control, double amount);
	/// **THE NOTES THE VOICES WERE HANDED** (`Synth::deliveredForTest`), oldest first since the last clear: event
	/// `index`'s kind (`device::NoteEvent::Kind`: 0 on, 1 off, 2 choke, 3 expression), its key, and the sample it was
	/// handed at, counted from the instance's first render. False past the last. `index == UINT32_MAX` clears the log
	/// and returns true. Read between blocks. Null in the rack, which has no voices.
	bool (*delivered_note)(const clap_plugin* plugin, uint32_t index, uint32_t* kind, int32_t* key, uint64_t* sample);
	/// **A DEVICE'S HAND BLOCK, WRITTEN WHOLE** (`StateBlockDesc::handEdited`), by the block's key, through the bridge's own
	/// `DeviceHost::setBlock` — the editor's road. False for no such device or block, or the wrong length. Null in the rack.
	bool (*set_device_block)(const clap_plugin* plugin, uint32_t position, const char* key, const float* floats,
	                         uint64_t count);
	/// **NAME THE PRESET THE PATCH CAME FROM** (/7, backlog QC) — `ParamBridge::setPresetIdentity`, the road the editor's
	/// browser takes when it loads or saves one. Empty `name` names none.
	bool (*set_preset_identity)(const clap_plugin* plugin, const char* name, const char* category);
	/// The patch's copy, as the `preset` block saves it. False when it does not fit the buffers.
	bool (*preset_identity)(const clap_plugin* plugin, char* name_out, uint32_t name_capacity, char* category_out,
	                        uint32_t category_capacity);
	/// **WHAT AN OPEN EDITOR'S BROWSER NAMES** — its face (`currentName`), "" when it names nothing. False with no editor.
	bool (*preset_face)(const clap_plugin* plugin, char* name_out, uint32_t capacity);
	/// **NAME THE PRESET A DEVICE'S PANEL CAME FROM** (/8) — the device at `position`, on its panel browser's road.
	bool (*set_device_preset)(const clap_plugin* plugin, uint32_t position, const char* name, const char* category);
	/// The device's copy, as the patch saves it. False for no device there, or buffers too small.
	bool (*device_preset)(const clap_plugin* plugin, uint32_t position, char* name_out, uint32_t name_capacity,
	                      char* category_out, uint32_t category_capacity);
	/// **ONE VOICE'S READING BY VALUE** (/9): voice `slot`'s `what` (`clap_feathers_voice_reading`). False for a slot
	/// out of range or an unknown `what`. Read between blocks. Null in the rack, which has no voices.
	bool (*voice_reading)(const clap_plugin* plugin, uint32_t slot, uint32_t what, double* out);
	/// **A MOMENTARY'S RELEASE** (/10) — `set_control` on the strip, by name, but as a release (`DeviceHost::releaseField`): a
	/// fall whose rise no block has read yet waits for the plugin's next main-thread visit after one has. Strip slots only.
	bool (*release_control)(const clap_plugin* plugin, uint32_t slot, const char* control, double value);
	/// **THE SYNTH'S SOAK READINGS BY VALUE** (/11): `what` is a `clap_feathers_synth_reading`. False for an unknown
	/// `what`. Read between blocks. Null in the rack, which has no voices.
	bool (*synth_reading)(const clap_plugin* plugin, uint32_t what, double* out);
	/// **THE PARAM INDEX THE BASE IS TOLD FOR `id`** (/12): the plugin's `getParamIndexForParamId`, -1 for no such id.
	int32_t (*param_index_of)(const clap_plugin* plugin, uint32_t id);
	/// **HOW MANY `clap_param_info` THE PLUGIN HAS BUILT** (/12), every `paramsInfo` call counted, the host's and the
	/// plugin's own.
	uint64_t (*param_info_builds)(const clap_plugin* plugin);
};

} // extern "C"
