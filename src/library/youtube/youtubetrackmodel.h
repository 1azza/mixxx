// [AI-generated: this file was written by an AI agent (Claude) on request of the
// fork owner. It is not part of upstream Mixxx.]
//
// YouTubeTrackModel: track table model for the YouTube library feature.
// The library search box searches YouTube via the external yt-dlp tool and
// the results are listed like regular tracks. Loading a result that has not
// been downloaded yet downloads it in the background and loads it into the
// requested deck as soon as it is ready. Downloads are kept in a folder and
// listed when the search box is empty.
//
// [End of AI-generated notice]

#pragma once

#include <QAbstractTableModel>
#include <QHash>
#include <QList>
#include <QPointer>
#include <QProcess>
#include <QStringList>
#include <QTimer>
#include <memory>

#include "library/trackmodel.h"
#include "preferences/usersettings.h"

class QJsonObject;
class TrackCollectionManager;

class YouTubeTrackModel final : public QAbstractTableModel, public virtual TrackModel {
    Q_OBJECT
  public:
    enum Column {
        kColumnArtist = 0,
        kColumnTitle,
        kColumnDuration,
        kColumnStatus,
        kColumnLocation,
        kColumnCount,
    };

    YouTubeTrackModel(QObject* pParent,
            UserSettingsPointer pConfig,
            TrackCollectionManager* pTrackCollectionManager);
    ~YouTubeTrackModel() override;

    QString findYtDlp() const;
    QString findFfmpeg() const;
    QString toolsDir() const;
    void setYtDlpPath(const QString& path);
    QString downloadDir() const;
    void setDownloadDir(const QString& dir);
    /// Repeat the current search, e.g. after yt-dlp has been installed
    void retrySearch();
    /// Re-read the list of downloaded files if it is currently shown
    void refreshDownloads();

    // QAbstractTableModel
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section,
            Qt::Orientation orientation,
            int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    QMimeData* mimeData(const QModelIndexList& indexes) const override;
    void sort(int column, Qt::SortOrder order) override;

    // TrackModel
    TrackPointer getTrack(const QModelIndex& index) const override;
    TrackPointer getTrackByRef(const TrackRef& trackRef) const override;
    bool isTrackDeferred(const QModelIndex& index) const override;
    void loadDeferredTrack(const QModelIndex& index, const QString& group, bool play) override;
    void fetchDeferredTrack(const QModelIndex& index) override;
    QUrl getTrackUrl(const QModelIndex& index) const override;
    QString getTrackLocation(const QModelIndex& index) const override;
    TrackId getTrackId(const QModelIndex& index) const override;
    CoverInfo getCoverInfo(const QModelIndex& index) const override;
    const QVector<int> getTrackRows(TrackId trackId) const override;
    void search(const QString& searchText) override;
    const QString currentSearch() const override;
    bool isColumnInternal(int column) override;
    bool isColumnHiddenByDefault(int column) override;
    TrackModel::Capabilities getCapabilities() const override;
    TrackModel::SortColumnId sortColumnIdFromColumnIndex(int index) const override;
    int columnIndexFromSortColumnId(TrackModel::SortColumnId sortColumn) const override;
    QString modelKey(bool noSearch) const override;
    bool updateTrackGenre(Track* pTrack, const QString& genre) const override;
#if defined(__EXTRA_METADATA__)
    bool updateTrackMood(Track* pTrack, const QString& mood) const override;
#endif // __EXTRA_METADATA__

  signals:
    void loadTrack(TrackPointer pTrack);
    void loadTrackToPlayer(TrackPointer pTrack, const QString& group, bool play);
    void ytDlpMissing();

  private:
    struct Row {
        /// Identifies the video ("yt:<id>"); empty for info rows
        QString key;
        QString url;
        QString title;
        QString channel;
        double duration = 0;
        /// Set for rows listing files of the download folder
        QString filePath;
    };

    enum class JobState {
        Queued,
        Running,
        Done,
        Failed,
    };

    struct Job {
        QString key;
        QString url;
        QString title;
        JobState state = JobState::Queued;
        int progress = 0;
        bool loadWhenReady = false;
        /// Empty means the next available deck
        QString group;
        bool play = false;
        /// Shown in the status column, e.g. why a finished track wasn't loaded
        QString note;
        QString filePath;
        QPointer<QProcess> pProcess;
        QString outputBuffer;
    };

    void startSearch();
    void showDownloads();
    void showInfo(const QString& text);
    void setRows(QList<Row> rows);
    Row rowFromEntry(const QJsonObject& entry) const;
    QString filePathOfRow(int row) const;
    QString statusOfRow(int row) const;

    void requestDownload(int row, bool load, const QString& group, bool play);
    void startNextJobs();
    void startJob(const std::shared_ptr<Job>& pJob);
    void readJobOutput(Job* pJob);
    void jobFinished(Job* pJob, const QString& filePath, const QString& error);
    bool mayLoadInto(const QString& group) const;
    void rowsChanged(const QString& key);

    const UserSettingsPointer m_pConfig;
    TrackCollectionManager* const m_pTrackCollectionManager;

    QList<Row> m_rows;
    QString m_search;
    QTimer m_searchTimer;
    QPointer<QProcess> m_pSearchProcess;
    int m_sortColumn;
    Qt::SortOrder m_sortOrder;

    QHash<QString, std::shared_ptr<Job>> m_jobs;
    QStringList m_queue;
    /// Downloaded files by key, including downloads of earlier sessions
    QHash<QString, QString> m_files;
};
