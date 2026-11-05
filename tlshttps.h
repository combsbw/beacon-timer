#pragma once

// The name on the certificate. mDNS is HOST_NAME ("beacon-timer") plus ".local".
#define TLS_HOST "beacon-timer.local"

// After LittleFS and Wi-Fi are up. Creates the certificate authority and server
// certificate on the board the first time, and renews the server certificate when
// this build's dates differ. Safe to call when storage failed: it does nothing.
void tlsPrepare();

// Listens on 443 and forwards to the page on port 80. Call after server.begin().
// True when https://beacon-timer.local is answering.
bool tlsStart();

// Frees the TLS server before an outbound HTTPS download (online update).
void tlsStop();
