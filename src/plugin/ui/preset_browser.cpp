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

#include "plugin/ui/widget_registry.h"
#include "plugin/ui/menu_anchor.h"
#include "plugin/ui/surface_layers.h"
#include "plugin/ui/preset_browser.h"

#include "plugin/editor_prefs.h"
#include "plugin/preset_store.h"
#include "plugin/ui/notify_tray.h"
#include "plugin/ui/theme.h"

#include <visage/ui.h>
#include <visage/widgets.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <string>

namespace feathers::ui {

namespace {

/// A preset file's extension, without its dot — the store's own (`preset_store.cpp`), as a dialog filter wants it.
constexpr const char* kPresetExtension = "fpreset";

/// The two rows of the "already exists" menu, and of the delete confirmation.
constexpr int kYes = 1;
constexpr int kNo = 2;

/// **EVERY BROWSER AND EVERY ROOT WATCH ALIVE IN THIS PROCESS** — what `setPresetRoot` re-lists and
/// tells. Each registers for exactly its own lifetime, so nothing here outlives what it points at.
/// MESSAGE THREAD, like every browser.
std::vector<PresetBrowser*>& liveBrowsers() {
	static std::vector<PresetBrowser*> live;
	return live;
}

std::vector<PresetRootWatch*>& rootWatches() {
	static std::vector<PresetRootWatch*> watches;
	return watches;
}

template <typename T>
void unregister(std::vector<T*>& list, T* item) {
	list.erase(std::remove(list.begin(), list.end(), item), list.end());
}

} // namespace

PresetRootWatch::PresetRootWatch(std::function<void(const std::filesystem::path&)> changed)
    : changed_(std::move(changed)) {
	rootWatches().push_back(this);
}

PresetRootWatch::~PresetRootWatch() {
	unregister(rootWatches(), this);
}

//------------------------------------------------------------------------------------------------//
// THE PROMPT'S PIECES                                                                              //
//------------------------------------------------------------------------------------------------//

/// **A FIELD THAT IS DEAF TO NOTHING** — visage's own `TextEditor`, single line. Every key it is handed
/// is reported as used, so none bubbles to a parent that would act on it; Tab is its own verb (`onTab`)
/// rather than a character.
class PresetField final : public visage::TextEditor {
public:
	PresetField() : visage::TextEditor("preset-field") {
		setTextFieldEntry();
		setBackgroundRounding(0.0f);
		setMargin(6.0f, 0.0f);
	}
	std::function<void()> onTab;
	bool keyPress(const visage::KeyEvent& key) override {
		if (key.keyCode() == visage::KeyCode::Tab) {
			if (onTab) {
				onTab();
			}
			return true;
		}
		(void)visage::TextEditor::keyPress(key);
		return true;
	}
	bool keyRelease(const visage::KeyEvent& key) override {
		(void)visage::TextEditor::keyRelease(key);
		return true;
	}

	/// **THE I-BEAM IS THIS FIELD'S TO TAKE BACK, AND ONLY WHILE IT IS STILL IN THE TREE** (backlog MF).
	/// visage's `TextEditor` sets the I-beam on enter and the arrow on exit, and on X11 that is the cursor
	/// of the WHOLE window. A hovered frame that leaves the tree gets no exit — `WindowEventHandler::
	/// giveUpFocus` just forgets it — so the I-beam stayed on the editor after a save with the pointer
	/// resting on the field. After removal `eraseChild` has taken the field's event handler, so whoever
	/// takes the prompt out calls this FIRST. The pointer elsewhere: nothing is said, the arrow is not ours.
	void mouseEnter(const visage::MouseEvent& e) override {
		hovered_ = true;
		visage::TextEditor::mouseEnter(e);
	}
	void mouseExit(const visage::MouseEvent& e) override {
		hovered_ = false;
		visage::TextEditor::mouseExit(e);
	}
	void releaseCursor() {
		if (hovered_) {
			hovered_ = false;
			setCursorStyle(visage::MouseCursor::Arrow);
		}
	}

private:
	bool hovered_ = false;
};

/// **THE PROMPT FIELDS' PALETTE — ONE, FOR THE LIFE OF THE PROCESS, NEVER A BROWSER'S MEMBER.**
///
/// visage's `Frame::drawToRegion` hands a frame's palette to the CANVAS and nothing ever takes it back:
/// a frame without a palette of its own (every visage popup) reads whatever palette the canvas was given
/// last. So a palette drawn once is read by the next popup that paints, long after its frame has gone.
/// When this was a member of the prompt, closing a panel whose browser had shown its prompt (FEATHERVERB's,
/// SKLITTER's) freed the palette the canvas still held, and the next menu SEGV'd in `Palette::color`
/// under `PopupList::draw` — seen in Bitwig, 2026-09-26. **Any frame here that is given a palette must be
/// given one that outlives every canvas**, which only a process-lifetime one does.
static visage::Palette& promptPalette() {
	static visage::Palette palette = [] {
		visage::Palette p;
		p.setColor(visage::TextEditor::TextEditorBackground, visage::Color(kVoid));
		p.setColor(visage::TextEditor::TextEditorBorder, visage::Color(kLine));
		p.setColor(visage::TextEditor::TextEditorText, visage::Color(kInk));
		p.setColor(visage::TextEditor::TextEditorDefaultText, visage::Color(kDisabled));
		p.setColor(visage::TextEditor::TextEditorCaret, visage::Color(kAccent));
		p.setColor(visage::TextEditor::TextEditorSelection, visage::Color(withAlpha(kAccent, 0.35f)));
		return p;
	}();
	return palette;
}

/// **THE PROMPT PANEL** — Name, Tags, chips and a notice line. Drawn and laid out here; every decision
/// is the browser's. It lives in the window's top frame while open (`PresetBrowser::openPrompt`).
class PresetPrompt final : public visage::Frame {
public:
	static constexpr float kPad = 8.0f;
	static constexpr float kLabel = 40.0f;
	static constexpr float kRow = 20.0f;
	static constexpr float kGap = 6.0f;
	static constexpr float kNotice = 14.0f;
	static constexpr float kWidth = 300.0f;
	static constexpr float kHeight = kPad + 3.0f * kRow + 2.0f * kGap + 4.0f + kNotice + kPad;
	static constexpr float kChipGap = 4.0f;
	static constexpr float kChipPad = 7.0f;

	explicit PresetPrompt(PresetBrowser& owner) : owner_(owner) {
		// **OUR PALETTE, NOT VISAGE'S DEFAULT GREY** — the TextEditor reads every colour it paints from
		// theme ids, so the fields get a palette of their own that maps those ids onto the mock's tokens:
		// a well darker than the panel, the hairline edge, bone ink, the dim placeholder, the accent caret
		// and an accent-tinted selection. Only the fields carry it (a child inherits its parent's palette
		// when added, and the prompt has none), so nothing else in the window changes colour.
		// `promptPalette()` says why it is not the prompt's own.
		for (PresetField* f : { &name, &tags }) {
			f->setFont(font(kControlSize));
			f->onEscapeKey() += [this] { owner_.cancelPrompt(); };
			f->onTextChange() += [this] { redraw(); };
			addChild(f);
			f->setPalette(&promptPalette());
		}
		name.onEnterKey() += [this] { owner_.submitPrompt(); };
		name.onTab = [this] { tags.requestKeyboardFocus(); };
		// ENTER IN THE TAGS FIELD TAKES A COMPLETION FIRST, and saves only when there is none to take.
		tags.onEnterKey() += [this] {
			if (!owner_.acceptCompletion()) {
				owner_.submitPrompt();
			}
		};
		tags.onTab = [this] { (void)owner_.acceptCompletion(); };
		tags.setDefaultText("tags, comma separated");
		setAcceptsKeystrokes(true);
		onMouseDown() = [this](const visage::MouseEvent& e) {
			const std::vector<std::string> chips = owner_.chips();
			for (size_t i = 0; i < chips.size(); ++i) {
				if (chipBounds(chips, i).contains(e.position)) {
					owner_.pressChip(i);
					// BACK TO TYPING TAGS: the click focused the panel, which holds no caret.
					tags.requestKeyboardFocus();
					return;
				}
			}
		};
	}

	/// **ENTER SAVES AND ESC CANCELS WHEREVER IN THE PROMPT THE FOCUS IS** (Enter saves whenever the
	/// preset dialog is open, even with the cursor outside a text box, as after clicking a tag chip). A click on the panel (a chip, the gap between rows) focuses the PANEL, and the panel hangs
	/// in the window's top frame beside the editor's root, so a key it declined went to no root handler and
	/// on to the DAW. Every other key is the prompt's too while it is up — and so is every key's TEXT
	/// (`receivesTextInput`): visage forwards a key it cannot name to the host unless a text entry is active.
	bool keyPress(const visage::KeyEvent& key) override {
		const visage::KeyCode code = key.keyCode();
		if (code == visage::KeyCode::Return || code == visage::KeyCode::KPEnter || code == visage::KeyCode::Return2) {
			owner_.submitPrompt();
		} else if (code == visage::KeyCode::Escape) {
			owner_.cancelPrompt();
		}
		return true;
	}
	bool keyRelease(const visage::KeyEvent&) override { return true; }
	bool receivesTextInput() override { return true; }
	void textInput(const std::string&) override {}

	void resized() override {
		const float fieldW = width() - 2.0f * kPad - kLabel;
		name.setBounds(kPad + kLabel, kPad, fieldW, kRow);
		tags.setBounds(kPad + kLabel, kPad + kRow + kGap, fieldW, kRow);
	}

	visage::Bounds chipBounds(const std::vector<std::string>& chips, size_t index) const {
		float x = kPad + kLabel;
		const float y = kPad + 2.0f * (kRow + kGap);
		const visage::Font& f = font(kControlSize);
		for (size_t i = 0; i < chips.size(); ++i) {
			const float w = trackedWidth(f, chips[i], 0.0f) + 2.0f * kChipPad;
			if (i == index) {
				return { x, y, w, kRow };
			}
			x += w + kChipGap;
		}
		return {};
	}

	void draw(visage::Canvas& canvas) override {
		canvas.setColor(kLine);
		canvas.fill(0.0f, 0.0f, width(), height());
		canvas.setColor(kSurfaceRaised);
		canvas.fill(1.0f, 1.0f, width() - 2.0f, height() - 2.0f);
		const visage::Font& f = font(kControlSize);
		canvas.setColor(kMuted);
		canvas.text(owner_.promptKind() == PresetBrowser::PromptKind::Save ? "Name" : "Rename", f,
		            visage::Font::kLeft, kPad, kPad, kLabel, kRow);
		canvas.text("Tags", f, visage::Font::kLeft, kPad, kPad + kRow + kGap, kLabel, kRow);
		const std::vector<std::string> chips = owner_.chips();
		for (size_t i = 0; i < chips.size(); ++i) {
			const visage::Bounds b = chipBounds(chips, i);
			if (b.right() > width() - kPad) {
				break; // a chip that does not fit is not drawn, and not pressable either (chips() order)
			}
			canvas.setColor(withAlpha(kAccent, 0.18f));
			canvas.roundedRectangle(b.x(), b.y(), b.width(), b.height(), 4.0f);
			canvas.setColor(kInk);
			canvas.text(chips[i], f, visage::Font::kCenter, b.x(), b.y(), b.width(), b.height());
		}
		const std::string& notice = owner_.notice();
		canvas.setColor(notice.empty() ? kDisabled : kModulation);
		canvas.text(notice.empty() ? "Enter saves · Esc cancels · Tab completes" : notice, f, visage::Font::kLeft,
		            kPad, height() - kPad - kNotice, width() - 2.0f * kPad, kNotice);
	}

	/// Out of the tree WITH THE ARROW BACK if a field was under the pointer (`PresetField::releaseCursor`).
	/// Every road that takes the prompt out goes through here.
	void takeOutOfTree() {
		name.releaseCursor();
		tags.releaseCursor();
		if (parent() != nullptr) {
			parent()->removeChild(this);
		}
	}

	PresetField name;
	PresetField tags;

private:
	PresetBrowser& owner_;
};

//------------------------------------------------------------------------------------------------//
// THE BROWSER                                                                                      //
//------------------------------------------------------------------------------------------------//

PresetBrowser::PresetBrowser([[maybe_unused]] WidgetRegistry& registry, std::vector<std::string> factoryNames,
                             std::vector<PresetData> factoryPresets)
    : PresetBrowser(std::move(factoryNames), std::move(factoryPresets), kPluginBank, kHeaderStyle) {}

PresetBrowser::PresetBrowser(std::vector<std::string> factoryNames, std::vector<PresetData> factoryPresets,
                             PresetBank bank, Style style)
    : ItemStepper(*this, style),
      factoryNames_(std::move(factoryNames)),
      factoryPresets_(std::move(factoryPresets)),
      bank_(std::move(bank)) {
	liveBrowsers().push_back(this);
	// UNTIL THE DISK IS READ, the factory bank is what there is — in memory, read-only.
	for (size_t i = 0; i < factoryNames_.size(); ++i) {
		entries_.push_back({ factoryNames_[i], {}, {}, i < factoryPresets_.size() ? factoryPresets_[i] : PresetData {}, {} });
	}
	prompt_ui_ = std::make_unique<PresetPrompt>(*this);
	prompt_ui_->setOnTop(true);
	// THE STEPPER'S OWN MOUSE, AND THE VARIANT CHIPS RIGHT OF IT: a chip's popup trigger is its menu (right-click, or
	// ctrl-click on macOS — visage knows which), its click saves or loads.
	onMouseDown() = [this](const visage::MouseEvent& e) {
		if (const int chip = variantChipAt(e.position); chip > 0) {
			if (e.shouldTriggerPopup()) {
				showVariantMenu(chip);
			}
			else {
				(void)pressVariant(chip);
			}
			return;
		}
		if (hitsPrev(e.position)) {
			step(-1);
		}
		else if (hitsNext(e.position)) {
			step(1);
		}
		else if (hitsName(e.position)) {
			showList();
		}
	};
	reopen_.onTimerCallback().set([this] {
		reopen_.stopTimer(); // one shot
		// A REOPEN REPLACES, IT IS NOT A RE-CLICK. `showMenuAt` closes a menu its own box asks for again
		// (backlog GG's toggle); this is the browser asking, so any list still open from this box is put
		// away first and the new one is opened rather than toggled shut.
		dismissMenuOf(*this);
		showList();
	});
}

PresetBrowser::~PresetBrowser() {
	unregister(liveBrowsers(), this);
	// DESTROYED WITH THE PROMPT UP, THE KEYBOARD STILL GOES BACK — the panel closed under it, the editor
	// torn down. Nothing else would give it back.
	promptClaim_.reset();
	// THE PROMPT OUT OF WHATEVER TOP FRAME HOLDS IT, before the member that owns it goes (and the arrow back if
	// a field was hovered). `takeOutOfTree` is guarded on its parent: a top frame torn down first has
	// already let go of it (`Frame::eraseChild`).
	if (prompt_ui_ != nullptr) {
		prompt_ui_->takeOutOfTree();
	}
}

int64_t PresetBrowser::now() const {
	if (clock) {
		return clock();
	}
	return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

void PresetBrowser::loadBankFromDisk() {
	const std::string wasName = index_ >= 0 && index_ < static_cast<int>(entries_.size()) ? entries_[index_].name : "";
	const std::string wasCategory =
	    index_ >= 0 && index_ < static_cast<int>(entries_.size()) ? entries_[index_].category : "";
	// FACTORY PRESETS ARE FILES: copied in when missing, never over a copy the user has edited.
	// The ones the plugin carries from `presets/factory/` first, so an authored file wins over a
	// computed entry of the same name; then the computed ones ("Init").
	(void)PresetStore::installFactoryFiles(bank_, factoryFiles);
	for (size_t i = 0; i < factoryNames_.size() && i < factoryPresets_.size(); ++i) {
		(void)PresetStore::installIfMissing(factoryNames_[i], factoryPresets_[i].bytes, bank_);
	}
	entries_.clear();
	for (StoredPreset& stored : PresetStore::loadAll(bank_)) {
		if (accepts && !accepts(stored.bytes)) {
			continue; // ANOTHER KIND'S FILE, in this kind's folder: not this browser's to offer
		}
		entries_.push_back({ std::move(stored.name), std::move(stored.category), std::move(stored.meta),
		                     PresetData { std::move(stored.bytes) }, std::move(stored.path) });
	}
	// A FACTORY PRESET THAT COULD NOT BE WRITTEN (no root, a read-only disk) IS STILL OFFERED, from
	// memory and read-only — "Init" must exist whatever the filesystem says.
	for (size_t i = 0; i < factoryNames_.size(); ++i) {
		if (indexOf(factoryNames_[i]) < 0 && i < factoryPresets_.size() && !factoryPresets_[i].bytes.empty()) {
			entries_.insert(entries_.begin() + static_cast<std::ptrdiff_t>(std::min(i, entries_.size())),
			                Entry { factoryNames_[i], {}, {}, factoryPresets_[i], {} });
		}
	}
	// The selection follows its entry; nothing is applied — opening an editor must not change the sound.
	const int found = indexOf(wasName, wasCategory);
	index_ = found >= 0 ? found : (entries_.empty() ? -1 : 0);
	redraw();
}

int PresetBrowser::indexOf(const std::string& name, const std::string& category) const {
	for (size_t i = 0; i < entries_.size(); ++i) {
		if (entries_[i].name == name && entries_[i].category == category) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

const std::string& PresetBrowser::currentName() const {
	static const std::string kNone;
	if (index_ < 0 || index_ >= static_cast<int>(entries_.size())) {
		return kNone;
	}
	return entries_[static_cast<size_t>(index_)].name;
}

void PresetBrowser::markCurrent(int index) {
	const int at = index >= 0 && index < static_cast<int>(entries_.size()) ? index : -1;
	// AN UNDO PUT THIS PATCH BACK: it is the one last loaded. One not in the bank leaves nothing named.
	if (at >= 0) {
		touch(at);
	}
	else {
		setTouched(false, touchedName_, touchedCategory_);
	}
	if (at != index_) {
		index_ = at;
		redraw();
	}
}

bool PresetBrowser::selectIndex(int index) {
	if (index < 0 || index >= static_cast<int>(entries_.size())) {
		return false;
	}
	// PICKING THE PRESET YOU ARE ALREADY ON IS A LOAD: you change something and pick it again to get it
	// back. And A PRESET THAT DID NOT LOAD DOES NOT BECOME CURRENT — the face would name a sound that is
	// not playing — so the selection moves only once the provider has taken it.
	if (provider_ != nullptr && !provider_->apply(entries_[static_cast<size_t>(index)].data)) {
		return false;
	}
	if (index != index_) {
		index_ = index;
		redraw();
	}
	touch(index_);
	if (onSelected_) {
		onSelected_(index_);
	}
	return true;
}

bool PresetBrowser::isReadOnly(int index) const {
	return index < 0 || index >= static_cast<int>(entries_.size()) || !entries_[static_cast<size_t>(index)].onDisk();
}

std::string PresetBrowser::suggestedName() const {
	for (int n = 1;; ++n) {
		const std::string candidate = "User " + std::to_string(n);
		if (indexOf(candidate) < 0) {
			return candidate;
		}
	}
}

bool PresetBrowser::saveAs(const std::string& typed, const std::vector<std::string>& tags) {
	const auto [name, category] = PresetStore::splitTyped(typed);
	if (name.empty() || indexOf(name, category) >= 0 || provider_ == nullptr) {
		return false;
	}
	PresetData data;
	if (!provider_->capture(data) || data.bytes.empty()) {
		return false;
	}
	const PresetMeta meta { tags, now(), category };
	// TO DISK FIRST, BUT NOT CONDITIONALLY: a store that cannot write must not cost the sound somebody
	// just made, so the entry is kept in memory either way — read-only, since it has no file.
	const bool wrote = PresetStore::save(name, data.bytes, bank_, category, meta);
	touch(insertSorted({ name, category, meta, std::move(data), wrote ? PresetStore::pathFor(name, bank_, category) : std::filesystem::path {} }));
	return true;
}

int PresetBrowser::insertSorted(Entry e) {
	// IN ITS SORTED PLACE, so the list reads the same now as after a reload.
	const auto at = std::lower_bound(entries_.begin(), entries_.end(), e, [](const Entry& a, const Entry& b) {
		return a.category != b.category ? a.category < b.category : a.name < b.name;
	});
	// TWO STATEMENTS, AND IT MATTERS: `insert(...) - begin()` in one expression may take `begin()` BEFORE
	// the insert reallocates — measured: an index of 2706 into a bank of two.
	const auto inserted = entries_.insert(at, std::move(e));
	index_ = static_cast<int>(inserted - entries_.begin());
	redraw();
	return index_;
}

bool PresetBrowser::overwriteCurrent(int index) {
	if (isReadOnly(index) || provider_ == nullptr) {
		return false;
	}
	PresetData data;
	if (!provider_->capture(data) || data.bytes.empty()) {
		return false;
	}
	Entry& e = entries_[static_cast<size_t>(index)];
	e.meta.saved = now();
	// THE FOLDER IT IS IN, WRITTEN INTO ITS METADATA — so a file moved by hand gets metadata that agrees.
	e.meta.category = e.category;
	PresetStore::save(e.name, data.bytes, bank_, e.category, e.meta);
	e.data = std::move(data);
	touch(index);
	return true;
}

void PresetBrowser::touch(int index) {
	if (index >= 0 && index < static_cast<int>(entries_.size())) {
		setTouched(true, entries_[static_cast<size_t>(index)].name, entries_[static_cast<size_t>(index)].category);
	}
	else {
		setTouched(false, touchedName_, touchedCategory_);
	}
}

void PresetBrowser::setTouched(bool touched, std::string name, std::string category) {
	const bool wasNamed = touched_;
	const std::string wasName = touchedName_;
	const std::string wasCategory = touchedCategory_;
	touched_ = touched;
	touchedName_ = std::move(name);
	touchedCategory_ = std::move(category);
	// WHAT IT NAMES, not what the fields hold: an untouched browser names nothing whatever its stale strings say.
	const bool changed = touched_ != wasNamed || (touched_ && (touchedName_ != wasName || touchedCategory_ != wasCategory));
	if (changed && onCurrentChanged) {
		onCurrentChanged(touched_ ? touchedName_ : std::string {}, touched_ ? touchedCategory_ : std::string {});
	}
}

bool PresetBrowser::restoreCurrent(const std::string& name, const std::string& category) {
	if (name.empty()) {
		return false;
	}
	const int at = indexOf(name, category);
	markCurrent(at);
	return at >= 0;
}

int PresetBrowser::lastTouched() const {
	return touched_ ? indexOf(touchedName_, touchedCategory_) : -1;
}

std::string PresetBrowser::variantName(int index) const {
	if (index < 0 || index >= static_cast<int>(entries_.size())) {
		return {};
	}
	const Entry& e = entries_[static_cast<size_t>(index)];
	// "Pad 2" IS A VARIANT ALREADY: its base is "Pad" and the count goes on from 2, so a variant of a
	// variant is "Pad 3", never "Pad 2 2". Only a SPACE then digits is a suffix — "TB303" is a name.
	std::string base = e.name;
	int from = 2;
	const size_t space = base.find_last_of(' ');
	if (space != std::string::npos && space > 0 && space + 1 < base.size() && space + 1 + 6 >= base.size()
	    && std::all_of(base.begin() + static_cast<std::ptrdiff_t>(space) + 1, base.end(),
	                   [](char c) { return c >= '0' && c <= '9'; })) {
		from = std::max(2, std::stoi(base.substr(space + 1)) + 1);
		base.resize(space);
	}
	for (int n = from;; ++n) {
		const std::string candidate = base + " " + std::to_string(n);
		if (indexOf(candidate, e.category) < 0) {
			return candidate;
		}
	}
}

bool PresetBrowser::saveVariant(int index) {
	const std::string name = variantName(index);
	if (name.empty()) {
		return false;
	}
	const Entry& e = entries_[static_cast<size_t>(index)];
	const std::string typed = e.category.empty() ? name : e.category + "/" + name;
	const std::vector<std::string> tags = e.meta.tags; // a copy: `saveAs` inserts, which may move `e`
	return saveAs(typed, tags);
}

bool PresetBrowser::isFactory(int index) const {
	if (index < 0 || index >= static_cast<int>(entries_.size())) {
		return false;
	}
	const Entry& e = entries_[static_cast<size_t>(index)];
	if (e.category.empty() && std::find(factoryNames_.begin(), factoryNames_.end(), e.name) != factoryNames_.end()) {
		return true;
	}
	if (!e.onDisk()) {
		return false;
	}
	const std::filesystem::path relative = e.path.lexically_relative(PresetStore::root());
	return std::any_of(factoryFiles.begin(), factoryFiles.end(), [&relative](const FactoryPresetFile& f) {
		return std::filesystem::path(std::string(f.path)) == relative;
	});
}

//------------------------------------------------------------------------------------------------//
// THE VARIANT CHIPS                                                                                //
//------------------------------------------------------------------------------------------------//

namespace {
constexpr std::string_view kVariantInfix = "_variant_";
} // namespace

std::string PresetBrowser::variantFileName(const std::string& base, int n) {
	// THE ROOT IS VARIANT 1: chip 1 is the preset's own file.
	return n == 1 ? base : base + std::string(kVariantInfix) + std::to_string(n);
}

std::string PresetBrowser::variantBase(const std::string& name) {
	const size_t at = name.rfind(kVariantInfix);
	if (at == std::string::npos || at == 0) {
		return name;
	}
	const std::string digits = name.substr(at + kVariantInfix.size());
	// 2..8 ONLY: "_variant_1" is the old scheme's name for what is now the root, so such a file is an ordinary preset.
	if (digits.size() != 1 || digits[0] < '2' || digits[0] > '0' + kVariants) {
		return name;
	}
	return name.substr(0, at);
}

bool PresetBrowser::itemListed(size_t index) const {
	return index >= entries_.size() || !isVariantFile(entries_[index].name);
}

int PresetBrowser::variantIndex(int n) const {
	if (n < 1 || n > kVariants || index_ < 0 || index_ >= static_cast<int>(entries_.size())) {
		return -1;
	}
	const Entry& shown = entries_[static_cast<size_t>(index_)];
	return indexOf(variantFileName(variantBase(shown.name), n), shown.category);
}

PresetBrowser::ChipState PresetBrowser::chipState(int n) const {
	const int at = variantIndex(n);
	if (at < 0) {
		return ChipState::Unused;
	}
	return at == index_ ? ChipState::Active : ChipState::Saved;
}

bool PresetBrowser::pressVariant(int n) {
	if (n < 1 || n > kVariants || index_ < 0 || index_ >= static_cast<int>(entries_.size())) {
		return false;
	}
	return actVariant(n, variantIndex(n) >= 0 ? VariantVerb::Load : VariantVerb::Save);
}

std::vector<int> PresetBrowser::clearableVariants() const {
	std::vector<int> out;
	for (int n = 2; n <= kVariants; ++n) {
		// NEVER A FACTORY FILE — the factory guard (`Save "X"`'s) — and never the root, which is variant 1.
		if (const int at = variantIndex(n); at >= 0 && !isFactory(at) && !isReadOnly(at)) {
			out.push_back(at);
		}
	}
	return out;
}

std::vector<PresetBrowser::VariantVerb> PresetBrowser::variantVerbs(int n) const {
	if (n < 1 || n > kVariants || index_ < 0) {
		return {};
	}
	std::vector<VariantVerb> out;
	const int at = variantIndex(n);
	if (at < 0) {
		out = { VariantVerb::Save };
	}
	else if (n == 1) {
		// THE ROOT: loaded and overwritten like any variant — never over a factory file — but not deleted or renamed
		// from a chip: those are the preset's own verbs, in Manage.
		out = { VariantVerb::Load };
		if (!isFactory(at) && !isReadOnly(at)) {
			out.push_back(VariantVerb::Overwrite);
		}
	}
	else {
		out = { VariantVerb::Load, VariantVerb::Overwrite, VariantVerb::Promote, VariantVerb::Delete };
	}
	// ON EVERY CHIP'S MENU, while there is anything to clear.
	if (!clearableVariants().empty()) {
		out.push_back(VariantVerb::ClearOthers);
	}
	return out;
}

std::string PresetBrowser::variantVerbLabel(VariantVerb verb, int n) {
	const std::string number = std::to_string(n);
	switch (verb) {
	case VariantVerb::Save: return "Save variant " + number;
	case VariantVerb::Load: return "Load variant " + number;
	case VariantVerb::Overwrite: return "Overwrite variant " + number;
	case VariantVerb::Delete: return "Delete variant " + number;
	case VariantVerb::Promote: return "Rename to a full preset\xe2\x80\xa6";
	case VariantVerb::ClearOthers: return "Clear variants 2\xe2\x80\x93" + std::to_string(kVariants) + "\xe2\x80\xa6";
	}
	return {};
}

bool PresetBrowser::actVariant(int n, VariantVerb verb) {
	if (n < 1 || n > kVariants || index_ < 0 || index_ >= static_cast<int>(entries_.size())) {
		return false;
	}
	const int at = variantIndex(n);
	switch (verb) {
	case VariantVerb::Save: {
		if (at >= 0) {
			return false; // a saved chip is overwritten only by its menu's Overwrite, never by a click
		}
		const Entry& shown = entries_[static_cast<size_t>(index_)];
		const std::string base = variantBase(shown.name);
		const std::string category = shown.category;
		// THE BASE'S TAGS when the base is in the bank; the shown entry's otherwise (a variant whose base was removed).
		const int baseAt = indexOf(base, category);
		const std::vector<std::string> tags =
		    entries_[static_cast<size_t>(baseAt >= 0 ? baseAt : index_)].meta.tags; // a copy: the save inserts
		const std::string name = variantFileName(base, n);
		return saveAs(category.empty() ? name : category + "/" + name, tags);
	}
	case VariantVerb::Load: return at >= 0 && selectIndex(at);
	case VariantVerb::Overwrite:
		// THE FACTORY GUARD: chip 1 over a shipped root writes nothing (Save as… still does).
		if (at < 0 || isFactory(at) || !overwriteCurrent(at)) {
			return false;
		}
		// THE SOUND PLAYING IS NOW THAT FILE'S: it is what the name shows.
		index_ = at;
		redraw();
		return true;
	case VariantVerb::Delete: return n != 1 && at >= 0 && !isFactory(at) && deleteEntry(at);
	case VariantVerb::Promote:
		if (n == 1 || at < 0 || isReadOnly(at)) {
			return false;
		}
		beginEdit(at);
		return true;
	case VariantVerb::ClearOthers:
		if (clearableVariants().empty()) {
			return false;
		}
		clearPending_ = true;
		showClearVariantsMenu();
		return true;
	}
	return false;
}

void PresetBrowser::showClearVariantsMenu() {
	const size_t count = clearableVariants().size();
	const std::string base = index_ >= 0 ? variantBase(entries_[static_cast<size_t>(index_)].name) : std::string {};
	visage::PopupMenu menu;
	menu.addOption(kYes, "Clear " + std::to_string(count) + (count == 1 ? " variant" : " variants") + " of \"" + base + "\"");
	menu.addOption(kNo, "Cancel");
	menu.onSelection() = [this](int id) { resolveClearVariants(id == kYes); };
	menu.onCancel() = [this] { resolveClearVariants(false); };
	showMenuAt(std::move(menu), *this, variantChipBounds(1));
}

void PresetBrowser::resolveClearVariants(bool confirmed) {
	if (!clearPending_) {
		return;
	}
	clearPending_ = false;
	if (!confirmed || index_ < 0) {
		return;
	}
	// BY NAME, THEN REMOVED ONE AT A TIME: each delete re-sorts the bank, so an index taken before it is stale.
	const Entry& shown = entries_[static_cast<size_t>(index_)];
	const std::string base = variantBase(shown.name);
	const std::string category = shown.category;
	const bool showingVariant = shown.name != base;
	std::vector<std::string> names;
	for (const int at : clearableVariants()) {
		names.push_back(entries_[static_cast<size_t>(at)].name);
	}
	for (const std::string& name : names) {
		if (const int at = indexOf(name, category); at >= 0) {
			(void)deleteEntry(at);
		}
	}
	// SHOWING A VARIANT THAT IS NOW GONE: back on the root, which is variant 1 and stays.
	if (showingVariant) {
		if (const int root = indexOf(base, category); root >= 0) {
			index_ = root;
		}
	}
	redraw();
}

void PresetBrowser::showVariantMenu(int n) {
	const std::vector<VariantVerb> verbs = variantVerbs(n);
	if (verbs.empty()) {
		return;
	}
	visage::PopupMenu menu;
	for (const VariantVerb verb : verbs) {
		menu.addOption(static_cast<int>(verb), variantVerbLabel(verb, n));
	}
	menu.onSelection() = [this, n](int id) { (void)actVariant(n, static_cast<VariantVerb>(id)); };
	showMenuAt(std::move(menu), *this, variantChipBounds(n));
}

visage::Bounds PresetBrowser::variantChipBounds(int n) const {
	if (n < 1 || n > kVariants) {
		return {};
	}
	const Style& s = style();
	const float w = chipWidthFor(s);
	const float h = chipHeightFor(s);
	const int column = (n - 1) % 4;
	const int row = (n - 1) / 4;
	const float top = (static_cast<float>(height()) - s.height) * 0.5f;
	return { totalWidth() + kChipsGap + static_cast<float>(column) * (w + kChipGap),
		     top + static_cast<float>(row) * (h + kChipGap), w, h };
}

int PresetBrowser::variantChipAt(visage::Point position) const {
	for (int n = 1; n <= kVariants; ++n) {
		if (variantChipBounds(n).contains(position)) {
			return n;
		}
	}
	return 0;
}

void PresetBrowser::drawVariantChips(visage::Canvas& canvas) {
	const Style& s = style();
	const visage::Font& f = font(std::min(kControlSize, chipHeightFor(s) - 1.0f));
	const bool live = index_ >= 0;
	for (int n = 1; n <= kVariants; ++n) {
		const visage::Bounds b = variantChipBounds(n);
		const ChipState state = chipState(n);
		// GREY unused, the ALTERNATE colour saved, BRIGHT active: theme tokens all.
		uint32_t ground = kSurface;
		uint32_t ink = live ? kDisabled : withAlpha(kDisabled, 0.5f);
		if (state == ChipState::Saved) {
			ground = withAlpha(kValue, 0.45f);
			ink = kInk;
		}
		else if (state == ChipState::Active) {
			ground = kAccent;
			ink = kVoid;
		}
		canvas.setColor(state == ChipState::Unused ? kLine : ground);
		canvas.roundedRectangle(b.x(), b.y(), b.width(), b.height(), 2.0f);
		if (state == ChipState::Unused) {
			canvas.setColor(ground);
			canvas.roundedRectangle(b.x() + 1.0f, b.y() + 1.0f, b.width() - 2.0f, b.height() - 2.0f, 1.5f);
		}
		canvas.setColor(ink);
		canvas.text(std::to_string(n), f, visage::Font::kCenter, b.x(), b.y(), b.width(), b.height());
	}
}

bool PresetBrowser::editEntry(int index, const std::string& typed, const std::vector<std::string>& tags) {
	if (isReadOnly(index)) {
		return false;
	}
	const auto [name, category] = PresetStore::splitTyped(typed);
	if (name.empty()) {
		return false;
	}
	Entry& e = entries_[static_cast<size_t>(index)];
	const bool moves = name != e.name || category != e.category;
	// A RENAME OF THE PRESET LAST SAVED OR LOADED is still that preset: the name follows it.
	const bool wasTouched = touched_ && e.name == touchedName_ && e.category == touchedCategory_;
	// AND THE CURRENT ENTRY, BY NAME — the sort below moves every index, not only the edited one's.
	const std::string currentWasName = index_ >= 0 && index_ < static_cast<int>(entries_.size()) ? entries_[static_cast<size_t>(index_)].name : "";
	const std::string currentWasCategory = index_ >= 0 && index_ < static_cast<int>(entries_.size()) ? entries_[static_cast<size_t>(index_)].category : "";
	const bool editingCurrent = index_ == index;
	if (moves) {
		if (indexOf(name, category) >= 0) {
			return false; // a rename never overwrites
		}
		const std::filesystem::path to = PresetStore::pathFor(name, bank_, category);
		if (!PresetStore::rename(e.path, to)) {
			return false;
		}
		e.path = to;
		e.name = name;
		e.category = category;
	}
	e.meta.tags = tags;
	e.meta.category = e.category;
	// THE TAGS GO BACK INTO THE FILE WITH THE SAME PAYLOAD — an edit is not a save of the current sound.
	PresetStore::save(e.name, e.data.bytes, bank_, e.category, e.meta);
	if (moves) {
		const std::string keepName = e.name;
		const std::string keepCategory = e.category;
		std::sort(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) {
			return a.category != b.category ? a.category < b.category : a.name < b.name;
		});
		if (editingCurrent) {
			index_ = indexOf(keepName, keepCategory);
		}
		else if (index_ >= 0) {
			index_ = indexOf(currentWasName, currentWasCategory);
		}
		if (wasTouched) {
			setTouched(true, keepName, keepCategory); // FOLLOWED THROUGH THE RENAME, and the patch's copy with it
		}
	}
	redraw();
	return true;
}

bool PresetBrowser::deleteEntry(int index) {
	if (isReadOnly(index)) {
		return false;
	}
	if (!PresetStore::erase(entries_[static_cast<size_t>(index)].path)) {
		return false;
	}
	if (index == lastTouched()) {
		setTouched(false, touchedName_, touchedCategory_); // gone: the prompt has nothing to pre-fill, and `Save "X"` nothing to save over
	}
	entries_.erase(entries_.begin() + index);
	// THE CURRENT PRESET WENT, OR ONE BEFORE IT DID: the face says nothing is current, or follows its
	// entry down one. Nothing is applied either way — deleting a file does not change the sound.
	if (index_ == index) {
		index_ = -1;
	}
	else if (index_ > index) {
		--index_;
	}
	redraw();
	return true;
}

//------------------------------------------------------------------------------------------------//
// WHERE THE LIBRARY IS                                                                             //
//------------------------------------------------------------------------------------------------//

void PresetBrowser::beginChooseRoot() {
	FileDialogOptions options;
	options.mode = FileDialogOptions::Mode::OpenFolder;
	options.title = "Choose preset folder";
	options.startDir = PresetStore::root();
	options.parent = nativeWindowOf(this);
	(void)fileDialog_.open(options, [](const FileDialogResult& result) {
		if (result.chosen()) {
			(void)setPresetRoot(result.path);
		}
	});
}

size_t PresetBrowser::setPresetRoot(const std::filesystem::path& chosen) {
	if (chosen == PresetStore::chosenRoot()) {
		return 0;
	}
	PresetStore::setChosenRoot(chosen);
	// A COPY OF THE LIST: a re-list redraws, and nothing it does may add or remove a browser mid-walk —
	// but a copy costs nothing and makes that a non-question.
	const std::vector<PresetBrowser*> browsers = liveBrowsers();
	for (PresetBrowser* b : browsers) {
		b->relistForNewRoot();
	}
	const std::vector<PresetRootWatch*> watches = rootWatches();
	for (PresetRootWatch* w : watches) {
		if (w->changed_) {
			w->changed_(chosen);
		}
	}
	return browsers.size();
}

void restorePresetRoot(const EditorPrefs& prefs) {
	const std::string text = prefs.text(PresetBrowser::kRootPrefKey);
	(void)PresetBrowser::setPresetRoot(
	    std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size())));
}

std::unique_ptr<PresetRootWatch> keepPresetRootIn(EditorPrefs& prefs) {
	return std::make_unique<PresetRootWatch>([&prefs](const std::filesystem::path& root) {
		const std::u8string utf8 = root.u8string();
		prefs.setText(PresetBrowser::kRootPrefKey, std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size()));
	});
}

size_t PresetBrowser::liveCountForTest() {
	return liveBrowsers().size();
}

size_t PresetBrowser::closePromptsUnder(const visage::Frame& top) {
	size_t closed = 0;
	const std::vector<PresetBrowser*> browsers = liveBrowsers();
	for (PresetBrowser* b : browsers) {
		if (b->prompt_ == Prompt::Closed) {
			continue;
		}
		// UNDER, NOT "WHOSE ROOT IS": visage seats an `ApplicationWindow` inside a `TopLevelFrame` of its
		// own, so the editor's frame is not the top of its tree. MEASURED: the root-equality first draft
		// closed nothing in either editor.
		const visage::Frame* at = b;
		while (at != nullptr && at != &top) {
			at = at->parent();
		}
		if (at == nullptr) {
			continue;
		}
		b->closePrompt();
		++closed;
	}
	return closed;
}

void PresetBrowser::relistForNewRoot() {
	// EVERYTHING HELD BY INDEX INTO THE OLD LIST GOES — a prompt over an entry, a delete or an import
	// waiting for an answer — because those indices now name entries of a different library.
	if (prompt_ != Prompt::Closed) {
		closePrompt();
	}
	// AND "LAST SAVED OR LOADED" NAMES A FILE OF THE OLD LIBRARY: the same name in the new one is another
	// preset, and `Save "X"` would overwrite it.
	setTouched(false, touchedName_, touchedCategory_);
	pendingDelete_ = -1;
	pendingImport_.reset();
	loadBankFromDisk();
	forgetFailures();
}

//------------------------------------------------------------------------------------------------//
// IMPORT AND EXPORT                                                                                //
//------------------------------------------------------------------------------------------------//

void PresetBrowser::say(const std::string& text) {
	if (notifier_ != nullptr) {
		notifier_->say(text);
	}
}

void PresetBrowser::beginImport() {
	FileDialogOptions options;
	options.mode = FileDialogOptions::Mode::OpenFile;
	options.title = "Import preset";
	options.filters = { { "Feathers preset", { kPresetExtension } } };
	options.parent = nativeWindowOf(this);
	// THE ANSWER COMES LATER, ON THIS THREAD, FROM THE DIALOG'S OWN TIMER — and never after this browser
	// is gone, because the dialog is a member and cancels when it is destroyed.
	(void)fileDialog_.open(options, [this](const FileDialogResult& result) {
		if (result.chosen()) {
			(void)importFile(result.path);
		} else if (result.status == FileDialogResult::Status::Failed) {
			say("Import: " + result.error);
		}
	});
}

void PresetBrowser::beginExport() {
	FileDialogOptions options;
	options.mode = FileDialogOptions::Mode::SaveFile;
	options.title = "Export preset";
	options.filters = { { "Feathers preset", { kPresetExtension } } };
	options.defaultName = (currentName().empty() ? std::string("Preset") : currentName()) + "." + kPresetExtension;
	options.parent = nativeWindowOf(this);
	(void)fileDialog_.open(options, [this](const FileDialogResult& result) {
		if (result.chosen()) {
			(void)exportTo(result.path);
		} else if (result.status == FileDialogResult::Status::Failed) {
			say("Export: " + result.error);
		}
	});
}

PresetBrowser::ImportOutcome PresetBrowser::importFile(const std::filesystem::path& file) {
	pendingImport_.reset();
	const std::string shown = file.filename().string();
	// 1. READ AND DECODE — a file that is not there, is a folder, is huge or is not a preset file stops
	// here, before anything is asked, loaded or written.
	std::error_code ec;
	const std::uintmax_t size = std::filesystem::file_size(file, ec);
	if (ec || size == 0 || size > kMaxImportBytes) {
		say("Import failed — \"" + shown + "\" could not be read");
		return ImportOutcome::Unreadable;
	}
	std::vector<uint8_t> bytes(static_cast<size_t>(size));
	{
		std::ifstream in(file, std::ios::binary);
		if (!in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
			say("Import failed — \"" + shown + "\" could not be read");
			return ImportOutcome::Unreadable;
		}
	}
	auto pending = std::make_unique<PendingImport>();
	if (!PresetStore::decode(bytes, &pending->meta, &pending->payload) || pending->payload.empty()) {
		say("Import failed — \"" + shown + "\" is not a preset file");
		return ImportOutcome::Unreadable;
	}
	pending->name = PresetStore::sanitise(file.stem().string());
	if (pending->name.empty()) {
		pending->name = "Imported";
	}
	// ITS CATEGORY IS WHERE IT GOES: the metadata's `category:` (already sanitised by the store), its
	// folder created if missing. A file with none goes to the top of the bank.
	pending->category = pending->meta.category;
	// 2. A NAME ALREADY IN THAT FOLDER IS ASKED ABOUT FIRST — before the sound changes, so "Cancel" leaves
	// everything exactly as it was.
	const int taken = indexOf(pending->name, pending->category);
	const std::string name = pending->name;
	pendingImport_ = std::move(pending);
	if (taken >= 0) {
		pendingImport_->taken = taken;
		showImportMenu(taken);
		return ImportOutcome::Asking;
	}
	return finishImport(name, -1);
}

PresetBrowser::ImportOutcome PresetBrowser::resolveImport(ImportChoice choice) {
	if (pendingImport_ == nullptr) {
		return ImportOutcome::Unreadable;
	}
	const int taken = pendingImport_->taken;
	if (choice == ImportChoice::Replace && taken >= 0 && !isReadOnly(taken)) {
		return finishImport(pendingImport_->name, taken);
	}
	if (choice == ImportChoice::KeepBoth) {
		// "Name 2", or the first number free: a copy BESIDE it, never over it.
		for (int n = 2;; ++n) {
			const std::string candidate = pendingImport_->name + " " + std::to_string(n);
			if (indexOf(candidate, pendingImport_->category) < 0) {
				return finishImport(candidate, -1);
			}
		}
	}
	pendingImport_.reset();
	return ImportOutcome::Refused;
}

PresetBrowser::ImportOutcome PresetBrowser::finishImport(const std::string& name, int replace) {
	const std::unique_ptr<PendingImport> pending = std::move(pendingImport_);
	// 3. THE SAME CHECK A PICK MAKES: the provider takes the payload or it is not imported. It has said
	// why when it refuses; nothing has been written.
	PresetData data { pending->payload };
	if (provider_ == nullptr || !provider_->apply(data)) {
		return ImportOutcome::Refused;
	}
	// 4. INTO THE BANK, at the top, keeping the file's tags. A store that cannot write still keeps the
	// sound that is now playing in the list, read-only, as a save does.
	PresetMeta meta = pending->meta;
	if (meta.saved == 0) {
		meta.saved = now();
	}
	const std::string category = pending->category;
	meta.category = category;
	const bool wrote = PresetStore::save(name, data.bytes, bank_, category, meta);
	if (!wrote) {
		say("Imported \"" + name + "\", but it could not be saved into the bank");
	}
	if (replace >= 0) {
		Entry& e = entries_[static_cast<size_t>(replace)];
		e.meta = meta;
		e.data = std::move(data);
		index_ = replace;
		redraw();
	} else {
		(void)insertSorted({ name, category, meta, std::move(data),
		                     wrote ? PresetStore::pathFor(name, bank_, category) : std::filesystem::path {} });
	}
	touch(index_);
	if (onSelected_) {
		onSelected_(index_);
	}
	return ImportOutcome::Imported;
}

void PresetBrowser::showImportMenu(int taken) {
	constexpr int kReplace = 1;
	constexpr int kKeepBoth = 2;
	constexpr int kCancel = 3;
	const std::string& name = entries_[static_cast<size_t>(taken)].name;
	visage::PopupMenu menu;
	// ALL THREE ANSWERS ON THE FACE BEFORE ANY IS TAKEN; Replace only where there is a file to replace.
	if (!isReadOnly(taken)) {
		menu.addOption(kReplace, "Replace \"" + name + "\"");
	}
	menu.addOption(kKeepBoth, "Keep both");
	menu.addOption(kCancel, "Cancel");
	menu.onSelection() = [this](int id) {
		(void)resolveImport(id == kReplace ? ImportChoice::Replace
		                                   : (id == kKeepBoth ? ImportChoice::KeepBoth : ImportChoice::Cancel));
	};
	menu.onCancel() = [this] { (void)resolveImport(ImportChoice::Cancel); };
	showMenuAt(std::move(menu), *this, nameBounds());
}

bool PresetBrowser::exportTo(std::filesystem::path file) {
	if (file.empty() || provider_ == nullptr) {
		return false;
	}
	PresetData data;
	if (!provider_->capture(data) || data.bytes.empty()) {
		say("Export failed — nothing to save");
		return false;
	}
	PresetMeta meta;
	if (index_ >= 0 && index_ < static_cast<int>(entries_.size())) {
		meta.tags = entries_[static_cast<size_t>(index_)].meta.tags;
		meta.category = entries_[static_cast<size_t>(index_)].category; // it travels, so an import files it
	}
	meta.saved = now();
	const std::vector<uint8_t> bytes = PresetStore::encode(meta, data.bytes);
	const auto write = [&bytes](const std::filesystem::path& to) {
		std::ofstream out(to, std::ios::binary | std::ios::trunc);
		out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		return static_cast<bool>(out.flush());
	};
	// THE EXTENSION IS ADDED WHEN THE NAME HAS NONE — but the chosen path is tried as it was if that
	// fails: inside a sandbox the document portal grants exactly the file that was chosen, and no other.
	bool wrote = false;
	if (!file.has_extension()) {
		std::filesystem::path withExtension = file;
		withExtension += std::string(".") + kPresetExtension;
		wrote = write(withExtension);
		if (wrote) {
			file = withExtension;
		}
	}
	if (!wrote) {
		wrote = write(file);
	}
	say(wrote ? "Exported \"" + file.filename().string() + "\"" : "Export failed — \"" + file.filename().string() + "\" could not be written");
	return wrote;
}

//------------------------------------------------------------------------------------------------//
// THE LIST, AS AN ITEM SOURCE                                                                      //
//------------------------------------------------------------------------------------------------//

std::string PresetBrowser::itemLabel(size_t index) const {
	return entries_[index].name;
}

void PresetBrowser::itemsWillList() const {
	tagView_.clear();
	if (view_ != View::Tags) {
		return;
	}
	// THE COUNTS OVER THE WHOLE BANK, ONCE PER LISTING — each value's place in its namespace and its label.
	std::vector<PresetMeta> metas;
	metas.reserve(entries_.size());
	for (const Entry& e : entries_) {
		if (isVariantFile(e.name)) {
			continue; // A VARIANT IS NOT LISTED, so its tags count once, on its root
		}
		metas.push_back(e.meta);
	}
	for (const auto& [ns, values] : tagViewCounts(metas)) {
		auto& slot = tagView_[ns];
		for (size_t r = 0; r < values.size(); ++r) {
			slot[values[r].value] = { static_cast<int>(r), values[r].value + " (" + std::to_string(values[r].count) + ")" };
		}
	}
}

std::vector<std::vector<std::string>> PresetBrowser::itemSectionPaths(size_t index) const {
	const Entry& e = entries_[index];
	if (view_ == View::Tags) {
		// namespace ▸ value (count) — or namespace ▸ More… ▸ value (count) past the first
		// `kTagViewValues`. Under every tag it carries; untagged at the top.
		std::vector<std::vector<std::string>> out;
		std::vector<std::string> seen;
		for (const std::string& raw : e.meta.tags) {
			const std::string tag = normaliseTag(raw);
			if (tag.empty() || std::find(seen.begin(), seen.end(), tag) != seen.end()) {
				continue;
			}
			seen.push_back(tag);
			const TagParts parts = splitTag(tag);
			const auto ns = tagView_.find(parts.ns);
			if (ns == tagView_.end()) {
				continue; // not counted: `itemsWillList` did not run for this listing
			}
			const auto at = ns->second.find(parts.value);
			if (at == ns->second.end()) {
				continue;
			}
			if (at->second.first < static_cast<int>(kTagViewValues)) {
				out.push_back({ parts.ns, at->second.second });
			}
			else {
				out.push_back({ parts.ns, kMoreSection, at->second.second });
			}
		}
		return out;
	}
	if (e.category.empty()) {
		return {};
	}
	return { { e.category } };
}

double PresetBrowser::sectionOrder(const std::vector<std::string>& path) const {
	// NAMESPACES, AND FOLDERS, ALPHABETICAL; a namespace's values by count (their rank), More… last.
	if (view_ != View::Tags || path.size() < 2) {
		return 0.0;
	}
	if (path.back() == kMoreSection) {
		return 1e9;
	}
	const auto ns = tagView_.find(path.front());
	if (ns == tagView_.end()) {
		return 0.0;
	}
	for (const auto& [value, rankAndLabel] : ns->second) {
		if (rankAndLabel.second == path.back()) {
			return static_cast<double>(rankAndLabel.first);
		}
	}
	return 0.0;
}

std::string PresetBrowser::itemNote() const {
	if (view_ != View::Tags) {
		return {};
	}
	// A TAG VIEW WITH NOTHING TO FILE UNDER looks exactly like the folder view without this line.
	const bool anyTag = std::any_of(entries_.begin(), entries_.end(), [](const Entry& e) { return !e.meta.tags.empty(); });
	return anyTag ? std::string {} : "No tags yet \xe2\x80\x94 add tags when saving";
}

std::vector<ItemAction> PresetBrowser::itemActions() const {
	const bool writable = !isReadOnly(index_);
	const std::string current = currentName();
	std::vector<ItemAction> out;
	// **AT THE FOOT, THE THREE SAVES** (backlog PP) — the verbs a hand reaches for every session. Each row
	// NAMES what it will write before it is chosen, so `Save "X"` needs no second question: the row is it.
	// They act on the preset last saved or loaded (`lastTouched`), which is the one the sound came from.
	const int touched = lastTouched();
	const std::string touchedName = touched >= 0 ? entries_[static_cast<size_t>(touched)].name : "";
	const bool canSave = provider_ != nullptr;
	// NOT FOR A FACTORY PRESET: one click must not overwrite a shipped file (Save as… and the variant still offer).
	const bool inPlace = canSave && touched >= 0 && !isReadOnly(touched) && !isFactory(touched);
	out.push_back({ kActOverwrite, inPlace ? "Save \"" + touchedName + "\"" : "Save", inPlace });
	out.push_back({ kActSave, "Save as\xe2\x80\xa6", true });
	out.push_back({ kActSaveVariant, touched >= 0 ? "Save as \"" + variantName(touched) + "\"" : "Save as variant",
	                canSave && touched >= 0 });
	// Everything else is in Manage, in four groups (the header comment says why each is where it is).
	const std::string manage = kManageMenu;
	// **OVERWRITE IS BACK IN MANAGE**, though it duplicates `Save "X"` and Save as… → Enter: the same
	// preset, the same write. NOT OFFERED with nothing saved or loaded — there is no file it could mean. Over a factory
	// preset (or a read-only one) it is SHOWN DISABLED with the reason in its label, `Save "X"`'s guard said out loud:
	// in Manage the row is what a hand looks for, so it says why it will not, rather than vanishing.
	if (touched >= 0) {
		const bool factory = isFactory(touched);
		const std::string why = factory ? " \xe2\x80\x94 factory preset, use Save as\xe2\x80\xa6"
		                        : isReadOnly(touched) ? " \xe2\x80\x94 read-only"
		                                              : "";
		out.push_back({ kActManageOverwrite, "Overwrite \"" + touchedName + "\"" + why, inPlace, manage });
	}
	out.push_back({ kActEdit, "Rename / tags…", writable, manage });
	out.push_back({ kActDelete, writable ? "Delete \"" + current + "\"…" : "Delete…", writable, manage });
	// DISABLED, NOT HIDDEN, WHEN NO OS DIALOG CAN BE SHOWN (Linux with no portal, zenity or kdialog): a
	// row that opens nothing is a lie, and a row that vanished is a menu that changed shape unexplained.
	const bool dialog = FileDialog::available();
	out.push_back({ kActImport, "Import…", dialog && provider_ != nullptr, manage, true });
	out.push_back({ kActExport, "Export…", dialog && provider_ != nullptr, manage });
	// BOTH VIEWS, THE CURRENT ONE MARKED — not a row naming only the other one, which is a multivalue
	// toggle (CLAUDE.md). The mark is in the label because visage's list draws no check for `select`, and
	// a bullet because Lato, the list's font, has no check mark (U+2713); it has U+2022.
	const auto mark = [this](View v, const char* label) { return (view_ == v ? "\xe2\x80\xa2 " : "") + std::string(label); };
	out.push_back({ kActByFolder, mark(View::Folders, "Browse by folder"), true, manage, true });
	out.push_back({ kActByTag, mark(View::Tags, "Browse by tag"), true, manage });
	// WHERE THE LIBRARY IS. Disabled while `FEATHERS_PRESET_ROOT` pins the root — choosing would change
	// nothing — and Reset only while a folder other than the default is chosen.
	const bool pinned = PresetStore::rootOverridden();
	out.push_back({ kActChooseRoot, "Choose preset folder…", dialog && !pinned, manage, true });
	out.push_back({ kActResetRoot, "Reset preset folder", !pinned && !PresetStore::chosenRoot().empty(), manage });
	return out;
}

void PresetBrowser::itemAct(int id) {
	switch (id) {
	case kActSave:
		beginSave();
		break;
	case kActOverwrite:
	case kActManageOverwrite: // THE SAME WRITE, from Manage — one road, `overwriteCurrent`, with `Save "X"`'s guard
		if (const int touched = lastTouched(); touched >= 0 && !isFactory(touched) && overwriteCurrent(touched)) {
			index_ = touched;
			redraw();
		}
		break;
	case kActSaveVariant:
		(void)saveVariant(lastTouched());
		break;
	case kActEdit:
		beginEdit(index_);
		break;
	case kActDelete:
		if (!isReadOnly(index_)) {
			pendingDelete_ = index_;
			showDeleteMenu();
		}
		break;
	case kActChooseRoot:
		if (!PresetStore::rootOverridden()) {
			beginChooseRoot();
		}
		break;
	case kActResetRoot:
		if (!PresetStore::rootOverridden()) {
			(void)setPresetRoot({});
		}
		break;
	case kActImport:
		beginImport();
		break;
	case kActExport:
		beginExport();
		break;
	case kActByFolder:
		chooseView(View::Folders);
		break;
	case kActByTag:
		chooseView(View::Tags);
		break;
	default:
		break;
	}
}

void PresetBrowser::chooseView(View view) {
	const bool changed = view != view_;
	view_ = view;
	if (changed && onViewChanged) {
		onViewChanged(view_);
	}
	// **THE CHOICE IS SEEN AT ONCE: THE LIST COMES BACK IN THE NEW VIEW.** Before this the menu closed and
	// the face looked exactly as it had, so a click appeared to do nothing. Deferred: see `reopen_`.
	reopen_.startTimer(1);
}

void PresetBrowser::showDeleteMenu() {
	visage::PopupMenu menu;
	menu.addOption(kYes, "Delete \"" + currentName() + "\"");
	menu.addOption(kNo, "Cancel");
	menu.onSelection() = [this](int id) { resolveDelete(id == kYes); };
	menu.onCancel() = [this] { resolveDelete(false); };
	showMenuAt(std::move(menu), *this, nameBounds());
}

void PresetBrowser::resolveDelete(bool confirmed) {
	const int target = pendingDelete_;
	pendingDelete_ = -1;
	if (confirmed && target >= 0) {
		(void)deleteEntry(target);
	}
}

//------------------------------------------------------------------------------------------------//
// THE PROMPT                                                                                       //
//------------------------------------------------------------------------------------------------//

void PresetBrowser::openPrompt(PromptKind kind, const std::string& name, const std::string& tags) {
	notice_.clear();
	replaceSlot_ = -1;
	promptKind_ = kind;
	prompt_ = Prompt::Naming;
	setInteractive(false);
	// ON THE EDITOR'S ESC STACK (backlog NN), found from the browser — the prompt itself hangs in the top
	// frame, above the editor's root. Esc there is `cancelPrompt`, the field's own Esc; a confirmation steps
	// back to the name and keeps the entry, which only `closePrompt` takes off.
	if (EscapeStack* stack = SurfaceLayers::escapeStackFor(*this); stack != nullptr && !promptEscape_.held()) {
		promptEscape_ = stack->push([this] {
			if (prompt_ == Prompt::Closed) {
				return false;
			}
			cancelPrompt();
			return true;
		});
	}
	// IN THE WINDOW'S TOP FRAME, under the browser, so a 28 px title row does not clip it. On its own
	// (a case with no window), the browser is its own top and the prompt hangs under it.
	visage::Frame* top = topParentFrame();
	if (prompt_ui_->parent() != top) {
		prompt_ui_->takeOutOfTree();
		top->addChild(prompt_ui_.get());
	}
	const visage::Point at = top == this ? visage::Point { 0.0f, 0.0f }
	                                     : positionInWindow() - top->positionInWindow();
	const float w = std::max(PresetPrompt::kWidth, totalWidth());
	const float x = top == this ? 0.0f : std::clamp(at.x, 0.0f, std::max(0.0f, static_cast<float>(top->width()) - w));
	// BELOW IF IT FITS, ELSE ABOVE, ELSE CLAMPED INSIDE — the rule `showMenuAt` gives every menu (backlog
	// GG). Always-below put the prompt off the window for a browser at the window's foot.
	float y = at.y + static_cast<float>(height());
	if (top != this) {
		const float topH = static_cast<float>(top->height());
		if (y + PresetPrompt::kHeight > topH) {
			const float above = at.y - PresetPrompt::kHeight;
			y = above >= 0.0f ? above : std::clamp(y, 0.0f, std::max(0.0f, topH - PresetPrompt::kHeight));
		}
	}
	prompt_ui_->setBounds(x, y, w, PresetPrompt::kHeight);
	prompt_ui_->name.setText(name);
	prompt_ui_->tags.setText(tags);
	prompt_ui_->setVisible(true);
	// SELECTED, so the first key typed replaces the suggestion rather than appending to it.
	prompt_ui_->name.selectAll();
	prompt_ui_->name.requestKeyboardFocus();
	// Once it is in the window's tree, where the window it claims for can be found. A reopen over an open
	// prompt takes the new claim before the old one goes.
	promptClaim_ = std::make_unique<KeyboardClaim>(*prompt_ui_);
	prompt_ui_->redraw();
	redraw();
}

void PresetBrowser::beginSave() {
	editing_ = -1;
	// PRE-FILLED WITH THE PRESET LAST SAVED OR LOADED (backlog PP: Save defaults to the name and tags
	// last saved or loaded): Enter then asks "Replace?" once — an overwrite is one
	// confirmation, not retyping — and a new name is typed over the selected text.
	if (const int touched = lastTouched(); touched >= 0) {
		const Entry& e = entries_[static_cast<size_t>(touched)];
		// THE TAGS CLOSED WITH ", " — as a taken chip leaves them — so the field reads as finished tags and the
		// chips offer more, rather than completing the last one as if it were being typed.
		const std::string tags = e.meta.tags.empty() ? std::string {} : joinTags(e.meta.tags) + ", ";
		openPrompt(PromptKind::Save, e.category.empty() ? e.name : e.category + "/" + e.name, tags);
		return;
	}
	openPrompt(PromptKind::Save, suggestedName(), {});
}

void PresetBrowser::beginEdit(int index) {
	if (isReadOnly(index)) {
		return;
	}
	editing_ = index;
	const Entry& e = entries_[static_cast<size_t>(index)];
	openPrompt(PromptKind::Edit, e.category.empty() ? e.name : e.category + "/" + e.name, joinTags(e.meta.tags));
}

std::string PresetBrowser::typedName() const {
	return prompt_ui_->name.text().toUtf8();
}

std::vector<std::string> PresetBrowser::typedTags() const {
	return parseTagList(prompt_ui_->tags.text().toUtf8());
}

std::string PresetBrowser::tagBeingTyped() const {
	const std::string text = prompt_ui_->tags.text().toUtf8();
	const size_t comma = text.rfind(',');
	// AS TYPED, only trimmed: "mood:" is a namespace waiting for its value, which normalising would
	// throw away (a tag with no value is no tag). `tagCompletions` reads it half by half.
	const std::string part = comma == std::string::npos ? text : text.substr(comma + 1);
	const size_t first = part.find_first_not_of(" \t");
	if (first == std::string::npos) {
		return {};
	}
	return part.substr(first, part.find_last_not_of(" \t") - first + 1);
}

void PresetBrowser::takeTag(std::vector<std::string> present, const std::string& tag) {
	const std::string typing = tagBeingTyped();
	present.erase(std::remove(present.begin(), present.end(), normaliseTag(typing)), present.end());
	// A NAMESPACE ("mood:") STAYS OPEN for its value to be typed; a whole tag is closed with a comma.
	const bool open = !tag.empty() && tag.back() == ':';
	if (!open && std::find(present.begin(), present.end(), tag) == present.end()) {
		present.push_back(tag);
	}
	std::string text = joinTags(present);
	if (open) {
		text += (text.empty() ? "" : ", ") + tag;
	}
	else {
		text += ", ";
	}
	prompt_ui_->tags.setText(text);
	prompt_ui_->redraw();
}

std::vector<TagStat> PresetBrowser::rankedTags() const {
	// THIS BANK'S ENTRIES AND NO OTHERS — which is what makes suggestions per device type.
	std::vector<PresetMeta> metas;
	metas.reserve(entries_.size());
	for (const Entry& e : entries_) {
		if (isVariantFile(e.name)) {
			continue; // A VARIANT IS NOT LISTED, so its tags count once, on its root
		}
		metas.push_back(e.meta);
	}
	return rankTags(metas, now());
}

std::vector<std::string> PresetBrowser::chips() const {
	if (prompt_ == Prompt::Closed) {
		return {};
	}
	const std::vector<TagStat> ranked = rankedTags();
	std::vector<std::string> present = typedTags();
	const std::string typing = tagBeingTyped();
	if (!typing.empty()) {
		// The tag being typed is not yet "present" for completion purposes: it is the prefix.
		present.erase(std::remove(present.begin(), present.end(), normaliseTag(typing)), present.end());
		return tagCompletions(ranked, typing, present);
	}
	return tagSuggestions(ranked, present);
}

bool PresetBrowser::acceptCompletion() {
	if (prompt_ != Prompt::Naming) {
		return false;
	}
	const std::string typing = tagBeingTyped();
	if (typing.empty()) {
		return false;
	}
	std::vector<std::string> present = typedTags();
	present.erase(std::remove(present.begin(), present.end(), normaliseTag(typing)), present.end());
	const std::vector<std::string> options = tagCompletions(rankedTags(), typing, present, 1);
	if (options.empty()) {
		return false;
	}
	takeTag(typedTags(), options.front());
	return true;
}

void PresetBrowser::pressChip(size_t index) {
	const std::vector<std::string> offered = chips();
	if (index >= offered.size()) {
		return;
	}
	takeTag(typedTags(), offered[index]);
}

void PresetBrowser::submitPrompt() {
	if (prompt_ != Prompt::Naming) {
		return;
	}
	const auto [name, category] = PresetStore::splitTyped(typedName());
	if (name.empty()) {
		notice_ = "Type a name";
		prompt_ui_->redraw();
		return;
	}
	const std::vector<std::string> tags = typedTags();
	const int taken = indexOf(name, category);
	if (promptKind_ == PromptKind::Edit) {
		if (taken >= 0 && taken != editing_) {
			notice_ = "\"" + name + "\" already exists";
			prompt_ui_->redraw();
			return;
		}
		if (!editEntry(editing_, typedName(), tags)) {
			notice_ = "Could not rename";
			prompt_ui_->redraw();
			return;
		}
		closePrompt();
		return;
	}
	if (taken >= 0) {
		if (isReadOnly(taken)) {
			notice_ = "\"" + name + "\" cannot be replaced";
			prompt_ui_->redraw();
			return;
		}
		// ASKED, NOT DONE: the two outcomes are on the face before either happens.
		replaceSlot_ = taken;
		prompt_ = Prompt::ConfirmReplace;
		showReplaceMenu();
		return;
	}
	if (saveAs(typedName(), tags)) {
		closePrompt();
	}
	else {
		notice_ = "Nothing to save";
		prompt_ui_->redraw();
	}
}

void PresetBrowser::resolveReplace(bool replace) {
	if (prompt_ != Prompt::ConfirmReplace) {
		return;
	}
	const int slot = replaceSlot_;
	replaceSlot_ = -1;
	if (!replace || slot < 0) {
		// BACK TO THE FIELDS, the typed name still in them — "offer rename" is exactly this.
		prompt_ = Prompt::Naming;
		prompt_ui_->name.selectAll();
		prompt_ui_->name.requestKeyboardFocus();
		return;
	}
	entries_[static_cast<size_t>(slot)].meta.tags = typedTags();
	if (overwriteCurrent(slot)) {
		index_ = slot;
	}
	closePrompt();
}

void PresetBrowser::cancelPrompt() {
	if (prompt_ == Prompt::ConfirmReplace) {
		resolveReplace(false);
		return;
	}
	closePrompt();
}

void PresetBrowser::closePrompt() {
	prompt_ = Prompt::Closed;
	replaceSlot_ = -1;
	editing_ = -1;
	notice_.clear();
	setInteractive(true);
	promptClaim_.reset();
	promptEscape_.release();
	// THE KEYBOARD GOES BACK TO THE EDITOR IF THE PROMPT HELD IT (backlog NN), as a menu's Esc hands it back: the
	// field leaving the tree would otherwise leave the focus on nobody, and the NEXT Esc — meant for whatever opened
	// before the prompt — would go to the host. To the first frame up from the browser that takes keys.
	const bool heldKeys = prompt_ui_->name.hasKeyboardFocus() || prompt_ui_->tags.hasKeyboardFocus()
	                      || prompt_ui_->hasKeyboardFocus();
	prompt_ui_->takeOutOfTree();
	if (heldKeys) {
		visage::Frame* keys = this;
		while (keys != nullptr && !keys->acceptsKeystrokes()) {
			keys = keys->parent();
		}
		if (keys != nullptr) {
			keys->requestKeyboardFocus();
		}
	}
	redraw();
}

void PresetBrowser::showReplaceMenu() {
	if (replaceSlot_ < 0) {
		return;
	}
	visage::PopupMenu menu;
	menu.addOption(kYes, "Replace \"" + entries_[static_cast<size_t>(replaceSlot_)].name + "\"");
	menu.addOption(kNo, "Choose another name");
	menu.onSelection() = [this](int id) { resolveReplace(id == kYes); };
	// UNDER THE NAME FIELD IT IS ABOUT — the prompt's first row.
	showMenuAt(std::move(menu), *prompt_ui_, prompt_ui_->name.bounds());
}

void PresetBrowser::hierarchyChanged() {
	// TAKEN OUT OF THE TREE WITH THE PROMPT UP (the panel dismissed under it): the prompt goes too, and
	// nothing is written.
	if (prompt_ != Prompt::Closed && parent() == nullptr) {
		closePrompt();
	}
	// AND A LIST WAITING TO REOPEN does not reopen onto a browser nobody can see — nor does an OS dialog
	// or an import question stay open over a panel that has gone.
	if (parent() == nullptr) {
		reopen_.stopTimer();
		fileDialog_.cancel();
		pendingImport_.reset();
	}
}

std::string PresetBrowser::promptTextForTest() const {
	return typedName();
}

void PresetBrowser::typeForTest(const std::string& text) {
	prompt_ui_->name.setText(text);
}

std::string PresetBrowser::tagsTextForTest() const {
	return prompt_ui_->tags.text().toUtf8();
}

void PresetBrowser::typeTagsForTest(const std::string& text) {
	prompt_ui_->tags.setText(text);
}

visage::Frame* PresetBrowser::nameFieldForTest() const {
	return &prompt_ui_->name;
}

visage::Frame* PresetBrowser::tagsFieldForTest() const {
	return &prompt_ui_->tags;
}

visage::Frame* PresetBrowser::promptFrameForTest() const {
	return prompt_ui_.get();
}

void PresetBrowser::draw(visage::Canvas& canvas) {
	// WHILE THE PROMPT IS UP the box wears the accent and the arrows go dim: they do nothing until it
	// closes. Otherwise the shared row draws itself.
	if (prompt_ != Prompt::Closed) {
		drawChrome(canvas, kAccent, false);
		const visage::Bounds name = nameBounds();
		canvas.setColor(kMuted);
		canvas.text(promptKind_ == PromptKind::Save ? "Saving…" : "Editing…", font(kControlSize), visage::Font::kLeft,
		            name.x() + 12.0f, name.y(), name.width() - 24.0f, name.height());
		drawVariantChips(canvas);
		return;
	}
	ItemStepper::draw(canvas);
	drawVariantChips(canvas);
}

} // namespace feathers::ui

namespace feathers::ui {

void bindPresetIdentity(PresetBrowser& browser, const std::function<PresetIdentity()>& read,
                        std::function<void(const PresetIdentity&)> write) {
	if (read) {
		const PresetIdentity stored = read();
		(void)browser.restoreCurrent(stored.name, stored.category);
	}
	browser.onCurrentChanged = [write = std::move(write)](const std::string& name, const std::string& category) {
		if (write) {
			write(PresetIdentity { name, category });
		}
	};
}

} // namespace feathers::ui
