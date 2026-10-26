#!/usr/bin/env python3
"""Check documentation/lua-api/vba.lua against what the engine registers.

The list of what exists is the luaL_Reg tables at the end of
src/common/lua-engine.cpp plus the lua_register() calls; the type definitions
have to name exactly the same functions.  A function added to one side and not
the other is a silent lie in the editor, so this compares the two lists.

    python check_api.py           # report, exit 1 on any difference
    python check_api.py --quiet   # only print the differences

Exit code 0 when both sides agree, 1 otherwise.
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))          # documentation/lua-api -> repo root
ENGINE = os.path.join(ROOT, "src", "common", "lua-engine.cpp")
META = os.path.join(HERE, "vba.lua")

# A registered array is named after its table plus "lib", except for these.
TABLE_OF = {
    "vba": "emu",        # the table is registered as both emu and vba
}

# Registered globally, but already declared by the Lua 5.1 standard library
# meta file, so redeclaring them here would only cause duplicate definitions.
DECLARED_BY_STDLIB = {"print", "tostring"}

REG_ARRAY = re.compile(r"static const struct luaL_Reg\s+(\w+)\s*\[\]\s*=\s*\{")
REG_ENTRY = re.compile(r'\{\s*"([^"]+)"\s*,')
REG_GLOBAL = re.compile(r'lua_register\(\s*L\s*,\s*"([^"]+)"')


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def registry(path):
    """{table name: set of function names} plus the set of global names."""
    text = read(path)
    libs = {}

    for match in REG_ARRAY.finditer(text):
        array = match.group(1)
        end = text.find("};", match.end())
        if end < 0:
            sys.exit("unterminated luaL_Reg %s in %s" % (array, path))

        table = TABLE_OF.get(array)
        if table is None:
            table = array[:-3] if array.endswith("lib") else array
            table = TABLE_OF.get(table, table)

        # Entries that are commented out do not exist at run time - `speedmode`
        # is one of them.
        body = "\n".join(line.split("//")[0]
                         for line in text[match.end():end].splitlines())

        libs.setdefault(table, set()).update(REG_ENTRY.findall(body))

    # luaL_register(L, "vba", vbalib) puts the same functions under a second name.
    globals_ = set(REG_GLOBAL.findall(text))
    return libs, globals_


def definitions(path):
    """{table name: set of function names} plus the set of global names."""
    libs = {}
    globals_ = set()

    for line in read(path).splitlines():
        line = line.strip()

        match = re.match(r"^function\s+(\w+)\.(\w+)\s*\(", line)
        if match:
            libs.setdefault(match.group(1), set()).add(match.group(2))
            continue

        # Aliases are written as `table.alias = table.canonical`.
        match = re.match(r"^(\w+)\.(\w+)\s*=\s*\w+\.\w+$", line)
        if match:
            libs.setdefault(match.group(1), set()).add(match.group(2))
            continue

        match = re.match(r"^function\s+(\w+)\s*\(", line)
        if match:
            globals_.add(match.group(1))

    return libs, globals_


def report(lines, label, registered, declared):
    missing = sorted(registered - declared)
    extra = sorted(declared - registered)
    ok = not missing and not extra

    print("%-10s %3d registered, %3d declared  %s"
          % (label, len(registered), len(declared), "ok" if ok else "MISMATCH"))
    for name in missing:
        print("    - %s.%s" % (label, name) if label != "(global)" else "    - %s" % name)
        lines.append("missing: %s %s" % (label, name))
    for name in extra:
        print("    + %s.%s" % (label, name) if label != "(global)" else "    + %s" % name)
        lines.append("extra:   %s %s" % (label, name))
    return ok


def main():
    quiet = "--quiet" in sys.argv[1:]
    libs_cpp, globals_cpp = registry(ENGINE)
    libs_lua, globals_lua = definitions(META)

    # `vba` is the same table as `emu` and is declared as such, not member by
    # member, so it never has definitions of its own.
    libs_lua.pop("vba", None)

    problems = []
    ok = True

    for table in sorted(libs_cpp):
        ok &= report(problems, table,
                     libs_cpp[table], libs_lua.get(table, set()))

    for table in sorted(set(libs_lua) - set(libs_cpp)):
        print("%-10s declared but not registered" % table)
        problems.append("unknown table: %s" % table)
        ok = False

    ok &= report(problems, "(global)",
                 globals_cpp - DECLARED_BY_STDLIB, globals_lua)

    if not ok:
        print("\n%d difference(s)" % len(problems))
        return 1

    if not quiet:
        print("\nvba.lua matches the engine's registry")
    return 0


if __name__ == "__main__":
    sys.exit(main())
