# Wallpaper Sync

Pulls the custom sleep-screen wallpaper from a host on your LAN, so a generated
image (todo list, dashboard, anything) stays current without ever putting the
device on the File Transfer screen.

Off by default. It is the only feature that brings WiFi up on its own.

## Setup

1. Serve two files from a machine on the same network:
   - `http://<host>:<port>/todo.ver` — a short version string (any hash)
   - `http://<host>:<port>/todo.bmp` — the wallpaper, 480x800 BMP
2. On the device: **Settings → Display → Sleep Screen → Custom**
3. **Settings → Display → Wallpaper Sync → On**
4. **Settings → Display → Wallpaper Sync URL →** `http://<host>:<port>`
   (base URL only; the leaf names are fixed)

Both settings are also editable from the web settings UI while in File Transfer.

### Use a hostname, not an IP

The URL may name the host as `something.local` — e.g.
`http://my-laptop.local:8000`. The device resolves it over mDNS at every sync,
so the URL keeps working when DHCP hands the host a new address or you move
between networks. `esp_http_client` cannot resolve `.local` itself, so the sync
does the lookup and substitutes the address. The last resolved address is cached
in `state.json` and used if a multicast answer goes missing.

## When it runs

Only at sleep entry, from `enterDeepSleep()` in `main.cpp`. That is the one
moment where the cost is zero:

- **Heap** — an open EPUB and the WiFi stack do not fit together on the C3. The
  reader already releases the book before any network work (see KOReader sync in
  `EpubReaderActivity`). At sleep entry that teardown has already happened.
- **Reboots** — every WiFi session elsewhere ends in `silentRestart()` to
  defragment the heap. `enterDeepSleep()` latches `deepSleepInProgress` first,
  which makes that a no-op, because waking from deep sleep is a full chip reset.

The device sleeps often (auto-sleep defaults to 10 minutes idle, plus the power
button), so no timer is needed — the trigger already fires several times a day.

## One sleep behind

The hook sits *after* `goToSleep()`, so the sleep screen paints immediately and
the fetched image appears at the **next** sleep. Moving the call above
`goToSleep()` makes the wallpaper always current, at the cost of a few seconds
where the panel still shows your book after you press power.

## Cost and failure behaviour

A session is one WiFi association (2-4s, the dominant cost), a version check of
a few bytes, and a 48 KB download only when the version changed. Roughly 3 mAh
per day at a typical sleep count.

Everything fails soft, keeping the previous wallpaper:

- No saved network, or association fails within 8s → skip
- Version unchanged → skip the download and the SD write
- Download fails, or the image does not parse via `Bitmap::parseHeaders()` →
  the temp file is discarded and `/sleep.bmp` is left untouched

The image is written to `/sleep.bmp.tmp` and renamed into place, so a truncated
transfer can never replace a wallpaper that renders.

## Why `/sleep.bmp`

`SleepActivity::renderCustomSleepScreen()` checks `/sleep.bmp` at the SD root
before the `/.sleep` folder, so a single fixed file wins deterministically and
needs no `mkdir`. Keep the folder for a random rotation of static wallpapers.

## Image format

Any BMP `Bitmap::parseHeaders()` accepts (bpp 1/2/4/8/24/32, uncompressed).
1-bit at 480x800 is 48 KB versus 1.15 MB for 24-bit, and for black text on white
it looks identical on the panel — worth it for a transfer the device makes while
you are waiting for it to sleep.
