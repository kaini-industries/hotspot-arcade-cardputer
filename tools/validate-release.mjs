#!/usr/bin/env node

// Validate the single-source release version, ownership, notes, and optional
// artifacts before any release system is allowed to mutate external state.

import { existsSync, readFileSync, statSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

export const CANONICAL_REPOSITORY = 'https://github.com/kaini-industries/hotspot-arcade-cardputer';
export const CANONICAL_AUTHOR = 'kaini-industries';
export const CANONICAL_REPOSITORY_SLUG = 'kaini-industries/hotspot-arcade-cardputer';
export const RELEASE_ARTIFACTS = [
  'hotspot-arcade-cardputer.ino.bin',
  'hotspot-arcade-cardputer.full.bin',
  'hotspot-arcade-cardputer-m5burner.zip',
  'hotspot-arcade-cardputer.spdx.json',
];

const SEMVER_RE = /^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-[0-9A-Za-z.-]+)?$/;
const root = join(dirname(fileURLToPath(import.meta.url)), '..');

function parseArgs(argv) {
  const options = { tag: '', repositorySlug: '', artifactsDir: '', requireArtifacts: false };
  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    if (arg === '--tag') {
      options.tag = argv[++i] ?? '';
      if (!options.tag) throw new Error('--tag requires a value');
    } else if (arg === '--artifacts-dir') {
      options.artifactsDir = argv[++i] ?? '';
      if (!options.artifactsDir) throw new Error('--artifacts-dir requires a value');
    } else if (arg === '--repository') {
      options.repositorySlug = argv[++i] ?? '';
      if (!options.repositorySlug) throw new Error('--repository requires a value');
    } else if (arg === '--require-artifacts') options.requireArtifacts = true;
    else throw new Error(`unknown argument: ${arg}`);
  }
  return options;
}

function readVersion(repoRoot) {
  const version = readFileSync(join(repoRoot, 'VERSION'), 'utf8').trim();
  if (!SEMVER_RE.test(version)) throw new Error(`VERSION is not valid semantic version syntax: ${version}`);
  return version;
}

function releaseNotesVersion(repoRoot) {
  const notes = readFileSync(join(repoRoot, 'docs', 'RELEASE_NOTES.md'), 'utf8');
  const match = notes.match(
    /^###\s+v((?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)(?:-[0-9A-Za-z.-]+)?)(?:\s|$)/m,
  );
  if (!match) throw new Error('docs/RELEASE_NOTES.md has no "### v<version>" heading');
  return match[1];
}

export function validateRelease(
  { repoRoot = root, tag = '', repositorySlug = '', artifactsDir = '', requireArtifacts = false } = {},
) {
  const errors = [];
  let version = '';
  let manifest = {};
  try {
    version = readVersion(repoRoot);
  } catch (error) {
    errors.push(error.message);
  }

  try {
    manifest = JSON.parse(readFileSync(join(repoRoot, 'm5burner.json'), 'utf8'));
  } catch (error) {
    errors.push(`cannot parse m5burner.json: ${error.message}`);
  }

  if (version) {
    if (manifest.version !== version) {
      errors.push(`m5burner.json version ${manifest.version ?? '(missing)'} does not match VERSION ${version}`);
    }
    try {
      const notesVersion = releaseNotesVersion(repoRoot);
      if (notesVersion !== version) {
        errors.push(`release notes version ${notesVersion} does not match VERSION ${version}`);
      }
    } catch (error) {
      errors.push(error.message);
    }
    if (tag && tag !== `v${version}`) errors.push(`tag ${tag} does not match VERSION v${version}`);
  }
  if (repositorySlug && repositorySlug !== CANONICAL_REPOSITORY_SLUG) {
    errors.push(`release repository ${repositorySlug} is not canonical ${CANONICAL_REPOSITORY_SLUG}`);
  }

  if (manifest.repository !== CANONICAL_REPOSITORY) {
    errors.push(
      `m5burner.json repository must be ${CANONICAL_REPOSITORY}; got ${manifest.repository ?? '(missing)'}`,
    );
  }
  if (manifest.author !== CANONICAL_AUTHOR) {
    errors.push(`m5burner.json author must be ${CANONICAL_AUTHOR}; got ${manifest.author ?? '(missing)'}`);
  }

  const readme = readFileSync(join(repoRoot, 'README.md'), 'utf8');
  if (!readme.includes('github.com/kaini-industries/hotspot-arcade-cardputer')) {
    errors.push('README.md does not point at the canonical Kaini Industries repository');
  }
  if (!readme.includes('tarikbc/hotspot-arcade')) {
    errors.push('README.md is missing upstream tarikbc/hotspot-arcade attribution');
  }

  if (requireArtifacts && !artifactsDir) errors.push('--require-artifacts requires --artifacts-dir');
  if (artifactsDir) {
    const directory = resolve(repoRoot, artifactsDir);
    for (const filename of RELEASE_ARTIFACTS) {
      const path = join(directory, filename);
      if (!existsSync(path)) errors.push(`release artifact is missing: ${path}`);
      else if (!statSync(path).isFile() || statSync(path).size === 0) {
        errors.push(`release artifact is not a non-empty file: ${path}`);
      }
    }
  }

  if (errors.length) throw new Error(errors.join('\n'));
  return { version, tag: tag || `v${version}`, repository: CANONICAL_REPOSITORY };
}

function main() {
  try {
    const options = parseArgs(process.argv.slice(2));
    const result = validateRelease(options);
    console.log(`release validation passed: ${result.tag} (${result.repository})`);
  } catch (error) {
    console.error(`release validation failed:\n${error.message}`);
    process.exitCode = 1;
  }
}

if (resolve(process.argv[1] ?? '') === fileURLToPath(import.meta.url)) main();
