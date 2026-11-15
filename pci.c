#include "pci.h"

#define PCI_CONFIG_ADDRESS 0x0CF8U
#define PCI_CONFIG_DATA 0x0CFCU

static void outl(unsigned short port, unsigned int value)
{
    __asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
}

static unsigned int inl(unsigned short port)
{
    unsigned int value;
    __asm__ volatile ("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static unsigned int pci_read_config(unsigned int bus, unsigned int device,
                                    unsigned int function,
                                    unsigned int offset)
{
    unsigned int address = 0x80000000U |
        ((bus & 0xFFU) << 16) |
        ((device & 0x1FU) << 11) |
        ((function & 0x07U) << 8) |
        (offset & 0xFCU);

    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

static void record_device(struct pci_device_info *devices,
                          unsigned int capacity,
                          unsigned int *device_count,
                          unsigned int bus, unsigned int device,
                          unsigned int function, unsigned int id,
                          unsigned int class_register,
                          unsigned int header_register)
{
    unsigned int index = *device_count;
    if (index < capacity && devices != 0) {
        devices[index].bus = (unsigned char)bus;
        devices[index].device = (unsigned char)device;
        devices[index].function = (unsigned char)function;
        devices[index].vendor_id = (unsigned short)(id & 0xFFFFU);
        devices[index].device_id = (unsigned short)(id >> 16);
        devices[index].class_code = (unsigned char)(class_register >> 24);
        devices[index].subclass = (unsigned char)(class_register >> 16);
        devices[index].programming_interface =
            (unsigned char)(class_register >> 8);
        devices[index].header_type = (unsigned char)(header_register >> 16);
    }
    ++*device_count;
}

unsigned int pci_discover(struct pci_device_info *devices,
                          unsigned int capacity)
{
    unsigned int device_count = 0U;

    for (unsigned int bus = 0U; bus < 256U; ++bus) {
        for (unsigned int device = 0U; device < 32U; ++device) {
            unsigned int id = pci_read_config(bus, device, 0U, 0U);
            if ((id & 0xFFFFU) == 0xFFFFU) {
                continue;
            }

            unsigned int class_register =
                pci_read_config(bus, device, 0U, 0x08U);
            unsigned int header_register =
                pci_read_config(bus, device, 0U, 0x0CU);
            record_device(devices, capacity, &device_count, bus, device,
                          0U, id, class_register, header_register);

            if (((header_register >> 16) & 0x80U) == 0U) {
                continue;
            }

            for (unsigned int function = 1U; function < 8U; ++function) {
                id = pci_read_config(bus, device, function, 0U);
                if ((id & 0xFFFFU) == 0xFFFFU) {
                    continue;
                }

                class_register =
                    pci_read_config(bus, device, function, 0x08U);
                header_register =
                    pci_read_config(bus, device, function, 0x0CU);
                record_device(devices, capacity, &device_count, bus, device,
                              function, id, class_register, header_register);
            }
        }
    }

    return device_count;
}
