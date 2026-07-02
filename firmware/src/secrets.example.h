#pragma once
// ===========================================================================
//  TEMPLATE — copy this file to secrets.h (same folder) and fill in real
//  values. secrets.h is gitignored so credentials never land in the repo.
// ===========================================================================

// WiFi networks, tried in priority order. pass "" = open network.
struct WifiCred { const char* ssid; const char* pass; };
WifiCred WIFI_NETS[] = {
  { "Your SSID here", "your-wifi-password" },  // use "" as the pass for an open network
};
const int N_WIFI = sizeof(WIFI_NETS) / sizeof(WIFI_NETS[0]);

// InfluxDB write token — use a write-only, bucket-scoped token.
const char* INFLUX_TOKEN = "PASTE_INFLUX_WRITE_TOKEN_HERE";
