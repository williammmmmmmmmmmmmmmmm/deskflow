/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/XWindowsClipboardMimeConverter.h"

XWindowsClipboardMimeConverter::XWindowsClipboardMimeConverter(
    Display *display, const char *name, IClipboard::Format format
)
    : m_atom(XInternAtom(display, name, False)),
      m_format(format)
{
}

IClipboard::Format XWindowsClipboardMimeConverter::getFormat() const
{
  return m_format;
}

Atom XWindowsClipboardMimeConverter::getAtom() const
{
  return m_atom;
}

int XWindowsClipboardMimeConverter::getDataSize() const
{
  return 8;
}

std::string XWindowsClipboardMimeConverter::fromIClipboard(const std::string &data) const
{
  return data;
}

std::string XWindowsClipboardMimeConverter::toIClipboard(const std::string &data) const
{
  return data;
}
