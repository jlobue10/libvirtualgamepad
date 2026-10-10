#!/usr/bin/env python3
"""Pin-rewrite rules of Update-VibepolloDriverPins.py: anchored tag replacement and the survival guard.

Pure Python; nothing is fetched or committed.
"""
import importlib.util
import json
import pathlib
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("pins", HERE / "Update-VibepolloDriverPins.py")
pins = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pins)

failures = 0


def check(ok, text):
    global failures
    print(("PASS " if ok else "FAIL ") + text)
    failures += not ok


sample = ('SUNSHINE_VHF_GAMEPAD_TAG "v0.1.0-beta.12"\n'
          'asset libvirtualgamepad-0.1.0-beta.12-windows-x64.zip\n'
          'unrelated v0.1.0-beta.120 stays\n'
          'also v0.1.0-beta.121 and 0.1.0-beta.1 and v0.1.0-beta.12.1\n')
out = pins.rewrite_tag(sample, 'v0.1.0-beta.12', 'v0.1.0-beta.120')
check(out.count('"v0.1.0-beta.120"') == 1 and 'beta.1200' not in out, 'a tag whose number is a prefix of the new one is rewritten once')
check('libvirtualgamepad-0.1.0-beta.120-windows-x64.zip' in out and 'beta.1200' not in out, 'the bare asset version is rewritten once')
check('unrelated v0.1.0-beta.120 stays' in out, 'an occurrence of the new tag is left alone')
check('v0.1.0-beta.121' in out and ' 0.1.0-beta.1 ' in out and 'v0.1.0-beta.12.1' in out, 'longer and dotted versions are not touched')
check(not pins.tag_survives(out, 'v0.1.0-beta.12'), 'the guard is quiet once the old tag is gone')
check(pins.tag_survives(sample, 'v0.1.0-beta.12'), 'the guard fires while the old tag remains')
check(not pins.tag_survives('v0.1.0-beta.120 only', 'v0.1.0-beta.12'), 'the guard does not fire on the new tag')
plain = pins.rewrite_tag('tag v0.1.0-beta.119 asset 0.1.0-beta.119', 'v0.1.0-beta.119', 'v0.1.0-beta.120')
check(plain == 'tag v0.1.0-beta.120 asset 0.1.0-beta.120', 'an ordinary bump rewrites both forms')

with tempfile.TemporaryDirectory() as directory:
    lock = pathlib.Path(directory) / 'x.release-lock.json'
    lock.write_bytes(b'\xef\xbb\xbf' + json.dumps({'tag': 'v0.1.0-beta.120'}).encode())
    try:
        loaded = json.load(open(lock, encoding='utf-8-sig'))
        check(loaded['tag'] == 'v0.1.0-beta.120', 'a BOM-prefixed lock file loads')
    except json.JSONDecodeError:
        check(False, 'a BOM-prefixed lock file loads')
    check(json.loads(('\ufeff' + json.dumps({'a': 1})).lstrip('\ufeff'))['a'] == 1, 'a BOM-prefixed download loads')

print(f'{failures} failures')
sys.exit(1 if failures else 0)
