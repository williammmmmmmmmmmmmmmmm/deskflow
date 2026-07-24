/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "platform/XWindowsClipboard.h"

//! Convert between the internal DIB clipboard format and image/png.
class XWindowsClipboardPNGConverter : public IXWindowsClipboardConverter
{
public:
  explicit XWindowsClipboardPNGConverter(Display *display);

  IClipboard::Format getFormat() const override;
  Atom getAtom() const override;
  int getDataSize() const override;
  std::string fromIClipboard(const std::string &data) const override;
  std::string toIClipboard(const std::string &data) const override;

private:
  Atom m_atom;
};
