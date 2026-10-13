# Demo media

The screenshots on rune.ninepointlabs.com and the narrated demo video are
made from the real app by `make-media.py`:

```sh
cmake --build build                  # Rune itself
tools/demo/make-media.py             # screenshots and video
tools/demo/make-media.py shots       # or just the site's screenshots
tools/demo/make-media.py guide       # or the guide's screenshots, then the guide
tools/demo/make-media.py video       # or just the video
```

It writes `docs/images/*.webp`, `docs/images/og.jpg`, `docs/guide/images/*.webp`,
`docs/media/rune-demo.mp4` and its poster. The guide's text is in
`tools/guide/pages/`; `tools/guide/build.py` turns it into `docs/guide/`.

**How.** `RuneDemo.cpp` is a Qt generic plugin. `run-scene.sh` loads it into
the unmodified `rune` binary (`-plugin runedemo`), offscreen, where it plays a
scene script from `scenes/` and records it. The plugin uses real key and mouse
events and the QML API, the way `--auto-test` does, and pipes frames to ffmpeg.
The step types are documented at the top of `RuneDemo.cpp`. The app uses a
scratch HOME in `work/`, with the site's palette as its Omarchy theme
(`theme/colors.toml`), so your own theme and settings are untouched.

**The AI is real.** The AI scenes use your own Rune sign-in: the settings are
copied from `~/.config/Rune` and the secrets come from the keyring. The account
name is blanked in the frame. A take whose AI result fails its `expect` check
is recorded again, up to four times.

**Narration** is ElevenLabs (`narration.json`: George,
`eleven_multilingual_v2`), with the key taken from `$ELEVENLABS_API_KEY` or
`~/.config/elevenlabs/api_key`. Each line is cached in `work/tts` by its
text, so changing one line only pays for that line. Title and end cards and
the captions are `cards/card.html`, rendered by headless Chromium.

Needs ffmpeg, ImageMagick, Chromium and Qt 6's Test module.
