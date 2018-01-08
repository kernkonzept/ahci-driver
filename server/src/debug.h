/*
 * Copyright (C) 2014 Kernkonzept GmbH.
 * Author(s): Sarah Hoffmann <sarah.hoffmann@kernkonzept.com>
 *
 * This file is distributed under the terms of the GNU General Public
 * License, version 2.  Please see the COPYING-GPL-2 file for details.
 */
#pragma once

#include <l4/re/util/debug>

class Err : public L4Re::Util::Err
{
public:
  explicit
  Err(Level l = Normal) : L4Re::Util::Err(l, "AHCI") {}
};

class Dbg : public L4Re::Util::Dbg
{
public:
  enum Level
  {
    Warn       = 1,
    Info       = 2,
    Trace      = 4,
  };

  explicit
  Dbg(unsigned long mask, char const *subs = 0)
  : L4Re::Util::Dbg(mask, subs, 0)
  {}
};

static Dbg info(Dbg::Info);
static Dbg warn(Dbg::Warn);
static Err error(Err::Normal);
static Err fatal(Err::Fatal);
