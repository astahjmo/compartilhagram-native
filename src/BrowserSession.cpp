#ifndef _WIN32
// Must be included before any Qt header: GLib's headers (pulled in by
// libsecret) use "signals" as a plain struct member name, which collides
// with Qt's signals/slots macros once QString.h has enabled them.
#include <libsecret/secret.h>
#endif
#include "BrowserSession.h"

#ifdef _WIN32
BrowserSession::Result BrowserSession::readCookie(const QString &, const QString &) {
  return {{},
         "Login pelo navegador ainda não é suportado no Windows nesta "
         "versão. Cole o valor da sessão manualmente.",
         {}};
}
#else
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QSettings>
#include <QTemporaryDir>
#include <openssl/evp.h>
#include <sqlite3.h>

namespace {

enum class Engine { Gecko, Chromium };

struct BrowserDef {
  QStringList desktopIdHints; // lowercase substrings matched against the default .desktop id
  QString displayName;
  Engine engine;
  QStringList profileRoots;   // relative to $HOME, tried in order
  QString keyringApplication; // Chromium only: the "application" attribute the
                              // browser's own OSCrypt keyring entry is stored under
};

const QList<BrowserDef> &knownBrowsers() {
  static const QList<BrowserDef> browsers = {
      // Gecko forks before plain "firefox" — their ids also contain neither.
      {{"zen"}, "Zen", Engine::Gecko, {".zen"}, {}},
      {{"librewolf"}, "LibreWolf", Engine::Gecko, {".librewolf"}, {}},
      {{"waterfox"}, "Waterfox", Engine::Gecko, {".waterfox"}, {}},
      {{"floorp"}, "Floorp", Engine::Gecko, {".floorp"}, {}},
      {{"firefox"}, "Firefox", Engine::Gecko, {".mozilla/firefox"}, {}},
      {{"brave"}, "Brave", Engine::Chromium, {".config/BraveSoftware/Brave-Browser"}, "brave"},
      {{"microsoft-edge", "msedge"}, "Microsoft Edge", Engine::Chromium, {".config/microsoft-edge"}, "microsoft-edge"},
      {{"vivaldi"}, "Vivaldi", Engine::Chromium, {".config/vivaldi"}, "vivaldi"},
      {{"opera"}, "Opera", Engine::Chromium, {".config/opera"}, "opera"},
      {{"chromium"}, "Chromium", Engine::Chromium, {".config/chromium"}, "chromium"},
      {{"chrome", "google-chrome"}, "Google Chrome", Engine::Chromium, {".config/google-chrome"}, "chrome"},
  };
  return browsers;
}

QString defaultDesktopId() {
  QProcess process;
  process.start("xdg-settings", {"get", "default-web-browser"});
  if (!process.waitForFinished(3000))
    return {};
  return QString::fromUtf8(process.readAllStandardOutput()).trimmed().toLower();
}

const BrowserDef *matchBrowser(const QString &desktopId) {
  if (desktopId.isEmpty())
    return nullptr;
  for (const auto &browser : knownBrowsers())
    for (const auto &hint : browser.desktopIdHints)
      if (desktopId.contains(hint))
        return &browser;
  return nullptr;
}

// The install-specific default recorded in installs.ini is what actually
// launches when the user opens the browser; profiles.ini's own Default=1
// marker is only a fallback for older/simpler installs.
QString geckoActiveProfileDir(const QString &root) {
  QSettings installs(root + "/installs.ini", QSettings::IniFormat);
  for (const auto &group : installs.childGroups()) {
    installs.beginGroup(group);
    auto path = installs.value("Default").toString();
    installs.endGroup();
    if (!path.isEmpty())
      return root + "/" + path;
  }
  QSettings profiles(root + "/profiles.ini", QSettings::IniFormat);
  for (const auto &group : profiles.childGroups()) {
    if (!group.startsWith("Profile"))
      continue;
    profiles.beginGroup(group);
    const bool isDefault = profiles.value("Default").toInt() == 1;
    const bool relative = profiles.value("IsRelative", 1).toInt() == 1;
    auto path = profiles.value("Path").toString();
    profiles.endGroup();
    if (isDefault && !path.isEmpty())
      return relative ? root + "/" + path : path;
  }
  return {};
}

// The browser keeps its cookie database open (Gecko in WAL mode) while
// running, so a live read can return stale data or fail outright with
// SQLITE_BUSY. Copy it, and any WAL/SHM sidecar, to a private temporary
// directory first; the caller's QTemporaryDir removes it once done.
QString snapshotDatabase(const QString &source, const QTemporaryDir &scratch) {
  if (!scratch.isValid() || !QFile::exists(source))
    return {};
  const QString copy = scratch.path() + "/db.sqlite";
  if (!QFile::copy(source, copy))
    return {};
  QFile::copy(source + "-wal", copy + "-wal"); // best-effort; fine if absent
  QFile::copy(source + "-shm", copy + "-shm");
  return copy;
}

QString readGeckoCookieValue(const QString &dbPath, const QString &host, const QString &name) {
  sqlite3 *db = nullptr;
  if (sqlite3_open_v2(dbPath.toUtf8().constData(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
    sqlite3_close(db);
    return {};
  }
  static const char *sql = "SELECT value FROM moz_cookies WHERE name = ?1 AND "
                           "(host = ?2 OR host = ?3) ORDER BY lastAccessed DESC LIMIT 1";
  sqlite3_stmt *stmt = nullptr;
  QString value;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
    const auto nameUtf8 = name.toUtf8(), hostUtf8 = host.toUtf8(), dotHostUtf8 = ("." + host).toUtf8();
    sqlite3_bind_text(stmt, 1, nameUtf8.constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, hostUtf8.constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, dotHostUtf8.constData(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_ROW)
      value = QString::fromUtf8(reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0)));
  }
  sqlite3_finalize(stmt);
  sqlite3_close(db);
  return value;
}

QByteArray readChromiumEncryptedValue(const QString &dbPath, const QString &host, const QString &name) {
  sqlite3 *db = nullptr;
  if (sqlite3_open_v2(dbPath.toUtf8().constData(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
    sqlite3_close(db);
    return {};
  }
  static const char *sql = "SELECT encrypted_value FROM cookies WHERE name = ?1 AND "
                           "(host_key = ?2 OR host_key = ?3) ORDER BY last_access_utc DESC LIMIT 1";
  sqlite3_stmt *stmt = nullptr;
  QByteArray value;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
    const auto nameUtf8 = name.toUtf8(), hostUtf8 = host.toUtf8(), dotHostUtf8 = ("." + host).toUtf8();
    sqlite3_bind_text(stmt, 1, nameUtf8.constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, hostUtf8.constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, dotHostUtf8.constData(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      const auto *data = static_cast<const char *>(sqlite3_column_blob(stmt, 0));
      const int size = sqlite3_column_bytes(stmt, 0);
      if (data && size > 0)
        value = QByteArray(data, size);
    }
  }
  sqlite3_finalize(stmt);
  sqlite3_close(db);
  return value;
}

// Chromium's own key-storage schema (components/os_crypt/sync in Chromium's
// source): a single passphrase per browser build, held in the desktop
// keyring under this schema with an "application" attribute identifying the
// browser (e.g. "chrome", "chromium", "brave").
QByteArray fetchChromiumKeyringSecret(const QString &application) {
  if (application.isEmpty())
    return {};
  static const SecretSchema schema = {
      "chrome_libsecret_os_crypt_password",
      SECRET_SCHEMA_DONT_MATCH_NAME,
      {{"application", SECRET_SCHEMA_ATTRIBUTE_STRING}, {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING}}};
  GError *error = nullptr;
  gchar *secret = secret_password_lookup_sync(&schema, nullptr, &error, "application",
                                              application.toUtf8().constData(), nullptr);
  QByteArray out;
  if (secret) {
    out = QByteArray(secret);
    secret_password_free(secret);
  }
  if (error)
    g_error_free(error);
  return out;
}

// Linux OSCrypt: AES-128-CBC, key = PBKDF2-HMAC-SHA1(passphrase,
// salt="saltysalt", 1 iteration, 16 bytes), fixed IV of 16 spaces, and the
// ciphertext is prefixed with a 3-byte "v10"/"v11" version tag that is not
// itself encrypted. Falls back to the hardcoded "peanuts" passphrase when no
// keyring secret was retrievable, matching Chromium's own fallback for
// desktops without a running secret service. Newer "v20"+ values use
// OS-level app-bound encryption that cannot be reproduced outside the
// browser process; such values are reported as unsupported.
QByteArray decryptChromiumValue(const QByteArray &encrypted, const QByteArray &keyringSecret) {
  if (encrypted.size() < 4)
    return {};
  const auto prefix = encrypted.left(3);
  if (prefix != "v10" && prefix != "v11")
    return {};
  const QByteArray passphrase = keyringSecret.isEmpty() ? QByteArrayLiteral("peanuts") : keyringSecret;
  unsigned char key[16];
  if (PKCS5_PBKDF2_HMAC_SHA1(passphrase.constData(), passphrase.size(),
                             reinterpret_cast<const unsigned char *>("saltysalt"), 9, 1, sizeof(key),
                             key) != 1)
    return {};
  const unsigned char iv[16] = {' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
                                ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
  const QByteArray cipherText = encrypted.mid(3);
  QByteArray plain(cipherText.size() + 16, '\0');
  EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
  int outLen1 = 0, outLen2 = 0;
  const bool ok =
      ctx && EVP_DecryptInit_ex(ctx, EVP_aes_128_cbc(), nullptr, key, iv) == 1 &&
      EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char *>(plain.data()), &outLen1,
                        reinterpret_cast<const unsigned char *>(cipherText.constData()),
                        cipherText.size()) == 1 &&
      EVP_DecryptFinal_ex(ctx, reinterpret_cast<unsigned char *>(plain.data()) + outLen1, &outLen2) == 1;
  if (ctx)
    EVP_CIPHER_CTX_free(ctx);
  if (!ok)
    return {};
  plain.resize(outLen1 + outLen2);
  return plain;
}

} // namespace

BrowserSession::Result BrowserSession::readCookie(const QString &host, const QString &cookieName) {
  const QString desktopId = defaultDesktopId();
  const BrowserDef *browser = matchBrowser(desktopId);
  if (!browser)
    return {{},
           "Não foi possível identificar o navegador padrão (" +
               (desktopId.isEmpty() ? QStringLiteral("nenhum configurado") : desktopId) +
               "). Navegadores suportados: Firefox, Zen, LibreWolf, Waterfox, Floorp, "
               "Chrome, Chromium, Edge, Brave, Vivaldi, Opera.",
           {}};

  QString profileRoot;
  for (const auto &relative : browser->profileRoots) {
    const QString candidate = QDir::homePath() + "/" + relative;
    if (QDir(candidate).exists()) {
      profileRoot = candidate;
      break;
    }
  }
  if (profileRoot.isEmpty())
    return {{},
           browser->displayName +
               " está configurado como navegador padrão, mas nenhum perfil foi encontrado.",
           browser->displayName};

  QTemporaryDir scratch;

  if (browser->engine == Engine::Gecko) {
    const QString profileDir = geckoActiveProfileDir(profileRoot);
    if (profileDir.isEmpty())
      return {{}, "Não foi possível localizar o perfil ativo do " + browser->displayName + ".",
             browser->displayName};
    const QString db = snapshotDatabase(profileDir + "/cookies.sqlite", scratch);
    if (db.isEmpty())
      return {{}, "Não foi possível ler o banco de cookies do " + browser->displayName + ".",
             browser->displayName};
    const QString value = readGeckoCookieValue(db, host, cookieName);
    if (value.isEmpty())
      return {{},
             "Sessão não encontrada no " + browser->displayName + ". Faça login em " + host +
                 " nesse navegador e tente de novo.",
             browser->displayName};
    return {value, {}, browser->displayName};
  }

  QString profileDir = profileRoot + "/Default";
  if (!QFile::exists(profileDir + "/Cookies"))
    for (const auto &entry : QDir(profileRoot).entryList(QDir::Dirs | QDir::NoDotAndDotDot))
      if (QFile::exists(profileRoot + "/" + entry + "/Cookies")) {
        profileDir = profileRoot + "/" + entry;
        break;
      }
  const QString db = snapshotDatabase(profileDir + "/Cookies", scratch);
  if (db.isEmpty())
    return {{}, "Não foi possível ler o banco de cookies do " + browser->displayName + ".",
           browser->displayName};
  const QByteArray encrypted = readChromiumEncryptedValue(db, host, cookieName);
  if (encrypted.isEmpty())
    return {{},
           "Sessão não encontrada no " + browser->displayName + ". Faça login em " + host +
               " nesse navegador e tente de novo.",
           browser->displayName};
  const QByteArray decrypted =
      decryptChromiumValue(encrypted, fetchChromiumKeyringSecret(browser->keyringApplication));
  if (decrypted.isEmpty())
    return {{},
           "Não foi possível decifrar o cookie do " + browser->displayName +
               " (formato de criptografia não suportado ou chave do keyring indisponível).",
           browser->displayName};
  return {QString::fromUtf8(decrypted), {}, browser->displayName};
}
#endif
