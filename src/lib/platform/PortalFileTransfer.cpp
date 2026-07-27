/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/PortalFileTransfer.h"

#include "base/Log.h"

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
        G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
        nullptr, kPortalBusName, kPortalObjectPath, kPortalInterface, nullptr, &error
    );
    if (error) {
      LOG_DEBUG(
          "[clipboard-file-transfer] direction=target event=proxy-created success=false bus=%s object=%s interface=%s "
          "error=\"%s\"",
          kPortalBusName, kPortalObjectPath, kPortalInterface, error->message
      );
      g_error_free(error);
    } else {
      LOG_DEBUG(
          "[clipboard-file-transfer] direction=target event=proxy-created success=true bus=%s object=%s interface=%s",
          kPortalBusName, kPortalObjectPath, kPortalInterface
      );
      g_signal_connect(
          m_proxy, "g-signal",
          G_CALLBACK(+[](GDBusProxy *, const gchar *, const gchar *signalName, GVariant *parameters, gpointer) {
            if (g_strcmp0(signalName, "TransferClosed") != 0)
              return;
            g_autofree gchar *values = g_variant_print(parameters, true);
            LOG_DEBUG(
                "[clipboard-file-transfer] direction=target event=transfer-closed signal=%s parameters=%s",
                signalName, values
            );
          }),
          nullptr
      );
    }
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
      LOG_DEBUG(
          "[clipboard-file-transfer] direction=target event=start-transfer success=false writable=false "
          "autostop=false error=\"%s\"",
          callError ? callError->message : "unknown portal error"
      );
      if (callError)
        g_error_free(callError);
      return {};
    }

    const char *key = nullptr;
    g_variant_get(reply, "(&s)", &key);
    const auto result = QString::fromUtf8(key);
    LOG_DEBUG(
        "[clipboard-file-transfer] direction=target event=start-transfer success=true writable=false "
        "autostop=false key=\"%s\" key_bytes=%lld",
        result.toUtf8().constData(), static_cast<long long>(result.toUtf8().size())
    );
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
      const auto fdValid = ::fcntl(fd, F_GETFD) >= 0;
      LOG_DEBUG(
          "[clipboard-file-transfer] direction=target event=add-files-fd key=\"%s\" fd=%d valid=%s",
          key.toUtf8().constData(), fd, fdValid ? "true" : "false"
      );
      GError *appendError = nullptr;
      const auto handle = g_unix_fd_list_append(fdList, fd, &appendError);
      if (handle < 0) {
        setError(error, glibError(appendError));
        LOG_DEBUG(
            "[clipboard-file-transfer] direction=target event=add-files success=false key=\"%s\" files=%lld "
            "error=\"%s\"",
            key.toUtf8().constData(), static_cast<long long>(fileDescriptors.size()),
            appendError ? appendError->message : "unknown fd-list error"
        );
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
      LOG_DEBUG(
          "[clipboard-file-transfer] direction=target event=add-files success=false key=\"%s\" files=%lld "
          "error=\"%s\"",
          key.toUtf8().constData(), static_cast<long long>(fileDescriptors.size()),
          callError ? callError->message : "unknown portal error"
      );
      if (callError)
        g_error_free(callError);
      return false;
    }

    g_variant_unref(reply);
    LOG_DEBUG(
        "[clipboard-file-transfer] direction=target event=add-files success=true key=\"%s\" files=%lld",
        key.toUtf8().constData(), static_cast<long long>(fileDescriptors.size())
    );
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
      LOG_DEBUG(
          "[clipboard-file-transfer] direction=source event=retrieve-files success=false key=\"%s\" error=\"%s\"",
          key.toUtf8().constData(), callError ? callError->message : "unknown portal error"
      );
      if (callError)
        g_error_free(callError);
      return {};
    }

    char **files = nullptr;
    g_variant_get(reply, "(^as)", &files);
    QStringList result;
    for (auto index = 0; files && files[index]; ++index)
      result.append(QString::fromUtf8(files[index]));
    LOG_DEBUG(
        "[clipboard-file-transfer] direction=source event=retrieve-files success=true key=\"%s\" files=%lld "
        "paths=\"%s\"",
        key.toUtf8().constData(), static_cast<long long>(result.size()), result.join(u'|').toUtf8().constData()
    );
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
    LOG_DEBUG(
        "[clipboard-file-transfer] direction=target event=stop-transfer-call success=%s key=\"%s\" error=\"%s\"",
        error ? "false" : "true", key.toUtf8().constData(), error ? error->message : ""
    );
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
  LOG_DEBUG("[clipboard-file-transfer] event=session-object-created object=%p", static_cast<void *>(this));
}

PortalFileTransfer::~PortalFileTransfer()
{
  LOG_DEBUG(
      "[clipboard-file-transfer] event=session-object-destroying object=%p active_key=%s",
      static_cast<void *>(this), m_key.isEmpty() ? "false" : "true"
  );
  stop("destructor");
}

bool PortalFileTransfer::exportFiles(const QStringList &paths, QString *error)
{
  std::scoped_lock lock{m_mutex};
  stopLocked("replace-before-export");

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
        LOG_DEBUG(
            "[clipboard-file-transfer] direction=target event=open-export-file success=false path=\"%s\" error=\"%s\"",
            paths.at(index).toUtf8().constData(), std::strerror(errno)
        );
        stopLocked("open-export-file-failed");
        return false;
      }
      LOG_DEBUG(
          "[clipboard-file-transfer] direction=target event=open-export-file success=true path=\"%s\" fd=%d "
          "fd_valid=%s",
          paths.at(index).toUtf8().constData(), fd, ::fcntl(fd, F_GETFD) >= 0 ? "true" : "false"
      );
      fileDescriptors.append(fd);
    }

    const auto added = m_backend->addFiles(m_key, fileDescriptors, error);
    for (const auto fd : fileDescriptors)
      ::close(fd);
    if (!added) {
      stopLocked("add-files-failed");
      return false;
    }
  }

  LOG_DEBUG(
      "[clipboard-file-transfer] direction=target event=export-files success=true key=\"%s\" files=%lld "
      "paths=\"%s\"",
      m_key.toUtf8().constData(), static_cast<long long>(paths.size()), paths.join(u'|').toUtf8().constData()
  );
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

  LOG_DEBUG(
      "[clipboard-file-transfer] direction=source event=portal-token-read raw_bytes=%lld trailing_nul=%s key=\"%s\" "
      "key_bytes=%lld",
      static_cast<long long>(keyData.size()), keyData.endsWith('\0') ? "true" : "false",
      key.toUtf8().constData(), static_cast<long long>(normalized.size())
  );
  std::scoped_lock lock{m_mutex};
  return m_backend->retrieveFiles(key, error);
}

QByteArray PortalFileTransfer::mimeData() const
{
  std::scoped_lock lock{m_mutex};
  auto data = m_key.toUtf8();
  if (!data.isEmpty())
    data.append('\0');
  return data;
}

void PortalFileTransfer::stop(const char *reason)
{
  std::scoped_lock lock{m_mutex};
  stopLocked(reason);
}

bool PortalFileTransfer::isFlatpakSandbox(const QString &markerPath)
{
  return QFileInfo::exists(markerPath);
}

void PortalFileTransfer::stopLocked(const char *reason)
{
  if (m_key.isEmpty())
    return;
  LOG_DEBUG(
      "[clipboard-file-transfer] direction=target event=stop-transfer-trigger key=\"%s\" reason=%s object=%p",
      m_key.toUtf8().constData(), reason, static_cast<void *>(this)
  );
  m_backend->stopTransfer(m_key);
  m_key.clear();
}

} // namespace deskflow
