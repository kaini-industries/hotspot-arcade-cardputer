import test from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { existsSync, readFileSync } from 'node:fs';
import { join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = resolve(fileURLToPath(new URL('..', import.meta.url)));

test('the reviewed Spectrum Wild Card pack remains lossless and English-only', () => {
  const relative = join('spectrum', 'wildcard.txt');
  const english = join(root, 'vendor', 'packs', 'en', relative);
  const bytes = readFileSync(english);
  assert.equal(
    createHash('sha256').update(bytes).digest('hex'),
    'a78c39496e98cff5266d5b3351e10b4607be12d4cf13a455d0718627c602ae52',
  );
  assert.equal(existsSync(join(root, 'vendor', 'packs', 'de', relative)), false);
  assert.equal(existsSync(join(root, 'vendor', 'packs', 'pt-br', relative)), false);

  const records = bytes.toString('utf8').trimEnd().split(/\n---\n/);
  assert.equal(records.length, 32);
  assert.match(records[0], /^Pack: Wild Card\nLeft: .+\nRight: .+$/);
  for (const record of records.slice(1))
    assert.match(record, /^Left: .+\nRight: .+$/);
});
