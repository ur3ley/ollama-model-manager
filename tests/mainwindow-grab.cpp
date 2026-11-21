// Dev aid: render the main window offscreen and save a PNG.
//
//   QT_QPA_PLATFORM=offscreen ./mainwindow-grab /tmp/main.png [tabIndex]
//
// Optional arguments after the tab index:
//   argv[4]  row to select (installed list, or the model card of a hub tab)
//   argv[5]  "adv"   — expand "Дополнительно" in the editor tab
//            "about" — hub tab: expand "О модели" (readme)
//            "hftoken" — render the HuggingFace token dialog
//            "hfpopup" — HuggingFace tab: search the repo in argv[6], open the
//                        variant combo and save the window with the popup
//            "hfsearch" — HuggingFace tab: search the repo in argv[6] and pick
//                        the variant in argv[7] (default 0), no popup
//   argv[6]  quantization index for "adv" / the hub tab, or the repo query
//            for "hfpopup"/"hfsearch"
//   argv[7]  variant index for "hfsearch"
//
// Handy for checking the layout without opening a real window. Uses the real
// Ollama instance for the model list; no wallet data is touched.

#include "mainwindow.h"
#include "bindsourcedialog.h"
#include "modellistwidget.h"
#include "theme.h"
#include "tokenmanager.h"
#include "i18n.h"
#include "discoverpanel.h"
#include "hfclient.h"
#include "hftokendialog.h"
#include "modelbrowserpanel.h"

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QListWidget>
#include <QPainter>
#include <QProgressBar>
#include <QLabel>
#include <QMessageBox>
#include <QScreen>
#include <QTimer>
#include <QComboBox>
#include <QPushButton>
#include <QToolButton>
#include <QTabWidget>
#include <QTextStream>
#include <QTextStream>
#include <QThread>

namespace {

void spin(int ms)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QCoreApplication::processEvents();
        QThread::msleep(20);
    }
}

} // namespace

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    // Same identity as the real binary: provenance (models.json) and QSettings
    // then point at the config the application itself uses.
    QCoreApplication::setApplicationName(QStringLiteral("Ollama Model Manager"));
    QCoreApplication::setOrganizationName(QStringLiteral("OllamaManager"));
    theme::apply();

    auto *i18n = I18n::instance();
    i18n->setLanguagesDir(QCoreApplication::applicationDirPath() + "/languages");
    i18n->setLanguage(argc > 2 && QString::fromLocal8Bit(argv[2]) == QLatin1String("en")
                              ? QStringLiteral("en") : QStringLiteral("ru"));

    const QString path = argc > 1 ? QString::fromLocal8Bit(argv[1])
                                  : QStringLiteral("/tmp/mainwindow.png");
    const int tab = argc > 3 ? QString::fromLocal8Bit(argv[3]).toInt() : 0;
    const int modelRow = argc > 4 ? QString::fromLocal8Bit(argv[4]).toInt() : -1;

    TokenManager tokens;
    MainWindow window(&tokens, nullptr);
    window.resize(1180, 780);
    window.show();

    // Let the model list and the panel load.
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 2500) {
        QCoreApplication::processEvents();
        QThread::msleep(20);
    }

    if (modelRow >= 0) {
        if (auto *list = window.findChild<QListWidget *>(QStringLiteral("modelList"))) {
            if (modelRow < list->count())
                list->setCurrentRow(modelRow);
        }
        timer.restart();
        while (timer.elapsed() < 1200) {
            QCoreApplication::processEvents();
            QThread::msleep(20);
        }
    }

    const QString shotMode = argc > 5 ? QString::fromLocal8Bit(argv[5]) : QString();
    const bool hfPopup = shotMode == QLatin1String("hfpopup");
    const bool hfSearch = shotMode == QLatin1String("hfsearch");
    const QString hfQuery = argc > 6 ? QString::fromLocal8Bit(argv[6]) : QString();
    const bool expandAdvanced = shotMode == QLatin1String("adv");
    const bool expandAbout = shotMode == QLatin1String("about");

    // "hftoken": the HuggingFace token dialog on its own. No wallet write: the
    // dialog only shows the (passive) stored state, which is "не задан" here.
    if (shotMode == QLatin1String("hftoken")) {
        HfClient hf;
        HfTokenDialog dialog(&hf);
        dialog.show();
        spin(600);
        const bool dialogSaved = dialog.grab().save(path);
        QTextStream(stdout) << "token dialog\n" << (dialogSaved ? "saved\n" : "save failed\n");
        return dialogSaved ? 0 : 1;
    }
    if (expandAdvanced) {
        if (auto *btn = window.findChild<QToolButton *>(QStringLiteral("advancedBtn")))
            btn->setChecked(true);
        timer.restart();
        while (timer.elapsed() < 1200) {
            QCoreApplication::processEvents();
            QThread::msleep(20);
        }
        // Print the FROM list so the quant entries are verifiable headlessly.
        if (auto *combo = window.findChild<QComboBox *>(QStringLiteral("fromCombo"))) {
            const int pick = argc > 6 ? QString::fromLocal8Bit(argv[6]).toInt() : -1;
            if (pick >= 0 && pick < combo->count()) {
                combo->setCurrentIndex(pick);
                combo->activated(pick);
                QCoreApplication::processEvents();
            }
            QTextStream out(stdout);
            out << "FROM items:\n";
            for (auto *button : window.findChildren<QPushButton *>()) {
                const QString text = button->text();
                if (text.contains(QLatin1String("модель")) || text.contains(QLatin1String("Установить"))
                        || text.contains(QLatin1String("Install")) || text.contains(QLatin1String("Update"))) {
                    out << "button: " << text << "\n";
                }
            }
            for (int i = 0; i < combo->count(); ++i) {
                if (combo->itemText(i).trimmed().isEmpty()) {
                    out << "  --- separator ---\n";
                    continue;
                }
                out << "  " << combo->itemText(i)
                    << "  [" << combo->itemData(i).toString() << "]\n";
            }
        }
    }

    // "frompopup": open the editor's FROM picker and composite the popup (it is
    // a separate top-level window) onto the main-window grab.
    if (shotMode == QLatin1String("frompopup")) {
        if (auto *combo = window.findChild<QComboBox *>(QStringLiteral("fromCombo"))) {
            QTextStream(stdout) << "combo count=" << combo->count()
                                << " current=" << combo->currentIndex()
                                << " text=" << combo->currentText() << "\n";
            combo->showPopup();
            spin(800);
            QWidget *popup = combo->view()->window();
            QPixmap shot = window.grab();
            if (popup && popup->isVisible()) {
                const QPoint at = popup->mapToGlobal(QPoint(0, 0))
                        - window.mapToGlobal(QPoint(0, 0));
                QPainter painter(&shot);
                painter.drawPixmap(at, popup->grab());
                QTextStream(stdout) << "popup " << popup->width() << "x" << popup->height()
                                    << " at " << at.x() << "," << at.y() << "\n";
            }
            const bool ok = shot.save(path);
            QTextStream(stdout) << (ok ? "saved\n" : "save failed\n");
            return ok ? 0 : 1;
        }
    }

    if (auto *tabs = window.findChild<QTabWidget *>()) {
        if (tab >= 0 && tab < tabs->count())
            tabs->setCurrentIndex(tab);
        timer.restart();
        while (timer.elapsed() < 1500) {
            QCoreApplication::processEvents();
            QThread::msleep(20);
        }

        // Hub tabs load asynchronously: wait for the cards, then for the tag
        // list of the selected model, then for the readme of the family.
        if (auto *discover = qobject_cast<DiscoverPanel *>(tabs->currentWidget())) {
            if ((hfPopup || hfSearch) && !hfQuery.isEmpty()) {
                // The tab already started the "popular" search; this aborts it
                // and asks for the repository the screenshot is about.
                discover->searchAs(hfQuery);
            }
            timer.restart();
            while (discover->modelCount() == 0 && timer.elapsed() < 20000) {
                QCoreApplication::processEvents();
                QThread::msleep(50);
            }
            if (discover->modelCount() > 0)
                discover->selectModel(modelRow >= 0 ? modelRow : 0);
            timer.restart();
            while (discover->tagCount() == 0 && timer.elapsed() < 20000) {
                QCoreApplication::processEvents();
                QThread::msleep(50);
            }
            if (discover->tagCount() > 0) {
                const int tagIndex = hfSearch
                        ? (argc > 7 ? QString::fromLocal8Bit(argv[7]).toInt() : 0)
                        : (argc > 6 && !hfPopup
                                   ? QString::fromLocal8Bit(argv[6]).toInt() : 0);
                discover->selectTag(tagIndex);
            }

            if (expandAbout) {
                if (auto *panel = discover->findChild<ModelBrowserPanel *>())
                    panel->setAboutExpanded(true);
            }
            // Readme + tag details are two more round trips.
            spin(2500);

            QTextStream out(stdout);
            out << "models: " << discover->modelCount()
                << ", variants: " << discover->tagCount()
                << ", selected: " << discover->suggestedName() << "\n";
            if (auto *panel = discover->findChild<ModelBrowserPanel *>()) {
                if (auto *combo = panel->findChild<QComboBox *>()) {
                    const int shown = qMin(combo->count(), 40);
                    for (int i = 0; i < shown; ++i)
                        out << "  [" << i << "] " << combo->itemText(i) << "\n";

                    // "hfpopup": open the variant picker and capture it together
                    // with the window (the popup is a separate top-level widget,
                    // a plain window.grab() would miss it).
                    if (hfPopup) {
                        if (auto *status = panel->findChild<QLabel *>(
                                    QStringLiteral("browserStatus")))
                            out << "status: " << status->text() << "\n";
                        combo->showPopup();
                        spin(800);
                        QWidget *popup = combo->view()->window();
                        QPixmap shot = window.grab();
                        if (popup && popup->isVisible()) {
                            const QPoint at = popup->mapToGlobal(QPoint(0, 0))
                                    - window.mapToGlobal(QPoint(0, 0));
                            const QRect avail = popup->screen()
                                    ? popup->screen()->availableGeometry() : QRect();
                            out << "popup: " << popup->width() << "x" << popup->height()
                                << " at " << at.x() << "," << at.y()
                                << ", screen " << avail.width() << "x" << avail.height()
                                << ", maxVisible " << combo->maxVisibleItems() << "\n";
                            QPainter painter(&shot);
                            painter.drawPixmap(at, popup->grab());
                        }
                        const bool shotSaved = shot.save(path);
                        out << (shotSaved ? "saved\n" : "save failed\n");
                        return shotSaved ? 0 : 1;
                    }
                }
            }
        }
    }

    // argv[5] = "bind" (or "bindauto"): screenshot the source-binding dialog
    // for a local card; argv[6] optionally picks the model by name.
    if (shotMode == QLatin1String("bind") || shotMode == QLatin1String("bindauto")
            || shotMode == QLatin1String("bindhf")) {
        QString modelName;
        QString digest;
        auto *list = window.findChild<ModelListWidget *>();
        const QString wanted = argc > 6 ? QString::fromLocal8Bit(argv[6]) : QString();
        if (list && !wanted.isEmpty()) {
            const RepoGroup group = list->groupOfModel(wanted);
            if (!group.quants.isEmpty()) {
                modelName = wanted;
                digest = group.quants.first().digest;
            }
        }
        if (list && modelName.isEmpty()) {
            for (const RepoGroup &group : list->groups()) {
                if (!group.local || group.quants.isEmpty())
                    continue;
                modelName = group.quants.first().modelName;
                digest = group.quants.first().digest;
                break;
            }
        }
        BindSourceDialog dialog(modelName, digest);
        dialog.show();
        spin(1500);
        if (shotMode == QLatin1String("bindhf")) {
            if (auto *tabs = dialog.findChild<QTabWidget *>())
                tabs->setCurrentIndex(1);
            spin(6000);
            QTextStream out(stdout);
            if (auto *files = dialog.findChild<QListWidget *>(QStringLiteral("bindHfFiles")))
                out << "hf files: " << files->count() << "\n";
        }
        if (shotMode == QLatin1String("bindauto")) {
            // The auto pass ends with a modal report; offscreen there is nobody
            // to click it, so close message boxes from a timer.
            QTimer msgCloser;
            msgCloser.setInterval(400);
            QObject::connect(&msgCloser, &QTimer::timeout, []() {
                for (QWidget *w : QApplication::topLevelWidgets()) {
                    if (auto *box = qobject_cast<QMessageBox *>(w)) {
                        if (box->isVisible())
                            box->done(QMessageBox::Ok);
                    }
                }
            });
            msgCloser.start();
            if (auto *autoBtn = dialog.findChild<QPushButton *>(QStringLiteral("bindAutoBtn")))
                autoBtn->click();
            if (auto *bar = dialog.findChild<QProgressBar *>(QStringLiteral("bindAutoBar"))) {
                QElapsedTimer wait;
                wait.start();
                while (bar->isVisible() && wait.elapsed() < 180000) {
                    QCoreApplication::processEvents();
                    QThread::msleep(50);
                }
            }
            spin(400);
            QTextStream out(stdout);
            if (auto *status = dialog.findChild<QLabel *>(QStringLiteral("bindStatus")))
                out << "status: " << status->text() << "\n";
            if (auto *autoLabel = dialog.findChild<QLabel *>(QStringLiteral("bindAutoLabel")))
                out << "auto: " << autoLabel->text() << "\n";
        }
        const bool dialogSaved = dialog.grab().save(path);
        QTextStream(stdout) << "bind dialog for: " << modelName << "\n"
                            << (dialogSaved ? "saved\n" : "save failed\n");
        return dialogSaved ? 0 : 1;
    }

    const bool saved = window.grab().save(path);
    QTextStream(stdout) << (saved ? "saved\n" : "save failed\n");
    return saved ? 0 : 1;
}
