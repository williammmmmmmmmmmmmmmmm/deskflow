/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "deskflow/ClipboardFileTransfer.h"

#include "base/Log.h"
#include "deskflow/Clipboard.h"
#include "deskflow/IClipboard.h"

#include <algorithm>
#include <array>
#include <limits>
#include <string_view>
#include <vector>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QStringList>
#include <QUrl>
#include <QUuid>

namespace deskflow {
namespace {

constexpr std::string_view kMagic = "DFCB";
constexpr std::uint32_t kVersion = 1;
constexpr std::uint32_t kMaximumEntries = 1'000'000;
constexpr qsizetype kIoChunkSize = 64 * 1024;

enum class EntryType : std::uint8_t
{
  Directory = 1,
  File = 2,
};

struct ClipboardSnapshot
{
  std::array<bool, static_cast<std::size_t>(IClipboard::Format::TotalFormats)> added{};
  std::array<std::string, static_cast<std::size_t>(IClipboard::Format::TotalFormats)> data;
  IClipboard::Time time = 0;
};

struct SourceEntry
{
  QString source;
  QString relative;
  EntryType type;
  QFileDevice::Permissions permissions;
  std::uint64_t size;
};

struct ParsedEntry
{
  QString relative;
  EntryType type;
  QFileDevice::Permissions permissions;
  std::string_view contents;
};

void setError(QString *error, const QString &message)
{
  if (error)
    *error = message;
}

void appendU32(std::string &out, std::uint32_t value)
{
  out.push_back(static_cast<char>((value >> 24) & 0xff));
  out.push_back(static_cast<char>((value >> 16) & 0xff));
  out.push_back(static_cast<char>((value >> 8) & 0xff));
  out.push_back(static_cast<char>(value & 0xff));
}

void appendU64(std::string &out, std::uint64_t value)
{
  appendU32(out, static_cast<std::uint32_t>(value >> 32));
  appendU32(out, static_cast<std::uint32_t>(value & 0xffffffffu));
}

void replaceU32(std::string &out, std::size_t offset, std::uint32_t value)
{
  out[offset] = static_cast<char>((value >> 24) & 0xff);
  out[offset + 1] = static_cast<char>((value >> 16) & 0xff);
  out[offset + 2] = static_cast<char>((value >> 8) & 0xff);
  out[offset + 3] = static_cast<char>(value & 0xff);
}

bool appendBytes(std::string &out, std::string_view bytes, std::uint64_t maximum)
{
  if (bytes.size() > maximum || out.size() > maximum - bytes.size())
    return false;
  out.append(bytes);
  return true;
}

bool appendString(std::string &out, const QByteArray &bytes, std::uint64_t maximum)
{
  if (bytes.size() < 0 || static_cast<std::uint64_t>(bytes.size()) > std::numeric_limits<std::uint32_t>::max())
    return false;
  if (out.size() > maximum || maximum - out.size() < 4 + static_cast<std::uint64_t>(bytes.size()))
    return false;
  appendU32(out, static_cast<std::uint32_t>(bytes.size()));
  out.append(bytes.constData(), static_cast<std::size_t>(bytes.size()));
  return true;
}

bool takeU8(std::string_view input, std::size_t &offset, std::uint8_t &value)
{
  if (offset >= input.size())
    return false;
  value = static_cast<std::uint8_t>(input[offset++]);
  return true;
}

bool takeU32(std::string_view input, std::size_t &offset, std::uint32_t &value)
{
  if (input.size() - std::min(input.size(), offset) < 4)
    return false;
  const auto *bytes = reinterpret_cast<const unsigned char *>(input.data() + offset);
  value = (static_cast<std::uint32_t>(bytes[0]) << 24) | (static_cast<std::uint32_t>(bytes[1]) << 16) |
          (static_cast<std::uint32_t>(bytes[2]) << 8) | static_cast<std::uint32_t>(bytes[3]);
  offset += 4;
  return true;
}

bool takeU64(std::string_view input, std::size_t &offset, std::uint64_t &value)
{
  std::uint32_t high = 0;
  std::uint32_t low = 0;
  if (!takeU32(input, offset, high) || !takeU32(input, offset, low))
    return false;
  value = (static_cast<std::uint64_t>(high) << 32) | low;
  return true;
}

bool takeString(std::string_view input, std::size_t &offset, QString &value)
{
  std::uint32_t size = 0;
  if (!takeU32(input, offset, size) || size > input.size() - std::min(input.size(), offset))
    return false;

  const QByteArray bytes(input.data() + offset, static_cast<qsizetype>(size));
  value = QString::fromUtf8(bytes);
  offset += size;
  return value.toUtf8() == bytes;
}

bool readSnapshot(const IClipboard *clipboard, ClipboardSnapshot &snapshot)
{
  if (!clipboard)
    return false;

  snapshot.time = clipboard->getTime();
  if (!clipboard->open(snapshot.time))
    return false;

  for (int value = 0; value < static_cast<int>(IClipboard::Format::TotalFormats); ++value) {
    const auto format = static_cast<IClipboard::Format>(value);
    snapshot.added[value] = clipboard->has(format);
    if (snapshot.added[value])
      snapshot.data[value] = clipboard->get(format);
  }
  clipboard->close();
  return true;
}

bool writeSnapshot(IClipboard *clipboard, const ClipboardSnapshot &snapshot)
{
  if (!clipboard || !clipboard->open(snapshot.time))
    return false;
  if (!clipboard->empty()) {
    clipboard->close();
    return false;
  }

  for (int value = 0; value < static_cast<int>(IClipboard::Format::TotalFormats); ++value) {
    if (snapshot.added[value])
      clipboard->add(static_cast<IClipboard::Format>(value), snapshot.data[value]);
  }
  clipboard->close();
  return true;
}

QStringList mimeLines(std::string_view bytes)
{
  QString text = QString::fromUtf8(bytes.data(), static_cast<qsizetype>(bytes.size()));
  text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
  text.replace(u'\r', u'\n');
  return text.split(u'\n', Qt::SkipEmptyParts);
}

QStringList localPaths(std::string_view uriList, std::string_view gnomeCopiedFiles, bool &cut, QString *error)
{
  cut = false;
  QStringList lines;
  const auto gnomeLines = mimeLines(gnomeCopiedFiles);
  if (!gnomeLines.isEmpty() &&
      (gnomeLines.front() == QStringLiteral("copy") || gnomeLines.front() == QStringLiteral("cut"))) {
    cut = gnomeLines.front() == QStringLiteral("cut");
    lines = gnomeLines.mid(1);
  } else {
    lines = mimeLines(uriList);
  }

  QStringList paths;
  QSet<QString> seen;
  for (const auto &rawLine : lines) {
    const auto line = rawLine.trimmed();
    if (line.isEmpty() || line.startsWith(u'#'))
      continue;

    const auto url = QUrl::fromEncoded(line.toUtf8(), QUrl::StrictMode);
    if (!url.isValid() || !url.isLocalFile()) {
      setError(error, QStringLiteral("clipboard contains a non-local file URI"));
      return {};
    }

    const auto path = QDir::cleanPath(url.toLocalFile());
    const QFileInfo info(path);
    if (!info.exists()) {
      setError(error, QStringLiteral("clipboard file no longer exists: %1").arg(path));
      return {};
    }

    const auto absolute = info.absoluteFilePath();
    if (!seen.contains(absolute)) {
      seen.insert(absolute);
      paths.append(absolute);
    }
  }

  if (paths.isEmpty())
    setError(error, QStringLiteral("clipboard contains no transferable local files"));
  return paths;
}

bool collectEntry(
    const QFileInfo &info, const QString &relative, std::vector<SourceEntry> &entries, QString *error
)
{
  if (info.isSymLink()) {
    setError(error, QStringLiteral("symbolic links are not supported: %1").arg(info.filePath()));
    return false;
  }

  if (info.isFile()) {
    if (info.size() < 0) {
      setError(error, QStringLiteral("invalid file size: %1").arg(info.filePath()));
      return false;
    }
    entries.push_back(
        {info.absoluteFilePath(), relative, EntryType::File, info.permissions(), static_cast<std::uint64_t>(info.size())}
    );
    return true;
  }

  if (!info.isDir()) {
    setError(error, QStringLiteral("unsupported clipboard file type: %1").arg(info.filePath()));
    return false;
  }

  entries.push_back({info.absoluteFilePath(), relative, EntryType::Directory, info.permissions(), 0});
  const QDir directory(info.absoluteFilePath());
  const auto children = directory.entryInfoList(
      QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::DirsFirst | QDir::Name
  );
  for (const auto &child : children) {
    if (!collectEntry(child, relative + u'/' + child.fileName(), entries, error))
      return false;
  }
  return true;
}

QString uniqueRootName(const QString &requested, QSet<QString> &used)
{
  QString base = requested;
  if (base.isEmpty() || base == QStringLiteral(".") || base == QStringLiteral(".."))
    base = QStringLiteral("file");

  QString candidate = base;
  int suffix = 2;
  while (used.contains(candidate))
    candidate = QStringLiteral("%1 (%2)").arg(base).arg(suffix++);
  used.insert(candidate);
  return candidate;
}

bool validRelativePath(const QString &path)
{
  if (path.isEmpty() || QDir::isAbsolutePath(path) || path.contains(u'\0'))
    return false;
  const auto components = path.split(u'/', Qt::KeepEmptyParts);
  return std::ranges::all_of(components, [](const QString &part) {
    return !part.isEmpty() && part != QStringLiteral(".") && part != QStringLiteral("..");
  });
}

bool parseBundle(
    std::string_view bundle, bool &cut, QStringList &roots, std::vector<ParsedEntry> &entries, QString *error
)
{
  if (!bundle.starts_with(kMagic)) {
    setError(error, QStringLiteral("invalid file clipboard signature"));
    return false;
  }

  std::size_t offset = kMagic.size();
  std::uint32_t version = 0;
  std::uint8_t action = 0;
  std::uint32_t rootCount = 0;
  if (!takeU32(bundle, offset, version) || version != kVersion || !takeU8(bundle, offset, action) || action > 1 ||
      !takeU32(bundle, offset, rootCount) || rootCount == 0 || rootCount > kMaximumEntries) {
    setError(error, QStringLiteral("invalid file clipboard header"));
    return false;
  }
  cut = action == 1;

  QSet<QString> rootSet;
  for (std::uint32_t index = 0; index < rootCount; ++index) {
    QString root;
    if (!takeString(bundle, offset, root) || !validRelativePath(root) || root.contains(u'/') || rootSet.contains(root)) {
      setError(error, QStringLiteral("invalid file clipboard root"));
      return false;
    }
    roots.append(root);
    rootSet.insert(root);
  }

  std::uint32_t entryCount = 0;
  if (!takeU32(bundle, offset, entryCount) || entryCount == 0 || entryCount > kMaximumEntries) {
    setError(error, QStringLiteral("invalid file clipboard entry count"));
    return false;
  }

  QSet<QString> paths;
  QSet<QString> presentRoots;
  entries.reserve(entryCount);
  for (std::uint32_t index = 0; index < entryCount; ++index) {
    std::uint8_t rawType = 0;
    std::uint32_t rawPermissions = 0;
    QString relative;
    std::uint64_t size = 0;
    if (!takeU8(bundle, offset, rawType) ||
        (rawType != static_cast<std::uint8_t>(EntryType::Directory) &&
         rawType != static_cast<std::uint8_t>(EntryType::File)) ||
        !takeU32(bundle, offset, rawPermissions) || !takeString(bundle, offset, relative) ||
        !validRelativePath(relative) || paths.contains(relative) || !takeU64(bundle, offset, size) ||
        size > bundle.size() - std::min(bundle.size(), offset)) {
      setError(error, QStringLiteral("invalid file clipboard entry"));
      return false;
    }

    const auto type = static_cast<EntryType>(rawType);
    if (type == EntryType::Directory && size != 0) {
      setError(error, QStringLiteral("directory entry contains file data"));
      return false;
    }

    const auto root = relative.section(u'/', 0, 0);
    if (!rootSet.contains(root)) {
      setError(error, QStringLiteral("file clipboard entry is outside its roots"));
      return false;
    }
    if (relative == root)
      presentRoots.insert(root);

    paths.insert(relative);
    entries.push_back(
        {relative, type, QFileDevice::Permissions::fromInt(rawPermissions), bundle.substr(offset, size)}
    );
    offset += static_cast<std::size_t>(size);
  }

  if (offset != bundle.size() || presentRoots.size() != roots.size()) {
    setError(error, QStringLiteral("file clipboard payload is incomplete"));
    return false;
  }
  return true;
}

void removeFileFormats(ClipboardSnapshot &snapshot)
{
  for (const auto format :
       {IClipboard::Format::UriList, IClipboard::Format::GnomeCopiedFiles, IClipboard::Format::FileBundle}) {
    const auto index = static_cast<std::size_t>(format);
    snapshot.added[index] = false;
    snapshot.data[index].clear();
  }
}

} // namespace

std::string ClipboardFileTransfer::buildBundle(
    std::string_view uriList, std::string_view gnomeCopiedFiles, std::uint64_t maximumBundleBytes, QString *error
)
{
  bool cut = false;
  const auto paths = localPaths(uriList, gnomeCopiedFiles, cut, error);
  if (paths.isEmpty())
    return {};

  QStringList roots;
  QSet<QString> usedRoots;
  std::vector<SourceEntry> entries;
  for (const auto &path : paths) {
    const QFileInfo info(path);
    const auto root = uniqueRootName(info.fileName(), usedRoots);
    roots.append(root);
    if (!collectEntry(info, root, entries, error))
      return {};
    if (entries.size() > kMaximumEntries) {
      setError(error, QStringLiteral("too many files in clipboard selection"));
      return {};
    }
  }

  std::string result(kMagic);
  if (maximumBundleBytes < result.size() + 13) {
    setError(error, QStringLiteral("file clipboard exceeds the configured clipboard size limit"));
    return {};
  }
  appendU32(result, kVersion);
  result.push_back(cut ? '\1' : '\0');
  appendU32(result, static_cast<std::uint32_t>(roots.size()));
  for (const auto &root : roots) {
    if (!appendString(result, root.toUtf8(), maximumBundleBytes)) {
      setError(error, QStringLiteral("file clipboard exceeds the configured clipboard size limit"));
      return {};
    }
  }

  const auto entryCountOffset = result.size();
  if (result.size() > maximumBundleBytes || maximumBundleBytes - result.size() < 4) {
    setError(error, QStringLiteral("file clipboard exceeds the configured clipboard size limit"));
    return {};
  }
  appendU32(result, 0);
  std::uint32_t entryCount = 0;
  std::array<char, kIoChunkSize> buffer;
  for (const auto &entry : entries) {
    const auto relative = entry.relative.toUtf8();
    const std::uint64_t headerBytes = 1 + 4 + 4 + static_cast<std::uint64_t>(relative.size()) + 8;
    if (result.size() > maximumBundleBytes || headerBytes > maximumBundleBytes - result.size() ||
        entry.size > maximumBundleBytes - result.size() - headerBytes) {
      setError(error, QStringLiteral("file clipboard exceeds the configured clipboard size limit"));
      return {};
    }

    result.push_back(static_cast<char>(entry.type));
    appendU32(result, static_cast<std::uint32_t>(entry.permissions.toInt()));
    appendU32(result, static_cast<std::uint32_t>(relative.size()));
    result.append(relative.constData(), static_cast<std::size_t>(relative.size()));
    appendU64(result, entry.size);

    if (entry.type == EntryType::File) {
      QFile file(entry.source);
      if (!file.open(QIODevice::ReadOnly)) {
        setError(error, QStringLiteral("cannot read clipboard file: %1").arg(entry.source));
        return {};
      }

      std::uint64_t remaining = entry.size;
      while (remaining > 0) {
        const auto wanted = static_cast<qint64>(std::min<std::uint64_t>(remaining, buffer.size()));
        const auto count = file.read(buffer.data(), wanted);
        if (count <= 0 || !appendBytes(result, std::string_view(buffer.data(), count), maximumBundleBytes)) {
          setError(error, QStringLiteral("clipboard file changed while being read: %1").arg(entry.source));
          return {};
        }
        remaining -= static_cast<std::uint64_t>(count);
      }
    }
    ++entryCount;
  }
  replaceU32(result, entryCountOffset, entryCount);
  return result;
}

bool ClipboardFileTransfer::extractBundle(
    std::string_view bundle, std::string &uriList, std::string &gnomeCopiedFiles, const QString &destinationRoot,
    QString *error
)
{
  bool cut = false;
  QStringList roots;
  std::vector<ParsedEntry> entries;
  if (!parseBundle(bundle, cut, roots, entries, error))
    return false;

  QString base = destinationRoot;
  if (base.isEmpty()) {
    base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (base.isEmpty())
      base = QDir::tempPath() + QStringLiteral("/deskflow");
    base += QStringLiteral("/clipboard-files");
  }
  if (!QDir().mkpath(base)) {
    setError(error, QStringLiteral("cannot create clipboard file cache"));
    return false;
  }

  const auto transferPath =
      QDir(base).filePath(QUuid::createUuid().toString(QUuid::WithoutBraces));
  if (!QDir().mkpath(transferPath)) {
    setError(error, QStringLiteral("cannot create clipboard transfer directory"));
    return false;
  }

  const auto fail = [&](const QString &message) {
    QDir(transferPath).removeRecursively();
    setError(error, message);
    return false;
  };

  for (const auto &entry : entries) {
    if (entry.type != EntryType::Directory)
      continue;
    const auto output = QDir(transferPath).filePath(entry.relative);
    if (!QDir().mkpath(output))
      return fail(QStringLiteral("cannot create received clipboard directory"));
    QFile(output).setPermissions(entry.permissions);
  }

  for (const auto &entry : entries) {
    if (entry.type != EntryType::File)
      continue;
    const auto output = QDir(transferPath).filePath(entry.relative);
    if (!QDir().mkpath(QFileInfo(output).absolutePath()))
      return fail(QStringLiteral("cannot create received clipboard file directory"));

    QSaveFile file(output);
    if (!file.open(QIODevice::WriteOnly))
      return fail(QStringLiteral("cannot create received clipboard file"));

    std::size_t offset = 0;
    while (offset < entry.contents.size()) {
      const auto count = static_cast<qint64>(
          std::min<std::size_t>(entry.contents.size() - offset, static_cast<std::size_t>(kIoChunkSize))
      );
      if (file.write(entry.contents.data() + offset, count) != count)
        return fail(QStringLiteral("cannot write received clipboard file"));
      offset += static_cast<std::size_t>(count);
    }
    if (!file.commit())
      return fail(QStringLiteral("cannot finish received clipboard file"));
    QFile(output).setPermissions(entry.permissions);
  }

  QStringList encodedUris;
  for (const auto &root : roots) {
    const auto output = QDir(transferPath).filePath(root);
    if (!QFileInfo::exists(output))
      return fail(QStringLiteral("received clipboard root is missing"));
    encodedUris.append(QString::fromUtf8(QUrl::fromLocalFile(output).toEncoded(QUrl::FullyEncoded)));
  }

  uriList = (encodedUris.join(QStringLiteral("\r\n")) + QStringLiteral("\r\n")).toUtf8().toStdString();
  gnomeCopiedFiles =
      ((cut ? QStringLiteral("cut\n") : QStringLiteral("copy\n")) + encodedUris.join(u'\n') + u'\n')
          .toUtf8()
          .toStdString();
  return true;
}

bool ClipboardFileTransfer::addFileBundle(IClipboard *clipboard, std::uint64_t maximumClipboardBytes)
{
  ClipboardSnapshot snapshot;
  if (!readSnapshot(clipboard, snapshot))
    return false;

  const auto uriIndex = static_cast<std::size_t>(IClipboard::Format::UriList);
  const auto gnomeIndex = static_cast<std::size_t>(IClipboard::Format::GnomeCopiedFiles);
  const auto bundleIndex = static_cast<std::size_t>(IClipboard::Format::FileBundle);
  if (snapshot.added[bundleIndex] || (!snapshot.added[uriIndex] && !snapshot.added[gnomeIndex]))
    return true;

  std::uint64_t existingBytes = 4;
  for (std::size_t index = 0; index < snapshot.added.size(); ++index) {
    if (snapshot.added[index])
      existingBytes += 8 + snapshot.data[index].size();
  }

  QString error;
  const auto maximumBundleBytes =
      maximumClipboardBytes > existingBytes + 8 ? maximumClipboardBytes - existingBytes - 8 : 0;
  auto bundle = buildBundle(snapshot.data[uriIndex], snapshot.data[gnomeIndex], maximumBundleBytes, &error);
  if (bundle.empty()) {
    LOG_WARN("file clipboard omitted: %s", error.toUtf8().constData());
    removeFileFormats(snapshot);
    writeSnapshot(clipboard, snapshot);
    return false;
  }

  snapshot.added[bundleIndex] = true;
  snapshot.data[bundleIndex] = std::move(bundle);
  return writeSnapshot(clipboard, snapshot);
}

bool ClipboardFileTransfer::prepareForLocalClipboard(
    Clipboard *destination, const IClipboard *source, const QString &destinationRoot
)
{
  if (!destination || !source || !IClipboard::copy(destination, source))
    return false;

  ClipboardSnapshot snapshot;
  if (!readSnapshot(destination, snapshot))
    return false;

  const auto uriIndex = static_cast<std::size_t>(IClipboard::Format::UriList);
  const auto gnomeIndex = static_cast<std::size_t>(IClipboard::Format::GnomeCopiedFiles);
  const auto bundleIndex = static_cast<std::size_t>(IClipboard::Format::FileBundle);
  const bool hasFileMetadata = snapshot.added[uriIndex] || snapshot.added[gnomeIndex];
  if (!snapshot.added[bundleIndex]) {
    if (hasFileMetadata) {
      LOG_WARN("received file clipboard has no content bundle; ignoring remote file paths");
      removeFileFormats(snapshot);
      return writeSnapshot(destination, snapshot);
    }
    return true;
  }

  std::string localUris;
  std::string localGnome;
  QString error;
  if (!extractBundle(snapshot.data[bundleIndex], localUris, localGnome, destinationRoot, &error)) {
    LOG_WARN("failed to materialize received file clipboard: %s", error.toUtf8().constData());
    removeFileFormats(snapshot);
    return writeSnapshot(destination, snapshot);
  }

  snapshot.added[uriIndex] = true;
  snapshot.data[uriIndex] = std::move(localUris);
  snapshot.added[gnomeIndex] = true;
  snapshot.data[gnomeIndex] = std::move(localGnome);
  return writeSnapshot(destination, snapshot);
}

} // namespace deskflow
