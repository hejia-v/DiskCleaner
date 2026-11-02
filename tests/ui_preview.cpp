// Optional UI evidence runner. Uses isolated settings and never accepts a file-operation dialog.
#include <QApplication>
#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QTranslator>
#include <QtTest>

#include "i18n.h"
#include "mainwindow.h"
#include "theme.h"
#include "treemap.h"

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() != 3) {
        qCritical("Usage: UiPreview <existing-sample-folder> <output-folder>");
        return 2;
    }
    if (!QDir(args[1]).exists()) return 2;
    QTemporaryDir settings;
    if (!settings.isValid() || !QDir().mkpath(args[2])) return 2;
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    std::unique_ptr<QTranslator> translator(createAppTranslator(QStringLiteral("zh_CN")));
    app.installTranslator(translator.get());
    app.setProperty("language", QStringLiteral("zh_CN"));
    applyAppTheme(QStringLiteral("light"));
    MainWindow window;
    window.resize(1080, 680);
    window.setBatchMode(true);
    window.show();
    bool done = false;
    window.setScanDoneCallback([&] { done = true; });
    window.scanPathFromCli(args[1]);
    if (!QTest::qWaitFor([&] { return done; }, 10000)) return 1;
    bool ok = true;
    auto capture = [&](QWidget* widget, const QString& name) {
        QTest::qWait(80);
        const auto path = QDir(args[2]).filePath(name + QStringLiteral(".png"));
        if (!widget || !widget->grab().save(path)) { ok = false; return; }
        qInfo().noquote() << "Captured" << name;
    };
    auto dialog = [&](const char* slot, const QString& name) {
        bool captured = false;
        QTimer::singleShot(100, &window, [&] {
            auto* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!modal) { ok = false; return; }
            capture(modal, name);
            captured = true;
            modal->reject();
        });
        if (!QMetaObject::invokeMethod(&window, slot, Qt::DirectConnection)) ok = false;
        ok = captured && ok;
    };
    auto* options = window.findChild<QToolButton*>("chartOptions");
    auto* detail = window.findChild<QSpinBox*>("treemapDetail");
    auto* chart = window.findChild<TreemapWidget*>("treemap");
    if (!options || !detail || !chart) return 1;
    options->menu()->popup(options->mapToGlobal(QPoint(0, options->height())));
    detail->setFocus();
    const int previousDetail = detail->value();
    QTest::keyClick(detail, Qt::Key_Up);
    ok = detail->value() == previousDetail + 1 && chart->detailLevel() == detail->value() && ok;
    auto* classes = options->menu()->findChild<QCheckBox*>();
    if (!classes) return 1;
    QTest::mouseClick(classes, Qt::LeftButton);
    ok = classes->isChecked() && chart->fileClassesStyle() && ok;
    capture(options->menu(), "options");
    options->menu()->hide();
    classes->setChecked(false);
    detail->setValue(previousDetail);

    auto* breadcrumb = window.findChild<QToolButton*>("treemapPath");
    breadcrumb->menu()->popup(breadcrumb->mapToGlobal(QPoint(0, breadcrumb->height())));
    capture(breadcrumb->menu(), "breadcrumb-menu");
    breadcrumb->menu()->hide();
    auto* fileMenu = window.menuBar()->actions().first()->menu();
    fileMenu->popup(window.mapToGlobal(QPoint(12, 26)));
    capture(fileMenu, "file-menu");
    fileMenu->hide();

    auto* types = window.findChild<QPushButton*>("fileTypesButton");
    types->setChecked(true);
    capture(&window, "file-types");
    types->setChecked(false);

    dialog("exportCsv", "export-options");
    dialog("exportTreemapPng", "image-export");
    dialog("showAbout", "about");
    bool locationsCaptured = false;
    QTimer::singleShot(100, &window, [&] {
        auto* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!modal) { ok = false; return; }
        capture(modal, "multiple-locations");
        modal->reject();
        locationsCaptured = true;
    });
    window.selectMultiple();
    ok = locationsCaptured && ok;

    for (auto* action : window.findChildren<QAction*>())
        if (action->shortcut() == QKeySequence(QStringLiteral("Ctrl+Shift+F"))) action->setChecked(true);
    capture(&window, "filters");
    window.setTabIndex(1);
    auto combos = window.findChildren<QComboBox*>();
    for (auto* combo : combos) {
        if (combo->isVisible() && combo->count() == 5) {
            combo->showPopup();
            capture(combo->view()->window(), "dropdown");
            combo->hidePopup();
            break;
        }
    }
    applyAppTheme(QStringLiteral("dark"));
    dialog("exportCsv", "export-options-dark");
    window.close();
    qInfo() << "UI interaction checks:" << (ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
