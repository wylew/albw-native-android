#!/usr/bin/env python3
"""Save states: the static data ranges that hold game state, from the GNU ld map of build/albw.

usage: tools/gen_ssmap.py <binary>.map <binary>  >  <binary>.ssmap   (tools/link.sh)
       tools/gen_ssmap.py <lib>.map <lib> --lld <hex load base>     (tools/link_android.sh)

Output (read by native/runtime/savestate.c at the first save / load):
    albw_data <hex address>                 the map belongs to this binary
    <hex start> <hex size> <object>         one line per range (adjacent input sections merged)

A range is every input section of the output sections .data and .bss (and COMMON) whose object
is one of ours (under build/) and not a host-side or separately-saved module:
  host side   native/platform/* (window, GL, input, menu, layout), native/render/* (GL renderer),
              native/audio/output.o (device ring), native/audio/fx.o, runtime prof / crash /
              fatal / entry / lowmem / savestate / ss_ctx
  own code    sdk/os.o, sdk/gfx.o, sdk/fs.o, sdk/memory.o (savestate.c saves them through their
              functions), sdk/snd.o and audio/mixer.o (their voices: albw_ss_add_range),
              sdk/light.o (a cache keyed by game addresses, rebuilt as needed)
Everything else - the game's generated data (albw_data / albw_bss), the decompiled objects, the
native game-function overrides, the remaining sdk modules - is saved."""
import os, re, sys

EXCLUDE = [
    r"/native/platform/", r"/native/render/", r"/native/audio/output\.o$", r"/native/audio/fx\.o$",
    r"/native/audio/mixer\.o$",
    r"/native/runtime/(prof|crash|fatal|entry|lowmem|savestate|ss_ctx|ss_convert|pmem)\.o$",
    # Android runtime: per-run handles (the log pipe's descriptors, the per-thread variables' pthread
    # key): restoring the saving run's values froze the game (A14) / crashed loads in ss_park (A15)
    r"/native/runtime/(android|android_watch|android_overflow|tlv)\.o$",
    r"/native/sdk/(os|gfx|fs|memory|snd|light)\.o$",
]

def main_lld(mp, base):
    """lld map (tools/link_android.sh): "VMA LMA Size Align Out/In/Symbol" rows; the output section
    name at the first indent, input sections "file:(section)" at the second, symbols at the third.
    `base`: the fixed load address of the shared library (android/loader/loader.c)."""
    ranges, albw_data, out_sec = [], None, None
    for line in open(mp, errors="replace"):
        m = re.match(r"^\s*([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+(\d+) (.*)$", line.rstrip("\n"))
        if not m:
            continue
        vma, size, rest = int(m.group(1), 16), int(m.group(3), 16), m.group(5)
        if not rest.startswith(" "):
            out_sec = rest.strip()
            continue
        if out_sec not in (".data", ".bss", ".albw_rw", ".albw_zi"):
            continue
        r = rest.strip()
        mi = re.match(r"^(.*):\((\S+)\)$", r)
        if not mi:
            if r == "albw_data":
                albw_data = vma + base
            continue
        obj = mi.group(1)
        if not size or not (obj.startswith("build/") or "/build/" in obj):
            continue
        if any(re.search(p, "/" + obj) for p in EXCLUDE):
            continue
        ranges.append([vma + base, size, os.path.basename(obj)])
    return ranges, albw_data

def main():
    if len(sys.argv) > 3 and sys.argv[3] == "--lld":
        base = int(sys.argv[4], 16) if len(sys.argv) > 4 else 0
        ranges, albw_data = main_lld(sys.argv[1], base)
        if albw_data is None:
            sys.exit("gen_ssmap: albw_data not in the map")
        return emit(ranges, albw_data)
    mp, binary = sys.argv[1], sys.argv[2]
    lines = open(mp, errors="replace").read().split("\n")
    out_sec = None
    ranges = []
    albw_data = None
    pending = None
    num = r"0x[0-9a-fA-F]+"
    for line in lines:
        m = re.match(r"^(\.\S+)\s+(%s)\s+(%s)" % (num, num), line)
        if m and not line.startswith(" "):
            out_sec = m.group(1)
            pending = None
            continue
        if re.match(r"^\S", line):
            out_sec = re.match(r"^(\S+)", line).group(1)
            pending = None
            continue
        if out_sec not in (".data", ".bss", ".albw_rw", ".albw_zi"):
            continue
        ms = re.match(r"^\s+(%s)\s+albw_data$" % num, line)
        if ms:
            albw_data = int(ms.group(1), 16)
            continue
        # input section: " .name  addr size file" or " .name" then "   addr size file"
        m = re.match(r"^ (\S+)\s+(%s)\s+(%s)\s+(\S.*)$" % (num, num), line)
        if not m and pending:
            m2 = re.match(r"^\s+(%s)\s+(%s)\s+(\S.*)$" % (num, num), line)
            if m2:
                m = (pending, m2.group(1), m2.group(2), m2.group(3))
            pending = None
        elif m:
            m = m.groups()
        else:
            m1 = re.match(r"^ (\.\S+|COMMON)\s*$", line)
            pending = m1.group(1) if m1 else None
            continue
        if not m:
            continue
        sec, addr, size, obj = m
        if not (sec.startswith(".data") or sec.startswith(".bss") or sec in (".albw_rw", ".albw_zi") or sec == "COMMON"):
            continue
        addr, size = int(addr, 16), int(size, 16)
        obj = obj.strip()
        if not size or "/build/" not in "/" + os.path.relpath(obj) and not obj.startswith("build/"):
            continue
        rel = "/" + obj
        if any(re.search(p, rel) for p in EXCLUDE):
            continue
        ranges.append([addr, size, os.path.basename(obj)])
    if albw_data is None:
        # the symbol line is "                0x09059a90                albw_data": find it anywhere
        for line in lines:
            ms = re.match(r"^\s+(%s)\s+albw_data\s*$" % num, line)
            if ms:
                albw_data = int(ms.group(1), 16)
                break
    if albw_data is None:
        sys.exit("gen_ssmap: albw_data not in the map")
    emit(ranges, albw_data)

def emit(ranges, albw_data):
    ranges.sort()
    merged = []
    for a, s, o in ranges:
        if merged and a <= merged[-1][0] + merged[-1][1]:   # exactly adjacent (a gap may hold an excluded object)
            end = max(merged[-1][0] + merged[-1][1], a + s)
            merged[-1][1] = end - merged[-1][0]
            if o not in merged[-1][2]:
                merged[-1][2] += "," + o if len(merged[-1][2]) < 200 else ""
        else:
            merged.append([a, s, o])
    print("albw_data %x" % albw_data)
    total = 0
    for a, s, o in merged:
        print("%x %x %s" % (a, s, o.replace(" ", "_")))
        total += s
    sys.stderr.write("gen_ssmap: %d ranges, %d KiB\n" % (len(merged), total // 1024))

main()
