# Changes: paste clipboard history entries on selection (Klipper)

## Summary

Selecting an entry in a clipboard menu no longer only copies it to the
clipboard — it is also pasted automatically into the window that had the
keyboard focus before the menu was opened.

- Paste keys are injected through KWin's `org_kde_kwin_fake_input` protocol
  (Wayland only; no uinput, no X11 XTEST).
- `Ctrl+V` is used normally, `Shift+Insert` when the target window is a terminal
  (detected via `org_kde_plasma_window_management` by application id / resource
  name).
- Configurable in the Klipper settings dialog, page **General**, checkbox
  **"Paste on selection"** ("Paste the item into the active window").
  The option is enabled by default.
- Applies to both menus: the clipboard applet in the system tray and the
  popup opened by "Show Clipboard Items at Mouse Position" (Meta+V).

## New files

### `klipper/pasteinjector.h`, `klipper/pasteinjector.cpp`

`PasteInjector`: a lazily created singleton (`PasteInjector::self()`) that
owns the Wayland objects and implements the paste-on-select state machine:

| method | when it runs |
|---|---|
| `requestPaste()` | an entry was selected; records that a paste is pending and starts the safety timer |
| `popupClosed()` | the menu was hidden; the paste is actually performed only then |
| `maybeStartPaste()` | the menu is closed **and** the clipboard has been quiet for 100 ms |

The clipboard-settled check uses a debounced `QClipboard::changed` (100 ms)
plus a 100 ms freshness check for the synchronous clipboard update that
usually happens before the menu is told to hide itself.

Before injecting the keys the injector waits for plasmashell itself to lose
the keyboard focus (`QGuiApplication::focusWindow` polled every 30 ms, up to
1.5 s), which means the menu has released it and the target window has a
chance to regain it. A further grace period of 100 ms follows. If the menu
never reports being closed, a safety timer (3 s) either pastes when focus has
already been released elsewhere or cancels the paste, so that nothing is ever
pasted into the clipboard menu itself.

**Terminal detection:** the active window's application id and resource name
(`org_kde_plasma_window_management`) are matched against a list of terminal
tokens; too generic names (`foot`, `st`, `rio`, `kgx`, `tilda`, `warp`) are
matched as whole words of the id/resource only, everything else as substrings
(`konsole`, `kitty`, `yakuake`, `alacritty`, …). The key events (press/release,
15 ms apart) are sent through KWayland's `FakeInput` client; `authenticate()`
is silent on the KWin side, the interface is granted based on the client's
desktop file (see below).

## Modified files

### `klipper/klipper.kcfg`

New entry `PasteOnSelect` (Bool, default `true`) with label and a "what's
this" explaining the paste-on-select behaviour and the `Shift+Insert` shortcut
for terminals.

### `klipper/configdialog.h` / `klipper/configdialog.cpp`

"Paste on selection" checkbox (`kcfg_PasteOnSelect`) on the General page of
the Klipper settings dialog, bound to the new config entry.

### `klipper/CMakeLists.txt`

`pasteinjector.cpp` / `pasteinjector.h` added to the `klipper` target.

### `klipper/klipper.cpp`

`PasteInjector::self()` is created eagerly at the end of the `Klipper`
constructor, so that the injector is already watching the clipboard (and has
set up its Wayland objects) when the first entry is selected.

### `klipper/klipperpopup.h` / `klipper/klipperpopup.cpp`

The C++ popup (Meta+V) now calls `PasteInjector::requestPaste()` from its
`onItemSelected()` slot and reports `popupClosed()` when its `QWindow` stops
being visible.

### `klipper/declarative/klipperinterface.h` / `klipper/declarative/klipperinterface.cpp`

Two new `Q_INVOKABLE`s, `requestPaste()` and `popupClosed()`, used by the QML
clipboard applet to drive the injector.

> **NOTE:** the logging category cannot be used here because `libklipper` does
> not export `KLIPPER_LOG()`, so it is defined again locally with the same
> category name (same filtering rules apply).

### `klipper/declarative/qml/KlipperPopup.qml`

New signal `itemSelected()`, emitted before `requestHidePopup()`.

> **WARNING:** `KlipperInterface` must **not** be instantiated from this
> component: it is loaded by C++ `KlipperPopup` from inside `Klipper`'s
> constructor, and reaching `Klipper::self()` from QML at that point re-entered
> the constructor, which crashed plasmashell in a stack overflow loop. The
> signal is therefore connected on the C++ side instead (comment in the file).

### `applets/clipboard/qml/main.qml`

`ClipboardMenu.onItemSelected` now calls `klipper.requestPaste()` and then
hides the popup as before; the `onExpandedChanged(false)` branch additionally
reports `klipper.popupClosed()`. `requestPaste()` is called unconditionally —
the injector decides on its own whether it is safe to paste, so the paste also
works when the popup does not close automatically
(`hideOnWindowDeactivate == false`).

### `shell/org.kde.plasmashell.desktop.cmake`

`org_kde_kwin_fake_input` added to `X-KDE-Wayland-Interfaces`. KWin announces
the fake input global only to clients whose desktop file lists it; without this
entry the injector logs `org_kde_kwin_fake_input is not available` and nothing
is pasted.

## Not part of this change

### `shell/scripting/scriptengine_v1.cpp`

Pre-existing local modification (`setFloatingApplets()` call commented out),
untouched by this work.

## Outside of the repository

### `~/.config/environment.d/kde-local-build.conf`

`QT_PLUGIN_PATH` now also contains `<lib64>/plugins`. The clipboard applet
plugin of the local build is installed into
`<lib64>/plugins/plasma/applets`, which Plasma only searches when the plugins
root itself is a Qt plugin path. Without it plasmashell silently ran the
distribution's clipboard applet, so the QML part of this feature (and of any
other locally built applet) was never loaded.

## Build / install

```sh
cmake --build build --target klipper klipperplugin org.kde.plasma.clipboard
cmake -DCMAKE_INSTALL_PREFIX="$HOME/.local" -P build/klipper/cmake_install.cmake
cmake -DCMAKE_INSTALL_PREFIX="$HOME/.local" -P build/applets/clipboard/cmake_install.cmake
```

plasmashell then has to be restarted; it must run with the environment from
`environment.d`, otherwise the distribution's libraries are used.

## Debugging

The whole state machine logs to the `org.kde.klipper` category. Starting
plasmashell with `QT_LOGGING_RULES=org.kde.klipper.debug=true` makes the
journal show the complete sequence:

```
QML requested a paste on selection
requestPaste: clipboard settled: false focus window: "popupWindow ()"
QML reported that the clipboard menu closed
clipboard settled
focus released after 30 ms, focus window now: "<none>"
Target window: "org.kde.yakuake" "yakuake" using Shift+Insert
sending Shift+Insert
```

## Tested (Wayland, KWin 6.7.5)

- LibreOffice Writer as target → `Ctrl+V`, pasted
- Yakuake as target → `Shift+Insert`, pasted
- clipboard applet in the system tray and the Meta+V popup, both working
- option in the settings dialog switches the behaviour off/on
