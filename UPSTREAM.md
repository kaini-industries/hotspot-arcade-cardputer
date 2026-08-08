# Upstream

Everything under `vendor/` is copied from one committed Git object in
[kaini-industries/hotspot-arcade](https://github.com/kaini-industries/hotspot-arcade), a Kaini Industries-maintained fork of [Tarik Caramanico's original hotspot-arcade](https://github.com/tarikbc/hotspot-arcade) (MIT). Third-party libraries retain their own license files. No file in `vendor/` is edited downstream.

| | |
| --- | --- |
| repository | `https://github.com/kaini-industries/hotspot-arcade` |
| commit | `25ad21523c9a66eb712911545792ee8ebf6281ad` |
| describe | `v1.6.0-3-g25ad215` |
| source tree SHA-256 | `f26daf924bf8229e6da3b51128124e471d1a33925c0f09c2f95baa4b56bbd92c` |
| web bundle | 1 file(s) |
| content packs | 70 pack(s) |
| full file inventory | `UPSTREAM.lock.json` |

Refresh from a clean upstream and downstream checkout:

```sh
node tools/sync-upstream.mjs --repo ../hotspot-arcade --commit <40-character-sha>
node tools/gen-assets.mjs
```

Review `git diff -- vendor UPSTREAM.md UPSTREAM.lock.json` before committing.
