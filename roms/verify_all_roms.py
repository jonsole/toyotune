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
KNOWN_BAD = {
    'D151804-0401_DIAG16_32K.ASM': 'does not assemble; see CLAUDE.md INSTALL.md note',
}


def run(args):
    return subprocess.run(args, cwd=REPO, capture_output=True)


def digest(path):
    with open(path, 'rb') as handle:
        return hashlib.sha256(handle.read()).hexdigest()


def find_sources(roots):
    for root in roots:
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [d for d in dirnames if d not in ('output', '__pycache__', '.git')]
            for name in sorted(filenames):
                if not name.lower().endswith('.asm'):
                    continue
                if ' - Copy' in name:           # superseded snapshots, see CLAUDE.md
                    continue
                yield os.path.join(dirpath, name)


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
    parser.add_argument('--tmp', default=os.path.join(tempfile.gettempdir(),
                                                      'verify_all_roms'),
                        help='scratch directory for assembler output (default: '
                             'the system temp dir, deliberately outside the repo '
                             'so a stray `git add -A` cannot commit it)')
    args = parser.parse_args(argv)

    os.makedirs(args.tmp, exist_ok=True)
    rows, failures, checked = [], 0, 0

    for source in find_sources(args.roots):
        rel = os.path.relpath(source, REPO).replace('\\', '/')
        name = os.path.basename(source)
        if name in KNOWN_BAD:
            rows.append((rel, 'skipped', KNOWN_BAD[name], ''))
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
