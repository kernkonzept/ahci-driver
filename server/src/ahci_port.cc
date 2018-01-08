/*
 * Copyright (C) 2014 Kernkonzept GmbH.
 * Author(s): Sarah Hoffmann <sarah.hoffmann@kernkonzept.com>
 *
 * This file is distributed under the terms of the GNU General Public
 * License, version 2.  Please see the COPYING-GPL-2 file for details.
 */

#include <l4/re/env>
#include <l4/re/error_helper>

#include <l4/vbus/vbus>
#include <l4/vbus/vbus_pci.h>
#include <cstring>

#include "ahci_port.h"
#include "debug.h"
#include "errand.h"

static Dbg trace(Dbg::Trace, "ahci-port");

namespace Ahci {

//--------------------------------------------
//  Command slot
//--------------------------------------------

int
Command_slot::setup_command(Fis::Taskfile const &task, Fis::Callback const &cb,
                            l4_uint8_t port)
{
  // fill command table
  l4_uint8_t *fis = _cmd_table->cfis;
  fis[0] = 0x27;                    // FIS type Host-to-Device
  fis[1] = (1 << 7) | (port & 0xF); // upper bit defines command FIS
  fis[2] = task.command;
  fis[3] = task.features & 0xFF;
  fis[4] = task.lba & 0xFF;
  fis[5] = (task.lba >> 8) & 0xFF;
  fis[6] = (task.lba >> 16) & 0xFF;
  fis[7] = task.device;
  fis[8] = (task.lba >> 24) & 0xFF;
  fis[9] = (task.lba >> 32) & 0xFF;
  fis[10] = (task.lba >> 40) & 0xFF;
  fis[11] = (task.features >> 8) & 0xFF;
  fis[12] = task.count;
  fis[13] = (task.count >> 8) & 0xFF;
  fis[14] = task.icc;
  fis[15] = task.control;

  // now add the slot information
  _cmd_header->flags = 0;
  _cmd_header->prdtl() = 0;
  _cmd_header->p() = (task.flags & Fis::Chf_prefetchable);
  _cmd_header->w() = (task.flags & Fis::Chf_write);
  _cmd_header->a() = (task.flags & Fis::Chf_atapi);
  _cmd_header->c() = true;
  _cmd_header->cfl() = 5;
  _cmd_header->prdbc = 0;
  _cmd_header->ctba0 = _cmd_table_pa;
  if (sizeof(_cmd_table_pa) == 8)
    _cmd_header->ctba0_u0 = (l4_uint64_t) _cmd_table_pa >> 32;

  // save client info
  _callback = cb;

  return L4_EOK;
}

int
Command_slot::setup_data(Fis::Datablock const *data, int len)
{
#if (__BYTE_ORDER == __BIG_ENDIAN)
#error "Big endlian not implemented."
#endif

  unsigned numblocks = len;
  if (numblocks > Command_table::Max_entries)
    numblocks = Command_table::Max_entries;

  for (unsigned i = 0; i < numblocks; ++i)
    {
      _cmd_table->prd[i].dba = data[i].addr;
      if (sizeof(l4_addr_t) == 8)
        _cmd_table->prd[i].dbau = (l4_uint64_t) data[i].addr >> 32;
      else
        _cmd_table->prd[i].dbau = 0;
      _cmd_table->prd[i].dbc = data[i].size - 1;
      // TODO: cache: make sure client data is flushed
    }

  _cmd_header->prdtl() = numblocks;

  return len;
}

//--------------------------------------------
//  Ahci_port
//--------------------------------------------


int
Ahci_port::attach(l4_addr_t base_addr, unsigned buswidth,
                  L4Re::Util::Shared_cap<L4Re::Dma_space> const &dma_space)
{
  if (_state != S_undefined)
    return -L4_EEXIST;

  trace.printf("Attaching port to address 0x%lx\n", base_addr);

  _regs = new Hw::Mmio_register_block_le<32>(base_addr);
  _buswidth = buswidth;

  _state = S_present;

  // detect device type (borrowed from linux)
  if (device_state() == 3)
    {
      l4_uint32_t tmp = _regs[Regs::Port::Sig];
      unsigned lbah = (tmp >> 24) & 0xff;
      unsigned lbam = (tmp >> 16) & 0xff;

      if ((lbam == 0) && (lbah == 0))
        _devtype = Ahcidev_ata;
      else if ((lbam == 0x14) && (lbah == 0xeb))
        _devtype = Ahcidev_atapi;
      else if ((lbam == 0x69) && (lbah == 0x96))
        _devtype = Ahcidev_pmp;
      else if ((lbam == 0x3c) && (lbah == 0xc3))
        _devtype = Ahcidev_semb;
      else
        _devtype = Ahcidev_unknown;
    }
  else
    {
      _devtype = Ahcidev_none;
      return -L4_ENODEV;
    }

  _dma_space = dma_space;

  return L4_EOK;
}


void
Ahci_port::initialize_memory(unsigned maxslots)
{
  if (_state != S_attached)
    L4Re::chksys(-L4_EIO, "Device encountered fatal error.");

  if (_devtype == Ahcidev_none)
    L4Re::chksys(-L4_ENODEV, "Device no longer available.");

  // disable all interrupts for now
  _regs[Regs::Port::Ie] = 0;

  // get physical memory
  unsigned memsz = sizeof(Command_data) + maxslots * sizeof(Command_table);
  _cmd_data = Phys_region(memsz, _dma_space.get(),
                          L4Re::Dma_space::Direction::Bidirectional);
  Command_data *cd = _cmd_data.get<Command_data>();

  info.printf("Initializing port @%p.\n", _cmd_data.get<void>());

  // setup command list
  l4_addr_t addr = _cmd_data.phys() + offsetof(Command_data, headers);
  _regs[Regs::Port::Clb] = addr;
  _regs[Regs::Port::Clbu] = (sizeof(l4_addr_t) == 8)
                            ? ((l4_uint64_t) addr >> 32) : 0;

  // setup FIS receive region
  addr = _cmd_data.phys() + offsetof(Command_data, fis);
  _regs[Regs::Port::Fb] = addr;
  _regs[Regs::Port::Fbu] = (sizeof(l4_addr_t) == 8)
                           ? ((l4_uint64_t) addr >> 32) : 0;

  // enable FIS buffer
  _regs[Regs::Port::Cmd].set(Regs::Port::Cmd_fre);

  // reset error register
  _regs[Regs::Port::Serr] = 0xFFFFFFFF;

  // Initialize command slots
  _slots.clear();
  _slots.reserve(maxslots);
  // to be available CI and SACT must be cleared
  l4_uint32_t state = _regs[Regs::Port::Ci] | _regs[Regs::Port::Sact];

  // physical address, used for pointer arithmetics
  l4_addr_t phys_ct = _cmd_data.phys() + offsetof(Command_data, tables);
  for (unsigned i = 0; i < maxslots; ++i)
    {
      _slots.emplace_back(&cd->headers[i], &cd->tables[i],
                          phys_ct + i * sizeof(Command_table));

      if (!(state & (1 << i)))
        _slots[i].release();
    }

  _state = S_disabled;

  trace.printf("== Initialisation finished.\n");
  dump_registers(trace);
}



void
Ahci_port::enable(Errand::Callback const &callback)
{
  if (_state != S_disabled)
    {
      // TODO should if be fatal if this is called in unexpected states?
      callback();
      return;
    }

  _state = S_enabling;

  if (L4_UNLIKELY(!is_port_idle()))
    {
      _regs[Regs::Port::Cmd].set(Regs::Port::Cmd_clo);
      Errand::poll(10, 50000,
                   std::bind(&Ahci_port::no_command_list_override, this),
                   [=](bool ret)
                     {
                       if (_state != S_enabling)
                         {
                           warn.printf("Unexpected state in Ahci_port::enable");
                           callback();
                         }
                       else if (ret)
                         dma_enable(callback);
                       else
                         {
                           _state = S_fatal;
                           callback();
                         }
                     });
    }
  else
    dma_enable(callback);
}


void
Ahci_port::dma_enable(Errand::Callback const &callback)
{
  _regs[Regs::Port::Cmd].set(Regs::Port::Cmd_st);

  Errand::poll(10, 50000,
               std::bind(&Ahci_port::is_enabled, this),
               [=](bool ret)
                 {
                   if (_state != S_enabling)
                     {
                       warn.printf("Unexpected state in Ahci_port::enable");
                       callback();
                     }
                   else if (ret)
                     {
                       enable_ints();
                       _state = S_ready;
                        callback();
                     }
                   else
                     {
                       // disable again
                       _state = S_error;
                       disable(callback);
                     }
                 });
}


void
Ahci_port::disable(Errand::Callback const &callback)
{
  if (_state == S_disabled || _state == S_error)
    {
      _state = S_fatal;
      error.printf("Port disable called in unexpected state.\n");
    }

  if (is_command_list_disabled())
    {
      _state = S_disabled;
      callback(); // already disabled
      return;
    }

  // disable interrupts
  _regs[Regs::Port::Ie] = 0;
  // disable DMA engine
  _regs[Regs::Port::Cmd].clear(Regs::Port::Cmd_st);

  if (is_command_list_disabled())
    {
      _state = S_disabled;
      callback();
      return;
    }

  _state = S_disabling;

  Errand::poll(10, 50000,
               std::bind(&Ahci_port::is_command_list_disabled, this),
               [=](bool ret)
                 {
                   if (_state != S_disabling)
                     warn.printf("Unexpected state in Ahci_port::disable");
                     else if (ret)
                     _state = S_disabled;
                   else
                     {
                       _state = S_fatal;
                       error.printf("Could not disable port.");
                     }
                   callback();
                 });
}


void
Ahci_port::abort(Errand::Callback const &callback)
{
  // disable the port and then cancel any outstanding requests
  disable(
    [=]()
      {
        trace.printf("START ERRAND Abort_slots_errand\n");
        for (auto &s : _slots)
          s.abort();

        callback();
      });
}

void
Ahci_port::dump_registers(L4Re::Util::Dbg const &log) const
{
  log.printf(" CLB: 0x%08x - 0x%08x\n",
             _regs[Regs::Port::Clbu].read(), _regs[Regs::Port::Clb].read());
  log.printf("  FB: 0x%08x - 0x%08x\n",
             _regs[Regs::Port::Fbu].read(), _regs[Regs::Port::Fb].read());
  log.printf("  IS: 0x%08x    IE: 0x%08x\n",
             _regs[Regs::Port::Is].read(), _regs[Regs::Port::Ie].read());
  log.printf(" CMD: 0x%08x   TFD: 0x%08x\n",
             _regs[Regs::Port::Cmd].read(), _regs[Regs::Port::Tfd].read());
  log.printf(" SIG: 0x%08x    VS: 0x%08x\n",
             _regs[Regs::Port::Sig].read(), _regs[Regs::Port::Vs].read());
  log.printf("SSTS: 0x%08x  SCTL: 0x%08x\n",
             _regs[Regs::Port::Ssts].read(), _regs[Regs::Port::Sctl].read());
  log.printf("SERR: 0x%08x  SACT: 0x%08x\n",
             _regs[Regs::Port::Serr].read(), _regs[Regs::Port::Sact].read());
  log.printf("  CI: 0x%08x  SNTF: 0x%08x\n",
             _regs[Regs::Port::Ci].read(), _regs[Regs::Port::Sntf].read());
  log.printf(" FBS: 0x%08x  SLEP: 0x%08x\n",
             _regs[Regs::Port::Fbs].read(), _regs[Regs::Port::Devslp].read());
}


void
Ahci_port::initialize(Errand::Callback const &callback)
{
  if (_state == S_present)
    _state = S_present_init;
  else if (_state == S_error)
    _state = S_error_init;
  else
    {
      fatal.printf("'Initialize' called out of order.\n");
      _state = S_fatal;
      return;
    }

  trace.printf("Port: starting reset\n");
  if (is_command_list_disabled())
    {
      disable_fis_receive(callback);
      return;
    }

  _regs[Regs::Port::Cmd].clear(Regs::Port::Cmd_st);

  Errand::poll(10, 50000,
               std::bind(&Ahci_port::is_command_list_disabled, this),
               [=](bool ret)
                 {
                   if (!(_state == S_present_init || _state == S_error_init))
                     {
                       // TODO Should this unexpected state change be fatal?
                       warn.printf("Unexpected state in Ahci_port::initialize\n");
                       callback();
                     }
                   else if (ret)
                     {
                       disable_fis_receive(callback);
                     }
                   else
                     {
                       error.printf("Init: ST disable failed.\n");
                       dump_registers(trace);
                       _state = S_fatal;
                       callback();
                     }
                 });
}

void
Ahci_port::disable_fis_receive(Errand::Callback const &callback)
{
  if (is_fis_receive_disabled())
    {
      _state = (_state == S_present_init) ? S_attached : S_disabled;
      callback();
      return;
    }

  _regs[Regs::Port::Cmd].clear(Regs::Port::Cmd_fre);

  Errand::poll(10, 50000,
               std::bind(&Ahci_port::is_fis_receive_disabled, this),
               [=](bool ret)
                 {
                   if (!(_state == S_present_init || _state == S_error_init))
                     {
                       // TODO Should this unexpected state change be fatal?
                       warn.printf("Unexpected state in Ahci_port::initialize\n");
                     }
                   else if (ret)
                     _state = (_state == S_present_init) ?
                              S_attached : S_disabled;
                   else
                     {
                       error.printf(" Reset: fis receive reset failed.\n");
                       _state = S_fatal;
                     }
                   callback();
                 }
              );
}


void
Ahci_port::reset(Errand::Callback const &callback)
{
  info.printf("Doing full port reset.\n");

  _regs[Regs::Port::Sctl] = 1;

  // wait for 5ms, according to spec
  Errand::schedule([=]()
    {
      _regs[Regs::Port::Sctl] = 0;

      Errand::poll(10, 50000,
                   std::bind(&Ahci_port::device_present, this),
                   [=](bool ret)
                     {
                       if (ret)
                         wait_tfd(callback);
                       else
                         callback();
                     });
    }, 5);

}

void
Ahci_port::wait_tfd(Errand::Callback const &callback)
{
  Errand::poll(10, 50000,
               std::bind(&Ahci_port::is_port_idle, this),
               [=](bool ret)
                 {
                   if (ret)
                     {
                       _regs[Regs::Port::Serr] = 0xFFFFFFFF;
                       _regs[Regs::Port::Is] = 0xFFFFFFFF;
                     }
                   callback();
                 });

}


int
Ahci_port::send_command(Fis::Taskfile const &task, Fis::Callback const &cb,
                        l4_uint8_t port)
{
  if (L4_UNLIKELY(!device_ready()))
    return -L4_ENODEV;

  /// XXX get rid of fixed task lists
  if (task.num_blocks > Command_table::Max_entries)
    return -L4_EINVAL;

  unsigned slot = 0;
  for (auto &s : _slots)
    {
      if (s.reserve())
        {
          s.setup_command(task, cb, port);
          s.setup_data(task.data, task.num_blocks);
          trace.printf("Reserved slot %d.\n", slot);
          if (is_ready())
            {
              trace.printf("Sending off slot %d.\n", slot);
              _cmd_data.get<Command_data>()->dma_flush(slot);
              _regs[Regs::Port::Ci] = 1 << slot;
            }
          else
            {
              // TODO If the mode is enabling, should we wait?
              trace.printf("Device not ready for serving slot %d.\n", slot);
              _slots[slot].abort();
            }

          return slot;
        }
      ++slot;
    }

  return -L4_EBUSY;
}


int
Ahci_port::process_interrupts()
{
  if (_devtype == Ahcidev_none)
    {
      warn.printf("Interrupt for inactive port received.\n");
      return -L4_ENODEV;
    }

  l4_uint32_t istate = _regs[Regs::Port::Is];

  if (istate & (Regs::Port::Is_mask_status))
    {
      warn.printf("Device state changed.\n");
      // TODO Restart the device detection cycle here.
      abort([=]{ reset([]{}); });
      // clear interrupts
      _regs[Regs::Port::Is] = istate;
      // XXX this should be propagated to the driver running the device
      return -L4_EIO;
    }


  if (istate & (Regs::Port::Is_mask_fatal | Regs::Port::Is_mask_error))
    handle_error();
  else
    check_pending_commands();

  // clear interrupts
  _regs[Regs::Port::Is] = Regs::Port::Is_mask_data;

  return L4_EOK;
}

void
Ahci_port::handle_error()
{
  // find the commands that are still pending
  l4_uint32_t slotstate = _regs[Regs::Port::Ci];

  if (is_started())
    {
      // If the port is still active, abort the failing task
      // and try to safe the rest.
      _slots[current_command_slot()].abort();

      check_pending_commands();
    }
  else
    {
      // Otherwise all tasks will be aborted.
      for (auto &s : _slots)
        s.abort();
      slotstate = 0;
    }

  _state = S_error;

  initialize(
    [=]()
      {
        // clear error register and error interrupts
        _regs[Regs::Port::Serr] = 0;
        _regs[Regs::Port::Is] = Regs::Port::Is_mask_fatal
                                | Regs::Port::Is_mask_error;
        enable(
          [=]()
            {
              // if all went well, reissue all commands that were
              // not aborted, otherwise abort everything
              if (slotstate)
                {
                  if (is_ready())
                    _regs[Regs::Port::Ci] = slotstate;
                  else
                    for (auto &s : _slots)
                      s.abort();
                }
            });
      });

}

}
