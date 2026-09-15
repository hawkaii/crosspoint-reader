#include "WallpaperSync.h"

#include <Arduino.h>
#include <Bitmap.h>
#include <ESPmDNS.h>
#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
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
// The fallback already cost a scan, so give the second attempt a tighter budget.
constexpr uint32_t FALLBACK_CONNECT_TIMEOUT_MS = 6000;
constexpr size_t MAX_VERSION_LEN = 64;

// The answer normally lands well under a second; this caps the cost when the
// host is off the network.
constexpr uint32_t MDNS_QUERY_TIMEOUT_MS = 3000;
constexpr char LOCAL_SUFFIX[] = ".local";
constexpr size_t LOCAL_SUFFIX_LEN = sizeof(LOCAL_SUFFIX) - 1;

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

bool waitForConnection(uint32_t timeoutMs) {
  const uint32_t deadline = millis() + timeoutMs;
  while (static_cast<int32_t>(deadline - millis()) > 0) {
    if (WiFi.status() == WL_CONNECTED) return true;
    delay(100);
  }
  return false;
}

bool tryCredential(const WifiCredential& credential, uint32_t timeoutMs) {
  WiFi.disconnect(true);
  delay(50);
  if (credential.password.empty()) {
    WiFi.begin(credential.ssid.c_str());
  } else {
    WiFi.begin(credential.ssid.c_str(), credential.password.c_str());
  }
  if (!waitForConnection(timeoutMs)) return false;
  LOG_INF("WLP", "Connected to '%s' as %s", credential.ssid.c_str(), WiFi.localIP().toString().c_str());
  return true;
}

// Pick whichever saved network is actually in range and strongest, the way the
// WiFi screen's auto-connect does. Without this the device keeps retrying the
// last network it joined — a cafe, say — on every sleep once you are home
// again, because lastConnectedSsid only changes from the WiFi screen.
bool connectBestSavedInRange(const std::string& alreadyTried) {
  WiFi.disconnect(true);
  const int16_t found = WiFi.scanNetworks();
  if (found <= 0) {
    WiFi.scanDelete();
    LOG_INF("WLP", "Scan found no networks");
    return false;
  }

  std::string bestSsid;
  int32_t bestRssi = INT32_MIN;
  for (int16_t i = 0; i < found; i++) {
    const std::string ssid = WiFi.SSID(i).c_str();
    if (ssid.empty() || ssid == alreadyTried) continue;
    if (!WIFI_STORE.hasSavedCredential(ssid)) continue;
    if (WiFi.RSSI(i) > bestRssi) {
      bestRssi = WiFi.RSSI(i);
      bestSsid = ssid;
    }
  }
  WiFi.scanDelete();

  if (bestSsid.empty()) {
    LOG_INF("WLP", "No saved network in range");
    return false;
  }
  const auto credential = WIFI_STORE.findCredential(bestSsid);
  if (!credential) return false;

  LOG_INF("WLP", "Trying saved network '%s' (%d dBm)", bestSsid.c_str(), static_cast<int>(bestRssi));
  if (!tryCredential(*credential, FALLBACK_CONNECT_TIMEOUT_MS)) return false;

  // Remember it, so the next sleep takes the fast path instead of scanning again.
  WIFI_STORE.setLastConnectedSsid(bestSsid);
  return true;
}

bool connectSavedWifi() {
  // Nothing loads the credential store at boot — WifiSelectionActivity does it
  // lazily when you open the WiFi screen (see its onEnter). On a boot that never
  // visits that screen the store is empty, so load it here or every sync bails
  // with "no last-connected network" while wifi.json sits on the card, populated.
  WIFI_STORE.loadFromFile();
  if (WIFI_STORE.getCredentialCount() == 0) {
    LOG_INF("WLP", "No saved networks; skipping wallpaper sync");
    return false;
  }

  WiFi.persistent(false);  // Credentials live in WifiCredentialStore, not SDK NVS
  WiFi.mode(WIFI_STA);

  // Fast path: the network we used last time, with no scan.
  const std::string last = WIFI_STORE.getLastConnectedSsid();
  if (!last.empty()) {
    if (const auto credential = WIFI_STORE.findCredential(last)) {
      if (tryCredential(*credential, CONNECT_TIMEOUT_MS)) return true;
      LOG_INF("WLP", "'%s' unavailable; scanning for another saved network", last.c_str());
    }
  }

  return connectBestSavedInRange(last);
}

// Splits "http://host:port/path" into "http://", "host", ":port/path".
bool splitHost(const std::string& url, std::string& prefix, std::string& host, std::string& rest) {
  const size_t schemeEnd = url.find("://");
  const size_t hostStart = schemeEnd == std::string::npos ? 0 : schemeEnd + 3;
  size_t hostEnd = url.find_first_of(":/", hostStart);
  if (hostEnd == std::string::npos) hostEnd = url.size();
  if (hostEnd == hostStart) return false;
  prefix = url.substr(0, hostStart);
  host = url.substr(hostStart, hostEnd - hostStart);
  rest = url.substr(hostEnd);
  return true;
}

bool isLocalHost(const std::string& host) {
  if (host.size() <= LOCAL_SUFFIX_LEN) return false;
  for (size_t i = 0; i < LOCAL_SUFFIX_LEN; i++) {
    const char c = static_cast<char>(tolower(static_cast<unsigned char>(host[host.size() - LOCAL_SUFFIX_LEN + i])));
    if (c != LOCAL_SUFFIX[i]) return false;
  }
  return true;
}

// esp_http_client resolves through lwIP DNS, which never answers .local names,
// so http://laptop.local:8000 would fail outright. Resolve it over mDNS here and
// hand the downloader a literal address — then the URL survives every DHCP lease
// change. Returns "" when there is no usable address at all.
std::string resolveBaseUrl(const char* configured) {
  std::string prefix, host, rest;
  if (!splitHost(configured, prefix, host, rest) || !isLocalHost(host)) return configured;

  // mdns.h: "hostname must not contain domain name, as mDNS uses '.local' domain".
  const std::string bare = host.substr(0, host.size() - LOCAL_SUFFIX_LEN);

  std::string ip;
  if (MDNS.begin("crosspoint")) {
    const IPAddress resolved = MDNS.queryHost(bare.c_str(), MDNS_QUERY_TIMEOUT_MS);
    MDNS.end();
    const String text = resolved.toString();
    if (text != "0.0.0.0") ip = text.c_str();
  } else {
    LOG_ERR("WLP", "mDNS responder failed to start");
  }

  if (!ip.empty()) {
    LOG_INF("WLP", "Resolved %s -> %s", host.c_str(), ip.c_str());
    // Persist only when the address moved, not on every sleep.
    if (APP_STATE.wallpaperResolvedHost != host || APP_STATE.wallpaperResolvedIp != ip) {
      APP_STATE.wallpaperResolvedHost = host;
      APP_STATE.wallpaperResolvedIp = ip;
      APP_STATE.saveToFile();
    }
  } else if (APP_STATE.wallpaperResolvedHost == host && !APP_STATE.wallpaperResolvedIp.empty()) {
    // A dropped multicast answer should not cost the sync when we know where
    // the host was last time.
    ip = APP_STATE.wallpaperResolvedIp;
    LOG_INF("WLP", "No mDNS answer for %s; trying last known %s", host.c_str(), ip.c_str());
  } else {
    LOG_INF("WLP", "Could not resolve %s and no cached address", host.c_str());
    return "";
  }
  return prefix + ip + rest;
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

  const std::string baseUrl = resolveBaseUrl(SETTINGS.wallpaperSyncUrl);
  if (baseUrl.empty()) {
    shutdownWifi();
    return;
  }

  // Ask for the version first. It is a handful of bytes, and when it matches we
  // skip a 48 KB transfer and a pointless SD write. The radio is the expensive
  // part either way, but the card wear is worth avoiding.
  std::string version;
  if (!HttpDownloader::fetchUrl(buildUrl(baseUrl.c_str(), "/todo.ver"), version)) {
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

  const auto result = HttpDownloader::downloadToFile(buildUrl(baseUrl.c_str(), "/todo.bmp"), TMP_PATH);
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
