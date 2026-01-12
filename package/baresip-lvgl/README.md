# baresip-lvgl

An embedded SIP Softphone application for ARM platforms using [Baresip](https://github.com/baresip/baresip) and [LVGL](https://lvgl.io).

## Features
- **SIP Telephony**: Make and receive VoIP calls using Baresip.
- **Touch UI**: Responsive interface built with LVGL (Light and Versatile Graphics Library).
- **Video Calls**: Support for raw video stream display (via SDL or FBDEV).
- **History & Contacts**: SQLite-backed call history and contact management.
- **Applet-based Architecture**: Modular design with independent applets:
    - **Home**: Dashboard with status and quick actions.
    - **Dialer**: Keypad for making calls.
    - **Contacts**: Address book management.
    - **History**: Call logs (Incoming, Outgoing, Missed).
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
