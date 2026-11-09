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

#include "plugin/gui_bridge.h"
#include "plugin/preset_store.h"
#include "plugin/preset_tags.h"
#include "plugin/ui/escape_stack.h"
#include "plugin/ui/file_dialog.h"
#include "plugin/ui/item_stepper.h"
#include "plugin/ui/keyboard_claim.h"
#include "plugin/ui/widget_registry.h"

#include <visage/ui.h>

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

// THE PRESET BROWSER — the `ItemSource` for presets, and the verbs only presets have. It sits in the
// editor's header right of the FEATHERS wordmark, and in a device panel's title row (backlog FO).
//
// **THE ROW IS THE SHARED `ItemStepper`** (◀ name ▶ and its list); this class is what a preset IS and
// what can be done to one:
//   - THE BANK is a folder under the preset root (`preset_store.h`): every `.fpreset` in it and its
//     category subfolders. Factory presets ship as FILES, copied in when missing and never over a copy
//     the user has edited; one that could not be written (no root) is kept in memory, read-only.
//   - THE LIST IS PRESETS, THEN AS FEW VERBS AS POSSIBLE (everything else is consolidated into a second
//     layer of hierarchy, so the menu is not clogged with things besides presets):
//     the presets and their sections, a separator, THE THREE SAVES — the verbs a hand reaches for every
//     session (backlog PP): `Save "X"` (overwrite the preset last saved or loaded, in place — never offered for a
//     factory preset; the row names
//     it, so the click is the one confirmation), `Save as…` (the prompt, pre-filled with X's name and tags)
//     and `Save as "X 2"` (a name-variant in one gesture: the next free suffix, X's tags, X's folder) — and
//     "Manage", a submenu holding everything else in four groups: what to do to THIS preset
//     (Rename / tags…, Delete…), the doors in and out (Import…, Export…), how the list is filed
//     (Browse by folder / by tag), and where the library is (Choose preset folder…, Reset preset folder).
//     `itemActions` is the one statement of that layout.
//   - THE LIST browses BY FOLDER (a category is a submenu) or BY TAG — two levels, namespace ▸ value
//     (count) ▸ presets, values by count and past twelve under "More…" (`preset_tags.h`), a preset under
//     every tag it carries — "Browse by folder" / "Browse by tag" in Manage, BOTH always there
//     and the current one marked "• ", so the choice is seen before it is made. Choosing one REOPENS the
//     list in that view (otherwise a click appeared to do nothing — the menu closed and the face looked the
//     same). A tag view over a bank with no tags says so in a dimmed first row.
//   - SAVE… asks for a name and tags first (tags autocomplete against the known tags, with a few
//     suggestions). EDIT… renames, moves between categories
//     ("Pads/Glass") and retags. DELETE asks first.
//   - CHOOSE PRESET FOLDER… opens the OS folder picker, and the folder chosen becomes the preset ROOT for
//     every browser in the process — this one, the header's, every device panel's — which all re-list at
//     once, installing their factory presets into it when missing. It replaced "Show in file manager"
//     because the picker shows the folder too.
//     RESET PRESET FOLDER goes back to ~/Documents/Owlet/Feathers/Presets. Both are disabled while
//     `FEATHERS_PRESET_ROOT` pins the root, when they would change nothing.
//   - IMPORT… AND EXPORT… go through the OS's own file dialog (`file_dialog.h`, the regular OS file
//     browser): a `.fpreset` from anywhere is loaded and copied into this bank, and the
//     live sound is written to anywhere as one. The list stays the way to BROWSE; the dialog is only the
//     door in and out.
//   - WHAT THE BYTES MEAN is the `PresetProvider`'s.
//
// **THE PROMPT** is a small panel under the browser, in the window's top frame so it is not clipped by
// a 28 px title row: a Name field, a Tags field (comma separated, completing against this bank's tags —
// Tab or Enter takes the first completion), a row of up to five chips (completions while a tag is being
// typed, the bank's best tags otherwise — `preset_tags.h` states the ranking), and a line saying why an
// Enter did not save. Enter saves, Esc writes nothing, a name already in the bank asks before replacing.
//
// **ITS FIELDS TAKE EVERY KEY WHILE OPEN.** visage bubbles an unused key to the parents, and the editor's
// root turns `+`/`-` into a UI-scale step and an overlay turns Esc into a dismissal. The plugin window
// must have X focus for any key to arrive: in Bitwig that is a click in the editor, which the click on
// "Save preset…" already is.

namespace feathers {
class EditorPrefs;
}

namespace feathers::ui {

/// One entry's payload: its serialized bytes.
struct PresetData {
	std::vector<uint8_t> bytes;
};

/// **THE ONE SEAM BETWEEN THE BROWSER AND WHAT A PRESET CHANGES.** A save asks `capture`; a pick hands
/// the entry to `apply`. Everything about the encoding lives behind it, so a new format (backlog JF's
/// keyed state) replaces a provider and not the browser.
class PresetProvider {
public:
	virtual ~PresetProvider() = default;
	/// The live state as bytes. False: nothing could be captured, and the save writes nothing.
	virtual bool capture(PresetData& out) = 0;
	/// Put an entry into the live thing. MESSAGE THREAD. FALSE: it did not load (the owner has already
	/// said why), the selection stays where it was, and a step moves on past it (`ItemStepper`).
	virtual bool apply(const PresetData& data) = 0;
};

/// A provider out of two functions — for an owner whose capture and apply are already free functions,
/// and for the cases. No apply function: every entry "loads".
class FunctionPresetProvider final : public PresetProvider {
public:
	FunctionPresetProvider(std::function<bool(PresetData&)> capture, std::function<bool(const PresetData&)> apply)
	    : capture_(std::move(capture)), apply_(std::move(apply)) {}
	bool capture(PresetData& out) override { return capture_ ? capture_(out) : false; }
	bool apply(const PresetData& data) override { return apply_ ? apply_(data) : true; }

private:
	std::function<bool(PresetData&)> capture_;
	std::function<bool(const PresetData&)> apply_;
};

class Notifier;
class PresetPrompt;

class PresetBrowser final : public ItemStepper, private ItemSource {
public:
	/// The editor's header strip is 52 px and takes the full size; a floating panel's title row is 28 px
	/// (`ExpandedOverlay::kTitleHeight`) and takes the compact one.
	static constexpr Style kHeaderStyle { 26.0f, 160.0f, 24.0f };
	static constexpr Style kPanelStyle { 20.0f, 124.0f, 18.0f };

	/// The plugin's own bank at the header's size.
	PresetBrowser(WidgetRegistry& registry, std::vector<std::string> factoryNames,
	              std::vector<PresetData> factoryPresets);
	/// **ONE BROWSER FOR EVERY BANK.** `factoryNames`/`factoryPresets` are installed into `bank` as files
	/// by `loadBankFromDisk`, at the top of it.
	PresetBrowser(std::vector<std::string> factoryNames, std::vector<PresetData> factoryPresets,
	              PresetBank bank, Style style);
	~PresetBrowser() override;

	/// WHO CAPTURES AND APPLIES. Not owned; must outlive this browser or be cleared first.
	void setProvider(PresetProvider* provider) { provider_ = provider; }
	[[nodiscard]] const PresetBank& bank() const noexcept { return bank_; }

	//------------------------------------------------------------------------------------------------//
	// THE LIST                                                                                        //
	//------------------------------------------------------------------------------------------------//

	struct Entry {
		std::string name;
		std::string category; ///< '/'-separated folders under the bank; empty at the top
		PresetMeta meta;
		PresetData data;
		std::filesystem::path path; ///< empty: in memory only, read-only
		[[nodiscard]] bool onDisk() const noexcept { return !path.empty(); }
	};

	/// Install the factory presets that are missing, then read the bank. The selection follows the
	/// entry it was on, by name and category, and nothing is applied: opening an editor changes no sound.
	void loadBankFromDisk();

	[[nodiscard]] size_t count() const noexcept { return entries_.size(); }
	[[nodiscard]] const Entry& entry(size_t index) const { return entries_[index]; }
	/// The entry named `name` in `category`, or -1.
	[[nodiscard]] int indexOf(const std::string& name, const std::string& category = {}) const;
	[[nodiscard]] int currentIndex() const noexcept { return index_; }
	const std::string& currentName() const;
	/// A pick or a load: the entry goes to the provider, then `onSelected` is told. False — the selection
	/// left where it was — when out of range or when the provider could not load it.
	bool selectIndex(int index);
	/// **SHOW `index` AS CURRENT WITHOUT LOADING IT** — for an undo that has already put that patch back
	/// (the editor's preset-load step), so the face names the sound that is playing. Out of range: the
	/// face names nothing rather than the patch the undo took away. `onSelected` is not told.
	void markCurrent(int index);
	/// Not on disk, so it cannot be overwritten, edited or deleted.
	[[nodiscard]] bool isReadOnly(int index) const;
	void onSelected(std::function<void(int)> selected) { onSelected_ = std::move(selected); }
	/// **THE PRESET THE SOUND CAME FROM, AS IT CHANGES** (backlog QC) — the entry last saved or loaded (`lastTouched`), by name
	/// and folder, or two empty strings once none is (a delete, a new library). A rename that follows it tells it too. The
	/// editor keeps the patch's copy (`ParamBridge::presetIdentity`) by it, so the name survives an engine restart.
	std::function<void(const std::string& name, const std::string& category)> onCurrentChanged;
	/// **PUT THE FACE BACK ON A NAMED PRESET** without loading it — what a state load or a rebuilt editor does with the
	/// patch's copy. `markCurrent` of the entry so named; an empty name changes nothing (a patch that names no preset
	/// leaves the face where the bank opened), and a name the bank no longer holds names nothing. Returns whether it found it.
	bool restoreCurrent(const std::string& name, const std::string& category);

	/// **HOW THE LIST FILES THE PRESETS.** Kept for as long as this browser lives — the header's for the
	/// whole editor, a panel's while the panel is open. `onViewChanged` is told when a HAND changes it (the
	/// list's view rows), so the header's owner can keep it across editor sessions (`EditorPrefs`);
	/// `setView` is the owner putting it back, and tells nobody.
	enum class View { Folders, Tags };
	void setView(View view) { view_ = view; }
	[[nodiscard]] View view() const noexcept { return view_; }
	std::function<void(View)> onViewChanged;
	/// THE LIST REOPENS IN THE NEW VIEW after a view row is chosen — on the next event-loop timer, not
	/// inside the closing menu's selection callback. True while that reopen is waiting.
	[[nodiscard]] bool reopenPending() const noexcept { return reopen_.isRunning(); }

	//------------------------------------------------------------------------------------------------//
	// SAVING, EDITING, DELETING                                                                       //
	//------------------------------------------------------------------------------------------------//

	/// **SAVE UNDER `typed` ("Name" or "Category/Name") WITH `tags`, NOW, WITHOUT ASKING** — what the
	/// prompt's confirmation does. Refused (false, nothing written) for an unusable name, one already in
	/// the bank (the prompt asks about that; `overwriteCurrent` replaces), or nothing captured. Selects
	/// the new entry WITHOUT applying it: it is the sound already playing.
	bool saveAs(const std::string& typed, const std::vector<std::string>& tags = {});
	/// Replace the entry at `index` with the current state, keeping its tags. Refused for a read-only one.
	bool overwriteCurrent(int index);
	/// **THE PRESET LAST SAVED OR LOADED IN THIS BROWSER** (backlog PP) — a pick, a step, a save, an
	/// overwrite, an import, an undo's `markCurrent`; followed through a rename; -1 once deleted, or before
	/// anything was saved or loaded (the selection `loadBankFromDisk` lands on by default is not a load).
	/// What the save prompt pre-fills and what the list's `Save "X"` and `Save as "X 2"` act on.
	[[nodiscard]] int lastTouched() const;
	/// **"<base> N", THE NEXT FREE SUFFIX, IN THE SAME FOLDER** as entry `index`: "Pad" → "Pad 2"; "Pad 2"
	/// → "Pad 3" (the suffix is counted on, never stacked into "Pad 2 2"); a taken one is skipped. Empty for
	/// an index out of range.
	[[nodiscard]] std::string variantName(int index) const;
	/// **ONE OF THIS BANK'S FACTORY PRESETS** — a computed one ("Init", top level) or a file the plugin ships
	/// (`factoryFiles`, at its path under the root). The list's one-click `Save "X"` is not offered for one: a
	/// factory file is not overwritten by a single click (Save as… and the name-variant still are).
	[[nodiscard]] bool isFactory(int index) const;
	/// **SAVE AS A NAME-VARIANT IN ONE GESTURE**: the live state under `variantName(index)`, with entry
	/// `index`'s tags, in its folder; the variant becomes current. A preset FILE, not the variants bank.
	/// False (nothing written) for an index out of range or nothing captured.
	bool saveVariant(int index);
	/// **RENAME, MOVE AND RETAG** entry `index`: `typed` may name a new category. Refused for a read-only
	/// entry, an unusable name, or one another entry already has — a rename never overwrites.
	bool editEntry(int index, const std::string& typed, const std::vector<std::string>& tags);
	/// Remove entry `index`'s file and the entry. Refused for a read-only one.
	bool deleteEntry(int index);
	/// What the prompt offers first: "User N", the smallest N no top-level entry has.
	std::string suggestedName() const;

	//------------------------------------------------------------------------------------------------//
	// THE VARIANT CHIPS (backlog PP part 1)                                                          //
	//------------------------------------------------------------------------------------------------//
	// Small chips next to the preset: grey if unused, bright if active, an alternate colour for saved but not active.
	// Clicking an unused chip saves; a right-click menu overwrites a variant. A chip is just a shortcut for the
	// _variant_x suffix, and the eight fit in two rows of four.
	//
	// **THE ROOT PRESET IS VARIANT 1** (semantically, the root preset is variant 1): chip 1 is the preset's own file, "<base>"; chips 2..8 are ordinary preset files
	// "<base>_variant_<n>" in the base's folder with the base's tags. Nothing in the patch: the chips are a shortcut onto
	// the bank. The BASE is the shown entry's name with any "_variant_<2..8>" taken off, so loading a variant keeps its
	// base's chips (never "X_variant_2_variant_3"). A file still named "_variant_1" (the old scheme) is no variant of
	// anything now: it lists as an ordinary preset.
	//
	// **THE LIST SHOWS EACH PRESET ONCE** (variants are not shown in the browser): a variant file
	// (`isVariantFile`) is not listed and the ◀ ▶ steps pass it by (`itemListed`); the chips are the one way to it.

	static constexpr int kVariants = 8;
	/// Chip `n`'s file name: `base` itself for 1 (the root), "<base>_variant_<n>" for 2..8.
	[[nodiscard]] static std::string variantFileName(const std::string& base, int n);
	/// `name` without a trailing "_variant_<2..8>"; `name` itself when it has none.
	[[nodiscard]] static std::string variantBase(const std::string& name);
	/// Whether `name` is a variant 2..8 of some base — what the list does not show.
	[[nodiscard]] static bool isVariantFile(const std::string& name) { return variantBase(name) != name; }
	/// NOT LISTED: a variant file (see above). Read by the list and by ◀ ▶.
	[[nodiscard]] bool itemListed(size_t index) const override;
	enum class ChipState {
		Unused, ///< grey: no file for that variant (or nothing is shown to hang it off)
		Saved,  ///< the alternate colour: the file exists and is not the one shown
		Active, ///< bright: the file is the one shown
	};
	/// Chip `n` (1..8), against the shown entry's base and folder.
	[[nodiscard]] ChipState chipState(int n) const;
	/// The entry chip `n` names, or -1.
	[[nodiscard]] int variantIndex(int n) const;
	/// **A CLICK ON CHIP `n`**: an unused one SAVES the live state as that variant, no prompt; a saved one LOADS it.
	/// False: nothing to hang it off, or the save or load refused.
	bool pressVariant(int n);
	enum class VariantVerb {
		Save,      ///< an unused chip's menu: what its click does
		Load,      ///< a saved chip's: what its click does
		Overwrite, ///< the live state over that file, in place — "overwrite variant"
		Delete,    ///< remove that file (to confirm)
		Promote,   ///< "Rename to a full preset…": the rename prompt over that file (to confirm)
		/// **"Clear variants 2–8…"** — on every chip's menu while any of 2..8 is saved. ASKS first
		/// (`showClearVariantsMenu`, Delete's pattern); the root, variant 1, is kept, and a factory file is never removed.
		ClearOthers,
	};
	/// The entries "Clear variants 2–8" would remove for the shown preset: its saved variants 2..8 that are not factory files.
	[[nodiscard]] std::vector<int> clearableVariants() const;
	/// The answer to "Clear N variants of X?": true removes `clearableVariants()` as they are NOW; false changes nothing.
	void resolveClearVariants(bool confirmed);
	/// Whether a "Clear variants" question is open.
	[[nodiscard]] bool clearVariantsPending() const noexcept { return clearPending_; }
	/// What chip `n`'s context menu lists, in order — every verb on the face before one is taken.
	[[nodiscard]] std::vector<VariantVerb> variantVerbs(int n) const;
	[[nodiscard]] static std::string variantVerbLabel(VariantVerb verb, int n);
	bool actVariant(int n, VariantVerb verb);
	void showVariantMenu(int n);

	/// THE CHIPS' GRID: 2 rows of 4, right of the stepper's ▶, the bar's full height.
	static constexpr float kChipsGap = 4.0f;  ///< between ▶ and the first column
	static constexpr float kChipGap = 2.0f;   ///< between chips, both ways
	static constexpr float kChipAspect = 1.5f;
	[[nodiscard]] static constexpr float chipHeightFor(const Style& s) { return (s.height - kChipGap) * 0.5f; }
	[[nodiscard]] static constexpr float chipWidthFor(const Style& s) { return chipHeightFor(s) * kChipAspect; }
	[[nodiscard]] static constexpr float chipsWidthFor(const Style& s) {
		return kChipsGap + 4.0f * chipWidthFor(s) + 3.0f * kChipGap;
	}
	/// **THE WHOLE BAR**: ◀ name ▶ and the chips. What every owner sizes the browser to.
	[[nodiscard]] static constexpr float barWidthFor(const Style& s) {
		return s.arrowWidth * 2.0f + s.nameWidth + chipsWidthFor(s);
	}
	[[nodiscard]] float barWidth() const noexcept { return barWidthFor(style()); }
	/// Chip `n`'s box (1..8) in the browser's coordinates; empty out of range.
	[[nodiscard]] visage::Bounds variantChipBounds(int n) const;
	/// The chip under `position`, 1..8, or 0.
	[[nodiscard]] int variantChipAt(visage::Point position) const;

	//------------------------------------------------------------------------------------------------//
	// WHERE THE LIBRARY IS — ONE ROOT FOR EVERY BROWSER                                               //
	//------------------------------------------------------------------------------------------------//

	/// "Choose preset folder…": the OS FOLDER picker, starting at the root in use; its answer goes to
	/// `setPresetRoot`. Changes nothing yet.
	void beginChooseRoot();
	/// **THE ROOT FOR THE WHOLE PROCESS** — `PresetStore::setChosenRoot`, then every live browser
	/// re-lists (factory presets installed into the new root when missing; each selection follows its
	/// entry by name, and nothing is applied), then every `PresetRootWatch` is told so its editor can keep
	/// it. Empty: back to the default. A root equal to the one already chosen does nothing. MESSAGE THREAD.
	/// Returns how many browsers re-listed (0 when the root did not change).
	static size_t setPresetRoot(const std::filesystem::path& chosen);
	/// The key the editors keep the chosen root under in `EditorPrefs`.
	static constexpr const char* kRootPrefKey = "presets.root";
	/// How many browsers are alive in this process.
	[[nodiscard]] static size_t liveCountForTest();
	/// **EVERY OPEN PROMPT IN ONE EDITOR, CLOSED — what that editor's CLOSE does** (backlog KS). The
	/// editors are kept across a close (`PluginWindow`), so a save or edit prompt open at the close stays
	/// open, and its `KeyboardClaim` keeps the X focus on a window that has left the screen: a DAW
	/// without its keyboard until something else moves the focus. Closed as a cancel — nothing typed is
	/// saved. `top` is the editor's frame and only browsers under it close; another editor's, or
	/// another instance's, are left alone. Returns how many prompts it closed.
	static size_t closePromptsUnder(const visage::Frame& top);

	//------------------------------------------------------------------------------------------------//
	// IMPORT AND EXPORT, THROUGH THE OS FILE DIALOG                                                   //
	//------------------------------------------------------------------------------------------------//

	/// "Import…": the OS dialog for one `.fpreset`; its answer goes to `importFile`. Writes nothing yet.
	void beginImport();
	/// "Export…": the OS save dialog; its answer goes to `exportTo`. Captures nothing yet.
	void beginExport();

	enum class ImportOutcome {
		Imported,   ///< loaded, copied into the bank, selected
		Asking,     ///< its name is taken: `resolveImport` has the answer. Nothing loaded or written yet
		Unreadable, ///< not a file, too big, or not a preset file: nothing loaded or written
		Refused,    ///< THE PROVIDER REFUSED IT — the same check a pick makes: nothing written
	};
	/// **IMPORT ONE FILE FROM ANYWHERE** — what the dialog's answer does. In this order, so nothing is
	/// touched until everything that can refuse it has had the chance:
	///   1. read (at most `kMaxImportBytes`) and decode it as a preset file;
	///   2. its name (the file's stem, sanitised) already in the bank: ASK, and stop here;
	///   3. hand the payload to the provider — THE SAME CHECK A NORMAL LOAD MAKES. Refused: nothing written;
	///   4. write it into the bank, at the top, keeping the file's tags; select it.
	ImportOutcome importFile(const std::filesystem::path& file);
	enum class ImportChoice { Replace, KeepBoth, Cancel };
	/// The answer to "already in the bank": replace that preset, import beside it as "Name 2", or nothing.
	/// Replace is not offered (and is `Cancel`) when the taken entry is read-only.
	ImportOutcome resolveImport(ImportChoice choice);
	[[nodiscard]] bool importPending() const noexcept { return pendingImport_ != nullptr; }
	/// **THE LIVE SOUND, AS A `.fpreset`, AT `file`** — the current entry's tags with it. ".fpreset" is
	/// added when the name has no extension. False: nothing captured, or it could not be written.
	bool exportTo(std::filesystem::path file);
	static constexpr std::uintmax_t kMaxImportBytes = std::uintmax_t { 64 } << 20;

	/// WHERE THE BROWSER SAYS WHAT HAPPENED (an import refused, an export written). Not owned; may be null.
	void setNotifier(Notifier* notifier) { notifier_ = notifier; }
	/// The OS dialog this browser opens — a case drives its answer through a scoped fake backend.
	[[nodiscard]] FileDialog& fileDialogForTest() noexcept { return fileDialog_; }

	//------------------------------------------------------------------------------------------------//
	// THE PROMPT                                                                                      //
	//------------------------------------------------------------------------------------------------//

	enum class Prompt { Closed, Naming, ConfirmReplace };
	enum class PromptKind { Save, Edit };
	/// "Save as…": open the prompt. **PRE-FILLED WITH THE NAME (category included) AND TAGS OF THE PRESET
	/// LAST SAVED OR LOADED** (`lastTouched`), so overwriting it is Enter and one "Replace?"; with none, a
	/// suggested "User N" and no tags. Writes nothing.
	void beginSave();
	/// "Edit…": the prompt over entry `index`, its name (with category) and tags in the fields.
	void beginEdit(int index);
	/// ENTER. Save: under the typed name and tags — or ASK, when that name is taken. Edit: apply the new
	/// name and tags. An unusable or (for Edit) taken name leaves the prompt open and says why.
	void submitPrompt();
	/// ESC: close and write nothing (from `ConfirmReplace`, back to naming instead).
	void cancelPrompt();
	/// The answer to "replace it?": true overwrites that preset, false returns to the fields.
	void resolveReplace(bool replace);
	/// TAB (and ENTER in the Tags field while a completion is offered): the tag being typed becomes the
	/// first completion. False when there was none to take.
	bool acceptCompletion();
	/// A chip pressed: its tag is added to the Tags field.
	void pressChip(size_t index);
	/// What the chips say now: completions for the tag being typed, else the bank's best tags.
	[[nodiscard]] std::vector<std::string> chips() const;

	[[nodiscard]] Prompt prompt() const noexcept { return prompt_; }
	[[nodiscard]] PromptKind promptKind() const noexcept { return promptKind_; }
	[[nodiscard]] const std::string& notice() const noexcept { return notice_; }
	[[nodiscard]] std::string promptTextForTest() const;
	void typeForTest(const std::string& text);
	[[nodiscard]] std::string tagsTextForTest() const;
	void typeTagsForTest(const std::string& text);
	[[nodiscard]] visage::Frame* nameFieldForTest() const;
	[[nodiscard]] visage::Frame* tagsFieldForTest() const;
	[[nodiscard]] visage::Frame* promptFrameForTest() const;

	/// **THIS BANK'S TAGS, RANKED** — per type by construction: this browser's own entries and no others.
	[[nodiscard]] std::vector<TagStat> rankedTags() const;
	/// Unix seconds. Replaceable so a case can put "now" where its presets are.
	std::function<int64_t()> clock;
	/// The embedded factory files `loadBankFromDisk` installs (`preset_store.h`). Replaceable so a case can
	/// ship its own set.
	std::span<const FactoryPresetFile> factoryFiles = factoryPresetSource();
	/// **WHAT THIS BROWSER LISTS** (backlog OU: a browser shows only the kind its context can take). Empty: every file in
	/// the bank. Set: a file whose payload it refuses is not listed — one moved into the wrong folder by hand, a Rack's in
	/// `Chains/`. Read by `loadBankFromDisk`, so set it before the first.
	std::function<bool(std::span<const uint8_t>)> accepts;

	//------------------------------------------------------------------------------------------------//
	// THE MENU                                                                                      //
	//------------------------------------------------------------------------------------------------//

	enum Action : int {
		kActSave = 1,
		kActOverwrite,
		kActEdit,
		kActDelete,
		kActByFolder,
		kActByTag,
		kActImport,
		kActExport,
		kActChooseRoot,
		kActResetRoot,
		kActSaveVariant,
		kActManageOverwrite,
	};
	static constexpr int kMenuSave = kActionBase + kActSave;
	static constexpr int kMenuOverwrite = kActionBase + kActOverwrite;
	static constexpr int kMenuEdit = kActionBase + kActEdit;
	static constexpr int kMenuDelete = kActionBase + kActDelete;
	static constexpr int kMenuByFolder = kActionBase + kActByFolder;
	static constexpr int kMenuByTag = kActionBase + kActByTag;
	static constexpr int kMenuImport = kActionBase + kActImport;
	static constexpr int kMenuExport = kActionBase + kActExport;
	static constexpr int kMenuChooseRoot = kActionBase + kActChooseRoot;
	static constexpr int kMenuResetRoot = kActionBase + kActResetRoot;
	static constexpr int kMenuSaveVariant = kActionBase + kActSaveVariant;
	/// **`Overwrite "X"` IN MANAGE** (restored to the Manage menu even though it duplicates Save as… → Enter with
	/// no changes) — the same write as `Save "X"` and as Save as… → Enter → Replace
	/// (`overwriteCurrent`), offered where it used to be. Absent with nothing saved or loaded; DISABLED, its label saying
	/// why, over a factory preset (`Save "X"`'s guard).
	static constexpr int kMenuManageOverwrite = kActionBase + kActManageOverwrite;
	/// The submenu everything but Save lives in.
	static constexpr const char* kManageMenu = "Manage";
	/// The tag view's submenu for a namespace's values past the first `kTagViewValues`.
	static constexpr const char* kMoreSection = "More…";
	/// A list row's id — an entry, or one of the `kMenu*`.
	void chooseFromMenu(int id) { choose(id); }
	/// Delete ASKS: the menu's Delete arms this and shows "Delete X / Cancel"; the answer lands here.
	void resolveDelete(bool confirmed);
	[[nodiscard]] int pendingDelete() const noexcept { return pendingDelete_; }

	// Open the preset list. Public so the header bar can call it when the name box is clicked.
	void showPresetMenu() { showList(); }

	static constexpr float kHeight = 26.0f;      // same box height as the engine selector
	static constexpr float kNameWidth = 160.0f;  // the name panel
	static constexpr float kArrowWidth = 24.0f;  // each stepper
	// WHERE THE BROWSER SITS INSIDE THE HEADER'S STRIP, and the ONE place that says so — read from here
	// by the editor's layout rather than written twice.
	static constexpr float kLeftOffset = 195.0f;
	static constexpr float kTopOffset = (52.0f - kHeight) * 0.5f; ///< HeaderBar::kHeight, centred

	void draw(visage::Canvas& canvas) override;

protected:
	/// Taken out of the tree (a panel dismissed): the prompt, which lives in the window's top frame, goes.
	void hierarchyChanged() override;

private:
	// ItemSource
	size_t itemCount() const override { return entries_.size(); }
	std::string itemLabel(size_t index) const override;
	int currentItem() const override { return index_; }
	bool selectItem(size_t index) override { return selectIndex(static_cast<int>(index)); }
	bool itemReadOnly(size_t index) const override { return isReadOnly(static_cast<int>(index)); }
	std::vector<std::vector<std::string>> itemSectionPaths(size_t index) const override;
	double sectionOrder(const std::vector<std::string>& path) const override;
	void itemsWillList() const override;
	std::string itemNote() const override;
	std::vector<ItemAction> itemActions() const override;
	void itemAct(int id) override;

	/// The root moved under this browser: drop what indexes the old list, then read the new one.
	void relistForNewRoot();
	void openPrompt(PromptKind kind, const std::string& name, const std::string& tags);
	void closePrompt();
	void showReplaceMenu();
	void showDeleteMenu();
	/// "Clear N variants of \"X\" / Cancel", hung off the chips — Delete's confirmation, for the chips' one destructive verb.
	void showClearVariantsMenu();
	/// A view row chosen: file the list the new way, say so, and reopen it.
	void chooseView(View view);
	int64_t now() const;
	[[nodiscard]] std::string typedName() const;
	[[nodiscard]] std::vector<std::string> typedTags() const;
	[[nodiscard]] std::string tagBeingTyped() const;
	/// A completion or chip taken: `tag` replaces what is being typed. A namespace ("mood:") is left open
	/// for its value; a whole tag is closed with ", ".
	void takeTag(std::vector<std::string> present, const std::string& tag);
	/// Put `e` in its sorted place and make it current; its index.
	int insertSorted(Entry e);
	/// Steps 3 and 4 of `importFile`, for the pending import, under `name`; `replace` the entry it replaces.
	ImportOutcome finishImport(const std::string& name, int replace);
	void showImportMenu(int taken);
	void say(const std::string& text);

	PresetProvider* provider_ = nullptr;
	Notifier* notifier_ = nullptr;
	std::function<void(int)> onSelected_;

	std::vector<std::string> factoryNames_;
	std::vector<PresetData> factoryPresets_;
	std::vector<Entry> entries_;
	int index_ = 0;
	PresetBank bank_;
	View view_ = View::Folders;
	/// THE TAG VIEW'S NUMBERS FOR THE LISTING IN PROGRESS: namespace → value → (its rank by count, its
	/// label "pad (14)"). Filled by `itemsWillList`, read by `itemSectionPaths` and `sectionOrder`.
	mutable std::map<std::string, std::map<std::string, std::pair<int, std::string>>> tagView_;

	std::unique_ptr<PresetPrompt> prompt_ui_;
	/// THE KEYBOARD, WHILE THE PROMPT IS UP — its Name and Tags are typed into, and a keystroke that
	/// reached the DAW instead started its transport (keyboard_claim.h). Taken in `openPrompt`, let go in
	/// `closePrompt` and in the destructor. Declared after the prompt, so it goes first.
	std::unique_ptr<KeyboardClaim> promptClaim_;
	/// THE PROMPT'S ENTRY ON THE EDITOR'S ESC STACK while it is up (backlog NN) — Esc cancels it as the field's own
	/// Esc does, in the order it opened among everything else.
	EscapeTicket promptEscape_;
	Prompt prompt_ = Prompt::Closed;
	PromptKind promptKind_ = PromptKind::Save;
	int editing_ = -1;     ///< the entry `Edit` is over
	int replaceSlot_ = -1; ///< the entry `ConfirmReplace` is asking about
	int pendingDelete_ = -1;
	/// A "Clear variants 2–8" question is open (`resolveClearVariants`).
	bool clearPending_ = false;
	std::string notice_;
	/// `lastTouched`'s entry, BY NAME AND CATEGORY — an index moves under every insert, sort and delete.
	bool touched_ = false;
	std::string touchedName_;
	std::string touchedCategory_;
	void touch(int index);
	/// THE ONE WRITER of the three above: sets them, and tells `onCurrentChanged` when what they name changed.
	void setTouched(bool touched, std::string name, std::string category);
	void drawVariantChips(visage::Canvas& canvas);

	/// A file read and decoded whose name is taken, waiting for "Replace / Keep both / Cancel".
	struct PendingImport {
		std::string name;
		std::string category; ///< the folder it goes into: its metadata's category
		PresetMeta meta;
		std::vector<uint8_t> payload;
		int taken = -1;
	};
	std::unique_ptr<PendingImport> pendingImport_;
	/// THE OS DIALOG, A MEMBER: destroyed with the browser, which cancels it, so its answer can never
	/// reach a browser a dismissed panel took with it (`file_dialog.h`).
	FileDialog fileDialog_;

	/// **WHY A TIMER AND NOT A CALL.** The view row's choice arrives inside the closing menu's
	/// `optionSelected`, which calls `exit()` on that menu AFTER the callback returns — so a list opened
	/// from inside it opens while the old one is still up and still holds the keyboard focus it took (not
	/// shown to misbehave; not worth finding out). One event-loop turn later the old menu has exited and
	/// the new one opens on its own. A member, so it dies with the browser — a
	/// posted `runOnEventThread` closure would outlive a dismissed panel and call into freed memory.
	/// LAST, so it is destroyed first.
	visage::EventTimer reopen_;

	friend class PresetPrompt;
};

/// **WHO KEEPS THE CHOSEN ROOT.** A browser has no prefs — a device panel's has no road to any — so the
/// editors that own prefs (Feathers' `Gui`, the rack's) each hold one of these, and every
/// `PresetBrowser::setPresetRoot` tells every watch alive, whichever browser the hand was in. Registered
/// for exactly its own lifetime. MESSAGE THREAD.
class PresetRootWatch {
public:
	explicit PresetRootWatch(std::function<void(const std::filesystem::path&)> changed);
	~PresetRootWatch();
	PresetRootWatch(const PresetRootWatch&) = delete;
	PresetRootWatch& operator=(const PresetRootWatch&) = delete;

private:
	friend class PresetBrowser;
	std::function<void(const std::filesystem::path&)> changed_;
};

/// **THE CHOSEN ROOT, KEPT IN AN EDITOR'S PREFS** — the two halves every editor with prefs does, written
/// once. `restorePresetRoot` at startup, before any browser is built (every browser already alive in
/// the process re-lists if it differs); `keepPresetRootIn` for the editor's lifetime, writing
/// `kRootPrefKey` on every change. The path is stored as UTF-8.
void restorePresetRoot(const EditorPrefs& prefs);
[[nodiscard]] std::unique_ptr<PresetRootWatch> keepPresetRootIn(EditorPrefs& prefs);

/// **BIND A BROWSER'S FACE TO A STORED NAME** (backlog QC) — the face is put back on `read()` now (`restoreCurrent`, which
/// tells nobody), and every later change of the preset last saved or loaded goes to `write`. What a device panel does with
/// its device's `DeviceHost::devicePreset`, and the rack plugin with its bridge's `topPreset`: a browser lives only while
/// its window does, and the name is the patch's.
void bindPresetIdentity(PresetBrowser& browser, const std::function<PresetIdentity()>& read,
                        std::function<void(const PresetIdentity&)> write);

} // namespace feathers::ui
