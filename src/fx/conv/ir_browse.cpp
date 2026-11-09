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

#include "fx/conv/ir_browse.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

namespace feathers::fx::conv {

namespace {

std::string lower(std::string s) {
	for (char& c : s) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return s;
}

/// "Hall 2" before "Hall 10": runs of digits compare as numbers, everything else case-insensitively.
bool naturalLess(const std::string& a, const std::string& b) {
	size_t i = 0;
	size_t j = 0;
	while (i < a.size() && j < b.size()) {
		const auto ca = static_cast<unsigned char>(a[i]);
		const auto cb = static_cast<unsigned char>(b[j]);
		if (std::isdigit(ca) != 0 && std::isdigit(cb) != 0) {
			size_t ie = i;
			size_t je = j;
			while (ie < a.size() && std::isdigit(static_cast<unsigned char>(a[ie])) != 0) {
				++ie;
			}
			while (je < b.size() && std::isdigit(static_cast<unsigned char>(b[je])) != 0) {
				++je;
			}
			// Compare the numbers without converting (they may be long): strip leading zeros, then length.
			size_t ia = i;
			size_t jb = j;
			while (ia + 1 < ie && a[ia] == '0') {
				++ia;
			}
			while (jb + 1 < je && b[jb] == '0') {
				++jb;
			}
			if (ie - ia != je - jb) {
				return ie - ia < je - jb;
			}
			const int cmp = a.compare(ia, ie - ia, b, jb, je - jb);
			if (cmp != 0) {
				return cmp < 0;
			}
			i = ie;
			j = je;
			continue;
		}
		const int la = std::tolower(ca);
		const int lb = std::tolower(cb);
		if (la != lb) {
			return la < lb;
		}
		++i;
		++j;
	}
	if (a.size() - i != b.size() - j) {
		return a.size() - i < b.size() - j;
	}
	return a < b; // a tie in every other sense: a stable, total order
}

} // namespace

bool isIrFileName(const std::string& name) {
	const std::string ext = lower(std::filesystem::path(name).extension().string());
	return ext == ".wav" || ext == ".aif" || ext == ".aiff" || ext == ".flac";
}

/// The IRs in one folder, in natural order (the lister both roads share).
std::vector<std::string> filesIn(const std::filesystem::path& folder);

std::vector<std::string> irFilesBeside(const std::string& path) {
	namespace fs = std::filesystem;
	std::vector<std::string> out;
	if (path.empty()) {
		return out;
	}
	return filesIn(fs::path(path).parent_path());
}

std::vector<std::string> filesIn(const std::filesystem::path& folder) {
	namespace fs = std::filesystem;
	std::vector<std::string> out;
	std::error_code ec;
	for (fs::directory_iterator it(folder.empty() ? fs::path(".") : folder, ec), end; !ec && it != end;
	     it.increment(ec)) {
		std::error_code fe;
		if (!it->is_regular_file(fe) || fe) {
			continue;
		}
		const std::string name = it->path().filename().string();
		if (isIrFileName(name)) {
			out.push_back((folder / name).string());
		}
	}
	std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
		return naturalLess(fs::path(a).filename().string(), fs::path(b).filename().string());
	});
	return out;
}

std::string steppedIr(const std::string& path, int step) {
	namespace fs = std::filesystem;
	const std::vector<std::string> files = irFilesBeside(path);
	if (files.empty()) {
		return {};
	}
	const std::string name = fs::path(path).filename().string();
	// WHERE `path` IS, or where it WOULD sort if it has gone.
	long at = -1;
	for (size_t i = 0; i < files.size(); ++i) {
		if (fs::path(files[i]).filename().string() == name) {
			at = static_cast<long>(i);
			break;
		}
	}
	const auto n = static_cast<long>(files.size());
	if (at < 0) {
		const auto upper = std::upper_bound(files.begin(), files.end(), path, [](const std::string& a, const std::string& b) {
			return naturalLess(fs::path(a).filename().string(), fs::path(b).filename().string());
		});
		const auto slot = static_cast<long>(upper - files.begin()); // the first one after where it would sit
		const long from = step > 0 ? slot - 1 : slot;               // stepping +1 lands on `slot`, -1 on slot-1
		at = ((from % n) + n) % n;
		if (step == 0) {
			return files[static_cast<size_t>(((slot % n) + n) % n)];
		}
	}
	const long to = (((at + step) % n) + n) % n;
	return files[static_cast<size_t>(to)];
}


std::string defaultIrFolder(bool create) {
	namespace fs = std::filesystem;
	// THE TEST OVERRIDE FIRST, as presets-2's FEATHERS_PRESET_ROOT: a case names a scratch folder and no run of the
	// suite ever makes one in a real home.
	if (const char* root = std::getenv("FEATHERS_IR_ROOT"); root != nullptr && *root != '\0') {
		if (create) {
			std::error_code ec;
			fs::create_directories(root, ec);
		}
		return std::string(root);
	}
#if defined(_WIN32)
	const char* home = std::getenv("USERPROFILE");
#else
	const char* home = std::getenv("HOME");
#endif
	if (home == nullptr || *home == '\0') {
		return {};
	}
	const fs::path folder = fs::path(home) / "Documents" / "Owlet" / "Feathers" / "IRs";
	if (create) {
		std::error_code ec;
		fs::create_directories(folder, ec); // a folder that cannot be made is simply empty
	}
	return folder.string();
}

std::vector<IrChoice> irChoices(int generatedRooms, const std::string& folder) {
	std::vector<IrChoice> out;
	for (int g = 0; g < generatedRooms; ++g) {
		out.push_back({ g, {} });
	}
	if (!folder.empty()) {
		for (std::string& file : filesIn(folder)) {
			out.push_back({ -1, std::move(file) });
		}
	}
	return out;
}

IrChoice steppedChoice(const std::vector<IrChoice>& choices, const IrChoice& here, int step) {
	namespace fs = std::filesystem;
	if (choices.empty()) {
		return here;
	}
	long at = -1;
	for (size_t i = 0; i < choices.size(); ++i) {
		const IrChoice& c = choices[i];
		const bool same = here.generated >= 0 ? c.generated == here.generated
		                                      : c.generated < 0 && fs::path(c.path).filename() == fs::path(here.path).filename();
		if (same) {
			at = static_cast<long>(i);
			break;
		}
	}
	if (at < 0) {
		return here;
	}
	const auto n = static_cast<long>(choices.size());
	return choices[static_cast<size_t>((((at + step) % n) + n) % n)];
}

std::vector<std::string> irFilesIn(const std::string& folder) {
	return folder.empty() ? std::vector<std::string> {} : filesIn(folder);
}

std::vector<std::string> subfoldersIn(const std::string& folder) {
	namespace fs = std::filesystem;
	std::vector<std::string> out;
	if (folder.empty()) {
		return out;
	}
	std::error_code ec;
	for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
		std::error_code fe;
		if (!it->is_directory(fe) || fe) {
			continue;
		}
		const std::string name = it->path().filename().string();
		if (name.empty() || name.front() == '.') {
			continue;
		}
		out.push_back((fs::path(folder) / name).string());
	}
	std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
		return naturalLess(fs::path(a).filename().string(), fs::path(b).filename().string());
	});
	return out;
}

std::vector<FolderRow> folderRows(const std::string& folder, const std::string& irRoot, const std::string& home) {
	namespace fs = std::filesystem;
	using Kind = FolderRow::Kind;
	std::vector<FolderRow> rows;
	const fs::path here = fs::path(folder).lexically_normal();
	const auto same = [&here](const std::string& other) {
		return !other.empty() && fs::path(other).lexically_normal() == here;
	};
	const auto nameOf = [](const fs::path& p) {
		const std::string n = p.filename().string();
		return n.empty() ? p.string() : n;
	};
	// WHERE THE MENU IS, the home folder written as "~" so a deep path still fits.
	std::string shown = here.string();
	if (!home.empty()) {
		const std::string h = fs::path(home).lexically_normal().string();
		if (shown.rfind(h, 0) == 0) {
			shown = "~" + shown.substr(h.size());
		}
	}
	rows.push_back({ Kind::Header, shown, {}, false });
	const size_t count = irFilesIn(folder).size();
	rows.push_back({ Kind::Use,
	                 count == 0 ? std::string("Use this folder (no impulses)")
	                            : "Use this folder (" + std::to_string(count) + (count == 1 ? " impulse)" : " impulses)"),
	                 here.string(), count > 0 });
	rows.push_back({ Kind::Break, {}, {}, false });
	// A trailing separator has an empty filename; the normalised path does not end in one except at the root.
	const fs::path parent = here.has_relative_path() ? here.parent_path() : here;
	if (parent != here && !parent.empty()) {
		rows.push_back({ Kind::Enter, "Up: " + nameOf(parent), parent.string(), true });
	}
	for (const std::string& sub : subfoldersIn(here.string())) {
		rows.push_back({ Kind::Enter, nameOf(fs::path(sub)) + "/", sub, true });
	}
	const bool rootRow = !irRoot.empty() && !same(irRoot);
	const bool homeRow = !home.empty() && !same(home);
	if (rootRow || homeRow) {
		rows.push_back({ Kind::Break, {}, {}, false });
	}
	if (rootRow) {
		rows.push_back({ Kind::Enter, "Impulse folder", irRoot, true });
	}
	if (homeRow) {
		rows.push_back({ Kind::Enter, "Home", home, true });
	}
	return rows;
}

} // namespace feathers::fx::conv
