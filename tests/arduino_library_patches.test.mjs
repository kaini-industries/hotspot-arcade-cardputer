import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import {
  appendFileSync,
  copyFileSync,
  mkdirSync,
  mkdtempSync,
  readFileSync,
  readdirSync,
  rmSync,
  writeFileSync,
} from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import test from 'node:test';
import { fileURLToPath } from 'node:url';

import {
  M5GFX_CARDPUTER_ADVANCE_PATCH,
  applyValidatedArduinoLibraryPatch,
  arduinoUserDirectory,
  validateArduinoLibraryPatches,
} from '../tools/arduino-library-patches.mjs';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const sha256 = (bytes) => createHash('sha256').update(bytes).digest('hex');

test('the lock binds M5GFX 0.2.26 to the exact accepted upstream fix', () => {
  const lock = JSON.parse(readFileSync(join(root, 'tools', 'toolchain.lock.json'), 'utf8'));
  const records = validateArduinoLibraryPatches(lock, root);
  assert.equal(records.length, 1);
  assert.deepEqual(records[0].patch, M5GFX_CARDPUTER_ADVANCE_PATCH);

  for (const [field, value] of [
    ['upstreamRepository', 'https://github.com/example/M5GFX'],
    ['upstreamCommit', '0'.repeat(40)],
    ['resultSha256', '0'.repeat(64)],
    ['target', '../M5GFX.cpp'],
  ]) {
    const changed = structuredClone(lock);
    changed.arduino.libraries.find((entry) => entry.name === 'M5GFX').patches[0][field] = value;
    assert.throws(
      () => validateArduinoLibraryPatches(changed, root),
      /does not match the reviewed upstream fix/,
    );
  }
});

test('the checked-in patch hash is verified from its bytes', () => {
  const fixtureRoot = mkdtempSync(join(tmpdir(), 'ha-library-patch-lock-'));
  const lock = JSON.parse(readFileSync(join(root, 'tools', 'toolchain.lock.json'), 'utf8'));
  const relativePatch = M5GFX_CARDPUTER_ADVANCE_PATCH.path;
  const copiedPatch = join(fixtureRoot, relativePatch);
  try {
    mkdirSync(dirname(copiedPatch), { recursive: true });
    copyFileSync(join(root, relativePatch), copiedPatch);
    validateArduinoLibraryPatches(lock, fixtureRoot);
    appendFileSync(copiedPatch, '\n');
    assert.throws(() => validateArduinoLibraryPatches(lock, fixtureRoot), /patch hash is/);
  } finally {
    rmSync(fixtureRoot, { recursive: true, force: true });
  }
});

function patchFixture() {
  const fixtureRoot = mkdtempSync(join(tmpdir(), 'ha-library-patch-apply-'));
  const userRoot = join(fixtureRoot, 'isolated-arduino-user');
  const libraryRoot = join(userRoot, 'libraries', 'M5GFX');
  const target = 'src/sample.cpp';
  const targetPath = join(libraryRoot, target);
  const patchPath = join(fixtureRoot, 'sample.patch');
  const preimage = 'int value = 1;\n';
  const result = 'int value = 2;\n';
  mkdirSync(dirname(targetPath), { recursive: true });
  writeFileSync(targetPath, preimage);
  writeFileSync(
    patchPath,
    [
      'diff --git a/src/sample.cpp b/src/sample.cpp',
      '--- a/src/sample.cpp',
      '+++ b/src/sample.cpp',
      '@@ -1 +1 @@',
      '-int value = 1;',
      '+int value = 2;',
      '',
    ].join('\n'),
  );
  return {
    fixtureRoot,
    libraryRoot,
    targetPath,
    preimage,
    result,
    environment: { ARDUINO_DIRECTORIES_USER: userRoot },
    record: {
      library: { name: 'M5GFX' },
      patch: {
        target,
        strip: 1,
        preimageSha256: sha256(preimage),
        resultSha256: sha256(result),
      },
      patchPath,
    },
  };
}

test('patch application is isolated, atomic, and idempotent', () => {
  const fixture = patchFixture();
  try {
    assert.equal(
      arduinoUserDirectory(fixture.fixtureRoot, fixture.environment),
      fixture.environment.ARDUINO_DIRECTORIES_USER,
    );
    assert.deepEqual(
      applyValidatedArduinoLibraryPatch(fixture.record, fixture.fixtureRoot, fixture.environment),
      { library: 'M5GFX', status: 'applied' },
    );
    assert.equal(readFileSync(fixture.targetPath, 'utf8'), fixture.result);
    assert.deepEqual(
      applyValidatedArduinoLibraryPatch(fixture.record, fixture.fixtureRoot, fixture.environment),
      { library: 'M5GFX', status: 'already-applied' },
    );
    assert.equal(readdirSync(fixture.libraryRoot).some((entry) => entry.startsWith('.patch-stage-')), false);

    writeFileSync(fixture.targetPath, 'unexpected local edit\n');
    assert.throws(
      () => applyValidatedArduinoLibraryPatch(fixture.record, fixture.fixtureRoot, fixture.environment),
      /refusing to patch/,
    );
  } finally {
    rmSync(fixture.fixtureRoot, { recursive: true, force: true });
  }
});

test('a wrong result hash leaves the installed preimage untouched', () => {
  const fixture = patchFixture();
  try {
    fixture.record.patch.resultSha256 = sha256('different expected result\n');
    assert.throws(
      () => applyValidatedArduinoLibraryPatch(fixture.record, fixture.fixtureRoot, fixture.environment),
      /patched M5GFX\/src\/sample\.cpp is/,
    );
    assert.equal(readFileSync(fixture.targetPath, 'utf8'), fixture.preimage);
    assert.equal(readdirSync(fixture.libraryRoot).some((entry) => entry.startsWith('.patch-stage-')), false);
  } finally {
    rmSync(fixture.fixtureRoot, { recursive: true, force: true });
  }
});

test('bootstrap applies the patch before installed-state verification', () => {
  const bootstrap = readFileSync(join(root, 'tools', 'bootstrap.sh'), 'utf8');
  const build = readFileSync(join(root, 'tools', 'build.sh'), 'utf8');
  const doctor = readFileSync(join(root, 'tools', 'doctor.sh'), 'utf8');
  const updater = readFileSync(join(root, 'tools', 'update-toolchain-lock.mjs'), 'utf8');
  const verifier = readFileSync(join(root, 'tools', 'verify-toolchain-lock.mjs'), 'utf8');
  const applyAt = bootstrap.indexOf('node tools/arduino-library-patches.mjs');
  const verifyAt = bootstrap.indexOf('node tools/verify-toolchain-lock.mjs --target "$TARGET" --installed');
  assert.ok(applyAt >= 0 && verifyAt > applyAt);
  const buildCheckAt = build.indexOf('node tools/arduino-library-patches.mjs --check');
  const compileAt = build.indexOf('"$ARDUINO_CLI" --config-file tools/arduino-cli.yaml compile');
  assert.ok(buildCheckAt >= 0 && compileAt > buildCheckAt);
  assert.match(doctor, /node tools\/arduino-library-patches\.mjs --check/);
  assert.match(updater, /locked\.patches = \[\{ \.\.\.M5GFX_CARDPUTER_ADVANCE_PATCH \}\]/);
  assert.match(verifier, /if \(verifyInstalled\) verifyInstalledArduinoLibraryPatches\(lock, root\)/);
});
