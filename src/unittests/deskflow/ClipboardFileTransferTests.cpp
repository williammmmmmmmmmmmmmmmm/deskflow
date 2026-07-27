/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "base/Log.h"
#include "deskflow/Clipboard.h"
#include "deskflow/ClipboardFileTransfer.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

class ClipboardFileTransferTests : public QObject
{
  Q_OBJECT

private:
  Log m_log;

  static void writeFile(const QString &path, const QByteArray &contents)
  {
    QFile file(path);
    QVERIFY2(file.open(QIODevice::WriteOnly), qPrintable(file.errorString()));
    QCOMPARE(file.write(contents), contents.size());
  }

  static std::string uriList(const QStringList &paths)
  {
    QStringList uris;
    for (const auto &path : paths)
      uris.append(QString::fromUtf8(QUrl::fromLocalFile(path).toEncoded(QUrl::FullyEncoded)));
    return (uris.join(QStringLiteral("\r\n")) + QStringLiteral("\r\n")).toUtf8().toStdString();
  }

  static QStringList pathsFromUriList(const std::string &uriList)
  {
    QString text = QString::fromUtf8(uriList.data(), static_cast<qsizetype>(uriList.size()));
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    QStringList paths;
    for (const auto &line : text.split(u'\n', Qt::SkipEmptyParts))
      paths.append(QUrl::fromEncoded(line.toUtf8()).toLocalFile());
    return paths;
  }

  static QByteArray readFile(const QString &path)
  {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
      return {};
    return file.readAll();
  }

private Q_SLOTS:
  void transfersMultipleFilesAndDirectory()
  {
    QTemporaryDir source;
    QTemporaryDir received;
    QVERIFY(source.isValid());
    QVERIFY(received.isValid());

    const auto first = source.filePath(QStringLiteral("first file.txt"));
    const auto second = source.filePath(QStringLiteral("second.bin"));
    const auto folder = source.filePath(QStringLiteral("folder"));
    QVERIFY(QDir().mkpath(folder));
    writeFile(first, QByteArrayLiteral("deskflow text"));
    writeFile(second, QByteArray::fromHex("000102ff"));
    writeFile(QDir(folder).filePath(QStringLiteral("nested.txt")), QByteArrayLiteral("nested"));

    QString error;
    const auto bundle = deskflow::ClipboardFileTransfer::buildBundle(
        uriList({first, second, folder}), {}, 1024 * 1024, &error
    );
    QVERIFY2(!bundle.empty(), qPrintable(error));

    std::string localUris;
    std::string localGnome;
    QVERIFY2(
        deskflow::ClipboardFileTransfer::extractBundle(
            bundle, localUris, localGnome, received.path(), &error
        ),
        qPrintable(error)
    );

    const auto paths = pathsFromUriList(localUris);
    QCOMPARE(paths.size(), 3);
    QCOMPARE(readFile(paths.at(0)), QByteArrayLiteral("deskflow text"));
    QCOMPARE(readFile(paths.at(1)), QByteArray::fromHex("000102ff"));
    QCOMPARE(
        readFile(QDir(paths.at(2)).filePath(QStringLiteral("nested.txt"))),
        QByteArrayLiteral("nested")
    );
    QVERIFY(QString::fromStdString(localGnome).startsWith(QStringLiteral("copy\nfile://")));
  }

  void preservesOtherClipboardFormatsAndRewritesUris()
  {
    QTemporaryDir sourceDir;
    QTemporaryDir received;
    QVERIFY(sourceDir.isValid());
    QVERIFY(received.isValid());

    const auto sourcePath = sourceDir.filePath(QStringLiteral("payload.txt"));
    writeFile(sourcePath, QByteArrayLiteral("payload"));
    const auto sourceUris = uriList({sourcePath});

    QString error;
    const auto bundle =
        deskflow::ClipboardFileTransfer::buildBundle(sourceUris, {}, 1024 * 1024, &error);
    QVERIFY2(!bundle.empty(), qPrintable(error));

    Clipboard incoming;
    QVERIFY(incoming.open(42));
    QVERIFY(incoming.empty());
    incoming.add(IClipboard::Format::Text, "ordinary text");
    incoming.add(IClipboard::Format::UriList, sourceUris);
    incoming.add(IClipboard::Format::FileBundle, bundle);
    incoming.close();

    Clipboard local;
    QVERIFY(deskflow::ClipboardFileTransfer::prepareForLocalClipboard(
        &local, &incoming, received.path()
    ));
    QVERIFY(local.open(0));
    QCOMPARE(local.get(IClipboard::Format::Text), "ordinary text");
    QVERIFY(local.has(IClipboard::Format::UriList));
    QVERIFY(local.has(IClipboard::Format::GnomeCopiedFiles));
    const auto localPaths = pathsFromUriList(local.get(IClipboard::Format::UriList));
    QCOMPARE(localPaths.size(), 1);
    QVERIFY(localPaths.front() != sourcePath);
    QCOMPARE(readFile(localPaths.front()), QByteArrayLiteral("payload"));
    local.close();
  }

  void omitsRemoteUrisWithoutContentBundle()
  {
    Clipboard incoming;
    QVERIFY(incoming.open(1));
    QVERIFY(incoming.empty());
    incoming.add(IClipboard::Format::Text, "text survives");
    incoming.add(IClipboard::Format::UriList, "file:///remote/missing.txt\r\n");
    incoming.close();

    Clipboard local;
    QVERIFY(deskflow::ClipboardFileTransfer::prepareForLocalClipboard(&local, &incoming));
    QVERIFY(local.open(0));
    QCOMPARE(local.get(IClipboard::Format::Text), "text survives");
    QVERIFY(!local.has(IClipboard::Format::UriList));
    QVERIFY(!local.has(IClipboard::Format::GnomeCopiedFiles));
    local.close();
  }

  void materializesFilesWithoutExposingSandboxUris()
  {
    QTemporaryDir sourceDir;
    QTemporaryDir sandboxCache;
    QVERIFY(sourceDir.isValid());
    QVERIFY(sandboxCache.isValid());

    const auto sourcePath = sourceDir.filePath(QStringLiteral("portal payload.txt"));
    writeFile(sourcePath, QByteArrayLiteral("portal payload"));
    const auto sourceUris = uriList({sourcePath});

    QString error;
    const auto bundle =
        deskflow::ClipboardFileTransfer::buildBundle(sourceUris, {}, 1024 * 1024, &error);
    QVERIFY2(!bundle.empty(), qPrintable(error));

    Clipboard incoming;
    QVERIFY(incoming.open(1));
    QVERIFY(incoming.empty());
    incoming.add(IClipboard::Format::Text, "text survives");
    incoming.add(IClipboard::Format::UriList, sourceUris);
    incoming.add(IClipboard::Format::FileBundle, bundle);
    incoming.close();

    Clipboard local;
    QStringList materializedPaths;
    QVERIFY(deskflow::ClipboardFileTransfer::prepareForLocalClipboard(
        &local, &incoming, sandboxCache.path(), &materializedPaths, false
    ));
    QCOMPARE(materializedPaths.size(), 1);
    QVERIFY(materializedPaths.front().startsWith(sandboxCache.path()));
    QCOMPARE(readFile(materializedPaths.front()), QByteArrayLiteral("portal payload"));

    QVERIFY(local.open(0));
    QCOMPARE(local.get(IClipboard::Format::Text), "text survives");
    QVERIFY(!local.has(IClipboard::Format::UriList));
    QVERIFY(!local.has(IClipboard::Format::GnomeCopiedFiles));
    local.close();
  }

  void rejectsTraversalAndSizeLimit()
  {
    QTemporaryDir source;
    QTemporaryDir received;
    QVERIFY(source.isValid());
    QVERIFY(received.isValid());

    const auto path = source.filePath(QStringLiteral("aa"));
    writeFile(path, QByteArrayLiteral("contents"));

    QString error;
    auto bundle =
        deskflow::ClipboardFileTransfer::buildBundle(uriList({path}), {}, 1024, &error);
    QVERIFY2(!bundle.empty(), qPrintable(error));

    // Root string begins after magic, version, action, root count, and its length.
    QCOMPARE(bundle.substr(17, 2), std::string("aa"));
    bundle.replace(17, 2, "..");
    std::string localUris;
    std::string localGnome;
    QVERIFY(!deskflow::ClipboardFileTransfer::extractBundle(
        bundle, localUris, localGnome, received.path(), &error
    ));
    QVERIFY(QDir(received.path()).entryList(QDir::NoDotAndDotDot | QDir::AllEntries).isEmpty());

    const auto tooSmall =
        deskflow::ClipboardFileTransfer::buildBundle(uriList({path}), {}, 20, &error);
    QVERIFY(tooSmall.empty());
  }

  void marshallsNewFormatsAfterLegacyFormats()
  {
    Clipboard source;
    QVERIFY(source.open(7));
    QVERIFY(source.empty());
    source.add(IClipboard::Format::Text, "legacy");
    source.add(IClipboard::Format::UriList, "file:///tmp/example\r\n");
    source.add(IClipboard::Format::GnomeCopiedFiles, "copy\nfile:///tmp/example\n");
    source.add(IClipboard::Format::FileBundle, "bundle");
    source.close();

    const auto wire = source.marshall();
    Clipboard restored;
    restored.unmarshall(wire, 8);
    QVERIFY(restored.open(0));
    QCOMPARE(restored.get(IClipboard::Format::Text), "legacy");
    QCOMPARE(restored.get(IClipboard::Format::UriList), "file:///tmp/example\r\n");
    QCOMPARE(
        restored.get(IClipboard::Format::GnomeCopiedFiles),
        "copy\nfile:///tmp/example\n"
    );
    QCOMPARE(restored.get(IClipboard::Format::FileBundle), "bundle");
    restored.close();
  }
};

QTEST_MAIN(ClipboardFileTransferTests)

#include "ClipboardFileTransferTests.moc"
