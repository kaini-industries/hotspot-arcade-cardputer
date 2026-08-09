#!/usr/bin/env node
// Verify that mutable package indexes still describe exactly the archives and
// checksums reviewed in tools/toolchain.lock.json before Arduino CLI downloads.

import { createHash } from 'node:crypto';
import { existsSync, readFileSync, readdirSync, statSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import {
  validateArduinoLibraryPatches,
  verifyInstalledArduinoLibraryPatches,
} from './arduino-library-patches.mjs';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const lock = JSON.parse(readFileSync(join(root, 'tools', 'toolchain.lock.json'), 'utf8'));
const verifyInstalled = process.argv.includes('--installed');
const configuredDirectory = (variable, fallback) => process.env[variable]
  ? resolve(root, process.env[variable])
  : join(root, '.cache', 'arduino', fallback);
const data = configuredDirectory('ARDUINO_DIRECTORIES_DATA', 'data');
const indexes = {
  libraries: JSON.parse(readFileSync(join(data, 'library_index.json'), 'utf8')),
  arduino: JSON.parse(readFileSync(join(data, 'package_index.json'), 'utf8')),
  esp32: JSON.parse(readFileSync(join(data, 'package_esp32_index.json'), 'utf8')),
};
const target = process.argv.includes('--target')
  ? process.argv[process.argv.indexOf('--target') + 1]
  : process.platform === 'darwin' && process.arch === 'arm64'
    ? 'darwin-arm64'
    : process.platform === 'linux' && process.arch === 'x64'
      ? 'linux-x64'
      : null;
if (!target || !['darwin-arm64', 'linux-x64'].includes(target)) throw new Error(`unsupported or missing target ${target}`);
if (lock.schema !== 1) throw new Error('unsupported toolchain lock schema');
for (const field of ['installerCommit', 'sdkReleaseCommit']) {
  if (!/^[0-9a-f]{40}$/.test(lock.hostTools.emscripten?.[field] ?? '')) {
    throw new Error(`invalid locked Emscripten ${field}`);
  }
}
const nvmrc = readFileSync(join(root, '.nvmrc'), 'utf8').trim();
if (nvmrc !== lock.node.version) throw new Error(`.nvmrc ${nvmrc} does not match lock ${lock.node.version}`);
const digest = (path) => createHash('sha256').update(readFileSync(path)).digest('hex');
for (const nodeTarget of ['darwin-arm64', 'linux-x64']) {
  const archive = lock.node?.archives?.[nodeTarget];
  if (
    typeof archive?.url !== 'string' ||
    !archive.url.startsWith(`https://nodejs.org/dist/v${lock.node.version}/`) ||
    typeof archive?.file !== 'string' ||
    archive.file.includes('/') ||
    typeof archive?.member !== 'string' ||
    archive.member.startsWith('/') ||
    archive.member.includes('\\') ||
    archive.member.split('/').includes('..') ||
    !/^[0-9a-f]{64}$/.test(archive?.sha256 ?? '')
  ) {
    throw new Error(`node.archives.${nodeTarget} is invalid`);
  }
}
const requirements = lock.pythonReleaseDependencies?.requirements;
if (
  typeof requirements !== 'string' ||
  requirements.startsWith('/') ||
  requirements.includes('\\') ||
  requirements.split('/').includes('..')
) {
  throw new Error('pythonReleaseDependencies.requirements must be a safe repository-relative path');
}
const requirementsPath = join(root, requirements);
if (digest(requirementsPath) !== lock.pythonReleaseDependencies.sha256) {
  throw new Error(`${requirements} does not match pythonReleaseDependencies.sha256`);
}
for (const toolName of ['actionlint', 'syft', 'cosign', 'githubCli']) {
  const tool = lock.hostTools?.[toolName];
  const archive = tool?.archives?.['linux-x64'];
  if (
    typeof tool?.version !== 'string' ||
    !/^[0-9]+\.[0-9]+\.[0-9]+$/.test(tool.version) ||
    typeof archive?.url !== 'string' ||
    !archive.url.startsWith('https://github.com/') ||
    typeof archive?.file !== 'string' ||
    archive.file.includes('/') ||
    typeof archive?.member !== 'string' ||
    archive.member.startsWith('/') ||
    archive.member.includes('\\') ||
    archive.member.split('/').includes('..') ||
    !/^[0-9a-f]{64}$/.test(archive?.sha256 ?? '')
  ) {
    throw new Error(`hostTools.${toolName} has an invalid linux-x64 archive lock`);
  }
}
const checksum = (value) => value?.replace(/^SHA-256:/, '');
const compareArchive = (locked, indexed, context) => {
  const actual = {
    url: indexed.url,
    file: indexed.archiveFileName,
    size: Number(indexed.size),
    sha256: checksum(indexed.checksum),
  };
  for (const key of ['url', 'file', 'size', 'sha256']) {
    if (locked[key] !== actual[key]) throw new Error(`${context} ${key} changed: locked ${locked[key]}, index ${actual[key]}`);
  }
};

const esp32Package = indexes.esp32.packages.find((entry) => entry.name === 'esp32');
const arduinoPackage = indexes.arduino.packages.find((entry) => entry.name === 'arduino');
const core = esp32Package.platforms.find((entry) => entry.version === lock.arduino.core.version);
if (!core) throw new Error(`index is missing ESP32 core ${lock.arduino.core.version}`);
compareArchive(lock.arduino.core, core, 'ESP32 core');
for (const lockedTool of lock.arduino.coreTools) {
  const packageEntry = lockedTool.packager === 'esp32' ? esp32Package : arduinoPackage;
  const tool = packageEntry.tools.find((entry) => entry.name === lockedTool.name && entry.version === lockedTool.version);
  if (!tool) throw new Error(`index is missing ${lockedTool.packager}:${lockedTool.name}@${lockedTool.version}`);
  const lockedArchive = lockedTool.archives[target];
  const indexedArchive = tool.systems.find((entry) => entry.host === lockedArchive.host);
  if (!indexedArchive) throw new Error(`${lockedTool.name}@${lockedTool.version} lost host ${lockedArchive.host}`);
  compareArchive(lockedArchive, indexedArchive, `${lockedTool.name}@${lockedTool.version}`);
}
for (const lockedLibrary of lock.arduino.libraries) {
  const library = indexes.libraries.libraries.find((entry) =>
    entry.name === lockedLibrary.name && entry.version === lockedLibrary.version);
  if (!library) throw new Error(`index is missing ${lockedLibrary.name}@${lockedLibrary.version}`);
  compareArchive(lockedLibrary, library, `${lockedLibrary.name}@${lockedLibrary.version}`);
  const dependencies = (library.dependencies ?? []).map((entry) => entry.name).sort();
  if (JSON.stringify(dependencies) !== JSON.stringify(lockedLibrary.dependencies)) {
    throw new Error(`${lockedLibrary.name}@${lockedLibrary.version} dependencies changed`);
  }
}
validateArduinoLibraryPatches(lock, root);
if (verifyInstalled) verifyInstalledArduinoLibraryPatches(lock, root);

// Arduino CLI verifies downloads against the index. If an archive remains in its
// cache, independently verify it against the reviewed lock as well.
const downloads = configuredDirectory('ARDUINO_DIRECTORIES_DOWNLOADS', 'downloads');
const cached = new Map();
const collect = (directory) => {
  if (!existsSync(directory)) return;
  for (const entry of readdirSync(directory, { withFileTypes: true })) {
    const path = join(directory, entry.name);
    if (entry.isDirectory()) collect(path);
    else if (entry.isFile()) cached.set(entry.name, path);
  }
};
collect(downloads);
const archives = [
  lock.arduino.core,
  ...lock.arduino.libraries,
  ...lock.arduino.coreTools.map((entry) => entry.archives[target]),
];
for (const entry of archives) {
  const path = cached.get(entry.file);
  if (path && digest(path) !== entry.sha256) throw new Error(`cached ${entry.file} does not match the toolchain lock`);
}

// When the optional local simulator SDK is present, bind both the installer and
// the version-to-release mapping to the reviewed commits in the lock. This
// catches an updated emsdk checkout silently resolving 6.0.2 differently.
const emsdk = join(root, '.tools', 'emsdk');
if (existsSync(join(emsdk, '.git'))) {
  const head = readFileSync(join(emsdk, '.git', 'HEAD'), 'utf8').trim();
  let installerCommit = head;
  if (head.startsWith('ref: ')) {
    installerCommit = readFileSync(join(emsdk, '.git', head.slice(5)), 'utf8').trim();
  }
  if (installerCommit !== lock.hostTools.emscripten.installerCommit) {
    throw new Error(`emsdk checkout ${installerCommit} does not match the toolchain lock`);
  }
  const releases = JSON.parse(readFileSync(join(emsdk, 'emscripten-releases-tags.json'), 'utf8'));
  const release = releases.releases?.[lock.hostTools.emscripten.version];
  if (release !== lock.hostTools.emscripten.sdkReleaseCommit) {
    throw new Error(`Emscripten ${lock.hostTools.emscripten.version} resolves to ${release}, not the lock`);
  }
}

console.log(
  `toolchain lock verified for ${target}: ${archives.length} archive checksums${verifyInstalled ? ', patched libraries installed' : ''}`,
);
