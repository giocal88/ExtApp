# ExtApp

A simple app to manage launcher shortcuts (`.desktop` files) for external
applications — AppImages and other loose executables — in Linux's
applications folder (`~/.local/share/applications`).

- **Find programs**: scan a folder (and sub-folders) for AppImages, ELF
  executables and, optionally, scripts. Each one shows whether it already has
  a shortcut, and whether it still needs to be made executable.
- **Create shortcuts**: name, description, category, extra arguments,
  "run in terminal", and an icon — from a file, or searched online on
  request (AppImageHub, Flathub and Wikimedia Commons).
- **Manage shortcuts**: list every application shortcut in your personal
  applications folder, edit the ones ExtApp created, and remove shortcuts
  (with a stronger warning for ones ExtApp did not create).

## Architecture

The backend and the frontends are fully separate, so ExtApp can get a Qt (or
any other) frontend without touching the core logic.

```
backend/                 libextapp — C11 + POSIX + libcurl, no GLib/Qt
  include/extapp/extapp.h  the only public header (C ABI, C++-safe)
  src/                     scanner, .desktop parser/writer, HTTP, icon search
                           (one icon_provider_*.c file per source)
  third_party/cjson/       vendored cJSON 1.7.18 (MIT)
  tests/                   unit tests (no network needed)
frontend/
  cli/                     extapp-cli: small command line frontend
  gtk4/                    GTK4 + libadwaita frontend
```

Shortcuts created by ExtApp are named `extapp-<name>.desktop` and carry
`X-ExtApp-Managed=true` and `X-ExtApp-Target=<program>`. That is how ExtApp
recognises its own files and links programs to their shortcuts. Downloaded
icons are stored in `~/.local/share/extapp/icons/`, and the AppImageHub
catalogue is cached in `~/.cache/extapp/`. All paths follow `$XDG_DATA_HOME`
and `$XDG_CACHE_HOME`.

## Building

Dependencies (Ubuntu/Debian):

```bash
sudo apt install build-essential cmake pkg-config libgtk-4-dev libadwaita-1-dev libcurl4-openssl-dev
```

GTK ≥ 4.12 and libadwaita ≥ 1.5 are required (Ubuntu 24.04 or newer).

```bash
cmake -B build
cmake --build build
ctest --test-dir build --output-on-failure
./build/frontend/gtk4/extapp
```

CMake options: `EXTAPP_BUILD_GTK`, `EXTAPP_BUILD_CLI`, `EXTAPP_BUILD_TESTS`
(all `ON` by default), and `EXTAPP_SHARED_BACKEND` (default `OFF`).

### Command line

```bash
extapp-cli scan ~/Applications
extapp-cli create ~/Applications/Foo-1.2-x86_64.AppImage --categories Utility --chmod
extapp-cli list
extapp-cli icons "foo"
extapp-cli remove ~/.local/share/applications/extapp-foo.desktop
```

## Writing another frontend (e.g. Qt)

Link against `ExtApp::backend` and include `<extapp/extapp.h>`. The contract:

- **Blocking calls.** Every function blocks; run scans, listings and network
  calls on a worker thread (`QtConcurrent::run`, `std::thread`, …). The GTK
  frontend wraps them in `GTask` (`frontend/gtk4/src/ea-async.c`).
- **Shared context.** Create one `ExtAppContext` with `extapp_context_new()`.
  It is immutable and can be shared between threads.
- **Ownership.** Every out-parameter is owned by the caller and freed with
  its `*_free()` function (plain `char *` / `unsigned char *` with `free()`).
- **Errors.** Calls return an `ExtAppStatus`; pass `char **err` to get a
  readable message.
- **Cancellation.** Long calls accept an `ExtAppCancelFn`, polled during the
  work; return `true` to abort with `EXTAPP_ERR_CANCELLED`.
- **Images.** The backend never decodes images. Convert icons to PNG
  (e.g. `QImage::save`) before `extapp_icon_save()`; SVG can be passed as-is.

## Online icon search

When asked, ExtApp searches three free sources in parallel. None of them needs
an API key:

| Source | What it returns |
|---|---|
| [AppImageHub](https://appimage.github.io) | The official icon of ~3700 AppImages. The catalogue (~2.5 MB) is cached and refreshed weekly; a stale copy is used offline. |
| [Flathub](https://flathub.org) | The official 128×128 icon of matching Flatpak apps (found by name, app id or keyword, e.g. "vscode"). |
| [Wikimedia Commons](https://commons.wikimedia.org) | Logo files whose name mentions the app. Big rasters are replaced by a 256 px rendering. |

Results are merged with application catalogues first. Words such as "icon"
or "logo" in the query are ignored. Each word of the query must start a word
of the name, so "lm studio" matches "LM Studio" but not "Elmer Studio".
ExtApp sends an honest `User-Agent` (`ExtApp/<version> (+project URL)`), as
Wikimedia requires.

Adding a source means writing one `EaIconProvider` (see
`backend/src/icon_provider_*.c`) and listing it in `backend/src/icon_search.c`.
Logos may be trademarks: check an image's license before redistributing it.

## License

GPL-3.0-or-later. cJSON is MIT-licensed (see
`backend/third_party/cjson/LICENSE`).
