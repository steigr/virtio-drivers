# virtio transport for IRIX

What every IRIX virtio driver here links into its module. It is not a module by itself.

- `virtq.c`, `virtq.h`: split rings. No kernel dependencies; cache maintenance arrives as
  `vq_sync_ops`. Everything the device reads is little-endian, stored a byte at a time.
- `virtio_gio.c`: virtio-mmio on GIO (IP22, IP28). GIO DMA is not cache coherent, so rings stay
  cached and are written back and invalidated around every hand-over.
- `virtio_pci.c`: virtio-pci (IP30, IP35), finding the device's functions in the hwgraph.
- `virtio_bus.h`: what a driver sees. Each transport fills in `virtio_bus_ops` and is compiled out
  on other boards; the board's transport supplies `virtio_bus_init`, which finds the device a
  `virtio_driver` names.
- `virtio.mk`: included by each driver's Makefile. It sets the kernel CFLAGS for `CPUBOARD` and
  compiles these files into the driver's object directory.

**Maturity:** runs on IP28 (GIO) and IP35 (PCI) under IRIS; compiles for IP22, IP24 and IP30.

## Testing

    make -C tests            # on any host with a C99 compiler: the split rings under ASan and UBSan
