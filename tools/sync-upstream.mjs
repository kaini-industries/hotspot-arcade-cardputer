#!/usr/bin/env node
// Replace vendor/ from one reviewed, committed upstream Git object. The source
// working tree is never copied, and all four destinations are replaced together.

import {
  copyFileSync,
  cpSync,
  existsSync,
  lstatSync,
  mkdirSync,
  mkdtempSync,
  readFileSync,
  readdirSync,
  renameSync,
  rmSync,
  statSync,
  writeFileSync,
} from 'node:fs';
import { createHash } from 'node:crypto';
import { execFileSync } from 'node:child_process';
import { dirname, join, relative, resolve, sep } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const fail = (message) => { throw new Error(message); };
const assert = (condition, message) => { if (!condition) fail(message); };
const run = (command, args, options = {}) => execFileSync(command, args, {
  encoding: 'utf8',
  stdio: ['ignore', 'pipe', 'pipe'],
  ...options,
}).trim();
const git = (repository, ...args) => run('git', ['-C', repository, ...args]);
const sha256 = (bytes) => createHash('sha256').update(bytes).digest('hex');
const readJson = (path) => {
  try { return JSON.parse(readFileSync(path, 'utf8')); }
  catch (error) { fail(`${relative(root, path)} is invalid JSON: ${error.message}`); }
};
const inside = (base, path, context) => {
  const rel = relative(base, path);
  assert(rel && rel !== '..' && !rel.startsWith(`..${sep}`), `${context} escapes its root`);
  return path;
};
const safeFileName = (name, context) => {
  assert(typeof name === 'string' && /^[A-Za-z0-9][A-Za-z0-9._-]*$/.test(name) && name !== '.' && name !== '..',
    `${context} is unsafe`);
  return name;
};
const safeTreeName = (name, context) => {
  assert(typeof name === 'string' && /^[A-Za-z0-9._+@-]+$/.test(name) && name !== '.' && name !== '..',
    `${context} is unsafe`);
  return name;
};

const usage = 'usage: node tools/sync-upstream.mjs --repo <clean-checkout> --commit <40-hex-sha>';
let repository;
let requestedCommit;
for (let index = 2; index < process.argv.length; index += 1) {
  const argument = process.argv[index];
  if (argument === '--repo' && !repository) repository = process.argv[++index];
  else if (argument === '--commit' && !requestedCommit) requestedCommit = process.argv[++index];
  else fail(`${usage}\nunknown or duplicate argument: ${argument ?? '<missing>'}`);
}
assert(repository && requestedCommit, usage);
repository = resolve(repository);
assert(/^[0-9a-f]{40}$/.test(requestedCommit), '--commit must be an explicit 40-character lowercase hexadecimal SHA');
assert(existsSync(join(repository, '.git')), `${repository} is not a Git checkout`);

const sourceContract = readJson(join(root, 'tools', 'upstream-source.json'));
assert(sourceContract?.schema === 1 && typeof sourceContract.repository === 'string', 'tools/upstream-source.json is invalid');
const canonicalizeRemote = (remote) => {
  const trimmed = remote.trim().replace(/\.git$/, '').replace(/\/$/, '');
  let match = /^git@github\.com:([^/]+\/[^/]+)$/.exec(trimmed);
  if (!match) match = /^ssh:\/\/git@github\.com\/([^/]+\/[^/]+)$/.exec(trimmed);
  if (!match) match = /^https:\/\/github\.com\/([^/]+\/[^/]+)$/.exec(trimmed);
  return match ? `https://github.com/${match[1]}` : null;
};
const expectedRemote = canonicalizeRemote(sourceContract.repository);
assert(expectedRemote === sourceContract.repository, 'reviewed upstream repository must be canonical HTTPS without .git');
const actualRemote = canonicalizeRemote(git(repository, 'remote', 'get-url', 'origin'));
assert(actualRemote === expectedRemote, `origin must be ${expectedRemote}; found ${actualRemote ?? 'an unsupported URL'}`);
assert(git(repository, 'status', '--porcelain=v1', '--untracked-files=all') === '', 'upstream checkout must be clean, including untracked files');
assert(git(root, 'status', '--porcelain=v1', '--untracked-files=all') === '', 'downstream checkout must be clean before replacing vendor/');
const resolvedCommit = git(repository, 'rev-parse', '--verify', `${requestedCommit}^{commit}`);
assert(resolvedCommit === requestedCommit, `requested commit resolved to unexpected object ${resolvedCommit}`);

const previousLockPath = join(root, 'UPSTREAM.lock.json');
if (existsSync(previousLockPath)) {
  const previousLock = readJson(previousLockPath);
  assert(previousLock.repository === expectedRemote,
    `existing provenance lock uses unreviewed repository ${previousLock.repository ?? '<missing>'}`);
}

const transaction = mkdtempSync(join(root, '.vendor-stage-'));
const extracted = join(transaction, 'source');
const stagedVendor = join(transaction, 'vendor');
const archive = join(transaction, 'source.tar');
mkdirSync(extracted);
mkdirSync(stagedVendor);

const checkTree = (directory) => {
  const walk = (current) => {
    for (const entry of readdirSync(current, { withFileTypes: true })) {
      safeTreeName(entry.name, relative(directory, join(current, entry.name)) || entry.name);
      const path = join(current, entry.name);
      const stat = lstatSync(path);
      assert(!stat.isSymbolicLink(), `${relative(directory, path)} is a symlink`);
      if (stat.isDirectory()) walk(path);
      else assert(stat.isFile(), `${relative(directory, path)} is not a regular file`);
    }
  };
  walk(directory);
};
const copyFile = (from, to) => {
  assert(existsSync(from) && statSync(from).isFile() && !lstatSync(from).isSymbolicLink(), `missing safe source file ${relative(extracted, from)}`);
  mkdirSync(dirname(to), { recursive: true });
  copyFileSync(from, to);
};

let completed = false;
try {
  execFileSync('git', ['-C', repository, 'archive', '--format=tar', '--output', archive, requestedCommit], { stdio: 'pipe' });
  execFileSync('tar', ['-xf', archive, '-C', extracted], { stdio: 'pipe' });
  checkTree(extracted);

  // Engine: this is the complete reviewed public header surface consumed here.
  const engineSource = join(extracted, 'esp32', 'hotspot-arcade-fw');
  const engineDestination = join(stagedVendor, 'engine');
  mkdirSync(engineDestination);
  for (const name of ['ha_proto.h', 'ha_json.h', 'ha_games.h']) {
    copyFile(join(engineSource, name), join(engineDestination, name));
  }

  // Web: copy exactly what the committed manifest names, rejecting unsafe and
  // duplicate routes before any live tree is touched.
  const webSource = join(extracted, 'web', 'dist');
  const webDestination = join(stagedVendor, 'web');
  const webManifest = readJson(join(webSource, 'manifest.json'));
  assert(Array.isArray(webManifest) && webManifest.length > 0, 'upstream web manifest must be a nonempty array');
  const routes = new Set();
  const webFiles = new Set();
  mkdirSync(webDestination);
  copyFile(join(webSource, 'manifest.json'), join(webDestination, 'manifest.json'));
  for (const [index, entry] of webManifest.entries()) {
    assert(entry && typeof entry === 'object' && !Array.isArray(entry), `web manifest entry ${index} must be an object`);
    assert(Object.keys(entry).every((key) => ['path', 'file', 'mime', 'gzip'].includes(key)), `web manifest entry ${index} has unknown fields`);
    assert(typeof entry.path === 'string' && entry.path.startsWith('/') && !entry.path.includes('..') &&
      !entry.path.includes('\\') && !entry.path.includes('?') && !entry.path.includes('#'), `web route ${index} is unsafe`);
    assert(!routes.has(entry.path), `duplicate web route ${entry.path}`);
    routes.add(entry.path);
    safeFileName(entry.file, `web manifest file ${index}`);
    assert(entry.file.endsWith('.gz') && entry.gzip === true, `web asset ${entry.file} must be gzip`);
    assert(!webFiles.has(entry.file), `duplicate web file ${entry.file}`);
    webFiles.add(entry.file);
    copyFile(inside(webSource, join(webSource, entry.file), `web asset ${entry.file}`), join(webDestination, entry.file));
  }

  // Packs: unknown directories/files are provenance failures rather than content
  // silently omitted from the device.
  const contentContract = readJson(join(root, 'tools', 'content-manifest.json'));
  const allowedPackDirectories = new Set(contentContract.games.filter((game) => game.packDirectory).map((game) => game.packDirectory));
  const packsSource = join(extracted, 'packs');
  const packEntries = readdirSync(packsSource, { withFileTypes: true });
  for (const entry of packEntries) {
    assert(entry.isDirectory() && allowedPackDirectories.has(entry.name), `unknown upstream pack entry ${entry.name}`);
    for (const file of readdirSync(join(packsSource, entry.name), { withFileTypes: true })) {
      assert(file.isFile() && /^[A-Za-z0-9][A-Za-z0-9._-]*\.txt$/i.test(file.name),
        `unsafe upstream pack entry ${entry.name}/${file.name}`);
    }
  }
  for (const directory of allowedPackDirectories) {
    assert(packEntries.some((entry) => entry.name === directory), `upstream is missing pack directory ${directory}`);
  }
  cpSync(packsSource, join(stagedVendor, 'packs'), { recursive: true, errorOnExist: true });

  const libsSource = join(extracted, 'esp32', 'libs');
  assert(existsSync(libsSource) && statSync(libsSource).isDirectory(), 'upstream esp32/libs is missing');
  cpSync(libsSource, join(stagedVendor, 'libs'), { recursive: true, errorOnExist: true });
  checkTree(stagedVendor);

  const filePaths = [];
  const collect = (directory) => {
    for (const entry of readdirSync(directory, { withFileTypes: true }).sort((a, b) => a.name.localeCompare(b.name, 'en'))) {
      const path = join(directory, entry.name);
      if (entry.isDirectory()) collect(path);
      else filePaths.push(path);
    }
  };
  collect(stagedVendor);
  const files = filePaths.map((path) => ({
    path: `vendor/${relative(stagedVendor, path).split(sep).join('/')}`,
    sha256: sha256(readFileSync(path)),
    size: statSync(path).size,
  }));
  files.sort((left, right) => left.path.localeCompare(right.path, 'en'));
  const treeMaterial = files.map((file) => `${file.sha256}  ${file.size}  ${file.path}\n`).join('');
  const sourceTreeSha256 = sha256(Buffer.from(treeMaterial, 'utf8'));
  let describe;
  try { describe = git(repository, 'describe', '--tags', '--always', requestedCommit); }
  catch { describe = requestedCommit.slice(0, 12); }
  const packCount = files.filter((file) => file.path.startsWith('vendor/packs/') && file.path.endsWith('.txt')).length;
  const lock = {
    schema: 1,
    repository: expectedRemote,
    commit: requestedCommit,
    describe,
    sourceTreeSha256,
    files,
  };
  const lockText = `${JSON.stringify(lock, null, 2)}\n`;
  const upstreamText = `# Upstream\n\nEverything under \`vendor/\` is copied from one committed Git object in\n` +
    `[tarikbc/hotspot-arcade](${expectedRemote}) (MIT, Tarik Caramanico). ` +
    `Third-party libraries retain their own license files. No file in \`vendor/\` is edited downstream.\n\n` +
    `| | |\n| --- | --- |\n| repository | \`${expectedRemote}\` |\n| commit | \`${requestedCommit}\` |\n` +
    `| describe | \`${describe}\` |\n| source tree SHA-256 | \`${sourceTreeSha256}\` |\n` +
    `| web bundle | ${webManifest.length} file(s) |\n| content packs | ${packCount} pack(s) |\n` +
    `| full file inventory | \`UPSTREAM.lock.json\` |\n\nRefresh from a clean upstream and downstream checkout:\n\n` +
    `\`\`\`sh\nnode tools/sync-upstream.mjs --repo ../hotspot-arcade --commit <40-character-sha>\n` +
    `node tools/gen-assets.mjs\n\`\`\`\n\nReview \`git diff -- vendor UPSTREAM.md UPSTREAM.lock.json\` before committing.\n`;

  const liveVendor = join(root, 'vendor');
  const backupVendor = join(transaction, 'vendor.previous');
  const upstreamPath = join(root, 'UPSTREAM.md');
  const backupLock = join(transaction, 'UPSTREAM.lock.previous.json');
  const backupMarkdown = join(transaction, 'UPSTREAM.previous.md');
  const lockTemp = join(transaction, 'UPSTREAM.lock.json');
  const markdownTemp = join(transaction, 'UPSTREAM.md');
  writeFileSync(lockTemp, lockText, 'utf8');
  writeFileSync(markdownTemp, upstreamText, 'utf8');
  const hadLock = existsSync(previousLockPath);
  const hadMarkdown = existsSync(upstreamPath);
  let vendorBackedUp = false;
  let lockBackedUp = false;
  let markdownBackedUp = false;
  let lockInstalled = false;
  let markdownInstalled = false;
  try {
    renameSync(liveVendor, backupVendor);
    vendorBackedUp = true;
    if (hadLock) {
      renameSync(previousLockPath, backupLock);
      lockBackedUp = true;
    }
    if (hadMarkdown) {
      renameSync(upstreamPath, backupMarkdown);
      markdownBackedUp = true;
    }
    renameSync(stagedVendor, liveVendor);
    renameSync(lockTemp, previousLockPath);
    lockInstalled = true;
    renameSync(markdownTemp, upstreamPath);
    markdownInstalled = true;
  } catch (error) {
    if (vendorBackedUp) {
      if (existsSync(liveVendor)) rmSync(liveVendor, { recursive: true, force: true });
      if (existsSync(backupVendor)) renameSync(backupVendor, liveVendor);
    }
    if (lockBackedUp) {
      if (existsSync(previousLockPath)) rmSync(previousLockPath, { force: true });
      if (existsSync(backupLock)) renameSync(backupLock, previousLockPath);
    } else if (lockInstalled && existsSync(previousLockPath)) {
      rmSync(previousLockPath, { force: true });
    }
    if (markdownBackedUp) {
      if (existsSync(upstreamPath)) rmSync(upstreamPath, { force: true });
      if (existsSync(backupMarkdown)) renameSync(backupMarkdown, upstreamPath);
    } else if (markdownInstalled && existsSync(upstreamPath)) {
      rmSync(upstreamPath, { force: true });
    }
    throw error;
  }
  completed = true;
  console.log(`synced vendor/ from ${expectedRemote}`);
  console.log(`  commit ${describe} (${requestedCommit})`);
  console.log(`  ${files.length} files; source tree SHA-256 ${sourceTreeSha256}`);
  console.log('next: node tools/gen-assets.mjs');
} finally {
  rmSync(transaction, { recursive: true, force: true });
  if (!completed) console.error('vendor/ was not changed');
}
