#!/usr/bin/env node
// Validate and apply the narrowly reviewed patches layered over locked Arduino
// Library Manager releases. Patches are staged and hash-checked before the
// installed source is replaced, so reruns are safe and partial writes cannot
// leave a library in an accepted state.

import { spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import {
  chmodSync,
  copyFileSync,
  existsSync,
  mkdirSync,
  mkdtempSync,
  readFileSync,
  renameSync,
  rmSync,
  statSync,
} from 'node:fs';
import { dirname, isAbsolute, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

export const M5GFX_CARDPUTER_ADVANCE_PATCH = Object.freeze({
  path: 'tools/patches/M5GFX-0.2.26-cardputer-advance.patch',
  sha256: '46a13e4ebe3ae0b79783220d2c47d36ec78a1e8325b55f36fe798c73d98acf3b',
  strip: 1,
  target: 'src/M5GFX.cpp',
  preimageSha256: '45d3a72c90fb71a69775970e5628fde365276dfc841461eb761429d08373187c',
  resultSha256: '19732e96ecb5bd339505f7935d0bcf4618316150d57036789c6adaeabdb2961d',
  upstreamRepository: 'https://github.com/m5stack/M5GFX',
  upstreamPullRequest: 233,
  upstreamCommit: '5f8a783f7dbc07e8ce5c19cf8779829d1eefcde1',
  upstreamMergeCommit: '701a8b4d23212644ddd65940ccc9f59107248386',
});

const SHA256 = /^[0-9a-f]{64}$/;
const safeRelativePath = (value) => typeof value === 'string'
  && value.length > 0
  && !isAbsolute(value)
  && !value.includes('\\')
  && value.split('/').every((part) => part.length > 0 && part !== '.' && part !== '..');
const fileSha256 = (path) => createHash('sha256').update(readFileSync(path)).digest('hex');

export function arduinoUserDirectory(root, environment = process.env) {
  const configured = environment.ARDUINO_DIRECTORIES_USER?.trim();
  if (!configured) return join(root, '.cache', 'arduino', 'user');
  return isAbsolute(configured) ? resolve(configured) : resolve(root, configured);
}

const fail = (message) => {
  throw new Error(`Arduino library patch: ${message}`);
};

export function validateArduinoLibraryPatches(lock, root) {
  const libraries = lock?.arduino?.libraries;
  if (!Array.isArray(libraries)) fail('toolchain lock has no Arduino library inventory');
  const matching = libraries.filter((entry) => entry?.name === 'M5GFX');
  if (matching.length !== 1 || matching[0].version !== '0.2.26') {
    fail('expected exactly one M5GFX 0.2.26 registry release');
  }

  const library = matching[0];
  if (!Array.isArray(library.patches) || library.patches.length !== 1) {
    fail('M5GFX 0.2.26 must carry exactly one reviewed patch');
  }
  const patch = library.patches[0];
  const expectedKeys = Object.keys(M5GFX_CARDPUTER_ADVANCE_PATCH).sort();
  const actualKeys = Object.keys(patch ?? {}).sort();
  if (JSON.stringify(actualKeys) !== JSON.stringify(expectedKeys)) {
    fail('M5GFX patch metadata fields do not match the reviewed contract');
  }
  for (const [key, expected] of Object.entries(M5GFX_CARDPUTER_ADVANCE_PATCH)) {
    if (patch[key] !== expected) fail(`M5GFX patch ${key} does not match the reviewed upstream fix`);
  }
  for (const field of ['sha256', 'preimageSha256', 'resultSha256']) {
    if (!SHA256.test(patch[field])) fail(`M5GFX patch ${field} is not SHA-256`);
  }
  for (const field of ['upstreamCommit', 'upstreamMergeCommit']) {
    if (!/^[0-9a-f]{40}$/.test(patch[field])) fail(`M5GFX patch ${field} is not a full commit`);
  }
  if (!safeRelativePath(patch.path) || !safeRelativePath(patch.target)) {
    fail('M5GFX patch contains an unsafe repository-relative path');
  }

  const patchPath = join(root, patch.path);
  if (!existsSync(patchPath) || !statSync(patchPath).isFile()) {
    fail(`reviewed patch is missing: ${patch.path}`);
  }
  const actualPatchSha256 = fileSha256(patchPath);
  if (actualPatchSha256 !== patch.sha256) {
    fail(`patch hash is ${actualPatchSha256}, expected ${patch.sha256}`);
  }

  return [{ library, patch, patchPath }];
}

export function verifyInstalledArduinoLibraryPatches(lock, root, environment = process.env) {
  const records = validateArduinoLibraryPatches(lock, root);
  for (const { library, patch } of records) {
    const targetPath = join(arduinoUserDirectory(root, environment), 'libraries', library.name, patch.target);
    if (!existsSync(targetPath) || !statSync(targetPath).isFile()) {
      fail(`installed target is missing: ${library.name}/${patch.target}`);
    }
    const actual = fileSha256(targetPath);
    if (actual !== patch.resultSha256) {
      fail(`installed ${library.name}/${patch.target} is ${actual}, expected patched ${patch.resultSha256}`);
    }
  }
  return records;
}

const runPatch = (arguments_, context) => {
  const result = spawnSync('patch', arguments_, { encoding: 'utf8' });
  if (result.error) fail(`${context}: ${result.error.message}`);
  if (result.status !== 0) {
    const detail = `${result.stderr ?? ''}${result.stdout ?? ''}`.trim();
    fail(`${context}${detail ? `: ${detail}` : ''}`);
  }
};

export function applyValidatedArduinoLibraryPatch(
  { library, patch, patchPath },
  root,
  environment = process.env,
) {
  if (
    typeof library?.name !== 'string'
    || library.name.length === 0
    || library.name.includes('/')
    || !safeRelativePath(patch?.target)
    || !Number.isInteger(patch?.strip)
    || patch.strip < 0
    || !SHA256.test(patch?.preimageSha256 ?? '')
    || !SHA256.test(patch?.resultSha256 ?? '')
    || typeof patchPath !== 'string'
  ) {
    fail('cannot apply malformed validated patch record');
  }
  const libraryRoot = join(arduinoUserDirectory(root, environment), 'libraries', library.name);
  const targetPath = join(libraryRoot, patch.target);
  if (!existsSync(targetPath) || !statSync(targetPath).isFile()) {
    fail(`installed target is missing: ${library.name}/${patch.target}`);
  }
  const actual = fileSha256(targetPath);
  if (actual === patch.resultSha256) {
    return { library: library.name, status: 'already-applied' };
  }
  if (actual !== patch.preimageSha256) {
    fail(`refusing to patch ${library.name}/${patch.target}: found ${actual}, expected ${patch.preimageSha256}`);
  }

  const stagingRoot = mkdtempSync(join(libraryRoot, '.patch-stage-'));
  try {
    const stagedTarget = join(stagingRoot, patch.target);
    mkdirSync(dirname(stagedTarget), { recursive: true });
    copyFileSync(targetPath, stagedTarget);
    const patchArguments = [
      '-f',
      '-s',
      '-F',
      '0',
      '-p',
      String(patch.strip),
      '-d',
      stagingRoot,
      '-i',
      patchPath,
    ];
    runPatch(['--dry-run', ...patchArguments], `patch dry-run failed for ${library.name}`);
    runPatch(patchArguments, `patch application failed for ${library.name}`);
    const stagedResult = fileSha256(stagedTarget);
    if (stagedResult !== patch.resultSha256) {
      fail(`patched ${library.name}/${patch.target} is ${stagedResult}, expected ${patch.resultSha256}`);
    }
    chmodSync(stagedTarget, statSync(targetPath).mode & 0o777);
    renameSync(stagedTarget, targetPath);
  } finally {
    rmSync(stagingRoot, { recursive: true, force: true });
  }
  if (fileSha256(targetPath) !== patch.resultSha256) {
    fail(`atomic replacement verification failed for ${library.name}/${patch.target}`);
  }
  return { library: library.name, status: 'applied' };
}

export function applyArduinoLibraryPatches(lock, root, environment = process.env) {
  return validateArduinoLibraryPatches(lock, root)
    .map((record) => applyValidatedArduinoLibraryPatch(record, root, environment));
}

const scriptPath = fileURLToPath(import.meta.url);
if (process.argv[1] && resolve(process.argv[1]) === scriptPath) {
  try {
    const root = resolve(dirname(scriptPath), '..');
    const lock = JSON.parse(readFileSync(join(root, 'tools', 'toolchain.lock.json'), 'utf8'));
    if (process.argv.includes('--check')) {
      verifyInstalledArduinoLibraryPatches(lock, root);
      console.log('reviewed Arduino library patches are installed');
    } else {
      for (const result of applyArduinoLibraryPatches(lock, root)) {
        console.log(`${result.library} patch ${result.status}`);
      }
    }
  } catch (error) {
    console.error(error.message);
    process.exitCode = 1;
  }
}
