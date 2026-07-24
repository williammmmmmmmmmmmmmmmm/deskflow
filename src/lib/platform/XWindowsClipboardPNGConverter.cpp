/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include <QBuffer>
#include <QByteArray>
#include <QDataStream>
#include <QImage>
#include <QtEndian>

#include "platform/XWindowsClipboardPNGConverter.h"

#include <cstring>

namespace {

constexpr qsizetype kBmpFileHeaderSize = 14;
constexpr quint32 kMinimumDibHeaderSize = 12;

QByteArray dibToBmp(const std::string &dib)
{
  if (dib.size() < sizeof(quint32))
    return {};

  quint32 headerSize = 0;
  std::memcpy(&headerSize, dib.data(), sizeof(headerSize));
  headerSize = qFromLittleEndian(headerSize);
  if (headerSize < kMinimumDibHeaderSize || headerSize > dib.size())
    return {};

  QByteArray bmp;
  QDataStream stream(&bmp, QIODevice::WriteOnly);
  stream.setByteOrder(QDataStream::LittleEndian);
  stream.writeRawData("BM", 2);
  stream << static_cast<quint32>(kBmpFileHeaderSize + dib.size());
  stream << quint32(0);
  stream << static_cast<quint32>(kBmpFileHeaderSize + headerSize);
  stream.writeRawData(dib.data(), static_cast<qsizetype>(dib.size()));
  return bmp;
}

std::string bmpToDib(const QByteArray &bmp)
{
  if (bmp.size() < kBmpFileHeaderSize + 40)
    return {};

  quint32 pixelOffset = 0;
  std::memcpy(&pixelOffset, bmp.constData() + 10, sizeof(pixelOffset));
  pixelOffset = qFromLittleEndian(pixelOffset);
  if (pixelOffset < kBmpFileHeaderSize + 40 || pixelOffset > bmp.size())
    return {};

  // The Deskflow bitmap format is the DIB header followed immediately by pixels.
  return std::string(bmp.constData() + kBmpFileHeaderSize, 40) +
         std::string(bmp.constData() + pixelOffset, bmp.size() - pixelOffset);
}

} // namespace

XWindowsClipboardPNGConverter::XWindowsClipboardPNGConverter(Display *display)
    : m_atom(XInternAtom(display, "image/png", False))
{
}

IClipboard::Format XWindowsClipboardPNGConverter::getFormat() const
{
  return IClipboard::Format::Bitmap;
}

Atom XWindowsClipboardPNGConverter::getAtom() const
{
  return m_atom;
}

int XWindowsClipboardPNGConverter::getDataSize() const
{
  return 8;
}

std::string XWindowsClipboardPNGConverter::fromIClipboard(const std::string &data) const
{
  const auto bmp = dibToBmp(data);
  QImage image;
  if (bmp.isEmpty() || !image.loadFromData(bmp, "BMP"))
    return {};

  QByteArray png;
  QBuffer buffer(&png);
  if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG"))
    return {};
  return png.toStdString();
}

std::string XWindowsClipboardPNGConverter::toIClipboard(const std::string &data) const
{
  QImage image;
  if (!image.loadFromData(QByteArray::fromStdString(data), "PNG"))
    return {};

  QByteArray bmp;
  QBuffer buffer(&bmp);
  if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "BMP"))
    return {};
  return bmpToDib(bmp);
}
