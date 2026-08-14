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
      schema: 2,
      maxPacksPerGame: 1,
      languages: [
        { code: 'en', label: 'English', root: 'vendor/packs/en' },
        { code: 'de', label: 'Deutsch', root: 'vendor/packs/de', fallback: 'en' },
      ],
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
          keyByteLimits: { Word: 23 },
          minItemsPerPack: 1,
          maxItemsPerPack: 1,
        },
        {
          id: 17,
          constant: 'HA_GAME_FILLBLANK',
          key: 'fillblank',
          label: 'Fill the Blank',
          description: 'Cards',
          duel: false,
          packDirectory: 'fillblank',
          requiredKeys: [],
          oneOfKeys: ['P', 'A'],
          keyByteLimits: { P: 127, A: 127 },
          minItemsPerPack: 2,
          maxItemsPerPack: 2,
          packKeyLimits: { P: { min: 1, max: 2 }, A: { min: 1, max: 1 } },
        },
        {
          id: 19,
          constant: 'HA_GAME_SPYFALL',
          key: 'spyfall',
          label: 'Spyfall',
          description: 'Roles',
          duel: false,
          packDirectory: 'spyfall',
          requiredKeys: ['Loc', 'R'],
          keyByteLimits: { Loc: 63, R: 63 },
          repeatableKeys: { R: 2 },
          minItemsPerPack: 1,
          maxItemsPerPack: 1,
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
    const contractPath = join(root, 'tools', 'content-manifest.json');
    const writeContract = () => put(contractPath, `${JSON.stringify(contract, null, 2)}\n`);
    writeContract();
    const webManifest = [{ path: '/', file: 'index.html.gz', mime: 'text/html', gzip: true }];
    put(join(root, 'vendor', 'web', 'manifest.json'), `${JSON.stringify(webManifest, null, 2)}\n`);
    put(join(root, 'vendor', 'web', 'index.html.gz'), gzipSync(Buffer.from('<h1>ok</h1>')));
    for (const name of ['ha_proto.h', 'ha_json.h', 'ha_games.h']) put(join(root, 'vendor', 'engine', name), `// ${name}\n`);
    const validPack = 'Pack: É\nWord: café\n';
    const packPath = join(root, 'vendor', 'packs', 'en', 'draw', 'words.txt');
    put(packPath, validPack);
    const fillblankPath = join(root, 'vendor', 'packs', 'en', 'fillblank', 'cards.txt');
    put(fillblankPath, 'Pack: Cards\nP: _____ wins.\n---\nA: a robot\n');
    const spyfallPath = join(root, 'vendor', 'packs', 'en', 'spyfall', 'places.txt');
    put(spyfallPath, 'Pack: Places\nLoc: Beach\nR: Lifeguard\nR: Surfer\n');
    put(join(root, 'vendor', 'packs', 'de', 'draw', 'words.txt'), 'Pack: Wörter\nWord: Haus\n');

    invoke(root);
    const outputs = ['ha_bundle.h', 'ha_metadata.h', 'ha_proto.h', 'ha_json.h', 'ha_games.h'];
    const first = outputs.map((name) => readFileSync(join(root, 'hotspot-arcade-cardputer', name)));
    invoke(root);
    const second = outputs.map((name) => readFileSync(join(root, 'hotspot-arcade-cardputer', name)));
    assert.deepEqual(second, first);
    invoke(root, '--check');
    const header = first[0].toString('utf8');
    assert.match(header, /R: Lifeguard\nR: Surfer/);
    const metadata = readFileSync(join(root, 'hotspot-arcade-cardputer', 'ha_metadata.h'), 'utf8');
    assert.match(metadata, /HA_GENERATED_LANGUAGES/);
    assert.match(metadata, /\{"de", "Deutsch", "en"\}/);
    assert.match(metadata, /static_assert\(HA_GAME_FILLBLANK == 17/);

    delete contract.games[0].keyByteLimits.Word;
    writeContract();
    failure(root, /game draw keyByteLimits are missing Word/);
    contract.games[0].keyByteLimits.Word = 256;
    writeContract();
    failure(root, /game draw keyByteLimits\.Word is invalid/);
    contract.games[0].keyByteLimits.Word = 23;
    contract.games[0].keyByteLimits.NotWord = 23;
    writeContract();
    failure(root, /game draw keyByteLimits has unknown field.*NotWord/);
    delete contract.games[0].keyByteLimits.NotWord;
    writeContract();

    put(join(root, 'hotspot-arcade-cardputer', 'ha_metadata.h'), '// stale\n');
    failure(root, /generated files are stale/, '--check');
    invoke(root);

    put(join(root, 'vendor', 'packs', 'en', 'unknown', 'words.txt'), validPack);
    failure(root, /unknown game entries/);
    rmSync(join(root, 'vendor', 'packs', 'en', 'unknown'), { recursive: true });

    put(join(root, 'vendor', 'packs', 'en', 'draw', 'extra.txt'), validPack);
    failure(root, /refusing truncation/);
    rmSync(join(root, 'vendor', 'packs', 'en', 'draw', 'extra.txt'));

    put(packPath, Buffer.from([0xff, 0xfe]));
    failure(root, /not valid UTF-8/);
    put(packPath, 'Pack: Broken\nNotWord: value\n');
    failure(root, /unknown key|missing Word/);
    put(packPath, 'Pack: Broken\nWord: )HAPACK"\n');
    failure(root, /reserved raw-string delimiter/);
    put(packPath, validPack);

    put(packPath, `Pack: ${'x'.repeat(64)}\nWord: safe\n`);
    failure(root, /pack name exceeds the engine's 63-byte limit/);
    put(packPath, `Pack: Safe\nWord: ${'é'.repeat(12)}\n`);
    failure(root, /Word value is 24 UTF-8 bytes; maximum is 23/);
    put(packPath, `Pack: Safe\nWord: ${'x'.repeat(256)}\n`);
    failure(root, /value exceeds the engine's 255-byte limit/);
    put(packPath, 'Pack:  Padded\nWord: safe\n');
    failure(root, /leading or trailing whitespace/);
    put(packPath, 'Pack: Safe\nWord: padded \n');
    failure(root, /leading or trailing whitespace/);
    put(packPath, 'Pack: Safe\nWord: bad\u0001value\n');
    failure(root, /ASCII control byte/);
    put(packPath, validPack);

    put(fillblankPath, 'Pack: Broken\nP: prompt\nA: answer\n');
    failure(root, /exactly one of P, A/);
    put(fillblankPath, 'Pack: Broken\nP: prompt\n---\nP: another\n');
    failure(root, /0 A record/);
    put(fillblankPath, 'Pack: Cards\nP: _____ wins.\n---\nA: a robot\n');
    put(spyfallPath, 'Pack: Broken\nLoc: Beach\nR: One\nR: Two\nR: Three\n');
    failure(root, /repeats R more than 2/);
    put(spyfallPath, 'Pack: Places\nLoc: Beach\nR: Lifeguard\nR: Surfer\n');

    put(join(root, 'vendor', 'web', 'manifest.json'), `${JSON.stringify([
      ...webManifest,
      { ...webManifest[0], file: 'second.html.gz' },
    ])}\n`);
    put(join(root, 'vendor', 'web', 'second.html.gz'), gzipSync(Buffer.from('duplicate')));
    failure(root, /duplicate web route/);
    rmSync(join(root, 'vendor', 'web', 'second.html.gz'));
    put(join(root, 'vendor', 'web', 'manifest.json'), `${JSON.stringify([{ ...webManifest[0], crc: 1 }])}\n`);
    failure(root, /CRC does not match/);
    put(join(root, 'vendor', 'web', 'manifest.json'), `${JSON.stringify(webManifest)}\n`);
    put(
      join(root, 'vendor', 'web', 'index.html.gz'),
      gzipSync(Buffer.alloc(72 * 1024), { level: 0 }),
    );
    failure(root, /compressed web bundle is .* limit is 73728/);
    put(join(root, 'vendor', 'web', 'index.html.gz'), Buffer.from('not gzip'));
    failure(root, /not valid gzip/);
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
});
