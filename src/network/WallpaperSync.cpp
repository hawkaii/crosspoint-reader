#include "WallpaperSync.h"

#include <Arduino.h>
#include <Bitmap.h>
#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>

#include <string>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "WifiCredentialStore.h"
#include "network/HttpDownloader.h"

namespace {

// SleepActivity::renderCustomSleepScreen() checks /sleep.bmp before the /.sleep
// folder, so a single fixed file wins deterministically and needs no mkdir.
constexpr char DEST_PATH[] = "/sleep.bmp";
constexpr char TMP_PATH[] = "/sleep.bmp.tmp";

// The user is waiting for the device to sleep, so fail fast when the network is
// not the one at home. A failed association costs this much and no more.
constexpr uint32_t CONNECT_TIMEOUT_MS = 8000;
constexpr size_t MAX_VERSION_LEN = 64;

std::string buildUrl(const char* base, const char* leaf) {
  std::string url(base);
  while (!url.empty() && url.back() == '/') url.pop_back();
  url += leaf;
  return url;
}

void trim(std::string& value) {
  const auto notSpace = [](unsigned char c) { return !isspace(c); };
  value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
  value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
}

bool connectSavedWifi() {
  const std::string ssid = WIFI_STORE.getLastConnectedSsid();
  if (ssid.empty()) {
    LOG_INF("WLP", "No last-connected network saved; skipping wallpaper sync");
    return false;
  }
  const auto credential = WIFI_STORE.findCredential(ssid);
  if (!credential) {
    LOG_INF("WLP", "No stored credential for '%s'; skipping", ssid.c_str());
    return false;
  }

  WiFi.persistent(false);  // Credentials live in WifiCredentialStore, not SDK NVS
  WiFi.mode(WIFI_STA);
  if (credential->password.empty()) {
    WiFi.begin(ssid.c_str());
  } else {
    WiFi.begin(ssid.c_str(), credential->password.c_str());
  }

  const uint32_t deadline = millis() + CONNECT_TIMEOUT_MS;
  while (static_cast<int32_t>(deadline - millis()) > 0) {
    if (WiFi.status() == WL_CONNECTED) {
      LOG_INF("WLP", "Connected to '%s' as %s", ssid.c_str(), WiFi.localIP().toString().c_str());
      return true;
    }
    delay(100);
  }
  LOG_INF("WLP", "'%s' did not connect within %ums; skipping", ssid.c_str(), CONNECT_TIMEOUT_MS);
  return false;
}

void shutdownWifi() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

// A truncated transfer must never replace a wallpaper that renders. Parse the
// candidate with the same reader the sleep screen will use.
bool parsesAsBitmap(const char* path) {
  HalFile file;
  if (!Storage.openFileForRead("WLP", path, file)) return false;
  Bitmap bitmap(file);
  const BmpReaderError err = bitmap.parseHeaders();
  if (err != BmpReaderError::Ok) {
    LOG_ERR("WLP", "Downloaded image rejected: %s", Bitmap::errorToString(err));
  }
  file.close();
  return err == BmpReaderError::Ok;
}

}  // namespace

void WallpaperSync::syncAtSleep() {
  if (!SETTINGS.wallpaperSyncEnabled) return;
  if (SETTINGS.wallpaperSyncUrl[0] == '\0') {
    LOG_INF("WLP", "Wallpaper sync enabled but no URL set");
    return;
  }

  if (!connectSavedWifi()) {
    shutdownWifi();
    return;
  }

  // Ask for the version first. It is a handful of bytes, and when it matches we
  // skip a 48 KB transfer and a pointless SD write. The radio is the expensive
  // part either way, but the card wear is worth avoiding.
  std::string version;
  if (!HttpDownloader::fetchUrl(buildUrl(SETTINGS.wallpaperSyncUrl, "/todo.ver"), version)) {
    LOG_INF("WLP", "Version check failed; leaving wallpaper untouched");
    shutdownWifi();
    return;
  }
  trim(version);
  if (version.empty() || version.size() > MAX_VERSION_LEN) {
    LOG_ERR("WLP", "Unusable version response (%u bytes)", static_cast<unsigned>(version.size()));
    shutdownWifi();
    return;
  }
  if (version == APP_STATE.wallpaperVersion) {
    LOG_INF("WLP", "Wallpaper already at version %s", version.c_str());
    shutdownWifi();
    return;
  }

  const auto result = HttpDownloader::downloadToFile(buildUrl(SETTINGS.wallpaperSyncUrl, "/todo.bmp"), TMP_PATH);
  if (result != HttpDownloader::OK) {
    LOG_ERR("WLP", "Download failed (%d); keeping previous wallpaper", static_cast<int>(result));
    Storage.remove(TMP_PATH);
    shutdownWifi();
    return;
  }

  if (!parsesAsBitmap(TMP_PATH)) {
    Storage.remove(TMP_PATH);
    shutdownWifi();
    return;
  }

  Storage.remove(DEST_PATH);  // rename() will not overwrite an existing entry
  if (!Storage.rename(TMP_PATH, DEST_PATH)) {
    LOG_ERR("WLP", "Could not move %s into place", TMP_PATH);
    Storage.remove(TMP_PATH);
    shutdownWifi();
    return;
  }

  APP_STATE.wallpaperVersion = version;
  APP_STATE.saveToFile();
  LOG_INF("WLP", "Wallpaper updated to version %s", version.c_str());
  shutdownWifi();
}
