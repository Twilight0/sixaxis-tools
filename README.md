# sixaxis-tools 0.2.0

User-mode Sony Sixaxis (PS3 controller) Bluetooth pairing, wrapping
`sixpair.c` (same `054c:0268` / feature-report `0xF5` transfers).

Bluetooth-connected controllers are listed too, but only when all hold:
bluetooth hardware present, daemon running (`bluez` installed), and at
least one Sony controller currently connected.
Three frontends, one backend (`sixpair_core.c` + `btsony.c`):

- `sixaxis-ctrl` — pure CLI: `list [-v]`, `show`, `pair`, `local`,
  `gen`, `masters`, `history`, `bt`
- `sixaxis-tui` — ncurses, pulsemixer-style (`F1`-`F4` modes, `j/k`,
  `?` help)
- `sixaxis-gtk` — GTK3 + XApp (`XAppGtkWindow`, `XAppStackSidebar`)

Known masters live in `~/.config/sixaxis/masters`, pairing log in
`~/.config/sixaxis/history`.

## Build

    make

Needs `libusb-compat` (`usb.h`), `ncurses`, `gtk3`, `xapp`; optional `bluez` for Bluetooth listing.

`sudo make install` installs to `/usr/local` plus the udev rule.

## Run as user (no sudo)

`99-sixaxis.rules` grants `0666` on the controller's USB node:

    sudo cp 99-sixaxis.rules /etc/udev/rules.d/
    sudo udevadm control --reload-rules && sudo udevadm trigger
    # re-plug the controller once

Note: reading/pairing detaches `usbhid` (`js0` vanishes); re-plug
afterwards, or let the tool best-effort rebind (works under sudo).
