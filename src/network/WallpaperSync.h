#pragma once

namespace WallpaperSync {

/**
 * Pull the custom sleep-screen wallpaper from a host on the LAN.
 *
 * Call this ONLY from enterDeepSleep(), after the outgoing activity has been
 * torn down. Two reasons, both structural:
 *
 *  - Heap. An open EPUB and the WiFi stack do not fit together on the C3; the
 *    reader releases the book before any network work (see KOReader sync in
 *    EpubReaderActivity). At sleep entry that teardown has already happened.
 *  - Reboots. Every WiFi session elsewhere ends in silentRestart() to defragment
 *    the heap. enterDeepSleep() latches deepSleepInProgress first, which makes
 *    that a no-op, because waking from deep sleep is a full chip reset anyway.
 *
 * Runs to completion or gives up quickly; never blocks sleep for long.
 */
void syncAtSleep();

}  // namespace WallpaperSync
