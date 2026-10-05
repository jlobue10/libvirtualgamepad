#!/usr/bin/env python3
"""Points the Vibepollo fork branch at a libvirtualgamepad fork prerelease.

docs/STEAM_CONTROLLER_PROFILE.md section 7.5: the Vibepollo Windows CI downloads a *released* driver
package by tag and verifies its archive hash, DriverVer, source revision and the submodule
gitlink. Those pins live in seven places. This script reads the release's
`*.release-lock.json`, rewrites every pin (and the producer repository) in the Vibepollo
fork branch through the GitHub API, updates the `third-party/libvirtualgamepad` gitlink to
the release commit, and commits in one go. Nothing is cloned locally (the local Vibepollo
clone is unusable, see section 7.7).

    python tools/capture/Update-VibepolloDriverPins.py --tag v0.1.0-beta.101 [--dry-run]
    python tools/capture/Update-VibepolloDriverPins.py --tag v0.1.0-beta.101 --lock path/to.release-lock.json

Requires `gh` authenticated for both repositories.
"""
import argparse
import base64
import json
import re
import subprocess
import sys

DRIVER_REPO = 'jlobue10/libvirtualgamepad'
UPSTREAM_DRIVER_REPO = 'Nonary/libvirtualgamepad'
VIBEPOLLO_REPO = 'jlobue10/Vibepollo'
VIBEPOLLO_BRANCH = 'feat/steam-controller-profile'
GITLINK_PATH = 'third-party/libvirtualgamepad'
FILES = [
    '.github/workflows/ci-windows.yml',
    'cmake/packaging/windows_virtual_gamepad_contract.cmake',
    'src_assets/windows/drivers/vhf-gamepad/install.ps1',
]


def gh(*args, data=None):
    cmd = ['gh', 'api', *args]
    if data is not None:
        cmd += ['--input', '-']
    result = subprocess.run(cmd, input=json.dumps(data) if data is not None else None,
                            capture_output=True, text=True)
    if result.returncode != 0:
        sys.exit(f"gh api {' '.join(args)} failed:\n{result.stderr}")
    return json.loads(result.stdout) if result.stdout.strip() else {}


def current_pins(cmake_text):
    """Old (tag, sha256, revision, driver_ver) from the CMake contract, where each is unambiguous."""
    def grab(name, pattern):
        m = re.search(r'set\(' + name + r'\s+"(' + pattern + r')"', cmake_text)
        if not m:
            sys.exit(f'{name} not found in the CMake contract')
        return m.group(1)
    return (grab('SUNSHINE_VHF_GAMEPAD_RELEASE_TAG', r'v0\.1\.0-beta\.\d+'),
            grab('SUNSHINE_VHF_GAMEPAD_RELEASE_ASSET_SHA256', r'[0-9a-f]{64}'),
            grab('SUNSHINE_VHF_GAMEPAD_SOURCE_REVISION', r'[0-9a-f]{40}'),
            grab('SUNSHINE_VHF_GAMEPAD_DRIVER_VER', r'\d\d/\d\d/\d{4},0\.1\.0\.\d+'))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--tag', required=True, help='driver fork release tag, e.g. v0.1.0-beta.101')
    ap.add_argument('--lock', help='local release-lock.json (default: download from the release)')
    ap.add_argument('--repository', default=DRIVER_REPO, help='producer repository to pin')
    ap.add_argument('--dry-run', action='store_true', help='show the changes, commit nothing')
    args = ap.parse_args()

    if args.lock:
        lock = json.load(open(args.lock, encoding='utf-8'))
    else:
        assets = gh(f'repos/{args.repository}/releases/tags/{args.tag}')['assets']
        lock_asset = [a for a in assets if a['name'].endswith('.release-lock.json')]
        if len(lock_asset) != 1:
            sys.exit(f'release {args.tag} has {len(lock_asset)} release-lock assets')
        out = subprocess.run(['gh', 'release', 'download', args.tag, '-R', args.repository,
                              '-p', '*.release-lock.json', '-O', '-'], capture_output=True, text=True)
        if out.returncode != 0:
            sys.exit(out.stderr)
        lock = json.loads(out.stdout)
    if lock['tag'] != args.tag:
        sys.exit(f"lock is for {lock['tag']}, not {args.tag}")
    new = dict(tag=lock['tag'], sha=lock['archive']['sha256'], rev=lock['source_revision'],
               ver=lock['driver_ver'], asset=lock['archive']['name'])
    print('new pins:', json.dumps(new, indent=2))

    head = gh(f'repos/{VIBEPOLLO_REPO}/git/ref/heads/{VIBEPOLLO_BRANCH}')['object']['sha']
    head_commit = gh(f'repos/{VIBEPOLLO_REPO}/git/commits/{head}')
    print(f'Vibepollo {VIBEPOLLO_BRANCH} at {head[:12]}: {head_commit["message"].splitlines()[0]}')

    contents = {}
    for path in FILES:
        blob = gh(f'repos/{VIBEPOLLO_REPO}/contents/{path}?ref={VIBEPOLLO_BRANCH}')
        contents[path] = base64.b64decode(blob['content']).decode('utf-8')
    old_tag, old_sha, old_rev, old_ver = current_pins(contents[FILES[1]])
    old_repo = re.search(r'set\(SUNSHINE_VHF_GAMEPAD_REPOSITORY "([^"]+)"', contents[FILES[1]]).group(1)
    print(f'old pins: {old_repo} {old_tag} sha {old_sha[:12]} rev {old_rev[:12]} ver {old_ver}')
    if old_rev == new['rev'] and old_sha == new['sha']:
        sys.exit('already pinned to this release')

    tree_entries = []
    for path in FILES:
        text = contents[path]
        # Replace the full tag and the bare version (asset file names: libvirtualgamepad-0.1.0-beta.N-...).
        updated = (text.replace(old_sha, new['sha']).replace(old_rev, new['rev'])
                   .replace(old_ver, new['ver']).replace(old_tag, new['tag'])
                   .replace(old_tag[1:], new['tag'][1:]).replace(old_repo, args.repository))
        counts = {k: updated.count(v) for k, v in
                  (('tag', new['tag']), ('sha', new['sha']), ('rev', new['rev']), ('ver', new['ver']),
                   ('repository', args.repository))}
        if old_rev in updated or old_sha in updated or old_tag in updated:
            sys.exit(f'{path}: an old pin survived')
        print(f'  {path}: {old_tag} -> {new["tag"]}, occurrences {counts}')
        if path.endswith('install.ps1') and new['asset'] not in updated:
            sys.exit('install.ps1 asset name was not updated')
        if updated == text:
            print('    (unchanged)')
            continue
        tree_entries.append({'path': path, 'mode': '100644', 'type': 'blob', 'content': updated})
    tree_entries.append({'path': GITLINK_PATH, 'mode': '160000', 'type': 'commit', 'sha': new['rev']})

    message = (f"packaging: pin the VHF gamepad driver to {args.repository} {new['tag']}\n\n"
               f"Fork prerelease carrying the Steam Controller (2026) profile, for the test rig in\n"
               f"libvirtualgamepad docs/STEAM_CONTROLLER_PROFILE.md section 7.5. Not for upstream: the\n"
               f"producer repository and tag go back to {UPSTREAM_DRIVER_REPO} once the driver PR\n"
               f"is released there.\n\n"
               f"source revision {new['rev']}\nDriverVer {new['ver']}\narchive sha256 {new['sha']}\n\n"
               f"Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>\n"
               f"Claude-Session: https://claude.ai/code/session_01YABzuxurYwwoGSaHYKJSNs\n")
    if args.dry_run:
        print('\n--dry-run: would commit\n' + message)
        return
    tree = gh(f'repos/{VIBEPOLLO_REPO}/git/trees', data={'base_tree': head_commit['tree']['sha'], 'tree': tree_entries})
    commit = gh(f'repos/{VIBEPOLLO_REPO}/git/commits', data={'message': message, 'tree': tree['sha'], 'parents': [head]})
    gh(f'repos/{VIBEPOLLO_REPO}/git/refs/heads/{VIBEPOLLO_BRANCH}', '-X', 'PATCH', data={'sha': commit['sha'], 'force': False})
    print(f"committed {commit['sha'][:12]} on {VIBEPOLLO_REPO} {VIBEPOLLO_BRANCH}")
    print(f"next: gh workflow run ci.yml -R {VIBEPOLLO_REPO} --ref {VIBEPOLLO_BRANCH}  (unsigned installer artifact)")


if __name__ == '__main__':
    main()
