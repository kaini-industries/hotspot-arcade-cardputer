#!/usr/bin/env node
// Emit a deterministic SPDX 2.3 JSON SBOM for the release images and every
// locked source/library/toolchain package used to compose those images.

import { createHash } from 'node:crypto';
import { mkdtempSync, readFileSync, renameSync, rmSync, statSync, writeFileSync } from 'node:fs';
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
import { validateSbomDocument } from './validate-sbom.mjs';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const build = resolve(root, process.argv[2] ?? 'build');
const version = readFileSync(join(root, 'VERSION'), 'utf8').trim();
const releaseFiles = [
  'hotspot-arcade-cardputer.ino.bin',
  'hotspot-arcade-cardputer.full.bin',
  'hotspot-arcade-cardputer-m5burner.zip',
];
for (const name of releaseFiles) {
  const path = join(build, name);
  if (!statSync(path).isFile() || statSync(path).size === 0) throw new Error(`missing SBOM input ${path}`);
}

const upstream = readUpstreamLock(root);
const toolchain = readToolchainLock(root);
const epoch = sourceDateEpoch();
const created = new Date(epoch * 1000).toISOString();
const rootId = 'SPDXRef-Package-HotspotArcadeCardputer';
const files = releaseFiles.map((name, index) => ({
  fileName: `./${name}`,
  SPDXID: `SPDXRef-File-${index + 1}`,
  checksums: [
    { algorithm: 'SHA1', checksumValue: fileDigest(join(build, name), 'sha1') },
    { algorithm: 'SHA256', checksumValue: fileDigest(join(build, name)) },
  ],
  fileTypes: ['BINARY'],
  licenseConcluded: 'NOASSERTION',
  copyrightText: 'NOASSERTION',
}));
const verificationCode = digest(
  'sha1',
  files.map((file) => file.checksums[0].checksumValue).sort().join(''),
);

let packageNumber = 0;
const packageId = () => `SPDXRef-Package-Dependency-${++packageNumber}`;
const lockedPackage = ({ name, versionInfo, downloadLocation, sha256, purpose = 'OTHER', license = 'NOASSERTION', supplier = 'NOASSERTION' }) => ({
  name,
  SPDXID: packageId(),
  versionInfo,
  downloadLocation,
  filesAnalyzed: false,
  licenseConcluded: license,
  licenseDeclared: license,
  copyrightText: 'NOASSERTION',
  supplier,
  primaryPackagePurpose: purpose,
  checksums: [{ algorithm: 'SHA256', checksumValue: sha256 }],
});

const subtreePackage = (name, localPrefix, upstreamPath, license) => {
  const entries = upstream.files.filter((file) => file.path.startsWith(localPrefix));
  if (entries.length === 0) throw new Error(`UPSTREAM.lock.json has no inventory for ${name}`);
  const material = entries.map((file) => `${file.sha256}  ${file.size}  ${file.path}\n`).join('');
  return lockedPackage({
    name,
    versionInfo: upstream.commit,
    downloadLocation: `${upstream.repository}/tree/${upstream.commit}/${upstreamPath}`,
    sha256: createHash('sha256').update(material).digest('hex'),
    purpose: 'LIBRARY',
    license,
  });
};

const linux = 'linux-x64';
const dependencies = [
  lockedPackage({
    name: 'hotspot-arcade',
    versionInfo: upstream.commit,
    downloadLocation: upstream.repository,
    sha256: upstream.sourceTreeSha256,
    purpose: 'SOURCE',
    license: 'MIT',
    supplier: 'Person: Tarik Caramanico',
  }),
  subtreePackage('AsyncTCP', 'vendor/libs/AsyncTCP/', 'esp32/libs/AsyncTCP', 'LGPL-3.0-only'),
  subtreePackage('ESPAsyncWebServer', 'vendor/libs/ESPAsyncWebServer/', 'esp32/libs/ESPAsyncWebServer', 'LGPL-3.0-only'),
  lockedPackage({
    name: 'arduino-cli',
    versionInfo: toolchain.hostTools.arduinoCli.version,
    downloadLocation: toolchain.hostTools.arduinoCli.archives[linux].url,
    sha256: toolchain.hostTools.arduinoCli.archives[linux].sha256,
  }),
  lockedPackage({
    name: 'arduino-esp32',
    versionInfo: toolchain.arduino.core.version,
    downloadLocation: toolchain.arduino.core.url,
    sha256: toolchain.arduino.core.sha256,
    purpose: 'LIBRARY',
    supplier: 'Organization: Espressif Systems',
  }),
  ...toolchain.arduino.coreTools.map((item) => lockedPackage({
    name: item.name,
    versionInfo: item.version,
    downloadLocation: item.archives[linux].url,
    sha256: item.archives[linux].sha256,
  })),
  ...toolchain.arduino.libraries.map((item) => lockedPackage({
    name: item.name,
    versionInfo: item.version,
    downloadLocation: item.url,
    sha256: item.sha256,
    purpose: 'LIBRARY',
  })),
];

const namespaceMaterial = [
  ...files.map((file) => `${file.fileName}:${file.checksums[1].checksumValue}`),
  `upstream:${upstream.sourceTreeSha256}`,
  `toolchain:${fileDigest(join(root, 'tools', 'toolchain.lock.json'))}`,
  `epoch:${epoch}`,
].join('\n');
const namespaceDigest = digest('sha256', namespaceMaterial);
const document = {
  spdxVersion: 'SPDX-2.3',
  dataLicense: 'CC0-1.0',
  SPDXID: 'SPDXRef-DOCUMENT',
  name: `hotspot-arcade-cardputer-${version}`,
  documentNamespace: `${CANONICAL_REPOSITORY}/spdx/${version}/${namespaceDigest}`,
  creationInfo: {
    created,
    creators: ['Organization: Kaini Industries', 'Tool: hotspot-arcade-cardputer/generate-sbom.mjs'],
  },
  documentDescribes: [rootId],
  packages: [
    {
      name: 'hotspot-arcade-cardputer',
      SPDXID: rootId,
      versionInfo: version,
      downloadLocation: CANONICAL_REPOSITORY,
      filesAnalyzed: true,
      packageVerificationCode: { packageVerificationCodeValue: verificationCode },
      licenseConcluded: 'MIT',
      licenseDeclared: 'MIT',
      copyrightText: 'NOASSERTION',
      supplier: 'Organization: Kaini Industries',
      primaryPackagePurpose: 'FIRMWARE',
      externalRefs: [{
        referenceCategory: 'PACKAGE-MANAGER',
        referenceType: 'purl',
        referenceLocator: `pkg:github/kaini-industries/hotspot-arcade-cardputer@${version}`,
      }],
    },
    ...dependencies,
  ],
  files,
  relationships: [
    { spdxElementId: 'SPDXRef-DOCUMENT', relationshipType: 'DESCRIBES', relatedSpdxElement: rootId },
    ...dependencies.map((dependency) => ({
      spdxElementId: rootId,
      relationshipType: 'DEPENDS_ON',
      relatedSpdxElement: dependency.SPDXID,
    })),
    ...files.map((file) => ({
      spdxElementId: rootId,
      relationshipType: 'CONTAINS',
      relatedSpdxElement: file.SPDXID,
    })),
  ],
};

validateSbomDocument(document, build, root);
const staging = mkdtempSync(join(build, '.sbom-'));
const staged = join(staging, 'hotspot-arcade-cardputer.spdx.json');
const output = join(build, basename(staged));
try {
  writeFileSync(staged, `${JSON.stringify(document, null, 2)}\n`, { encoding: 'utf8', mode: 0o644 });
  renameSync(staged, output);
} finally {
  rmSync(staging, { recursive: true, force: true });
}
console.log(`wrote ${basename(output)} with ${document.packages.length} packages and ${files.length} release files`);
