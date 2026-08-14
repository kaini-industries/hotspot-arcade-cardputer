import test from 'node:test';
import assert from 'node:assert/strict';
import {
  copyFileSync,
  cpSync,
  existsSync,
  mkdtempSync,
  mkdirSync,
  readFileSync,
  rmSync,
  writeFileSync,
} from 'node:fs';
import { createHash } from 'node:crypto';
import { gzipSync } from 'node:zlib';
import { join, resolve } from 'node:path';
import { tmpdir } from 'node:os';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const project = resolve(fileURLToPath(new URL('..', import.meta.url)));
const node = process.execPath;
const run = (command, args, cwd) => execFileSync(command, args, {
  cwd,
  encoding: 'utf8',
  stdio: ['ignore', 'pipe', 'pipe'],
});
const git = (cwd, ...args) => run('git', args, cwd).trim();
const put = (path, value) => {
  mkdirSync(resolve(path, '..'), { recursive: true });
  writeFileSync(path, value);
};
const commitAll = (cwd, message) => {
  git(cwd, 'add', '-A');
  git(cwd, 'commit', '-m', message);
  return git(cwd, 'rev-parse', 'HEAD');
};
const initRepository = (cwd) => {
  mkdirSync(cwd, { recursive: true });
  git(cwd, 'init', '-q');
  git(cwd, 'config', 'user.name', 'Vendoring Test');
  git(cwd, 'config', 'user.email', 'vendoring@example.invalid');
};
const failure = (fn, pattern) => {
  assert.throws(fn, (error) => pattern.test(`${error.stderr ?? ''}${error.stdout ?? ''}${error.message ?? ''}`));
};

test('sync copies only the exact commit and records a complete deterministic lock', () => {
  const temporary = mkdtempSync(join(tmpdir(), 'ha-sync-test-'));
  const upstream = join(temporary, 'upstream');
  const downstream = join(temporary, 'downstream');
  try {
    initRepository(upstream);
    git(upstream, 'remote', 'add', 'origin', 'https://github.com/kaini-industries/hotspot-arcade.git');
    for (const name of ['ha_proto.h', 'ha_json.h', 'ha_games.h']) {
      put(join(upstream, 'esp32', 'hotspot-arcade-fw', name), `// committed ${name}\n`);
    }
    put(join(upstream, 'web', 'dist', 'manifest.json'), `${JSON.stringify([
      { path: '/', file: 'index.html.gz', mime: 'text/html', gzip: true },
    ], null, 2)}\n`);
    put(join(upstream, 'web', 'dist', 'index.html.gz'), gzipSync(Buffer.from('<h1>test</h1>')));
    const packShapes = {
      trivia: 'Pack: Test\nQ: Q?\nA: A\nB: B\nC: C\nD: D\nAnswer: A\n',
      wyr: 'Pack: Test\nA: A\nB: B\n',
      scramble: 'Pack: Test\nWord: hello\n',
      draw: 'Pack: Test\nWord: house\n',
      spectrum: 'Pack: Test\nLeft: A\nRight: B\n',
      kmk: 'Pack: Test\nName: Ada\n',
      secrets: 'Pack: Test\nQ: Secret?\n',
      fillblank: 'Pack: Test\nP: _____ wins.\n---\nA: a robot\n',
      spyfall: 'Pack: Test\nLoc: Beach\nR: Lifeguard\nR: Surfer\n',
    };
    put(join(upstream, 'packs', 'README.md'), '# Pack format\n');
    for (const [directory, text] of Object.entries(packShapes)) {
      put(join(upstream, 'packs', directory, 'test.txt'), text);
      put(join(upstream, 'packs', directory, 'de', 'test.txt'), text);
      if (!['secrets', 'fillblank', 'spyfall'].includes(directory))
        put(join(upstream, 'packs', directory, 'pt-br', 'test.txt'), text);
    }
    put(join(upstream, 'packs', 'spectrum', 'README.md'), '# Spectrum notes\n');
    put(join(upstream, 'esp32', 'libs', 'Example', '.gitignore'), 'build\n');
    put(join(upstream, 'esp32', 'libs', 'Example', 'LICENSE'), 'test license\n');
    const commit = commitAll(upstream, 'fixture upstream');

    initRepository(downstream);
    mkdirSync(join(downstream, 'tools'), { recursive: true });
    copyFileSync(join(project, 'tools', 'sync-upstream.mjs'), join(downstream, 'tools', 'sync-upstream.mjs'));
    copyFileSync(join(project, 'tools', 'upstream-source.json'), join(downstream, 'tools', 'upstream-source.json'));
    put(join(downstream, 'tools', 'content-manifest.json'), `${JSON.stringify({
      schema: 2,
      maxPacksPerGame: 8,
      languages: [
        { code: 'en', label: 'English', root: 'vendor/packs/en' },
        { code: 'de', label: 'Deutsch', root: 'vendor/packs/de', fallback: 'en' },
        { code: 'pt-br', label: 'Português (Brasil)', root: 'vendor/packs/pt-br', fallback: 'en' },
      ],
      games: Object.keys(packShapes).map((packDirectory, id) => ({ id, packDirectory })),
    }, null, 2)}\n`);
    for (const directory of ['engine', 'web', 'packs', 'libs']) put(join(downstream, 'vendor', directory, 'obsolete.txt'), 'obsolete\n');
    put(join(downstream, 'UPSTREAM.md'), 'old provenance\n');
    commitAll(downstream, 'fixture downstream');

    git(upstream, 'remote', 'set-url', 'origin', 'https://github.com/example/hostile.git');
    failure(() => run(node, ['tools/sync-upstream.mjs', '--repo', upstream, '--commit', commit], downstream), /origin must be/);
    git(upstream, 'remote', 'set-url', 'origin', 'https://github.com/kaini-industries/hotspot-arcade.git');

    put(join(upstream, 'UNTRACKED'), 'dirty\n');
    failure(() => run(node, ['tools/sync-upstream.mjs', '--repo', upstream, '--commit', commit], downstream), /upstream checkout must be clean/);
    rmSync(join(upstream, 'UNTRACKED'));

    put(join(downstream, 'UNTRACKED'), 'dirty\n');
    failure(() => run(node, ['tools/sync-upstream.mjs', '--repo', upstream, '--commit', commit], downstream), /downstream checkout must be clean/);
    rmSync(join(downstream, 'UNTRACKED'));

    const hiddenChange = join(upstream, 'esp32', 'hotspot-arcade-fw', 'ha_proto.h');
    git(upstream, 'update-index', '--assume-unchanged', 'esp32/hotspot-arcade-fw/ha_proto.h');
    writeFileSync(hiddenChange, '// working-tree injection\n');
    assert.equal(git(upstream, 'status', '--porcelain=v1', '--untracked-files=all'), '');

    run(node, ['tools/sync-upstream.mjs', '--repo', upstream, '--commit', commit], downstream);
    const firstLockDocument = readFileSync(join(downstream, 'UPSTREAM.lock.json'), 'utf8');
    const firstUpstreamDocument = readFileSync(join(downstream, 'UPSTREAM.md'), 'utf8');
    commitAll(downstream, 'first deterministic sync');
    run(node, ['tools/sync-upstream.mjs', '--repo', upstream, '--commit', commit], downstream);
    assert.equal(git(downstream, 'status', '--porcelain=v1', '--untracked-files=all'), '');
    assert.equal(readFileSync(join(downstream, 'UPSTREAM.lock.json'), 'utf8'), firstLockDocument);
    assert.equal(readFileSync(join(downstream, 'UPSTREAM.md'), 'utf8'), firstUpstreamDocument);
    assert.equal(readFileSync(join(downstream, 'vendor', 'engine', 'ha_proto.h'), 'utf8'), '// committed ha_proto.h\n');
    for (const directory of ['engine', 'web', 'packs', 'libs']) {
      assert.equal(existsSync(join(downstream, 'vendor', directory, 'obsolete.txt')), false, `${directory} was not fully replaced`);
    }
    for (const language of ['en', 'de', 'pt-br']) {
      assert.equal(
        readFileSync(join(downstream, 'vendor', 'packs', language, 'draw', 'test.txt'), 'utf8'),
        packShapes.draw,
      );
    }
    assert.equal(existsSync(join(downstream, 'vendor', 'packs', 'pt-br', 'fillblank')), false);
    assert.equal(existsSync(join(downstream, 'vendor', 'packs', 'pt-br', 'spyfall')), false);
    assert.equal(existsSync(join(downstream, 'vendor', 'packs', 'README.md')), false);
    const lock = JSON.parse(readFileSync(join(downstream, 'UPSTREAM.lock.json'), 'utf8'));
    assert.equal(lock.repository, 'https://github.com/kaini-industries/hotspot-arcade');
    assert.equal(lock.commit, commit);
    assert.ok(lock.files.length > 10);
    assert.deepEqual(lock.files.map((file) => file.path), [...lock.files.map((file) => file.path)].sort());
    for (const file of lock.files) {
      const bytes = readFileSync(join(downstream, file.path));
      assert.equal(file.size, bytes.length);
      assert.equal(file.sha256, createHash('sha256').update(bytes).digest('hex'));
    }
    const treeMaterial = lock.files.map((file) => `${file.sha256}  ${file.size}  ${file.path}\n`).join('');
    assert.equal(lock.sourceTreeSha256, createHash('sha256').update(treeMaterial).digest('hex'));
    const upstreamDocument = readFileSync(join(downstream, 'UPSTREAM.md'), 'utf8');
    assert.match(upstreamDocument, new RegExp(commit));
    assert.match(upstreamDocument, /kaini-industries\/hotspot-arcade/);
    assert.match(upstreamDocument, /Tarik Caramanico's original hotspot-arcade/);
  } finally {
    rmSync(temporary, { recursive: true, force: true });
  }
});
