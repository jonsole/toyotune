"""Check that every ROM source still assembles to the bytes it should.

Renames and comments in a `.ASM` are supposed to be inert. This asserts it,
across every source in the repo at once, so a session's worth of edits can be
checked in one command instead of remembering which files were touched.

    python roms/verify_all_roms.py                  # every source it can find
    python roms/verify_all_roms.py --vs-ref master  # also diff against a git ref
    python roms/verify_all_roms.py roms/3S-GTE      # limit to a subtree

Two checks per source, the second only with --vs-ref:

  vs ROM     assembled output against the shipped .bin/.BIN sitting beside it,
             matched by name. This is the real check, but not every source has
             an image to compare against (the 32K and DIAG16 variants do not).
  vs ref     the same file assembled as it was at a git ref, compared both by
             bytes and through verify_assembly_match.py. This covers the
             sources that have no shipped image, and catches drift introduced
             since that ref.

Exit code is 0 only if everything that could be checked passed.
"""

import argparse
import hashlib
import os
import subprocess
import tempfile
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
ASSEMBLER = os.path.join(HERE, 'd8x_assembler', 'asm_d8x.py')
MATCHER = os.path.join(HERE, 'verify_assembly_match.py')

#: Sources that are known not to assemble, with the reason. Skipped rather than
#: reported as failures, so a real regression stays visible.
#: Keys are compared case-INSENSITIVELY. This file is `.asm` on disk while the
#: key was written `.ASM`, so the entry matched nothing and the source was
#: reported as a fresh failure on every run - the exact noise this table exists
#: to prevent.
KNOWN_BAD = {
    'd151804-0401_diag16_32k.asm': 'does not assemble; see CLAUDE.md INSTALL.md note',
}


def run(args):
    return subprocess.run(args, cwd=REPO, capture_output=True)


def digest(path):
    with open(path, 'rb') as handle:
        return hashlib.sha256(handle.read()).hexdigest()


def ignored_by_git(relpaths):
    """The subset of `relpaths` (repo-relative, / separated) that git ignores.

    Scratch copies live in the tree on purpose - roms/d8x_assembler/ keeps two
    ROM sources and a .BIN to test the assembler against, all listed in
    .gitignore. They are not repo content, so a stale one must not be reported
    as a repo-wide regression; that happened, and the "failure" was in a file
    that would not exist in a fresh clone. Deleting them would be wrong - they
    are deliberate - so skip them instead.

    One `git check-ignore` call for the whole set rather than one per file. If
    git is unavailable, ignore nothing rather than guessing.
    """
    if not relpaths:
        return set()
    try:
        result = subprocess.run(['git', 'check-ignore', '--stdin'], cwd=REPO,
                                input=chr(10).join(relpaths).encode(),
                                capture_output=True)
    except OSError:
        return set()
    # check-ignore exits 1 when nothing matched, which is not an error here.
    if result.returncode not in (0, 1):
        return set()
    return {line.strip().replace(chr(92), '/')
            for line in result.stdout.decode('utf-8', 'replace').splitlines() if line.strip()}


def find_sources(roots):
    found = []
    for root in roots:
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [d for d in dirnames if d not in ('output', '__pycache__', '.git')]
            for name in sorted(filenames):
                if not name.lower().endswith('.asm'):
                    continue
                if ' - Copy' in name:           # superseded snapshots, see CLAUDE.md
                    continue
                found.append(os.path.join(dirpath, name))
    rels = [os.path.relpath(f, REPO).replace(os.sep, '/') for f in found]
    ignored = ignored_by_git(rels)
    for path, rel in zip(found, rels):
        if rel not in ignored:
            yield path


def shipped_image(source):
    """The .bin/.BIN a source should assemble to, if one exists.

    Looks beside the source and, for a `Claude/` working copy, in the parent
    ECU directory as well - that is where the image lives, and the working
    copies are exactly the files most worth checking.
    """
    directory, name = os.path.split(source)
    stem = os.path.splitext(name)[0].lower()
    places = [directory]
    if os.path.basename(directory).lower() in ('claude', 'toyotune'):
        places.append(os.path.dirname(directory))
    for place in places:
        images = [c for c in os.listdir(place) if c.lower().endswith('.bin')]
        for candidate in images:
            if os.path.splitext(candidate)[0].lower() == stem:
                return os.path.join(place, candidate)
        # Some images carry a suffix the source does not, e.g.
        # D151804-0481.asm against D151804-0481_ORIGNAL.bin. Accept that only
        # when it is unambiguous, so a directory holding several variants does
        # not get matched to the wrong one.
        prefixed = [c for c in images
                    if os.path.splitext(c)[0].lower().startswith(stem + '_')]
        if len(prefixed) == 1:
            return os.path.join(place, prefixed[0])
    return None


def assemble(source, out_prefix):
    binary, listing = out_prefix + '.bin', out_prefix + '.lst'
    for path in (binary, listing):
        if os.path.exists(path):
            os.remove(path)
    run([sys.executable, ASSEMBLER, '-p', '5F', source, binary, listing])
    if os.path.exists(binary) and os.path.getsize(binary):
        return binary, listing
    return None, None


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('roots', nargs='*', default=[os.path.join(REPO, 'roms')],
                        help='directories to search (default: roms/)')
    parser.add_argument('--vs-ref', metavar='REF',
                        help='also assemble each file as at this git ref and compare')
    parser.add_argument('--tmp', default=None,
                        help='scratch directory for assembler output. Default is '
                             'a fresh directory under the system temp dir - '
                             'outside the repo, so a stray `git add -A` cannot '
                             'commit it, and unique per run, so two concurrent '
                             'runs cannot clobber the same output files')
    args = parser.parse_args(argv)

    # A fixed scratch path means two runs in parallel fight over cur.bin and
    # one dies with a PermissionError partway through. Give each run its own.
    if args.tmp:
        os.makedirs(args.tmp, exist_ok=True)
    else:
        args.tmp = tempfile.mkdtemp(prefix='verify_all_roms_')
    rows, failures, checked = [], 0, 0

    for source in find_sources(args.roots):
        rel = os.path.relpath(source, REPO).replace('\\', '/')
        name = os.path.basename(source)
        if name.lower() in KNOWN_BAD:
            rows.append((rel, 'skipped', KNOWN_BAD[name.lower()], ''))
            continue

        image = shipped_image(source)
        binary, listing = assemble(source, os.path.join(args.tmp, 'cur'))
        if binary is None:
            # Plenty of .asm files here are not ROMs: extracted routines kept as
            # annotation examples, standalone bring-up programs. Only something
            # with a shipped image beside it is a regression when it stops
            # building.
            if image:
                rows.append((rel, 'FAILS TO ASSEMBLE', 'has ' + os.path.basename(image), ''))
                failures += 1
            else:
                rows.append((rel, 'not a ROM source', 'no image; fragment or test program', ''))
            continue

        vs_rom = ''
        if image:
            checked += 1
            same = digest(binary) == digest(image)
            vs_rom = 'ok' if same else 'DIFFERS from ' + os.path.basename(image)
            failures += 0 if same else 1

        vs_ref = ''
        if args.vs_ref:
            show = run(['git', 'show', f'{args.vs_ref}:{rel}'])
            if show.returncode:
                vs_ref = 'not at ref'
            else:
                old_src = os.path.join(args.tmp, 'old_' + name)
                with open(old_src, 'wb') as handle:
                    handle.write(show.stdout)
                old_bin, old_lst = assemble(old_src, os.path.join(args.tmp, 'old'))
                if old_bin is None:
                    vs_ref = 'ref did not assemble'
                elif digest(old_bin) != digest(binary):
                    vs_ref, failures = 'DIFFERS', failures + 1
                else:
                    checked += 1
                    match = run([sys.executable, MATCHER, old_lst, listing])
                    ok = b'Total real edit regions: 0' in match.stdout
                    vs_ref = 'ok' if ok else 'LST MISMATCH'
                    failures += 0 if ok else 1

        rows.append((rel, 'ok', vs_rom, vs_ref))

    width = max((len(r[0]) for r in rows), default=10)
    print(f"{'source':{width}}  {'assembles':18} {'vs ROM':28} vs {args.vs_ref or 'ref'}")
    print('-' * (width + 60))
    for rel, status, vs_rom, vs_ref in rows:
        print(f'{rel:{width}}  {status:18} {vs_rom:28} {vs_ref}')
    print('-' * (width + 60))
    print(f'{len(rows)} sources, {checked} comparisons, {failures} failure(s)')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
