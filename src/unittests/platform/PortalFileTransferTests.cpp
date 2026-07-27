/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/PortalFileTransfer.h"

#include <fcntl.h>

#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

using namespace deskflow;

class FakePortalBackend : public PortalFileTransferBackend
{
public:
  QString startTransfer(QString *) override
  {
    ++startCalls;
    return key;
  }

  bool addFiles(const QString &receivedKey, const QList<int> &fileDescriptors, QString *) override
  {
    addedKeys.append(receivedKey);
    QList<QString> batch;
    for (const auto fd : fileDescriptors) {
      if (::fcntl(fd, F_GETFD) < 0)
        validDescriptors = false;
      batch.append(QFileInfo(QStringLiteral("/proc/self/fd/%1").arg(fd)).symLinkTarget());
    }
    batches.append(batch);
    return addResult;
  }

  QStringList retrieveFiles(const QString &receivedKey, QString *) override
  {
    retrievedKey = receivedKey;
    return retrievedPaths;
  }

  void stopTransfer(const QString &stoppedKey) override
  {
    stoppedKeys.append(stoppedKey);
  }

  QString key = QStringLiteral("portal-transfer-key");
  QStringList retrievedPaths;
  QString retrievedKey;
  QList<QString> addedKeys;
  QList<QList<QString>> batches;
  QStringList stoppedKeys;
  int startCalls = 0;
  bool addResult = true;
  bool validDescriptors = true;
};

class PortalFileTransferTests : public QObject
{
  Q_OBJECT

private:
  static void writeFile(const QString &path)
  {
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write("payload"), qint64(7));
  }

private Q_SLOTS:
  void detectsFlatpakMarker()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto marker = dir.filePath(QStringLiteral(".flatpak-info"));
    writeFile(marker);
    QVERIFY(PortalFileTransfer::isFlatpakSandbox(marker));
    QVERIFY(!PortalFileTransfer::isFlatpakSandbox(dir.filePath(QStringLiteral("missing"))));
  }

  void exportsFilesAsDescriptorsAndPublishesOnlyToken()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    QStringList paths;
    for (int index = 0; index < 17; ++index) {
      const auto path = dir.filePath(QStringLiteral("file-%1").arg(index));
      writeFile(path);
      paths.append(path);
    }

    auto backend = std::make_unique<FakePortalBackend>();
    auto *fake = backend.get();
    PortalFileTransfer transfer(std::move(backend));
    QString error;
    QVERIFY2(transfer.exportFiles(paths, &error), qPrintable(error));

    QCOMPARE(fake->startCalls, 1);
    QCOMPARE(fake->batches.size(), 2);
    QCOMPARE(fake->batches.at(0).size(), 16);
    QCOMPARE(fake->batches.at(1).size(), 1);
    QVERIFY(fake->validDescriptors);
    QCOMPARE(fake->addedKeys, QList<QString>({fake->key, fake->key}));
    QCOMPARE(transfer.mimeData(), fake->key.toUtf8());
    QVERIFY(!transfer.mimeData().contains(dir.path().toUtf8()));

    transfer.stop();
    QCOMPARE(fake->stoppedKeys, QStringList({fake->key}));
  }

  void retrievesPortalDocumentPaths()
  {
    auto backend = std::make_unique<FakePortalBackend>();
    auto *fake = backend.get();
    fake->retrievedPaths = {QStringLiteral("/run/user/1000/doc/abc/payload.txt")};
    PortalFileTransfer transfer(std::move(backend));

    QString error;
    const auto paths = transfer.retrieveFiles(QByteArray("incoming-key\0", 13), &error);
    QCOMPARE(fake->retrievedKey, QStringLiteral("incoming-key"));
    QCOMPARE(paths, fake->retrievedPaths);
  }
};

QTEST_MAIN(PortalFileTransferTests)

#include "PortalFileTransferTests.moc"
