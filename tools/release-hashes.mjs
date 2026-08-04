#!/usr/bin/env node
// Print a stable SHA-256 inventory for every uploaded/attested release output.

import { existsSync, readFileSync, statSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { fileDigest } from './release-provenance.mjs';

const repository = resolve(process.argv[2] ?? join(dirname(fileURLToPath(import.meta.url)), '..'));
const paths = [
  'build/hotspot-arcade-cardputer.ino.bin',
  'build/hotspot-arcade-cardputer.full.bin',
  'build/hotspot-arcade-cardputer-m5burner.zip',
  'build/hotspot-arcade-cardputer.spdx.json',
  'build/build-manifest.json',
  'build/SHA256SUMS',
  'firmware/cardputer/bootloader_0x0.bin',
  'firmware/cardputer/partitions_0x8000.bin',
  'firmware/cardputer/boot_app0_0xe000.bin',
  'firmware/cardputer/hotspot-arcade_0x10000.bin',
];
for (const relativePath of paths) {
  const path = join(repository, relativePath);
  if (!existsSync(path) || !statSync(path).isFile() || statSync(path).size === 0) {
    throw new Error(`release output is missing or empty: ${path}`);
  }
  process.stdout.write(`${fileDigest(path)}  ${relativePath}\n`);
}

// Ensure the publisher-facing checksum list binds every public build artifact.
const checksums = readFileSync(join(repository, 'build', 'SHA256SUMS'), 'utf8');
const checksumLines = checksums.trimEnd().split('\n');
if (checksumLines.length !== 5 || new Set(checksumLines).size !== checksumLines.length) {
  throw new Error('SHA256SUMS must contain exactly five unique release entries');
}
for (const relativePath of paths.slice(0, 5)) {
  const name = relativePath.slice('build/'.length);
  const expected = `${fileDigest(join(repository, relativePath))}  ${name}`;
  if (!checksums.split('\n').includes(expected)) throw new Error(`SHA256SUMS does not bind ${name}`);
}
