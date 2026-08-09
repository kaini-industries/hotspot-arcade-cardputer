import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import test from 'node:test';
import { fileURLToPath } from 'node:url';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const validator = join(root, 'tools', 'validate-build-paths.mjs');

function fixture() {
  const directory = mkdtempSync(join(tmpdir(), 'ha-build-paths-'));
  writeFileSync(join(directory, 'firmware.elf'), Buffer.from('clean elf fixture'));
  writeFileSync(join(directory, 'firmware.bin'), Buffer.from('clean binary fixture'));
  return directory;
}

test('build applies prefix maps to every compiler and validates executable artifacts', () => {
  const build = readFileSync(join(root, 'tools', 'build.sh'), 'utf8');
  assert.match(build, /compiler\.c\.extra_flags=\$PREFIX_MAP_FLAGS/);
  assert.match(build, /compiler\.cpp\.extra_flags=\$PREFIX_MAP_FLAGS/);
  assert.match(build, /compiler\.S\.extra_flags=\$PREFIX_MAP_FLAGS/);
  assert.match(build, /validate-build-paths\.mjs "\$BUILD_TMP" "\$ROOT" "\$ARDUINO_DATA" "\$ARDUINO_USER"/);
  const rootMap = build.indexOf('PREFIX_MAP_FLAGS="$(prefix_map_flag "$ROOT" .)"');
  const dataMap = build.indexOf('PREFIX_MAP_FLAGS+=" $(prefix_map_flag "$ARDUINO_DATA" .arduino-data)"');
  const userMap = build.indexOf('PREFIX_MAP_FLAGS+=" $(prefix_map_flag "$ARDUINO_USER" .arduino-user)"');
  assert.ok(rootMap >= 0 && rootMap < dataMap && dataMap < userMap);
});

test('build path validator rejects an absolute compiler root in ELF or firmware', () => {
  const directory = fixture();
  const forbidden = join(directory, 'checkout-root');
  try {
    execFileSync(process.execPath, [validator, directory, forbidden]);
    writeFileSync(join(directory, 'firmware.bin'), Buffer.from(`prefix ${forbidden}/source.cpp suffix`));
    assert.throws(
      () => execFileSync(process.execPath, [validator, directory, forbidden], { stdio: 'pipe' }),
      /Command failed/,
    );
  } finally {
    rmSync(directory, { recursive: true, force: true });
  }
});
