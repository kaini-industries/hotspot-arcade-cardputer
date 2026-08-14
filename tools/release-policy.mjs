#!/usr/bin/env node

// Resolve publication policy before any release workflow can mutate an external
// service. Repository variables are untrusted configuration: only the exact
// documented values are accepted for a final tag, while candidates are always
// non-publishing regardless of repository configuration.

import { resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

export function resolveM5BurnerPublication({ eventName = '', setting = '' } = {}) {
  if (eventName === 'workflow_dispatch') return false;
  if (eventName !== 'push') {
    throw new Error(`unsupported release event: ${eventName || '(missing)'}`);
  }
  if (setting === '' || setting === 'false') return false;
  if (setting === 'true') return true;
  throw new Error('M5BURNER_PUBLISH_ENABLED must be unset, "false", or "true"');
}

function main() {
  try {
    const enabled = resolveM5BurnerPublication({
      eventName: process.env.GITHUB_EVENT_NAME ?? '',
      setting: process.env.M5BURNER_PUBLISH_ENABLED ?? '',
    });
    process.stdout.write(`${enabled}\n`);
  } catch (error) {
    console.error(`release policy validation failed: ${error.message}`);
    process.exitCode = 1;
  }
}

if (resolve(process.argv[1] ?? '') === fileURLToPath(import.meta.url)) main();
