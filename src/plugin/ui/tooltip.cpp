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

#include "plugin/ui/tooltip.h"

#include "plugin/ui/theme.h"

#include <visage_utils/string_utils.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>

namespace feathers::ui {

namespace {

double steadyMs() {
	using namespace std::chrono;
	return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

/// `local` in `frame`'s coordinates, carried up to `host`'s by summing each frame's own position —
/// the same arithmetic visage's `positionInWindow` does, stopped at the host rather than the window,
/// so it needs no window and a headless case gets the same answer the editor does.
visage::Point toHost(const visage::Frame& frame, const visage::Frame& host, visage::Point local) {
	visage::Point at = local;
	for (const visage::Frame* f = &frame; f != nullptr && f != &host; f = f->parent()) {
		at.x += f->x();
		at.y += f->y();
	}
	return at;
}

/// One hover event, carried to the nearest host. Asked afresh every time — see `TooltipHost`.
void forward(visage::Frame& frame, const TooltipSource& source, const std::shared_ptr<TooltipAnchor>& anchor,
             const visage::MouseEvent& e) {
	visage::Frame* hostFrame = nullptr;
	TooltipHost* host = tooltipHostOf(frame, &hostFrame);
	if (host == nullptr) {
		return;
	}
	const std::string text = hoverText(source, e.position);
	host->tooltipLayer().hover(toHost(frame, *hostFrame, e.position), text.c_str(), anchor);
}

template <bool (TooltipLayer::*Verb)()>
void tell(visage::Frame& frame) {
	if (TooltipHost* host = tooltipHostOf(frame)) {
		(void)(host->tooltipLayer().*Verb)();
	}
}

TooltipDetail& detailSlot() {
	static TooltipDetail detail = TooltipDetail::Full;
	return detail;
}

std::vector<TooltipDetailWatch*>& detailWatches() {
	static std::vector<TooltipDetailWatch*> watches;
	return watches;
}

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

/// Words whose "." is not a sentence's end. Compared lower-cased, so "E.g." and "Etc." are caught too.
constexpr std::string_view kAbbreviations[] = { "e.g.", "i.e.", "etc.", "vs.", "cf.", "approx." };

/// Whether the "." at `i` ends a sentence — see `briefTooltip`.
bool endsSentence(std::string_view text, size_t i) {
	// FOLLOWED BY WHITESPACE OR THE END. This is the rule that keeps "0.5 dB" and "44.1 kHz" whole, and
	// the inner dots of "e.g." and "...".
	if (i + 1 < text.size() && !isSpace(text[i + 1])) {
		return false;
	}
	// THE LAST DOT OF AN ELLIPSIS trails off; it does not end anything.
	if (i > 0 && text[i - 1] == '.') {
		return false;
	}
	// THE WORD THIS DOT ENDS, back to whitespace or an opening bracket.
	size_t start = i;
	while (start > 0 && !isSpace(text[start - 1]) && text[start - 1] != '(') {
		--start;
	}
	const std::string_view word = text.substr(start, i + 1 - start);
	for (const std::string_view abbreviation : kAbbreviations) {
		if (word.size() == abbreviation.size()
		    && std::equal(word.begin(), word.end(), abbreviation.begin(), [](char a, char b) {
			       return std::tolower(static_cast<unsigned char>(a)) == b;
		       })) {
			return false;
		}
	}
	return true;
}

std::string trimmedEnd(std::string_view text) {
	size_t end = text.size();
	while (end > 0 && isSpace(text[end - 1])) {
		--end;
	}
	return std::string(text.substr(0, end));
}

} // namespace

TooltipDetail tooltipDetail() noexcept { return detailSlot(); }

size_t tooltipDetailWatchCount() noexcept { return detailWatches().size(); }

size_t setTooltipDetail(TooltipDetail detail) {
	if (detailSlot() == detail) {
		return 0;
	}
	detailSlot() = detail;
	// A COPY: nothing a watch does may add or remove one mid-walk, and a copy makes that a non-question.
	const std::vector<TooltipDetailWatch*> watches = detailWatches();
	for (TooltipDetailWatch* w : watches) {
		if (w->changed_) {
			w->changed_(detail);
		}
	}
	return watches.size();
}

TooltipDetailWatch::TooltipDetailWatch(std::function<void(TooltipDetail)> changed) : changed_(std::move(changed)) {
	detailWatches().push_back(this);
}

TooltipDetailWatch::~TooltipDetailWatch() { std::erase(detailWatches(), this); }

std::string briefTooltip(std::string_view text) {
	for (size_t i = 0; i < text.size(); ++i) {
		if (text[i] == '.' && endsSentence(text, i)) {
			return trimmedEnd(text.substr(0, i + 1));
		}
	}
	return trimmedEnd(text);
}

std::string displayedTooltip(std::string_view text) {
	return tooltipDetail() == TooltipDetail::Brief ? briefTooltip(text) : std::string(text);
}

TooltipHost* tooltipHostOf(visage::Frame& frame, visage::Frame** hostFrame) {
	for (visage::Frame* f = &frame; f != nullptr; f = f->parent()) {
		if (auto* host = dynamic_cast<TooltipHost*>(f)) {
			if (hostFrame != nullptr) {
				*hostFrame = f;
			}
			return host;
		}
	}
	return nullptr;
}

const char* tooltipText(const char* tooltip, const char* name) {
	return (tooltip != nullptr && tooltip[0] != '\0') ? tooltip : name;
}

const char* tooltipOf(const device::Modulatable* control) {
	if (control == nullptr) {
		return nullptr;
	}
	// A CONTROL THAT CHANGES NOTHING WHERE IT STANDS SAYS WHY (a Poly switch inside a Rack, backlog NO).
	if (const char* why = control->unavailable(); why != nullptr) {
		return why;
	}
	const device::ParamDesc& p = control->desc();
	if (const char* own = control->tooltipOverride(); own != nullptr) {
		return own;
	}
	return tooltipText(p.tooltip, p.name);
}

namespace {

char lowered(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
bool isWordChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; }

/// The letters and digits of `text`, lower-cased — what an abbreviation is compared on.
std::string folded(std::string_view text) {
	std::string out;
	for (const char c : text) {
		if (isWordChar(c)) {
			out += lowered(c);
		}
	}
	return out;
}

/// The words of `text` (runs of letters and digits), lower-cased.
std::vector<std::string> wordsOf(std::string_view text) {
	std::vector<std::string> out;
	std::string word;
	for (const char c : text) {
		if (isWordChar(c)) {
			word += lowered(c);
		}
		else if (!word.empty()) {
			out.push_back(std::move(word));
			word.clear();
		}
	}
	if (!word.empty()) {
		out.push_back(std::move(word));
	}
	return out;
}

bool startsWithIgnoringCase(std::string_view text, std::string_view prefix) {
	return text.size() >= prefix.size()
	       && std::equal(prefix.begin(), prefix.end(), text.begin(), [](char a, char b) { return lowered(a) == lowered(b); });
}

} // namespace

bool abbreviates(std::string_view label, std::string_view full) {
	const std::string l = folded(label);
	const std::string f = folded(full);
	// SHORTER, AND NOT THE SAME NAME — "LENGTH" upper-cased on a knob is "Length", not a shortening of it.
	if (l.empty() || l.size() >= f.size()) {
		return false;
	}
	// IN ORDER: every letter of the label, found left to right in the full name.
	size_t at = 0;
	for (const char c : l) {
		at = f.find(c, at);
		if (at == std::string::npos) {
			return false;
		}
		++at;
	}
	// **NOT ONLY WHOLE WORDS OF IT** — "Room" under a card headed FEATHERVERB, "Feedback" for "Delay Feedback": the
	// device's own qualifier dropped, nothing shortened, and the head already says the rest.
	const std::vector<std::string> fullWords = wordsOf(full);
	for (const std::string& w : wordsOf(label)) {
		if (std::find(fullWords.begin(), fullWords.end(), w) == fullWords.end()) {
			return true;
		}
	}
	return false;
}

std::string fullNameToShow(const LabelFace& face) {
	if (abbreviates(face.label, face.full)) {
		return face.full;
	}
	if (face.cut) {
		// A CUT LABEL THAT IS ITS OWN NAME (a macro's, a filter macro's meaning) says itself whole; one that was the
		// control's own name says that.
		return face.label.empty() ? face.full : face.label;
	}
	return {};
}

std::string withFullName(std::string_view full, const char* tip) {
	const std::string_view text = tip != nullptr ? std::string_view(tip) : std::string_view {};
	if (full.empty()) {
		return std::string(text);
	}
	if (text.empty()) {
		return std::string(full);
	}
	if (startsWithIgnoringCase(text, full)) {
		return std::string(text);
	}
	// AN EM DASH, NOT A FULL STOP: Brief cuts at the first sentence end, and the name must be inside it.
	return std::string(full) + " \xE2\x80\x94 " + std::string(text);
}

std::string hoverText(const TooltipSource& source, visage::Point local) {
	const char* tip = source.tooltipAt(local);
	const std::optional<LabelFace> face = source.labelFaceAt(local);
	return withFullName(face ? fullNameToShow(*face) : std::string {}, tip);
}

void installTooltipHover(visage::Frame& frame, const TooltipSource& source) {
	// THE LIVENESS TOKEN, OWNED BY THE HOOKS BELOW — and so by the frame's callback lists, which die with
	// the frame. See `TooltipAnchor`.
	auto anchor = std::make_shared<TooltipAnchor>(TooltipAnchor { &frame });
	// **`+=`, NEVER `=`** — these run BESIDE whatever the widget does with the same events. See the
	// header for why the call must come last.
	frame.onMouseEnter() += [&frame, &source, anchor](const visage::MouseEvent& e) {
		forward(frame, source, anchor, e);
	};
	frame.onMouseMove() += [&frame, &source, anchor](const visage::MouseEvent& e) {
		forward(frame, source, anchor, e);
	};
	frame.onMouseExit() += [&frame](const visage::MouseEvent&) { tell<&TooltipLayer::leave>(frame); };
	frame.onMouseDown() += [&frame](const visage::MouseEvent&) { tell<&TooltipLayer::press>(frame); };
	frame.onMouseDrag() += [&frame](const visage::MouseEvent&) { tell<&TooltipLayer::press>(frame); };
}

TooltipLayer::TooltipLayer() : clock_(steadyMs) {
	// ABOVE EVERY SIBLING, and never in the way: the pointer goes straight through to the control
	// being described.
	setOnTop(true);
	setIgnoresMouseEvents(true, false);
	setVisible(false);
}

void TooltipLayer::watch(visage::Frame& host) {
	host.onResize() += [this, &host] {
		cover(host);
		// A RE-LAID-OUT HOST HAS MOVED ITS CONTROLS, so any anchor is stale.
		(void)clear();
	};
	// A SCALE CHANGE: the bounds re-set unconditionally (CLAUDE.md's setBounds/DPI rule), and the lines —
	// measured at the old scale — forgotten rather than drawn at the wrong width.
	onDpiChange() += [this, &host] {
		cover(host);
		(void)clear();
	};
	// **visage DELIVERS NO EXIT FOR ANY OF THESE** — a host hidden, taken out of its tree, or put back
	// into one (a card re-added at the same size fires no resize) would otherwise come back showing the
	// box it had when it went.
	host.onVisibilityChange() += [this] { (void)clear(); };
	host.onHierarchyChange() += [this] { (void)clear(); };
}

void TooltipLayer::cover(const visage::Frame& host) {
	// UNCONDITIONALLY — see the header, and CLAUDE.md's rule about a guarded setBounds under a scale
	// change.
	setBounds(0.0f, 0.0f, host.width(), host.height());
}

void TooltipLayer::hover(visage::Point at, const char* text, std::weak_ptr<TooltipAnchor> from) {
	if (state_ == State::Suppressed) {
		return;
	}
	if (text == nullptr || text[0] == '\0') {
		(void)leave();
		return;
	}
	anchor_ = at;
	from_ = std::move(from);
	fromWidget_ = !from_.expired();
	if (state_ != State::Idle && text_ == text) {
		// THE SAME THING, A LITTLE FURTHER ALONG. A shown box follows the pointer — RE-PLACED, not
		// re-measured: the lines depend on the text and the scale, neither of which a move changes. A
		// pending one keeps its clock, or a hand that never quite stops would never see a tooltip.
		if (state_ == State::Shown) {
			place();
			redraw();
		}
		return;
	}
	const bool wasShown = state_ == State::Shown;
	text_ = text;
	state_ = State::Pending;
	since_ = clock_();
	displayed_.clear();
	lines_.clear();
	box_ = {};
	if (wasShown) {
		setVisible(false);
	}
}

bool TooltipLayer::leave() {
	const bool wasShown = state_ == State::Shown;
	state_ = State::Idle;
	text_.clear();
	from_.reset();
	fromWidget_ = false;
	displayed_.clear();
	lines_.clear();
	box_ = {};
	if (wasShown) {
		setVisible(false);
	}
	return wasShown;
}

bool TooltipLayer::press() {
	const bool wasShown = state_ == State::Shown;
	if (wasShown) {
		setVisible(false);
	}
	state_ = State::Suppressed;
	displayed_.clear();
	lines_.clear();
	box_ = {};
	return wasShown;
}

bool TooltipLayer::clear() { return leave(); }

bool TooltipLayer::anchorAlive() const {
	if (!fromWidget_) {
		// A HOVER NO WIDGET MADE (a case's direct call) has nothing that can go away.
		return true;
	}
	const std::shared_ptr<TooltipAnchor> anchor = from_.lock();
	if (anchor == nullptr || anchor->frame == nullptr) {
		return false; // destroyed
	}
	// STILL UNDER THIS LAYER'S HOST, AND DRAWN ALL THE WAY UP TO IT — a widget taken out of the tree, or
	// hidden by any container on the way, is not one a hand can be resting on.
	for (const visage::Frame* f = anchor->frame; f != nullptr; f = f->parent()) {
		if (f == parent()) {
			return true;
		}
		if (!f->isVisible()) {
			return false;
		}
	}
	return false;
}

bool TooltipLayer::advance() {
	if ((state_ == State::Pending || state_ == State::Shown) && !anchorAlive()) {
		return leave();
	}
	if (state_ != State::Pending || clock_() - since_ < kDelayMs) {
		return false;
	}
	state_ = State::Shown;
	// **WHERE EVERY CONTROL'S TOOLTIP IS RESOLVED FOR DISPLAY** — at the detail current NOW, so a
	// setting changed since the last box shows on this one. (OWLTT's graph, which draws its own box,
	// asks the same `displayedTooltip`.)
	displayed_ = displayedTooltip(text_);
	measure();
	place();
	setVisible(true);
	redraw();
	return true;
}

void TooltipLayer::measure() {
	++measures_;
	const visage::Font& face = font(kKnobReadoutSize);
	// **MEASURED AT THIS FRAME'S SCALE WITH visage's OWN LINE BREAKER** — `Canvas::text` draws with
	// `font.withDpiScale(scale)`, so this is the face the renderer rasterises, and `lineBreaks` is the
	// function its multi-line text calls. A per-character estimate would disagree with the glyphs at
	// exactly the widths where a line nearly fits.
	const float scale = dpiScale() > 0.0f ? dpiScale() : 1.0f;
	const visage::Font scaled = face.withDpiScale(scale);
	const float hostW = width();
	const float innerMax = std::max(20.0f, std::min(kMaxWidth, hostW - 2.0f * kMargin) - 2.0f * kPadX);

	const std::u32string all = visage::String::convertToUtf32(displayed_);
	std::vector<int> breaks = scaled.lineBreaks(all.c_str(), static_cast<int>(all.size()), innerMax);
	breaks.push_back(static_cast<int>(all.size()));
	lines_.clear();
	float widest = 0.0f;
	int from = 0;
	for (int to : breaks) {
		std::u32string line = all.substr(static_cast<size_t>(from), static_cast<size_t>(to - from));
		// A BREAK LANDS AFTER THE SPACE; the space is not part of what is read, or measured.
		while (!line.empty() && (line.back() == U' ' || line.back() == U'\n')) {
			line.pop_back();
		}
		widest = std::max(widest, scaled.stringWidth(line));
		lines_.push_back(visage::String::convertToUtf8(line));
		from = to;
	}
	// **NEVER WIDER THAN THE HOST ALLOWS** — an unbreakable word longer than the wrap width would
	// otherwise push the box off the host's right edge; clipped inside the box is the lesser harm.
	const float w = std::min(std::ceil(widest) + 2.0f * kPadX, std::max(0.0f, hostW - 2.0f * kMargin));
	size_ = { w, scaled.lineHeight() * static_cast<float>(lines_.size()) + 2.0f * kPadY };
}

void TooltipLayer::place() {
	const float hostW = width();
	const float hostH = height();
	const float w = size_.x;
	const float h = size_.y;
	// ABOVE THE POINTER, so it does not cover the thing it describes — and BELOW when there is no room
	// above, then clamped inside the host either way. OWLTT's graph's placement, at card scale.
	const float x = std::clamp(anchor_.x - w * 0.5f, kMargin, std::max(kMargin, hostW - w - kMargin));
	float y = anchor_.y - h - kGap;
	if (y < kMargin) {
		y = anchor_.y + kGap + 4.0f;
	}
	y = std::clamp(y, kMargin, std::max(kMargin, hostH - h - kMargin));
	box_ = { x, y, w, h };
}

void TooltipLayer::draw(visage::Canvas& canvas) {
	if (state_ != State::Shown || lines_.empty()) {
		return;
	}
	// OWLTT's STYLE: the surface, a line-coloured border, ink text.
	canvas.setColor(withAlpha(kSurface, 0.96f));
	canvas.roundedRectangle(box_.x(), box_.y(), box_.width(), box_.height(), 3.0f);
	canvas.setColor(withAlpha(kLine, 0.7f));
	canvas.roundedRectangleBorder(box_.x(), box_.y(), box_.width(), box_.height(), 3.0f, 1.0f);
	canvas.setColor(kInk);
	const visage::Font& face = font(kKnobReadoutSize);
	const float lineH = (box_.height() - 2.0f * kPadY) / static_cast<float>(lines_.size());
	for (size_t i = 0; i < lines_.size(); ++i) {
		canvas.text(lines_[i], face, visage::Font::kLeft, box_.x() + kPadX,
		            box_.y() + kPadY + lineH * static_cast<float>(i), box_.width() - 2.0f * kPadX + 2.0f, lineH);
	}
}

} // namespace feathers::ui
