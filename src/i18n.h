#pragma once

#include <QString>

class QTranslator;

// Returns a translator for the requested language ("en" returns nullptr) or
// nullptr when the language has no dictionary.
QTranslator* createAppTranslator(const QString& language);
