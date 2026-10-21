// [AI-generated: this file was written by an AI agent (Claude) on request of the
// fork owner. It is not part of upstream Mixxx.]

#include "library/youtube/youtubefeature.h"

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QMenu>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSysInfo>
#include <QUrl>

#include "library/library.h"
#include "library/treeitem.h"
#include "library/treeitemmodel.h"
#include "library/youtube/youtubetrackmodel.h"
#include "moc_youtubefeature.cpp"

namespace {

/// Downloads of yt-dlp only time out when no data arrives for this long
constexpr int kDownloadStallTimeoutMs = 60000;

#ifdef Q_OS_WIN
const QString kYtDlpExe = QStringLiteral("yt-dlp.exe");
#else
const QString kYtDlpExe = QStringLiteral("yt-dlp");
#endif

/// Name of the standalone yt-dlp release binary for this platform.
QString ytDlpReleaseAsset() {
    const bool arm = QSysInfo::currentCpuArchitecture() == QStringLiteral("arm64");
#if defined(Q_OS_WIN)
    return arm ? QStringLiteral("yt-dlp_arm64.exe") : QStringLiteral("yt-dlp.exe");
#elif defined(Q_OS_MACOS)
    Q_UNUSED(arm);
    return QStringLiteral("yt-dlp_macos");
#else
    return arm ? QStringLiteral("yt-dlp_linux_aarch64") : QStringLiteral("yt-dlp_linux");
#endif
}

} // anonymous namespace

YouTubeFeature::YouTubeFeature(Library* pLibrary, UserSettingsPointer pConfig)
        : LibraryFeature(pLibrary, pConfig, QStringLiteral("youtube")),
          m_pTrackModel(make_parented<YouTubeTrackModel>(
                  this, pConfig, pLibrary->trackCollectionManager())),
          m_pSidebarModel(make_parented<TreeItemModel>(this)),
          m_pNetwork(new QNetworkAccessManager(this)),
          m_ytDlpOffered(false),
          m_fetchingYtDlp(false) {
    m_pSidebarModel->setRootItem(TreeItem::newRoot(this));

    connect(m_pTrackModel.get(),
            &YouTubeTrackModel::loadTrack,
            this,
            &YouTubeFeature::loadTrack);
    connect(m_pTrackModel.get(),
            &YouTubeTrackModel::loadTrackToPlayer,
            this,
            [this](TrackPointer pTrack, const QString& group, bool play) {
#ifdef __STEM__
                emit loadTrackToPlayer(pTrack, group, mixxx::StemChannelSelection(), play);
#else
                emit loadTrackToPlayer(pTrack, group, play);
#endif
            });
    connect(m_pTrackModel.get(),
            &YouTubeTrackModel::ytDlpMissing,
            this,
            &YouTubeFeature::slotYtDlpMissing);
}

QVariant YouTubeFeature::title() {
    return tr("YouTube");
}

TreeItemModel* YouTubeFeature::sidebarModel() const {
    return m_pSidebarModel;
}

void YouTubeFeature::activate() {
    m_pTrackModel->refreshDownloads();
    emit saveModelState();
    emit showTrackModel(m_pTrackModel);
    emit enableCoverArtDisplay(false);
}

void YouTubeFeature::onRightClick(const QPoint& globalPos) {
    QMenu menu;
    const QString ytDlp = m_pTrackModel->findYtDlp();
    QAction* pFetchAction = menu.addAction(
            ytDlp.isEmpty() ? tr("Download yt-dlp") : tr("Update yt-dlp"));
    pFetchAction->setEnabled(!m_fetchingYtDlp);
    connect(pFetchAction, &QAction::triggered, this, &YouTubeFeature::slotFetchYtDlp);
    QAction* pLocateAction = menu.addAction(tr("Locate yt-dlp..."));
    connect(pLocateAction, &QAction::triggered, this, &YouTubeFeature::slotLocateYtDlp);

    // Show which tools are in use, for troubleshooting
    QAction* pYtDlpInfo = menu.addAction(ytDlp.isEmpty()
                    ? tr("yt-dlp: not found")
                    : tr("yt-dlp: %1").arg(QDir::toNativeSeparators(ytDlp)));
    pYtDlpInfo->setEnabled(false);
    QAction* pFfmpegInfo = menu.addAction(m_pTrackModel->findFfmpeg().isEmpty()
                    ? tr("ffmpeg not found: saving M4A instead of MP3")
                    : tr("ffmpeg found: saving MP3"));
    pFfmpegInfo->setEnabled(false);

    menu.addSeparator();
    QAction* pOpenAction = menu.addAction(tr("Open Download Folder"));
    connect(pOpenAction, &QAction::triggered, this, &YouTubeFeature::slotOpenDownloadFolder);
    QAction* pChooseAction = menu.addAction(tr("Change Download Folder..."));
    connect(pChooseAction, &QAction::triggered, this, &YouTubeFeature::slotChooseDownloadFolder);
    menu.exec(globalPos);
}

void YouTubeFeature::slotYtDlpMissing() {
    // Only ask once per session, the track table explains what to do anyway
    if (m_ytDlpOffered || m_fetchingYtDlp) {
        return;
    }
    m_ytDlpOffered = true;
    const auto answer = QMessageBox::question(nullptr,
            tr("YouTube"),
            tr("Searching YouTube needs the free tool yt-dlp, which was not "
               "found.\n\nDownload it now?"));
    if (answer == QMessageBox::Yes) {
        slotFetchYtDlp();
    }
}

void YouTubeFeature::slotFetchYtDlp() {
    if (m_fetchingYtDlp) {
        return;
    }
    const QString toolsDir = m_pTrackModel->toolsDir();
    if (!QDir().mkpath(toolsDir)) {
        QMessageBox::warning(nullptr, tr("YouTube"), tr("Could not create %1").arg(toolsDir));
        return;
    }
    const QString target = QDir(toolsDir).filePath(kYtDlpExe);
    const QUrl url(QStringLiteral(
                           "https://github.com/yt-dlp/yt-dlp/releases/latest/download/") +
            ytDlpReleaseAsset());
    qInfo() << "Downloading" << url.toString() << "to" << target;
    m_fetchingYtDlp = true;

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Mixxx"));
    request.setTransferTimeout(kDownloadStallTimeoutMs);
    QNetworkReply* pReply = m_pNetwork->get(request);
    connect(pReply, &QNetworkReply::finished, this, [this, pReply, target]() {
        pReply->deleteLater();
        m_fetchingYtDlp = false;
        if (pReply->error() != QNetworkReply::NoError) {
            QMessageBox::warning(nullptr,
                    tr("YouTube"),
                    tr("Downloading yt-dlp failed: %1").arg(pReply->errorString()));
            return;
        }
        // Write to a temporary file first so a broken download never
        // replaces a working yt-dlp.
        const QString partPath = target + QStringLiteral(".part");
        QFile file(partPath);
        if (!file.open(QIODevice::WriteOnly) || file.write(pReply->readAll()) < 0) {
            QMessageBox::warning(nullptr, tr("YouTube"), tr("Could not write %1").arg(partPath));
            return;
        }
        file.close();
        QFile::remove(target);
        if (!QFile::rename(partPath, target)) {
            QMessageBox::warning(nullptr, tr("YouTube"), tr("Could not write %1").arg(target));
            return;
        }
        QFile::setPermissions(target,
                QFile::permissions(target) | QFileDevice::ExeOwner |
                        QFileDevice::ExeUser);
        m_pTrackModel->setYtDlpPath(target);
        qInfo() << "yt-dlp saved to" << target;
        m_pTrackModel->retrySearch();
    });
}

void YouTubeFeature::slotLocateYtDlp() {
#ifdef Q_OS_WIN
    const QString filter = tr("Programs (*.exe)");
#else
    const QString filter;
#endif
    const QString path = QFileDialog::getOpenFileName(
            nullptr, tr("Locate yt-dlp"), QString(), filter);
    if (path.isEmpty()) {
        return;
    }
    m_pTrackModel->setYtDlpPath(path);
    m_pTrackModel->retrySearch();
}

void YouTubeFeature::slotOpenDownloadFolder() {
    const QString dir = m_pTrackModel->downloadDir();
    QDir().mkpath(dir);
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

void YouTubeFeature::slotChooseDownloadFolder() {
    const QString dir = QFileDialog::getExistingDirectory(
            nullptr, tr("Choose Download Folder"), m_pTrackModel->downloadDir());
    if (!dir.isEmpty()) {
        m_pTrackModel->setDownloadDir(dir);
    }
}
