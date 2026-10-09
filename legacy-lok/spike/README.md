# Rune spike: LibreOfficeKit tiled rendering → QML

**Result: architecture validated.** LibreOfficeKit opens `test.docx` in-process
and renders page 0 with `paintTile`. A `QQuickImageProvider` passes the bitmap
to a Qt Quick window that uses the active Omarchy palette.

## Build & run

```sh
cmake -S . -B build -G Ninja
cmake --build build
./build/rune_spike                      # window, renders ./test.docx
./build/rune_spike other.docx           # any document
./build/rune_spike --png /tmp/p0.png    # headless: render page 0 to PNG and exit
./build/rune_spike --scale 2            # tile resolution multiplier (default 1.5)
```

You don't need to source `setsdkenv_unix.sh` because the SDK (UNO/IDL tooling)
isn't used. `lok_cpp_init()` is given `/usr/lib/libreoffice/program` and
sets up LibreOffice from there.

## Files

- `main.cpp`: loads the document through LOK, reads the page rect, calls `paintTile`, sets up the image provider and Omarchy theme
- `Main.qml`: dark window, page shown centered and scrollable, status bar with timings
- `test.docx`: generated with `soffice --headless --convert-to docx` from a small HTML file (headings, bold/italic, list, table, colored/underlined/monospace text)

## Measured (test.docx, A4, scale 1.5)

| Step | Time |
|---|---|
| `lok_cpp_init` + `documentLoad` + `initializeForRendering` | ~1.4 s (cold) |
| `paintTile` of the full page, 1191×1684 px | 10–17 ms |

## Findings for the real build

1. **Don't link `liblibreofficekitgtk.so`.** That library is the GTK widget
   wrapper. On the client side LOK is header-only: `LibreOfficeKitInit.h`
   `dlopen()`s `libsofficeapp.so` at runtime. The spike only needs the
   include path plus `${CMAKE_DL_LIBS}`. Linking the GTK lib would bring GTK
   into a Qt process.
2. **Shutdown crash in LO 26.8 (worked around).** `lok::Document` and
   `lok::Office` destroy cleanly. After that, the normal `exit()` segfaults in
   `libswlo` static destructors: `vcl_SystemClipboard_get_implementation` →
   `desktop_LOKClipboard_get_implementation` runs after LOK teardown (SIGSEGV,
   then abort). Keeping `Office` alive until exit makes it worse
   ("Unspecified Application Error"). The workaround is `fflush` +
   `std::_Exit()` once Qt objects are destroyed. Any production code needs the
   same fix, and it's one more reason to run LOK in its own engine process
   (see 4).
3. **Page geometry.** `getPartPageRectangles()` returns `"x, y, w, h; …"` in
   twips for Writer. Page 0 is `0,0 11906×16838` here (A4). `getDocumentSize`
   adds the inter-page gap and margins (`12474×17406`). Use twips/15 × scale
   for pixels at 96 DPI.
4. **Pixel format.** `getTileMode()` returns `LOK_TILEMODE_BGRA`. That maps
   with no copy-conversion to `QImage::Format_ARGB32_Premultiplied` on
   little-endian.
5. **VCL plugin.** `SAL_USE_VCLPLUGIN=svp` is set before init so LO stays
   headless and never loads its own Qt/GTK VCL plugin into our Qt process.
   LOK runs before `QGuiApplication` is created.
6. **Not covered by this spike:** callbacks (`registerCallback` for
   invalidation), input events, multi-tile viewport rendering, threading, and
   running LOK out-of-process over the JSON/socket bridge described in
   CLAUDE.md. The in-process rendering path all of those rely on works.
