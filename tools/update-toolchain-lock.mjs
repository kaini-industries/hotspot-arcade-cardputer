#!/usr/bin/env node
// Regenerate tools/toolchain.lock.json from exact versions in the project-local
// signed Arduino indexes. Review the resulting checksum diff before committing.

import { createHash } from 'node:crypto';
import { existsSync, readFileSync, statSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { M5GFX_CARDPUTER_ADVANCE_PATCH } from './arduino-library-patches.mjs';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const data = process.env.ARDUINO_DIRECTORIES_DATA
  ? resolve(root, process.env.ARDUINO_DIRECTORIES_DATA)
  : join(root, '.cache', 'arduino', 'data');
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
  ['LibSSH-ESP32', '5.9.0'],
  ['M5GFX', '0.2.26'],
  ['M5Unified', '0.2.19'],
  ['M5Cardputer', '1.1.1'],
];
const libraries = libraryPins.map(([name, version]) => {
  const entry = libraryIndex.libraries.find((candidate) => candidate.name === name && candidate.version === version);
  if (!entry) throw new Error(`library index does not contain ${name}@${version}`);
  const locked = {
    name,
    version,
    url: entry.url,
    file: entry.archiveFileName,
    size: Number(entry.size),
    sha256: checksum(entry.checksum, `${name}@${version}`),
    dependencies: (entry.dependencies ?? []).map((dependency) => dependency.name).sort(),
  };
  if (name === 'M5GFX') locked.patches = [{ ...M5GFX_CARDPUTER_ADVANCE_PATCH }];
  return locked;
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
          member: 'node-v24.19.0-darwin-arm64',
          sha256: '3f1cf157479c1480352083105e13faf9d008ede98e7e157746b6df940d197b94',
        },
        'linux-x64': {
          url: 'https://nodejs.org/dist/v24.19.0/node-v24.19.0-linux-x64.tar.xz',
          file: 'node-v24.19.0-linux-x64.tar.xz',
          member: 'node-v24.19.0-linux-x64',
          sha256: '14b342e71204f811bde6153be8e04b62aef63c236fef92b55f9c83154b409647',
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
      version: '6.0.6',
      installer: 'emsdk',
      installerCommit: '9981799f744be74ac67b1c1813ff172f63be0630',
      sdkReleaseCommit: '833aa203ba2283fc2b6adb504a79a3a0d692df81',
    },
    actionlint: {
      version: '1.7.12',
      sourceSha256: '454800bd4f854592bcfe79b161f71d56e35940eb7016e48a26dd356adc9d400a',
      archives: {
        'linux-x64': {
          url: 'https://github.com/rhysd/actionlint/releases/download/v1.7.12/actionlint_1.7.12_linux_amd64.tar.gz',
          file: 'actionlint_1.7.12_linux_amd64.tar.gz',
          member: 'actionlint',
          sha256: '8aca8db96f1b94770f1b0d72b6dddcb1ebb8123cb3712530b08cc387b349a3d8',
        },
      },
    },
    syft: {
      version: '1.51.0',
      archives: {
        'linux-x64': {
          url: 'https://github.com/anchore/syft/releases/download/v1.51.0/syft_1.51.0_linux_amd64.tar.gz',
          file: 'syft_1.51.0_linux_amd64.tar.gz',
          member: 'syft',
          sha256: '2a2e837a2c8d59ec9af5472ee22d3b04ee463c4e44476ecf993fd1e5ab6ebc7f',
        },
      },
    },
    cosign: {
      version: '3.1.3',
      archives: {
        'linux-x64': {
          url: 'https://github.com/sigstore/cosign/releases/download/v3.1.3/cosign-linux-amd64',
          file: 'cosign-linux-amd64-3.1.3',
          member: 'cosign',
          sha256: '4629c757b7618056f8ddd7e2625ae9fdd94c0372a65049520bc7d9df9efc7f71',
        },
      },
    },
    githubCli: {
      version: '2.97.0',
      archives: {
        'linux-x64': {
          url: 'https://github.com/cli/cli/releases/download/v2.97.0/gh_2.97.0_linux_amd64.tar.gz',
          file: 'gh_2.97.0_linux_amd64.tar.gz',
          member: 'gh_2.97.0_linux_amd64/bin/gh',
          sha256: 'a2c9b8497e1f85b1ad0dfcb78b5a622e098801b8e461e459e88e1ee12f018112',
        },
      },
    },
  },
  pythonReleaseDependencies: {
    requirements: 'tools/requirements-release.txt',
    sha256: sha256(readFileSync(join(root, 'tools', 'requirements-release.txt'))),
    packages: [
      { name: 'requests', version: '2.34.2' },
      { name: 'charset-normalizer', version: '3.5.0' },
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
