/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <memory>
#include <mutex>

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

namespace deskflow {

class PortalFileTransferBackend
{
public:
  virtual ~PortalFileTransferBackend() = default;

  virtual QString startTransfer(QString *error) = 0;
  virtual bool addFiles(const QString &key, const QList<int> &fileDescriptors, QString *error) = 0;
  virtual QStringList retrieveFiles(const QString &key, QString *error) = 0;
  virtual void stopTransfer(const QString &key) = 0;
};

//! XDG FileTransfer Portal owner used by the Wayland clipboard.
class PortalFileTransfer
{
public:
  static constexpr const char *kMimeType = "application/vnd.portal.filetransfer";

  explicit PortalFileTransfer(std::unique_ptr<PortalFileTransferBackend> backend = {});
  ~PortalFileTransfer();

  PortalFileTransfer(const PortalFileTransfer &) = delete;
  PortalFileTransfer &operator=(const PortalFileTransfer &) = delete;

  bool exportFiles(const QStringList &paths, QString *error = nullptr);
  QStringList retrieveFiles(const QByteArray &keyData, QString *error = nullptr);
  QByteArray mimeData() const;
  void stop(const char *reason = "explicit");

  static bool isFlatpakSandbox(const QString &markerPath = QStringLiteral("/.flatpak-info"));

private:
  void stopLocked(const char *reason);

  std::unique_ptr<PortalFileTransferBackend> m_backend;
  QString m_key;
  mutable std::mutex m_mutex;
};

} // namespace deskflow
