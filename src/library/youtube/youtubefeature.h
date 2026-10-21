// [AI-generated: this file was written by an AI agent (Claude) on request of the
// fork owner. It is not part of upstream Mixxx.]
//
// YouTubeFeature: "YouTube" entry in the library sidebar. Selecting it shows
// YouTube search results in the track table, see YouTubeTrackModel.
//
// [End of AI-generated notice]

#pragma once

#include "library/libraryfeature.h"
#include "util/parented_ptr.h"

class QNetworkAccessManager;
class TreeItemModel;
class YouTubeTrackModel;

class YouTubeFeature final : public LibraryFeature {
    Q_OBJECT
  public:
    YouTubeFeature(Library* pLibrary, UserSettingsPointer pConfig);
    ~YouTubeFeature() override = default;

    QVariant title() override;
    TreeItemModel* sidebarModel() const override;
    bool hasTrackTable() override {
        return true;
    }

  public slots:
    void activate() override;
    void onRightClick(const QPoint& globalPos) override;

  private slots:
    void slotYtDlpMissing();
    void slotFetchYtDlp();
    void slotLocateYtDlp();
    void slotOpenDownloadFolder();
    void slotChooseDownloadFolder();

  private:
    parented_ptr<YouTubeTrackModel> m_pTrackModel;
    parented_ptr<TreeItemModel> m_pSidebarModel;
    QNetworkAccessManager* m_pNetwork;
    bool m_ytDlpOffered;
    bool m_fetchingYtDlp;
};
