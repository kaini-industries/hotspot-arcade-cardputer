#!/usr/bin/env node

// Produce deterministic release checksums and a machine-readable build manifest
// from already-built firmware. This script does not compile or publish anything.

import { createHash } from 'node:crypto';
import { existsSync, mkdtempSync, readFileSync, renameSync, rmSync, statSync, writeFileSync } from 'node:fs';
import { basename, dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { execFileSync } from 'node:child_process';
import { CANONICAL_REPOSITORY, RELEASE_ARTIFACTS, validateRelease } from './validate-release.mjs';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const FQBN = 'esp32:esp32:m5stack_cardputer:FlashSize=8M,PartitionScheme=default_8MB';

function sha256(path) {
  return createHash('sha256').update(readFileSync(path)).digest('hex');
}

function upstreamCommit() {
  const upstream = readFileSync(join(root, 'UPSTREAM.md'), 'utf8');
  const match = upstream.match(/\| commit \| `([0-9a-f]{40})` \|/);
  if (!match) throw new Error('UPSTREAM.md does not contain a full 40-character commit');
  return match[1];
}

function parseArgs(argv) {
  const options = { artifactsDir: 'build', tag: '' };
  for (let i = 0; i < argv.length; i++) {
    if (argv[i] === '--artifacts-dir') options.artifactsDir = argv[++i] ?? '';
    else if (argv[i] === '--tag') {
      options.tag = argv[++i] ?? '';
      if (!options.tag) throw new Error('--tag requires a value');
    }
    else throw new Error(`unknown argument: ${argv[i]}`);
  }
  if (!options.artifactsDir) throw new Error('--artifacts-dir cannot be empty');
  return options;
}

function main() {
  try {
    const options = parseArgs(process.argv.slice(2));
    const release = validateRelease({
      tag: options.tag,
      artifactsDir: options.artifactsDir,
      requireArtifacts: true,
    });
    const artifactsDir = resolve(root, options.artifactsDir);
    const artifacts = RELEASE_ARTIFACTS.map((filename) => {
      const path = join(artifactsDir, filename);
      return { filename, bytes: statSync(path).size, sha256: sha256(path) };
    }).sort((a, b) => (a.filename < b.filename ? -1 : a.filename > b.filename ? 1 : 0));

    const manifest = {
      schemaVersion: 1,
      version: release.version,
      tag: release.tag,
      repository: CANONICAL_REPOSITORY,
      commit: execFileSync('git', ['-C', root, 'rev-parse', 'HEAD'], { encoding: 'utf8' }).trim(),
      upstream: {
        repository: 'https://github.com/tarikbc/hotspot-arcade',
        commit: upstreamCommit(),
      },
      fqbn: FQBN,
      artifacts,
    };

    const manifestText = `${JSON.stringify(manifest, null, 2)}\n`;
    const staged = mkdtempSync(join(artifactsDir, '.release-metadata-'));
    const stagedManifest = join(staged, 'build-manifest.json');
    const stagedChecksums = join(staged, 'SHA256SUMS');
    writeFileSync(stagedManifest, manifestText);
    const checksummed = [
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
