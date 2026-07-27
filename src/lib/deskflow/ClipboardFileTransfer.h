/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <QString>
#include <QStringList>

class Clipboard;
class IClipboard;

namespace deskflow {

//! File content transport carried by the normal clipboard protocol.
class ClipboardFileTransfer
{
public:
  //! Add a content bundle for local file URIs already present in clipboard.
  static bool addFileBundle(IClipboard *clipboard, std::uint64_t maximumClipboardBytes);

  //! Copy a received clipboard and replace remote file URIs with local ones.
  static bool prepareForLocalClipboard(
      Clipboard *destination, const IClipboard *source, const QString &destinationRoot = QString(),
      QStringList *materializedPaths = nullptr, bool exposeFileUris = true
  );

  //! Build and extract helpers are public to allow format-level unit testing.
  static std::string buildBundle(
      std::string_view uriList, std::string_view gnomeCopiedFiles, std::uint64_t maximumBundleBytes,
      QString *error = nullptr
  );
  static bool extractBundle(
      std::string_view bundle, std::string &uriList, std::string &gnomeCopiedFiles,
      const QString &destinationRoot = QString(), QString *error = nullptr
  );

private:
  ClipboardFileTransfer() = delete;
};

} // namespace deskflow
