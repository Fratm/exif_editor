#pragma once

// File-level AI / provenance markers that Exiv2 does not expose as metadata:
//  - C2PA "Content Credentials" manifests (JUMBF), stored in
//      JPEG: APP11 segments          PNG: caBX chunk          WebP: C2PA chunk
//  - PNG text chunks written by image generators (Stable Diffusion "parameters",
//    ComfyUI "prompt"/"workflow", InvokeAI, NovelAI, ...)

#include <QString>
#include <QStringList>
#include <QVector>

namespace Provenance {

struct RawMarker {
    QString key;          // e.g. "C2PA.Manifest" or "PNG.Text.parameters"
    QString description;  // human readable name
    QString value;        // short summary of contents
    qint64 bytes = 0;
    bool removable = true;  // false when the file format isn't supported for stripping
};

// Inspect the file on disk.
QVector<RawMarker> scan(const QString &path);

// Remove the given markers (by key) from the file in place.
// Returns false and fills *error on failure.
bool strip(const QString &path, const QStringList &keys, QString *error);

// Does this text mention a known AI generator / AI provenance term?
// Returns the matched term, or an empty string.
QString aiMention(const QString &text);

} // namespace Provenance
