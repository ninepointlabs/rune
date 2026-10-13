#!/usr/bin/env python3
"""Makes the site's screenshots and the narrated demo video from the real app.

    tools/demo/make-media.py               # everything
    tools/demo/make-media.py shots         # screenshots only
    tools/demo/make-media.py guide         # the guide's screenshots, then the guide
    tools/demo/make-media.py video         # video only

Needs a built Rune (build/native/rune), ffmpeg, ImageMagick, Chromium, and an
ElevenLabs API key in $ELEVENLABS_API_KEY or ~/.config/elevenlabs/api_key.
The AI scenes use your own Rune sign-in (the keyring and ~/.config/Rune), so
the AI's edits are real; the account name is blanked in the frame. Narration
is cached in work/tts by its text, so re-runs only pay for changed lines.

Writes docs/images/*.webp (and og.jpg), docs/guide/images/*.webp,
docs/media/rune-demo.mp4 and its poster.
"""

import hashlib
import json
import os
import shutil
import subprocess
import sys
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
WORK = HERE / "work"
OUT = WORK / "out"
DOCS = ROOT / "docs"
FPS = 30
W, H = 1920, 1080


def run(*cmd, **kw):
    print("+", " ".join(str(c) for c in cmd)[:200], flush=True)
    return subprocess.run([str(c) for c in cmd], check=kw.pop("check", True), **kw)


def duration(path):
    out = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", str(path)],
                         check=True, capture_output=True, text=True).stdout
    return float(out.strip())


def build_plugin():
    run("cmake", "-S", HERE, "-B", HERE / "build", "-G", "Ninja", stdout=subprocess.DEVNULL)
    run("cmake", "--build", HERE / "build")


def scene(name, scale=1, tries=1):
    """Plays scenes/<name>.json; retries a take whose `expect` failed (exit 4)."""
    for attempt in range(1, tries + 1):
        result = run(HERE / "run-scene.sh", HERE / "scenes" / f"{name}.json", scale, check=False,
                     stdout=subprocess.DEVNULL)
        if result.returncode == 0:
            return
        if result.returncode != 4:
            sys.exit(f"scene {name} failed ({result.returncode})")
        print(f"  take {attempt} of {name} was not usable; again", flush=True)
    sys.exit(f"scene {name}: no usable take in {tries}")


def prepare():
    """The demo documents: a novel with notes, an essay with sources."""
    OUT.mkdir(parents=True, exist_ok=True)
    conf = WORK / "home/.config/Rune"
    conf.mkdir(parents=True, exist_ok=True)
    # Your AI sign-in state (the secrets stay in the keyring), autosave off.
    mine = Path.home() / ".config/Rune/Rune.conf"
    text = mine.read_text() if mine.exists() else "[editor]\nautosave=false\n"
    (conf / "Rune.conf").write_text(text.replace("autosave=true", "autosave=false"))
    scene("build-novel")
    scene("build-essay")


# ---------------------------------------------------------------- screenshots

def shots():
    prepare()
    scene("shots-book", scale=2, tries=4)
    scene("shots-cite", scale=2)
    dest = DOCS / "images"
    dest.mkdir(parents=True, exist_ok=True)
    for name in ["hero", "outline", "notes", "ai-edit", "citations", "palette", "headers"]:
        src = OUT / f"shot-{name}.png"
        # 2× the site's 1040px column, for sharp text on HiDPI screens.
        run("magick", src, "-resize", "2080x>", "-quality", "86", "-define", "webp:method=6", dest / f"{name}.webp")
    # The link preview (og:image): the hero, cropped to 1200×630.
    run("magick", OUT / "shot-hero.png", "-resize", "1200x", "-gravity", "north", "-crop", "1200x630+0+0", "+repage",
        "-quality", "85", dest / "og.jpg")


def guide():
    """The guide's screenshots (docs/guide/images), then the guide itself."""
    prepare()
    scene("guide-book", scale=2)
    scene("guide-cite", scale=2)
    scene("guide-ai", scale=2, tries=4)
    dest = DOCS / "guide" / "images"
    dest.mkdir(parents=True, exist_ok=True)
    for src in sorted(OUT.glob("g-*.png")):
        # Whole-window shots at 2× the column; close-ups at their own size.
        run("magick", src, "-resize", "2080x>", "-quality", "86", "-define", "webp:method=6",
            dest / (src.stem[2:] + ".webp"))
    run(sys.executable, ROOT / "tools" / "guide" / "build.py")


# ---------------------------------------------------------------------- video

def chromium():
    for name in ["chromium", "google-chrome-stable", "chromium-browser"]:
        if shutil.which(name):
            return name
    sys.exit("needs Chromium for the title cards")


def card(spec, path, transparent=False):
    args = [chromium(), "--headless=new", "--disable-gpu", "--hide-scrollbars", f"--window-size={W},{H}",
            "--virtual-time-budget=5000", f"--screenshot={path}", f"file://{HERE / 'cards/card.html'}#{spec}"]
    if transparent:
        args.insert(1, "--default-background-color=00000000")
    run(*args, stderr=subprocess.DEVNULL)


def api_key():
    key = os.environ.get("ELEVENLABS_API_KEY")
    if not key:
        key = (Path.home() / ".config/elevenlabs/api_key").read_text().strip()
    return key


def narrate(narration):
    """One MP3 per segment, cached by voice, model and text."""
    tts = WORK / "tts"
    tts.mkdir(parents=True, exist_ok=True)
    segments = narration["segments"]
    paths = []
    for i, seg in enumerate(segments):
        body = {
            "text": seg["text"],
            "model_id": narration["model"],
            "voice_settings": {"stability": 0.5, "similarity_boost": 0.8, "style": 0.15, "use_speaker_boost": True},
            # Neighbouring lines, so the read flows from one to the next.
            "previous_text": segments[i - 1]["text"] if i > 0 else "",
            "next_text": segments[i + 1]["text"] if i + 1 < len(segments) else "",
        }
        digest = hashlib.sha256(json.dumps([narration["voice"], body], sort_keys=True).encode()).hexdigest()[:16]
        path = tts / f"{seg['clip']}-{digest}.mp3"
        if not path.exists():
            print(f"  narrating {seg['clip']}", flush=True)
            req = urllib.request.Request(
                f"https://api.elevenlabs.io/v1/text-to-speech/{narration['voice']}?output_format=mp3_44100_128",
                data=json.dumps(body).encode(), method="POST",
                headers={"xi-api-key": api_key(), "Content-Type": "application/json", "Accept": "audio/mpeg"})
            with urllib.request.urlopen(req, timeout=120) as resp:
                path.write_bytes(resp.read())
        paths.append(path)
    return paths


def segment(seg, audio, out, fade_out):
    """One finished piece: the clip (or card), its narration and caption."""
    lead, tail = 0.35, 0.7
    spoken = duration(audio)
    inputs, filters = [], []
    if "card" in seg:
        png = WORK / "cards" / f"{seg['card']}.png"
        length = lead + spoken + (2.4 if seg["card"] == "end" else 1.2)
        inputs += ["-loop", "1", "-framerate", FPS, "-t", f"{length:.3f}", "-i", png]
        # A slow push in, so the card isn't a still.
        filters.append(f"[0:v]scale={W * 2}:-1,zoompan=z='1+0.04*on/({length:.3f}*{FPS})':d=1:s={W}x{H}:fps={FPS}"
                       f":x='iw/2-(iw/zoom/2)':y='ih/2-(ih/zoom/2)'[v0]")
    else:
        clip = OUT / f"{seg['clip']}.mp4"
        length = max(duration(clip), lead + spoken + tail)
        inputs += ["-i", clip]
        # Hold the last frame if the narration runs longer than the clip.
        filters.append(f"[0:v]tpad=stop_mode=clone:stop_duration={length:.3f},trim=duration={length:.3f},"
                       f"setpts=PTS-STARTPTS[v0]")
    inputs += ["-i", audio]
    last = "v0"
    if "caption" in seg:
        png = WORK / "cards" / f"caption-{seg['clip']}.png"
        inputs += ["-loop", "1", "-framerate", FPS, "-t", f"{length:.3f}", "-i", png]
        filters.append("[2:v]format=rgba,fade=in:st=0.3:d=0.4:alpha=1,fade=out:st=3.4:d=0.5:alpha=1[cap]")
        filters.append("[v0][cap]overlay=0:0:shortest=1[v1]")
        last = "v1"
    fades = []
    if seg.get("fade_in") or seg.get("card") == "title":
        fades.append(f"fade=in:st=0:d={0.8 if seg.get('card') == 'title' else 0.35}")
    if fade_out:
        d = 1.2 if seg.get("card") == "end" else 0.35
        fades.append(f"fade=out:st={length - d:.3f}:d={d}")
    filters.append(f"[{last}]" + ",".join(fades + ["format=yuv420p"]) + "[v]")
    delay = int(lead * 1000)
    filters.append(f"[1:a]adelay={delay}|{delay},apad,atrim=duration={length:.3f},aresample=48000[a]")
    run("ffmpeg", "-loglevel", "error", "-y", *inputs, "-filter_complex", ";".join(filters),
        "-map", "[v]", "-map", "[a]", "-r", FPS, "-c:v", "libx264", "-preset", "slow", "-crf", "12",
        "-c:a", "pcm_s16le", "-t", f"{length:.3f}", out)


def video():
    prepare()
    for name in ["video-book", "video-ai", "video-cite", "video-palette"]:
        scene(name, scale=1.5, tries=4)
    narration = json.loads((HERE / "narration.json").read_text())
    segments = narration["segments"]
    cards = WORK / "cards"
    cards.mkdir(parents=True, exist_ok=True)
    card("title", cards / "title.png")
    card("end", cards / "end.png")
    for seg in segments:
        if "caption" in seg:
            card(f"caption={seg['caption']}", cards / f"caption-{seg['clip']}.png", transparent=True)
    audio = narrate(narration)
    pieces = WORK / "pieces"
    pieces.mkdir(parents=True, exist_ok=True)
    listing = []
    for i, seg in enumerate(segments):
        nxt = segments[i + 1] if i + 1 < len(segments) else None
        fade_out = nxt is None or bool(nxt.get("fade_in")) or "card" in seg
        path = pieces / f"{seg['clip']}.mkv"
        segment(seg, audio[i], path, fade_out)
        listing.append(f"file '{path}'")
    (pieces / "list.txt").write_text("\n".join(listing) + "\n")
    joined = WORK / "joined.mkv"
    run("ffmpeg", "-loglevel", "error", "-y", "-f", "concat", "-safe", "0", "-i", pieces / "list.txt", "-c", "copy", joined)
    media = DOCS / "media"
    media.mkdir(parents=True, exist_ok=True)
    final = media / "rune-demo.mp4"
    run("ffmpeg", "-loglevel", "error", "-y", "-i", joined,
        "-c:v", "libx264", "-preset", "slow", "-crf", "23", "-pix_fmt", "yuv420p", "-movflags", "+faststart",
        "-af", "loudnorm=I=-16:TP=-1.5:LRA=11", "-c:a", "aac", "-b:a", "160k", "-ar", "48000", final)
    run("magick", cards / "title.png", "-resize", "1600x", "-quality", "82", media / "rune-demo-poster.jpg")
    print(f"{final}: {duration(final):.1f}s, {final.stat().st_size / 1e6:.1f} MB")


if __name__ == "__main__":
    what = sys.argv[1] if len(sys.argv) > 1 else "all"
    build_plugin()
    if what in ("all", "shots"):
        shots()
    if what in ("all", "guide"):
        guide()
    if what in ("all", "video"):
        video()
