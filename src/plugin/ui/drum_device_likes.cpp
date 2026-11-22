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

#include "plugin/ui/drum_device_likes.h"

#include "plugin/ui/like_glyph.h"
#include "plugin/ui/menu_anchor.h"
#include "plugin/ui/theme.h"
#include "plugin/ui/type_in.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace feathers::ui {

namespace {

constexpr float kButtonRounding = 7.0f;
constexpr float kButtonTracking = 0.18f;

/// **A LIKES' EDIT AS ONE UNDO STEP**: the whole block either side, written back through the host, as the synth's `LikesEdit` is.
class LikesBlockEdit final : public EditRecord {
public:
	LikesBlockEdit(backplane::DeviceHost& host, DeviceInstanceId id, std::vector<float> before, std::vector<float> after, const char* verb)
	    : host_(host), id_(id), before_(std::move(before)), after_(std::move(after)), verb_(verb) {}
	void undo() const override { (void)host_.setBlock(id_, dd::kLikesBlock, before_); }
	void redo() const override { (void)host_.setBlock(id_, dd::kLikesBlock, after_); }
	const char* verb() const override { return verb_; }

private:
	backplane::DeviceHost& host_;
	DeviceInstanceId id_;
	std::vector<float> before_;
	std::vector<float> after_;
	const char* verb_;
};

} // namespace

DrumLikeBar::DrumLikeBar(DrumDeviceModel& model, DrumDevicePad& pad, Notifier* notifier) : model_(model), pad_(pad), notifier_(notifier) {
	pad_.applied = [this]() -> const std::vector<dd::DrumLike>& { return applied_; };
	onMouseDown() = [this](const visage::MouseEvent& e) {
		if (heartBounds().contains(e.position)) {
			// A POPUP TRIGGER (a right press, or ctrl-left on a Mac) opens the heart's menu; any other press is the like.
			if (e.shouldTriggerPopup()) {
				showMenuAt(menu(), *this, heartBounds());
			}
			else {
				(void)toggleLike();
			}
		}
		else if (mutateBounds().contains(e.position)) {
			mutate();
		}
	};
}

std::vector<dd::DrumLike> DrumLikeBar::all() const {
	std::vector<float> block(dd::kLikesBlockWords);
	std::vector<dd::DrumLike> likes;
	if (model_.host().blockAt(model_.id(), dd::kLikesBlock, block) && dd::readLikeBlock(block.data(), block.size(), &likes)) {
		return likes;
	}
	return {};
}

bool DrumLikeBar::refresh() {
	const std::vector<dd::DrumLike> now = all();
	const dd::Settings s = model_.settings();
	const std::array<float, 13> key = drumLikeKey(s);
	std::vector<dd::DrumLike> applied;
	for (const dd::DrumLike& like : now) {
		if (likeApplies(like, s)) {
			applied.push_back(like);
		}
	}
	const bool changed = applied != applied_ || key != stateKey_ || strength_ != stateStrength_;
	// THE HEART LIGHTS WHILE THE STAR STANDS ON A LIKE, so it repaints when that changes (a drag, a Mutate), not only when the likes do.
	const bool under = pad_.likeUnderStar().has_value();
	if (under != shownLiked_) {
		shownLiked_ = under;
		redraw();
	}
	lastBlock_ = now;
	applied_ = applied;
	if (changed) {
		stateKey_ = key;
		stateStrength_ = strength_;
		state_ = deriveDrumLikes(s, now, std::min(100.0, kWarpStrength * strength_));
		redraw();
	}
	return changed;
}

bool DrumLikeBar::writeLikes(const std::vector<dd::DrumLike>& likes) {
	std::vector<float> before(dd::kLikesBlockWords);
	if (!model_.host().blockAt(model_.id(), dd::kLikesBlock, before)) {
		dd::writeLikeBlock({}, before.data());
	}
	std::vector<float> after(dd::kLikesBlockWords);
	dd::writeLikeBlock(likes, after.data());
	if (!model_.host().setBlock(model_.id(), dd::kLikesBlock, after)) {
		return false;
	}
	if (EditHistory* history = historyOf(model_.control(dd::Param::TravelX))) {
		history->record(std::make_unique<LikesBlockEdit>(model_.host(), model_.id(), std::move(before), std::move(after), "like"));
	}
	return true;
}

bool DrumLikeBar::toggleLike() {
	std::vector<dd::DrumLike> likes = all();
	const std::optional<size_t> under = pad_.likeUnderStar();
	bool liked = false;
	if (under && *under < applied_.size()) {
		const auto at = std::find(likes.begin(), likes.end(), applied_[*under]);
		if (at != likes.end()) {
			likes.erase(at);
		}
	}
	else {
		if (likes.size() >= dd::kMaxDrumLikes) {
			// SAID, NOT SILENT: the stored layout holds kMaxDrumLikes, and a press that did nothing would look broken.
			if (notifier_ != nullptr) {
				notifier_->say("Likes full: remove one first");
			}
			return false;
		}
		likes.push_back(likeOf(model_.settings()));
		liked = true;
	}
	const bool ok = writeLikes(likes);
	if (ok && notifier_ != nullptr) {
		notifier_->say(liked ? "Liked" : "Like removed");
	}
	(void)refresh();
	return ok;
}

void DrumLikeBar::clearLikes() {
	const dd::Settings s = model_.settings();
	const auto kind = static_cast<uint16_t>(std::clamp(s.index(dd::Param::Kind), 0, static_cast<int>(feathers::drum::kKindCount) - 1));
	std::vector<dd::DrumLike> keep;
	for (const dd::DrumLike& like : all()) {
		if (like.kind != kind) {
			keep.push_back(like);
		}
	}
	if (keep.size() == all().size()) {
		return; // nothing of this Kind's to clear; not an edit
	}
	if (writeLikes(keep) && notifier_ != nullptr) {
		notifier_->say("Likes cleared");
	}
	(void)refresh();
}

double DrumLikeBar::rollUnit() noexcept {
	// A plain xorshift, so a roll is reproducible from a seed and the bar owns no global (as the synth's dice).
	dice_ ^= dice_ << 13;
	dice_ ^= dice_ >> 17;
	dice_ ^= dice_ << 5;
	return static_cast<double>(dice_) / 4294967296.0 * 2.0 - 1.0;
}

double DrumLikeBar::reach() const {
	return DrumDevicePad::kTravelPerHalfDrag / pad_.zoom();
}

void DrumLikeBar::mutate() {
	const double unit = rollUnit();
	const double sideways = rollUnit();
	(void)refresh();
	DeviceControl& tx = model_.control(dd::Param::TravelX);
	DeviceControl& ty = model_.control(dd::Param::TravelY);
	const std::array<double, 2> to = drumMutate(state_.get(), tx.value(), ty.value(), sideways, unit, reach());
	EditHistory* history = historyOf(tx);
	if (history != nullptr) {
		history->beginGroup("Mutate drum");
	}
	tx.beginGesture();
	ty.beginGesture();
	tx.setValue(to[0]);
	ty.setValue(to[1]);
	tx.endGesture();
	ty.endGesture();
	if (history != nullptr) {
		history->endGroup();
	}
	if (notifier_ != nullptr) {
		notifier_->say("Mutated");
	}
}

visage::PopupMenu DrumLikeBar::menu() {
	// SAYS WHOSE LIKES IT CLEARS: this Kind's, on every plane.
	const dd::Settings s = model_.settings();
	const auto kind = static_cast<size_t>(std::clamp(s.index(dd::Param::Kind), 0, static_cast<int>(feathers::drum::kKindCount) - 1));
	char caption[96] = {};
	std::snprintf(caption, sizeof(caption), "Clear all likes on %s", dd::kKindLabels[kind]);
	bool any = false;
	for (const dd::DrumLike& like : all()) {
		any = any || like.kind == kind;
	}
	visage::PopupMenu menu;
	menu.addOption(kClearLikes, caption).enable(any);
	// THE LIKES' STRENGTH: how strongly every like pulls the warp (x1/x2/x4; view state, not saved).
	menu.addBreak();
	for (const int v : { 1, 2, 4 }) {
		char label[48] = {};
		std::snprintf(label, sizeof(label), "Like strength \xc3\x97%d", v);
		menu.addOption(kLikeStrength + v, label).select(strength_ == v);
	}
	menu.onSelection() = [this](int id) {
		if (id == kClearLikes) {
			clearLikes();
		}
		else if (id == kLikeStrength + 1 || id == kLikeStrength + 2 || id == kLikeStrength + 4) {
			strength_ = id - kLikeStrength;
			(void)refresh();
		}
	};
	return menu;
}

const char* DrumLikeBar::tooltipAt(visage::Point local) const {
	if (heartBounds().contains(local)) {
		return "Like this drum where it stands, or remove the like it stands on. Liked places are given more room on the terrain, and Mutate favours them. Right-click for more";
	}
	// EVERYWHERE ELSE ON THE BAR (the gap, a hover at its middle) IT IS MUTATE's: the bar is the two buttons and nothing else.
	return "Roll the dice: move the drum a step across the terrain, in the view as it is drawn, so the likes steer it";
}

void DrumLikeBar::offeredControls(std::vector<Offered>& out) const {
	out.push_back({ &model_.control(dd::Param::TravelX), Road::Gesture });
	out.push_back({ &model_.control(dd::Param::TravelY), Road::Gesture });
}

void DrumLikeBar::draw(visage::Canvas& canvas) {
	const bool liked = pad_.likeUnderStar().has_value();
	const visage::Bounds heart = heartBounds();
	canvas.setColor(liked ? kLikeHue : kLine);
	canvas.roundedRectangleBorder(heart.x(), heart.y(), heart.width(), heart.height(), kButtonRounding, 1.0f);
	canvas.setColor(liked ? kLikeHue : kMuted);
	drawHeart(canvas, heart.x() + heart.width() * 0.5f, heart.y() + heart.height() * 0.5f, heart.height() * 0.5f);
	const visage::Bounds b = mutateBounds();
	canvas.setColor(kBorderOn);
	canvas.roundedRectangleBorder(b.x(), b.y(), b.width(), b.height(), kButtonRounding, 1.0f);
	const visage::Font& face = font(kControlSize);
	const std::string caption = "MUTATE";
	const float captionWidth = trackedWidth(face, caption, kButtonTracking);
	canvas.setColor(kAccent);
	drawTracked(canvas, caption, face, kButtonTracking, b.x() + (b.width() - captionWidth) * 0.5f,
	            b.y() + (b.height() - kControlSize) * 0.5f - 1.0f, kControlSize + 2.0f);
}

} // namespace feathers::ui
