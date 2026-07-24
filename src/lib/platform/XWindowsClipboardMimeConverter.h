/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "platform/XWindowsClipboard.h"

//! Lossless converter for clipboard MIME formats whose wire bytes are UTF-8.
class XWindowsClipboardMimeConverter : public IXWindowsClipboardConverter
{
public:
  XWindowsClipboardMimeConverter(Display *display, const char *name, IClipboard::Format format);

  IClipboard::Format getFormat() const override;
  Atom getAtom() const override;
  int getDataSize() const override;
  std::string fromIClipboard(const std::string &data) const override;
  std::string toIClipboard(const std::string &data) const override;

private:
  Atom m_atom;
  IClipboard::Format m_format;
};
