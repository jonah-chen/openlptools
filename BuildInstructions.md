## Building

### Requirements

- Windows 10 or 11 (x64)
- [CMake](https://cmake.org/download/) 4.4 or newer
- Visual Studio 2022 or 2026 (or the free Build Tools edition) with the **Desktop development with C++** workload

All third-party libraries (Adobe DNG SDK, XMP Toolkit, libjpeg, libjxl, expat, zlib, SDL3, Dear ImGui) are included in `third_party/`, so nothing else needs to be installed or downloaded.

### Build

From the repository root, run:

```
cmake --workflow --preset release
```

This configures the project into `build/` and compiles it. The first build takes a few minutes, mostly for libjxl and SDL. The program is written to `build\Release\raw2raw2.exe`.

For a debug build, use `cmake --workflow --preset debug`; the program is then written to `build\Debug\raw2raw2.exe`.

### Run

```
build\Release\raw2raw2.exe [path\to\photo.dng | path\to\folder] [--limit N]
```

This opens the home screen: every tool of the app as a card in a grid that wraps to
the window width and scrolls when there are more cards than fit. Click a card to open
that tool. A bar above every tool has a back button: **Home**, or **Back to …** when
the tool was opened from another one (with a **Home** button next to it). Esc does the
same as the back button, and **File > Home** (Ctrl+H) always returns to the grid.
Leaving a tool keeps its state and lets its work go on in the background (its card
says *Working...*). Given a path, the app skips
the grid and opens the tool that fits it: a DNG opens in the **Image Viewer**, a
folder of DNGs in **Exposure Match** (see below; `--limit N` analyzes only its first N
files), and a folder holding TIFFs but no DNGs in **Video Render** (see
[Rendering TIFFs to a video](#rendering-tiffs-to-a-video)). The same applies to files
and folders dropped onto the window. Shortcuts that open things work from any screen:
**File > Open** (Ctrl+O), **File > Open folder in viewer** (Ctrl+Alt+O), **File > Open
sequence folder** (Ctrl+Shift+O), **File > Open sequence files** (Ctrl+Alt+Shift+O),
**File > Render TIFF folder to video** (Ctrl+R).

```
build\Release\raw2raw2.exe --video path\to\tiffs [--out video.mp4] [--width 3840]
                           [--fit crop|letterbox|photo] [--fps 30] [--mbps 40] [--codec h264|hevc]
```

This renders a folder of TIFFs to a video without opening a window, printing the
progress to the console it was started from. The defaults are those of the dialog.

## Image Viewer

The files opened make up a **filmstrip** along the bottom, as in Lightroom: a single
DNG by itself, the files chosen together in the Open dialog (Ctrl+A there for all of
a folder) or dropped together on the window, or every DNG of a folder with **File >
Open folder in viewer** (Ctrl+Alt+O). Thumbnails render in the background, the ones
in view first. One frame is *active* (shown large, the lightest cell) and any number
are *selected* (lighter cells) for a tool that works on a set of files; the info pane
counts them.

| Action | Input |
|---|---|
| Show a frame | Click it, or Left / Right arrow, Home / End |
| Select a range | Shift+click, or Shift+arrows |
| Add or remove one frame | Ctrl+click (Ctrl+Shift+click adds a range) |
| Show another of the selected frames | Click it: the selection stays |
| Select all / none | Ctrl+A / Ctrl+D |
| Scroll the strip | Mouse wheel over it, or the scrollbar |
| Resize the strip | Drag its top edge |
| Hide / show the strip | F6, or the bar at the bottom while hidden |
| Zoom around the cursor | Mouse wheel |
| Pan | Left-drag |
| Fit to window | F, or double-click when zoomed |
| Actual pixels (100%) | 1, or double-click when fitted |
| Adjust exposure | Slider in the right pane (Ctrl+click to type a value); edits are not saved yet |

The right pane also shows an RGB histogram of the rendered image, with how much of
it is clipped to black or white.

## Raw Processor: mean and median of a sequence

Combines DNGs shot in sequence, with the camera untouched, into one DNG:

- **Mean** simulates one exposure as long as all the frames together (the usual
  long-exposure look: smooth water, car light trails fading). Each frame is linearized
  by the DNG SDK (black subtracted, linearization table applied) and added into a
  floating-point sum, so any number of frames needs the memory of about two. The sum
  divided by the count is written as 16-bit linear data (black 0, white 65535, no
  linearization table): a 14-bit sensor's values gain two bits of precision that way.
  The result's ISO is divided by the frame count and the shutter speed multiplied by
  it, so it reads as the exposure the frames add up to.
- **Median** keeps, per pixel, the value that at least half the frames agree on, so
  anything passing through (people, cars, birds) disappears and the static scene stays.
  It works on the stored values and writes them back in the frames' own encoding, with
  the same black and white levels and linearization table, so the output is a raw file
  like the inputs. All frames are held in memory; the way the middle value is found
  depends on the count: fixed min/max networks for 3, 5, 7 and 9 frames, an insertion
  sort up to 16, quickselect beyond. An even count averages the two middle values.

Every frame must agree with the others in camera, lens, exposure settings and raw
format (about 35 properties, from the body serial number and shutter speed to the CFA
pattern, black levels and camera profiles). Any difference stops the job and the
message lists each differing property with its values and the frames involved.

The result takes the highlighted frame's metadata and embedded Camera Raw settings
(XMP), except that the capture time is the earliest frame's. A confirmation says so
before anything is written. The default name is `<highlighted>_<mode>_x<count>.dng`
next to the frames; edit it or browse to another place.

Two ways in:

1. Open the **Raw Processor** from the home screen and use **Open DNG files...**
   (Ctrl+A in the dialog for a whole folder) or **Open folder...**. The files fill the
   filmstrip, all selected, the first highlighted; adjust the selection and the
   highlight as in the viewer.
2. In the **Image Viewer**, select frames in the filmstrip, highlight the one whose
   metadata the result should carry, and choose **Use in Raw Processor** (right-click
   the filmstrip, View menu, or Ctrl+P). Esc returns to the viewer.

The median keeps every frame in memory, so a build accepts at most 100 frames; raise
the cap with the CMake cache option `RAW2RAW2_MAX_STACK_FRAMES` (for example
`cmake -S . -B build -DRAW2RAW2_MAX_STACK_FRAMES=300`). The tool shows the memory a
median needs against what is free, and refuses to start when it would not fit.

From the command line, without a window:

```
build\Release\raw2raw2.exe --stack mean|median [--out result.dng] [--meta frame.dng] frame1.dng frame2.dng ...
```

## Settings and exporting from the viewer

**Settings** is the first card on the home screen (also **File > Settings**, Ctrl+,).
Its *Export server* section chooses which server the app exports through:

- **LightCraft export** (the default) or **Photoshop export**: the app's own tools
  (below) at the fixed address `127.0.0.1:31000`. When no server answers there,
  **Launch server...** asks for the application the tool drives (`Photoshop.exe`, with
  the dialog opening in the newest `C:\Program Files\Adobe\Adobe Photoshop *` folder,
  or `lightcraft-cli.exe`) and starts it; **Stop server** stops a server the app
  started, and the app stops it on exit. If `lightcraft-cli.exe` sits next to
  `raw2raw2.exe`, the LightCraft server starts with the app.
- **Custom export**: any server speaking the protocol below, at an address you type.

The page also holds the *export defaults* (format, bit depth, colour space, JPEG
quality, long edge, the subfolder exports go into) and a switch for the warning
before files are replaced. Settings live in `%APPDATA%\raw2raw2\settings.json`.

In the **Image Viewer**, **Export selected...** (button, View menu, Ctrl+E, or the
filmstrip's right-click menu) exports the selected frames (the shown one if none is
selected). A dialog starts from the defaults and lets you change them and the
destination (by default `<each file's folder>\export`), shows which outputs would be
replaced, then sends the files to the server four at a time and reports each as it
finishes. Without a running server the dialog says so and offers to open Settings.

## Export tools: TIFF/JPEG with the Camera Raw edits applied

raw2raw2 renders raws with the DNG SDK, which knows Adobe's default conversion but
none of Camera Raw's develop controls (Highlights, Shadows, Whites, HSL, curves, ...).
To get finished TIFFs or JPEGs with those edits applied, two small programs drive an
application that does. Each builds to its own executable (sources in `src/tools/`),
takes the application's path on the command line, and serves JSON requests on
`http://127.0.0.1:31000/` (change with `--port`):

```
build\Release\r2r-photoshop-export.exe "C:\Program Files\Adobe\Adobe Photoshop 2026\Photoshop.exe"
build\Release\r2r-lightcraft-export.exe C:\path\to\lightcraft-cli.exe [--jobs 4]
```

| Tool | Renders with | Fidelity | Formats | Parallelism |
|---|---|---|---|---|
| `r2r-photoshop-export` | Photoshop's Camera Raw (COM automation; Photoshop is started if needed and stays open) | exact: Adobe's own pipeline with the settings stored in the file | tiff, jpeg, png, psd | requests queue; Photoshop runs one at a time |
| `r2r-lightcraft-export` | [LightCraft](https://github.com/storytold/lightcraft)'s `lightcraft-cli render` | approximate: a clean-room pipeline that maps the `crs:` fields to its own controls | jpeg, png, tiff, webp, avif, jxl, psd, dng | `--jobs` renders at once (default: half the cores) |

**Protocol.** `POST /export` with a JSON body; the answer comes when that export is
done, so a client may send many at once and collect each result as it arrives.
`GET /health` reports the tool, the application's version and how many exports are
active or waiting; `POST /shutdown` stops the service after the exports in flight.

**Started by raw2raw2.** When raw2raw2 starts a tool (from Settings, or on its own at
startup), it owns it: closing the window first asks every tool it started to shut
down, then waits up to 5 seconds before ending it. Each tool runs in a Windows job
object that only raw2raw2 holds, so even if raw2raw2 crashes or is killed, Windows ends
the tool and anything the tool started, such as `lightcraft-cli` renders. The one
exception is Photoshop. When the Photoshop tool starts it, Photoshop leaves the job
and stays open with your documents.

```json
{ "id": 7, "input": "C:/photos/DSC00001.dng", "output": "C:/photos/out/DSC00001.tif",
  "bitDepth": 16, "colorSpace": "AdobeRGB", "longEdge": 4000, "ppi": 300, "quality": 92,
  "photoshop": { "settings": "image", "size": "native", "tiffCompression": "zip" },
  "lightcraft": { "metadata": "all", "set": { "light.exposure": 0.5 } } }
```

`input` and `output` are required; the output's extension names the format unless
`format` says otherwise. `bitDepth` (8/16), `colorSpace` (sRGB, AdobeRGB, ProPhotoRGB,
ColorMatchRGB; LightCraft also displayP3, rec2020), `quality` (JPEG, 1–100),
`longEdge` and `ppi` are optional. The tool-specific objects are ignored by the other
tool: Photoshop takes `settings` (`image` = the file's own Camera Raw settings, or
`cameraDefault`), `size` (Camera Raw's workflow sizes: native, minimum, small, medium,
large, extraLarge, maximum), `tiffCompression` (none, lzw, zip, jpeg) and `jpegQuality`
(0–12); LightCraft takes `set` (develop controls on top of the file's edits), `settings`
(a partial develop JSON or an XMP/.lcpreset preset), `preset`, `metadata` (all,
allExceptCamera, copyright, none) and `opt` (any other `app.export` option).

The answer is `{"ok": true, "id": .., "output": .., "width": .., "height": .., "seconds": ..}`
or `{"ok": false, "id": .., "error": "..."}`; a malformed request gets HTTP 400.
Photoshop writes the complete EXIF (MakerNote included) with an embedded profile and
the XMP; LightCraft rebuilds a smaller EXIF (camera, lens, exposure, dates, GPS) from
its own metadata model.

## Matching the exposure of a sequence

A sequence is the DNGs of a folder, or some of them. **Open sequence folder...**
(Ctrl+Shift+O, or drop the folder on the window) takes every DNG in it. **Open DNG
files...** (Ctrl+Alt+Shift+O) takes only the files picked in the dialog (Ctrl+click,
Shift+click, or Ctrl+A for all). Either way the frames are sorted by file name, which
for camera files is capture order. Each frame is metered against the first, which is
taken as correctly exposed. The result is an exposure adjustment per frame that makes
it match the first frame, for example to remove flicker and the exposure steps of a
time-lapse. *Re-analyze* reads a folder again, picking up frames added since; for
picked files it reuses the same files.

**The metering server.** The one measurement, "how many stops brighter is frame B
than frame A", is made by a separate server; everything else (listing, scheduling,
camera settings, the fit, CSV and sidecars, thumbnails and previews) happens in the
app. The built-in server is `r2r-exposure-match.exe` next to `raw2raw2.exe`, started
with the app on `127.0.0.1:31001` and stopped with it; *Settings > Exposure match
server* can point at a custom server instead (any program answering the same
request), and sets how many comparisons the app asks for at once (default 4). The
server caches the metered images by path, so a sequence against one reference decodes
each file once.

```
r2r-exposure-match [--port 31001] [--jobs N]      N files decoding at once (default: a quarter of the cores)
```

| Request | Answer |
|---|---|
| `POST /compare` `{"id": any, "reference": "C:/seq/DSC00001.dng", "image": "C:/seq/DSC00002.dng"}` | `{"ok": true, "id": any, "stops": 0.31, "spreadEv": 0.02, "validFraction": 0.97, "method": "pixels", "seconds": 1.2}` |
| `GET /health` | `{"ok": true, "tool": "exposure-match", "version": "1", "cached": 12, "active": 1, "decoding": 1, "jobs": 2}` |
| `POST /shutdown` | `{"ok": true}` |

`stops` is log2(brightness of `image` / brightness of `reference`): positive means
the image is brighter. `spreadEv` is the robust spread of the per-pixel ratios (small
means a clean match), `validFraction` the share of pixels usable in both frames, and
`method` is `pixels` (the median of per-pixel ratios, which ignores the minority of
pixels where something moved) or `percentiles` (brightness percentiles matched, when
the frames differ in size or almost nothing is usable). A missing field or file
answers HTTP 400 with `{"ok": false, "error": "..."}`; a file that cannot be decoded
answers HTTP 200 with `ok: false` and the reason. A custom server only has to honor
`POST /compare` and `GET /health` (`ok: true` is enough).

**Exposure scale.** Exposures are given in EV relative to 1 s, f/1.0, ISO 100:
`log2(t / 1 s) − 2 log2(N) + log2(ISO / 100)`, so 1 s at f/2 and ISO 100 is −2 EV.
The first frame sits exactly at its camera settings ("Camera EV"). The raw brightness
of every other frame is metered against the first ("Raw vs 1st"); since raw brightness
is scene brightness plus camera exposure, taking the frame's own change of settings out
again gives the scene's absolute brightness on the same scale ("Scene EV"): the
brightness the frame would have had with the first frame's settings. A sunrise shot
with the camera stepping its shutter down therefore shows a rising scene curve with
the camera steps below it.

**Metering.** Files are decoded several at a time, as many as fit in half of the free
memory (up to one per core), with the cores shared between them; the toolbar shows
the split while analyzing. Each file is demosaiced at reduced size, averaged down to
a small linear image, and compared with the reference pixel by pixel: the result is the median
of the per-pixel log brightness ratios, ignoring pixels that are clipped or nearly
black in either frame. Moving clouds, people or waves are a minority of pixels and
drop out of the median. "Spread" is the robust spread of those ratios in EV; values
far above 0.05 mean the two frames differ in more than brightness. If the frames
cannot be compared pixel by pixel, brightness percentiles are matched instead.
**Compare with** chooses between metering every frame against the first (no drift)
or against its predecessor with the differences added up (follows a changing scene).

**Graph.** The panel above the list plots camera EV (steps), measured EV (points) and
the fit (line) against the frame number or the capture time (**X axis** dropdown).
Hover for values, click to select a frame.

**Fit and target.** The scene brightness is smoothed with a polynomial (choose the
degree) or a Gaussian moving average, or used as it is (**None**), and the curve is
anchored to the first frame, which is taken as correctly exposed. **Adjust EV** is the
exposure to apply to each frame for the chosen **Target**: *Scene brightness* renders
every frame as if it had been shot with the first frame's settings, so the camera's
exposure steps disappear and the scene's own, smoothed brightening or darkening
remains (the usual time-lapse "holy grail" correction); *Match first frame* makes
every frame as bright as the first. The first frame always gets 0.

**Right pane.** Selecting a frame renders a preview with its adjustment applied
(uncheck **Apply adjustment** to see it as shot), with its histogram and metadata.
Double-click a row, press Enter or use **Open in viewer** to open the frame in the
single-image view with the exposure slider set to its adjustment; Esc returns to the
sequence.

**Output.** *Write CSV + XMP* writes `exposure_match.csv` into the first frame's folder
(replacing one that is there; one row per analyzed frame with settings, camera/measured/fitted EV, adjustment and spread) and a sidecar
`.xmp` next to each analyzed DNG containing only `crs:Exposure2012` = the adjustment,
for Camera Raw and Lightroom to pick up. *Apply CSV with first file's edits* reads the
adjustments back from the CSV and the first frame's full Camera Raw settings
(highlights, saturation, tone curve, ...), then writes a sidecar for every frame in the
CSV carrying those settings with `Exposure2012` set to the first frame's value plus the
frame's adjustment. So the workflow is: analyze and write, edit the first frame in
Camera Raw or Lightroom, then apply. The first frame's settings come from its sidecar
if present, otherwise from the XMP embedded in the DNG; when both exist and carry an
`xmp:MetadataDate`, the later one wins. Both actions ask before replacing existing
`.xmp` files.

Lightroom Classic and Camera Raw read sidecars next to DNGs and prefer them over the
settings embedded in the DNG, but Adobe's behavior here has varied between versions;
if a sidecar is ignored, Lightroom's *Metadata > Read Metadata from File* usually
picks it up.

## Rendering TIFFs to a video

A DNG holds camera data that every program develops differently (demosaicing, color
profile, tone curve, noise reduction), so the same file looks different in each. A
TIFF exported from Lightroom holds finished RGB pixels: what you saw in Lightroom is
what is in the file, and any program shows the same picture. The two things a reader
still has to honor are the bit depth (8 or 16 bits per channel) and the embedded ICC
profile that says which RGB the numbers mean (sRGB, Adobe RGB, ProPhoto RGB, ...). A
program that ignores the profile shows an Adobe RGB or ProPhoto export too dull; sRGB
exports are safe everywhere. There is no further processing to agree on.

So the last step of the workflow is: export the sequence from Lightroom as TIFFs (any
profile, 8 or 16 bit, uncompressed, LZW or ZIP), then **File > Render TIFF folder to
video** (Ctrl+R), the **Render TIFFs to video** button in the sequence toolbar, or drop
the folder onto the window. The dialog lists the files (in name order, which is
capture order for Lightroom's numbered exports) and shows the first one's format.

| Option | Meaning |
|---|---|
| Codec | H.264 plays everywhere; HEVC (H.265) makes files about half the size but needs a GPU with an HEVC encoder and a player that supports it. |
| Width | 1280, 1920 (Full HD), 2560, 3840 (4K UHD) or custom. Most encoders stop at 4096 pixels. |
| Frame | *Crop to 16:9* fills a 16:9 frame and cuts the top and bottom off a 3:2 photo; *Letterbox to 16:9* keeps the whole photo with black bars; *Photo's own shape* gives the video the photo's aspect ratio (3840 x 2560 for 3:2). |
| Frame rate | 24 to 60 frames per second; the dialog shows the resulting length. |
| Bitrate | Follows the frame size (40 Mbit/s for 4K at 30 fps) until you set it. Higher keeps more detail. |
| Output | Defaults to `<folder name>.mp4` inside the folder. |

Frames are decoded and scaled on all cores while the next file is read from disk, with
the colors converted to sRGB when the profile is something else (matrix/TRC profiles
such as Adobe RGB, ProPhoto and Display P3; LUT-based profiles are not applied). The
scaled frames are encoded with Windows Media Foundation, using the GPU's H.264 or HEVC
encoder when it has one and Windows' own H.264 encoder otherwise. Output is BT.709,
limited range, the standard for video. The dialog shows each frame as it is encoded;
cancelling keeps the frames encoded so far as a playable file.

TIFFs that are tiled, big-endian, grayscale or carry an Orientation tag are handled;
JPEG-compressed, floating point, CMYK, Lab and BigTIFF files are not.

## Code layout and adding a feature

```
src/main.cpp            entry point: a windowless command-line job, or the GUI
src/cli/                command line parsing and the windowless --video / --stack modes
src/app/                the shell:
  gui.*                   window and ImGui setup, event loop, orderly shutdown
  app.*                   App: features, menu bar, navigation bar, routing input
  home_screen.cpp         the grid of feature cards
  file_dialogs.*          native file dialogs, answered on the main thread
  server_manager.*        health checks of the export/metering servers; starts and stops
                          the app's own
  child_process.*         a process the app owns, in a job object that ends it with the app
  feature.*, settings.*, filmstrip.*, image_loading.*, ui.*
                          Feature base class and registry, user settings, shared widgets
src/features/           one .h/.cpp pair per tool
src/common/             helpers used everywhere: paths and text (util.*), parallel.h
src/io/                 file formats: DNG (dng_loader.*), TIFF, XMP settings, MP4 writing
src/processing/         UI-free engines: metering, sequence analysis, raw stacking
                        (raw_stack.*, median_select.h), video export
src/net/                HTTP client and the clients of the export and metering servers
src/tools/<name>/       separate programs (one executable each): the export services that
                        drive Photoshop and LightCraft; src/tools/common/ is their HTTP
                        server, process runner and request handling
```

Each tool is a `Feature` (`src/app/feature.h`). The app owns them, lists them as
cards on the home screen, and gives the current one the whole window. A feature
implements `Draw()` and, as needed, `Poll()` (background work), `DrawMenus()`,
`HandleShortcuts()`, `OpenScore()`/`Open()` (which dropped or chosen paths it takes)
and `Shutdown()`. It reaches the app through `app_`: `Activate()`/`GoBack()` to
navigate (the app draws the back button), `Dialogs().PickFile()`/`PickFolder()`/`PickSaveFile()` for native dialogs,
`UploadTexture()` for images, `Find<T>()` to hand work to another feature.

To add a feature:

1. Create `src/features/my_tool.h` and `.cpp` with a class deriving from `Feature`,
   passing its name and card description to the base constructor.
2. End the `.cpp` with `R2R_REGISTER_FEATURE(MyToolFeature, 40);` (the number orders
   the cards on the home screen).
3. Re-run CMake (every `.cpp` under `src/` is picked up automatically).

No other file needs to change. `src/features/video_render.*` is the smallest complete
example.
