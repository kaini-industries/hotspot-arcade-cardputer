#!/usr/bin/env node
import { execFileSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { existsSync, mkdtempSync, readFileSync, rmSync, statSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { basename, dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const lock = JSON.parse(readFileSync(join(root, 'tools', 'toolchain.lock.json'), 'utf8'));
const lockedEsptoolVersion = lock.hostTools?.esptool?.version;
if (!/^\d+\.\d+\.\d+$/.test(lockedEsptoolVersion ?? '')) {
  throw new Error('toolchain.lock.json is missing a valid hostTools.esptool.version');
}
let buildDirectory = join(root, 'build');
let bootAppPath = '';
for (let index = 2; index < process.argv.length; index += 1) {
  if (process.argv[index] === '--build') buildDirectory = resolve(process.argv[++index] ?? '');
  else if (process.argv[index] === '--boot-app') bootAppPath = resolve(process.argv[++index] ?? '');
  else throw new Error(`unknown argument ${process.argv[index]}`);
}
if (!bootAppPath) throw new Error('--boot-app is required');

const images = [
  { name: 'hotspot-arcade-cardputer.ino.bootloader.bin', offset: 0x0, limit: 0x8000, image: true },
  { name: 'hotspot-arcade-cardputer.ino.partitions.bin', offset: 0x8000, limit: 0xe000 },
  { name: basename(bootAppPath), path: bootAppPath, offset: 0xe000, limit: 0x10000 },
  { name: 'hotspot-arcade-cardputer.ino.bin', offset: 0x10000, limit: 0x340000, image: true },
];
for (const image of images) {
  image.path ??= join(buildDirectory, image.name);
  if (!existsSync(image.path) || !statSync(image.path).isFile() || statSync(image.path).size === 0) {
    throw new Error(`missing nonempty image ${image.path}`);
  }
  image.size = statSync(image.path).size;
  if (image.offset + image.size > image.limit) {
    throw new Error(`${image.name} overlaps the next partition: 0x${image.offset.toString(16)} + ${image.size}`);
  }
}

const version = execFileSync('esptool', ['version'], { encoding: 'utf8' }).trim();
const actualEsptoolVersion = version.match(/\besptool(?:\.py)?\s+v?(\d+\.\d+\.\d+)\b/i)?.[1];
if (actualEsptoolVersion !== lockedEsptoolVersion) {
  throw new Error(`esptool ${lockedEsptoolVersion} is required; found ${version}`);
}

const fullPath = join(buildDirectory, 'hotspot-arcade-cardputer.full.bin');
if (!existsSync(fullPath) || !statSync(fullPath).isFile() || statSync(fullPath).size === 0) {
  throw new Error(`missing nonempty full image ${fullPath}`);
}
for (const image of [...images.filter((entry) => entry.image), { name: basename(fullPath), path: fullPath }]) {
  const info = execFileSync('esptool', ['image-info', image.path], { encoding: 'utf8' });
  if (!info.includes('Detected image type: ESP32-S3')) throw new Error(`${image.name} is not an ESP32-S3 image`);
  if (!info.includes('Flash size: 8MB')) throw new Error(`${image.name} does not declare 8MB flash`);
  if (!info.includes('(valid)')) throw new Error(`${image.name} did not pass esptool validation`);
}

const partitionPath = join(buildDirectory, 'hotspot-arcade-cardputer.ino.partitions.bin');
const partitionBytes = readFileSync(partitionPath);
if (partitionBytes.length > 0x1000) throw new Error('partition table exceeds the reserved 0x1000-byte region');
const partitions = new Map();
let sawPartitionMd5 = false;
for (let offset = 0; offset + 32 <= partitionBytes.length; offset += 32) {
  const magic = partitionBytes.readUInt16LE(offset);
  if (magic === 0xffff) break;
  if (magic === 0xebeb) {
    const expectedMd5 = partitionBytes.subarray(offset + 16, offset + 32);
    const actualMd5 = createHash('md5').update(partitionBytes.subarray(0, offset)).digest();
    if (!actualMd5.equals(expectedMd5)) throw new Error('partition-table MD5 is invalid');
    sawPartitionMd5 = true;
    if (partitionBytes.subarray(offset + 32).some((byte) => byte !== 0xff)) {
      throw new Error('partition table contains non-0xff data after its MD5 record');
    }
    break;
  }
  if (magic !== 0x50aa) throw new Error(`invalid partition-table magic at byte ${offset}`);
  const flashOffset = partitionBytes.readUInt32LE(offset + 4);
  const size = partitionBytes.readUInt32LE(offset + 8);
  const labelBytes = partitionBytes.subarray(offset + 12, offset + 28);
  const nul = labelBytes.indexOf(0);
  const label = labelBytes.subarray(0, nul < 0 ? labelBytes.length : nul).toString('ascii');
  if (!label || partitions.has(label)) throw new Error(`invalid or duplicate partition label ${label}`);
  partitions.set(label, { offset: flashOffset, size });
}
if (!sawPartitionMd5) throw new Error('partition table is missing its MD5 record');
const expectedPartitions = {
  nvs: [0x9000, 0x5000],
  otadata: [0xe000, 0x2000],
  app0: [0x10000, 0x330000],
  app1: [0x340000, 0x330000],
  spiffs: [0x670000, 0x180000],
  coredump: [0x7f0000, 0x10000],
};
if (partitions.size !== Object.keys(expectedPartitions).length) {
  throw new Error(`partition table must contain exactly ${Object.keys(expectedPartitions).length} entries`);
}
for (const [label, [offset, size]] of Object.entries(expectedPartitions)) {
  const actual = partitions.get(label);
  if (!actual || actual.offset !== offset || actual.size !== size) {
    throw new Error(`partition ${label} must be offset 0x${offset.toString(16)}, size 0x${size.toString(16)}`);
  }
}

// Rebuild the complete 0x0 image with the locked esptool. This independently
// verifies every component offset, every erased gap, and the trimmed full-image
// boundary instead of trusting the Arduino-generated merged image filename.
const mergeDirectory = mkdtempSync(join(tmpdir(), 'hotspot-image-validation-'));
try {
  const reconstructedPath = join(mergeDirectory, 'reconstructed.bin');
  execFileSync(
    'esptool',
    [
      '--chip',
      'esp32s3',
      'merge-bin',
      '--output',
      reconstructedPath,
      '--flash-size',
      '8MB',
      ...images.flatMap((image) => [`0x${image.offset.toString(16)}`, image.path]),
    ],
    { encoding: 'utf8' },
  );
  const reconstructed = readFileSync(reconstructedPath);
  const full = readFileSync(fullPath);
  const expectedFullSize = Math.ceil(reconstructed.length / 0x1000) * 0x1000;
  if (full.length !== expectedFullSize) {
    throw new Error(`full image must be trimmed to 0x${expectedFullSize.toString(16)} bytes; found 0x${full.length.toString(16)}`);
  }
  if (!reconstructed.equals(full.subarray(0, reconstructed.length))) {
    throw new Error('full image components or fixed offsets do not match the locked esptool reconstruction');
  }
  if (full.subarray(reconstructed.length).some((byte) => byte !== 0xff)) {
    throw new Error('full image has non-0xff data after the reconstructed image boundary');
  }
} finally {
  rmSync(mergeDirectory, { recursive: true, force: true });
}

console.log(
  `validated app/full images, ${images.length} fixed-offset components, locked esptool ${lockedEsptoolVersion}, and ${partitions.size} partitions`,
);
