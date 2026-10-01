"""Package the D8X extension as a .vsix, and optionally install it.

    python tools/vscode-d8x/package_vsix.py            # -> d8x-asm-<version>.vsix
    python tools/vscode-d8x/package_vsix.py --install  # ... then code --install-extension

A .vsix is a zip with a manifest; building it here avoids needing node and
@vscode/vsce, neither of which the bench machine has.

Before packaging, the grammar's two mnemonic lists are checked against the
assembler's opcode table (roms/d8x_assembler/instruction.py), so the
highlighter cannot quietly drift from what asm_d8x.py accepts.
"""

import argparse
import json
import re
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path
from xml.sax.saxutils import escape

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent

# Everything the extension needs at runtime; tests and tooling stay out.
FILES = [
    'package.json',
    'extension.js',
    'symbols.js',
    'language-configuration.json',
    'syntaxes/d8x.tmLanguage.json',
    'README.md',
]


def check_mnemonics():
    sys.path.insert(0, str(REPO / 'roms' / 'd8x_assembler'))
    import instruction

    grammar = json.loads((HERE / 'syntaxes' / 'd8x.tmLanguage.json').read_text(encoding='utf-8'))
    listed = set()
    for key in ('mnemonic-flow', 'mnemonic-other'):
        m = re.search(r'\(\?i:\(([a-z|]+)\)\)', grammar['repository'][key]['begin'])
        listed |= set(m.group(1).split('|'))
    opcodes = set(instruction.opcodes)
    if listed != opcodes:
        missing = ', '.join(sorted(opcodes - listed)) or '-'
        extra = ', '.join(sorted(listed - opcodes)) or '-'
        sys.exit(f'grammar mnemonics differ from instruction.opcodes\n'
                 f'  missing from grammar: {missing}\n  not in assembler: {extra}')


def manifest(pkg):
    return f'''<?xml version="1.0" encoding="utf-8"?>
<PackageManifest Version="2.0.0" xmlns="http://schemas.microsoft.com/developer/vsx-schema/2011" xmlns:d="http://schemas.microsoft.com/developer/vsx-schema-design/2011">
  <Metadata>
    <Identity Language="en-US" Id="{escape(pkg['name'])}" Version="{escape(pkg['version'])}" Publisher="{escape(pkg['publisher'])}" />
    <DisplayName>{escape(pkg['displayName'])}</DisplayName>
    <Description xml:space="preserve">{escape(pkg['description'])}</Description>
    <Categories>{escape(','.join(pkg['categories']))}</Categories>
    <Properties>
      <Property Id="Microsoft.VisualStudio.Code.Engine" Value="{escape(pkg['engines']['vscode'])}" />
    </Properties>
  </Metadata>
  <Installation>
    <InstallationTarget Id="Microsoft.VisualStudio.Code" />
  </Installation>
  <Dependencies />
  <Assets>
    <Asset Type="Microsoft.VisualStudio.Code.Manifest" Path="extension/package.json" Addressable="true" />
  </Assets>
</PackageManifest>
'''


CONTENT_TYPES = '''<?xml version="1.0" encoding="utf-8"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
  <Default Extension=".json" ContentType="application/json" />
  <Default Extension=".js" ContentType="application/javascript" />
  <Default Extension=".md" ContentType="text/markdown" />
  <Default Extension=".vsixmanifest" ContentType="text/xml" />
</Types>
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--install', action='store_true', help='install into VS Code once built')
    args = ap.parse_args()

    check_mnemonics()
    pkg = json.loads((HERE / 'package.json').read_text(encoding='utf-8'))
    out = HERE / f"{pkg['name']}-{pkg['version']}.vsix"
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr('[Content_Types].xml', CONTENT_TYPES)
        z.writestr('extension.vsixmanifest', manifest(pkg))
        for f in FILES:
            z.write(HERE / f, 'extension/' + f)
    print(f'wrote {out.relative_to(REPO)}')

    if args.install:
        code = shutil.which('code') or shutil.which('code.cmd')
        if not code:
            sys.exit("'code' is not on PATH - install the .vsix from the Extensions view instead")
        subprocess.run([code, '--install-extension', str(out), '--force'], check=True)
        print('installed - run "Developer: Reload Window" in VS Code to pick it up')


if __name__ == '__main__':
    main()
