# Upstream

Everything under `vendor/` is copied from one committed Git object in
[kaini-industries/hotspot-arcade](https://github.com/kaini-industries/hotspot-arcade), a Kaini Industries-maintained fork of [Tarik Caramanico's original hotspot-arcade](https://github.com/tarikbc/hotspot-arcade) (MIT). Third-party libraries retain their own license files. No file in `vendor/` is edited downstream.

| | |
| --- | --- |
| repository | `https://github.com/kaini-industries/hotspot-arcade` |
| commit | `b7b4b235ab07ed08c205f4bb451b153d1508bf4d` |
| describe | `v1.8.0-10-gb7b4b23` |
| source tree SHA-256 | `1830d3da68294993a6cd34646165f153f1a313eb30a9f0309d0743ae10af9db0` |
| web bundle | 1 file(s) |
| content packs | 87 pack(s) |
| full file inventory | `UPSTREAM.lock.json` |

Refresh from a clean upstream and downstream checkout:

```sh
node tools/sync-upstream.mjs --repo ../hotspot-arcade --commit <40-character-sha>
node tools/gen-assets.mjs
```

Review `git diff -- vendor UPSTREAM.md UPSTREAM.lock.json` before committing.
