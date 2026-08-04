#!/usr/bin/env node
// Emit a deterministic SPDX 2.3 JSON SBOM for the release images and the
// source/toolchain packages that materially compose them.

import { createHash } from 'node:crypto';
import { execFileSync } from 'node:child_process';
import { existsSync, readFileSync, statSync, writeFileSync } from 'node:fs';
import { basename, dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const build = resolve(root, process.argv[2] ?? 'build');
const version = readFileSync(join(root, 'VERSION'), 'utf8').trim();
const repository = 'https://github.com/kaini-industries/hotspot-arcade-cardputer';
const releaseFiles = [
  'hotspot-arcade-cardputer.ino.bin',
  'hotspot-arcade-cardputer.full.bin',
  'hotspot-arcade-cardputer-m5burner.zip',
];
const sha256 = (path) => createHash('sha256').update(readFileSync(path)).digest('hex');
for (const name of releaseFiles) {
  const path = join(build, name);
  if (!existsSync(path) || !statSync(path).isFile() || statSync(path).size === 0) throw new Error(`missing SBOM input ${path}`);
}
const upstreamText = readFileSync(join(root, 'UPSTREAM.md'), 'utf8');
const upstreamCommit = upstreamText.match(/\| commit \| `([0-9a-f]{40})` \|/)?.[1];
if (!upstreamCommit) throw new Error('UPSTREAM.md is missing its pinned commit');
const epochText = process.env.SOURCE_DATE_EPOCH || execFileSync('git', ['-C', root, 'show', '-s', '--format=%ct', 'HEAD'], { encoding: 'utf8' }).trim();
if (!/^[0-9]+$/.test(epochText)) throw new Error('SOURCE_DATE_EPOCH must be integer seconds');
const created = new Date(Number(epochText) * 1000).toISOString();
const material = releaseFiles.map((name) => `${name}:${sha256(join(build, name))}`).join('\n') + `\n${upstreamCommit}`;
const namespaceDigest = createHash('sha256').update(material).digest('hex');
const rootId = 'SPDXRef-Package-HotspotArcadeCardputer';
const upstreamId = 'SPDXRef-Package-HotspotArcade';
const asyncTcpId = 'SPDXRef-Package-AsyncTCP';
const asyncWebId = 'SPDXRef-Package-ESPAsyncWebServer';
const toolchain = JSON.parse(readFileSync(join(root, 'tools', 'toolchain.lock.json'), 'utf8'));
const files = releaseFiles.map((name, index) => ({
  fileName: `./${name}`,
  SPDXID: `SPDXRef-File-${index + 1}`,
  checksums: [{ algorithm: 'SHA256', checksumValue: sha256(join(build, name)) }],
  fileTypes: ['BINARY'],
}));
const document = {
  spdxVersion: 'SPDX-2.3',
  dataLicense: 'CC0-1.0',
  SPDXID: 'SPDXRef-DOCUMENT',
  name: `hotspot-arcade-cardputer-${version}`,
  documentNamespace: `${repository}/spdx/${version}/${namespaceDigest}`,
  creationInfo: {
    created,
    creators: ['Organization: Kaini Industries', 'Tool: hotspot-arcade-cardputer/generate-sbom.mjs'],
    licenseListVersion: '3.26',
  },
  documentDescribes: [rootId],
  packages: [
    {
      name: 'hotspot-arcade-cardputer',
      SPDXID: rootId,
      versionInfo: version,
      downloadLocation: repository,
      filesAnalyzed: true,
      licenseConcluded: 'MIT',
      licenseDeclared: 'MIT',
      supplier: 'Organization: Kaini Industries',
      checksums: [{ algorithm: 'SHA256', checksumValue: namespaceDigest }],
      externalRefs: [{
        referenceCategory: 'PACKAGE-MANAGER',
        referenceType: 'purl',
        referenceLocator: `pkg:github/kaini-industries/hotspot-arcade-cardputer@${version}`,
      }],
    },
    {
      name: 'hotspot-arcade',
      SPDXID: upstreamId,
      versionInfo: upstreamCommit,
      downloadLocation: 'https://github.com/tarikbc/hotspot-arcade',
      filesAnalyzed: false,
      licenseConcluded: 'MIT',
      licenseDeclared: 'MIT',
      supplier: 'Person: Tarik Caramanico',
    },
    {
      name: 'AsyncTCP',
      SPDXID: asyncTcpId,
      versionInfo: 'vendored-upstream',
      downloadLocation: 'NOASSERTION',
      filesAnalyzed: false,
      licenseConcluded: 'LGPL-3.0-only',
      licenseDeclared: 'LGPL-3.0-only',
    },
    {
      name: 'ESPAsyncWebServer',
      SPDXID: asyncWebId,
      versionInfo: 'vendored-upstream',
      downloadLocation: 'NOASSERTION',
      filesAnalyzed: false,
      licenseConcluded: 'LGPL-3.0-only',
      licenseDeclared: 'LGPL-3.0-only',
    },
    ...toolchain.arduino.libraries.map((library, index) => ({
      name: library.name,
      SPDXID: `SPDXRef-Package-ArduinoLibrary-${index + 1}`,
      versionInfo: library.version,
      downloadLocation: library.url,
      filesAnalyzed: false,
      licenseConcluded: 'NOASSERTION',
      licenseDeclared: 'NOASSERTION',
      checksums: [{ algorithm: 'SHA256', checksumValue: library.sha256 }],
    })),
  ],
  files,
  relationships: [
    { spdxElementId: 'SPDXRef-DOCUMENT', relationshipType: 'DESCRIBES', relatedSpdxElement: rootId },
    ...[upstreamId, asyncTcpId, asyncWebId].map((dependency) => ({
      spdxElementId: rootId,
      relationshipType: 'DEPENDS_ON',
      relatedSpdxElement: dependency,
    })),
    ...toolchain.arduino.libraries.map((_, index) => ({
      spdxElementId: rootId,
      relationshipType: 'DEPENDS_ON',
      relatedSpdxElement: `SPDXRef-Package-ArduinoLibrary-${index + 1}`,
    })),
    ...files.map((file) => ({
      spdxElementId: rootId,
      relationshipType: 'CONTAINS',
      relatedSpdxElement: file.SPDXID,
    })),
  ],
};

const output = join(build, 'hotspot-arcade-cardputer.spdx.json');
writeFileSync(output, `${JSON.stringify(document, null, 2)}\n`, 'utf8');
console.log(`wrote ${basename(output)} with ${document.packages.length} packages and ${files.length} release files`);
