#pragma once
#include <stddef.h>
#include <stdint.h>

// The name on the certificate. mDNS is HOST_NAME ("beacon-timer") plus ".local".
#define TLS_HOST "beacon-timer.local"

// After LittleFS and Wi-Fi are up. Creates the certificate authority and server
// certificate on the board the first time, and renews the server certificate when
// this build's dates differ. Safe to call when storage failed: it does nothing.
void tlsPrepare();

// Listens on 443 and forwards to the page on port 80. Call after server.begin().
// True when https://beacon-timer.local is answering.
bool tlsStart();

// Things the TLS server answers itself instead of relaying to port 80. Both tables must stay valid for the life of the program.
//   asset: GET/HEAD of uri returns data (gzip = the bytes are already gzip-compressed), with Content-Length and the given Cache-Control.
//   json:  GET (post = 0) or POST (post = 1) of uri; fn fills buf with a JSON reply and returns its length (or < 0 for an error).
struct TlsAsset { const char* uri; const char* type; const uint8_t* data; size_t len; uint8_t gzip; const char* cache; };
struct TlsJson { const char* uri; uint8_t post; int (*fn)(char* buf, size_t cap); };
void tlsRoutes(const TlsAsset* assets, size_t nAssets, const TlsJson* json, size_t nJson);

// Frees the TLS server before an outbound HTTPS download (online update).
void tlsStop();
