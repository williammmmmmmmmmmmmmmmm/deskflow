/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/PortalFileTransfer.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#include <QFile>
#include <QFileInfo>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>

namespace deskflow {
namespace {

constexpr auto kPortalBusName = "org.freedesktop.portal.Documents";
constexpr auto kPortalObjectPath = "/org/freedesktop/portal/documents";
constexpr auto kPortalInterface = "org.freedesktop.portal.FileTransfer";
constexpr int kMaximumFileDescriptorsPerCall = 16;
constexpr int kPortalTimeoutMs = 5000;

#ifndef O_PATH
#define O_PATH 0
#endif

void setError(QString *error, const QString &message)
{
  if (error)
    *error = message;
}

QString glibError(const GError *error)
{
  return error ? QString::fromUtf8(error->message) : QStringLiteral("unknown portal error");
}

class GDbusPortalFileTransferBackend : public PortalFileTransferBackend
{
public:
  GDbusPortalFileTransferBackend()
  {
    GError *error = nullptr;
    m_proxy = g_dbus_proxy_new_for_bus_sync(
        G_BUS_TYPE_SESSION,
        static_cast<GDBusProxyFlags>(
            G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES | G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS
        ),
        nullptr, kPortalBusName, kPortalObjectPath, kPortalInterface, nullptr, &error
    );
    if (error)
      g_error_free(error);
  }

  ~GDbusPortalFileTransferBackend() override
  {
    if (m_proxy)
      g_object_unref(m_proxy);
  }

  QString startTransfer(QString *error) override
  {
    if (!m_proxy) {
      setError(error, QStringLiteral("XDG FileTransfer Portal is unavailable"));
      return {};
    }

    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&options, "{sv}", "writable", g_variant_new_boolean(false));
    g_variant_builder_add(&options, "{sv}", "autostop", g_variant_new_boolean(false));

    GError *callError = nullptr;
    GVariant *reply = g_dbus_proxy_call_sync(
        m_proxy, "StartTransfer", g_variant_new("(a{sv})", &options), G_DBUS_CALL_FLAGS_NONE, kPortalTimeoutMs,
        nullptr, &callError
    );
    if (!reply) {
      setError(error, glibError(callError));
      if (callError)
        g_error_free(callError);
      return {};
    }

    const char *key = nullptr;
    g_variant_get(reply, "(&s)", &key);
    const auto result = QString::fromUtf8(key);
    g_variant_unref(reply);
    return result;
  }

  bool addFiles(const QString &key, const QList<int> &fileDescriptors, QString *error) override
  {
    if (!m_proxy) {
      setError(error, QStringLiteral("XDG FileTransfer Portal is unavailable"));
      return false;
    }

    GUnixFDList *fdList = g_unix_fd_list_new();
    GVariantBuilder handles;
    GVariantBuilder options;
    g_variant_builder_init(&handles, G_VARIANT_TYPE("ah"));
    g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);

    for (const auto fd : fileDescriptors) {
      GError *appendError = nullptr;
      const auto handle = g_unix_fd_list_append(fdList, fd, &appendError);
      if (handle < 0) {
        setError(error, glibError(appendError));
        if (appendError)
          g_error_free(appendError);
        g_object_unref(fdList);
        return false;
      }
      g_variant_builder_add(&handles, "h", handle);
    }

    GError *callError = nullptr;
    GVariant *reply = g_dbus_proxy_call_with_unix_fd_list_sync(
        m_proxy, "AddFiles", g_variant_new("(saha{sv})", key.toUtf8().constData(), &handles, &options),
        G_DBUS_CALL_FLAGS_NONE, kPortalTimeoutMs, fdList, nullptr, nullptr, &callError
    );
    g_object_unref(fdList);
    if (!reply) {
      setError(error, glibError(callError));
      if (callError)
        g_error_free(callError);
      return false;
    }

    g_variant_unref(reply);
    return true;
  }

  QStringList retrieveFiles(const QString &key, QString *error) override
  {
    if (!m_proxy) {
      setError(error, QStringLiteral("XDG FileTransfer Portal is unavailable"));
      return {};
    }

    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);

    GError *callError = nullptr;
    GVariant *reply = g_dbus_proxy_call_sync(
        m_proxy, "RetrieveFiles", g_variant_new("(sa{sv})", key.toUtf8().constData(), &options),
        G_DBUS_CALL_FLAGS_NONE, kPortalTimeoutMs, nullptr, &callError
    );
    if (!reply) {
      setError(error, glibError(callError));
      if (callError)
        g_error_free(callError);
      return {};
    }

    char **files = nullptr;
    g_variant_get(reply, "(^as)", &files);
    QStringList result;
    for (auto index = 0; files && files[index]; ++index)
      result.append(QString::fromUtf8(files[index]));
    g_strfreev(files);
    g_variant_unref(reply);
    return result;
  }

  void stopTransfer(const QString &key) override
  {
    if (!m_proxy || key.isEmpty())
      return;

    GError *error = nullptr;
    GVariant *reply = g_dbus_proxy_call_sync(
        m_proxy, "StopTransfer", g_variant_new("(s)", key.toUtf8().constData()), G_DBUS_CALL_FLAGS_NONE,
        kPortalTimeoutMs, nullptr, &error
    );
    if (reply)
      g_variant_unref(reply);
    if (error)
      g_error_free(error);
  }

private:
  GDBusProxy *m_proxy = nullptr;
};

} // namespace

PortalFileTransfer::PortalFileTransfer(std::unique_ptr<PortalFileTransferBackend> backend)
    : m_backend(backend ? std::move(backend) : std::make_unique<GDbusPortalFileTransferBackend>())
{
}

PortalFileTransfer::~PortalFileTransfer()
{
  stop();
}

bool PortalFileTransfer::exportFiles(const QStringList &paths, QString *error)
{
  std::scoped_lock lock{m_mutex};
  stopLocked();

  if (paths.isEmpty()) {
    setError(error, QStringLiteral("no local files are available for Portal export"));
    return false;
  }

  m_key = m_backend->startTransfer(error);
  if (m_key.isEmpty())
    return false;

  for (qsizetype offset = 0; offset < paths.size(); offset += kMaximumFileDescriptorsPerCall) {
    QList<int> fileDescriptors;
    const auto end = std::min(paths.size(), offset + kMaximumFileDescriptorsPerCall);
    for (auto index = offset; index < end; ++index) {
      const auto encodedPath = QFile::encodeName(paths.at(index));
      const auto fd = ::open(encodedPath.constData(), O_PATH | O_CLOEXEC);
      if (fd < 0) {
        setError(
            error,
            QStringLiteral("cannot open received file for Portal export: %1: %2")
                .arg(paths.at(index), QString::fromLocal8Bit(std::strerror(errno)))
        );
        for (const auto openedFd : fileDescriptors)
          ::close(openedFd);
        stopLocked();
        return false;
      }
      fileDescriptors.append(fd);
    }

    const auto added = m_backend->addFiles(m_key, fileDescriptors, error);
    for (const auto fd : fileDescriptors)
      ::close(fd);
    if (!added) {
      stopLocked();
      return false;
    }
  }

  return true;
}

QStringList PortalFileTransfer::retrieveFiles(const QByteArray &keyData, QString *error)
{
  auto normalized = keyData;
  while (normalized.endsWith('\0'))
    normalized.chop(1);
  if (normalized.isEmpty() || normalized.contains('\0')) {
    setError(error, QStringLiteral("invalid XDG FileTransfer Portal key"));
    return {};
  }

  const auto key = QString::fromUtf8(normalized);
  if (key.toUtf8() != normalized) {
    setError(error, QStringLiteral("XDG FileTransfer Portal key is not valid UTF-8"));
    return {};
  }

  std::scoped_lock lock{m_mutex};
  return m_backend->retrieveFiles(key, error);
}

QByteArray PortalFileTransfer::mimeData() const
{
  std::scoped_lock lock{m_mutex};
  return m_key.toUtf8();
}

void PortalFileTransfer::stop()
{
  std::scoped_lock lock{m_mutex};
  stopLocked();
}

bool PortalFileTransfer::isFlatpakSandbox(const QString &markerPath)
{
  return QFileInfo::exists(markerPath);
}

void PortalFileTransfer::stopLocked()
{
  if (m_key.isEmpty())
    return;
  m_backend->stopTransfer(m_key);
  m_key.clear();
}

} // namespace deskflow
