# ExtApp

A simple app to manage launcher shortcuts (`.desktop` files) for external
applications — AppImages and other loose executables — in Linux's
applications folder (`~/.local/share/applications`).

- **Find programs**: scan a folder (and sub-folders) for AppImages, ELF
  executables and, optionally, scripts. Each one shows whether it already has
  a shortcut, and whether it still needs to be made executable.
- **Create shortcuts**: name, description, category, extra arguments,
  "run in terminal", and an icon — from a file, or searched online
  (DuckDuckGo Images) on request.
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
  third_party/cjson/       vendored cJSON 1.7.18 (MIT)
  tests/                   unit tests (no network needed)
frontend/
  cli/                     extapp-cli: small command line frontend
  gtk4/                    GTK4 + libadwaita frontend
```

Shortcuts created by ExtApp are named `extapp-<name>.desktop` and carry
`X-ExtApp-Managed=true` and `X-ExtApp-Target=<program>`. That is how ExtApp
recognises its own files and links programs to their shortcuts. Downloaded
icons are stored in `~/.local/share/extapp/icons/`. All paths follow
`$XDG_DATA_HOME`.

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
extapp-cli icons "foo icon"
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

## Notes on the online icon search

DuckDuckGo has no official image API; ExtApp uses the same requests its web
page makes. DuckDuckGo may rate-limit or show a bot check to repeated
automated searches. When that happens ExtApp reports it instead of working
around it — wait a while, or choose an icon from a file. The provider sits
behind a small table in `backend/src/icon_search_ddg.c`, so another source can
be added. Check an image's license before redistributing it.

## License

GPL-3.0-or-later. cJSON is MIT-licensed (see
`backend/third_party/cjson/LICENSE`).
