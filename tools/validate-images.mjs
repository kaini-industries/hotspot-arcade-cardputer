#!/usr/bin/env node
import { execFileSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { existsSync, readFileSync, statSync } from 'node:fs';
import { basename, dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
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
if (!/\bv?5\.3\.1\b/.test(version)) throw new Error(`esptool 5.3.1 is required; found ${version}`);
for (const image of images.filter((entry) => entry.image)) {
  const info = execFileSync('esptool', ['image-info', image.path], { encoding: 'utf8' });
  if (!info.includes('Detected image type: ESP32-S3')) throw new Error(`${image.name} is not an ESP32-S3 image`);
  if (!info.includes('Flash size: 8MB')) throw new Error(`${image.name} does not declare 8MB flash`);
  if (!info.includes('(valid)')) throw new Error(`${image.name} did not pass esptool validation`);
}

const partitionPath = join(buildDirectory, 'hotspot-arcade-cardputer.ino.partitions.bin');
const partitionBytes = readFileSync(partitionPath);
if (partitionBytes.length > 0x1000) throw new Error('partition table exceeds the reserved 0x1000-byte region');
const partitions = new Map();
for (let offset = 0; offset + 32 <= partitionBytes.length; offset += 32) {
  const magic = partitionBytes.readUInt16LE(offset);
  if (magic === 0xffff) break;
  if (magic === 0xebeb) {
    const expectedMd5 = partitionBytes.subarray(offset + 16, offset + 32);
    const actualMd5 = createHash('md5').update(partitionBytes.subarray(0, offset)).digest();
    if (!actualMd5.equals(expectedMd5)) throw new Error('partition-table MD5 is invalid');
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
const expectedPartitions = {
  nvs: [0x9000, 0x5000],
  otadata: [0xe000, 0x2000],
  app0: [0x10000, 0x330000],
  app1: [0x340000, 0x330000],
  spiffs: [0x670000, 0x180000],
  coredump: [0x7f0000, 0x10000],
};
for (const [label, [offset, size]] of Object.entries(expectedPartitions)) {
  const actual = partitions.get(label);
  if (!actual || actual.offset !== offset || actual.size !== size) {
    throw new Error(`partition ${label} must be offset 0x${offset.toString(16)}, size 0x${size.toString(16)}`);
  }
}
console.log(`validated ${images.length} images, fixed offsets, ESP32-S3 metadata, and ${partitions.size} partitions`);
