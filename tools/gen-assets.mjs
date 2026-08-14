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
const crc32 = (bytes) => {
  let crc = 0xFFFFFFFF;
  for (const byte of bytes) {
    crc ^= byte;
    for (let bit = 0; bit < 8; bit += 1)
      crc = (crc >>> 1) ^ (crc & 1 ? 0xEDB88320 : 0);
  }
  return (crc ^ 0xFFFFFFFF) >>> 0;
};

// ---- reviewed content contract ---------------------------------------------
const contentPath = join(root, 'tools', 'content-manifest.json');
const content = readJson(contentPath);
assertKeys(content, ['schema', 'maxPacksPerGame', 'languages', 'games'], 'content manifest');
assert(content.schema === 2, 'content manifest schema must be 2');
assert(Number.isSafeInteger(content.maxPacksPerGame) && content.maxPacksPerGame > 0 && content.maxPacksPerGame <= 16,
  'maxPacksPerGame must be an integer from 1 to 16');
assert(Array.isArray(content.languages) && content.languages.length > 0, 'content manifest needs languages');
assert(Array.isArray(content.games) && content.games.length > 0, 'content manifest needs games');

const languageCodes = new Set();
const languagesByCode = new Map();
for (const [index, language] of content.languages.entries()) {
  assertKeys(language, ['code', 'label', 'root', 'fallback'], `language ${index}`);
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
  if (language.fallback !== undefined) {
    assert(typeof language.fallback === 'string' && /^[a-z]{2}(?:-[a-z]{2})?$/.test(language.fallback),
      `language ${language.code} fallback is invalid`);
  }
  languagesByCode.set(language.code, language);
}
assert(content.languages[0].code === 'en' && content.languages[0].fallback === undefined,
  'the first language must be en without a fallback');
for (const [index, language] of content.languages.entries()) {
  if (language.fallback === undefined) continue;
  const fallbackIndex = content.languages.findIndex((candidate) => candidate.code === language.fallback);
  assert(fallbackIndex >= 0, `language ${language.code} fallback ${language.fallback} is unknown`);
  assert(fallbackIndex < index, `language ${language.code} fallback must precede it`);
}

const gameIds = new Set();
const gameKeys = new Set();
const gameConstants = new Set();
const packDirectories = new Map();
for (const [index, game] of content.games.entries()) {
  assertKeys(game, [
    'id', 'constant', 'key', 'label', 'description', 'duel', 'packDirectory',
    'requiredKeys', 'oneOfKeys', 'repeatableKeys', 'minItemsPerPack',
    'maxItemsPerPack', 'maxPacks', 'packKeyLimits', 'keyByteLimits',
  ], `game ${index}`);
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
  const hasPack = game.packDirectory !== undefined || game.requiredKeys !== undefined ||
    game.oneOfKeys !== undefined || game.repeatableKeys !== undefined ||
    game.minItemsPerPack !== undefined || game.maxItemsPerPack !== undefined ||
    game.maxPacks !== undefined || game.packKeyLimits !== undefined ||
    game.keyByteLimits !== undefined;
  if (hasPack) {
    safeSegment(game.packDirectory, `game ${game.key} packDirectory`);
    assert(!packDirectories.has(game.packDirectory), `duplicate pack directory ${game.packDirectory}`);
    assert(Array.isArray(game.requiredKeys), `game ${game.key} requiredKeys are missing`);
    game.oneOfKeys = game.oneOfKeys ?? [];
    assert(Array.isArray(game.oneOfKeys), `game ${game.key} oneOfKeys must be an array`);
    assert(game.requiredKeys.length > 0 || game.oneOfKeys.length > 0,
      `game ${game.key} needs requiredKeys or oneOfKeys`);
    const allowed = new Set();
    for (const key of game.requiredKeys) {
      assert(typeof key === 'string' && /^[A-Za-z][A-Za-z0-9_]*$/.test(key), `game ${game.key} has an invalid pack key`);
      assert(!allowed.has(key), `game ${game.key} repeats pack key ${key}`);
      allowed.add(key);
    }
    for (const key of game.oneOfKeys) {
      assert(typeof key === 'string' && /^[A-Za-z][A-Za-z0-9_]*$/.test(key), `game ${game.key} has an invalid one-of key`);
      assert(!allowed.has(key), `game ${game.key} repeats pack key ${key}`);
      allowed.add(key);
    }
    assert(game.keyByteLimits !== undefined, `game ${game.key} keyByteLimits are missing`);
    assertKeys(game.keyByteLimits, [...allowed], `game ${game.key} keyByteLimits`);
    const missingByteLimits = [...allowed].filter((key) => !Object.hasOwn(game.keyByteLimits, key));
    assert(!missingByteLimits.length,
      `game ${game.key} keyByteLimits are missing ${missingByteLimits.join(', ')}`);
    for (const [key, maximum] of Object.entries(game.keyByteLimits)) {
      assert(Number.isSafeInteger(maximum) && maximum > 0 && maximum <= 255,
        `game ${game.key} keyByteLimits.${key} is invalid`);
    }
    game.repeatableKeys = game.repeatableKeys ?? {};
    assertKeys(game.repeatableKeys, game.requiredKeys, `game ${game.key} repeatableKeys`);
    for (const [key, maximum] of Object.entries(game.repeatableKeys)) {
      assert(Number.isSafeInteger(maximum) && maximum >= 2 && maximum <= 64,
        `game ${game.key} repeatable key ${key} maximum is invalid`);
    }
    assert(Number.isSafeInteger(game.minItemsPerPack) && game.minItemsPerPack > 0,
      `game ${game.key} minItemsPerPack is invalid`);
    assert(Number.isSafeInteger(game.maxItemsPerPack) && game.maxItemsPerPack >= game.minItemsPerPack &&
      game.maxItemsPerPack <= 255, `game ${game.key} maxItemsPerPack is invalid`);
    game.maxPacks = game.maxPacks ?? content.maxPacksPerGame;
    assert(Number.isSafeInteger(game.maxPacks) && game.maxPacks > 0 && game.maxPacks <= content.maxPacksPerGame,
      `game ${game.key} maxPacks is invalid`);
    game.packKeyLimits = game.packKeyLimits ?? {};
    assertKeys(game.packKeyLimits, [...allowed], `game ${game.key} packKeyLimits`);
    for (const [key, limits] of Object.entries(game.packKeyLimits)) {
      assertKeys(limits, ['min', 'max'], `game ${game.key} packKeyLimits.${key}`);
      assert(Number.isSafeInteger(limits.min) && limits.min >= 0 &&
        Number.isSafeInteger(limits.max) && limits.max >= limits.min &&
        limits.max <= game.maxItemsPerPack * (game.repeatableKeys[key] ?? 1),
      `game ${game.key} packKeyLimits.${key} is invalid`);
    }
    game.allowedKeys = allowed;
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
  let item = [];
  let itemCount = 0;
  const packKeyCounts = new Map([...game.allowedKeys].map((key) => [key, 0]));
  const finishItem = (lineNumber) => {
    if (!item.length) fail(`${relative(root, path)} has an empty item before line ${lineNumber}`);
    const counts = new Map();
    for (const [key] of item) counts.set(key, (counts.get(key) ?? 0) + 1);
    const missing = game.requiredKeys.filter((key) => !counts.has(key));
    const extra = [...new Set(item.map(([key]) => key))].filter((key) => !game.allowedKeys.has(key));
    assert(!missing.length, `${relative(root, path)} item ${itemCount + 1} is missing ${missing.join(', ')}`);
    assert(!extra.length, `${relative(root, path)} item ${itemCount + 1} has unknown key(s) ${extra.join(', ')}`);
    for (const key of game.requiredKeys) {
      const count = counts.get(key) ?? 0;
      const maximum = game.repeatableKeys[key] ?? 1;
      assert(count <= maximum,
        `${relative(root, path)} item ${itemCount + 1} repeats ${key} more than ${maximum} time(s)`);
    }
    if (game.oneOfKeys.length) {
      const selected = game.oneOfKeys.filter((key) => counts.has(key));
      assert(selected.length === 1 && counts.get(selected[0]) === 1,
        `${relative(root, path)} item ${itemCount + 1} must contain exactly one of ${game.oneOfKeys.join(', ')}`);
    }
    if (game.key === 'trivia') {
      const answer = item.find(([key]) => key === 'Answer')?.[1];
      assert(/^[ABCD]$/.test(answer), `${relative(root, path)} has a trivia Answer outside A-D`);
    }
    for (const [key, count] of counts)
      if (packKeyCounts.has(key)) packKeyCounts.set(key, packKeyCounts.get(key) + count);
    itemCount += 1;
    assert(itemCount <= game.maxItemsPerPack,
      `${relative(root, path)} has more than ${game.maxItemsPerPack} items`);
    item = [];
  };

  for (let index = 0; index < lines.length; index += 1) {
    const line = lines[index];
    const lineNumber = index + 1;
    if (!line.trim() || line === '---') {
      if (item.length) finishItem(lineNumber);
      continue;
    }
    const match = /^([A-Za-z][A-Za-z0-9_]*): (.*)$/.exec(line);
    assert(match, `${relative(root, path)} line ${lineNumber} is malformed`);
    const [, key, value] = match;
    assert(value.length > 0, `${relative(root, path)} line ${lineNumber} has an empty value`);
    assert(!/^[ \t]|[ \t]$/.test(value),
      `${relative(root, path)} line ${lineNumber} has leading or trailing whitespace`);
    assert(!/[\x00-\x1F\x7F]/.test(value),
      `${relative(root, path)} line ${lineNumber} contains an ASCII control byte`);
    if (key === 'Pack') {
      assert(!titleSeen && itemCount === 0 && item.length === 0, `${relative(root, path)} has a misplaced or duplicate Pack line`);
      assert(byteLength(value) <= 63,
        `${relative(root, path)} line ${lineNumber} pack name exceeds the engine's 63-byte limit`);
      titleSeen = true;
      continue;
    }
    assert(titleSeen, `${relative(root, path)} must begin with Pack:`);
    assert(byteLength(value) <= 255,
      `${relative(root, path)} line ${lineNumber} value exceeds the engine's 255-byte limit`);
    const keyByteLimit = game.keyByteLimits[key];
    if (keyByteLimit !== undefined) {
      const valueBytes = byteLength(value);
      assert(valueBytes <= keyByteLimit,
        `${relative(root, path)} line ${lineNumber} ${key} value is ${valueBytes} UTF-8 bytes; maximum is ${keyByteLimit}`);
    }
    item.push([key, value]);
  }
  if (item.length) finishItem(lines.length + 1);
  assert(titleSeen, `${relative(root, path)} is missing Pack:`);
  assert(itemCount >= game.minItemsPerPack,
    `${relative(root, path)} has ${itemCount} items; minimum is ${game.minItemsPerPack}`);
  for (const [key, limits] of Object.entries(game.packKeyLimits)) {
    const count = packKeyCounts.get(key) ?? 0;
    assert(count >= limits.min && count <= limits.max,
      `${relative(root, path)} has ${count} ${key} record(s); expected ${limits.min}..${limits.max}`);
  }
  return { text, itemCount };
};

const packs = [];
for (const language of content.languages) {
  const entries = readdirSync(language.resolvedRoot, { withFileTypes: true });
  const unknown = entries.filter((entry) => !entry.name.startsWith('.') && !packDirectories.has(entry.name));
  assert(!unknown.length, `${language.root} contains unknown game entries: ${unknown.map((entry) => entry.name).join(', ')}`);
  for (const [packDirectory, game] of packDirectories) {
    const directory = join(language.resolvedRoot, packDirectory);
    if (!existsSync(directory)) {
      assert(language.fallback !== undefined,
        `${language.root} is missing ${packDirectory} and has no fallback`);
      continue;
    }
    const stat = lstatSync(directory);
    assert(stat.isDirectory() && !stat.isSymbolicLink(), `${relative(root, directory)} must be a real directory`);
    const entriesInDirectory = readdirSync(directory, { withFileTypes: true });
    for (const entry of entriesInDirectory) {
      assert(entry.isFile() && !entry.isSymbolicLink(), `${relative(root, join(directory, entry.name))} is not a regular file`);
      assert(/^[A-Za-z0-9][A-Za-z0-9._-]*\.txt$/i.test(entry.name), `${relative(root, join(directory, entry.name))} is not a safe .txt pack`);
    }
    const names = entriesInDirectory.map((entry) => entry.name).sort((a, b) => a.localeCompare(b, 'en'));
    assert(names.length > 0, `${relative(root, directory)} has no packs`);
    assert(names.length <= game.maxPacks,
      `${relative(root, directory)} has ${names.length} packs; maximum is ${game.maxPacks} (refusing truncation)`);
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
const packCoverage = new Set(packs.map((pack) => `${pack.language}\0${pack.gameConstant}`));
for (const language of content.languages) {
  for (const game of packDirectories.values()) {
    let candidate = language;
    while (candidate && !packCoverage.has(`${candidate.code}\0${game.constant}`))
      candidate = candidate.fallback ? languagesByCode.get(candidate.fallback) : null;
    assert(candidate,
      `language ${language.code} has no ${game.packDirectory} packs in its fallback chain`);
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
  assertKeys(entry, ['path', 'file', 'mime', 'gzip', 'crc'], `web manifest entry ${index}`);
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
  if (entry.crc !== undefined) {
    assert(Number.isSafeInteger(entry.crc) && entry.crc >= 0 && entry.crc <= 0xFFFFFFFF,
      `web asset ${entry.file} CRC is invalid`);
    assert(crc32(bytes) === entry.crc,
      `web asset ${entry.file} CRC does not match its compressed bytes`);
  }
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
const maxWebBytes = 72 * 1024;
assert(totalWeb <= maxWebBytes,
  `compressed web bundle is ${totalWeb} bytes; limit is ${maxWebBytes}`);

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

const metadataHeader = `// GENERATED by tools/gen-assets.mjs -- do not edit by hand.\n// Game and language UI metadata comes from tools/content-manifest.json.\n#pragma once\n#include <Arduino.h>\n#include "ha_proto.h"\n\nstruct HaGeneratedGame {\n    uint8_t id;\n    const char* key;\n    const char* label;\n    const char* desc;\n    bool duel;\n};\n\nstatic const HaGeneratedGame HA_GENERATED_GAMES[] = {\n${content.games.map((game) => `    {${game.constant}, ${cppString(game.key)}, ${cppString(game.label)}, ${cppString(game.description)}, ${game.duel ? 'true' : 'false'}},`).join('\n')}\n};\nstatic const size_t HA_GENERATED_GAME_COUNT = sizeof(HA_GENERATED_GAMES) / sizeof(HA_GENERATED_GAMES[0]);\n${content.games.map((game) => `static_assert(${game.constant} == ${game.id}, ${cppString(`manifest ID mismatch for ${game.key}`)});`).join('\n')}\n\nstruct HaGeneratedLanguage {\n    const char* code;\n    const char* label;\n    const char* fallback;\n};\n\nstatic const HaGeneratedLanguage HA_GENERATED_LANGUAGES[] = {\n${content.languages.map((language) => `    {${cppString(language.code)}, ${cppString(language.label)}, ${cppString(language.fallback ?? '')}},`).join('\n')}\n};\nstatic const size_t HA_GENERATED_LANGUAGE_COUNT = sizeof(HA_GENERATED_LANGUAGES) / sizeof(HA_GENERATED_LANGUAGES[0]);\n`;

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
