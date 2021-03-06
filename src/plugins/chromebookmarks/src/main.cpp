// albert - a simple application launcher for linux
// Copyright (C) 2014-2016 Manuel Schneider
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.


#include <QApplication>
#include <QClipboard>
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QtConcurrent>
#include <QTimer>
#include <QUrl>
#include <functional>
#include <memory>
#include <vector>
#include "configwidget.h"
#include "main.h"
#include "indexable.h"
#include "offlineindex.h"
#include "query.h"
#include "queryhandler.h"
#include "standardaction.h"
#include "standardindexitem.h"
#include "xdgiconlookup.h"
using std::shared_ptr;
using std::vector;
using namespace Core;

namespace {

const char* CFG_PATH  = "bookmarkfile";
const char* CFG_FUZZY = "fuzzy";
const bool  DEF_FUZZY = false;

/** ***************************************************************************/
vector<shared_ptr<StandardIndexItem>> indexChromeBookmarks(const QString &bookmarksPath) {

    // Build a new index
    vector<shared_ptr<StandardIndexItem>> bookmarks;

    // Define a recursive bookmark indexing lambda
    std::function<void(const QJsonObject &json)> rec_bmsearch =
            [&rec_bmsearch, &bookmarks](const QJsonObject &json) {
        QJsonValue type = json["type"];
        if (type == QJsonValue::Undefined)
            return;
        if (type.toString() == "folder"){
            QJsonArray jarr = json["children"].toArray();
            for (const QJsonValue &i : jarr)
                rec_bmsearch(i.toObject());
        }
        if (type.toString() == "url") {
            QString name = json["name"].toString();
            QString urlstr = json["url"].toString();

            shared_ptr<StandardIndexItem> ssii  = std::make_shared<StandardIndexItem>(json["id"].toString());
            ssii->setText(name);
            ssii->setSubtext(urlstr);
            QString icon = XdgIconLookup::instance()->themeIconPath("www");
            if (icon.isEmpty())
                icon = XdgIconLookup::instance()->themeIconPath("web-browser");
            if (icon.isEmpty())
                icon = XdgIconLookup::instance()->themeIconPath("emblem-web");
            if (icon.isEmpty())
                icon = ":favicon";
            ssii->setIconPath(icon);

            vector<Indexable::WeightedKeyword> weightedKeywords;
            QUrl url(urlstr);
            QString host = url.host();
            weightedKeywords.emplace_back(name, USHRT_MAX);
            weightedKeywords.emplace_back(host.left(host.size()-url.topLevelDomain().size()), USHRT_MAX/2);
            ssii->setIndexKeywords(std::move(weightedKeywords));

            vector<shared_ptr<Action>> actions;
            shared_ptr<StandardAction> action = std::make_shared<StandardAction>();
            action->setText("Open in default browser");
            action->setAction([urlstr](){
                QDesktopServices::openUrl(QUrl(urlstr));
            });
            actions.push_back(std::move(action));

            action = std::make_shared<StandardAction>();
            action->setText("Copy url to clipboard");
            action->setAction([urlstr](){
                QApplication::clipboard()->setText(urlstr);
            });
            actions.push_back(std::move(action));

            ssii->setActions(std::move(actions));

            bookmarks.push_back(std::move(ssii));
        }
    };

    QFile f(bookmarksPath);
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning() << qPrintable(QString("Could not open %1").arg(bookmarksPath));
        return vector<shared_ptr<StandardIndexItem>>();
    }

    QJsonObject json = QJsonDocument::fromJson(f.readAll()).object();
    QJsonObject roots = json.value("roots").toObject();
    for (const QJsonValue &i : roots)
        if (i.isObject())
            rec_bmsearch(i.toObject());

    f.close();

    return bookmarks;
}

}



/** ***************************************************************************/
/** ***************************************************************************/
/** ***************************************************************************/
/** ***************************************************************************/
class ChromeBookmarks::ChromeBookmarksPrivate
{
public:
    ChromeBookmarksPrivate(Extension *q) : q(q) {}

    Extension *q;

    QPointer<ConfigWidget> widget;
    QFileSystemWatcher fileSystemWatcher;
    QString bookmarksFile;

    vector<shared_ptr<Core::StandardIndexItem>> index;
    Core::OfflineIndex offlineIndex;
    QFutureWatcher<vector<shared_ptr<Core::StandardIndexItem>>> futureWatcher;

    void finishIndexing();
    void startIndexing();
};



/** ***************************************************************************/
void ChromeBookmarks::ChromeBookmarksPrivate::startIndexing() {

    // Never run concurrent
    if ( futureWatcher.future().isRunning() )
        return;

    // Run finishIndexing when the indexing thread finished
    futureWatcher.disconnect();
    QObject::connect(&futureWatcher, &QFutureWatcher<vector<shared_ptr<Core::StandardIndexItem>>>::finished,
                     std::bind(&ChromeBookmarksPrivate::finishIndexing, this));

    // Run the indexer thread
    futureWatcher.setFuture(QtConcurrent::run(indexChromeBookmarks, bookmarksFile));

    // Notification
    qDebug() << qPrintable(QString("[%1] Start indexing in background thread.").arg(q->Core::Extension::id));
    emit q->statusInfo("Indexing bookmarks ...");

}



/** ***************************************************************************/
void ChromeBookmarks::ChromeBookmarksPrivate::finishIndexing() {

    // Get the thread results
    index = futureWatcher.future().result();

    // Rebuild the offline index
    offlineIndex.clear();
    for (const auto &item : index)
        offlineIndex.add(item);

    /*
     * Finally update the watches (maybe folders changed)
     * Note that QFileSystemWatcher stops monitoring files once they have been
     * renamed or removed from disk, and directories once they have been removed
     * from disk.
     * Chromium seems to mv the file (inode change).
     */
    if ( fileSystemWatcher.files().empty() )
        if( !fileSystemWatcher.addPath(bookmarksFile))
            qWarning() << qPrintable(QString("%1 could not be watched. Changes in this path will not be noticed.").arg(bookmarksFile));

    // Notification
    qDebug() << qPrintable(QString("[%1] Indexing done (%2 items).").arg(q->Core::Extension::id).arg(index.size()));
    emit q->statusInfo(QString("%1 bookmarks indexed.").arg(index.size()));
}



/** ***************************************************************************/
/** ***************************************************************************/
/** ***************************************************************************/
/** ***************************************************************************/
ChromeBookmarks::Extension::Extension()
    : Core::Extension("org.albert.extension.chromebookmarks"),
      Core::QueryHandler(Core::Extension::id),
      d(new ChromeBookmarksPrivate(this)) {

    // Load settings
    QSettings s(qApp->applicationName());
    s.beginGroup(Core::Extension::id);
    d->offlineIndex.setFuzzy(s.value(CFG_FUZZY, DEF_FUZZY).toBool());

    // Load and set a valid path
    QVariant v = s.value(CFG_PATH);
    if (v.isValid() && v.canConvert(QMetaType::QString) && QFileInfo(v.toString()).exists())
        setPath(v.toString());
    else
        restorePath();

    // If the path changed write it to the settings
    connect(this, &Extension::pathChanged, [this](const QString& path){
        QSettings(qApp->applicationName()).setValue(QString("%1/%2").arg(Core::Extension::id, CFG_PATH), path);
    });

    s.endGroup();

    // Update index if bookmark file changed
    connect(&d->fileSystemWatcher, &QFileSystemWatcher::fileChanged,
            this, &Extension::updateIndex);

    // Update index if bookmark file's path changed
    connect(this, &Extension::pathChanged,
            this, &Extension::updateIndex);

    // Trigger an initial update
    updateIndex();
}



/** ***************************************************************************/
ChromeBookmarks::Extension::~Extension() {
    delete d;
}



/** ***************************************************************************/
QWidget *ChromeBookmarks::Extension::widget(QWidget *parent) {
    if (d->widget.isNull()){
        d->widget = new ConfigWidget(parent);

        // Paths
        d->widget->ui.lineEdit_path->setText(d->bookmarksFile);
        connect(d->widget.data(), &ConfigWidget::requestEditPath, this, &Extension::setPath);
        connect(this, &Extension::pathChanged, d->widget->ui.lineEdit_path, &QLineEdit::setText);

        // Fuzzy
        d->widget->ui.checkBox_fuzzy->setChecked(fuzzy());
        connect(d->widget->ui.checkBox_fuzzy, &QCheckBox::toggled, this, &Extension::setFuzzy);

        // Info
        d->widget->ui.label_info->setText(QString("%1 bookmarks indexed.").arg(d->index.size()));
        connect(this, &Extension::statusInfo, d->widget->ui.label_info, &QLabel::setText);
    }
    return d->widget;
}



/** ***************************************************************************/
void ChromeBookmarks::Extension::handleQuery(Core::Query * query) {

    // Search for matches
    const vector<shared_ptr<Core::Indexable>> &indexables = d->offlineIndex.search(query->searchTerm().toLower());

    // Add results to query
    vector<pair<shared_ptr<Core::Item>,short>> results;
    for (const shared_ptr<Core::Indexable> &item : indexables)
        results.emplace_back(std::static_pointer_cast<Core::StandardIndexItem>(item), 0);

    query->addMatches(results.begin(), results.end());
}



/** ***************************************************************************/
const QString &ChromeBookmarks::Extension::path() {
    return d->bookmarksFile;
}



/** ***************************************************************************/
void ChromeBookmarks::Extension::setPath(const QString &path) {

    QFileInfo fi(path);
    if (!(fi.exists() && fi.isFile()))
        return;

    d->bookmarksFile = path;

    emit pathChanged(path);
}



/** ***************************************************************************/
void ChromeBookmarks::Extension::restorePath() {
    // Find a bookmark file (Take first one)
    for (const QString &browser : {"chromium","google-chrome"}){
        QString root = QDir(QStandardPaths::writableLocation(QStandardPaths::ConfigLocation)).filePath(browser);
        QDirIterator it(root, {"Bookmarks"}, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            setPath(it.next());
            return;
        }
    }
}



/** ***************************************************************************/
bool ChromeBookmarks::Extension::fuzzy() {
    return d->offlineIndex.fuzzy();
}



/** ***************************************************************************/
void ChromeBookmarks::Extension::updateIndex() {
    d->startIndexing();
}



/** ***************************************************************************/
void ChromeBookmarks::Extension::setFuzzy(bool b) {
    QSettings(qApp->applicationName()).setValue(QString("%1/%2").arg(Core::Extension::id, CFG_FUZZY), b);
    d->offlineIndex.setFuzzy(b);
}

