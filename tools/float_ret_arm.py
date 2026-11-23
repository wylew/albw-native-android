#!/usr/bin/env python3
"""Returns-this functions whose definition returns a float or double (README: Android stop A9).

    tools/float_ret_arm.py > build/android/float_ret.txt      (tools/link_android.sh runs it)

Exact: each source of a returns-this function (decomp/returns_this.json, native/returns_this3.json)
is compiled to LLVM IR (tools/abi_scan_arm.py flags) and the return type read from the definition
that becomes func_<ADDR>. Cached by source mtime in build/android/float_ret_cache.json. Prints the
func_<ADDR> names (one per line); those get a tail branch instead of a returns-this thunk on ARM
(softfp returns floats in r0, where the thunk puts `this`)."""
import concurrent.futures as cf, json, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import abi_scan_arm as A

ROOT = A.ROOT
cache_p = os.path.join(ROOT, "build", "android", "float_ret_cache.json")
try:
    cache = json.load(open(cache_p))
except (OSError, ValueError):
    cache = {}
want = set(json.load(open(os.path.join(ROOT, "decomp", "returns_this.json"))))
rt3 = os.path.join(ROOT, "native", "returns_this3.json")
if os.path.exists(rt3):
    want |= set(json.load(open(rt3)))
jobs = [j for j in A.decomp_jobs() if set(j[2]) & want]
clang = A.ndk_clang()

def ret_types(job):
    src = job[1]
    key = "%s:%d" % (src, int(os.path.getmtime(src)))
    if key in cache:
        return key, cache[key]
    r = A.scan_one(job, clang)
    out = {}
    if "err" not in r:
        for name, ret in r["rets"].items():
            sym = A.def_symbol(name, job[2], len(r["defs"]))
            if sym:
                out[sym] = ret
    return key, out

found = set()
with cf.ThreadPoolExecutor(os.cpu_count()) as ex:
    for key, out in ex.map(ret_types, jobs):
        cache[key] = out
        for sym, ret in out.items():
            if ret in ("float", "double") and sym[5:] in want:
                found.add(sym)
os.makedirs(os.path.dirname(cache_p), exist_ok=True)
json.dump(cache, open(cache_p, "w"))
print("\n".join(sorted(found)))
print("float_ret_arm: %d returns-this sources, %d return float/double" % (len(jobs), len(found)), file=sys.stderr)
