#pragma once

#include <QString>

// Applies "auto" / "light" / "dark" to the whole application.
void applyAppTheme(const QString& theme);
QString effectiveTheme(const QString& theme);
