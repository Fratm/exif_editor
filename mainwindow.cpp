#include "mainwindow.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QCompleter>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHash>
#include <QHeaderView>
#include <QImageReader>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSplitter>
#include <QStatusBar>
#include <QTableWidget>
#include <QToolBar>
#include <QVBoxLayout>

#include <algorithm>
#include <map>
#include <set>

#if EXIV2_TEST_VERSION(0, 28, 0)
using ExivError = Exiv2::Error;
using ValuePtr = Exiv2::Value::UniquePtr;
#else
using ExivError = Exiv2::AnyError;
using ValuePtr = Exiv2::Value::AutoPtr;
#endif

namespace {

enum Family { FamExif, FamIptc, FamXmp, FamFile };
enum Column { ColKey, ColLabel, ColType, ColValue, ColInterpreted, ColCount };
constexpr int OccurrenceRole = Qt::UserRole + 1;
constexpr int FamilyRole = Qt::UserRole + 2;
constexpr long MaxEditableSize = 2048;   // bigger values (maker notes, blobs) are read-only

const QString kDigitalSourceTypeBase = "http://cv.iptc.org/newscodes/digitalsourcetype/";
const QStringList kDigitalSourceTypes = {
    "digitalCapture", "computationalCapture", "negativeFilm", "positiveFilm", "print",
    "humanEdits", "compositeCapture", "algorithmicallyEnhanced", "digitalCreation",
    "dataDrivenMedia", "screenCapture", "virtualRecording", "composite",
    "trainedAlgorithmicMedia", "compositeWithTrainedAlgorithmicMedia", "algorithmicMedia",
    "compositeSynthetic",
};

QString qs(const std::string &s) { return QString::fromStdString(s); }
QString exivMessage(const ExivError &e) { return QString::fromLocal8Bit(e.what()); }

int familyOf(const QString &key)
{
    if (key.startsWith("Exif.")) return FamExif;
    if (key.startsWith("Iptc.")) return FamIptc;
    if (key.startsWith("Xmp.")) return FamXmp;
    return FamFile;
}

template <class Data>
typename Data::iterator findNth(Data &data, const std::string &key, int n)
{
    int seen = 0;
    for (auto it = data.begin(); it != data.end(); ++it) {
        if (it->key() == key && seen++ == n)
            return it;
    }
    return data.end();
}

QString tagDescription(int family, const std::string &key)
{
    try {
        switch (family) {
        case FamExif: return qs(Exiv2::ExifKey(key).tagDesc());
        case FamIptc: {
            Exiv2::IptcKey k(key);
            const char *d = Exiv2::IptcDataSets::dataSetDesc(k.tag(), k.record());
            return d ? QString::fromUtf8(d) : QString();
        }
        case FamXmp: {
            const char *d = Exiv2::XmpProperties::propertyDesc(Exiv2::XmpKey(key));
            return d ? QString::fromUtf8(d) : QString();
        }
        }
    } catch (...) {
    }
    return {};
}

bool isXmpArrayType(Exiv2::TypeId t)
{
    return t == Exiv2::xmpBag || t == Exiv2::xmpSeq || t == Exiv2::xmpAlt;
}

QString formatHint(Exiv2::TypeId type)
{
    switch (type) {
    case Exiv2::asciiString:
    case Exiv2::string:
    case Exiv2::xmpText: return "Text";
    case Exiv2::unsignedRational:
    case Exiv2::signedRational: return "Rational, e.g. 1/200 — several values separated by spaces";
    case Exiv2::unsignedShort:
    case Exiv2::unsignedLong:
    case Exiv2::signedShort:
    case Exiv2::signedLong:
    case Exiv2::unsignedByte: return "Integer(s) separated by spaces";
    case Exiv2::undefined: return "Raw bytes as space-separated decimal numbers, e.g. 48 50 51 48";
    case Exiv2::comment: return "Text (optionally prefixed with charset=Ascii)";
    case Exiv2::date: return "Date, e.g. 2024-05-01";
    case Exiv2::time: return "Time, e.g. 10:00:00+00:00";
    case Exiv2::xmpBag:
    case Exiv2::xmpSeq:
    case Exiv2::xmpAlt: return "List of items separated by \", \"";
    case Exiv2::langAlt: return "Text, optionally prefixed with a language: lang=\"x-default\" My caption";
    default: return {};
    }
}

// Build a fresh value of the given type from user text (used for XMP and new tags).
ValuePtr parseFresh(Exiv2::TypeId type, const QString &text)
{
    ValuePtr v = Exiv2::Value::create(type);
    QStringList parts;
    if (isXmpArrayType(type))
        parts = text.split(", ", Qt::SkipEmptyParts);
    else if (type == Exiv2::langAlt)
        parts = text.split(QRegularExpression(",\\s*(?=lang=)"), Qt::SkipEmptyParts);
    else
        parts = {text};
    for (const QString &p : parts) {
        if (v->read(p.toStdString()) != 0)
            return ValuePtr();
    }
    if (!v->ok())
        return ValuePtr();
    return v;
}

// Re-read user text into a copy of an existing value, keeping its exact type/class.
template <class Datum>
ValuePtr parseLike(const Datum &md, int family, const QString &text)
{
    if (family == FamXmp)
        return parseFresh(md.typeId(), text);
    ValuePtr v = md.getValue();
    if (v->read(text.toStdString()) != 0 || !v->ok())
        return ValuePtr();
    return v;
}

template <class Datum>
bool isEditable(const Datum &md, int family, const QSet<QString> &containers)
{
    const std::string key = md.key();
    if (family == FamXmp)
        return !containers.contains(qs(key));
    if (md.size() > MaxEditableSize)
        return false;
    // Makernote blobs, IFD pointers and thumbnail offsets are structural; editing corrupts files.
    static const std::set<std::string> structural = {
        "Exif.Photo.MakerNote", "Exif.Image.ExifTag", "Exif.Image.GPSTag",
        "Exif.Photo.InteroperabilityTag", "Exif.Thumbnail.JPEGInterchangeFormat",
        "Exif.Thumbnail.JPEGInterchangeFormatLength"};
    return structural.count(key) == 0;
}

// XMP structs/arrays of structs show up as a parent key plus children like
// "Parent[1]/ns:field". The parent row isn't directly editable.
QSet<QString> xmpContainers(const Exiv2::XmpData &xmp)
{
    QSet<QString> keys, containers;
    for (const auto &md : xmp)
        keys.insert(qs(md.key()));
    for (const QString &k : keys) {
        for (const QString &sep : {QStringLiteral("["), QStringLiteral("/")}) {
            int idx = k.lastIndexOf(sep);
            while (idx > 0) {
                containers.insert(k.left(idx));
                idx = k.left(idx).lastIndexOf(sep);
            }
        }
    }
    return containers;
}

// Why is this key/value an AI marker? Empty string = it isn't.
QString markerReason(const QString &key, const QString &value)
{
    const QString lk = key.toLower();
    if (lk.contains("digitalsourcetype")) {
        if (Provenance::aiMention(value).isEmpty())
            return {};
        return "IPTC Digital Source Type: " + value.section('/', -1);
    }
    if (lk.contains("c2pa"))
        return "C2PA provenance reference";
    if (value.size() <= 8192) {
        const QString m = Provenance::aiMention(value);
        if (!m.isEmpty())
            return QString("Mentions \"%1\"").arg(m);
    }
    return {};
}

QStringList standardTagKeys()
{
    QStringList keys;
    for (const char *group : {"Image", "Photo", "GPSInfo", "Iop"}) {
        const Exiv2::TagInfo *ti = Exiv2::ExifTags::tagList(group);
        for (; ti && ti->tag_ != 0xffff; ++ti)
            keys << QStringLiteral("Exif.%1.%2").arg(QLatin1String(group), QLatin1String(ti->name_));
    }
    const Exiv2::DataSet *ds = Exiv2::IptcDataSets::application2RecordList();
    for (; ds && ds->number_ != 0xffff; ++ds)
        keys << QStringLiteral("Iptc.Application2.%1").arg(QLatin1String(ds->name_));
    for (const char *prefix : {"dc", "xmp", "xmpRights", "photoshop", "iptc", "iptcExt", "tiff", "exif"}) {
        try {
            const Exiv2::XmpPropertyInfo *pi = Exiv2::XmpProperties::propertyList(prefix);
            for (; pi && pi->name_; ++pi)
                keys << QStringLiteral("Xmp.%1.%2").arg(QLatin1String(prefix), QLatin1String(pi->name_));
        } catch (...) {
        }
    }
    keys.removeDuplicates();
    keys.sort(Qt::CaseInsensitive);
    return keys;
}

} // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    buildUi();
    setAcceptDrops(true);
    updateActions();
    resize(1250, 780);
    setWindowTitle("EXIF Editor");
    restoreGeometry(QSettings().value("geometry").toByteArray());
}

void MainWindow::buildUi()
{
    // ---- Actions ----
    auto *openAct = new QAction(QIcon::fromTheme("document-open"), "&Open…", this);
    openAct->setShortcut(QKeySequence::Open);
    connect(openAct, &QAction::triggered, this, &MainWindow::onOpen);

    m_saveAct = new QAction(QIcon::fromTheme("document-save"), "&Save", this);
    m_saveAct->setShortcut(QKeySequence::Save);
    connect(m_saveAct, &QAction::triggered, this, &MainWindow::onSave);

    m_revertAct = new QAction(QIcon::fromTheme("document-revert"), "&Revert", this);
    connect(m_revertAct, &QAction::triggered, this, &MainWindow::onRevert);

    m_addAct = new QAction(QIcon::fromTheme("list-add"), "&Add Tag…", this);
    m_addAct->setShortcut(QKeySequence("Ctrl+N"));
    connect(m_addAct, &QAction::triggered, this, &MainWindow::onAddTag);

    m_removeAct = new QAction(QIcon::fromTheme("list-remove"), "&Remove Selected", this);
    m_removeAct->setShortcut(QKeySequence::Delete);
    connect(m_removeAct, &QAction::triggered, this, &MainWindow::onRemoveTags);

    m_gpsAct = new QAction(QIcon::fromTheme("mark-location"), "Remove &GPS Data", this);
    connect(m_gpsAct, &QAction::triggered, this, &MainWindow::onRemoveGps);

    m_aiAct = new QAction(QIcon::fromTheme("tools-wizard"), "Remove A&I Markers…", this);
    connect(m_aiAct, &QAction::triggered, this, &MainWindow::onRemoveAiMarkers);

    m_stripAct = new QAction(QIcon::fromTheme("edit-clear-all"), "Remove All &EXIF", this);
    connect(m_stripAct, &QAction::triggered, this, &MainWindow::onStripAll);

    m_keepTimeAct = new QAction("Keep File Modification &Time", this);
    m_keepTimeAct->setCheckable(true);
    m_keepTimeAct->setChecked(QSettings().value("keepMtime", true).toBool());
    connect(m_keepTimeAct, &QAction::toggled, this, [](bool on) { QSettings().setValue("keepMtime", on); });

    auto *quitAct = new QAction(QIcon::fromTheme("application-exit"), "&Quit", this);
    quitAct->setShortcut(QKeySequence::Quit);
    connect(quitAct, &QAction::triggered, this, &QWidget::close);

    // ---- Menus ----
    auto *fileMenu = menuBar()->addMenu("&File");
    fileMenu->addActions({openAct, m_saveAct, m_revertAct});
    fileMenu->addSeparator();
    fileMenu->addAction(m_keepTimeAct);
    fileMenu->addSeparator();
    fileMenu->addAction(quitAct);

    auto *editMenu = menuBar()->addMenu("&Edit");
    editMenu->addActions({m_addAct, m_removeAct});
    editMenu->addSeparator();
    editMenu->addActions({m_gpsAct, m_aiAct, m_stripAct});

    // ---- Toolbar ----
    auto *tb = addToolBar("Main");
    tb->setObjectName("mainToolbar");
    tb->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    tb->addActions({openAct, m_saveAct, m_revertAct});
    tb->addSeparator();
    tb->addActions({m_addAct, m_removeAct});
    tb->addSeparator();
    tb->addActions({m_gpsAct, m_aiAct});

    // ---- Left: preview + file info ----
    auto *left = new QWidget;
    auto *leftLayout = new QVBoxLayout(left);
    m_preview = new QLabel("Open an image or drop one here");
    m_preview->setAlignment(Qt::AlignCenter);
    m_preview->setMinimumSize(260, 200);
    m_preview->setFrameShape(QFrame::StyledPanel);
    m_info = new QLabel;
    m_info->setWordWrap(true);
    m_info->setTextInteractionFlags(Qt::TextSelectableByMouse);
    leftLayout->addWidget(m_preview, 1);
    leftLayout->addWidget(m_info);

    // ---- Right: filter + table ----
    auto *right = new QWidget;
    auto *rightLayout = new QVBoxLayout(right);
    m_filter = new QLineEdit;
    m_filter->setPlaceholderText("Filter tags… (try \"Xmp\", \"GPS\" or \"AI\")");
    m_filter->setClearButtonEnabled(true);
    connect(m_filter, &QLineEdit::textChanged, this, &MainWindow::onFilterChanged);

    m_table = new QTableWidget(0, ColCount);
    m_table->setHorizontalHeaderLabels({"Tag", "Name", "Type", "Value (editable)", "Interpreted"});
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed |
                             QAbstractItemView::AnyKeyPressed);
    m_table->setAlternatingRowColors(true);
    m_table->verticalHeader()->hide();
    m_table->setWordWrap(false);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setContextMenuPolicy(Qt::ActionsContextMenu);
    m_table->addActions({m_addAct, m_removeAct});
    connect(m_table, &QTableWidget::cellChanged, this, &MainWindow::onCellChanged);
    connect(m_table, &QTableWidget::itemSelectionChanged, this, &MainWindow::updateActions);

    rightLayout->addWidget(m_filter);
    rightLayout->addWidget(m_table);

    auto *splitter = new QSplitter;
    splitter->addWidget(left);
    splitter->addWidget(right);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 3);
    setCentralWidget(splitter);

    statusBar()->showMessage("Ready");
}

bool MainWindow::openFile(const QString &path)
{
    if (!maybeSave())
        return false;

    try {
        auto image = Exiv2::ImageFactory::open(QFile::encodeName(path).toStdString());
        image->readMetadata();
        m_image = std::move(image);
    } catch (const ExivError &e) {
        showError("Could not open file", QString("%1\n\n%2").arg(path, exivMessage(e)));
        return false;
    }

    m_path = path;
    m_exif = m_image->exifData();
    m_iptc = m_image->iptcData();
    m_xmp = m_image->xmpData();
    m_rawMarkers = Provenance::scan(path);
    m_rawToStrip.clear();
    m_editedKeys.clear();
    m_filter->clear();
    populateTable();
    loadPreview();
    setModified(false);
    statusBar()->showMessage(QString("Loaded %1").arg(path), 5000);
    return true;
}

QVector<MainWindow::AiMarker> MainWindow::findAiMarkers() const
{
    QVector<AiMarker> out;
    auto scanData = [&](const auto &data, int family) {
        std::map<std::string, int> occ;
        for (const auto &md : data) {
            const std::string key = md.key();
            const int n = occ[key]++;
            if (md.size() > 65536)
                continue;
            const QString value = qs(md.toString());
            const QString reason = markerReason(qs(key), value);
            if (!reason.isEmpty())
                out.push_back({family, qs(key), n, reason, value});
        }
    };
    scanData(m_exif, FamExif);
    scanData(m_iptc, FamIptc);
    scanData(m_xmp, FamXmp);

    for (const auto &m : m_rawMarkers) {
        if (m_rawToStrip.contains(m.key))
            continue;
        QString reason;
        if (m.key.startsWith("PNG.Text"))
            reason = "AI generator settings / prompt";
        else if (m.value.startsWith("Mentions"))
            reason = "C2PA manifest with AI assertions";
        else
            reason = "C2PA manifest (no AI terms found — may be from a camera or editor)";
        out.push_back({FamFile, m.key, 0, reason, m.value});
    }
    return out;
}

void MainWindow::populateTable()
{
    m_populating = true;
    m_table->setSortingEnabled(false);
    m_table->clearContents();

    // Index AI markers so matching rows can be highlighted.
    QHash<QString, QString> markerRows;
    for (const auto &m : findAiMarkers())
        markerRows.insert(QString("%1#%2").arg(m.key).arg(m.occurrence), m.reason);

    const QSet<QString> containers = xmpContainers(m_xmp);
    QFont boldFont = m_table->font();
    boldFont.setBold(true);
    const QColor aiTint(255, 150, 0, 55);   // translucent amber, readable in light and dark themes
    const QIcon aiIcon = QIcon::fromTheme("dialog-warning");

    int pendingRaw = 0;
    for (const auto &m : m_rawMarkers)
        if (!m_rawToStrip.contains(m.key))
            ++pendingRaw;
    m_table->setRowCount(int(m_exif.count() + m_iptc.size() + m_xmp.count()) + pendingRaw);

    int row = 0;
    auto addRow = [&](int family, const QString &key, int occ, const QString &label, const QString &type,
                      const QString &value, const QString &interpreted, bool editable, Exiv2::TypeId typeId) {
        auto *keyItem = new QTableWidgetItem(key);
        keyItem->setData(OccurrenceRole, occ);
        keyItem->setData(FamilyRole, family);
        auto *labelItem = new QTableWidgetItem(label);
        auto *typeItem = new QTableWidgetItem(type);
        auto *valueItem = new QTableWidgetItem(value);
        auto *interpItem = new QTableWidgetItem(interpreted);

        const QString desc = tagDescription(family, key.toStdString());
        keyItem->setToolTip(desc);
        labelItem->setToolTip(desc);
        valueItem->setToolTip(editable ? formatHint(typeId) : "Read-only (binary or structural data)");

        for (auto *it : {keyItem, labelItem, typeItem, interpItem})
            it->setFlags(it->flags() & ~Qt::ItemIsEditable);
        if (!editable) {
            valueItem->setFlags(valueItem->flags() & ~Qt::ItemIsEditable);
            valueItem->setForeground(palette().color(QPalette::Disabled, QPalette::Text));
        }
        QList<QTableWidgetItem *> items = {keyItem, labelItem, typeItem, valueItem, interpItem};
        const QString reason = markerRows.value(QString("%1#%2").arg(key).arg(occ));
        if (!reason.isEmpty()) {
            keyItem->setIcon(aiIcon);
            keyItem->setToolTip("AI marker: " + reason + (desc.isEmpty() ? "" : "\n\n" + desc));
            for (auto *it : items)
                it->setBackground(aiTint);
            // Makes the filter box find these rows with "AI".
            keyItem->setData(Qt::UserRole + 3, "AI marker");
        }
        if (m_editedKeys.contains(key))
            for (auto *it : items)
                it->setFont(boldFont);
        for (int c = 0; c < ColCount; ++c)
            m_table->setItem(row, c, items[c]);
        ++row;
    };

    auto addData = [&](const auto &data, int family) {
        std::map<std::string, int> occurrences;
        for (const auto &md : data) {
            const std::string key = md.key();
            const int occ = occurrences[key]++;
            const bool editable = isEditable(md, family, containers);
            QString raw = (md.size() > MaxEditableSize && family != FamXmp)
                              ? QString("<%1 bytes>").arg(md.size())
                              : qs(md.toString());
            QString interpreted;
            try {
                interpreted = qs(md.print(&m_exif));
            } catch (...) {
                interpreted = raw;
            }
            if (interpreted.size() > 300)
                interpreted = interpreted.left(300) + "…";
            const QString type = QString("%1 [%2]").arg(QLatin1String(md.typeName() ? md.typeName() : "?")).arg(md.count());
            addRow(family, qs(key), occ, qs(md.tagLabel()), type, raw, interpreted, editable, md.typeId());
        }
    };
    addData(m_exif, FamExif);
    addData(m_iptc, FamIptc);
    addData(m_xmp, FamXmp);

    for (const auto &m : m_rawMarkers) {
        if (m_rawToStrip.contains(m.key))
            continue;
        addRow(FamFile, m.key, 0, m.description,
               m.bytes ? QString("File data [%1 bytes]").arg(m.bytes) : QString("File data"),
               m.value, m.value, false, Exiv2::invalidTypeId);
    }

    m_table->resizeColumnsToContents();
    if (m_table->columnWidth(ColKey) > 330)
        m_table->setColumnWidth(ColKey, 330);
    if (m_table->columnWidth(ColValue) > 350)
        m_table->setColumnWidth(ColValue, 350);
    if (m_table->columnWidth(ColLabel) > 220)
        m_table->setColumnWidth(ColLabel, 220);
    m_populating = false;
    onFilterChanged(m_filter->text());
    updateInfo();
    updateActions();
}

void MainWindow::updateInfo()
{
    if (!m_image.get()) {
        m_info->clear();
        return;
    }
    QFileInfo fi(m_path);
    const int aiCount = findAiMarkers().size();
    QString text = QString("<b>%1</b><br>%2<br>%3 × %4 px<br>%5 KiB<br>"
                           "%6 EXIF · %7 IPTC · %8 XMP tags")
                       .arg(fi.fileName().toHtmlEscaped(), qs(m_image->mimeType()))
                       .arg(m_image->pixelWidth())
                       .arg(m_image->pixelHeight())
                       .arg(fi.size() / 1024)
                       .arg(m_exif.count())
                       .arg(m_iptc.size())
                       .arg(m_xmp.count());
    text += aiCount ? QString("<br><span style='color:#d97a00'><b>⚠ %1 AI marker(s)</b></span>").arg(aiCount)
                    : QString("<br>No AI markers found");
    m_info->setText(text);
}

void MainWindow::loadPreview()
{
    QImageReader reader(m_path);
    reader.setAutoTransform(true);   // honour EXIF orientation
    QImage img = reader.read();

    if (img.isNull() && m_image.get()) {
        // RAW files etc.: use the largest embedded preview Exiv2 can find.
        try {
            Exiv2::PreviewManager pm(*m_image);
            auto props = pm.getPreviewProperties();
            if (!props.empty()) {
                Exiv2::PreviewImage pi = pm.getPreviewImage(props.back());
                img.loadFromData(reinterpret_cast<const uchar *>(pi.pData()), static_cast<int>(pi.size()));
            }
        } catch (...) {
        }
    }

    if (img.isNull()) {
        m_preview->setPixmap({});
        m_preview->setText("No preview available");
        return;
    }
    m_preview->setPixmap(QPixmap::fromImage(img.scaled(QSize(420, 420), Qt::KeepAspectRatio, Qt::SmoothTransformation)));
}

void MainWindow::markEdited(const QString &key)
{
    m_editedKeys.insert(key);
    setModified(true);
}

void MainWindow::onCellChanged(int row, int column)
{
    if (m_populating || column != ColValue)
        return;

    auto *keyItem = m_table->item(row, ColKey);
    auto *valueItem = m_table->item(row, ColValue);
    if (!keyItem || !valueItem)
        return;

    const int family = keyItem->data(FamilyRole).toInt();
    const std::string key = keyItem->text().toStdString();
    const int occ = keyItem->data(OccurrenceRole).toInt();
    const QString text = valueItem->text();

    auto refresh = [this] { QMetaObject::invokeMethod(this, [this] { populateTable(); }, Qt::QueuedConnection); };

    auto apply = [&](auto &data) -> bool {
        auto it = findNth(data, key, occ);
        if (it == data.end())
            return false;
        if (text.toStdString() == it->toString())
            return false;
        ValuePtr v = parseLike(*it, family, text);
        if (!v.get()) {
            showError("Invalid value", QString("'%1' is not a valid value for %2.\n\nExpected: %3")
                                           .arg(text, keyItem->text(), formatHint(it->typeId())));
            return false;
        }
        it->setValue(v.get());
        return true;
    };

    bool changed = false;
    try {
        switch (family) {
        case FamExif: changed = apply(m_exif); break;
        case FamIptc: changed = apply(m_iptc); break;
        case FamXmp: changed = apply(m_xmp); break;
        default: break;
        }
    } catch (const ExivError &e) {
        showError("Invalid value", exivMessage(e));
    }
    if (changed)
        markEdited(keyItem->text());
    refresh();
}

void MainWindow::onAddTag()
{
    if (!m_image.get())
        return;

    QDialog dlg(this);
    dlg.setWindowTitle("Add Tag");
    auto *form = new QFormLayout(&dlg);

    auto *keyBox = new QComboBox;
    keyBox->setEditable(true);
    keyBox->addItems(standardTagKeys());
    keyBox->setCurrentText("Exif.Image.Artist");
    keyBox->completer()->setFilterMode(Qt::MatchContains);
    keyBox->completer()->setCaseSensitivity(Qt::CaseInsensitive);
    keyBox->setMinimumWidth(420);

    auto *valueBox = new QComboBox;
    valueBox->setEditable(true);
    auto *hint = new QLabel;
    hint->setWordWrap(true);

    auto typeFor = [](const std::string &k) -> Exiv2::TypeId {
        switch (familyOf(qs(k))) {
        case FamExif: return Exiv2::ExifKey(k).defaultTypeId();
        case FamIptc: {
            Exiv2::IptcKey ik(k);
            return Exiv2::IptcDataSets::dataSetType(ik.tag(), ik.record());
        }
        case FamXmp: return Exiv2::XmpProperties::propertyType(Exiv2::XmpKey(k));
        }
        throw std::runtime_error("Keys must start with Exif., Iptc. or Xmp.");
    };

    auto updateHint = [&] {
        const QString k = keyBox->currentText().trimmed();
        const QString current = valueBox->currentText();
        valueBox->clear();
        if (k.contains("DigitalSourceType")) {
            for (const QString &t : kDigitalSourceTypes)
                valueBox->addItem(kDigitalSourceTypeBase + t);
        }
        valueBox->setCurrentText(current);
        try {
            const Exiv2::TypeId t = typeFor(k.toStdString());
            hint->setText(QString("<i>%1</i><br>Type: %2 — %3")
                              .arg(tagDescription(familyOf(k), k.toStdString()).toHtmlEscaped(),
                                   QLatin1String(Exiv2::TypeInfo::typeName(t)), formatHint(t)));
        } catch (...) {
            hint->setText("<i>Unknown tag key</i>");
        }
    };
    connect(keyBox, &QComboBox::currentTextChanged, &dlg, updateHint);
    updateHint();

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    form->addRow("Tag:", keyBox);
    form->addRow("Value:", valueBox);
    form->addRow(hint);
    form->addRow(buttons);

    if (dlg.exec() != QDialog::Accepted)
        return;

    const QString keyText = keyBox->currentText().trimmed();
    const QString valueText = valueBox->currentText();
    try {
        const std::string k = keyText.toStdString();
        const int family = familyOf(keyText);
        const Exiv2::TypeId type = typeFor(k);

        auto setOrAdd = [&](auto &data, const auto &keyObj) -> bool {
            auto it = data.findKey(keyObj);
            ValuePtr v = (it != data.end()) ? parseLike(*it, family, valueText) : parseFresh(type, valueText);
            if (!v.get()) {
                showError("Invalid value", QString("'%1' is not valid for %2.\n\nExpected: %3")
                                               .arg(valueText, keyText, formatHint(type)));
                return false;
            }
            if (it != data.end())
                it->setValue(v.get());
            else
                data.add(keyObj, v.get());
            return true;
        };

        bool ok = false;
        if (family == FamExif)
            ok = setOrAdd(m_exif, Exiv2::ExifKey(k));
        else if (family == FamIptc)
            ok = setOrAdd(m_iptc, Exiv2::IptcKey(k));
        else if (family == FamXmp)
            ok = setOrAdd(m_xmp, Exiv2::XmpKey(k));
        if (!ok)
            return;

        markEdited(keyText);
        populateTable();
        statusBar()->showMessage(QString("Set %1").arg(keyText), 4000);
    } catch (const std::exception &e) {
        showError("Could not add tag", QString::fromLocal8Bit(e.what()));
    }
}

bool MainWindow::removeEntry(int family, const std::string &key, int occurrence)
{
    auto eraseFrom = [&](auto &data) {
        auto it = findNth(data, key, occurrence);
        if (it == data.end())
            return false;
        data.erase(it);
        return true;
    };
    switch (family) {
    case FamExif: return eraseFrom(m_exif);
    case FamIptc: return eraseFrom(m_iptc);
    case FamXmp: return eraseFrom(m_xmp);
    case FamFile: {
        const QString k = qs(key);
        auto it = std::find_if(m_rawMarkers.begin(), m_rawMarkers.end(),
                               [&](const Provenance::RawMarker &m) { return m.key == k; });
        if (it == m_rawMarkers.end() || !it->removable || m_rawToStrip.contains(k))
            return false;
        m_rawToStrip << k;
        return true;
    }
    }
    return false;
}

void MainWindow::onRemoveTags()
{
    std::set<int> rows;
    for (auto *item : m_table->selectedItems())
        rows.insert(item->row());
    if (rows.empty())
        return;

    struct Victim { int family; std::string key; int occ; };
    std::vector<Victim> victims;
    for (int r : rows) {
        auto *k = m_table->item(r, ColKey);
        victims.push_back({k->data(FamilyRole).toInt(), k->text().toStdString(), k->data(OccurrenceRole).toInt()});
    }
    // Erase higher occurrences first so the indices of the rest stay valid.
    std::sort(victims.begin(), victims.end(), [](const Victim &a, const Victim &b) { return a.occ > b.occ; });
    int removed = 0;
    for (const auto &v : victims)
        removed += removeEntry(v.family, v.key, v.occ) ? 1 : 0;

    if (removed) {
        setModified(true);
        populateTable();
    }
    statusBar()->showMessage(QString("Removed %1 item(s)").arg(removed), 4000);
}

void MainWindow::onRemoveGps()
{
    int removed = 0;
    for (auto it = m_exif.begin(); it != m_exif.end();) {
        if (it->groupName() == "GPSInfo" || it->key() == "Exif.Image.GPSTag") {
            it = m_exif.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    for (auto it = m_xmp.begin(); it != m_xmp.end();) {
        if (QString::fromStdString(it->key()).startsWith("Xmp.exif.GPS")) {
            it = m_xmp.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    if (removed == 0) {
        statusBar()->showMessage("No GPS data in this file", 4000);
        return;
    }
    setModified(true);
    populateTable();
    statusBar()->showMessage(QString("Removed %1 GPS tag(s) — save to apply").arg(removed), 5000);
}

void MainWindow::onRemoveAiMarkers()
{
    const auto markers = findAiMarkers();
    if (markers.isEmpty()) {
        QMessageBox::information(this, "Remove AI Markers",
                                 "No AI markers were found in this file's metadata.\n\n"
                                 "Note: invisible pixel watermarks (such as Google SynthID) live in the "
                                 "image itself and can't be detected or changed by a metadata editor.");
        return;
    }

    QDialog dlg(this);
    dlg.setWindowTitle("Remove AI Markers");
    dlg.resize(760, 420);
    auto *layout = new QVBoxLayout(&dlg);
    auto *intro = new QLabel(QString("Found <b>%1</b> marker(s). Checked items will be removed when you save.")
                                 .arg(markers.size()));
    auto *list = new QListWidget;
    for (int i = 0; i < markers.size(); ++i) {
        const auto &m = markers[i];
        QString value = m.value.simplified();
        if (value.size() > 90)
            value = value.left(90) + "…";
        auto *item = new QListWidgetItem(QString("%1  —  %2\n      %3").arg(m.key, m.reason, value));
        item->setData(Qt::UserRole, i);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        // A C2PA manifest without AI terms is probably from a camera/editor: leave it unchecked.
        const bool likely = !m.reason.startsWith("C2PA manifest (no AI");
        const bool removable = m.family != FamFile ||
            std::any_of(m_rawMarkers.begin(), m_rawMarkers.end(),
                        [&](const Provenance::RawMarker &r) { return r.key == m.key && r.removable; });
        item->setCheckState(likely && removable ? Qt::Checked : Qt::Unchecked);
        if (!removable) {
            item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
            item->setToolTip("This file format isn't supported for removing C2PA data.");
        }
        list->addItem(item);
    }
    auto *note = new QLabel("<small>Invisible pixel watermarks (e.g. Google SynthID) are part of the image "
                            "itself and are not affected.</small>");
    note->setWordWrap(true);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    buttons->addButton("Remove Checked", QDialogButtonBox::AcceptRole);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    layout->addWidget(intro);
    layout->addWidget(list);
    layout->addWidget(note);
    layout->addWidget(buttons);

    if (dlg.exec() != QDialog::Accepted)
        return;

    QVector<AiMarker> chosen;
    for (int i = 0; i < list->count(); ++i)
        if (list->item(i)->checkState() == Qt::Checked)
            chosen.push_back(markers[list->item(i)->data(Qt::UserRole).toInt()]);
    std::sort(chosen.begin(), chosen.end(), [](const AiMarker &a, const AiMarker &b) { return a.occurrence > b.occurrence; });

    int removed = 0;
    for (const auto &m : chosen)
        removed += removeEntry(m.family, m.key.toStdString(), m.occurrence) ? 1 : 0;
    if (!removed)
        return;
    setModified(true);
    populateTable();
    statusBar()->showMessage(QString("Removed %1 AI marker(s) — save to apply").arg(removed), 5000);
}

void MainWindow::onStripAll()
{
    if (QMessageBox::question(this, "Remove all EXIF",
                              "Remove every EXIF tag from this file?\n\n"
                              "This includes camera settings, dates, GPS and orientation. IPTC and XMP are "
                              "kept. Nothing changes on disk until you save.") != QMessageBox::Yes)
        return;
    m_exif.clear();
    setModified(true);
    populateTable();
}

bool MainWindow::onSave()
{
    if (!m_image.get())
        return false;

    const QDateTime mtime = QFileInfo(m_path).lastModified();

    try {
        m_image->setExifData(m_exif);
        m_image->setIptcData(m_iptc);
        m_image->setXmpData(m_xmp);
        m_image->writeMetadata();
    } catch (const ExivError &e) {
        showError("Could not save", QString("Writing metadata to %1 failed:\n\n%2").arg(m_path, exivMessage(e)));
        return false;
    }

    // C2PA manifests and generator text chunks aren't handled by Exiv2; strip them directly.
    if (!m_rawToStrip.isEmpty()) {
        QString err;
        if (!Provenance::strip(m_path, m_rawToStrip, &err)) {
            showError("Could not remove file-level markers",
                      QString("The metadata was saved, but removing C2PA / generator data failed:\n\n%1").arg(err));
        }
    }

    if (m_keepTimeAct->isChecked()) {
        QFile f(m_path);
        if (f.open(QIODevice::ReadWrite))
            f.setFileTime(mtime, QFileDevice::FileModificationTime);
    }

    // Reload from disk so the table shows exactly what got written.
    m_modified = false;
    const QString path = m_path;
    openFile(path);
    statusBar()->showMessage(QString("Saved %1").arg(path), 5000);
    return true;
}

void MainWindow::onRevert()
{
    if (!m_image.get())
        return;
    m_modified = false;
    openFile(m_path);
    statusBar()->showMessage("Reverted to file on disk", 4000);
}

void MainWindow::onOpen()
{
    QSettings s;
    const QString dir = s.value("lastDir", QDir::homePath()).toString();
    const QString path = QFileDialog::getOpenFileName(
        this, "Open Image", dir,
        "Images (*.jpg *.jpeg *.tif *.tiff *.png *.webp *.heic *.heif *.avif *.jxl "
        "*.dng *.cr2 *.cr3 *.nef *.arw *.orf *.rw2 *.raf *.pef *.srw);;All files (*)");
    if (path.isEmpty())
        return;
    s.setValue("lastDir", QFileInfo(path).absolutePath());
    openFile(path);
}

void MainWindow::onFilterChanged(const QString &text)
{
    for (int r = 0; r < m_table->rowCount(); ++r) {
        bool match = text.isEmpty();
        auto *keyItem = m_table->item(r, ColKey);
        if (!match && keyItem && keyItem->data(Qt::UserRole + 3).toString().contains(text, Qt::CaseInsensitive))
            match = true;
        for (int c = 0; !match && c < ColCount; ++c) {
            auto *it = m_table->item(r, c);
            match = it && it->text().contains(text, Qt::CaseInsensitive);
        }
        m_table->setRowHidden(r, !match);
    }
}

void MainWindow::setModified(bool modified)
{
    m_modified = modified;
    const QString title = m_path.isEmpty() ? QString("EXIF Editor") : QFileInfo(m_path).fileName();
    setWindowTitle(title + "[*]");
    setWindowModified(modified);
    updateActions();
}

void MainWindow::updateActions()
{
    const bool loaded = m_image.get() != nullptr;
    m_saveAct->setEnabled(loaded && m_modified);
    m_revertAct->setEnabled(loaded && m_modified);
    m_addAct->setEnabled(loaded);
    m_removeAct->setEnabled(loaded && m_table && !m_table->selectedItems().isEmpty());
    m_gpsAct->setEnabled(loaded);
    m_aiAct->setEnabled(loaded);
    m_stripAct->setEnabled(loaded && !m_exif.empty());
}

bool MainWindow::maybeSave()
{
    if (!m_modified)
        return true;
    const auto ret = QMessageBox::warning(
        this, "Unsaved changes",
        QString("%1 has unsaved metadata changes. Save them?").arg(QFileInfo(m_path).fileName()),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (ret == QMessageBox::Save)
        return onSave();
    if (ret == QMessageBox::Discard) {
        m_modified = false;
        return true;
    }
    return false;
}

void MainWindow::showError(const QString &title, const QString &message)
{
    QMessageBox::critical(this, title, message);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (maybeSave()) {
        QSettings().setValue("geometry", saveGeometry());
        event->accept();
    } else {
        event->ignore();
    }
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *event)
{
    const auto urls = event->mimeData()->urls();
    if (!urls.isEmpty() && urls.first().isLocalFile())
        openFile(urls.first().toLocalFile());
}
