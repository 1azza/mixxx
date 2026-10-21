// [AI-generated: this file was written by an AI agent (Claude) on request of the
// fork owner. It is not part of upstream Mixxx.]

#include "library/youtube/youtubetrackmodel.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUrl>
#include <algorithm>

#include "control/controlobject.h"
#include "library/trackcollection.h"
#include "library/trackcollectionmanager.h"
#include "mixer/playermanager.h"
#include "moc_youtubetrackmodel.cpp"
#include "preferences/dialog/dlgprefdeck.h"
#include "track/track.h"

namespace {

const QString kConfigGroup = QStringLiteral("[YouTube]");
const ConfigKey kDownloadDirKey(kConfigGroup, QStringLiteral("DownloadDir"));
const ConfigKey kYtDlpPathKey(kConfigGroup, QStringLiteral("YtDlpPath"));

constexpr int kSearchResults = 30;
constexpr int kSearchDelayMs = 600;
constexpr int kMaxParallelDownloads = 3;

const QString kResultFilePrefix = QStringLiteral("MIXXX_FILE:");

const QStringList kAudioFileFilters = {
        QStringLiteral("*.mp3"),
        QStringLiteral("*.m4a"),
        QStringLiteral("*.ogg"),
        QStringLiteral("*.opus"),
        QStringLiteral("*.flac"),
        QStringLiteral("*.wav"),
};

#ifdef Q_OS_WIN
const QString kYtDlpExe = QStringLiteral("yt-dlp.exe");
const QString kFfmpegExe = QStringLiteral("ffmpeg.exe");
#else
const QString kYtDlpExe = QStringLiteral("yt-dlp");
const QString kFfmpegExe = QStringLiteral("ffmpeg");
#endif

QString youTubeKey(const QString& videoId) {
    return QStringLiteral("yt:") + videoId;
}

QString formatDuration(double seconds) {
    if (seconds <= 0) {
        return QString();
    }
    const int total = static_cast<int>(seconds + 0.5);
    if (total >= 3600) {
        return QStringLiteral("%1:%2:%3")
                .arg(total / 3600)
                .arg((total / 60) % 60, 2, 10, QChar('0'))
                .arg(total % 60, 2, 10, QChar('0'));
    }
    return QStringLiteral("%1:%2").arg(total / 60).arg(total % 60, 2, 10, QChar('0'));
}

/// "[Channel2]" -> "Deck 2" etc.
QString groupName(const QString& group) {
    static const QRegularExpression kGroup(
            QStringLiteral("^\\[(Channel|Sampler|PreviewDeck)(\\d+)\\]$"));
    const auto match = kGroup.match(group);
    if (!match.hasMatch()) {
        return group;
    }
    if (match.captured(1) == QStringLiteral("Channel")) {
        return QObject::tr("Deck %1").arg(match.captured(2));
    }
    if (match.captured(1) == QStringLiteral("Sampler")) {
        return QObject::tr("Sampler %1").arg(match.captured(2));
    }
    return QObject::tr("Preview Deck");
}

QProcessEnvironment ytDlpEnvironment() {
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    // Make sure non-ASCII titles and paths survive on Windows consoles
    env.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    return env;
}

} // anonymous namespace

YouTubeTrackModel::YouTubeTrackModel(QObject* pParent,
        UserSettingsPointer pConfig,
        TrackCollectionManager* pTrackCollectionManager)
        : QAbstractTableModel(pParent),
          TrackModel(pTrackCollectionManager->internalCollection()->database(),
                  "mixxx.db.model.youtube"),
          m_pConfig(std::move(pConfig)),
          m_pTrackCollectionManager(pTrackCollectionManager),
          m_sortColumn(-1),
          m_sortOrder(Qt::AscendingOrder) {
    m_searchTimer.setSingleShot(true);
    m_searchTimer.setInterval(kSearchDelayMs);
    connect(&m_searchTimer, &QTimer::timeout, this, &YouTubeTrackModel::startSearch);
    showDownloads();
}

YouTubeTrackModel::~YouTubeTrackModel() {
    if (m_pSearchProcess) {
        m_pSearchProcess->disconnect(this);
        m_pSearchProcess->kill();
        m_pSearchProcess->waitForFinished(1000);
    }
    for (const auto& pJob : std::as_const(m_jobs)) {
        if (pJob->pProcess) {
            pJob->pProcess->disconnect(this);
            pJob->pProcess->kill();
            pJob->pProcess->waitForFinished(1000);
        }
    }
}

// ---------------------------------------------------------------------------
// Tools and folders
// ---------------------------------------------------------------------------

QString YouTubeTrackModel::toolsDir() const {
    return QDir(m_pConfig->getSettingsPath()).filePath(QStringLiteral("tools"));
}

QString YouTubeTrackModel::findYtDlp() const {
    const QString configured = m_pConfig->getValueString(kYtDlpPathKey);
    if (!configured.isEmpty() && QFileInfo(configured).isExecutable()) {
        return configured;
    }
    const QStringList candidates = {
            QDir(QCoreApplication::applicationDirPath()).filePath(kYtDlpExe),
            QDir(toolsDir()).filePath(kYtDlpExe),
    };
    for (const auto& candidate : candidates) {
        if (QFileInfo(candidate).isExecutable()) {
            return candidate;
        }
    }
    return QStandardPaths::findExecutable(QStringLiteral("yt-dlp"));
}

QString YouTubeTrackModel::findFfmpeg() const {
    QStringList candidates;
    const QString ytDlp = findYtDlp();
    if (!ytDlp.isEmpty()) {
        candidates << QFileInfo(ytDlp).absoluteDir().filePath(kFfmpegExe);
    }
    candidates << QDir(QCoreApplication::applicationDirPath()).filePath(kFfmpegExe);
    candidates << QDir(toolsDir()).filePath(kFfmpegExe);
    for (const auto& candidate : std::as_const(candidates)) {
        if (QFileInfo(candidate).isExecutable()) {
            return candidate;
        }
    }
    return QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
}

void YouTubeTrackModel::setYtDlpPath(const QString& path) {
    m_pConfig->setValue(kYtDlpPathKey, path);
}

QString YouTubeTrackModel::downloadDir() const {
    QString dir = m_pConfig->getValueString(kDownloadDirKey);
    if (dir.isEmpty()) {
        dir = QDir(QStandardPaths::writableLocation(QStandardPaths::MusicLocation))
                      .filePath(QStringLiteral("Mixxx YouTube"));
    }
    return dir;
}

void YouTubeTrackModel::setDownloadDir(const QString& dir) {
    m_pConfig->setValue(kDownloadDirKey, dir);
    refreshDownloads();
}

// ---------------------------------------------------------------------------
// Searching
// ---------------------------------------------------------------------------

void YouTubeTrackModel::search(const QString& searchText) {
    const QString text = searchText.trimmed();
    if (text == m_search) {
        return;
    }
    m_search = text;
    m_searchTimer.stop();
    if (m_pSearchProcess) {
        m_pSearchProcess->disconnect(this);
        m_pSearchProcess->kill();
        m_pSearchProcess->deleteLater();
    }
    if (m_search.isEmpty()) {
        showDownloads();
        return;
    }
    // Wait until the user stops typing, every search starts a process
    m_searchTimer.start();
}

const QString YouTubeTrackModel::currentSearch() const {
    return m_search;
}

void YouTubeTrackModel::retrySearch() {
    if (m_search.isEmpty()) {
        showDownloads();
    } else {
        startSearch();
    }
}

void YouTubeTrackModel::refreshDownloads() {
    if (m_search.isEmpty()) {
        showDownloads();
    }
}

void YouTubeTrackModel::startSearch() {
    if (m_search.isEmpty()) {
        return;
    }
    const QString ytDlp = findYtDlp();
    if (ytDlp.isEmpty()) {
        showInfo(tr("YouTube search needs yt-dlp. Right-click \"YouTube\" in the "
                    "sidebar and choose \"Download yt-dlp\"."));
        emit ytDlpMissing();
        return;
    }
    const bool isUrl = m_search.startsWith(QStringLiteral("http://")) ||
            m_search.startsWith(QStringLiteral("https://"));
    const QString target = isUrl
            ? m_search
            : QStringLiteral("ytsearch%1:%2").arg(kSearchResults).arg(m_search);

    auto* pProcess = new QProcess(this);
    m_pSearchProcess = pProcess;
    pProcess->setProcessEnvironment(ytDlpEnvironment());
    pProcess->setProgram(ytDlp);
    pProcess->setArguments({
            QStringLiteral("--flat-playlist"),
            QStringLiteral("--dump-single-json"),
            QStringLiteral("--no-warnings"),
            QStringLiteral("--playlist-end"),
            QString::number(isUrl ? 200 : kSearchResults),
            target,
    });
    showInfo(tr("Searching YouTube for \"%1\" ...").arg(m_search));

    connect(pProcess, &QProcess::errorOccurred, this, [this, pProcess](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            showInfo(tr("Could not start yt-dlp: %1").arg(pProcess->errorString()));
            pProcess->deleteLater();
        }
    });
    connect(pProcess,
            &QProcess::finished,
            this,
            [this, pProcess](int exitCode, QProcess::ExitStatus exitStatus) {
                pProcess->deleteLater();
                if (exitStatus != QProcess::NormalExit) {
                    return;
                }
                if (exitCode != 0) {
                    const QString error =
                            QString::fromUtf8(pProcess->readAllStandardError()).trimmed();
                    qWarning() << "yt-dlp search failed:" << error;
                    showInfo(tr("Search failed: %1").arg(error.section('\n', -1)));
                    return;
                }
                const QJsonObject root =
                        QJsonDocument::fromJson(pProcess->readAllStandardOutput()).object();
                QList<Row> rows;
                if (root.contains(QStringLiteral("entries"))) {
                    const auto entries = root.value(QStringLiteral("entries")).toArray();
                    for (const auto& entry : entries) {
                        Row row = rowFromEntry(entry.toObject());
                        if (!row.key.isEmpty()) {
                            rows.append(std::move(row));
                        }
                    }
                } else if (!root.isEmpty()) {
                    Row row = rowFromEntry(root);
                    if (!row.key.isEmpty()) {
                        rows.append(std::move(row));
                    }
                }
                if (rows.isEmpty()) {
                    showInfo(tr("No results for \"%1\"").arg(m_search));
                } else {
                    setRows(std::move(rows));
                }
            });
    pProcess->start();
}

YouTubeTrackModel::Row YouTubeTrackModel::rowFromEntry(const QJsonObject& entry) const {
    Row row;
    const QString id = entry.value(QStringLiteral("id")).toString();
    if (id.isEmpty()) {
        return row;
    }
    row.key = youTubeKey(id);
    row.url = entry.value(QStringLiteral("webpage_url")).toString();
    if (row.url.isEmpty()) {
        row.url = entry.value(QStringLiteral("url")).toString();
    }
    if (row.url.isEmpty()) {
        row.url = QStringLiteral("https://www.youtube.com/watch?v=") + id;
    }
    row.title = entry.value(QStringLiteral("title")).toString(row.url);
    row.channel = entry.value(QStringLiteral("channel")).toString();
    if (row.channel.isEmpty()) {
        row.channel = entry.value(QStringLiteral("uploader")).toString();
    }
    row.duration = entry.value(QStringLiteral("duration")).toDouble();
    return row;
}

void YouTubeTrackModel::showDownloads() {
    // yt-dlp names files "<title> [<video id>].<ext>", which lets us match
    // them with search results later.
    static const QRegularExpression kVideoId(QStringLiteral("^(.*) \\[([A-Za-z0-9_-]{11})\\]$"));
    QList<Row> rows;
    const auto entries = QDir(downloadDir()).entryInfoList(
            kAudioFileFilters, QDir::Files | QDir::Readable, QDir::Time);
    for (const auto& info : entries) {
        Row row;
        row.filePath = info.absoluteFilePath();
        row.title = info.completeBaseName();
        const auto match = kVideoId.match(row.title);
        if (match.hasMatch()) {
            row.title = match.captured(1);
            row.key = youTubeKey(match.captured(2));
            m_files.insert(row.key, row.filePath);
        } else {
            row.key = QStringLiteral("file:") + row.filePath;
        }
        rows.append(std::move(row));
    }
    if (rows.isEmpty()) {
        showInfo(tr("Type in the search box to search YouTube. Downloaded "
                    "tracks are listed here."));
        return;
    }
    setRows(std::move(rows));
}

void YouTubeTrackModel::showInfo(const QString& text) {
    Row row;
    row.title = text;
    setRows({row});
}

void YouTubeTrackModel::setRows(QList<Row> rows) {
    beginResetModel();
    m_rows = std::move(rows);
    endResetModel();
    if (m_sortColumn >= 0) {
        sort(m_sortColumn, m_sortOrder);
    }
}

// ---------------------------------------------------------------------------
// Table model
// ---------------------------------------------------------------------------

int YouTubeTrackModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
}

int YouTubeTrackModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : kColumnCount;
}

QString YouTubeTrackModel::filePathOfRow(int row) const {
    if (row < 0 || row >= m_rows.size()) {
        return QString();
    }
    const Row& r = m_rows.at(row);
    if (!r.filePath.isEmpty()) {
        return r.filePath;
    }
    const QString path = m_files.value(r.key);
    if (!path.isEmpty() && QFileInfo::exists(path)) {
        return path;
    }
    return QString();
}

QString YouTubeTrackModel::statusOfRow(int row) const {
    const Row& r = m_rows.at(row);
    if (r.key.isEmpty()) {
        return QString();
    }
    const auto pJob = m_jobs.value(r.key);
    QString target;
    if (pJob && pJob->loadWhenReady) {
        target = QStringLiteral(" → ") +
                (pJob->group.isEmpty() ? tr("next deck") : groupName(pJob->group));
    }
    if (!filePathOfRow(row).isEmpty()) {
        QString status = QStringLiteral("✓ ") + tr("Downloaded");
        if (pJob && !pJob->note.isEmpty()) {
            status += QStringLiteral(" (") + pJob->note + QChar(')');
        }
        return status;
    }
    if (!pJob) {
        return QString();
    }
    switch (pJob->state) {
    case JobState::Queued:
        return tr("Queued") + target;
    case JobState::Running:
        return (pJob->progress >= 100 ? tr("Converting...")
                                      : QStringLiteral("%1%").arg(pJob->progress)) +
                target;
    case JobState::Failed:
        return tr("Failed") + (pJob->note.isEmpty() ? QString() : QStringLiteral(": ") + pJob->note);
    case JobState::Done:
        break;
    }
    return QString();
}

QVariant YouTubeTrackModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= m_rows.size()) {
        return QVariant();
    }
    const Row& r = m_rows.at(index.row());
    if (role == Qt::DisplayRole || role == Qt::ToolTipRole) {
        switch (index.column()) {
        case kColumnArtist:
            return r.channel;
        case kColumnTitle:
            return r.title;
        case kColumnDuration:
            return formatDuration(r.duration);
        case kColumnStatus:
            return statusOfRow(index.row());
        case kColumnLocation:
            return role == Qt::ToolTipRole ? r.url
                                           : QDir::toNativeSeparators(
                                                     filePathOfRow(index.row()));
        default:
            return QVariant();
        }
    }
    if (role == Qt::FontRole && r.key.isEmpty()) {
        // Info rows ("Searching ...")
        QFont font;
        font.setItalic(true);
        return font;
    }
    return QVariant();
}

QVariant YouTubeTrackModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal) {
        return QAbstractTableModel::headerData(section, orientation, role);
    }
    switch (role) {
    case Qt::DisplayRole:
        switch (section) {
        case kColumnArtist:
            return tr("Channel");
        case kColumnTitle:
            return tr("Title");
        case kColumnDuration:
            return tr("Duration");
        case kColumnStatus:
            return tr("Status");
        case kColumnLocation:
            return tr("Location");
        default:
            return QVariant();
        }
    case TrackModel::kHeaderNameRole:
        switch (section) {
        case kColumnArtist:
            return QStringLiteral("artist");
        case kColumnTitle:
            return QStringLiteral("title");
        case kColumnDuration:
            return QStringLiteral("duration");
        case kColumnStatus:
            return QStringLiteral("status");
        case kColumnLocation:
            return QStringLiteral("location");
        default:
            return QVariant();
        }
    case TrackModel::kHeaderWidthRole:
        switch (section) {
        case kColumnArtist:
            return 180;
        case kColumnTitle:
            return 380;
        case kColumnDuration:
            return 70;
        case kColumnStatus:
            return 160;
        default:
            return 250;
        }
    default:
        return QVariant();
    }
}

Qt::ItemFlags YouTubeTrackModel::flags(const QModelIndex& index) const {
    Qt::ItemFlags itemFlags = QAbstractTableModel::flags(index);
    if (index.isValid() && !filePathOfRow(index.row()).isEmpty()) {
        // Downloaded tracks can be dragged onto decks
        itemFlags |= Qt::ItemIsDragEnabled;
    }
    return itemFlags;
}

QMimeData* YouTubeTrackModel::mimeData(const QModelIndexList& indexes) const {
    auto* pMimeData = new QMimeData();
    QList<QUrl> urls;
    QList<int> rows;
    for (const auto& index : indexes) {
        if (!index.isValid() || rows.contains(index.row())) {
            continue;
        }
        rows.append(index.row());
        const QString path = filePathOfRow(index.row());
        if (!path.isEmpty()) {
            urls.append(QUrl::fromLocalFile(path));
        }
    }
    pMimeData->setUrls(urls);
    return pMimeData;
}

void YouTubeTrackModel::sort(int column, Qt::SortOrder order) {
    if (column < 0 || column >= kColumnCount) {
        return;
    }
    m_sortColumn = column;
    m_sortOrder = order;
    emit layoutAboutToBeChanged();
    const auto lessThan = [this, column](const Row& a, const Row& b) {
        switch (column) {
        case kColumnArtist:
            return a.channel.localeAwareCompare(b.channel) < 0;
        case kColumnDuration:
            return a.duration < b.duration;
        case kColumnStatus:
            return m_files.contains(a.key) && !m_files.contains(b.key);
        default:
            return a.title.localeAwareCompare(b.title) < 0;
        }
    };
    if (order == Qt::AscendingOrder) {
        std::stable_sort(m_rows.begin(), m_rows.end(), lessThan);
    } else {
        std::stable_sort(m_rows.begin(), m_rows.end(), [&lessThan](const Row& a, const Row& b) {
            return lessThan(b, a);
        });
    }
    emit layoutChanged();
}

// ---------------------------------------------------------------------------
// TrackModel
// ---------------------------------------------------------------------------

TrackPointer YouTubeTrackModel::getTrack(const QModelIndex& index) const {
    const QString path = filePathOfRow(index.row());
    if (path.isEmpty()) {
        return TrackPointer();
    }
    return getTrackByRef(TrackRef::fromFilePath(path));
}

TrackPointer YouTubeTrackModel::getTrackByRef(const TrackRef& trackRef) const {
    // Like the Browse feature this adds the track to the library
    return m_pTrackCollectionManager->getOrAddTrack(trackRef);
}

bool YouTubeTrackModel::isTrackDeferred(const QModelIndex& index) const {
    return index.isValid() && index.row() < m_rows.size() &&
            !m_rows.at(index.row()).key.isEmpty() && filePathOfRow(index.row()).isEmpty();
}

void YouTubeTrackModel::loadDeferredTrack(
        const QModelIndex& index, const QString& group, bool play) {
    if (index.isValid()) {
        requestDownload(index.row(), true, group, play);
    }
}

void YouTubeTrackModel::fetchDeferredTrack(const QModelIndex& index) {
    if (index.isValid()) {
        requestDownload(index.row(), false, QString(), false);
    }
}

QUrl YouTubeTrackModel::getTrackUrl(const QModelIndex& index) const {
    const QString path = filePathOfRow(index.row());
    return path.isEmpty() ? QUrl() : QUrl::fromLocalFile(path);
}

QString YouTubeTrackModel::getTrackLocation(const QModelIndex& index) const {
    return filePathOfRow(index.row());
}

TrackId YouTubeTrackModel::getTrackId(const QModelIndex& index) const {
    const TrackPointer pTrack = getTrack(index);
    return pTrack ? pTrack->getId() : TrackId();
}

CoverInfo YouTubeTrackModel::getCoverInfo(const QModelIndex& index) const {
    Q_UNUSED(index);
    return CoverInfo();
}

const QVector<int> YouTubeTrackModel::getTrackRows(TrackId trackId) const {
    Q_UNUSED(trackId);
    return QVector<int>();
}

bool YouTubeTrackModel::isColumnInternal(int column) {
    Q_UNUSED(column);
    return false;
}

bool YouTubeTrackModel::isColumnHiddenByDefault(int column) {
    return column == kColumnLocation;
}

TrackModel::Capabilities YouTubeTrackModel::getCapabilities() const {
    return Capability::AddToTrackSet |
            Capability::AddToAutoDJ |
            Capability::LoadToDeck |
            Capability::LoadToPreviewDeck |
            Capability::LoadToSampler |
            Capability::Sorting;
}

TrackModel::SortColumnId YouTubeTrackModel::sortColumnIdFromColumnIndex(int index) const {
    switch (index) {
    case kColumnArtist:
        return SortColumnId::Artist;
    case kColumnTitle:
        return SortColumnId::Title;
    case kColumnDuration:
        return SortColumnId::Duration;
    case kColumnStatus:
        return SortColumnId::Comment;
    case kColumnLocation:
        return SortColumnId::NativeLocation;
    default:
        return SortColumnId::Invalid;
    }
}

int YouTubeTrackModel::columnIndexFromSortColumnId(TrackModel::SortColumnId sortColumn) const {
    switch (sortColumn) {
    case SortColumnId::Artist:
        return kColumnArtist;
    case SortColumnId::Title:
        return kColumnTitle;
    case SortColumnId::Duration:
        return kColumnDuration;
    case SortColumnId::Comment:
        return kColumnStatus;
    case SortColumnId::NativeLocation:
        return kColumnLocation;
    default:
        return -1;
    }
}

QString YouTubeTrackModel::modelKey(bool noSearch) const {
    if (noSearch || m_search.isEmpty()) {
        return QStringLiteral("youtube");
    }
    return QStringLiteral("youtube#") + m_search;
}

bool YouTubeTrackModel::updateTrackGenre(Track* pTrack, const QString& genre) const {
    Q_UNUSED(pTrack);
    Q_UNUSED(genre);
    return false;
}

#if defined(__EXTRA_METADATA__)
bool YouTubeTrackModel::updateTrackMood(Track* pTrack, const QString& mood) const {
    Q_UNUSED(pTrack);
    Q_UNUSED(mood);
    return false;
}
#endif // __EXTRA_METADATA__

// ---------------------------------------------------------------------------
// Downloads
// ---------------------------------------------------------------------------

void YouTubeTrackModel::requestDownload(int row, bool load, const QString& group, bool play) {
    if (row < 0 || row >= m_rows.size() || m_rows.at(row).key.isEmpty()) {
        return;
    }
    const Row& r = m_rows.at(row);

    // The latest load request for a deck wins: no other download may still
    // be waiting to load into it.
    if (load && !group.isEmpty()) {
        for (const auto& pOther : std::as_const(m_jobs)) {
            if (pOther->key != r.key && pOther->loadWhenReady && pOther->group == group) {
                pOther->loadWhenReady = false;
                rowsChanged(pOther->key);
            }
        }
    }

    auto pJob = m_jobs.value(r.key);
    if (pJob && (pJob->state == JobState::Queued || pJob->state == JobState::Running)) {
        if (load) {
            pJob->loadWhenReady = true;
            pJob->group = group;
            pJob->play = play;
            if (pJob->state == JobState::Queued) {
                // Somebody is waiting for this one: move it to the front
                m_queue.removeAll(r.key);
                m_queue.prepend(r.key);
            }
        }
        rowsChanged(r.key);
        return;
    }

    pJob = std::make_shared<Job>();
    pJob->key = r.key;
    pJob->url = r.url;
    pJob->title = r.title;
    pJob->loadWhenReady = load;
    pJob->group = group;
    pJob->play = play;
    m_jobs.insert(r.key, pJob);
    if (load) {
        m_queue.prepend(r.key);
    } else {
        m_queue.append(r.key);
    }
    rowsChanged(r.key);
    startNextJobs();
}

void YouTubeTrackModel::startNextJobs() {
    int running = 0;
    for (const auto& pJob : std::as_const(m_jobs)) {
        if (pJob->state == JobState::Running) {
            ++running;
        }
    }
    while (running < kMaxParallelDownloads && !m_queue.isEmpty()) {
        const auto pJob = m_jobs.value(m_queue.takeFirst());
        if (!pJob || pJob->state != JobState::Queued) {
            continue;
        }
        pJob->state = JobState::Running;
        pJob->progress = 0;
        ++running;
        rowsChanged(pJob->key);
        startJob(pJob);
    }
}

void YouTubeTrackModel::startJob(const std::shared_ptr<Job>& pJob) {
    const QString ytDlp = findYtDlp();
    if (ytDlp.isEmpty()) {
        jobFinished(pJob.get(), QString(), tr("yt-dlp not found"));
        emit ytDlpMissing();
        return;
    }
    if (!QDir().mkpath(downloadDir())) {
        jobFinished(pJob.get(), QString(), tr("Cannot create %1").arg(downloadDir()));
        return;
    }
    QStringList args = {
            QStringLiteral("--no-playlist"),
            QStringLiteral("--newline"),
            QStringLiteral("--progress"),
            QStringLiteral("--no-warnings"),
            QStringLiteral("--paths"),
            downloadDir(),
            QStringLiteral("--output"),
            QStringLiteral("%(title).150B [%(id)s].%(ext)s"),
            QStringLiteral("--print"),
            QStringLiteral("after_move:") + kResultFilePrefix + QStringLiteral("%(filepath)s"),
    };
    const QString ffmpeg = findFfmpeg();
    if (ffmpeg.isEmpty()) {
        // Without ffmpeg we can't convert, so pick a stream Mixxx can decode
        // as-is.
        args << QStringLiteral("--format") << QStringLiteral("bestaudio[ext=m4a]/bestaudio");
    } else {
        args << QStringLiteral("--ffmpeg-location") << ffmpeg
             << QStringLiteral("--extract-audio")
             << QStringLiteral("--audio-format") << QStringLiteral("mp3")
             << QStringLiteral("--audio-quality") << QStringLiteral("0")
             << QStringLiteral("--embed-metadata");
    }
    args << pJob->url;

    auto* pProcess = new QProcess(this);
    pJob->pProcess = pProcess;
    pJob->outputBuffer.clear();
    pJob->filePath.clear();
    pProcess->setProcessEnvironment(ytDlpEnvironment());
    pProcess->setProgram(ytDlp);
    pProcess->setArguments(args);
    connect(pProcess, &QProcess::readyReadStandardOutput, this, [this, pJob]() {
        readJobOutput(pJob.get());
    });
    connect(pProcess, &QProcess::readyReadStandardError, this, [pJob, pProcess]() {
        const QString text = QString::fromUtf8(pProcess->readAllStandardError()).trimmed();
        if (!text.isEmpty()) {
            qWarning() << "yt-dlp:" << pJob->title << text;
            // Remember the last error to show it if the download fails
            pJob->note = text.section('\n', -1);
        }
    });
    connect(pProcess,
            &QProcess::errorOccurred,
            this,
            [this, pJob, pProcess](QProcess::ProcessError error) {
                if (error == QProcess::FailedToStart) {
                    pProcess->deleteLater();
                    jobFinished(pJob.get(), QString(), pProcess->errorString());
                }
            });
    connect(pProcess,
            &QProcess::finished,
            this,
            [this, pJob, pProcess](int exitCode, QProcess::ExitStatus exitStatus) {
                readJobOutput(pJob.get());
                if (pJob->outputBuffer.startsWith(kResultFilePrefix)) {
                    pJob->filePath = pJob->outputBuffer.mid(kResultFilePrefix.size()).trimmed();
                }
                pProcess->deleteLater();
                if (exitStatus != QProcess::NormalExit) {
                    jobFinished(pJob.get(), QString(), tr("Cancelled"));
                } else if (exitCode != 0 || pJob->filePath.isEmpty()) {
                    jobFinished(pJob.get(),
                            QString(),
                            pJob->note.isEmpty()
                                    ? tr("yt-dlp exited with code %1").arg(exitCode)
                                    : pJob->note);
                } else {
                    jobFinished(pJob.get(), pJob->filePath, QString());
                }
            });
    pProcess->start();
}

void YouTubeTrackModel::readJobOutput(Job* pJob) {
    if (!pJob->pProcess) {
        return;
    }
    pJob->outputBuffer += QString::fromUtf8(pJob->pProcess->readAllStandardOutput());
    static const QRegularExpression kNewline(QStringLiteral("[\r\n]"));
    static const QRegularExpression kProgress(QStringLiteral("^\\[download\\]\\s+([0-9.]+)%"));
    QStringList lines = pJob->outputBuffer.split(kNewline);
    // The last element is an incomplete line (or empty)
    pJob->outputBuffer = lines.takeLast();
    const int oldProgress = pJob->progress;
    for (const auto& line : std::as_const(lines)) {
        if (line.startsWith(kResultFilePrefix)) {
            pJob->filePath = line.mid(kResultFilePrefix.size()).trimmed();
            continue;
        }
        const auto match = kProgress.match(line);
        if (match.hasMatch()) {
            pJob->progress = static_cast<int>(match.captured(1).toDouble());
        }
    }
    if (pJob->progress != oldProgress) {
        rowsChanged(pJob->key);
    }
}

void YouTubeTrackModel::jobFinished(Job* pJob, const QString& filePath, const QString& error) {
    const bool load = pJob->loadWhenReady;
    pJob->loadWhenReady = false;
    if (!error.isEmpty()) {
        pJob->state = JobState::Failed;
        pJob->note = error;
        qWarning() << "YouTube download failed:" << pJob->title << error;
    } else {
        pJob->state = JobState::Done;
        pJob->note.clear();
        pJob->filePath = filePath;
        m_files.insert(pJob->key, filePath);
        if (load) {
            const TrackPointer pTrack = getTrackByRef(TrackRef::fromFilePath(filePath));
            if (!pTrack) {
                pJob->note = tr("could not load");
            } else if (pJob->group.isEmpty()) {
                emit loadTrack(pTrack);
            } else if (mayLoadInto(pJob->group)) {
                emit loadTrackToPlayer(pTrack, pJob->group, pJob->play);
            } else {
                // Never interrupt a playing deck
                pJob->note = tr("%1 was playing").arg(groupName(pJob->group));
            }
        }
    }
    rowsChanged(pJob->key);
    // Start the next download once the finished process is cleaned up
    QMetaObject::invokeMethod(
            this, [this]() { startNextJobs(); }, Qt::QueuedConnection);
}

bool YouTubeTrackModel::mayLoadInto(const QString& group) const {
    // Same rules as loading from the library: unless the preferences allow
    // it, never replace the track of a playing deck.
    if (PlayerManager::isPreviewDeckGroup(group) ||
            ControlObject::get(ConfigKey(group, QStringLiteral("play"))) <= 0.0) {
        return true;
    }
    if (m_pConfig->exists(kConfigKeyLoadWhenDeckPlaying)) {
        switch (static_cast<LoadWhenDeckPlaying>(
                m_pConfig->getValueString(kConfigKeyLoadWhenDeckPlaying).toInt())) {
        case LoadWhenDeckPlaying::Allow:
        case LoadWhenDeckPlaying::AllowButStopDeck:
            return true;
        case LoadWhenDeckPlaying::Reject:
            return false;
        }
    }
    return m_pConfig->getValue<bool>(kConfigKeyAllowTrackLoadToPlayingDeck);
}

void YouTubeTrackModel::rowsChanged(const QString& key) {
    for (int row = 0; row < m_rows.size(); ++row) {
        if (m_rows.at(row).key == key) {
            emit dataChanged(index(row, 0), index(row, kColumnCount - 1));
        }
    }
}
