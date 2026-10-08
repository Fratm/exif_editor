#pragma once

#include "provenance.h"

#include <QMainWindow>
#include <QSet>
#include <QString>
#include <QStringList>

#include <exiv2/exiv2.hpp>

class QTableWidget;
class QLabel;
class QLineEdit;
class QAction;

#if EXIV2_TEST_VERSION(0, 28, 0)
using ImagePtr = Exiv2::Image::UniquePtr;
#else
using ImagePtr = Exiv2::Image::AutoPtr;
#endif

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    bool openFile(const QString &path);

    // One detected AI marker, as listed in the "Remove AI Markers" dialog.
    struct AiMarker {
        int family;       // Family enum value (see mainwindow.cpp)
        QString key;
        int occurrence;
        QString reason;
        QString value;
    };
    QVector<AiMarker> findAiMarkers() const;

protected:
    void closeEvent(QCloseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private slots:
    void onOpen();
    bool onSave();
    void onRevert();
    void onAddTag();
    void onRemoveTags();
    void onRemoveGps();
    void onRemoveAiMarkers();
    void onStripAll();
    void onCellChanged(int row, int column);
    void onFilterChanged(const QString &text);

private:
    void buildUi();
    void populateTable();
    void loadPreview();
    void updateInfo();
    void setModified(bool modified);
    void updateActions();
    bool maybeSave();
    void showError(const QString &title, const QString &message);
    bool removeEntry(int family, const std::string &key, int occurrence);
    void markEdited(const QString &key);

    QTableWidget *m_table = nullptr;
    QLabel *m_preview = nullptr;
    QLabel *m_info = nullptr;
    QLineEdit *m_filter = nullptr;

    QAction *m_saveAct = nullptr;
    QAction *m_revertAct = nullptr;
    QAction *m_addAct = nullptr;
    QAction *m_removeAct = nullptr;
    QAction *m_gpsAct = nullptr;
    QAction *m_aiAct = nullptr;
    QAction *m_stripAct = nullptr;
    QAction *m_keepTimeAct = nullptr;

    QString m_path;
    ImagePtr m_image;
    // Working copies that get edited; written back on save.
    Exiv2::ExifData m_exif;
    Exiv2::IptcData m_iptc;
    Exiv2::XmpData m_xmp;
    // C2PA manifests / generator text chunks found in the raw file.
    QVector<Provenance::RawMarker> m_rawMarkers;
    QStringList m_rawToStrip;        // raw marker keys queued for removal on save

    QSet<QString> m_editedKeys;      // keys changed since the last load/save
    bool m_modified = false;
    bool m_populating = false;
};
