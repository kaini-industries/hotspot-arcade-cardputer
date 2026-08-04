#!/usr/bin/env node
// Regenerate tools/toolchain.lock.json from exact versions in the project-local
// signed Arduino indexes. Review the resulting checksum diff before committing.

import { createHash } from 'node:crypto';
import { existsSync, readFileSync, statSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const data = join(root, '.cache', 'arduino', 'data');
const readJson = (name) => JSON.parse(readFileSync(join(data, name), 'utf8'));
const libraryIndex = readJson('library_index.json');
const packageIndex = readJson('package_index.json');
const esp32Index = readJson('package_esp32_index.json');
const sha256 = (bytes) => createHash('sha256').update(bytes).digest('hex');
const checksum = (value, context) => {
  const match = /^SHA-256:([0-9a-f]{64})$/.exec(value ?? '');
  if (!match) throw new Error(`${context} has no SHA-256 checksum`);
  return match[1];
};
const archive = (entry, context) => ({
  host: entry.host,
  url: entry.url,
  file: entry.archiveFileName,
  size: Number(entry.size),
  sha256: checksum(entry.checksum, context),
});

const libraryPins = [
  ['IRremote', '4.7.1'],
  ['LibSSH-ESP32', '5.8.0'],
  ['M5GFX', '0.2.26'],
  ['M5Unified', '0.2.19'],
  ['M5Cardputer', '1.1.1'],
];
const libraries = libraryPins.map(([name, version]) => {
  const entry = libraryIndex.libraries.find((candidate) => candidate.name === name && candidate.version === version);
  if (!entry) throw new Error(`library index does not contain ${name}@${version}`);
  return {
    name,
    version,
    url: entry.url,
    file: entry.archiveFileName,
    size: Number(entry.size),
    sha256: checksum(entry.checksum, `${name}@${version}`),
    dependencies: (entry.dependencies ?? []).map((dependency) => dependency.name).sort(),
  };
});

const esp32Package = esp32Index.packages.find((entry) => entry.name === 'esp32');
const arduinoPackage = packageIndex.packages.find((entry) => entry.name === 'arduino');
const coreEntry = esp32Package.platforms.find((entry) => entry.version === '3.3.11');
if (!coreEntry) throw new Error('ESP32 index does not contain core 3.3.11');
const bootAppPath = join(data, 'packages', 'esp32', 'hardware', 'esp32', '3.3.11', 'tools', 'partitions', 'boot_app0.bin');
if (!existsSync(bootAppPath)) throw new Error('install ESP32 core 3.3.11 before locking boot_app0.bin');
const targetPreferences = {
  'darwin-arm64': [
    'arm64-apple-darwin',
    'aarch64-apple-darwin',
    'i386-apple-darwin11',
    'x86_64-apple-darwin',
  ],
  'linux-x64': [
    'x86_64-linux-gnu',
    'x86_64-pc-linux-gnu',
  ],
};
const systemFor = (tool, target) => {
  const preferences = targetPreferences[target];
  for (const host of preferences) {
    const exact = tool.systems.find((system) => system.host === host);
    if (exact) return exact;
  }
  const compatible = tool.systems.find((system) => target === 'linux-x64'
    ? system.host.startsWith('x86_64-') && system.host.includes('linux')
    : (system.host.includes('apple-darwin') || system.host.includes('apple-darwin')));
  if (!compatible) throw new Error(`${tool.name}@${tool.version} has no ${target} archive`);
  return compatible;
};
const coreTools = coreEntry.toolsDependencies.map((dependency) => {
  const packageEntry = dependency.packager === 'esp32' ? esp32Package : arduinoPackage;
  const tool = packageEntry.tools.find((entry) => entry.name === dependency.name && entry.version === dependency.version);
  if (!tool) throw new Error(`package index lacks ${dependency.packager}:${dependency.name}@${dependency.version}`);
  return {
    packager: dependency.packager,
    name: dependency.name,
    version: dependency.version,
    archives: Object.fromEntries(Object.keys(targetPreferences).map((target) => [
      target,
      archive(systemFor(tool, target), `${dependency.name}@${dependency.version} for ${target}`),
    ])),
  };
});

const lock = {
  schema: 1,
  node: {
    version: '24.19.0',
    archives: {
      'darwin-arm64': {
        url: 'https://nodejs.org/dist/v24.19.0/node-v24.19.0-darwin-arm64.tar.xz',
        file: 'node-v24.19.0-darwin-arm64.tar.xz',
        sha256: '3f1cf157479c1480352083105e13faf9d008ede98e7e157746b6df940d197b94',
      },
    },
  },
  hostTools: {
    arduinoCli: {
      version: '1.5.1',
      sourceSha256: '262fbe874f62677d01eb15593144ca82bd6e7c2d0c00f82aa93949ab12924de6',
      archives: {
        'darwin-arm64': {
          url: 'https://github.com/arduino/arduino-cli/releases/download/v1.5.1/arduino-cli_1.5.1_macOS_ARM64.tar.gz',
          file: 'arduino-cli_1.5.1_macOS_ARM64.tar.gz',
          sha256: 'cb952e8c1621c95ef5f1d17831c945e3d0ec5973f89c557a7ec8feb9c4f7d4c9',
        },
        'linux-x64': {
          url: 'https://github.com/arduino/arduino-cli/releases/download/v1.5.1/arduino-cli_1.5.1_Linux_64bit.tar.gz',
          file: 'arduino-cli_1.5.1_Linux_64bit.tar.gz',
          sha256: '28a8e119c498a25607821c36cb2dc49e8463941b261a0d99091baa7bc692dd2b',
        },
      },
    },
    esptool: {
      version: '5.3.1',
      sourceSha256: '125781f36e6a2d08c484524a45f340694675368b5eeead9d0cb21b2034a91d98',
    },
    emscripten: {
      version: '6.0.2',
      installer: 'emsdk',
      installerCommit: 'c8f3a11e8813ed68428c750e283fe72a362fab8e',
      sdkReleaseCommit: '004876f1984e18a9eb0736c5ca417ac86d386fb8',
    },
    actionlint: {
      version: '1.7.12',
      sourceSha256: '454800bd4f854592bcfe79b161f71d56e35940eb7016e48a26dd356adc9d400a',
    },
  },
  pythonReleaseDependencies: {
    requirements: 'tools/requirements-release.txt',
    sha256: sha256(readFileSync(join(root, 'tools', 'requirements-release.txt'))),
    packages: [
      { name: 'requests', version: '2.32.3' },
      { name: 'charset-normalizer', version: '3.4.9' },
      { name: 'idna', version: '3.18' },
      { name: 'urllib3', version: '2.7.0' },
      { name: 'certifi', version: '2026.7.22' },
    ],
  },
  arduino: {
    cliVersion: '1.5.1',
    config: 'tools/arduino-cli.yaml',
    fqbn: 'esp32:esp32:m5stack_cardputer:FlashSize=8M,PartitionScheme=default_8MB',
    core: {
      packager: 'esp32',
      architecture: 'esp32',
      version: coreEntry.version,
      url: coreEntry.url,
      file: coreEntry.archiveFileName,
      size: Number(coreEntry.size),
      sha256: checksum(coreEntry.checksum, 'ESP32 core 3.3.11'),
    },
    bootApp0: {
      relativeToData: 'packages/esp32/hardware/esp32/3.3.11/tools/partitions/boot_app0.bin',
      size: statSync(bootAppPath).size,
      sha256: sha256(readFileSync(bootAppPath)),
    },
    coreTools,
    libraries,
  },
  indexDigests: {
    arduinoPackagesSha256: sha256(readFileSync(join(data, 'package_index.json'))),
    esp32PackagesSha256: sha256(readFileSync(join(data, 'package_esp32_index.json'))),
    librariesSha256: sha256(readFileSync(join(data, 'library_index.json'))),
  },
};

writeFileSync(join(root, 'tools', 'toolchain.lock.json'), `${JSON.stringify(lock, null, 2)}\n`, 'utf8');
console.log('wrote tools/toolchain.lock.json');
