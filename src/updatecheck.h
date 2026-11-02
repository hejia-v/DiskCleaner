#pragma once

#include <QString>

namespace wt {

struct UpdateInfo {
    bool ok = false;
    QString version;   // "VERSION|..." value
    QString infoUrl;   // "INFOURL|..." value
    QString error;
};

// GETs `url`, which must answer with plain-text lines "VERSION|x.y.z" and
// optionally "INFOURL|https://...". Blocking (used by the UI action and CLI).
UpdateInfo checkForUpdates(const QString& url, int timeoutMs = 6000);

// Numeric dotted-version comparison: returns true when a > b.
bool isNewerVersion(const QString& a, const QString& b);

}  // namespace wt
