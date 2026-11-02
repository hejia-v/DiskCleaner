#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "shellintegration.h"

#include <QDir>

namespace wt {
namespace {

const wchar_t* kMenuText = L"Scan with DiskCleaner";

struct Entry {
    const wchar_t* key;   // relative to HKCU\Software\Classes
    const wchar_t* arg;   // %1 or %V
};

const Entry kEntries[] = {
    {L"directory\\shell\\WizTree", L"\"%1\""},
    {L"Directory\\shell\\WizTree", L"\"%1\""},
    {L"Folder\\shell\\WizTree", L"\"%1\""},
    {L"Drive\\shell\\WizTree", L"\"%1\""},
    {L"directory\\Background\\shell\\WizTree", L"\"%V\""},
};

bool setDefaultString(HKEY root, const std::wstring& subkey, const wchar_t* value)
{
    HKEY key = nullptr;
    if (RegCreateKeyExW(root, subkey.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS)
        return false;
    const DWORD bytes = DWORD((wcslen(value) + 1) * sizeof(wchar_t));
    const LONG rc = RegSetValueExW(key, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(value),
                                   bytes);
    RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}

std::wstring classesPath(const wchar_t* rel)
{
    std::wstring s = L"Software\\Classes\\";
    s += rel;
    return s;
}

}  // namespace

bool installShellIntegration(const QString& exePath, QString& error)
{
    if (exePath.isEmpty()) {
        error = QStringLiteral("empty executable path");
        return false;
    }
    const std::wstring exe = QDir::toNativeSeparators(exePath).toStdWString();
    for (const Entry& e : kEntries) {
        const std::wstring key = classesPath(e.key);
        if (!setDefaultString(HKEY_CURRENT_USER, key, kMenuText)) {
            error = QStringLiteral("cannot write %1").arg(QString::fromStdWString(key));
            return false;
        }
        const std::wstring command = QStringLiteral("\"%1\" %2")
                                         .arg(QString::fromStdWString(exe),
                                              QString::fromStdWString(e.arg))
                                         .toStdWString();
        if (!setDefaultString(HKEY_CURRENT_USER, key + L"\\command", command.c_str())) {
            error = QStringLiteral("cannot write %1\\command").arg(QString::fromStdWString(key));
            return false;
        }
    }
    return true;
}

bool removeShellIntegration(QString* error)
{
    bool ok = true;
    for (const Entry& e : kEntries) {
        const std::wstring key = classesPath(e.key);
        const LONG rc = RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str());
        if (rc != ERROR_SUCCESS && rc != ERROR_FILE_NOT_FOUND) {
            ok = false;
            if (error) *error = QStringLiteral("cannot delete %1").arg(QString::fromStdWString(key));
        }
    }
    return ok;
}

bool isShellIntegrationInstalled()
{
    HKEY key = nullptr;
    const std::wstring path = classesPath(kEntries[0].key);
    const LONG rc = RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_READ, &key);
    if (rc != ERROR_SUCCESS) return false;
    RegCloseKey(key);
    return true;
}

}  // namespace wt
