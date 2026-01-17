# baresip-lvgl

An embedded SIP Softphone application for ARM platforms using [Baresip](https://github.com/baresip/baresip) and [LVGL](https://lvgl.io).

## Features
- **SIP Telephony**: Make and receive VoIP calls using Baresip.
- **Touch UI**: Responsive interface built with LVGL (Light and Versatile Graphics Library).
- **Video Calls**: Support for raw video stream display (via SDL or FBDEV).
- **Multi-Call Management**: internal handling and UI for multiple incoming calls.
- **History & Contacts**: SQLite-backed call history and contact management.
- **Applet-based Architecture**: Modular design with independent applets:
    - **Home**: Dashboard with account status, date/time, and quick actions.
    - **Dialer**: Keypad for making calls with status indication.
    - **Contacts**: Address book management.
    - **History**: Call logs (Incoming, Outgoing, Missed) with interactive details.
    - **Settings**: Configuration for Accounts, Audio, and System.
    - **Chat**: Instant messaging.
- **Hardware Support**: Optimized for ARM VersatilePB (QEMU) with Framebuffer output.

## Dependencies
- **baresip**: SIP stack.
- **re**: Libre SIP library.
- **lvgl**: Graphics library (v8.3+).
- **sqlite3**: Database for persistence.
- **ffmpeg**: (Optional) For video coding.

## Project Structure
The source code is organized in `src/`:
- `src/main_fbdev.c`: Entry point and LVGL initialization.
- `src/applets/`: Individual applet logic and UI.
- `src/manager/`: Core managers (Baresip, Config, Database, etc.).
- `src/ui/`: Shared UI helpers and components.

## Usage
Run directly on the target:
```sh
baresip-lvgl
```

The application normally starts via init script `/etc/init.d/S99baresip-lvgl`.

## System Architecture
The application follows a modular "Manager-Applet" architecture ensuring separation of concerns between SIP logic, UI handling, and data persistence:

- **AppletManager**: Controls the lifecycle, visibility, and switching of UI Applets (Home, Dialer, Call, etc.).
- **BaresipManager**: Acts as the bridge between the Baresip core (SIP stack) and the LVGL UI, interacting via a thread-safe event queue.
- **ConfigManager**: Manages application settings (SIP accounts, Audio, UI prefs) with persistence.
- **DatabaseManager**: Handles SQLite operations for Contacts and Call History.

## Development
To rebuild the package individually within the Buildroot environment:
```sh
make baresip-lvgl-dirclean && make baresip-lvgl
```
