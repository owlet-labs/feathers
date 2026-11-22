#!/usr/bin/env python3
# Static checks for the macOS-only CI failures that a Linux compile cannot see. Run by scripts/clang-preflight.sh.
#
# This file is part of Feathers.
# Copyright (C) 2026 Owlet Records — GPL-3.0-or-later.
#
# Two checks, each for a class that reached macOS CI first and is a property of the SOURCE, not of a compiler:
#
#  ALIGNED-ALLOC. `aligned_alloc` requires the size to be a multiple of the alignment. glibc tolerates a size that is not;
#  macOS returns null (a replaced `operator new` then throws bad_alloc). Every call must show the rounding in the few lines
#  above it, or sit in a helper that does. Rounding is recognised as `(n + a - 1) / a * a`, `& ~(a - 1)`, or a `% a` test.
#
#  PINNED-HASH. A bit-exact golden (a hash or bit-identical render) holds only where it was taken, Linux x86-64: libm and code
#  generation differ elsewhere. A test that asserts one must be (a) in a binary carrying the `platform-pinned` ctest label,
#  or (b) inside `#if FEATHERS_PINNED_PLATFORM` (tests/platform_pin.h), or after an early `return` under
#  `#if !FEATHERS_PINNED_PLATFORM`, or (c) say why it is platform-neutral with a `PLATFORM-NEUTRAL:` comment in the case.
#  A test written out as `defined(__x86_64__) && defined(__linux__)` is refused too: the preflight's second pass can only
#  force the macro, and a hand-written guard escapes it.
#
# The pinned-hash detector is textual: an assertion (CHECK/REQUIRE) naming a 64-bit literal, or a pinned/golden/hash
# identifier, inside a test case that renders or reads float bits. It is the shape that has broken CI, not a parser; a
# false positive is answered with the PLATFORM-NEUTRAL marker and its reason, which is then reviewable.
import bisect, glob, io, os, re, sys

root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
os.chdir(root)
bad = 0


def read(path):
	return io.open(path, encoding='utf-8', errors='ignore').read()


# ---- ALIGNED-ALLOC -------------------------------------------------------------------------------------------------
call = re.compile(r'(?<![\w])(?:std::)?aligned_alloc\s*\(')
rounded = re.compile(r'\+\s*\w+\s*-\s*1\s*\)\s*/\s*\w+\s*\*\s*\w+|&\s*~\s*\(|%\s*\w+\s*(?:!=|==)\s*0|bytes\s*%\s*\w+|size\s*%\s*\w+')
for path in sorted(glob.glob('src/**/*.[ch]pp', recursive=True) + glob.glob('src/**/*.h', recursive=True)
                   + glob.glob('tests/**/*.cpp', recursive=True) + glob.glob('tests/**/*.h', recursive=True)
                   + glob.glob('libs/feathers-device-sdk/**/*.h', recursive=True)):
	lines = read(path).split('\n')
	for i, ln in enumerate(lines):
		code = ln.split('//')[0]
		if not call.search(code):
			continue
		window = '\n'.join(l.split('//')[0] for l in lines[max(0, i - 12):i + 1])
		if not rounded.search(window):
			print('%s:%d: aligned_alloc with a size not visibly rounded to the alignment — macOS returns null for a size that is'
			      ' not a multiple of it (glibc tolerates it). Round it: n = (bytes + a - 1) / a * a; or use'
			      ' heapAlignedAlloc / portableAlignedAlloc (tests/test_heap.h, tests/portable_aligned_alloc.h).' % (path, i + 1))
			bad += 1

# ---- PINNED-HASH ---------------------------------------------------------------------------------------------------
# Binaries whose every case is pinned by ctest label: add_test(NAME x COMMAND test_y) + a platform-pinned set_tests_properties.
cm = read('tests/CMakeLists.txt')
pinned_names = set()
for m in re.finditer(r'set_tests_properties\(([^)]*?)PROPERTIES\s+LABELS\s+"[^"]*platform-pinned', cm):
	pinned_names.update(m.group(1).split())
pinned_files = set()
for m in re.finditer(r'add_test\(\s*NAME\s+(\S+)\s+COMMAND\s+([^\s)]+)', cm):
	if m.group(1) in pinned_names:
		pinned_files.add('tests/%s.cpp' % m.group(2))

lit = re.compile(r'(?<![\w.])(0[xX][0-9a-fA-F]{12,}[uUlL]*|\d{13,}[uU][lL]{0,2})(?![\w.])')
chk = re.compile(r'\b(CHECK|REQUIRE)(?:_EQ)?\s*\(')
name = re.compile(r'(\bkPinned\w*|\bk?[gG]olden\w*|\bpin\.hash\b|\.hash\b|\bexpected\w*[hH]ash|[hH]ash\w*\s*==|==\s*\w*[hH]ash\b)')
floaty = re.compile(r'(bit_cast|memcpy\s*\(\s*&|floatBits|bitsOf|asBits|toBits|\bprocess\s*\(|render)', re.I)
rawpin = re.compile(r'defined\s*\(\s*__x86_64__\s*\)\s*&&\s*defined\s*\(\s*__linux__\s*\)|defined\s*\(\s*__linux__\s*\)\s*&&\s*defined\s*\(\s*__x86_64__\s*\)')

for path in sorted(glob.glob('tests/*.cpp') + glob.glob('tests/gui_widgets/*.cpp')):
	lines = read(path).split('\n')
	for i, ln in enumerate(lines):
		t = ln.strip()
		if t.startswith('#') and re.match(r'#\s*(if|elif)', t) and rawpin.search(t):
			print('%s:%d: platform guard written out as __linux__ && __x86_64__ — use `#if FEATHERS_PINNED_PLATFORM`'
			      ' (tests/platform_pin.h), the preflight\'s second pass can only force that macro.' % (path, i + 1))
			bad += 1
	if path in pinned_files:
		continue
	starts = [i for i, l in enumerate(lines) if re.match(r'\s*(TEST_CASE|SCENARIO|TEST_CASE_TEMPLATE)\b', l)]
	stack = []
	early = {}      # case start -> line from which an early return under `#if !PIN` has taken the rest of the case
	opened = []     # (line, stack depth) of an open `#if !...PIN` block
	for i, l in enumerate(lines):
		t = l.strip()
		if re.match(r'#\s*if', t):
			stack.append(t)
		elif re.match(r'#\s*(elif|else)', t) and stack:
			stack[-1] += ' |' + t
		elif re.match(r'#\s*endif', t) and stack:
			stack.pop()
		code = l.split('//')[0]
		if not chk.search(code) or not (lit.search(code) or name.search(code)):
			continue
		g = ' >> '.join(stack)
		if 'FEATHERS_PINNED_PLATFORM' in g:
			continue
		k = bisect.bisect_right(starts, i) - 1
		a = starts[k] if k >= 0 else 0
		b = starts[k + 1] if k + 1 < len(starts) else len(lines)
		body = '\n'.join(lines[a:b])
		if not floaty.search(body):
			continue
		if 'PLATFORM-NEUTRAL' in body:
			continue
		# an early return under `#if !FEATHERS_PINNED_PLATFORM` earlier in the same case guards the rest of it
		before = '\n'.join(lines[a:i])
		if re.search(r'#\s*if\s+!\s*\(?\s*FEATHERS_PINNED_PLATFORM[^\n]*\n(?:(?!#\s*endif)[^\n]*\n)*?[^\n]*\breturn\b', before):
			continue
		print('%s:%d: %s — a golden hash or bit-exact render not guarded to Linux x86-64: wrap it in'
		      ' `#if FEATHERS_PINNED_PLATFORM`, or say why it is platform-neutral with a `PLATFORM-NEUTRAL:` comment in the case.'
		      % (path, i + 1, t[:90]))
		bad += 1

if bad:
	print()
	print('macos-static-checks: %d finding(s).' % bad)
	sys.exit(1)
print('macos-static-checks: aligned_alloc sizes rounded; pinned hashes guarded (%d platform-pinned binaries exempt).' % len(pinned_files))
