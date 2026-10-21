#!/usr/bin/env python3
# Compile every drm-kmod source (drm, dmabuf, ttm, i915) for the XNU kernel target with -fsyntax-only and rank what
# breaks: missing headers first, then the most common first errors. The scoreboard is the X1 progress measure.
import argparse, collections, concurrent.futures as cf, json, os, re, subprocess, sys

SRC = os.path.expanduser("~/src/xe-src")
KMOD = os.path.join(SRC, "drm-kmod")
LKPI = os.path.join(SRC, "freebsd/sys/compat/linuxkpi")
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MODULES = {"drm": "drivers/gpu/drm", "dmabuf": "drivers/dma-buf", "ttm": "drivers/gpu/drm/ttm",
           "i915": "drivers/gpu/drm/i915"}


MAKEVARS = {"MACHINE_CPUARCH": "amd64", "MACHINE_ARCH": "amd64", "NO_FBDEV": "1"}
# drm-kmod features that have no counterpart on XNU
KCONFIG_DROP = {"DEBUG_FS", "PROC_FS", "DRM_PANIC", "VIDEO_CMDLINE", "MTRR", "DRM_I915_PXP", "HWMON"}


def make_cond(expr, kconfig):
    """Evaluate the subset of bmake conditionals drm-kmod uses; unknown forms count as false."""
    def term(t):
        t = t.strip()
        neg = t.startswith("!")
        t = t.lstrip("!").strip()
        m = re.match(r"^(empty|defined)\((\w+)(?::M(\S+))?\)$", t)
        if m:
            fn, var, pat = m.groups()
            if var == "KCONFIG" and pat:
                v = pat in kconfig
            else:
                v = var in MAKEVARS
            v = (not v) if fn == "empty" else v
        else:
            m = re.match(r'^\$\{(\w+)(?::M([^}]*))?\}\s*(==|!=)\s*"([^"]*)"$', t)
            if not m:
                return False
            var, pat, op, val = m.groups()
            cur = MAKEVARS.get(var, "")
            if pat:
                cur = cur if re.fullmatch(pat.replace("*", ".*"), cur) else ""
            v = (cur == val) if op == "==" else (cur != val)
        return v != neg
    return any(all(term(a) for a in o.split("&&")) for o in expr.split("||"))


def make_list(path, var, kconfig=None):
    """Values of VAR (and VAR+=) in a BSD makefile, following .if/.else/.endif."""
    out, cur, stack = [], None, []
    for line in open(path):
        line = line.split("#", 1)[0].rstrip()
        d = re.match(r"^\.\s*(if|elif|else|endif)\b(.*)$", line)
        if d:
            kw, expr = d.groups()
            if kw == "if":
                stack.append([make_cond(expr, kconfig or set()), False])
                stack[-1][1] = stack[-1][0]
            elif kw == "elif":
                take = not stack[-1][1] and make_cond(expr, kconfig or set())
                stack[-1] = [take, stack[-1][1] or take]
            elif kw == "else":
                stack[-1] = [not stack[-1][1], True]
            else:
                stack.pop()
            continue
        if not all(c[0] for c in stack):
            continue
        m = re.match(r"^(\w+)\s*(\+?=)\s*(.*)$", line)
        if m:
            cur = m.group(1) if m.group(1) == var else None
            line = m.group(3)
        if cur:
            cont = line.endswith("\\")
            out += line.rstrip("\\").split()
            if not cont:
                cur = None
    return out


def kconfig():
    return [v for v in make_list(os.path.join(KMOD, "kconfig.mk"), "KCONFIG") if v.partition("=")[0] not in KCONFIG_DROP]


def kconfig_defines():
    d = []
    for v in kconfig():
        k, _, val = v.partition("=")
        d.append(f"-DCONFIG_{k}={val or 1}")
    return d


def sources(mod):
    mk = os.path.join(KMOD, mod, "Makefile")
    paths = [os.path.join(KMOD, MODULES[mod])]
    for line in open(mk):
        m = re.match(r"^\s*(?:\.PATH:)?\s*\$\{SRCDIR\}(/\S+)", line)
        if m:
            paths.append(os.path.join(KMOD, MODULES[mod]) + m.group(1).rstrip("\\").strip())
    found = []
    for s in make_list(mk, "SRCS", {v.partition("=")[0] for v in kconfig()}):
        if not s.endswith(".c"):
            continue
        for p in paths:
            if os.path.exists(os.path.join(p, s)):
                found.append(os.path.join(p, s))
                break
    return found


def make_cflags(mod):
    """The module Makefile's own CFLAGS+= lines with its make variables resolved."""
    out, kmodname = [], make_list(os.path.join(KMOD, mod, "Makefile"), "KMOD")
    sub = {"${.CURDIR:H}": KMOD, "${SRCDIR:H:H}": os.path.join(KMOD, "drivers"),
           "${SRCDIR}": os.path.join(KMOD, MODULES[mod]), "${KMOD}": kmodname[0] if kmodname else mod}
    for line in open(os.path.join(KMOD, mod, "Makefile")):
        m = re.match(r"^CFLAGS\s*\+=\s*(.*)$", line.strip())
        if not m or "KCONFIG" in line or "SYSDIR" in line:
            continue
        v = m.group(1)
        for k, r in sub.items():
            v = v.replace(k, r)
        if "${" in v:
            continue
        out += [t.strip("'") for t in re.findall(r"'[^']*'|\S+", v)]
    return out


def flags(mod):
    inc = [os.path.join(ROOT, "shim/include"), os.path.join(KMOD, "linuxkpi/gplv2/include"),
           os.path.join(KMOD, "linuxkpi/bsd/include"), os.path.join(LKPI, "common/include"),
           os.path.join(LKPI, "dummy/include"), os.path.join(KMOD, "include"),
           os.path.join(KMOD, "include/uapi"), os.path.join(KMOD, "drivers/gpu/drm"), os.path.join(KMOD, "drivers/gpu"),
           os.path.join(ROOT, "shim/gen"),
           os.path.join(KMOD, MODULES[mod]),
           # FreeBSD's own kernel headers as the fallback, machine/ and x86/ mapped to amd64
           os.path.join(ROOT, "shim/fbsd"), os.path.join(SRC, "freebsd/sys"),
           os.path.join(SRC, "freebsd/sys/contrib/ck/include")]
    f = ["-target", "x86_64-apple-macos15.0", "-ffreestanding", "-nostdinc", "-fno-builtin", "-fsyntax-only",
         "-std=gnu11", "-D_KERNEL", "-DKLD_MODULE", "-D__FreeBSD__=15", "-U__APPLE__", "-U__MACH__", "-U__APPLE_CC__", "-D__amd64__", "-D__x86_64__", "-D__XNU_LKPI__", "-DLINUXKPI_VERSION=61400",
         "-include", os.path.join(LKPI, "common/include/linux/compiler_types.h"),
         "-include", os.path.join(LKPI, "common/include/linux/kconfig.h"),
         "-Wno-everything", "-ferror-limit=1"]
    return f + kconfig_defines() + make_cflags(mod) + [x for i in inc for x in ("-I", i)]


def compile_one(args):
    mod, path = args
    r = subprocess.run(["xcrun", "clang"] + flags(mod) + [path], capture_output=True, text=True)
    err = next((l for l in r.stderr.splitlines() if " error: " in l), "")
    return mod, os.path.relpath(path, KMOD), r.returncode == 0, err


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mods", default="drm,dmabuf,ttm,i915")
    ap.add_argument("--json", default=os.path.join(ROOT, "out/score.json"))
    a = ap.parse_args()
    jobs = [(m, p) for m in a.mods.split(",") for p in sources(m)]
    with cf.ThreadPoolExecutor(max_workers=os.cpu_count()) as ex:
        res = list(ex.map(compile_one, jobs))
    ok = collections.Counter(m for m, _, good, _ in res if good)
    tot = collections.Counter(m for m, _, _, _ in res)
    missing, kinds = collections.Counter(), collections.Counter()
    for _, _, good, e in res:
        if good:
            continue
        m = re.search(r"'([^']+)' file not found", e)
        if m:
            missing[m.group(1)] += 1
        else:
            kinds[re.sub(r"^.*? error: ", "", e)[:110]] += 1
    for m in tot:
        print(f"{m:7s} {ok[m]:4d}/{tot[m]:4d} compile")
    print(f"total   {sum(ok.values()):4d}/{sum(tot.values()):4d}")
    print("\nmissing headers (files blocked):")
    for h, n in missing.most_common(25):
        print(f"  {n:4d}  {h}")
    print("\nfirst errors:")
    for k, n in kinds.most_common(25):
        print(f"  {n:4d}  {k}")
    json.dump([dict(mod=m, file=f, ok=g, err=e) for m, f, g, e in res], open(a.json, "w"), indent=1)


if __name__ == "__main__":
    sys.exit(main())
