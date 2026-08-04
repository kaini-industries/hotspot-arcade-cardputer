# Upstream

Everything under `vendor/` is copied from one committed Git object in
[tarikbc/hotspot-arcade](https://github.com/tarikbc/hotspot-arcade) (MIT, Tarik Caramanico). Third-party libraries retain their own license files. No file in `vendor/` is edited downstream.

| | |
| --- | --- |
| repository | `https://github.com/tarikbc/hotspot-arcade` |
| commit | `4204458b5231a4e5514aae271eeee2f6ab456672` |
| describe | `v1.6.0-6-g4204458` |
| source tree SHA-256 | `aff7e366adf91a88e4353bb4c3a445db4ce902e11c1e83bbd3f1c1fb7ee961db` |
| web bundle | 1 file(s) |
| content packs | 70 pack(s) |
| full file inventory | `UPSTREAM.lock.json` |

Refresh from a clean upstream and downstream checkout:

```sh
node tools/sync-upstream.mjs --repo ../hotspot-arcade --commit <40-character-sha>
node tools/gen-assets.mjs
```

Review `git diff -- vendor UPSTREAM.md UPSTREAM.lock.json` before committing.
