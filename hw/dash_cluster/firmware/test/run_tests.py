#!/usr/bin/env python3
"""Build and run the dash node's host tests.

Decode, the signal store, node identity and page selection are all free of
hardware dependencies, deliberately - so they can be tested on a host long
before the board arrives, and re-tested in seconds afterwards.

The display layer is not covered here and cannot be: it needs the vendor
CO5300 driver and a panel.

Finds a compiler on PATH (cl, gcc, clang), then the Visual Studio Build Tools
via vswhere. Mirrors the runner in the SAMC21 firmware so both trees are
driven the same way.

Run:  python test/run_tests.py
"""
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC = os.path.join(ROOT, "src")
BUILD = os.path.join(HERE, "build")

SOURCES = [
    os.path.join(HERE, "test_dash.c"),
    os.path.join(HERE, "test_ui_model.c"),
    os.path.join(SRC, "node_id.c"),
    os.path.join(SRC, "pages.c"),
    os.path.join(SRC, "signal_store.c"),
    os.path.join(SRC, "signals.c"),
    os.path.join(SRC, "telemetry.c"),
    os.path.join(SRC, "ui_model.c"),
]


def find_vswhere_cl():
    """Locate cl.exe through vswhere, returning (cl_path, env) or (None, None)."""
    pf86 = os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")
    vswhere = os.path.join(pf86, "Microsoft Visual Studio", "Installer", "vswhere.exe")
    if not os.path.exists(vswhere):
        return None, None
    try:
        install = subprocess.run(
            [vswhere, "-latest", "-products", "*", "-property", "installationPath"],
            capture_output=True, text=True, check=True).stdout.strip()
    except (subprocess.CalledProcessError, OSError):
        return None, None
    if not install:
        return None, None

    vcvars = os.path.join(install, "VC", "Auxiliary", "Build", "vcvars64.bat")
    if not os.path.exists(vcvars):
        return None, None

    try:
        out = subprocess.run(["cmd", "/c", "call", vcvars, ">nul", "&&", "set"],
                             capture_output=True, text=True, check=True).stdout
    except (subprocess.CalledProcessError, OSError):
        return None, None

    env = dict(os.environ)
    for line in out.splitlines():
        if "=" in line:
            k, _, v = line.partition("=")
            env[k] = v
    cl = shutil.which("cl", path=env.get("PATH", ""))
    return (cl, env) if cl else (None, None)


def main():
    os.makedirs(BUILD, exist_ok=True)
    exe = os.path.join(BUILD, "test_dash" + (".exe" if os.name == "nt" else ""))

    env = None
    cc = shutil.which("cl") or shutil.which("gcc") or shutil.which("clang")
    if cc is None:
        cc, env = find_vswhere_cl()
    if cc is None:
        print("ERROR: no C compiler found.")
        print("Install the Visual Studio Build Tools, or run from a Developer")
        print("Command Prompt, or put gcc/clang on PATH.")
        return 1

    name = os.path.basename(cc).lower()
    if name.startswith("cl"):
        cmd = [cc, "/nologo", "/W4", "/wd4996",
               "/Fe:" + exe, "/Fo:" + BUILD + os.sep,
               "/I" + SRC, "/I" + HERE] + SOURCES
    else:
        cmd = [cc, "-std=c11", "-Wall", "-Wextra", "-O1",
               "-I" + SRC, "-I" + HERE, "-o", exe] + SOURCES

    print("compiling with %s" % cc)
    r = subprocess.run(cmd, env=env, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stdout.write(r.stdout)
        sys.stderr.write(r.stderr)
        print("ERROR: compilation failed.")
        return 1

    # MSVC echoes source names even on success; only surface real warnings.
    for line in (r.stdout + r.stderr).splitlines():
        if "warning" in line.lower():
            print("  " + line)

    print()
    return subprocess.run([exe], env=env).returncode


if __name__ == "__main__":
    sys.exit(main())
