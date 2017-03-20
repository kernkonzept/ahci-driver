/*
 * Copyright (C) 2015 Kernkonzept GmbH.
 * Author(s): Sarah Hoffmann <sarah.hoffmann@kernkonzept.com>
 *
 * This file is distributed under the terms of the GNU General Public
 * License, version 2.  Please see the COPYING-GPL-2 file for details.
 */
#pragma once

#include <l4/sys/factory>
#include <l4/re/dataspace>
#include <l4/re/env>
#include <l4/re/util/cap_alloc>
#include <l4/re/util/object_registry>
#include <l4/re/error_helper>
#include <l4/cxx/ipc_server>

#include <l4/util/atomic.h>
#include <l4/l4virtio/l4virtio>
#include <l4/l4virtio/virtqueue>
#include <l4/l4virtio/virtio_block.h>

#include <cstring>
#include <vector>
#include <functional>

namespace L4virtio { namespace Driver {


/**
 * \brief Client-side implementation for a general virtio device.
 */
class Device
{
public:
  /**
   * Contacts the device and starts the initial handshake.
   *
   * \param srvcap    Capability for device communication.
   *
   * \throws L4::Runtime_error if the initialisation fails
   *
   * This function contacts the server, sets up the notification
   * channels and the configuration dataspace. After this is done,
   * the caller can set up any dataspaces it needs. The initialisation
   * then needs to be finished by calling driver_acknowledge().
   */
  void driver_connect(L4::Cap<L4virtio::Device> srvcap)
  {
    _device = srvcap;
    _guest_irq = L4Re::chkcap(L4Re::Util::cap_alloc.alloc<L4::Irq>(),
                              "Cannot allocate guest IRQ");

    _host_irq = L4Re::chkcap(L4Re::Util::cap_alloc.alloc<L4::Irq>(),
                             "Cannot allocate host irq");

    _config_cap = L4Re::chkcap(L4Re::Util::cap_alloc.alloc<L4Re::Dataspace>(),
                               "Cannot allocate cap for config dataspace");

    _next_devaddr = L4_SUPERPAGESIZE;

    auto *e = L4Re::Env::env();
    L4Re::chksys(e->rm()->attach(&_config, L4_PAGESIZE, L4Re::Rm::Search_addr,
                                 L4::Ipc::make_cap_rw(_config_cap.get()), 0,
                                 L4_PAGESHIFT),
                 "Cannot attach config dataspace");

    L4Re::chksys(l4_error(e->factory()->create(_guest_irq.get())),
                 "Cannot create guest irq");

    L4Re::chksys(_device->register_iface(_guest_irq.get(), _host_irq.get(),
                                         _config_cap.get()),
                 "Error registering interface with device");

    if (memcmp(&_config->magic, "virt", 4) != 0)
      L4Re::chksys(-L4_ENODEV, "Device config has wrong magic value");

    if (_config->version != 2)
      L4Re::chksys(-L4_ENODEV, "Invalid virtio version, must be 2");

    _device->set_status(0); // reset
    int status = L4VIRTIO_STATUS_ACKNOWLEDGE;
    _device->set_status(status);

    status |= L4VIRTIO_STATUS_DRIVER;
    _device->set_status(status);

    if (_config->status & L4VIRTIO_STATUS_FAILED)
      L4Re::chksys(-L4_EIO, "Device failure during initialisation.");
  }

  /**
   * Attach the given thread to the notification interrupt.
   *
   * \param cap Thread to attach the IRQ to.
   *
   * \return L4_EOK or error message.
   *
   * This function should only be used, when the device is used
   * synchronously with send_and_wait. Otherwise the instance
   * needs to be registered with a server registry.
   */
  int attach_guest_irq(L4::Cap<L4::Thread> const &cap)
  {
    return l4_error(_guest_irq->attach(0, cap));
  }

  /// Return true if the device is in a fail state.
  bool failed() const { return _config->status & L4VIRTIO_STATUS_FAILED; }

  /**
   * Finalize handshake with the device.
   *
   * Must be called after all queues have been set up and before the first
   * request is sent. It is still possible to add more shared dataspaces
   * after the handshake has been finished.
   *
   */
  int driver_acknowledge()
  {
    _config->driver_features_map[0] = _config->dev_features_map[0];
    _config->driver_features_map[1] = _config->dev_features_map[1];

    _device->set_status(_config->status | L4VIRTIO_STATUS_DRIVER_OK);

    if (_config->status & L4VIRTIO_STATUS_FAILED)
      return -L4_EIO;

    return L4_EOK;
  }


  /**
   * Share a dataspace with the device.
   *
   * \param ds      Dataspace to share with the device.
   * \param offset  Offset in dataspace where the shared part starts.
   * \param size    Total size in bytes of the shared space.
   * \param devaddr Start of shared space in the device address space.
   *
   * Although this function allows to share only a part of the given dataspace
   * for convenience, the granularity of sharing is always the dataspace level.
   * Thus, the remainder of the dataspace is not protected from the device.
   *
   * When communicating with the device, addresses must be given with respect
   * to the device address space. This is not the same as the virtual address
   * space of the client in order to not leak information about the address
   * space layout.
   */
  int register_ds(L4::Cap<L4Re::Dataspace> ds, l4_umword_t offset,
                  l4_umword_t size, l4_uint64_t *devaddr)
  {
    *devaddr = next_device_address(size);
    return _device->register_ds(L4::Ipc::make_cap_rw(ds), *devaddr, offset, size);
  }

  /**
   * Send the virtqueue configuration to the device.
   *
   * \param  num         Number of queue to configure.
   * \param  size        Size of rings in the queue, must be a power of 2)
   * \param  desc_addr   Address of descriptor table (device address)
   * \param  avail_addr  Address of available ring (device address)
   * \param  used_addr   Address of used ring (device address)
   */
  int config_queue(int num, unsigned size, l4_uint64_t desc_addr,
                   l4_uint64_t avail_addr, l4_uint64_t used_addr)
  {
    auto *queueconf = &_config->queues()[num];
    queueconf->num = size;
    queueconf->desc_addr = desc_addr;
    queueconf->avail_addr = avail_addr;
    queueconf->used_addr = used_addr;
    queueconf->ready = 1;

    return _device->config_queue(num);
  }

  /**
   * Maximum queue size allowed by the device.
   *
   * \param num  Number of queue for which to determine the maximum size.
   */
  int max_queue_size(int num) const
  {
    return _config->queues()[num].num_max;
  }

  /**
   * Send a request to the device and wait for it to be processed.
   *
   * \param queue  Queue that contains the request in its descriptor table
   * \param descno Index of first entry in descriptor table where
   *
   * This function provides a simple mechanism to send requests
   * synchronously. It must not be used with other requests at the same
   * time as it directly waits for a notification on the device irq cap.
   */
  int send_and_wait(Virtqueue &queue, int descno)
  {
    // send the request away
    queue.enqueue_descriptor(descno);
    if (!queue.no_notify_host())
      _host_irq->trigger();

    // wait for a reply, we assume that no other
    // request will get in the way.
    while (true)
      {
        int err = l4_error(l4_ipc_receive(_guest_irq.get().cap(),
                                          l4_utcb(), L4_IPC_NEVER));
        if (err < 0)
          return err;

        int head = queue.find_next_used();
        if (head >= 0) // may be null if there was a spurious interrupt
          return (head == descno) ? L4_EOK : -L4_EINVAL;
      }
  }

  /**
   * Send a request to the device.
   *
   * \param queue  Queue that contains the request in its descriptor table
   * \param descno Index of first entry in descriptor table where
   */
  void send(Virtqueue &queue, int descno)
  {
    queue.enqueue_descriptor(descno);
    if (!queue.no_notify_host())
      _host_irq->trigger();
  }

private:
  /**
   * Get the next free address, covering the given area.
   *
   * \param size Size of requested area.
   *
   * Builds up a virtual address space for the device.
   * Simply give out the memory linearly, it is unlikely that a client
   * wants to map more than 4GB and it certainly shouldn't reallocate all the
   * time.
   */
  l4_uint64_t next_device_address(l4_umword_t size)
  {
    l4_umword_t ret;
    size = l4_round_page(size);
    do
      {
        ret = _next_devaddr;
        if (l4_umword_t(~0) - ret < size)
          L4Re::chksys(-L4_ENOMEM, "Out of device address space.");
      }
    while (!l4util_cmpxchg(&_next_devaddr, ret, ret + size));

    return ret;
  }

protected:
  L4::Cap<L4virtio::Device> _device;
  L4Re::Rm::Auto_region<L4virtio::Device::Config_hdr *> _config;
  l4_umword_t _next_devaddr;
  L4Re::Util::Auto_cap<L4::Irq>::Cap _guest_irq;

private:
  L4Re::Util::Auto_cap<L4::Irq>::Cap _host_irq;
  L4Re::Util::Auto_cap<L4Re::Dataspace>::Cap _config_cap;
};


/**
 * Simple class for accessing a virtio block device synchronously.
 */
class Block_device : public Device, public L4::Irq_handler_object
{
public:
  typedef std::function<void(unsigned char)> Callback;

private:
  enum { Header_size = sizeof(l4virtio_block_header_t) };

  struct Request
  {
    l4_uint16_t tail;
    Callback callback;

    Request() : tail(Virtqueue::Eoq), callback(0) {}
  };

public:
  /**
   * Handle to an ongoing request.
   */
  class Handle
  {
    friend Block_device;
    l4_uint16_t head;

    explicit Handle(l4_uint16_t descno) : head(descno) {}

  public:
    Handle() : head(Virtqueue::Eoq) {}
    bool valid() const { return head != Virtqueue::Eoq; }
  };

  /**
   * Setup a connection to a device and set up shared memory.
   *
   * \param srvcap       IPC capability of the channel to the server.
   * \param usermem      Size of additional memory to share with device.
   * \param userdata     Pointer to the region of user-usable memory.
   * \param user_devaddr Adress of user-usable memory in device address space.
   *
   * This function starts a hand shake with the device and sets up the
   * virtqueues for communication and the additional data structures for
   * the block device. It will also allocate and share additional memory
   * that the caller then can use freely, i.e. normally this memory would
   * be used as a reception buffer. The caller may also decide to not make use
   * of this convenience funcion and request 0 bytes in usermem. Then it has
   * to allocate the block buffers for sending/receiving payload manually and
   * share them using register_ds().
   */
  void setup_device(L4::Cap<L4virtio::Device> srvcap,
                   l4_size_t usermem, void **userdata,
                   Ptr<void> &user_devaddr)
  {
    // Contact device.
    driver_connect(srvcap);

    if (_config->device != L4VIRTIO_ID_BLOCK)
      L4Re::chksys(-L4_ENODEV, "Device is not a block device.");

    if (_config->num_queues != 1)
      L4Re::chksys(-L4_EINVAL, "Invalid number of queues reported.");

    // Memory is shared in one large dataspace which contains queues,
    // space for header/status and additional user-defined memory.
    unsigned queuesz = max_queue_size(0);
    l4_size_t totalsz = l4_round_page(usermem);

    // reserve space for one header/status per descriptor
    // TODO Should be reduced to 1/3 but this way no freelist is needed.
    totalsz += l4_round_page(_queue.total_size(queuesz)
                             + queuesz * (Header_size + 1));

    _queue_ds = L4Re::Util::cap_alloc.alloc<L4Re::Dataspace>();
    auto *e = L4Re::Env::env();
    L4Re::chksys(e->mem_alloc()->alloc(totalsz, _queue_ds,
                                       L4Re::Mem_alloc::Continuous
                                       | L4Re::Mem_alloc::Pinned),
                 "Cannot allocate memory for virtio structures.");

    // Now sort out which region goes where in the dataspace.
    l4_addr_t baseaddr;
    L4Re::chksys(e->rm()->attach(&baseaddr, totalsz, L4Re::Rm::Search_addr,
                                 L4::Ipc::make_cap_rw(_queue_ds), 0, L4_PAGESHIFT),
                 "Cannot attach dataspace for virtio structures.");

    l4_uint64_t devaddr;
    L4Re::chksys(register_ds(_queue_ds, 0, totalsz, &devaddr),
                 "Cannot share virtio datastructures.");

    _queue.init_queue(queuesz, (void *) baseaddr);

    config_queue(0, queuesz, devaddr, devaddr + _queue.avail_offset(),
                 devaddr + _queue.used_offset());

    l4_uint64_t offset = _queue.total_size();
    _header_addr = devaddr + offset;
    _headers = (l4virtio_block_header_t *) (baseaddr + offset);

    offset += queuesz * Header_size;
    _status_addr = devaddr + offset;
    _status = (unsigned char *) (baseaddr + offset);

    offset = l4_round_page(offset + queuesz);
    user_devaddr = Ptr<void>(devaddr + offset);
    if (userdata)
      *userdata = (void *) (baseaddr + offset);

    // setup the callback mechanism
    _pending.assign(queuesz, Request());

    // Finish handshake with device.
    driver_acknowledge();
  }

  /**
   * Register the server with a server registry.
   *
   * \param registry Registry to attach to.
   *
   * The server is needed to handle device notifications, when
   * requests are ready.
   */
  void register_server(L4Re::Util::Object_registry *registry)
  {
    registry->register_irq_obj(this, _guest_irq.get());
  }

  /**
   * Return a reference to the device configuration.
   */
  l4virtio_block_config_t const &device_config() const
  {
    return *_config->device_config<l4virtio_block_config_t>();
  }

  /**
   * Start the setup of a new request.
   *
   * \param sector   First sector to write to/read from.
   * \param type     Request type.
   * \param callback Function to call, when the request is finished.
   *                 May be 0 for synchronous requests.
   */
  Handle start_request(l4_uint64_t sector, l4_uint32_t type,
                       Callback callback)
  {
    l4_uint16_t descno = _queue.alloc_descriptor();
    if (descno == Virtqueue::Eoq)
      return Handle(Virtqueue::Eoq);

    L4virtio::Virtqueue::Desc &desc = _queue.desc(descno);
    Request &req = _pending[descno];

    // setup the header
    l4virtio_block_header_t &head = _headers[descno];
    head.type = type;
    head.ioprio = 0;
    head.sector = sector;

    // and put it in the descriptor
    desc.addr = Ptr<void>(_header_addr + descno * Header_size);
    desc.len = Header_size;
    desc.flags.raw = 0; // no write, no indirect

    req.tail = descno;
    req.callback = callback;

    return Handle(descno);
  }

  /**
   * Add a data block to a request that has already been set up.
   *
   * \param handle  Handle to request previously set up with start_request().
   * \param addr    Address of data block in device address space.
   * \param size    Size of data block.
   */
  int add_block(Handle handle, Ptr<void> addr, l4_uint32_t size)
  {
    l4_uint16_t descno = _queue.alloc_descriptor();
    if (descno == Virtqueue::Eoq)
      return -L4_ENOMEM;

    Request &req = _pending[handle.head];
    L4virtio::Virtqueue::Desc &desc = _queue.desc(descno);
    L4virtio::Virtqueue::Desc &prev = _queue.desc(req.tail);

    prev.next = descno;
    prev.flags.next() = true;

    desc.addr = addr;
    desc.len = size;
    desc.flags.raw = 0;
    if (_headers[handle.head].type > 0) // write or flush request
      desc.flags.write() = true;

    req.tail = descno;

    return L4_EOK;
  }

  /**
   * Process request asynchronously.
   *
   * \param handle  Handle to request to send to the device
   *
   * Sends a request to the driver that was previously set up
   * with start_request() and add_block() and wait for it to be
   * executed.
   */
  int send_request(Handle handle)
  {
    // add the status bit
    l4_uint16_t descno = _queue.alloc_descriptor();
    if (descno == Virtqueue::Eoq)
      return -L4_ENOMEM;

    Request &req = _pending[handle.head];
    L4virtio::Virtqueue::Desc &desc = _queue.desc(descno);
    L4virtio::Virtqueue::Desc &prev = _queue.desc(req.tail);

    prev.next = descno;
    prev.flags.next() = true;

    desc.addr = Ptr<void>(_status_addr + descno);
    desc.len = 1;
    desc.flags.raw = 0;
    desc.flags.write() = true;

    req.tail = descno;

    send(_queue, handle.head);

    return L4_EOK;
  }

  /**
   * Process request synchronously.
   *
   * \param req  Request to send to the device
   *
   * Sends a request to the driver that was previously set up
   * with start_request() and add_block() and wait for it to be
   * executed.
   */
  int process_request(Handle handle)
  {
    // add the status bit
    int descno = _queue.alloc_descriptor();
    if (descno < 0)
      return descno;

    L4virtio::Virtqueue::Desc &desc = _queue.desc(descno);
    L4virtio::Virtqueue::Desc &prev = _queue.desc(_pending[handle.head].tail);

    prev.next = descno;
    prev.flags.next() = true;

    desc.addr = Ptr<void>(_status_addr + descno);
    desc.len = 1;
    desc.flags.raw = 0;
    desc.flags.write() = true;

    _pending[handle.head].tail = descno;

    int ret = send_and_wait(_queue, handle.head);
    unsigned char status = _status[descno];
    free_request(handle);

    if (ret < 0)
      return ret;

    switch (status)
      {
      case L4VIRTIO_BLOCK_S_OK: return L4_EOK;
      case L4VIRTIO_BLOCK_S_IOERR: return -L4_EIO;
      case L4VIRTIO_BLOCK_S_UNSUPP: return -L4_ENOSYS;
      }

    return -L4_EINVAL;
  }

  void free_request(Handle handle)
  {
    if (handle.head != Virtqueue::Eoq
        && _pending[handle.head].tail != Virtqueue::Eoq)
      _queue.free_descriptor(handle.head, _pending[handle.head].tail);
    _pending[handle.head].tail = Virtqueue::Eoq;
  }

  int dispatch(l4_umword_t, L4::Ipc::Iostream &)
  {
    l4_uint16_t descno = _queue.find_next_used();
    while (descno != Virtqueue::Eoq)
      {
        if (descno < _queue.num() && _pending[descno].tail != Virtqueue::Eoq)
          {
            unsigned char status = _status[descno];
            free_request(Handle(descno));
            if (_pending[descno].callback)
              _pending[descno].callback(status);
          }
        else
          L4Re::chksys(-L4_ENOSYS, "Bad descriptor number");

        descno = _queue.find_next_used();
      }

    return -L4_ENOREPLY;
  }

private:
  L4::Cap<L4Re::Dataspace> _queue_ds;
  l4virtio_block_header_t *_headers;
  unsigned char *_status;
  l4_uint64_t _header_addr;
  l4_uint64_t _status_addr;
  Virtqueue _queue;
  std::vector<Request> _pending;
};

} }
