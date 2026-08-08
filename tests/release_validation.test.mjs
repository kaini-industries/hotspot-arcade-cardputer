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
import { readCleanGitSource } from '../tools/release-provenance.mjs';

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

test('release candidates require an explicit numbered rc tag for this VERSION', () => {
  const candidate = validateRelease({
    repoRoot: root,
    tag: 'v0.6.0-rc.1',
    candidate: true,
  });
  assert.equal(candidate.candidate, true);
  assert.equal(candidate.tag, 'v0.6.0-rc.1');
  for (const tag of ['', 'v0.6.0', 'v0.6.0-rc.0', 'v0.6.1-rc.1', 'v0.6.0-beta.1']) {
    assert.throws(
      () => validateRelease({ repoRoot: root, tag, candidate: true }),
      /candidate tag .* must match v0\.6\.0-rc\.<positive integer>/,
    );
  }
});

test('clean source provenance rejects tracked and untracked changes', () => {
  const repository = mkdtempSync(join(tmpdir(), 'hotspot-source-provenance-test-'));
  const git = (...args) => execFileSync('git', ['-C', repository, ...args], { stdio: 'pipe' });
  git('init', '--quiet');
  git('config', 'user.name', 'Release Test');
  git('config', 'user.email', 'release-test@example.invalid');
  writeFileSync(join(repository, 'tracked.txt'), 'clean\n');
  git('add', 'tracked.txt');
  git('commit', '--quiet', '-m', 'fixture');
  const clean = readCleanGitSource(repository);
  assert.match(clean.commit, /^[0-9a-f]{40}$/);
  assert.equal(clean.sourceTreeClean, true);

  writeFileSync(join(repository, 'untracked.txt'), 'dirty\n');
  assert.throws(() => readCleanGitSource(repository), /source checkout is dirty/);
});

test('release packaging emits stable checksums and manifest', () => {
  const build = mkdtempSync(join(tmpdir(), 'hotspot-release-test-'));
  writeFileSync(join(build, 'hotspot-arcade-cardputer.ino.bin'), 'app fixture');
  writeFileSync(join(build, 'hotspot-arcade-cardputer.full.bin'), 'full fixture');
  writeFileSync(join(build, 'hotspot-arcade-cardputer-m5burner.zip'), 'archive fixture');

  const command = [join(root, 'tools', 'package-release.mjs'), '--artifacts-dir', build, '--tag', 'v0.6.0'];
  const options = { cwd: root, stdio: 'pipe', env: { ...process.env, SOURCE_DATE_EPOCH: '1700000000' } };
  execFileSync(process.execPath, [join(root, 'tools', 'generate-sbom.mjs'), build], options);
  execFileSync(process.execPath, command, options);
  const firstManifest = readFileSync(join(build, 'build-manifest.json'), 'utf8');
  const firstChecksums = readFileSync(join(build, 'SHA256SUMS'), 'utf8');
  execFileSync(process.execPath, command, options);

  assert.equal(readFileSync(join(build, 'build-manifest.json'), 'utf8'), firstManifest);
  assert.equal(readFileSync(join(build, 'SHA256SUMS'), 'utf8'), firstChecksums);
  const manifest = JSON.parse(firstManifest);
  assert.equal(manifest.repository, CANONICAL_REPOSITORY);
  assert.equal(manifest.version, '0.6.0');
  assert.match(manifest.commit, /^[0-9a-f]{40}$/);
  assert.equal(manifest.sourceTreeClean, true);
  assert.equal(manifest.candidate, false);
  assert.equal(manifest.sourceDateEpoch, 1700000000);
  assert.equal(manifest.upstream.repository, JSON.parse(readFileSync(join(root, 'UPSTREAM.lock.json'))).repository);
  assert.equal(manifest.upstream.sourceTreeSha256, JSON.parse(readFileSync(join(root, 'UPSTREAM.lock.json'))).sourceTreeSha256);
  assert.equal(manifest.fqbn, JSON.parse(readFileSync(join(root, 'tools', 'toolchain.lock.json'))).arduino.fqbn);
  assert.equal(manifest.installLayouts.app.flashOffset, '0x170000');
  assert.deepEqual(
    manifest.installLayouts.m5burner.components.map((item) => item.flashOffset),
    ['0x0', '0x8000', '0xe000', '0x10000'],
  );
  assert.equal(manifest.artifacts.length, 4);
  assert.match(firstChecksums, /hotspot-arcade-cardputer\.full\.bin/);
  assert.match(firstChecksums, /build-manifest\.json/);
});

test('release metadata failure preserves the previous atomic outputs', () => {
  const build = mkdtempSync(join(tmpdir(), 'hotspot-release-atomic-test-'));
  for (const [name, content] of [
    ['hotspot-arcade-cardputer.ino.bin', 'app fixture'],
    ['hotspot-arcade-cardputer.full.bin', 'full fixture'],
    ['hotspot-arcade-cardputer-m5burner.zip', 'archive fixture'],
  ]) writeFileSync(join(build, name), content);
  const command = [join(root, 'tools', 'package-release.mjs'), '--artifacts-dir', build, '--tag', 'v0.6.0'];
  const options = { cwd: root, stdio: 'pipe', env: { ...process.env, SOURCE_DATE_EPOCH: '1700000000' } };
  execFileSync(process.execPath, [join(root, 'tools', 'generate-sbom.mjs'), build], options);
  execFileSync(process.execPath, command, options);
  const manifest = readFileSync(join(build, 'build-manifest.json'), 'utf8');
  const checksums = readFileSync(join(build, 'SHA256SUMS'), 'utf8');
  writeFileSync(join(build, 'hotspot-arcade-cardputer.spdx.json'), '{}\n');
  assert.throws(() => execFileSync(process.execPath, command, options));
  assert.equal(readFileSync(join(build, 'build-manifest.json'), 'utf8'), manifest);
  assert.equal(readFileSync(join(build, 'SHA256SUMS'), 'utf8'), checksums);
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
  assert.ok(spdx.packages.some((entry) => entry.name === 'arduino-cli'));
  assert.ok(spdx.packages.some((entry) => entry.name === 'esptool_py'));
  assert.ok(spdx.packages.some((entry) => entry.name === 'AsyncTCP'));
  const rootPackage = spdx.packages.find((entry) => entry.name === 'hotspot-arcade-cardputer');
  assert.equal(rootPackage.supplier, 'Organization: Kaini Industries');
  assert.match(rootPackage.packageVerificationCode.packageVerificationCodeValue, /^[0-9a-f]{40}$/);
  execFileSync(process.execPath, [join(root, 'tools', 'validate-sbom.mjs'), build], options);

  writeFileSync(join(build, 'hotspot-arcade-cardputer.ino.bin'), 'tampered app fixture');
  assert.throws(() => execFileSync(process.execPath, [join(root, 'tools', 'validate-sbom.mjs'), build], options));
});

test('release metadata derives upstream repository provenance only from the strict lock', () => {
  assert.doesNotMatch(readFileSync(join(root, 'tools', 'package-release.mjs'), 'utf8'), /github\.com\/tarikbc/);
  assert.doesNotMatch(readFileSync(join(root, 'tools', 'generate-sbom.mjs'), 'utf8'), /github\.com\/tarikbc/);
});

test('workflows gate publication on isolated reproducibility and verified attestations', () => {
  const ci = readFileSync(join(root, '.github', 'workflows', 'ci.yml'), 'utf8');
  const release = readFileSync(join(root, '.github', 'workflows', 'release.yml'), 'utf8');
  for (const workflow of [ci, release]) {
    assert.equal((workflow.match(/git worktree add --detach/g) ?? []).length, 2);
    assert.match(workflow, /release-hashes\.mjs/);
    assert.match(workflow, /diff -u "\$RUNNER_TEMP\/first\.sha256" "\$RUNNER_TEMP\/second\.sha256"/);
    assert.match(workflow, /tools\/test-native\.sh --tsan/);
    assert.match(workflow, /tools\/bootstrap-ci-tools\.sh/);
    assert.doesNotMatch(workflow, /go install/);
  }
  assert.match(release, /\n  workflow_dispatch:\n/);
  assert.match(release, /default: 0\.6\.0-rc\.1/);
  assert.match(release, /VALIDATE_ARGS\+=\(--candidate\)/);
  assert.match(release, /BUILD_ARGS\+=\(--candidate\)/);
  assert.match(release, /refs\/tags\/\$RELEASE_TAG\^\{commit\}/);
  assert.match(release, /verify-build-provenance:/);
  assert.match(release, /gh attestation verify/);
  assert.match(release, /--signer-workflow/);
  for (const job of ['draft-github-release', 'publish-m5burner', 'finalize-github-release']) {
    const start = release.indexOf(`  ${job}:`);
    assert.notEqual(start, -1);
    assert.match(release.slice(start, start + 180), /if: github\.event_name == 'push'/);
  }
});

test('CI host tools use reviewed release archives instead of runner globals', () => {
  const lock = JSON.parse(readFileSync(join(root, 'tools', 'toolchain.lock.json'), 'utf8'));
  for (const name of ['actionlint', 'syft', 'cosign', 'githubCli']) {
    const tool = lock.hostTools[name];
    assert.match(tool.version, /^\d+\.\d+\.\d+$/);
    assert.match(tool.archives['linux-x64'].url, /^https:\/\/github\.com\//);
    assert.match(tool.archives['linux-x64'].sha256, /^[0-9a-f]{64}$/);
  }
  const bootstrap = readFileSync(join(root, 'tools', 'bootstrap-ci-tools.sh'), 'utf8');
  assert.match(bootstrap, /sha256sum --check --status/);
  assert.match(bootstrap, /curl --proto '=https' --tlsv1\.2/);
});

test('fresh detached candidates generate headers and share the locked Arduino data path', () => {
  const candidate = readFileSync(join(root, 'tools', 'build-release-candidate.sh'), 'utf8');
  const firstGeneration = candidate.indexOf('node tools/gen-assets.mjs\n');
  const build = candidate.indexOf('tools/build.sh');
  const finalCheck = candidate.indexOf('node tools/gen-assets.mjs --check');
  assert.ok(firstGeneration >= 0 && firstGeneration < build && build < finalCheck);

  const budgets = readFileSync(join(root, 'tools', 'check-build-budgets.mjs'), 'utf8');
  assert.match(budgets, /process\.env\.ARDUINO_DIRECTORIES_DATA/);
});
