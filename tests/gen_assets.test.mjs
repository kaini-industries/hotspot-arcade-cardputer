import test from 'node:test';
import assert from 'node:assert/strict';
import {
  copyFileSync,
  mkdtempSync,
  mkdirSync,
  readFileSync,
  rmSync,
  writeFileSync,
} from 'node:fs';
import { gzipSync } from 'node:zlib';
import { join, resolve } from 'node:path';
import { tmpdir } from 'node:os';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const project = resolve(fileURLToPath(new URL('..', import.meta.url)));
const put = (path, value) => {
  mkdirSync(resolve(path, '..'), { recursive: true });
  writeFileSync(path, value);
};
const invoke = (cwd, ...args) => execFileSync(process.execPath, ['tools/gen-assets.mjs', ...args], {
  cwd,
  encoding: 'utf8',
  stdio: ['ignore', 'pipe', 'pipe'],
});
const failure = (cwd, pattern, ...args) => {
  assert.throws(() => invoke(cwd, ...args), (error) => pattern.test(`${error.stderr ?? ''}${error.stdout ?? ''}${error.message ?? ''}`));
};

test('asset generation is deterministic and rejects omissions, unsafe content, and malformed bytes', () => {
  const root = mkdtempSync(join(tmpdir(), 'ha-assets-test-'));
  try {
    mkdirSync(join(root, 'tools'));
    mkdirSync(join(root, 'hotspot-arcade-cardputer'));
    copyFileSync(join(project, 'tools', 'gen-assets.mjs'), join(root, 'tools', 'gen-assets.mjs'));
    const contract = {
      schema: 1,
      maxPacksPerGame: 1,
      languages: [{ code: 'en', label: 'English', root: 'vendor/packs' }],
      games: [
        {
          id: 5,
          constant: 'HA_GAME_DRAW',
          key: 'draw',
          label: 'Drawing',
          description: 'Draw it',
          duel: false,
          packDirectory: 'draw',
          requiredKeys: ['Word'],
        },
        {
          id: 0,
          constant: 'HA_GAME_NONE',
          key: 'none',
          label: 'None',
          description: 'Lobby',
          duel: false,
        },
      ],
    };
    put(join(root, 'tools', 'content-manifest.json'), `${JSON.stringify(contract, null, 2)}\n`);
    const webManifest = [{ path: '/', file: 'index.html.gz', mime: 'text/html', gzip: true }];
    put(join(root, 'vendor', 'web', 'manifest.json'), `${JSON.stringify(webManifest, null, 2)}\n`);
    put(join(root, 'vendor', 'web', 'index.html.gz'), gzipSync(Buffer.from('<h1>ok</h1>')));
    for (const name of ['ha_proto.h', 'ha_json.h', 'ha_games.h']) put(join(root, 'vendor', 'engine', name), `// ${name}\n`);
    const validPack = 'Pack: É\nWord: café\n';
    const packPath = join(root, 'vendor', 'packs', 'draw', 'words.txt');
    put(packPath, validPack);

    invoke(root);
    const outputs = ['ha_bundle.h', 'ha_metadata.h', 'ha_proto.h', 'ha_json.h', 'ha_games.h'];
    const first = outputs.map((name) => readFileSync(join(root, 'hotspot-arcade-cardputer', name)));
    invoke(root);
    const second = outputs.map((name) => readFileSync(join(root, 'hotspot-arcade-cardputer', name)));
    assert.deepEqual(second, first);
    invoke(root, '--check');
    const header = first[0].toString('utf8');
    assert.match(header, new RegExp(`packs: ${Buffer.byteLength(validPack, 'utf8')} UTF-8 bytes`));
    assert.match(readFileSync(join(root, 'hotspot-arcade-cardputer', 'ha_metadata.h'), 'utf8'), /HA_GENERATED_LANGUAGES/);

    put(join(root, 'hotspot-arcade-cardputer', 'ha_metadata.h'), '// stale\n');
    failure(root, /generated files are stale/, '--check');
    invoke(root);

    put(join(root, 'vendor', 'packs', 'unknown', 'words.txt'), validPack);
    failure(root, /unknown game entries/);
    rmSync(join(root, 'vendor', 'packs', 'unknown'), { recursive: true });

    put(join(root, 'vendor', 'packs', 'draw', 'extra.txt'), validPack);
    failure(root, /refusing truncation/);
    rmSync(join(root, 'vendor', 'packs', 'draw', 'extra.txt'));

    put(packPath, Buffer.from([0xff, 0xfe]));
    failure(root, /not valid UTF-8/);
    put(packPath, 'Pack: Broken\nNotWord: value\n');
    failure(root, /unknown key|missing Word/);
    put(packPath, 'Pack: Broken\nWord: )HAPACK"\n');
    failure(root, /reserved raw-string delimiter/);
    put(packPath, validPack);

    put(join(root, 'vendor', 'web', 'manifest.json'), `${JSON.stringify([
      ...webManifest,
      { ...webManifest[0], file: 'second.html.gz' },
    ])}\n`);
    put(join(root, 'vendor', 'web', 'second.html.gz'), gzipSync(Buffer.from('duplicate')));
    failure(root, /duplicate web route/);
    rmSync(join(root, 'vendor', 'web', 'second.html.gz'));
    put(join(root, 'vendor', 'web', 'manifest.json'), `${JSON.stringify(webManifest)}\n`);
    put(join(root, 'vendor', 'web', 'index.html.gz'), Buffer.from('not gzip'));
    failure(root, /not valid gzip/);
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
});
