#include "provenance.h"

#include <QFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QtEndian>

#include <map>
#include <zlib.h>

namespace Provenance {

namespace {

// Text chunk keywords that image generators use to store prompts and settings.
const QSet<QString> kPngAiKeywords = {
    "parameters",       // AUTOMATIC1111 / Forge / SD.Next
    "prompt",           // ComfyUI
    "workflow",         // ComfyUI
    "invokeai_metadata", "invokeai_graph", "sd-metadata", "Dream",  // InvokeAI
    "generation_data",  // Fooocus and others
    "NovelAI", "Software:NovelAI",
    "ai_metadata", "c2pa",
};

// Generator text chunk? (Exiv2-managed chunks like the XMP packet are handled as metadata instead.)
bool isAiTextChunk(const QString &kw, const QByteArray &text)
{
    if (kw == "XML:com.adobe.xmp" || kw.startsWith("Raw profile type"))
        return false;
    return kPngAiKeywords.contains(kw) || !aiMention(QString::fromUtf8(text)).isEmpty();
}

QString summarize(const QByteArray &bytes, int maxLen = 160)
{
    QString s = QString::fromUtf8(bytes).simplified();
    s.remove(QChar(0));
    if (s.size() > maxLen)
        s = s.left(maxLen) + "…";
    return s;
}

// Collect printable text runs from binary data and look for AI terms in them.
QString binaryMentions(const QByteArray &data)
{
    QStringList hits;
    QByteArray run;
    auto flush = [&] {
        if (run.size() >= 4) {
            const QString m = aiMention(QString::fromLatin1(run));
            if (!m.isEmpty() && !hits.contains(m))
                hits << m;
        }
        run.clear();
    };
    for (char c : data) {
        if (c >= 0x20 && c < 0x7f)
            run.append(c);
        else
            flush();
    }
    flush();
    return hits.join(", ");
}

enum class Format { Jpeg, Png, WebP, Other };

Format detect(const QByteArray &d)
{
    if (d.size() > 3 && uchar(d[0]) == 0xFF && uchar(d[1]) == 0xD8)
        return Format::Jpeg;
    if (d.startsWith("\x89PNG\r\n\x1a\n"))
        return Format::Png;
    if (d.size() > 12 && d.startsWith("RIFF") && d.mid(8, 4) == "WEBP")
        return Format::WebP;
    return Format::Other;
}

// ---------------- JPEG ----------------
struct JpegSeg {
    int start;     // offset of 0xFF marker
    int length;    // total bytes including marker
    uchar marker;
};

// Walk the header segments up to SOS. Returns false if the file looks malformed.
bool jpegSegments(const QByteArray &d, QVector<JpegSeg> &segs, int &sosOffset)
{
    int pos = 2;
    while (pos + 4 <= d.size()) {
        if (uchar(d[pos]) != 0xFF)
            return false;
        uchar m = uchar(d[pos + 1]);
        if (m == 0xFF) { ++pos; continue; }                // fill byte
        if (m == 0xDA || m == 0xD9) { sosOffset = pos; return true; }
        if ((m >= 0xD0 && m <= 0xD7) || m == 0x01) {       // no length field
            segs.push_back({pos, 2, m});
            pos += 2;
            continue;
        }
        int len = (uchar(d[pos + 2]) << 8) | uchar(d[pos + 3]);
        if (len < 2 || pos + 2 + len > d.size())
            return false;
        segs.push_back({pos, 2 + len, m});
        pos += 2 + len;
    }
    return false;
}

// APP11 JUMBF segment: FF EB Lp(2) 'J''P' En(2) Z(4) <box data>
// A C2PA manifest store may span several segments that share the same En.
std::map<int, QByteArray> jpegC2paInstances(const QByteArray &d, const QVector<JpegSeg> &segs)
{
    std::map<int, QByteArray> byInstance;
    for (const auto &s : segs) {
        if (s.marker != 0xEB || s.length < 12)
            continue;
        const QByteArray body = d.mid(s.start + 4, s.length - 4);
        if (!body.startsWith("JP"))
            continue;
        const int en = (uchar(body[2]) << 8) | uchar(body[3]);
        byInstance[en].append(body.mid(8));
    }
    for (auto it = byInstance.begin(); it != byInstance.end();) {
        if (it->second.contains("c2pa"))
            ++it;
        else
            it = byInstance.erase(it);
    }
    return byInstance;
}

// ---------------- PNG ----------------
struct PngChunk {
    int start;      // offset of length field
    int total;      // 12 + data length
    QByteArray type;
    QByteArray data;
};

bool pngChunks(const QByteArray &d, QVector<PngChunk> &chunks)
{
    int pos = 8;
    while (pos + 12 <= d.size()) {
        const quint32 len = qFromBigEndian<quint32>(d.constData() + pos);
        if (len > quint32(d.size() - pos - 12))
            return false;
        chunks.push_back({pos, int(len) + 12, d.mid(pos + 4, 4), d.mid(pos + 8, int(len))});
        pos += int(len) + 12;
        if (chunks.back().type == "IEND")
            return true;
    }
    return false;
}

// Keyword + readable text of a tEXt / zTXt / iTXt chunk.
bool pngTextChunk(const PngChunk &c, QString &keyword, QByteArray &text)
{
    const int nul = c.data.indexOf('\0');
    if (nul <= 0)
        return false;
    keyword = QString::fromLatin1(c.data.left(nul));
    if (c.type == "tEXt") {
        text = c.data.mid(nul + 1);
    } else if (c.type == "zTXt") {
        QByteArray comp = c.data.mid(nul + 2);
        uLongf outLen = 1 << 20;
        QByteArray out(int(outLen), Qt::Uninitialized);
        if (uncompress(reinterpret_cast<Bytef *>(out.data()), &outLen,
                       reinterpret_cast<const Bytef *>(comp.constData()), uLong(comp.size())) == Z_OK)
            text = out.left(int(outLen));
    } else if (c.type == "iTXt") {
        // keyword\0 compFlag compMethod lang\0 translatedKeyword\0 text
        int p = nul + 3;
        p = c.data.indexOf('\0', p) + 1;   // after language tag
        p = c.data.indexOf('\0', p) + 1;   // after translated keyword
        if (p > 0 && c.data[nul + 1] == 0)
            text = c.data.mid(p);
        else
            text = "<compressed>";
    } else {
        return false;
    }
    return true;
}

// ---------------- WebP ----------------
struct RiffChunk {
    int start;
    int total;   // 8 + size + pad
    QByteArray fourcc;
    QByteArray data;
};

bool webpChunks(const QByteArray &d, QVector<RiffChunk> &chunks)
{
    int pos = 12;
    while (pos + 8 <= d.size()) {
        const quint32 size = qFromLittleEndian<quint32>(d.constData() + pos + 4);
        const int total = 8 + int(size) + int(size & 1);
        if (size > quint32(d.size() - pos - 8))
            return false;
        chunks.push_back({pos, int(qMin<qsizetype>(total, d.size() - pos)), d.mid(pos, 4), d.mid(pos + 8, int(size))});
        pos += total;
    }
    return true;
}

bool readAll(const QString &path, QByteArray &out, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = f.errorString();
        return false;
    }
    out = f.readAll();
    return true;
}

} // namespace

QString aiMention(const QString &text)
{
    static const QRegularExpression re(
        QStringLiteral(
            "(trainedAlgorithmicMedia|compositeWithTrainedAlgorithmicMedia|algorithmicMedia|compositeSynthetic"
            "|\\bfirefly\\b|midjourney|dall[\\s\\-·.]?e|\\bopenai\\b|chatgpt|gpt-4o|gpt-image|\\bsora\\b"
            "|stable[\\s\\-_]?diffusion|stability\\s?ai|\\bsdxl\\b|comfyui|automatic1111|invokeai|fooocus"
            "|novelai|google\\s+imagen|\\bimagen\\s?\\d|\\bgemini\\b|nano[\\s\\-]?banana|synthid|made with google ai|edited with google ai"
            "|\\bflux\\.\\d|black forest labs|leonardo\\.ai|ideogram|bing image creator|image creator from microsoft"
            "|\\bmeta ai\\b|imagined with ai|\\bgrok\\b|runwayml|runway gen|adobe generative|generative fill|generative expand"
            "|ai[\\s\\-]generated|generated (by|with) ai|made with ai"
            "|negative prompt:|cfg scale:|sampler:)"),
        QRegularExpression::CaseInsensitiveOption);
    const auto m = re.match(text);
    return m.hasMatch() ? m.captured(1) : QString();
}

QVector<RawMarker> scan(const QString &path)
{
    QVector<RawMarker> out;
    QByteArray d;
    if (!readAll(path, d, nullptr))
        return out;

    switch (detect(d)) {
    case Format::Jpeg: {
        QVector<JpegSeg> segs;
        int sos = 0;
        if (!jpegSegments(d, segs, sos))
            break;
        for (const auto &[en, data] : jpegC2paInstances(d, segs)) {
            const QString mentions = binaryMentions(data);
            out.push_back({QString("C2PA.Manifest.%1").arg(en), "C2PA Content Credentials",
                           mentions.isEmpty() ? QString("Signed provenance manifest")
                                              : "Mentions: " + mentions,
                           data.size(), true});
        }
        break;
    }
    case Format::Png: {
        QVector<PngChunk> chunks;
        pngChunks(d, chunks);
        for (const auto &c : chunks) {
            if (c.type == "caBX") {
                const QString mentions = binaryMentions(c.data);
                out.push_back({"C2PA.Manifest", "C2PA Content Credentials",
                               mentions.isEmpty() ? QString("Signed provenance manifest") : "Mentions: " + mentions,
                               c.data.size(), true});
                continue;
            }
            QString kw;
            QByteArray text;
            if (!pngTextChunk(c, kw, text))
                continue;
            if (isAiTextChunk(kw, text)) {
                // Avoid duplicate keys if a keyword repeats.
                QString key = "PNG.Text." + kw;
                int n = 2;
                while (std::any_of(out.begin(), out.end(), [&](const RawMarker &m) { return m.key == key; }))
                    key = QString("PNG.Text.%1#%2").arg(kw).arg(n++);
                out.push_back({key, QString("PNG %1 chunk \"%2\"").arg(QString::fromLatin1(c.type), kw),
                               summarize(text), c.data.size(), true});
            }
        }
        break;
    }
    case Format::WebP: {
        QVector<RiffChunk> chunks;
        webpChunks(d, chunks);
        for (const auto &c : chunks) {
            if (c.fourcc == "C2PA") {
                const QString mentions = binaryMentions(c.data);
                out.push_back({"C2PA.Manifest", "C2PA Content Credentials",
                               mentions.isEmpty() ? QString("Signed provenance manifest") : "Mentions: " + mentions,
                               c.data.size(), true});
            }
        }
        break;
    }
    case Format::Other:
        // HEIC/AVIF/TIFF/RAW: report presence but don't try to rewrite the container.
        if (d.contains("jumb") && d.contains("c2pa")) {
            out.push_back({"C2PA.Manifest", "C2PA Content Credentials",
                           "Present — removal is only supported for JPEG, PNG and WebP", 0, false});
        }
        break;
    }
    return out;
}

bool strip(const QString &path, const QStringList &keys, QString *error)
{
    if (keys.isEmpty())
        return true;

    QByteArray d;
    if (!readAll(path, d, error))
        return false;

    QByteArray out;
    out.reserve(d.size());

    switch (detect(d)) {
    case Format::Jpeg: {
        QVector<JpegSeg> segs;
        int sos = 0;
        if (!jpegSegments(d, segs, sos)) {
            if (error) *error = "Could not parse JPEG structure.";
            return false;
        }
        QSet<int> dropInstances;
        for (const auto &[en, data] : jpegC2paInstances(d, segs)) {
            Q_UNUSED(data);
            if (keys.contains(QString("C2PA.Manifest.%1").arg(en)))
                dropInstances.insert(en);
        }
        out.append(d.left(2));
        for (const auto &s : segs) {
            if (s.marker == 0xEB && s.length >= 12 && d.mid(s.start + 4, 2) == "JP") {
                const int en = (uchar(d[s.start + 6]) << 8) | uchar(d[s.start + 7]);
                if (dropInstances.contains(en))
                    continue;
            }
            out.append(d.mid(s.start, s.length));
        }
        out.append(d.mid(sos));
        break;
    }
    case Format::Png: {
        QVector<PngChunk> chunks;
        if (!pngChunks(d, chunks)) {
            if (error) *error = "Could not parse PNG structure.";
            return false;
        }
        // Rebuild the same key names scan() produced so selections line up.
        QMap<QString, int> seen;
        out.append(d.left(8));
        for (const auto &c : chunks) {
            bool drop = false;
            if (c.type == "caBX") {
                drop = keys.contains("C2PA.Manifest");
            } else {
                QString kw;
                QByteArray text;
                if (pngTextChunk(c, kw, text) && isAiTextChunk(kw, text)) {
                    const int n = ++seen[kw];
                    const QString key = n == 1 ? "PNG.Text." + kw : QString("PNG.Text.%1#%2").arg(kw).arg(n);
                    drop = keys.contains(key);
                }
            }
            if (!drop)
                out.append(d.mid(c.start, c.total));
        }
        break;
    }
    case Format::WebP: {
        QVector<RiffChunk> chunks;
        if (!webpChunks(d, chunks)) {
            if (error) *error = "Could not parse WebP structure.";
            return false;
        }
        out.append(d.left(12));
        for (const auto &c : chunks) {
            if (c.fourcc == "C2PA" && keys.contains("C2PA.Manifest"))
                continue;
            out.append(d.mid(c.start, c.total));
        }
        qToLittleEndian<quint32>(quint32(out.size() - 8), out.data() + 4);
        break;
    }
    case Format::Other:
        if (error) *error = "Removing C2PA data is only supported for JPEG, PNG and WebP files.";
        return false;
    }

    // Write atomically so a crash can never leave a half-written photo.
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(out) != out.size() || !f.commit()) {
        if (error) *error = f.errorString();
        return false;
    }
    return true;
}

} // namespace Provenance
