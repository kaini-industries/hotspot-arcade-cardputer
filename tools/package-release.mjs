#!/usr/bin/env node

// Produce deterministic release checksums and a machine-readable build manifest
// from already-built firmware. This script does not compile or publish anything.

import { createHash } from 'node:crypto';
import { existsSync, mkdtempSync, readFileSync, renameSync, rmSync, statSync, writeFileSync } from 'node:fs';
import { basename, dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import {
  fileDigest,
  readCleanGitSource,
  readToolchainLock,
  readUpstreamLock,
  sourceDateEpoch,
  verifyFinalTag,
} from './release-provenance.mjs';
import { CANONICAL_REPOSITORY, RELEASE_ARTIFACTS, validateRelease } from './validate-release.mjs';
import { validateSbomDocument } from './validate-sbom.mjs';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');

function sha256(path) {
  return createHash('sha256').update(readFileSync(path)).digest('hex');
}

function parseArgs(argv) {
  const options = { artifactsDir: 'build', tag: '', candidate: false };
  for (let i = 0; i < argv.length; i++) {
    if (argv[i] === '--artifacts-dir') options.artifactsDir = argv[++i] ?? '';
    else if (argv[i] === '--tag') {
      options.tag = argv[++i] ?? '';
      if (!options.tag) throw new Error('--tag requires a value');
    }
    else if (argv[i] === '--candidate') options.candidate = true;
    else throw new Error(`unknown argument: ${argv[i]}`);
  }
  if (!options.artifactsDir) throw new Error('--artifacts-dir cannot be empty');
  if (!options.tag) throw new Error('--tag is required');
  return options;
}

function main() {
  try {
    const options = parseArgs(process.argv.slice(2));
    const release = validateRelease({
      tag: options.tag,
      artifactsDir: options.artifactsDir,
      requireArtifacts: true,
      candidate: options.candidate,
    });
    const source = readCleanGitSource(root);
    if (!release.candidate) verifyFinalTag(root, release.tag, source.commit);
    const artifactsDir = resolve(root, options.artifactsDir);
    const upstream = readUpstreamLock(root);
    const toolchain = readToolchainLock(root);
    const epoch = sourceDateEpoch();
    const sbomPath = join(artifactsDir, 'hotspot-arcade-cardputer.spdx.json');
    validateSbomDocument(JSON.parse(readFileSync(sbomPath, 'utf8')), artifactsDir, root);
    const artifacts = RELEASE_ARTIFACTS.map((filename) => {
      const path = join(artifactsDir, filename);
      return { filename, bytes: statSync(path).size, sha256: sha256(path) };
    }).sort((a, b) => (a.filename < b.filename ? -1 : a.filename > b.filename ? 1 : 0));

    const manifest = {
      schemaVersion: 1,
      version: release.version,
      tag: release.tag,
      repository: CANONICAL_REPOSITORY,
      commit: source.commit,
      sourceTreeClean: source.sourceTreeClean,
      candidate: release.candidate,
      upstream: {
        repository: upstream.repository,
        commit: upstream.commit,
        describe: upstream.describe,
        sourceTreeSha256: upstream.sourceTreeSha256,
        lockFile: 'UPSTREAM.lock.json',
      },
      sourceDateEpoch: epoch,
      fqbn: toolchain.arduino.fqbn,
      singleImage: true,
      compatibleDevices: [
        {
          manufacturer: 'M5Stack',
          model: 'Cardputer',
          board: 'M5Cardputer',
          boardId: 14,
        },
        {
          manufacturer: 'M5Stack',
          model: 'Cardputer-Adv',
          board: 'M5CardputerADV',
          boardId: 24,
        },
      ],
      toolchain: {
        lockFile: 'tools/toolchain.lock.json',
        lockSha256: fileDigest(join(root, 'tools', 'toolchain.lock.json')),
        node: toolchain.node.version,
        arduinoCli: toolchain.arduino.cliVersion,
        esp32Core: toolchain.arduino.core.version,
        esptool: toolchain.hostTools.esptool.version,
      },
      installLayouts: {
        app: {
          filename: 'hotspot-arcade-cardputer.ino.bin',
          target: 'M5Launcher',
          flashOffset: '0x170000',
        },
        full: {
          filename: 'hotspot-arcade-cardputer.full.bin',
          target: 'ESP32-S3 flash',
          flashOffset: '0x0',
          flashSizeBytes: 0x800000,
        },
        m5burner: {
          filename: 'hotspot-arcade-cardputer-m5burner.zip',
          components: [
            { filename: 'bootloader_0x0.bin', flashOffset: '0x0' },
            { filename: 'partitions_0x8000.bin', flashOffset: '0x8000' },
            { filename: 'boot_app0_0xe000.bin', flashOffset: '0xe000' },
            { filename: 'hotspot-arcade_0x10000.bin', flashOffset: '0x10000' },
          ],
        },
      },
      artifacts,
    };

    const manifestText = `${JSON.stringify(manifest, null, 2)}\n`;
    const staged = mkdtempSync(join(artifactsDir, '.release-metadata-'));
    const stagedManifest = join(staged, 'build-manifest.json');
    const stagedChecksums = join(staged, 'SHA256SUMS');
    let checksummed;
    try {
      writeFileSync(stagedManifest, manifestText);
      checksummed = [
        ...artifacts,
        {
          filename: basename(stagedManifest),
          bytes: statSync(stagedManifest).size,
          sha256: sha256(stagedManifest),
        },
      ].sort((a, b) => (a.filename < b.filename ? -1 : a.filename > b.filename ? 1 : 0));
      const checksumText = checksummed.map((item) => `${item.sha256}  ${item.filename}`).join('\n') + '\n';
      writeFileSync(stagedChecksums, checksumText);

      const targets = [
        [stagedManifest, join(artifactsDir, 'build-manifest.json'), join(staged, 'build-manifest.previous')],
        [stagedChecksums, join(artifactsDir, 'SHA256SUMS'), join(staged, 'SHA256SUMS.previous')],
      ];
      const backedUp = [];
      const installed = [];
      try {
        for (const [, target, backup] of targets) {
          if (existsSync(target)) {
            renameSync(target, backup);
            backedUp.push([backup, target]);
          }
        }
        for (const [source, target] of targets) {
          renameSync(source, target);
          installed.push(target);
        }
      } catch (error) {
        for (const target of installed) if (existsSync(target)) rmSync(target, { force: true });
        for (const [backup, target] of backedUp) if (existsSync(backup)) renameSync(backup, target);
        throw error;
      }
    } finally {
      rmSync(staged, { recursive: true, force: true });
    }

    console.log(`packaged release metadata for ${release.tag}`);
    for (const artifact of checksummed) {
      console.log(`  ${artifact.sha256}  ${artifact.filename} (${artifact.bytes} bytes)`);
    }
  } catch (error) {
    console.error(`release packaging failed: ${error.message}`);
    process.exitCode = 1;
  }
}

main();
