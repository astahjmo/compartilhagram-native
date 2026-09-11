#pragma once
#include <QString>

// Reads the Better Auth session cookie directly from the user's default
// browser profile, so logging in to the native app does not require copying
// the cookie value out of DevTools by hand. Linux only for now: Gecko-based
// browsers (Firefox, Zen, LibreWolf, Waterfox, Floorp — unencrypted SQLite)
// and Chromium-based browsers (Chrome, Chromium, Edge, Brave, Vivaldi, Opera
// — SQLite with the value AES-encrypted under a key held in the desktop
// keyring). The cookie database is always copied to a private temporary file
// before being read, since the browser holds it open while running; the copy
// is deleted immediately after the lookup, win or lose.
class BrowserSession {
public:
  struct Result {
    QString value;   // The raw, unescaped cookie value. Empty on failure.
    QString error;   // Empty on success.
    QString browser; // Human-readable browser name actually used, if known.
  };
  // Looks up `cookieName` for `host` in the current default browser's
  // profile. `host` should be a bare domain (no scheme), e.g.
  // "games.butecodosdevs.com"; matches that host and its parent-domain
  // cookies (a leading-dot host in the cookie store).
  static Result readCookie(const QString &host, const QString &cookieName);
};
