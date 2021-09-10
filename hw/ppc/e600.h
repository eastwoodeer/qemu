#ifndef PPCE600_H
#define PPCE600_H

#include "hw/boards.h"
#include "hw/platform-bus.h"
#include "qom/object.h"

struct PPCE600MachineState {
    /*< private >*/
    MachineState parent_obj;

    /* points to instance of TYPE_PLATFORM_BUS_DEVICE if
     * board supports dynamic sysbus devices
     */
    PlatformBusDevice *pbus_dev;
};

struct PPCE600MachineClass {
    /*< private >*/
    MachineClass parent_class;

    /* required -- must at least add toplevel board compatible */
    void (*fixup_devtree)(void *fdt);

    int pci_first_slot;
    int pci_nr_slots;

    int mpic_version;
    bool has_mpc8xxx_gpio;
    bool has_platform_bus;
    hwaddr platform_bus_base;
    hwaddr platform_bus_size;
    int platform_bus_first_irq;
    int platform_bus_num_irqs;
    hwaddr ccsrbar_base;
    hwaddr pci_pio_base;
    hwaddr pci_mmio_base;
    hwaddr pci_mmio_bus_base;
    hwaddr spin_base;
};

/* void ppce500_init(MachineState *machine); */

#define TYPE_PPCE600_MACHINE      "ppce600-base-machine"
OBJECT_DECLARE_TYPE(PPCE600MachineState, PPCE600MachineClass, PPCE600_MACHINE)

#endif /* PPCE600_H */
