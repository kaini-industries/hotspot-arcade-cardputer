#!/usr/bin/env node

import { readFileSync, statSync } from 'node:fs';
import { basename, dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { CANONICAL_REPOSITORY } from './validate-release.mjs';
import {
  digest,
  fileDigest,
  readToolchainLock,
  readUpstreamLock,
  sourceDateEpoch,
} from './release-provenance.mjs';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const ROOT_PACKAGE = 'SPDXRef-Package-HotspotArcadeCardputer';
const RELEASE_FILES = [
  'hotspot-arcade-cardputer.ino.bin',
  'hotspot-arcade-cardputer.full.bin',
  'hotspot-arcade-cardputer-m5burner.zip',
];

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

function checksum(entry, algorithm) {
  return entry.checksums?.find((item) => item.algorithm === algorithm)?.checksumValue;
}

function verificationCode(files) {
  const sha1s = files.map((file) => checksum(file, 'SHA1')).sort();
  return digest('sha1', sha1s.join(''));
}

export function validateSbomDocument(document, buildDirectory, repoRoot = root) {
  const upstream = readUpstreamLock(repoRoot);
  const toolchain = readToolchainLock(repoRoot);
  const epoch = sourceDateEpoch();
  assert(document?.spdxVersion === 'SPDX-2.3', 'SBOM must use SPDX-2.3');
  assert(document.dataLicense === 'CC0-1.0', 'SBOM dataLicense must be CC0-1.0');
  assert(document.SPDXID === 'SPDXRef-DOCUMENT', 'SBOM document SPDXID is invalid');
  assert(
    typeof document.documentNamespace === 'string' && document.documentNamespace.startsWith(`${CANONICAL_REPOSITORY}/spdx/`),
    'SBOM namespace must use the canonical Kaini repository',
  );
  assert(
    document.creationInfo?.created === new Date(epoch * 1000).toISOString(),
    'SBOM creation time must equal SOURCE_DATE_EPOCH',
  );
  assert(document.documentDescribes?.length === 1 && document.documentDescribes[0] === ROOT_PACKAGE, 'SBOM must describe one root package');
  assert(Array.isArray(document.packages) && document.packages.length > 1, 'SBOM packages are missing');
  assert(Array.isArray(document.files) && document.files.length === RELEASE_FILES.length, 'SBOM release files are incomplete');
  assert(Array.isArray(document.relationships), 'SBOM relationships are missing');

  const allIds = [document.SPDXID, ...document.packages.map((item) => item.SPDXID), ...document.files.map((item) => item.SPDXID)];
  assert(allIds.every((id) => /^SPDXRef-[A-Za-z0-9.-]+$/.test(id ?? '')), 'SBOM contains an invalid SPDXID');
  assert(new Set(allIds).size === allIds.length, 'SBOM SPDXIDs must be unique');
  const packagesById = new Map(document.packages.map((item) => [item.SPDXID, item]));
  const packagePurposes = new Set([
    'APPLICATION', 'FRAMEWORK', 'LIBRARY', 'CONTAINER', 'OPERATING-SYSTEM', 'DEVICE',
    'FIRMWARE', 'SOURCE', 'ARCHIVE', 'FILE', 'INSTALL', 'OTHER',
  ]);
  for (const item of document.packages) {
    assert(typeof item.name === 'string' && item.name, `SBOM package name is missing for ${item.SPDXID}`);
    assert(packagePurposes.has(item.primaryPackagePurpose), `invalid SPDX package purpose for ${item.name}`);
    assert(typeof item.filesAnalyzed === 'boolean', `filesAnalyzed must be explicit for ${item.name}`);
  }
  const rootPackage = packagesById.get(ROOT_PACKAGE);
  assert(rootPackage?.name === 'hotspot-arcade-cardputer', 'SBOM root package is missing');
  assert(rootPackage.supplier === 'Organization: Kaini Industries', 'SBOM root supplier must be Kaini Industries');
  assert(rootPackage.downloadLocation === CANONICAL_REPOSITORY, 'SBOM root download location is not canonical');
  assert(rootPackage.filesAnalyzed === true, 'SBOM root package must analyze release files');

  const actualFileNames = document.files.map((file) => file.fileName);
  assert(
    JSON.stringify(actualFileNames) === JSON.stringify(RELEASE_FILES.map((name) => `./${name}`)),
    'SBOM must enumerate the exact release files in stable order',
  );
  for (const file of document.files) {
    const path = join(buildDirectory, basename(file.fileName));
    assert(statSync(path).isFile() && statSync(path).size > 0, `SBOM file is missing: ${path}`);
    assert(checksum(file, 'SHA1') === fileDigest(path, 'sha1'), `SBOM SHA1 mismatch for ${file.fileName}`);
    assert(checksum(file, 'SHA256') === fileDigest(path), `SBOM SHA256 mismatch for ${file.fileName}`);
    assert(file.licenseConcluded === 'NOASSERTION', `SBOM file license must be explicit for ${file.fileName}`);
    assert(file.copyrightText === 'NOASSERTION', `SBOM file copyright must be explicit for ${file.fileName}`);
  }
  assert(
    rootPackage.packageVerificationCode?.packageVerificationCodeValue === verificationCode(document.files),
    'SBOM package verification code is invalid',
  );

  const dependencyPackages = document.packages.filter((item) => item.SPDXID !== ROOT_PACKAGE);
  for (const dependency of dependencyPackages) {
    assert(dependency.filesAnalyzed === false, `dependency package must set filesAnalyzed=false: ${dependency.name}`);
    assert(!dependency.packageVerificationCode, `dependency package cannot have a verification code: ${dependency.name}`);
    assert(dependency.downloadLocation && dependency.downloadLocation !== 'NOASSERTION', `dependency download location is missing: ${dependency.name}`);
    assert(/^[0-9a-f]{64}$/.test(checksum(dependency, 'SHA256') ?? ''), `dependency SHA256 is missing: ${dependency.name}`);
  }

  const expectedDependencies = [
    ['hotspot-arcade', upstream.commit, upstream.repository, upstream.sourceTreeSha256],
    ['arduino-cli', toolchain.hostTools.arduinoCli.version, toolchain.hostTools.arduinoCli.archives['linux-x64'].url, toolchain.hostTools.arduinoCli.archives['linux-x64'].sha256],
    ['arduino-esp32', toolchain.arduino.core.version, toolchain.arduino.core.url, toolchain.arduino.core.sha256],
    ...toolchain.arduino.coreTools.map((item) => [item.name, item.version, item.archives['linux-x64'].url, item.archives['linux-x64'].sha256]),
    ...toolchain.arduino.libraries.map((item) => [item.name, item.version, item.url, item.sha256]),
  ];
  for (const [name, version, location, sha256] of expectedDependencies) {
    const matches = dependencyPackages.filter((item) => item.name === name && item.versionInfo === version);
    assert(matches.length === 1, `SBOM must contain exactly one locked dependency ${name}@${version}`);
    assert(matches[0].downloadLocation === location, `SBOM dependency URL mismatch for ${name}@${version}`);
    assert(checksum(matches[0], 'SHA256') === sha256, `SBOM dependency hash mismatch for ${name}@${version}`);
  }
  for (const name of ['AsyncTCP', 'ESPAsyncWebServer']) {
    assert(dependencyPackages.some((item) => item.name === name), `SBOM is missing vendored dependency ${name}`);
  }

  const knownIds = new Set(allIds);
  for (const relationship of document.relationships) {
    assert(knownIds.has(relationship.spdxElementId), `SBOM relationship has unknown source ${relationship.spdxElementId}`);
    assert(knownIds.has(relationship.relatedSpdxElement), `SBOM relationship has unknown target ${relationship.relatedSpdxElement}`);
  }
  const hasRelationship = (type, target) => document.relationships.some(
    (item) => item.spdxElementId === ROOT_PACKAGE && item.relationshipType === type && item.relatedSpdxElement === target,
  );
  for (const file of document.files) assert(hasRelationship('CONTAINS', file.SPDXID), `SBOM root does not contain ${file.SPDXID}`);
  for (const dependency of dependencyPackages) {
    assert(hasRelationship('DEPENDS_ON', dependency.SPDXID), `SBOM root does not depend on ${dependency.SPDXID}`);
  }
  return document;
}

function main() {
  try {
    const buildDirectory = resolve(root, process.argv[2] ?? 'build');
    const path = join(buildDirectory, 'hotspot-arcade-cardputer.spdx.json');
    const document = JSON.parse(readFileSync(path, 'utf8'));
    validateSbomDocument(document, buildDirectory, root);
    console.log(`validated complete SPDX 2.3 SBOM ${path}`);
  } catch (error) {
    console.error(`SBOM validation failed: ${error.message}`);
    process.exitCode = 1;
  }
}

if (resolve(process.argv[1] ?? '') === fileURLToPath(import.meta.url)) main();
