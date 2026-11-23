#!/usr/bin/env python3
"""ARM-only calling-convention mismatches between call sites and the definitions they reach
(README: Android port, boot stop A9).

    tools/abi_scan_arm.py [--jobs N] [--out build/android/abi_scan.json]

Why: the decompiled code often declares a function differently at a call site than where it is
defined (Ghidra prototypes, unprototyped `extern T f();` declarations, matched sources). On i386
every argument is a 4-byte-aligned stack word in order, so such a call still delivers the right
words, and the PC build works. On 32-bit Android (AAPCS softfp) arguments go to r0-r3 then the
stack, and 64-bit values (long long, double) start on an even register / 8-byte stack slot, so
the same call hands the callee different words: garbage volumes, a camera 27,000 units up...

How: every source the Android link uses (build/decomp-arm32 decomp sources, the native/ sources
link_android.sh compiles) goes to LLVM IR for armv7a softfp with the build's flags. From the IR:
each definition's parameter types (the linked definition: native code overrides decomp), and the
argument types of every direct call. Both sides are laid out as argument words twice, i386
(sequential) and ARM (AAPCS); a call is reported when the i386 layouts agree on every word the
callee reads but the ARM layouts do not.

Output: JSON {"mismatches": [{caller, callee, call: [types], def: [types], i386_words, ...}], ...}
and a summary on stdout. Needs build/decomp-arm32 (tools/build_decomp_arm32.sh) and the NDK.
"""
import argparse, collections, concurrent.futures as cf, glob, json, os, re, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
D = os.path.join(ROOT, "build", "decomp-arm32")
TARGET = "--target=armv7a-linux-androideabi24"
COMMON = [TARGET, "-S", "-emit-llvm", "-O0", "-g0", "-w", "-fno-strict-aliasing", "-fwrapv", "-fno-exceptions",
          "-mfpu=neon", "-mfloat-abi=softfp", "-ffp-contract=off", "-fPIC", "-fno-stack-protector", "-fsigned-char",
          "-ferror-limit=3"]
CFIX = ["-Wno-error=implicit-function-declaration", "-Wno-error=int-conversion", "-Wno-error=incompatible-pointer-types",
        "-Wno-error=incompatible-function-pointer-types", "-Wno-error=return-type"]
NATIVE_DIRS = ["game", "robust", "gameplay", "compilefix", "framework", "core", "route", "android", "hot"]


def ndk_clang():
    sdk = os.environ.get("ANDROID_HOME") or os.path.expanduser("~/Android/Sdk")
    ndks = sorted(glob.glob(os.path.join(sdk, "ndk", "*")))
    return os.path.join(ndks[-1], "toolchains", "llvm", "prebuilt", "linux-x86_64", "bin", "clang")


def decomp_jobs():
    jobs = []
    for path, addrs in json.load(open(os.path.join(ROOT, "decomp", "jobs.json"))):
        if not addrs:
            m = re.search(r"func_([0-9A-F]{8})", path)
            addrs = [m.group(1)] if m else []
        src = os.path.join(D, path)
        if os.path.exists(src):
            cpp = path.endswith((".cpp", ".cc"))
            flags = ["-include", os.path.join(ROOT, "native", "albw_native.h"), "-I", os.path.join(D, "shared", "include"),
                     "-I", os.path.dirname(src), "-DALBW_V4", "-ftrivial-auto-var-init=zero"]
            flags += ["-x", "c++", "-std=gnu++11", "-fno-threadsafe-statics"] if cpp else ["-std=gnu11"] + CFIX
            jobs.append(("decomp", src, addrs, flags))
    return jobs


def native_jobs():
    jobs = []
    inc = ["-I", os.path.join(ROOT, "native", "include")]
    sdl = ["-I", os.path.join(ROOT, "build", "deps", "sdl3-android-armv7", "include")]
    game = ["-DALBW_LINK_GAME_003E6AA8", "-DALBW_V4"] + sum((["-I", os.path.join(ROOT, "native", d)] for d in NATIVE_DIRS), []) + \
           ["-include", os.path.join(ROOT, "native", "albw_native.h"), "-I", os.path.join(D, "shared", "include")]
    base = ["-std=gnu11"] + CFIX + ["-DALBW_ANDROID", "-include", os.path.join(ROOT, "native", "runtime", "android_compat.h")]
    files = [os.path.join(ROOT, "native", "platform", "platform.c")]
    for d in ["runtime", "render", "sdk"] + NATIVE_DIRS:
        files += sorted(glob.glob(os.path.join(ROOT, "native", d, "*.c")))
    files += [os.path.join(ROOT, "native", "audio", f) for f in ("mixer.c", "fx.c", "output.c")]
    for f in files:
        rel = os.path.relpath(f, os.path.join(ROOT, "native")).split(os.sep)[0]
        jobs.append(("native", f, [], base + inc + sdl + (game if rel in NATIVE_DIRS else [])))
    return jobs


RE_DEFINE = re.compile(r"^define\s+(?:[\w\(\)]+\s+)*?(?P<ret>[^@]+?)\s+@(?P<name>[\w.$\"]+)\((?P<args>.*?)\)\s*(?:#\d+\s*)?(?:local_unnamed_addr\s*)?(?:#\d+\s*)?\{",)
RE_CALL = re.compile(r"(?:call|invoke)\s+(?:\w+\s+)*?(?P<ret>[^@%]+?)\s+(?:\((?P<fty>[^)]*)\)\s+)?@(?P<name>[\w.$]+)\((?P<args>.*)\)")


def split_top(s):
    """split an IR argument list at top-level commas"""
    out, depth, cur = [], 0, []
    for ch in s:
        if ch in "([{<":
            depth += 1
        elif ch in ")]}>":
            depth -= 1
        if ch == "," and depth == 0:
            out.append("".join(cur).strip()); cur = []
        else:
            cur.append(ch)
    if "".join(cur).strip():
        out.append("".join(cur).strip())
    return out


def arg_type(a):
    """the IR type of one argument / parameter, plus sret / byval flags"""
    a = a.strip()
    if a.startswith("..."):
        return None
    m = re.match(r"(\[\d+ x [\w.]+\]|\{[^}]*\}|<[^>]*>|[\w.%]+)", a)
    t = m.group(1) if m else a.split()[0]
    sret = " sret(" in a or a.startswith("ptr sret")
    byval = " byval(" in a
    return (t, sret, byval)


def words(t):
    """(size in 4-byte words, 8-byte aligned on ARM) of an IR type"""
    if t in ("i64", "double"):
        return 2, True
    m = re.match(r"\[(\d+) x ([\w.]+)\]", t)
    if m:
        n, e = int(m.group(1)), m.group(2)
        return (2 * n, True) if e in ("i64", "double") else (n, False)
    return 1, False


def layouts(params):
    """per parameter: ARM word range and i386 word range in a virtual argument area (r0-r3 =
    words 0-3, then the stack); i386: stack words from 0"""
    arm, x86 = [], []
    ncrn, nsaa, x = 0, 0, 0   # next core register, next stack word (ARM); next stack word (i386)
    for t, sret, byval in params:
        n, al = words(t) if not byval else (1, False)
        x86.append((x, n)); x += n
        if al and ncrn % 2:
            ncrn += 1
        if ncrn + n <= 4:
            arm.append((ncrn, n)); ncrn += n
        elif ncrn < 4 and not al:
            # AAPCS: a non-8-byte-aligned aggregate may be split between registers and stack
            arm.append((ncrn, n)); nsaa += n - (4 - ncrn); ncrn = 4
        else:
            ncrn = 4
            if al and nsaa % 2:
                nsaa += 1
            arm.append((4 + nsaa, n)); nsaa += n
    return arm, x86


def scan_one(job, clang):
    kind, src, addrs, flags = job
    with tempfile.NamedTemporaryFile(suffix=".ll", delete=False) as tf:
        out = tf.name
    try:
        r = subprocess.run([clang] + COMMON + flags + [src, "-o", out], capture_output=True, text=True)
        if r.returncode:
            return {"src": src, "err": (r.stderr.strip().splitlines() or ["?"])[0][:200]}
        defs, rets, calls = {}, {}, collections.defaultdict(set)
        for line in open(out, errors="ignore"):
            if line.startswith("define "):
                if " internal " in line.split("@")[0] or " private " in line.split("@")[0]:
                    continue
                m = RE_DEFINE.match(line)
                if m:
                    defs[m.group("name").strip('"')] = [arg_type(a) for a in split_top(m.group("args")) if arg_type(a)]
                    rets[m.group("name").strip('"')] = m.group("ret").split()[-1]
            elif ("call " in line or "invoke " in line) and "@" in line:
                m = RE_CALL.search(line)
                if not m or m.group("name").startswith("llvm."):
                    continue
                args = [arg_type(a) for a in split_top(m.group("args")) if arg_type(a)]
                calls[m.group("name")].add(tuple(args))
        return {"src": src, "kind": kind, "addrs": addrs, "defs": defs, "rets": rets, "calls": {k: sorted(v) for k, v in calls.items()}}
    finally:
        try: os.remove(out)
        except OSError: pass


def def_symbol(name, addrs, ndefs):
    """the exported func_<ADDR> a decomp definition becomes (build_decomp primary symbol)"""
    if re.fullmatch(r"func_[0-9A-F]{8}", name):
        return name
    for a in addrs:
        short = a.lstrip("0").upper()
        if a.upper() in name.upper() or (short and re.search(r"(?<![0-9A-Fa-f])%s(?![0-9A-Fa-f])" % short, name.upper())):
            return "func_" + a.upper()
    if len(addrs) == 1 and ndefs == 1:
        return "func_" + addrs[0].upper()
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--jobs", type=int, default=os.cpu_count())
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "android", "abi_scan.json"))
    ap.add_argument("--limit", type=int, default=0, help="debug: scan only the first N sources")
    a = ap.parse_args()
    clang = ndk_clang()
    jobs = native_jobs() + decomp_jobs()
    if a.limit:
        jobs = jobs[:a.limit]
    print("scanning %d sources with %s" % (len(jobs), clang), flush=True)
    res = []
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        for i, r in enumerate(ex.map(lambda j: scan_one(j, clang), jobs)):
            res.append(r)
            if (i + 1) % 4000 == 0:
                print("  %d / %d" % (i + 1, len(jobs)), flush=True)
    errs = [r for r in res if "err" in r]
    # definitions: native wins over decomp (strong vs weak)
    defs, def_src = {}, {}
    for kind in ("decomp", "native"):
        for r in res:
            if "err" in r or r["kind"] != kind:
                continue
            for name, params in r["defs"].items():
                sym = name if kind == "native" else def_symbol(name, r["addrs"], len(r["defs"]))
                if sym:
                    defs[sym] = params; def_src[sym] = r["src"]
    mism, checked = [], 0
    for r in res:
        if "err" in r:
            continue
        for callee, sigs in r["calls"].items():
            d = defs.get(callee)
            if d is None:
                continue
            for call in sigs:
                call = [tuple(x) for x in call]
                checked += 1
                if [t for t, _, _ in call] == [t for t, _, _ in d]:
                    continue
                ca, cx = layouts(call)
                da, dx = layouts(d)
                # words the callee reads, by parameter
                arm_ok = all(k < len(ca) and ca[k] == da[k] for k in range(len(da)))
                if arm_ok:
                    continue
                # i386: every callee parameter covers the same stack words the caller wrote there
                cwords_x = sum(n for _, n in cx)
                x86_ok = all(off + n <= cwords_x for off, n in dx)
                # word-for-word identical i386 area up to what the callee reads
                x86_ok = x86_ok and [w for off, n in cx for w in range(off, off + n)][:sum(n for _, n in dx)] == \
                    list(range(sum(n for _, n in dx)))
                mism.append({"caller": os.path.relpath(r["src"], ROOT), "callee": callee,
                             "def_src": os.path.relpath(def_src[callee], ROOT),
                             "call": [t for t, _, _ in call], "def": [t for t, _, _ in d],
                             "call_sret": [s for _, s, _ in call], "def_sret": [s for _, s, _ in d],
                             "arm_call": ca, "arm_def": da, "x86_ok": x86_ok})
    arm_only = [m for m in mism if m["x86_ok"]]
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    json.dump({"checked_calls": checked, "definitions": len(defs), "errors": [(e["src"], e["err"]) for e in errs],
               "mismatches": mism}, open(a.out, "w"), indent=1)
    print("sources %d (IR failed %d), definitions %d, direct calls checked %d" % (len(res), len(errs), len(defs), checked))
    print("ARM layout differs: %d call sites; of those the i386 words line up (works on the PC, not on Android): %d"
          % (len(mism), len(arm_only)))
    pat = collections.Counter("call (%s) -> def (%s)" % (", ".join(m["call"]), ", ".join(m["def"])) for m in arm_only)
    for k, v in pat.most_common(15):
        print("  %4d  %s" % (v, k))
    print("written", a.out)


if __name__ == "__main__":
    main()
