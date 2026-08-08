# Upstream

Everything under `vendor/` is copied from one committed Git object in
[tarikbc/hotspot-arcade](https://github.com/tarikbc/hotspot-arcade) (MIT, Tarik Caramanico). Third-party libraries retain their own license files. No file in `vendor/` is edited downstream.

| | |
| --- | --- |
| repository | `https://github.com/tarikbc/hotspot-arcade` |
| commit | `aad6e8ffa03a125aa4d6be14030a3f887d5cde05` |
| describe | `v1.6.0-7-gaad6e8f` |
| source tree SHA-256 | `48df7455a097376d968a3676acdd6f99ef84c8c214341866757aaeb08835c606` |
| web bundle | 1 file(s) |
| content packs | 70 pack(s) |
| full file inventory | `UPSTREAM.lock.json` |

Refresh from a clean upstream and downstream checkout:

```sh
node tools/sync-upstream.mjs --repo ../hotspot-arcade --commit <40-character-sha>
node tools/gen-assets.mjs
```

Review `git diff -- vendor UPSTREAM.md UPSTREAM.lock.json` before committing.
