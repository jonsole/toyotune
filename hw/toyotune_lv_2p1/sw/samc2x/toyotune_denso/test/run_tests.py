#!/usr/bin/env python3
"""Build and run the host unit tests.

These build natively rather than for the SAMC21, so the conversions can be
checked without hardware.  ecu_scale.c is deliberately free of any target
dependency - no registers, nothing from Device_Startup - precisely so this is
possible.

Finds a compiler in this order: whatever is already on PATH (cl, gcc, clang),
then the Visual Studio Build Tools via vswhere.  On a Developer Command Prompt
the first case applies and nothing else is needed.

Run:  python test/run_tests.py
"""
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BUILD = os.path.join(HERE, "build")

# Each entry is (name, [sources]).  can_telemetry.c is not listed as a source
# for the second suite because that test includes it directly, to reach the
# static tables and CanTelemetry_SendFrame().
SUITES = [
    ("ecu_scale", [os.path.join(HERE, "test_ecu_scale.c"),
                   os.path.join(ROOT, "ecu_scale.c")]),
    ("can_telemetry", [os.path.join(HERE, "test_can_telemetry.c"),
                       os.path.join(ROOT, "ecu_scale.c")]),
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

    # Ask vcvars for the environment it sets, rather than trying to guess the
    # include and library paths ourselves.
    try:
        out = subprocess.run(
            ["cmd", "/c", "call", vcvars, ">nul", "&&", "set"],
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


def verify_generated_tables():
    """A stale table would let a broken curve look correct, so check it first."""
    gen = os.path.join(ROOT, "tools", "gen_ecu_scale_tables.py")
    header = os.path.join(ROOT, "ecu_scale_tables.h")
    if not os.path.exists(gen):
        return True
    r = subprocess.run([sys.executable, gen, "--verify", header],
                       capture_output=True, text=True)
    sys.stdout.write(r.stdout)
    if r.returncode != 0:
        sys.stderr.write(r.stderr)
        print("\nERROR: ecu_scale_tables.h does not match its generator.")
        print("  python tools/gen_ecu_scale_tables.py -o ecu_scale_tables.h")
        return False
    return True


def build_and_run():
    os.makedirs(BUILD, exist_ok=True)

    env = None
    cc = shutil.which("cl") or shutil.which("gcc") or shutil.which("clang")
    if cc is None:
        cc, env = find_vswhere_cl()
    if cc is None:
        print("ERROR: no C compiler found.")
        print("Install the Visual Studio Build Tools, or run from a Developer")
        print("Command Prompt, or put gcc/clang on PATH.")
        return 1

    print("compiling with %s" % cc)
    print()
    name = os.path.basename(cc).lower()
    failed = 0

    for suite, sources in SUITES:
        exe = os.path.join(BUILD, suite + (".exe" if os.name == "nt" else ""))
        objdir = os.path.join(BUILD, suite)
        os.makedirs(objdir, exist_ok=True)

        if name.startswith("cl"):
            cmd = [cc, "/nologo", "/W4", "/wd4996",
                   "/DTOYOTUNE_CPU1", "/DTOYOTUNE_ECU_MR2",
                   "/Fe:" + exe, "/Fo:" + objdir + os.sep,
                   "/I" + ROOT, "/I" + HERE] + sources
        else:
            cmd = [cc, "-std=c99", "-Wall", "-Wextra", "-O1",
                   "-DTOYOTUNE_CPU1", "-DTOYOTUNE_ECU_MR2",
                   "-I" + ROOT, "-I" + HERE,
                   "-o", exe] + sources

        r = subprocess.run(cmd, env=env, capture_output=True, text=True)
        if r.returncode != 0:
            sys.stdout.write(r.stdout)
            sys.stderr.write(r.stderr)
            print("ERROR: %s failed to compile." % suite)
            failed = 1
            continue

        # MSVC echoes the source names even on success, so only surface the
        # lines that actually say something.
        for line in (r.stdout + r.stderr).splitlines():
            if "warning" in line.lower():
                print("  " + line)

        if subprocess.run([exe], env=env).returncode != 0:
            failed = 1
        print()

    return failed


def verify_generated_dbc():
    """The DBC is generated too, and a stale one would defeat the round-trip."""
    gen = os.path.join(ROOT, "tools", "gen_dbc.py")
    dbc = os.path.join(ROOT, "toyotune.dbc")
    if not os.path.exists(gen):
        return True
    r = subprocess.run([sys.executable, gen, "--verify", dbc],
                       capture_output=True, text=True)
    sys.stdout.write(r.stdout)
    if r.returncode != 0:
        sys.stderr.write(r.stderr)
        print()
        print("ERROR: toyotune.dbc does not match its generator.")
        print("  python tools/gen_dbc.py -o toyotune.dbc")
        return False
    return True


def run_dbc_roundtrip():
    """Decode the firmware's own bytes through the DBC - see the script."""
    script = os.path.join(HERE, "test_dbc_roundtrip.py")
    if not os.path.exists(script):
        return 0
    return subprocess.run([sys.executable, script]).returncode


def main():
    if not verify_generated_tables():
        return 1
    if not verify_generated_dbc():
        return 1
    rc = build_and_run()
    return rc | run_dbc_roundtrip()


if __name__ == "__main__":
    sys.exit(main())
