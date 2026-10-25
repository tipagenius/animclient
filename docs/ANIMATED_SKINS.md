# Animated skins

BestClient can play animated skins. An animated skin is a normal 0.6 skin sheet (same layout as `default.png`) stored as an animation, every frame is a full skin sheet.

The skin name sent to the server does not change, so players without BestClient (or without the animated file) see the regular static skin with that name.

## Formats

| Format | File | Notes |
| --- | --- | --- |
| APNG | `skins/<name>.apng` or `skins/<name>.png` | full alpha, recommended |
| GIF | `skins/<name>.gif` | 1-bit transparency only, edges look jagged |
| WebP | `skins/<name>.webp` | needs an FFmpeg with the animated WebP decoder (8.0 or newer) |

APNGs are decoded with libpng and work in every build. GIF and WebP are decoded with FFmpeg, so builds without `VIDEORECORDER` only support APNG.

Frame durations are used exactly as stored in the file. Like in browsers, delays of 10 ms or less are shown for 100 ms.

If both `<name>.png` and an animated file exist, the PNG is used in places that never animate and the animation is played on tees. A skin can also exist only as an animated file, then its first frame is used as the static skin.

## Limits

Every frame is uploaded as 24 small textures (12 skin parts, original and colorable), so animations are limited:

- `bc_anim_skins` (default `1`): play animated skins at all.
- `bc_anim_skins_max_frames` (default `100`): longer animations are reduced by skipping frames evenly, the total duration stays the same.
- `bc_anim_skins_max_width` (default `256`): frames are downscaled to this width.
- `bc_anim_skins_max_duration` (default `15000` ms): longer animations only show their first frame.

## Download server

Set `bc_anim_skins_download_url` to share animated skins between BestClient users, for example `https://example.com/animskins/`. For every loaded skin the client requests `<url><skin name>` (no file extension, the format is detected from the content) and caches the result in `downloadedskins/<name>.anim`.

- `200`: the animation is decoded and replaces the static skin.
- `304`: the cached file is up to date (the client sends `If-Modified-Since`).
- `404`: there is no animated version, a stale cache file is removed.

Each skin is requested once until the skin list is refreshed, skins with a local animated file in `skins/` are never requested. Only HTTPS is allowed unless `http_allow_insecure 1` is set.
