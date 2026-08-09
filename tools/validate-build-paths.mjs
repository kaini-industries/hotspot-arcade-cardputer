#!/usr/bin/env node

// Fail a build if compiler input roots leak into its executable artifacts. The
// ESP32 image descriptor records the ELF digest, so even debug-only path leakage
// changes the shipped application image.

import { readFileSync, readdirSync, statSync } from 'node:fs';
import { isAbsolute, join, resolve } from 'node:path';

function main() {
  const [artifactsArgument, ...forbiddenArguments] = process.argv.slice(2);
  if (!artifactsArgument || forbiddenArguments.length === 0) {
    throw new Error('usage: validate-build-paths.mjs <artifacts-dir> <forbidden-root>...');
  }

  const artifactsDir = resolve(artifactsArgument);
  if (!statSync(artifactsDir).isDirectory()) throw new Error('artifacts path is not a directory');
  const forbiddenRoots = [...new Set(forbiddenArguments)].map((root) => {
    if (!isAbsolute(root) || root.length < 2) throw new Error(`forbidden root must be absolute: ${root}`);
    return root;
  });
  const artifacts = readdirSync(artifactsDir, { withFileTypes: true })
    .filter((entry) => entry.isFile() && /\.(?:bin|elf)$/.test(entry.name))
    .map((entry) => join(artifactsDir, entry.name))
    .sort();
  if (!artifacts.some((path) => path.endsWith('.elf')) || !artifacts.some((path) => path.endsWith('.bin'))) {
    throw new Error('build path validation requires both ELF and binary artifacts');
  }

  for (const artifact of artifacts) {
    const bytes = readFileSync(artifact);
    for (const root of forbiddenRoots) {
      if (bytes.indexOf(Buffer.from(root)) !== -1) {
        throw new Error(`absolute build path leaked into ${artifact}: ${root}`);
      }
    }
  }
  console.log(`validated ${artifacts.length} build artifacts contain no absolute compiler roots`);
}

try {
  main();
} catch (error) {
  console.error(`build path validation failed: ${error.message}`);
  process.exitCode = 1;
}
