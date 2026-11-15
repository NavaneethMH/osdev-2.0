#ifndef TINYOS_PCI_H
#define TINYOS_PCI_H

struct pci_device_info {
    unsigned char bus;
    unsigned char device;
    unsigned char function;
    unsigned char class_code;
    unsigned char subclass;
    unsigned char programming_interface;
    unsigned char header_type;
    unsigned short vendor_id;
    unsigned short device_id;
};

#define PCI_DEVICE_LIST_CAPACITY 64U

/* Returns the total number of present PCI functions. The output array is
   filled with up to capacity entries in bus/device/function order. */
unsigned int pci_discover(struct pci_device_info *devices,
                          unsigned int capacity);

#endif
