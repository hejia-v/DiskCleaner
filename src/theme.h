#pragma once

#include <QString>
#include <QIcon>

enum class AppIcon { Drive, Folder, Play, Info, Help, Back, Forward, Up, Home, Copy, Search, Settings, Close };

// Vector icons follow the current palette, including disabled and primary-button states.
QIcon appIcon(AppIcon icon, bool primary = false);

// Applies "auto" / "light" / "dark" to the whole application.
void applyAppTheme(const QString& theme);
QString effectiveTheme(const QString& theme);
