#!/usr/bin/env python3
# ==========================================================================
#  PWRadarSystem - build gate
#  File    : tools/check_name_collisions.py
#  Purpose : Prove that no identifier exported by the public PWRadarCore
#            headers is also an object-like macro in the Windows SDK.
#
#  Why this exists
#  ---------------
#  <winuser.h> contains, for the legacy WM_POWER broadcast:
#
#      #define PWR_OK              1
#      #define PWR_FAIL            (-1)
#      #define PWR_SUSPENDREQUEST  1
#      #define PWR_CRITICALRESUME  3
#
#  A macro beats an enumerator no matter the include order, so an enumerator
#  named PWR_OK is textually rewritten to `1` in every translation unit that
#  reaches <windows.h>.  `return PWR_OK;` then means `return 1;`, every
#  `status != PWR_OK` test fires, and the failure surfaces a long way from the
#  cause - in our case as "engine creation failed: thread failure" from a
#  mutex that had initialised perfectly.  The Linux build is unaffected, so
#  CI on Linux alone will never see it.
#
#  The library therefore uses PWR_STATUS_OK, and this script keeps every other
#  public name honest.  It is a build gate, not a lint: exit code 1 means the
#  header set would miscompile on Windows.
#
#  Usage
#  -----
#      python3 tools/check_name_collisions.py [--sdk DIR]... [--quiet]
#
#  With no --sdk the script auto-discovers header roots:
#    * Windows : the Windows Kits 10 "um"/"shared"/"ucrt" directories and the
#                MSVC toolchain include directory.
#    * Linux   : the mingw-w64 include tree, which mirrors the SDK macro set
#                closely enough to catch collisions of this class.
#  Exit codes: 0 = clean, 1 = collision found, 2 = no SDK headers located.
# ==========================================================================

from __future__ import annotations

import argparse
import os
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# Header sets whose identifiers must survive contact with <windows.h>.
PUBLIC_HEADER_DIRS = [
    REPO_ROOT / "PWRadarCore" / "include" / "pwradar",
]

# Candidate Windows SDK / mingw header roots, most specific first.
SDK_CANDIDATES_POSIX = [
    "/usr/share/mingw-w64/include",
    "/usr/x86_64-w64-mingw32/include",
    "/usr/i686-w64-mingw32/include",
    "/usr/local/mingw-w64/include",
]

# An object-like macro definition: `#define NAME <replacement>` with no '('
# immediately after NAME.  Function-like macros cannot shadow an enumerator
# used as a plain value, so they are not a hazard and are skipped.
RE_OBJECT_MACRO = re.compile(
    r"^[ \t]*#[ \t]*define[ \t]+([A-Za-z_][A-Za-z0-9_]*)(?![A-Za-z0-9_(])"
)

# Identifiers we declare.  Anything that could be a macro target: enumerators,
# typedef names, struct tags, function names, and our own macros.
RE_IDENTIFIER = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\b")

# Our own macro definitions - a name we #define ourselves is allowed to be
# redefined-checked by the compiler, and the tripwire in the header handles it.
RE_OWN_DEFINE = re.compile(
    r"^[ \t]*#[ \t]*define[ \t]+([A-Za-z_][A-Za-z0-9_]*)"
)

# Comments and string/char literals are stripped before identifier extraction
# so that prose such as "PWR_OK" inside the rationale comment is not treated
# as a declared identifier.  Without this the script would flag its own
# documentation.
#
# Backslash-newline splicing (translation phase 2) has to happen FIRST.  A
# multi-line `#error "... \<newline> ..."` string is a single literal to the
# compiler, but a newline-naive string regex stops at the first line break and
# leaves the remaining prose exposed as bare words.  Those words then enter the
# identifier set, and any one of them that happens to match an SDK macro fails
# the build for no reason.  Splice, then strip.
RE_LINE_SPLICE = re.compile(r"\\\n")
RE_BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.DOTALL)
RE_LINE_COMMENT = re.compile(r"//[^\n]*")
# `\\[\s\S]` rather than `\\.`: an escape may legitimately span a newline.
RE_STRING = re.compile(r'"(?:[^"\\]|\\[\s\S])*"')
RE_CHAR = re.compile(r"'(?:[^'\\]|\\[\s\S])*'")

# C keywords and standard-library spellings: never our identifiers, and some
# (e.g. `const`) legitimately appear as macros in odd SDK corners.
C_KEYWORDS = frozenset(
    """
    alignas alignof auto bool break case char const constexpr continue default
    do double else enum extern false float for goto if inline int long
    noreturn nullptr register restrict return short signed sizeof static
    static_assert struct switch thread_local true typedef typeof union
    unsigned void volatile while _Alignas _Alignof _Atomic _Bool _Complex
    _Generic _Imaginary _Noreturn _Static_assert _Thread_local
    size_t ptrdiff_t intptr_t uintptr_t
    int8_t int16_t int32_t int64_t uint8_t uint16_t uint32_t uint64_t
    float_t double_t va_list FILE
    """.split()
)

# Preprocessor directive and operator spellings.  These appear after '#' in our
# own headers but are not things we declare, so a same-named SDK macro is not a
# collision we could ever suffer from.
PP_WORDS = frozenset(
    """
    define defined undef include include_next if ifdef ifndef elif elifdef
    elifndef else endif error warning pragma line embed once
    """.split()
)

# Compiler vocabulary the public headers *reference* but never *declare*:
# platform test macros and calling-convention keywords.  These are supposed to
# be macros (newer SDKs define _WIN32 in minwindef.h and __cdecl in ntdef.h as
# object-like macros), so shadowing is their normal operation, not a hazard.
COMPILER_PREDEFINED = frozenset(
    """
    _WIN32 _WIN64 _MSC_VER __cdecl __stdcall __fastcall __vectorcall
    __declspec __attribute__ __GNUC__ __clang__ __cplusplus
    """.split()
)


def splice_continuations(text: str) -> str:
    """Translation phase 2: join backslash-newline pairs."""
    return RE_LINE_SPLICE.sub("", text)


def strip_noise(text: str) -> str:
    """Remove comments and literals, preserving line structure for #define."""
    text = splice_continuations(text)
    text = RE_BLOCK_COMMENT.sub(lambda m: "\n" * m.group(0).count("\n"), text)
    text = RE_LINE_COMMENT.sub("", text)
    text = RE_STRING.sub('""', text)
    text = RE_CHAR.sub("''", text)
    return text


def collect_public_identifiers() -> tuple[dict[str, set[str]], set[str]]:
    """Return (identifier -> {header names}, names we #define ourselves)."""
    declared: dict[str, set[str]] = {}
    own_defines: set[str] = set()

    for directory in PUBLIC_HEADER_DIRS:
        if not directory.is_dir():
            continue
        for header in sorted(directory.rglob("*.h")):
            raw = header.read_text(encoding="utf-8", errors="replace")
            # Splice first so a continued #define is seen as one logical line.
            for line in splice_continuations(raw).splitlines():
                m = RE_OWN_DEFINE.match(line)
                if m:
                    own_defines.add(m.group(1))
            clean = strip_noise(raw)
            rel = header.relative_to(REPO_ROOT).as_posix()
            for name in RE_IDENTIFIER.findall(clean):
                if (name in C_KEYWORDS or name in PP_WORDS
                        or name in COMPILER_PREDEFINED):
                    continue
                declared.setdefault(name, set()).add(rel)

    return declared, own_defines


def discover_sdk_roots(explicit: list[str]) -> list[Path]:
    if explicit:
        return [Path(p) for p in explicit if Path(p).is_dir()]

    roots: list[Path] = []

    if os.name == "nt":
        program_files = os.environ.get("ProgramFiles", r"C:\Program Files")
        # 'ProgramFiles(x86)' cannot be read via %VAR% in cmd without quoting
        # games, but os.environ handles the parentheses fine.
        program_files_x86 = os.environ.get(
            "ProgramFiles(x86)", r"C:\Program Files (x86)"
        )
        for base in (program_files_x86, program_files):
            kits = Path(base) / "Windows Kits" / "10" / "Include"
            if kits.is_dir():
                # Newest SDK version wins; older ones are supersets rarely.
                versions = sorted(
                    (d for d in kits.iterdir() if d.is_dir()),
                    key=lambda d: d.name,
                    reverse=True,
                )
                if versions:
                    roots.append(versions[0])
        include = os.environ.get("INCLUDE", "")
        for part in include.split(os.pathsep):
            part = part.strip()
            if part and Path(part).is_dir():
                roots.append(Path(part))
    else:
        for candidate in SDK_CANDIDATES_POSIX:
            if Path(candidate).is_dir():
                roots.append(Path(candidate))

    # De-duplicate while preserving order.
    seen: set[Path] = set()
    unique: list[Path] = []
    for root in roots:
        resolved = root.resolve()
        if resolved not in seen:
            seen.add(resolved)
            unique.append(root)
    return unique


def collect_sdk_macros(roots: list[Path]) -> tuple[dict[str, str], int]:
    """Return (macro name -> 'file:line' of first definition, files scanned)."""
    macros: dict[str, str] = {}
    scanned = 0

    for root in roots:
        for path in root.rglob("*"):
            if not path.is_file() or path.suffix.lower() not in (".h", ".hpp", ""):
                continue
            if path.suffix == "" and path.name.lower() not in ("stdio", "stdlib"):
                # Extension-less C++ standard headers hold no Win32 macros.
                continue
            try:
                text = path.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            scanned += 1
            for lineno, line in enumerate(text.splitlines(), start=1):
                if "define" not in line:
                    continue
                m = RE_OBJECT_MACRO.match(line)
                if m:
                    macros.setdefault(m.group(1), f"{path}:{lineno}")

    return macros, scanned


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Fail the build if a public PWRadar identifier collides "
        "with a Windows SDK object-like macro."
    )
    parser.add_argument(
        "--sdk",
        action="append",
        default=[],
        metavar="DIR",
        help="Header root to scan (repeatable). Overrides auto-discovery.",
    )
    parser.add_argument(
        "--quiet", action="store_true", help="Print only on failure."
    )
    args = parser.parse_args()

    declared, own_defines = collect_public_identifiers()
    if not declared:
        print("check_name_collisions: no public headers found", file=sys.stderr)
        return 2

    roots = discover_sdk_roots(args.sdk)
    if not roots:
        print(
            "check_name_collisions: no Windows SDK / mingw-w64 headers found; "
            "pass --sdk DIR to scan an explicit tree. Skipping (not a failure "
            "on hosts without a Windows toolchain).",
            file=sys.stderr,
        )
        return 0

    macros, files_scanned = collect_sdk_macros(roots)

    collisions = sorted(
        (name, sorted(headers), macros[name])
        for name, headers in declared.items()
        if name in macros and name not in own_defines
    )

    if not args.quiet:
        print(f"check_name_collisions: {len(declared)} public identifiers")
        print(
            f"check_name_collisions: {len(macros)} object-like macros from "
            f"{files_scanned} headers in {len(roots)} root(s)"
        )
        for root in roots:
            print(f"    {root}")

    if collisions:
        print("")
        print("COLLISION: public identifiers shadowed by Windows SDK macros")
        print("-" * 74)
        for name, headers, where in collisions:
            print(f"  {name}")
            print(f"      declared in : {', '.join(headers)}")
            print(f"      macro at    : {where}")
        print("-" * 74)
        print(
            "Rename the identifier. Do NOT #undef the macro: the collision "
            "returns\nthe moment include order changes, and #undef breaks the "
            "SDK consumer that\nwanted the macro."
        )
        return 1

    if not args.quiet:
        print("check_name_collisions: OK - no collisions")
    return 0


if __name__ == "__main__":
    sys.exit(main())
