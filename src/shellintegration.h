#pragma once

#include <QString>

namespace wt {

// Adds/removes "Scan with DiskCleaner" entries under HKCU\Software\Classes for
// directories, folder backgrounds and drives. Returns false and fills `error`
// when a registry operation fails.
bool installShellIntegration(const QString& exePath, QString& error);
bool removeShellIntegration(QString* error = nullptr);
bool isShellIntegrationInstalled();

}  // namespace wt
