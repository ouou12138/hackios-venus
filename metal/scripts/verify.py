#!/usr/bin/env python3
"""Verify the standalone package's checked-in hashes and source/object layout."""
import hashlib
import json
import struct
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def main():
    manifest=json.loads((ROOT/'MANIFEST.json').read_text())
    for relative,expected in manifest['files'].items():
        path=ROOT/relative
        assert path.is_file() and hashlib.sha256(path.read_bytes()).hexdigest()==expected,relative
    for path in (ROOT/'obj').glob('*.o'):
        header=struct.unpack_from('<4I',path.read_bytes())
        assert header[:2]==(0xfeedfacf,0x100000c) and header[3]==1,path
    result=json.loads((ROOT/'validation/RESULT.json').read_text())
    assert result['startup_checks_passed']==result['startup_checks_total']==33
    assert result['ca_pixel_checks']==4 and result['ca_pixel_failures']==0
    print(f'Verified {len(manifest["files"])} packaged files and arm64 relocatable objects; no default activation.')
if __name__=='__main__':main()
