import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { mkdtempSync, readFileSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import test from 'node:test';
import { fileURLToPath } from 'node:url';

import {
  CANONICAL_REPOSITORY,
  CANONICAL_REPOSITORY_SLUG,
  validateRelease,
} from '../tools/validate-release.mjs';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');

test('checked-in release identity and version are internally consistent', () => {
  const result = validateRelease({
    repoRoot: root,
    tag: 'v0.6.0',
    repositorySlug: CANONICAL_REPOSITORY_SLUG,
  });
  assert.equal(result.version, '0.6.0');
  assert.equal(result.repository, CANONICAL_REPOSITORY);
});

test('a release from a non-canonical repository is rejected', () => {
  assert.throws(
    () =>
      validateRelease({
        repoRoot: root,
        tag: 'v0.6.0',
        repositorySlug: 'genkigenki/hotspot-arcade-cardputer',
      }),
    /release repository .* is not canonical/,
  );
});

test('a mismatched tag is rejected before publishing', () => {
  assert.throws(
    () => validateRelease({ repoRoot: root, tag: 'v0.5.0' }),
    /tag v0\.5\.0 does not match VERSION v0\.6\.0/,
  );
});

test('release packaging emits stable checksums and manifest', () => {
  const build = mkdtempSync(join(tmpdir(), 'hotspot-release-test-'));
  writeFileSync(join(build, 'hotspot-arcade-cardputer.ino.bin'), 'app fixture');
  writeFileSync(join(build, 'hotspot-arcade-cardputer.full.bin'), 'full fixture');
  writeFileSync(join(build, 'hotspot-arcade-cardputer-m5burner.zip'), 'archive fixture');
  writeFileSync(join(build, 'hotspot-arcade-cardputer.spdx.json'), '{}\n');

  const command = [join(root, 'tools', 'package-release.mjs'), '--artifacts-dir', build, '--tag', 'v0.6.0'];
  execFileSync(process.execPath, command, { cwd: root, stdio: 'pipe' });
  const firstManifest = readFileSync(join(build, 'build-manifest.json'), 'utf8');
  const firstChecksums = readFileSync(join(build, 'SHA256SUMS'), 'utf8');
  execFileSync(process.execPath, command, { cwd: root, stdio: 'pipe' });

  assert.equal(readFileSync(join(build, 'build-manifest.json'), 'utf8'), firstManifest);
  assert.equal(readFileSync(join(build, 'SHA256SUMS'), 'utf8'), firstChecksums);
  const manifest = JSON.parse(firstManifest);
  assert.equal(manifest.repository, CANONICAL_REPOSITORY);
  assert.equal(manifest.version, '0.6.0');
  assert.equal(manifest.artifacts.length, 4);
  assert.match(firstChecksums, /hotspot-arcade-cardputer\.full\.bin/);
  assert.match(firstChecksums, /build-manifest\.json/);
});

test('SPDX generation is deterministic at SOURCE_DATE_EPOCH', () => {
  const build = mkdtempSync(join(tmpdir(), 'hotspot-sbom-test-'));
  writeFileSync(join(build, 'hotspot-arcade-cardputer.ino.bin'), 'app fixture');
  writeFileSync(join(build, 'hotspot-arcade-cardputer.full.bin'), 'full fixture');
  writeFileSync(join(build, 'hotspot-arcade-cardputer-m5burner.zip'), 'archive fixture');
  const command = [join(root, 'tools', 'generate-sbom.mjs'), build];
  const options = {
    cwd: root,
    stdio: 'pipe',
    env: { ...process.env, SOURCE_DATE_EPOCH: '1700000000' },
  };
  execFileSync(process.execPath, command, options);
  const path = join(build, 'hotspot-arcade-cardputer.spdx.json');
  const first = readFileSync(path, 'utf8');
  execFileSync(process.execPath, command, options);
  assert.equal(readFileSync(path, 'utf8'), first);
  const spdx = JSON.parse(first);
  assert.equal(spdx.spdxVersion, 'SPDX-2.3');
  assert.equal(spdx.creationInfo.created, '2023-11-14T22:13:20.000Z');
  assert.equal(spdx.files.length, 3);
  assert.ok(spdx.packages.some((entry) => entry.name === 'hotspot-arcade'));
  assert.ok(spdx.packages.some((entry) => entry.name === 'M5Cardputer'));
});
