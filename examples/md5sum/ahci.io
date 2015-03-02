local hw = Io.system_bus()

Io.add_vbus("ahcidrv", Io.Vi.System_bus
{
  PCI0 = Io.Vi.PCI_bus_ident
  {
    pci_hd = wrap(hw:match("PCI/CC_01"));
  }
})

