#!/usr/bin/env python3
"""Pin-rewrite rules of Update-VibepolloDriverPins.py: anchored tag replacement and the survival guard.

Pure Python; nothing is fetched or committed.
"""
import importlib.util
import json
import pathlib
import sys
import tempfile

# A tracked tree must stay clean after running this (the build scripts refuse a dirty tree).
sys.dont_write_bytecode = True

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

# The lock must describe the tag's commit before anything is pinned (D22-04).
commit = 'a' * 40
ref = {'object': {'type': 'commit', 'sha': commit}}
good = {'tag': 'v0.1.0-beta.120', 'source_revision': commit, 'tag_target': commit}
check(pins.check_lock_against_tag(good, 'v0.1.0-beta.120', ref) is None, 'a lock made from the tagged commit passes')
check(pins.check_lock_against_tag(dict(good, source_revision='b' * 40), 'v0.1.0-beta.120', ref) is not None,
      'a lock whose source revision is not the tagged commit is refused')
check(pins.check_lock_against_tag(dict(good, tag_target='b' * 40), 'v0.1.0-beta.120', ref) is not None,
      'a lock whose tag target is not the tagged commit is refused')
check(pins.check_lock_against_tag(good, 'v0.1.0-beta.121', ref) is not None, 'a lock for another tag is refused')
check(pins.check_lock_against_tag(good, 'v0.1.0-beta.120', {'object': {'type': 'tag', 'sha': commit}}) is not None,
      'an annotated tag object is not taken for the commit')
check(pins.check_lock_against_tag(good, 'v0.1.0-beta.120', {'object': {'type': 'commit', 'sha': commit.upper()}}) is None,
      'commit hashes compare case-insensitively')
good_pins = dict(tag='v0.1.0-beta.120', sha='a' * 64, rev='b' * 40, ver='10/12/2026,0.1.0.180',
                 asset='libvirtualgamepad-0.1.0-beta.120-windows-x64.zip', protocol='3')
check(pins.validate_new_pins(good_pins) is None, 'well-formed lock pins are accepted')
for key, bad in (('tag', 'v0.1.0-beta.120 '), ('sha', 'A' * 64), ('rev', 'b' * 39), ('ver', '10/12/2026,0.1.0'),
                 ('asset', '../evil.zip'), ('protocol', '3)|('), ('protocol', 3)):
    check(pins.validate_new_pins(dict(good_pins, **{key: bad})) is not None, f'lock pin {key}={bad!r} is refused')
check(pins.REPOSITORY_PATTERN.fullmatch('jlobue10/libvirtualgamepad') is not None, 'owner/name repositories are accepted')
for bad in ('jlobue10', 'a/b/c', 'a b/c', 'owner/name;rm', 'owner/..', 'owner/.', '../name', './name'):
    check(pins.REPOSITORY_PATTERN.fullmatch(bad) is None, f'repository {bad!r} is refused')
for good in ('v0.1.0-beta.132', 'v1.2.3', 'v0.1.0-rc.1'):
    check(pins.TAG_PATTERN.fullmatch(good) is not None, f'tag {good!r} is accepted')
for bad in ('0.1.0-beta.132', 'v0.1', 'v0.1.0 beta', 'v0.1.0-beta.1;rm', 'v0.1.0-beta.1/x'):
    check(pins.TAG_PATTERN.fullmatch(bad) is None, f'tag {bad!r} is refused')
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
