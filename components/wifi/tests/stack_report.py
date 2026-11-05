# SPDX-License-Identifier: Apache-2.0
"""Stack frames of the code that runs in the event loop task (README.md: "Stack of the
event loop task").

Compiles src/ with the host compiler and -fstack-usage (ESP-IDF itself is
replaced by tests/idf_stub/), at -Og (ESP-IDF's default) and -Os, reads the
frame of every function and the calls between them from the assembly, and
prints a Markdown table: every module function reachable from the event loop
task's entry points, its frame, and the deepest chain of module frames.
Calls through the wl_ctl_ops_t pointers count as calls to every op_*.

Fails (exit 1) on recursion, on a chain over 1024 bytes, on a single frame
over 256 bytes, or on a call to settings_* or nvs_* from that code. Host frames (arm64 or x86-64, 8-byte pointers, on macOS every
variadic argument on the stack) are not the Xtensa ones; they show the size
of the module's own frames, not ESP-IDF's (logging, Wi-Fi calls).

Usage: python3 -I tests/stack_report.py [cc [src_dir]]
"""

import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCES = ["wifi.c", "wifi_ctl.c", "wifi_logic.c", "wifi_saved.c"]
# Entry points that run in the event loop task: the handler of all events
# (registered in wifi_init_apsta()) and the timer callback (esp_timer task,
# listed by README.md with the event loop task's code).
ROOTS = ["on_event", "timer_cb"]
INDIRECT_FROM = "wifi_ctl.c"  # its calls through ops go to wifi.c's op_*
CHAIN_LIMIT = 1024
FRAME_LIMIT = 256
LEVELS = ["-Og", "-Os"]
FORBIDDEN = ("settings_", "nvs_")  # only wifi_init_apsta() reads settings


def compile_level(cc, src_dir, level, out):
    frames, edges, indirect, owner = {}, {}, set(), {}
    for src in SOURCES:
        base = os.path.join(out, src[:-2] + level)
        args = [
            cc, "-std=c11", level, "-S", "-fstack-usage",
            "-fno-asynchronous-unwind-tables", "-Wall", "-Wextra", "-Werror",
            "-Wshadow", "-Wformat=2", "-Wno-format-nonliteral",
            "-I" + os.path.join(ROOT, "include"),
            "-I" + src_dir,
            "-I" + os.path.join(ROOT, "tests", "stubs"),
            "-I" + os.path.join(ROOT, "tests", "idf_stub"),
            "-I" + os.path.join(ROOT, "..", "..", "common"),
            "-I" + os.path.join(ROOT, "..", "..", "common", "network"),
            "-o", base + ".s", os.path.join(src_dir, src),
        ]
        subprocess.run(args, check=True)
        with open(base + ".su") as f:
            for line in f:
                where, size, _kind = line.rstrip("\n").split("\t")
                name = where.rsplit(":", 1)[-1]
                frames[name] = int(size)
                owner[name] = src
        func = None
        with open(base + ".s") as f:
            for line in f:
                m = re.match(r"^_?([A-Za-z_][A-Za-z0-9_.]*):", line)
                if m and not line.startswith(("L", ".L", "l_")):
                    func = m.group(1)
                    edges.setdefault(func, set())
                    continue
                if func is None:
                    continue
                ins = line.split()
                if not ins:
                    continue
                op = ins[0]
                if op in ("bl", "b", "call", "callq", "jmp") and len(ins) > 1:
                    target = ins[1].lstrip("_").split("@")[0]
                    edges[func].add(target)
                elif op in ("blr", "br") or (
                        op in ("call", "callq", "jmp") and len(ins) > 1
                        and ins[1].startswith("*")):
                    indirect.add(func)
    ops = {f for f in frames if f.startswith("op_")}
    for f in indirect:
        if owner.get(f) == INDIRECT_FROM:
            edges[f] |= ops
    return frames, edges


def deepest(frames, edges, func, stack, memo):
    if func in stack:
        raise RuntimeError("recursion: " + " -> ".join(stack + [func]))
    if func in memo:
        return memo[func]
    best = (0, [])
    for callee in sorted(edges.get(func, ())):
        if callee in frames:
            best = max(best, deepest(frames, edges, callee, stack + [func],
                                     memo), key=lambda x: x[0])
    memo[func] = (frames[func] + best[0], [func] + best[1])
    return memo[func]


def reachable(frames, edges):
    seen, todo = set(), list(ROOTS)
    while todo:
        f = todo.pop()
        if f in seen or f not in frames:
            continue
        seen.add(f)
        todo.extend(edges.get(f, ()))
    return seen


def main():
    cc = sys.argv[1] if len(sys.argv) > 1 else "cc"
    src_dir = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "src")
    ok = True
    results = {}
    with tempfile.TemporaryDirectory() as out:
        for level in LEVELS:
            frames, edges = compile_level(cc, src_dir, level, out)
            funcs = reachable(frames, edges)
            memo = {}
            try:
                chains = [deepest(frames, edges, r, [], memo) for r in ROOTS
                          if r in frames]
            except RuntimeError as e:
                print("FAIL " + level + ": " + str(e))
                return 1
            for f in sorted(funcs):
                for callee in sorted(edges.get(f, ())):
                    if callee.startswith(FORBIDDEN):
                        print("FAIL " + level + ": " + f + " calls " + callee +
                              " in the event loop task")
                        ok = False
            results[level] = (frames, funcs, max(chains, key=lambda c: c[0]))

    names = sorted(set().union(*(r[1] for r in results.values())))
    print("| Function | " + " | ".join(LEVELS) + " |")
    print("|---|" + "---:|" * len(LEVELS))
    for n in names:
        cells = []
        for level in LEVELS:
            frames, funcs, _ = results[level]
            if n in funcs:
                cells.append(str(frames[n]))
                if frames[n] > FRAME_LIMIT:
                    print("FAIL: " + n + " over " + str(FRAME_LIMIT) + " B")
                    ok = False
            else:
                cells.append("inlined" if n not in frames else "-")
        print("| `" + n + "` | " + " | ".join(cells) + " |")
    print()
    for level in LEVELS:
        total, path = results[level][2]
        print("Deepest chain " + level + ": " + str(total) + " B: " +
              " -> ".join(path))
        if total > CHAIN_LIMIT:
            print("FAIL: over " + str(CHAIN_LIMIT) + " B")
            ok = False
    print()
    print("OK" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
