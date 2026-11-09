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

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "plugin/device_registry.h"
#include "state/json.h"

#include <bit>
#include <clocale>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <locale>
#include <random>
#include <string>
#include <vector>

// THE STATE FORMAT'S TEXT (backlog JF step 1a; state format §3.12 cases 3 and 8).
// The number road is asserted TO THE BIT, because §3.1 measured it exact and anything less would be a
// preset that drifts a little every time it is saved.

using namespace feathers::state;

namespace {

bool floatRoundTrips(float v) {
	std::string text;
	json::appendNumber(&text, v);
	float back = 0.0f;
	return json::parseNumber(text, &back) && std::bit_cast<uint32_t>(back) == std::bit_cast<uint32_t>(v);
}

bool doubleRoundTrips(double v) {
	std::string text;
	json::appendNumber(&text, v);
	double back = 0.0;
	return json::parseNumber(text, &back) && std::bit_cast<uint64_t>(back) == std::bit_cast<uint64_t>(v);
}

std::string write(const json::Value& v, bool indented = true) {
	std::string out;
	json::Writer w(&out, indented);
	w.value(v);
	REQUIRE(w.ok());
	return out;
}

} // namespace

TEST_CASE("json numbers: every registry value round-trips to the bit, as float32 and as double") {
	// Every control's default, minimum and maximum, plus seeded values in range: §3.1's census, kept.
	std::mt19937 rng(20260925);
	size_t tried = 0;
	size_t failedFloat = 0;
	size_t failedDouble = 0;
	for (const feathers::device::Descriptor& d : feathers::kDevices) {
		for (const feathers::device::ParamDesc& p : d.params) {
			std::vector<double> values { p.def, p.min, p.max };
			std::uniform_real_distribution<double> in(p.min, p.max);
			for (int k = 0; k < 200; ++k) {
				values.push_back(in(rng));
			}
			for (const double v : values) {
				++tried;
				failedFloat += floatRoundTrips(static_cast<float>(v)) ? 0 : 1;
				failedDouble += doubleRoundTrips(v) ? 0 : 1;
			}
		}
	}
	CHECK(tried > 50000);
	CHECK(failedFloat == 0);
	CHECK(failedDouble == 0);
}

TEST_CASE("json numbers: a strided sweep of every float32 bit pattern round-trips to the bit") {
	// The design measured every 97th pattern (44 million) in a scratch program; a prime stride of 4093
	// keeps a million here, spread evenly over signs, exponents and denormals.
	size_t tried = 0;
	size_t failed = 0;
	uint32_t firstFailure = 0;
	for (uint64_t bits = 0; bits <= 0xFFFFFFFFull; bits += 4093) {
		const float v = std::bit_cast<float>(static_cast<uint32_t>(bits));
		if (!std::isfinite(v)) {
			continue;
		}
		++tried;
		if (!floatRoundTrips(v)) {
			if (failed == 0) {
				firstFailure = static_cast<uint32_t>(bits);
			}
			++failed;
		}
	}
	INFO("first failing pattern 0x" << std::hex << firstFailure);
	CHECK(tried > 1000000);
	CHECK(failed == 0);
}

TEST_CASE("json numbers: NaN and infinity are written as null; -0, denormals and the extremes survive") {
	std::string text;
	json::appendNumber(&text, std::numeric_limits<float>::quiet_NaN());
	CHECK(text == "null");
	text.clear();
	json::appendNumber(&text, -std::numeric_limits<double>::infinity());
	CHECK(text == "null");

	text.clear();
	json::appendNumber(&text, -0.0f);
	CHECK(text == "-0");
	float back = 1.0f;
	REQUIRE(json::parseNumber(text, &back));
	CHECK(std::signbit(back));
	CHECK(back == 0.0f);

	CHECK(floatRoundTrips(std::numeric_limits<float>::denorm_min()));
	CHECK(floatRoundTrips(std::numeric_limits<float>::max()));
	CHECK(floatRoundTrips(-std::numeric_limits<float>::max()));
	CHECK(doubleRoundTrips(std::numeric_limits<double>::denorm_min()));
	CHECK(doubleRoundTrips(std::numeric_limits<double>::max()));

	// `null` is not a number, and a value that does not fit is out of range, never infinity.
	CHECK_FALSE(json::parseNumber("null", &back));
	CHECK_FALSE(json::parseNumber("1e999", &back));
	CHECK_FALSE(json::parseNumber("nan", &back));
	CHECK_FALSE(json::parseNumber("inf", &back));
	CHECK_FALSE(json::parseNumber("+1", &back));
	CHECK_FALSE(json::parseNumber("01", &back));
	CHECK_FALSE(json::parseNumber(".5", &back));
	CHECK_FALSE(json::parseNumber("1.", &back));
	CHECK_FALSE(json::parseNumber("0x10", &back));

	uint32_t u = 0;
	CHECK(json::parseNumber("4294967295", &u));
	CHECK(u == 4294967295u);
	CHECK_FALSE(json::parseNumber("4294967296", &u));
	CHECK_FALSE(json::parseNumber("-1", &u));
	CHECK_FALSE(json::parseNumber("1.0", &u));
	CHECK_FALSE(json::parseNumber("1e2", &u));
}

namespace {

/// A comma decimal point, for iostreams — what a host under a German locale would impose on anything
/// that formats through the C++ locale.
struct CommaDecimal : std::numpunct<char> {
	char do_decimal_point() const override { return ','; }
};

} // namespace

TEST_CASE("json numbers: a comma-decimal locale changes nothing") {
	// THE C LOCALE IS THE ONE `printf` AND `strtod` OBEY, and it can only be switched to one the machine
	// has installed — this host has none with a comma, so the case says which it could set. The C++
	// global locale is set regardless: it is the one a stream would obey.
	const char* setTo = nullptr;
	for (const char* name : { "de_DE.UTF-8", "de_DE.utf8", "fr_FR.UTF-8", "fr_FR.utf8", "de_DE", "fr_FR" }) {
		if (std::setlocale(LC_NUMERIC, name) != nullptr) {
			setTo = name;
			break;
		}
	}
	if (setTo == nullptr) {
		MESSAGE("no comma-decimal C locale is installed here; the C++ global locale is still set to one");
	}
	const std::locale before = std::locale::global(std::locale(std::locale::classic(), new CommaDecimal));

	json::Value doc;
	REQUIRE(json::parse(R"({"a": 0.5, "b": [1.25, -3e-7]})", &doc));
	std::string out;
	json::Writer w(&out, false);
	w.beginObject();
	w.key("half");
	w.number(0.5f);
	w.key("tenth");
	w.number(0.1);
	w.key("doc");
	w.value(doc);
	w.endObject();
	CHECK(w.ok());
	CHECK(out == R"({"half":0.5,"tenth":0.1,"doc":{"a":0.5,"b":[1.25,-3e-7]}})");
	float half = 0.0f;
	CHECK(json::parseNumber("0.5", &half));
	CHECK(half == 0.5f);

	std::locale::global(before);
	std::setlocale(LC_NUMERIC, "C");
}

TEST_CASE("json writer: tabs, one member per line, empty containers inline, and it reads back") {
	std::string out;
	json::Writer w(&out);
	w.beginObject();
	w.key("name");
	w.string("FVERB \"hall\"\n");
	w.key("values");
	w.beginObject();
	w.key("mix");
	w.number(0.25f);
	w.key("mode");
	w.string("Plate");
	w.endObject();
	w.key("list");
	w.beginArray();
	w.number(uint64_t { 1 });
	w.null();
	w.boolean(true);
	w.endArray();
	w.key("empty");
	w.beginObject();
	w.endObject();
	w.key("none");
	w.beginArray();
	w.endArray();
	w.endObject();
	REQUIRE(w.ok());
	CHECK(out
	      == "{\n\t\"name\": \"FVERB \\\"hall\\\"\\n\",\n\t\"values\": {\n\t\t\"mix\": 0.25,\n\t\t\"mode\": "
	         "\"Plate\"\n\t},\n\t\"list\": [\n\t\t1,\n\t\tnull,\n\t\ttrue\n\t],\n\t\"empty\": {},\n\t\"none\": "
	         "[]\n}");
	json::Value back;
	REQUIRE(json::parse(out, &back));
	REQUIRE(back.find("values") != nullptr);
	CHECK(back.find("name")->text == "FVERB \"hall\"\n");
	float mix = 0.0f;
	CHECK(back.find("values")->find("mix")->asFloat(&mix));
	CHECK(mix == 0.25f);
	CHECK(back.find("values")->find("mode")->text == "Plate");
}

TEST_CASE("json writer: misuse is seen — a member without a key, an unclosed object, two roots") {
	std::string out;
	{
		json::Writer w(&out);
		w.beginObject();
		w.number(1.0f);
		w.endObject();
		CHECK_FALSE(w.ok());
	}
	{
		json::Writer w(&out);
		w.beginObject();
		CHECK_FALSE(w.ok());
	}
	{
		json::Writer w(&out);
		w.null();
		w.null();
		CHECK_FALSE(w.ok());
	}
	{
		json::Writer w(&out);
		w.beginArray();
		w.key("a");
		w.endArray();
		CHECK_FALSE(w.ok());
	}
}

TEST_CASE("json writer: a string that is not UTF-8 is written as one that is, so the output always reads back") {
	std::string out;
	json::Writer w(&out, false);
	w.string(std::string("a\xff" "b\xc3\xa9" "\xed\xa0\x80", 8));
	REQUIRE(w.ok());
	CHECK(out == "\"a\\ufffdb\xc3\xa9\\ufffd\\ufffd\\ufffd\"");
	json::Value back;
	REQUIRE(json::parse(out, &back));
	CHECK(back.text == "a\xef\xbf\xbd" "b\xc3\xa9\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd");
}

TEST_CASE("json reader: the grammar is RFC 8259's and nothing looser") {
	const char* const bad[] = {
		"",
		"   ",
		"{",
		"}",
		"[1,]",
		"{\"a\":1,}",
		"{\"a\" 1}",
		"{a:1}",
		"{'a':1}",
		"[01]",
		"[1.]",
		"[.5]",
		"[+1]",
		"[NaN]",
		"[Infinity]",
		"[-Infinity]",
		"[nul]",
		"[truex]",
		"\"unterminated",
		"\"tab\there\"",
		"\"\\x41\"",
		"\"\\u12\"",
		"\"\\uD800\"",
		"\"\\uDC00\"",
		"\"\\uD800\\u0041\"",
		"\"\xff\"",
		"\"\xc0\xaf\"",
		"\"\xed\xa0\x80\"",
		"\"\xf4\x90\x80\x80\"",
		"{\"a\":1,\"a\":2}",
		"1 2",
		"[1] x",
		"/* comment */ 1",
	};
	for (const char* text : bad) {
		json::Value v;
		std::string error;
		INFO("text: " << text);
		CHECK_FALSE(json::parse(text, &v, {}, &error));
		CHECK_FALSE(error.empty());
	}

	json::Value v;
	REQUIRE(json::parse(" {\"s\": \"\\u00e9\\uD83D\\uDE00\\\"\\\\\\/\\b\\f\\n\\r\\t\", \"n\": -1.5e+3} ", &v));
	CHECK(v.find("s")->text == "\xc3\xa9\xf0\x9f\x98\x80\"\\/\b\f\n\r\t");
	CHECK(v.find("n")->text == "-1.5e+3"); // KEPT AS SPELLED
	double n = 0.0;
	CHECK(v.find("n")->asDouble(&n));
	CHECK(n == -1500.0);
	REQUIRE(json::parse("[true, false, null, 0, -0, \"\"]", &v));
	CHECK(v.items.size() == 6);
}

TEST_CASE("json reader: depth, size and node count are bounded before they are paid for") {
	json::Limits limits;
	limits.maxDepth = 8;
	std::string deep(8, '[');
	deep += std::string(8, ']');
	json::Value v;
	CHECK(json::parse(deep, &v, limits));
	std::string deeper(9, '[');
	deeper += std::string(9, ']');
	CHECK_FALSE(json::parse(deeper, &v, limits));

	// The default depth, against a document ten thousand levels deep — refused, not a stack overflow.
	std::string abyss(10000, '[');
	abyss += std::string(10000, ']');
	CHECK_FALSE(json::parse(abyss, &v));

	limits = {};
	limits.maxNodes = 100;
	std::string wide = "[";
	for (int i = 0; i < 200; ++i) {
		wide += i == 0 ? "0" : ",0";
	}
	wide += "]";
	CHECK_FALSE(json::parse(wide, &v, limits));
	limits.maxNodes = 1000;
	CHECK(json::parse(wide, &v, limits));

	limits = {};
	limits.maxBytes = 4;
	CHECK_FALSE(json::parse("[1,2]", &v, limits));
	CHECK(json::parse("[12]", &v, limits));
}

TEST_CASE("json $bin: bytes round-trip; a size that disagrees, or passes the ceiling, is refused before allocating") {
	std::mt19937 rng(7);
	for (size_t n = 0; n < 300; ++n) {
		std::vector<uint8_t> bytes(n);
		for (uint8_t& b : bytes) {
			b = static_cast<uint8_t>(rng());
		}
		std::string out;
		json::Writer w(&out, false);
		w.bytes(bytes);
		REQUIRE(w.ok());
		json::Value v;
		REQUIRE(json::parse(out, &v));
		CHECK(json::isBin(v));
		std::vector<uint8_t> back;
		REQUIRE(json::binOf(v, &back, 1024));
		CHECK(back == bytes);
	}

	json::Value v;
	std::vector<uint8_t> back;
	REQUIRE(json::parse(R"({"$bin": "AAEC", "size": 3})", &v));
	CHECK(json::binOf(v, &back, 3));
	CHECK(back == std::vector<uint8_t> { 0, 1, 2 });
	CHECK_FALSE(json::binOf(v, &back, 2)); // past the ceiling
	REQUIRE(json::parse(R"({"$bin": "AAEC", "size": 2})", &v));
	CHECK_FALSE(json::binOf(v, &back, 1024)); // disagrees
	REQUIRE(json::parse(R"({"$bin": "AAEC", "size": 18446744073709551615})", &v));
	CHECK_FALSE(json::binOf(v, &back, 1024)); // giant: refused on the number, never allocated
	REQUIRE(json::parse(R"({"$bin": "AAE=", "size": 2})", &v));
	CHECK(json::binOf(v, &back, 1024));
	REQUIRE(json::parse(R"({"$bin": "AAF=", "size": 2})", &v));
	CHECK_FALSE(json::binOf(v, &back, 1024)); // not canonical: unused bits set
	REQUIRE(json::parse(R"({"$bin": "AA=E", "size": 2})", &v));
	CHECK_FALSE(json::binOf(v, &back, 1024));
	REQUIRE(json::parse(R"({"$bin": "AAE", "size": 2})", &v));
	CHECK_FALSE(json::binOf(v, &back, 1024));
	REQUIRE(json::parse(R"({"$bin": 12, "size": 2})", &v));
	CHECK_FALSE(json::binOf(v, &back, 1024));
	REQUIRE(json::parse(R"({"size": 2})", &v));
	CHECK_FALSE(json::isBin(v));
}

TEST_CASE("json byte stability: what is read is written back unchanged, numbers as spelled") {
	// A value from a newer build — written by a writer we did not write — comes back out exactly.
	const std::string compact =
	    R"({"format":"feathers.rack","n":[1.50,-0,1E+2,0.000001],"s":"é\n","o":{"deep":[[{}],[]]},"t":true,"z":null})";
	json::Value v;
	REQUIRE(json::parse(compact, &v));
	CHECK(write(v, false) == "{\"format\":\"feathers.rack\",\"n\":[1.50,-0,1E+2,0.000001],\"s\":\"é\\n\","
	                         "\"o\":{\"deep\":[[{}],[]]},\"t\":true,\"z\":null}");
	const std::string indented = write(v);
	json::Value again;
	REQUIRE(json::parse(indented, &again));
	CHECK(write(again) == indented);
	CHECK(json::toCompactText(again) == write(v, false));
}

//------------------------------------------------------------------------------------------------//
// THE MUTATION FUZZER — §3.12 case 8. No clang or AFL on this host, so a seeded driver: every mutant   //
// must be a clean refusal or a clean load, never a crash, a hang or an allocation past the limits    //
// (the ASan gate runs this suite too). A load is then held to more: written back out, it must read   //
// again, and writing THAT must give the same bytes.                                                   //
//------------------------------------------------------------------------------------------------//

namespace {

std::vector<std::string> fuzzCorpus() {
	std::vector<std::string> corpus;
	{
		std::string doc;
		json::Writer w(&doc);
		w.beginObject();
		w.key("format");
		w.string("feathers.rack");
		w.key("chains");
		w.beginArray();
		w.beginArray();
		w.beginObject();
		w.key("tag");
		w.string("FVERB");
		w.key("values");
		w.beginObject();
		w.key("mix");
		w.number(0.35f);
		w.key("mode");
		w.string("Plate");
		w.key("gone");
		w.null();
		w.endObject();
		w.key("body");
		const uint8_t body[] = { 'F', 'I', 'R', '1', 1, 1, 0, 0, 0, 0, 0xff };
		w.bytes(body);
		w.endObject();
		w.endArray();
		w.endArray();
		w.key("routes");
		w.beginArray();
		w.beginObject();
		w.key("amount");
		w.number(-0.5f);
		w.endObject();
		w.endArray();
		w.endObject();
		corpus.push_back(doc);
	}
	corpus.push_back(R"(["\uD83D\uDE00", "\u00e9", "é", "\\", "\"", 1e-45, -3.4028235e+38, 0, -0])");
	corpus.push_back(R"({"a":{"b":{"c":{"d":{"e":[[[[[[[1]]]]]]]}}}}})");
	corpus.push_back(R"({"$bin":"SGVsbG8=","size":5})");
	corpus.push_back("[true,false,null,123456789012345678901234567890,1.5e308,\"x\"]");
	return corpus;
}

/// What the mutator splices in: the fragments a hostile or damaged file is made of.
constexpr const char* kSplices[] = {
	"{", "}", "[", "]", ",", ":", "\"", "\\", "\\u", "\\uD800", "\\uDC00", "\\uD800\\uDC00", "\xff", "\xc0\x80",
	"\xed\xa0\x80", "\xf4\x90\x80\x80", "\x01", "1e99999", "-0.0e-0", "123456789012345678901234567890",
	"NaN", "Infinity", "null", "true", "\"$bin\":\"////\",\"size\":99999999999", "[[[[[[[[[[[[[[[[",
	"{\"a\":{\"a\":{\"a\":{\"a\":", "\"size\":18446744073709551615",
};

std::string mutate(std::string s, std::mt19937& rng) {
	const int edits = 1 + static_cast<int>(rng() % 4);
	for (int e = 0; e < edits; ++e) {
		const size_t at = s.empty() ? 0 : rng() % (s.size() + 1);
		switch (rng() % 7) {
		case 0: // flip a byte
			if (!s.empty()) {
				s[at % s.size()] = static_cast<char>(s[at % s.size()] ^ static_cast<char>(1u << (rng() % 8)));
			}
			break;
		case 1: // replace a byte with anything
			if (!s.empty()) {
				s[at % s.size()] = static_cast<char>(rng());
			}
			break;
		case 2: // delete a run
			if (!s.empty()) {
				s.erase(at % s.size(), 1 + rng() % 8);
			}
			break;
		case 3: // truncate
			s.resize(at);
			break;
		case 4: // splice a fragment
			s.insert(at, kSplices[rng() % std::size(kSplices)]);
			break;
		case 5: // duplicate a run
			if (!s.empty()) {
				const size_t from = at % s.size();
				s.insert(at, s.substr(from, 1 + rng() % 32));
			}
			break;
		default: // nest deeply
			s.insert(at, std::string(1 + rng() % 200, rng() % 2 == 0 ? '[' : '{'));
			break;
		}
	}
	return s;
}

/// Every `$bin` in the tree, asked for its bytes under a small ceiling — a giant `size` must be refused
/// on the number and never allocated.
size_t decodeEveryBin(const json::Value& v) {
	size_t decoded = 0;
	if (json::isBin(v)) {
		std::vector<uint8_t> bytes;
		if (json::binOf(v, &bytes, 1u << 16)) {
			++decoded;
		}
		CHECK(bytes.size() <= (1u << 16));
	}
	for (const json::Value& item : v.items) {
		decoded += decodeEveryBin(item);
	}
	for (const json::Member& m : v.members) {
		decoded += decodeEveryBin(m.value);
	}
	return decoded;
}

} // namespace

TEST_CASE("json fuzz: every mutant is a clean refusal or a clean load, and a load writes back stably") {
	const std::vector<std::string> corpus = fuzzCorpus();
	std::mt19937 rng(0x5eed0f25);
	size_t loaded = 0;
	size_t refused = 0;
	size_t unstable = 0;
	std::string firstUnstable;
	for (int i = 0; i < 40000; ++i) {
		const std::string mutant = mutate(corpus[static_cast<size_t>(i) % corpus.size()], rng);
		json::Value v;
		std::string error;
		if (!json::parse(mutant, &v, {}, &error)) {
			++refused;
			continue;
		}
		++loaded;
		(void)decodeEveryBin(v);
		const std::string once = write(v);
		json::Value again;
		const bool reads = json::parse(once, &again);
		if (!reads || write(again) != once) {
			if (unstable == 0) {
				firstUnstable = mutant;
			}
			++unstable;
		}
	}
	INFO("first unstable mutant: " << firstUnstable);
	CHECK(unstable == 0);
	// THE DRIVER IS NOT VACUOUS: it reaches both outcomes in quantity.
	CHECK(loaded > 1000);
	CHECK(refused > 10000);
}
