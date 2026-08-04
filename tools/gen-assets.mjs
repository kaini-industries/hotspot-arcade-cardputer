#!/usr/bin/env node
// Validate and bake the reviewed upstream web/content manifest into deterministic
// Cardputer headers. This script deliberately rejects content it cannot prove is
// safe and complete; it never guesses, truncates, or silently skips an asset.

import {
  existsSync,
  lstatSync,
  readFileSync,
  readdirSync,
  writeFileSync,
} from 'node:fs';
import { createHash } from 'node:crypto';
import { gunzipSync } from 'node:zlib';
import { dirname, join, relative, resolve, sep } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const sketch = join(root, 'hotspot-arcade-cardputer');
const vendor = join(root, 'vendor');
const checkOnly = process.argv.includes('--check');
const unexpectedArgs = process.argv.slice(2).filter((arg) => arg !== '--check');
if (unexpectedArgs.length) throw new Error(`unknown argument(s): ${unexpectedArgs.join(', ')}`);

const fail = (message) => {
  throw new Error(message);
};
const assert = (condition, message) => {
  if (!condition) fail(message);
};
const utf8 = new TextDecoder('utf-8', { fatal: true });
const readUtf8 = (path) => {
  try {
    return utf8.decode(readFileSync(path));
  } catch (error) {
    fail(`${relative(root, path)} is not valid UTF-8: ${error.message}`);
  }
};
const readJson = (path) => {
  try {
    return JSON.parse(readUtf8(path));
  } catch (error) {
    fail(`${relative(root, path)} is not valid JSON: ${error.message}`);
  }
};
const ownKeys = (object) => Object.keys(object).sort();
const assertKeys = (object, allowed, context) => {
  assert(object && typeof object === 'object' && !Array.isArray(object), `${context} must be an object`);
  const unknown = ownKeys(object).filter((key) => !allowed.includes(key));
  assert(!unknown.length, `${context} has unknown field(s): ${unknown.join(', ')}`);
};
const safeSegment = (value, context) => {
  assert(typeof value === 'string' && /^[a-z0-9][a-z0-9._-]*$/i.test(value), `${context} is unsafe`);
  assert(value !== '.' && value !== '..', `${context} is unsafe`);
  return value;
};
const inside = (base, path, context) => {
  const rel = relative(base, path);
  assert(rel && rel !== '..' && !rel.startsWith(`..${sep}`) && !resolve(path).includes('\0'), `${context} escapes its root`);
  return path;
};
const cppString = (value) => {
  assert(typeof value === 'string', 'C++ string value must be a string');
  return `"${value
    .replaceAll('\\', '\\\\')
    .replaceAll('"', '\\"')
    .replaceAll('\n', '\\n')
    .replaceAll('\r', '\\r')
    .replaceAll('\t', '\\t')}"`;
};
const byteLength = (text) => Buffer.byteLength(text, 'utf8');
const sha256 = (bytes) => createHash('sha256').update(bytes).digest('hex');

// ---- reviewed content contract ---------------------------------------------
const contentPath = join(root, 'tools', 'content-manifest.json');
const content = readJson(contentPath);
assertKeys(content, ['schema', 'maxPacksPerGame', 'languages', 'games'], 'content manifest');
assert(content.schema === 1, 'content manifest schema must be 1');
assert(Number.isSafeInteger(content.maxPacksPerGame) && content.maxPacksPerGame > 0 && content.maxPacksPerGame <= 16,
  'maxPacksPerGame must be an integer from 1 to 16');
assert(Array.isArray(content.languages) && content.languages.length > 0, 'content manifest needs languages');
assert(Array.isArray(content.games) && content.games.length > 0, 'content manifest needs games');

const languageCodes = new Set();
for (const [index, language] of content.languages.entries()) {
  assertKeys(language, ['code', 'label', 'root'], `language ${index}`);
  assert(typeof language.code === 'string' && /^[a-z]{2}(?:-[a-z]{2})?$/.test(language.code), `language ${index} code is invalid`);
  assert(!languageCodes.has(language.code), `duplicate language code ${language.code}`);
  languageCodes.add(language.code);
  assert(typeof language.label === 'string' && language.label.length > 0 && byteLength(language.label) <= 48,
    `language ${language.code} label is invalid`);
  assert(typeof language.root === 'string' && !language.root.startsWith('/') && !language.root.includes('\\'),
    `language ${language.code} root is unsafe`);
  const resolved = resolve(root, language.root);
  inside(root, resolved, `language ${language.code} root`);
  assert(existsSync(resolved) && lstatSync(resolved).isDirectory() && !lstatSync(resolved).isSymbolicLink(),
    `language ${language.code} root is not a real directory`);
  language.resolvedRoot = resolved;
}

const gameIds = new Set();
const gameKeys = new Set();
const gameConstants = new Set();
const packDirectories = new Map();
for (const [index, game] of content.games.entries()) {
  assertKeys(game, ['id', 'constant', 'key', 'label', 'description', 'duel', 'packDirectory', 'requiredKeys'], `game ${index}`);
  assert(Number.isSafeInteger(game.id) && game.id >= 0 && game.id <= 255, `game ${index} id is invalid`);
  assert(!gameIds.has(game.id), `duplicate game id ${game.id}`);
  gameIds.add(game.id);
  assert(typeof game.constant === 'string' && /^HA_GAME_[A-Z0-9_]+$/.test(game.constant), `game ${index} constant is invalid`);
  assert(!gameConstants.has(game.constant), `duplicate game constant ${game.constant}`);
  gameConstants.add(game.constant);
  safeSegment(game.key, `game ${index} key`);
  assert(!gameKeys.has(game.key), `duplicate game key ${game.key}`);
  gameKeys.add(game.key);
  assert(typeof game.label === 'string' && game.label.length && byteLength(game.label) <= 48, `game ${game.key} label is invalid`);
  assert(typeof game.description === 'string' && game.description.length && byteLength(game.description) <= 96,
    `game ${game.key} description is invalid`);
  assert(typeof game.duel === 'boolean', `game ${game.key} duel must be boolean`);
  const hasPack = game.packDirectory !== undefined || game.requiredKeys !== undefined;
  if (hasPack) {
    safeSegment(game.packDirectory, `game ${game.key} packDirectory`);
    assert(!packDirectories.has(game.packDirectory), `duplicate pack directory ${game.packDirectory}`);
    assert(Array.isArray(game.requiredKeys) && game.requiredKeys.length > 0, `game ${game.key} requiredKeys are missing`);
    const required = new Set();
    for (const key of game.requiredKeys) {
      assert(typeof key === 'string' && /^[A-Za-z][A-Za-z0-9_]*$/.test(key), `game ${game.key} has an invalid pack key`);
      assert(!required.has(key), `game ${game.key} repeats pack key ${key}`);
      required.add(key);
    }
    packDirectories.set(game.packDirectory, game);
  }
}
assert(gameIds.has(0), 'content manifest must include HA_GAME_NONE');

const parsePack = (path, game) => {
  let text = readUtf8(path);
  assert(!text.includes('\0'), `${relative(root, path)} contains NUL`);
  assert(!text.includes('\r') || !text.replaceAll('\r\n', '').includes('\r'), `${relative(root, path)} contains a bare carriage return`);
  text = text.replaceAll('\r\n', '\n');
  assert(!text.includes(')HAPACK"'), `${relative(root, path)} contains the reserved raw-string delimiter`);
  const lines = text.split('\n');
  assert(lines.every((line) => byteLength(line) <= 1024), `${relative(root, path)} has a line over 1024 UTF-8 bytes`);

  let titleSeen = false;
  let item = new Map();
  let itemCount = 0;
  const required = new Set(game.requiredKeys);
  const finishItem = (lineNumber) => {
    if (!item.size) fail(`${relative(root, path)} has an empty item before line ${lineNumber}`);
    const missing = game.requiredKeys.filter((key) => !item.has(key));
    const extra = [...item.keys()].filter((key) => !required.has(key));
    assert(!missing.length, `${relative(root, path)} item ${itemCount + 1} is missing ${missing.join(', ')}`);
    assert(!extra.length, `${relative(root, path)} item ${itemCount + 1} has unknown key(s) ${extra.join(', ')}`);
    if (game.key === 'trivia') assert(/^[ABCD]$/.test(item.get('Answer')), `${relative(root, path)} has a trivia Answer outside A-D`);
    itemCount += 1;
    item = new Map();
  };

  for (let index = 0; index < lines.length; index += 1) {
    const line = lines[index];
    const lineNumber = index + 1;
    if (!line.trim()) continue;
    if (line === '---') {
      finishItem(lineNumber);
      continue;
    }
    const match = /^([A-Za-z][A-Za-z0-9_]*):[ \t]*(.*)$/.exec(line);
    assert(match, `${relative(root, path)} line ${lineNumber} is malformed`);
    const [, key, value] = match;
    assert(value.length > 0, `${relative(root, path)} line ${lineNumber} has an empty value`);
    if (key === 'Pack') {
      assert(!titleSeen && itemCount === 0 && item.size === 0, `${relative(root, path)} has a misplaced or duplicate Pack line`);
      titleSeen = true;
      continue;
    }
    assert(titleSeen, `${relative(root, path)} must begin with Pack:`);
    assert(!item.has(key), `${relative(root, path)} item ${itemCount + 1} repeats ${key}`);
    item.set(key, value);
  }
  if (item.size) finishItem(lines.length + 1);
  assert(titleSeen, `${relative(root, path)} is missing Pack:`);
  assert(itemCount > 0, `${relative(root, path)} has no content items`);
  return { text, itemCount };
};

const packs = [];
for (const language of content.languages) {
  const entries = readdirSync(language.resolvedRoot, { withFileTypes: true });
  const unknown = entries.filter((entry) => !entry.name.startsWith('.') && !packDirectories.has(entry.name));
  assert(!unknown.length, `${language.root} contains unknown game entries: ${unknown.map((entry) => entry.name).join(', ')}`);
  for (const [packDirectory, game] of packDirectories) {
    const directory = join(language.resolvedRoot, packDirectory);
    assert(existsSync(directory), `${language.root} is missing ${packDirectory}`);
    const stat = lstatSync(directory);
    assert(stat.isDirectory() && !stat.isSymbolicLink(), `${relative(root, directory)} must be a real directory`);
    const entriesInDirectory = readdirSync(directory, { withFileTypes: true });
    for (const entry of entriesInDirectory) {
      assert(entry.isFile() && !entry.isSymbolicLink(), `${relative(root, join(directory, entry.name))} is not a regular file`);
      assert(/^[A-Za-z0-9][A-Za-z0-9._-]*\.txt$/i.test(entry.name), `${relative(root, join(directory, entry.name))} is not a safe .txt pack`);
    }
    const names = entriesInDirectory.map((entry) => entry.name).sort((a, b) => a.localeCompare(b, 'en'));
    assert(names.length > 0, `${relative(root, directory)} has no packs`);
    assert(names.length <= content.maxPacksPerGame,
      `${relative(root, directory)} has ${names.length} packs; maximum is ${content.maxPacksPerGame} (refusing truncation)`);
    for (const name of names) {
      const path = inside(directory, join(directory, name), `pack ${name}`);
      const parsed = parsePack(path, game);
      packs.push({
        gameConstant: game.constant,
        language: language.code,
        fallback: name.slice(0, -4),
        text: parsed.text,
        itemCount: parsed.itemCount,
      });
    }
  }
}

// ---- web bundle -------------------------------------------------------------
const webRoot = join(vendor, 'web');
const webManifestPath = join(webRoot, 'manifest.json');
const webManifest = readJson(webManifestPath);
assert(Array.isArray(webManifest) && webManifest.length > 0, 'vendor/web/manifest.json must be a nonempty array');
const routes = new Set();
const webNames = new Set();
const files = webManifest.map((entry, index) => {
  assertKeys(entry, ['path', 'file', 'mime', 'gzip'], `web manifest entry ${index}`);
  assert(typeof entry.path === 'string' && entry.path.startsWith('/') && !entry.path.includes('..') &&
    !entry.path.includes('\\') && !entry.path.includes('?') && !entry.path.includes('#') && !entry.path.includes('\0'),
  `web manifest entry ${index} route is unsafe`);
  assert(!routes.has(entry.path), `duplicate web route ${entry.path}`);
  routes.add(entry.path);
  safeSegment(entry.file, `web manifest entry ${index} file`);
  assert(entry.file.endsWith('.gz'), `web asset ${entry.file} must end in .gz`);
  assert(!webNames.has(entry.file), `duplicate web asset ${entry.file}`);
  webNames.add(entry.file);
  assert(typeof entry.mime === 'string' && /^[a-z0-9.+-]+\/[a-z0-9.+-]+$/i.test(entry.mime), `web asset ${entry.file} MIME is invalid`);
  assert(entry.gzip === true, `web asset ${entry.file} must declare gzip: true`);
  const path = inside(webRoot, join(webRoot, entry.file), `web asset ${entry.file}`);
  assert(existsSync(path) && lstatSync(path).isFile() && !lstatSync(path).isSymbolicLink(), `web asset ${entry.file} is missing or unsafe`);
  const bytes = readFileSync(path);
  assert(bytes.length > 0, `web asset ${entry.file} is empty`);
  try {
    const expanded = gunzipSync(bytes);
    assert(expanded.length > 0, `web asset ${entry.file} expands to an empty file`);
  } catch (error) {
    fail(`web asset ${entry.file} is not valid gzip: ${error.message}`);
  }
  return { ...entry, bytes, symbol: `HA_WEB_${index}` };
});
const expectedWebEntries = new Set(['manifest.json', ...webNames]);
const unexpectedWebEntries = readdirSync(webRoot).filter((name) => !expectedWebEntries.has(name));
assert(!unexpectedWebEntries.length, `vendor/web contains unmanifested entries: ${unexpectedWebEntries.join(', ')}`);
const totalWeb = files.reduce((total, file) => total + file.bytes.length, 0);
assert(totalWeb <= 60 * 1024, `compressed web bundle is ${totalWeb} bytes; limit is 61440`);

const hex = (buffer) => {
  const lines = [];
  for (let index = 0; index < buffer.length; index += 16) {
    lines.push(`    ${[...buffer.subarray(index, index + 16)]
      .map((byte) => `0x${byte.toString(16).padStart(2, '0')}`)
      .join(',')}`);
  }
  return lines.join(',\n');
};
const totalPacks = packs.reduce((total, pack) => total + byteLength(pack.text), 0);
const totalItems = packs.reduce((total, pack) => total + pack.itemCount, 0);

let bundleHeader = `// GENERATED by tools/gen-assets.mjs -- do not edit by hand.\n// Input contract: tools/content-manifest.json\n// web: ${totalWeb} compressed bytes; packs: ${totalPacks} UTF-8 bytes / ${totalItems} items.\n#pragma once\n#include <Arduino.h>\n#include "ha_proto.h"\n`;
for (const file of files) bundleHeader += `\nstatic const uint8_t ${file.symbol}[] = {\n${hex(file.bytes)}};\n`;
bundleHeader += `
struct HaBakedFile {
    const char* path;
    const char* mime;
    bool gzip;
    const uint8_t* data;
    size_t len;
};

static const HaBakedFile HA_BAKED_FILES[] = {
${files.map((file) => `    {${cppString(file.path)}, ${cppString(file.mime)}, true, ${file.symbol}, sizeof(${file.symbol})},`).join('\n')}
};
static const size_t HA_BAKED_FILE_COUNT = sizeof(HA_BAKED_FILES) / sizeof(HA_BAKED_FILES[0]);

struct HaBakedPack {
    uint8_t game;
    const char* lang;
    const char* fallback;
    const char* text;
};

static const HaBakedPack HA_BAKED_PACKS[] = {
${packs.map((pack) => `    {${pack.gameConstant}, ${cppString(pack.language)}, ${cppString(pack.fallback)}, R"HAPACK(${pack.text})HAPACK"},`).join('\n')}
};
static const size_t HA_BAKED_PACK_COUNT = sizeof(HA_BAKED_PACKS) / sizeof(HA_BAKED_PACKS[0]);
`;

const metadataHeader = `// GENERATED by tools/gen-assets.mjs -- do not edit by hand.\n// Game and language UI metadata comes from tools/content-manifest.json.\n#pragma once\n#include <Arduino.h>\n#include "ha_proto.h"\n\nstruct HaGeneratedGame {\n    uint8_t id;\n    const char* key;\n    const char* label;\n    const char* desc;\n    bool duel;\n};\n\nstatic const HaGeneratedGame HA_GENERATED_GAMES[] = {\n${content.games.map((game) => `    {${game.constant}, ${cppString(game.key)}, ${cppString(game.label)}, ${cppString(game.description)}, ${game.duel ? 'true' : 'false'}},`).join('\n')}\n};\nstatic const size_t HA_GENERATED_GAME_COUNT = sizeof(HA_GENERATED_GAMES) / sizeof(HA_GENERATED_GAMES[0]);\n\nstruct HaGeneratedLanguage {\n    const char* code;\n    const char* label;\n};\n\nstatic const HaGeneratedLanguage HA_GENERATED_LANGUAGES[] = {\n${content.languages.map((language) => `    {${cppString(language.code)}, ${cppString(language.label)}},`).join('\n')}\n};\nstatic const size_t HA_GENERATED_LANGUAGE_COUNT = sizeof(HA_GENERATED_LANGUAGES) / sizeof(HA_GENERATED_LANGUAGES[0]);\n`;

const generated = new Map([
  [join(sketch, 'ha_bundle.h'), bundleHeader],
  [join(sketch, 'ha_metadata.h'), metadataHeader],
]);
for (const name of ['ha_proto.h', 'ha_json.h', 'ha_games.h']) {
  const source = join(vendor, 'engine', name);
  assert(existsSync(source) && lstatSync(source).isFile() && !lstatSync(source).isSymbolicLink(), `missing safe engine header ${name}`);
  const banner = `// GENERATED COPY of vendor/engine/${name} (upstream hotspot-arcade) -- do not edit here.\n` +
    '// Change it upstream, sync a reviewed commit, then run tools/gen-assets.mjs.\n';
  generated.set(join(sketch, name), banner + readUtf8(source));
}

if (checkOnly) {
  const stale = [...generated].filter(([path, expected]) => !existsSync(path) || readUtf8(path) !== expected);
  assert(!stale.length, `generated files are stale: ${stale.map(([path]) => relative(root, path)).join(', ')}`);
} else {
  for (const [path, output] of generated) writeFileSync(path, output, 'utf8');
}

const digest = sha256(Buffer.from([...generated.values()].join('\0'), 'utf8'));
console.log(`${checkOnly ? 'verified' : 'generated'} ${generated.size} file(s); digest ${digest}`);
console.log(`  web: ${files.length} file(s), ${totalWeb} compressed bytes`);
console.log(`  packs: ${packs.length} file(s), ${totalItems} items, ${totalPacks} UTF-8 bytes`);
