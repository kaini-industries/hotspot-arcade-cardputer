import { createHash } from 'node:crypto';
import { lstatSync, readFileSync, readdirSync, statSync } from 'node:fs';
import { dirname, join, relative, resolve, sep } from 'node:path';
import { fileURLToPath } from 'node:url';
import { execFileSync } from 'node:child_process';

export const REPOSITORY_ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');

export function digest(algorithm, value) {
  return createHash(algorithm).update(value).digest('hex');
}

export function fileDigest(path, algorithm = 'sha256') {
  return digest(algorithm, readFileSync(path));
}

export function sourceDateEpoch() {
  const value = process.env.SOURCE_DATE_EPOCH ?? '';
  if (!/^(0|[1-9][0-9]*)$/.test(value)) {
    throw new Error('SOURCE_DATE_EPOCH must be set to integer seconds for release metadata');
  }
  const epoch = Number(value);
  if (!Number.isSafeInteger(epoch)) throw new Error('SOURCE_DATE_EPOCH is outside the safe integer range');
  return epoch;
}

export function readCleanGitSource(repoRoot = REPOSITORY_ROOT) {
  const status = execFileSync(
    'git',
    ['-C', repoRoot, 'status', '--porcelain=v1', '--untracked-files=all'],
    { encoding: 'utf8' },
  );
  if (status !== '') {
    throw new Error('release source checkout is dirty; commit or remove every tracked and untracked change');
  }
  const commit = execFileSync('git', ['-C', repoRoot, 'rev-parse', '--verify', 'HEAD^{commit}'], {
    encoding: 'utf8',
  }).trim();
  if (!/^[0-9a-f]{40}$/.test(commit)) throw new Error('release source commit is not a full Git object ID');
  return { commit, sourceTreeClean: true };
}

export function readToolchainLock(repoRoot = REPOSITORY_ROOT) {
  const lock = JSON.parse(readFileSync(join(repoRoot, 'tools', 'toolchain.lock.json'), 'utf8'));
  if (lock.schema !== 1 || !lock.arduino?.fqbn || !lock.arduino?.core?.version) {
    throw new Error('toolchain.lock.json has an unsupported or incomplete schema');
  }
  return lock;
}

function inventory(directory, repoRoot) {
  const paths = [];
  const visit = (current) => {
    for (const entry of readdirSync(current, { withFileTypes: true }).sort((a, b) => a.name.localeCompare(b.name, 'en'))) {
      const path = join(current, entry.name);
      const metadata = lstatSync(path);
      if (metadata.isSymbolicLink()) throw new Error(`upstream vendor tree contains a symbolic link: ${path}`);
      if (entry.isDirectory()) visit(path);
      else if (entry.isFile()) paths.push(relative(repoRoot, path).split(sep).join('/'));
      else throw new Error(`upstream vendor tree contains a non-file entry: ${path}`);
    }
  };
  visit(directory);
  return paths.sort((left, right) => left.localeCompare(right, 'en'));
}

export function readUpstreamLock(repoRoot = REPOSITORY_ROOT, { verifyTree = true } = {}) {
  const lockPath = join(repoRoot, 'UPSTREAM.lock.json');
  const lock = JSON.parse(readFileSync(lockPath, 'utf8'));
  if (lock.schema !== 1) throw new Error('UPSTREAM.lock.json schema must be 1');
  let repository;
  try {
    repository = new URL(lock.repository);
  } catch {
    throw new Error('UPSTREAM.lock.json repository must be an absolute URL');
  }
  if (
    repository.protocol !== 'https:' ||
    repository.hostname !== 'github.com' ||
    repository.username ||
    repository.password ||
    repository.port ||
    repository.search ||
    repository.hash ||
    !/^\/[A-Za-z0-9_.-]+\/[A-Za-z0-9_.-]+$/.test(repository.pathname)
  ) {
    throw new Error('UPSTREAM.lock.json repository must be a clean HTTPS GitHub repository URL');
  }
  if (!/^[0-9a-f]{40}$/.test(lock.commit ?? '')) throw new Error('UPSTREAM.lock.json commit must be a full SHA-1');
  if (!/^[0-9a-f]{64}$/.test(lock.sourceTreeSha256 ?? '')) {
    throw new Error('UPSTREAM.lock.json sourceTreeSha256 must be a SHA-256');
  }
  if (!Array.isArray(lock.files) || lock.files.length === 0) throw new Error('UPSTREAM.lock.json files must be nonempty');

  const paths = [];
  for (const file of lock.files) {
    if (
      typeof file?.path !== 'string' ||
      !file.path.startsWith('vendor/') ||
      file.path.includes('\\') ||
      file.path.split('/').includes('..') ||
      !/^[0-9a-f]{64}$/.test(file.sha256 ?? '') ||
      !Number.isSafeInteger(file.size) ||
      file.size < 0
    ) {
      throw new Error('UPSTREAM.lock.json contains an invalid file entry');
    }
    paths.push(file.path);
  }
  const sortedPaths = [...paths].sort((left, right) => left.localeCompare(right, 'en'));
  if (new Set(paths).size !== paths.length || paths.some((path, index) => path !== sortedPaths[index])) {
    throw new Error('UPSTREAM.lock.json file inventory must be unique and sorted');
  }
  const treeMaterial = lock.files.map((file) => `${file.sha256}  ${file.size}  ${file.path}\n`).join('');
  if (digest('sha256', treeMaterial) !== lock.sourceTreeSha256) {
    throw new Error('UPSTREAM.lock.json source tree digest does not match its inventory');
  }

  if (verifyTree) {
    const vendorDirectory = join(repoRoot, 'vendor');
    const actualPaths = inventory(vendorDirectory, repoRoot);
    if (actualPaths.length !== paths.length || actualPaths.some((path, index) => path !== paths[index])) {
      throw new Error('vendor/ file inventory does not match UPSTREAM.lock.json');
    }
    for (const file of lock.files) {
      const path = join(repoRoot, file.path);
      if (statSync(path).size !== file.size || fileDigest(path) !== file.sha256) {
        throw new Error(`vendored upstream file does not match UPSTREAM.lock.json: ${file.path}`);
      }
    }
  }
  return lock;
}
