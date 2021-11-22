#include "qemu/osdep.h"
#include "qemu-common.h"
#include "qemu/datadir.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "e600.h"
#include "e600-ccsr.h"
#include "hw/net/fsl_etsec/etsec.h"
#include "sysemu/device_tree.h"
#include "sysemu/sysemu.h"
#include "sysemu/reset.h"
#include "sysemu/kvm.h"
#include "hw/sysbus.h"
#include "hw/core/generic-loader.h"
#include "hw/pci/pci.h"
#include "hw/ppc/openpic.h"
#include "hw/char/serial.h"
#include "kvm_ppc.h"
#include "hw/ppc/openpic.h"
#include "hw/ppc/openpic_kvm.h"
#include "hw/ppc/ppc.h"
#include "hw/qdev-properties.h"
#include "hw/loader.h"
#include "elf.h"
#include "hw/sysbus.h"
#include "qemu/host-utils.h"
#include "qemu/option.h"
#include "hw/platform-bus.h"
#include "hw/net/fsl_etsec/etsec.h"
#include "hw/i2c/i2c.h"
#include "hw/irq.h"

#define SERIAL0_REGS_OFFSET        0x4500ULL
#define SERIAL1_REGS_OFFSET        0x4600ULL
#define CCSRBAR_SIZE               0x00100000ULL
#define MPIC_REGS_OFFSET           0x40000ULL
#define RAM_SIZES_ALIGN            (64 * MiB)
#define EPAPR_MAGIC                (0x65504150)
#define CLK_FREQ_HZ (400UL * 1000UL * 1000UL)
#define DTC_LOAD_PAD                0x1800000
#define DTC_PAD_MASK                0xFFFFF

#define TYPE_E600PLAT_MACHINE  MACHINE_TYPE_NAME("ppce600")

static GenericLoaderState *loaders[10] = {0};
static int loaders_index = 0;

static void e600_ccsr_initfn(Object *obj)
{
    PPCE600CCSRState *ccsr = CCSR(obj);
    memory_region_init(&ccsr->ccsr_space, obj, "e600-ccsr",
                       CCSRBAR_SIZE);
}

struct boot_info
{
    uint32_t dt_base;
    uint32_t dt_size;
    uint32_t entry;
};

static void ppce600_cpu_reset(void *opaque)
{
    info_report("ppce600 cpu reset..");
    PowerPCCPU *cpu = opaque;
    CPUState *cs = CPU(cpu);
    CPUPPCState *env = &cpu->env;
    struct boot_info *bi = env->load_info;

    cpu_reset(cs);

    /* Set initial guest state. */
    cs->halted = 0;
    env->gpr[1] = (16 * MiB) - 8;
    env->gpr[3] = bi->dt_base;
    env->gpr[4] = 0;
    env->gpr[5] = 0;
    env->gpr[6] = EPAPR_MAGIC;
    env->gpr[7] = 0;
    env->gpr[8] = 0;
    env->gpr[9] = 0;
    env->nip = 0xFFF00100;
    info_report("excp prefix: %08x, msr: %08x", env->excp_prefix, env->msr);
}

static DeviceState *ppce600_init_mpic_qemu(PPCE600MachineState *pms,
                                           IrqLines  *irqs)
{
    DeviceState *dev;
    SysBusDevice *s;
    int i, j, k;
    MachineState *machine = MACHINE(pms);
    unsigned int smp_cpus = machine->smp.cpus;
    const PPCE600MachineClass *pmc = PPCE600_MACHINE_GET_CLASS(pms);

    dev = qdev_new(TYPE_OPENPIC);
    object_property_add_child(OBJECT(machine), "pic", OBJECT(dev));
    qdev_prop_set_uint32(dev, "model", pmc->mpic_version);
    qdev_prop_set_uint32(dev, "nb_cpus", smp_cpus);

    s = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(s, &error_fatal);

    k = 0;
    for (i = 0; i < smp_cpus; i++) {
        for (j = 0; j < OPENPIC_OUTPUT_NB; j++) {
            sysbus_connect_irq(s, k++, irqs[i].irq[j]);
        }
    }

    return dev;
}

static DeviceState *ppce600_init_mpic(PPCE600MachineState *pms,
                                      MemoryRegion *ccsr,
                                      IrqLines *irqs)
{
    DeviceState *dev = NULL;
    SysBusDevice *s;

    if (!dev) {
        dev = ppce600_init_mpic_qemu(pms, irqs);
    }

    s = SYS_BUS_DEVICE(dev);
    memory_region_add_subregion(ccsr, MPIC_REGS_OFFSET,
                                s->mmio[0].memory);

    return dev;
}

typedef struct DeviceTreeParams {
    PPCE600MachineState *machine;
    hwaddr addr;
    hwaddr kernel_base;
    hwaddr kernel_size;
    Notifier notifier;
} DeviceTreeParams;


static int ppce600_load_device_tree(PPCE600MachineState *pms,
                                    hwaddr addr,
                                    hwaddr kernel_base,
                                    hwaddr kernel_size,
                                    bool dry_run)
{
    /* MachineState *machine = MACHINE(pms); */
    /* const PPCE600MachineClass *pmc = PPCE600_MACHINE_GET_CLASS(pms); */
    int ret;
    int fdt_size;
    void *fdt;

    error_report("-->> addr: %08lx", addr);

    fdt = create_device_tree(&fdt_size);
    if (fdt == NULL) {
        return -1;
    }

    qemu_fdt_add_subnode(fdt, "/chosen");

    if (kernel_base != -1ULL) {
        qemu_fdt_setprop_cells(fdt, "/chosen", "qemu,boot-kernel",
                                     kernel_base >> 32, kernel_base,
                                     kernel_size >> 32, kernel_size);
    }

    for (int i = 0; i < loaders_index; i++) {
        GenericLoaderState *s = loaders[i];
        char name[32] = {0};
        sprintf(name, "%s,%d", "loader", i);
        char value[512] = {0};
        sprintf(value, "%s@0x%08lx", s->file, s->addr);
        qemu_fdt_setprop_string(fdt, "/chosen", name, value);
    }

    if (!dry_run) {
        qemu_fdt_dumpdtb(fdt, fdt_size);
        cpu_physical_memory_write(addr, fdt, fdt_size);
    }
    ret = fdt_size;
    g_free(fdt);

    return ret;
}

static void ppce600_reset_device_tree(void *opaque)
{
    DeviceTreeParams *p = opaque;
    ppce600_load_device_tree(p->machine, p->addr,
                             p->kernel_base, p->kernel_size,
                             false);
}

static void ppce600_init_notify(Notifier *notifier, void *data)
{
    DeviceTreeParams *p = container_of(notifier, DeviceTreeParams, notifier);
    ppce600_reset_device_tree(p);
}

static int ppce600_prep_device_tree(PPCE600MachineState *machine,
                                    hwaddr addr,
                                    hwaddr kernel_base,
                                    hwaddr kernel_size)
{
    DeviceTreeParams *p = g_new(DeviceTreeParams, 1);
    p->machine = machine;
    p->addr = addr;
    p->kernel_base = kernel_base;
    p->kernel_size = kernel_size;

    p->notifier.notify = ppce600_init_notify;
    qemu_add_machine_init_done_notifier(&p->notifier);

    /* Issue the device tree loader once, so that we get the size of the blob */
    return ppce600_load_device_tree(machine, addr, kernel_base, kernel_size, true);
}


static void ppce600_init(MachineState *machine)
{
    MemoryRegion *address_space_mem = get_system_memory();
    struct PPCE600MachineState *pms = PPCE600_MACHINE(machine);
    const PPCE600MachineClass *pmc = PPCE600_MACHINE_GET_CLASS(machine);
    /* PCIBus *pci_bus; */
    CPUPPCState *env = NULL;
    uint64_t loadaddr;
    hwaddr kernel_base = -1LL;
    int kernel_size = 0;
    /* hwaddr dt_base = 0; */
    /* hwaddr initrd_base = 0; */
    /* int initrd_size = 0; */
    hwaddr cur_base = 0;
    char *filename;
    const char *payload_name;
    bool kernel_as_payload;
    hwaddr bios_entry = 0;
    target_long payload_size;
    struct boot_info *boot_info;
    /* int dt_size; */
    int i;

    CPUPPCState *firstenv = NULL;
    DeviceState *dev;
    DeviceState *mpicdev;
    MemoryRegion *ccsr_addr_space;
    /* SysBusDevice *s; */
    PPCE600CCSRState *ccsr;
    unsigned int smp_cpus = machine->smp.cpus;

    IrqLines *irqs;
    irqs = g_new0(IrqLines, smp_cpus);
    for (i = 0; i < smp_cpus; i++) {
        PowerPCCPU *cpu;
        CPUState *cs;
        qemu_irq *input;

        cpu = POWERPC_CPU(object_new(machine->cpu_type));
        env = &cpu->env;
        cs = CPU(cpu);

        if (env->mmu_model != POWERPC_MMU_32B) {
            error_report("MMU model %i not supported by this machine",
                         env->mmu_model);
            exit(1);
        }

        /*
         * Secondary CPU starts in halted state for now. Needs to change
         * when implementing non-kernel boot.
         */
        object_property_set_bool(OBJECT(cs), "start-powered-off", i != 0,
                                 &error_fatal);
        qdev_realize_and_unref(DEVICE(cs), NULL, &error_fatal);

        if (!firstenv) {
            firstenv = env;
        }

        input = (qemu_irq *)env->irq_inputs;
        irqs[i].irq[OPENPIC_OUTPUT_INT] = input[PPC6xx_INPUT_INT];
        cpu_ppc_tb_init(env, CLK_FREQ_HZ);

        /* TODO: register reset handler */
        /* Register reset handler */
        if (!i) {
            /* Primary CPU */
            struct boot_info *boot_info;
            boot_info = g_malloc0(sizeof(struct boot_info));
            qemu_register_reset(ppce600_cpu_reset, cpu);
            env->load_info = boot_info;
        } else {
            /* Secondary CPUs */
            /* qemu_register_reset(ppce600_cpu_reset_sec, cpu); */
        }
    }

    env = firstenv;
    if (!QEMU_IS_ALIGNED(machine->ram_size, RAM_SIZES_ALIGN)) {
        error_report("RAM size must be multiple of %" PRIu64, RAM_SIZES_ALIGN);
        exit(EXIT_FAILURE);
    }

    info_report("RAM size: %lu", machine->ram_size);

    /* Register Memory */
    memory_region_add_subregion(address_space_mem, 0, machine->ram);
    dev = qdev_new("e600-ccsr");
    object_property_add_child(qdev_get_machine(), "e600-ccsr",
                              OBJECT(dev));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    ccsr = CCSR(dev);
    ccsr_addr_space = &ccsr->ccsr_space;
    memory_region_add_subregion(address_space_mem, pmc->ccsrbar_base,
                                ccsr_addr_space);

    mpicdev = ppce600_init_mpic(pms, ccsr_addr_space, irqs);
    g_free(irqs);

    /* Serial */
    if (serial_hd(0)) {
        serial_mm_init(ccsr_addr_space, SERIAL0_REGS_OFFSET,
                       0, qdev_get_gpio_in(mpicdev, 42), 399193,
                       serial_hd(0), DEVICE_BIG_ENDIAN);
    }

    if (serial_hd(1)) {
        serial_mm_init(ccsr_addr_space, SERIAL1_REGS_OFFSET,
                       0, qdev_get_gpio_in(mpicdev, 42), 399193,
                       serial_hd(1), DEVICE_BIG_ENDIAN);
    }

    /*
     * Smart firmware defaults ahead!
     *
     * We follow the following table to select which payload we execute.
     *
     *  -kernel | -bios | payload
     * ---------+-------+---------
     *     N    |   Y   | u-boot
     *     N    |   N   | u-boot
     *     Y    |   Y   | u-boot
     *     Y    |   N   | kernel
     *
     * This ensures backwards compatibility with how we used to expose
     * -kernel to users but allows them to run through u-boot as well.
     */
    kernel_as_payload = false;
    if (machine->firmware == NULL) {
        if (machine->kernel_filename) {
            payload_name = machine->kernel_filename;
            kernel_as_payload = true;
        } else {
            payload_name = "u-boot.e600";
        }
    } else {
        payload_name = machine->firmware;
    }

    filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, payload_name);
    if (!filename) {
        error_report("could not find firmware/kernel file '%s'", payload_name);
        exit(1);
    }

    payload_size = load_elf(filename, NULL, NULL, NULL,
                            &bios_entry, &loadaddr, NULL, NULL,
                            1, PPC_ELF_MACHINE, 0, 0);
    if (payload_size < 0) {
        /*
         * Hrm. No ELF image? Try a uImage, maybe someone is giving us an
         * ePAPR compliant kernel
         */
        loadaddr = LOAD_UIMAGE_LOADADDR_INVALID;
        payload_size = load_uimage(filename, &bios_entry, &loadaddr, NULL,
                                   NULL, NULL);
        if (payload_size < 0) {
            error_report("could not load firmware '%s'", filename);
            exit(1);
        }
    }

    g_free(filename);

    if (kernel_as_payload) {
        kernel_base = loadaddr;
        kernel_size = payload_size;
    }

    info_report("kernel base: 0x%08lx", kernel_base);

    cur_base = loadaddr + payload_size;
    if (cur_base < 32 * MiB) {
        /* u-boot occupies memory up to 32MB, so load blobs above */
        cur_base = 32 * MiB;
    }

    /* Load bare kernel only if no bios/u-boot has been provided */
    if (machine->kernel_filename && !kernel_as_payload) {
        kernel_base = cur_base;
        kernel_size = load_image_targphys(machine->kernel_filename,
                                          cur_base,
                                          machine->ram_size - cur_base);
        if (kernel_size < 0) {
            error_report("could not load kernel '%s'",
                         machine->kernel_filename);
            exit(1);
        }

        cur_base += kernel_size;
    }

    hwaddr dt_base = (loadaddr + payload_size + DTC_LOAD_PAD) & ~DTC_PAD_MASK;
    int dt_size = ppce600_prep_device_tree(pms, dt_base, kernel_base, kernel_size);

    boot_info = env->load_info;
    boot_info->entry = bios_entry;
    boot_info->dt_base = dt_base;
    boot_info->dt_size = dt_size;

    info_report("init done");
}

static void e600plat_init(MachineState *machine)
{
    ppce600_init(machine);

    return;
}

static HotplugHandler *e600plat_machine_get_hotpug_handler(MachineState *machine, DeviceState *dev)
{
    if (object_dynamic_cast(OBJECT(dev), "loader")) {
        GenericLoaderState *s = GENERIC_LOADER(dev);
        loaders[loaders_index++] = s;
        error_report("[%d]: %s", loaders_index, s->file);
    }

    return NULL;
}


static void e600plat_machine_class_init(ObjectClass *oc, void *data)
{
    PPCE600MachineClass *pmc = PPCE600_MACHINE_CLASS(oc);
    MachineClass *mc = MACHINE_CLASS(oc);
    mc->get_hotplug_handler = e600plat_machine_get_hotpug_handler;

    pmc->platform_bus_base = 0xf00000000ULL;
    pmc->platform_bus_size = 128 * MiB;
    pmc->platform_bus_first_irq = 5;
    pmc->platform_bus_num_irqs = 10;
    pmc->ccsrbar_base = 0XE0000000ULL;

    mc->desc = "generic paravirt e600 platform";
    mc->init = e600plat_init;
    mc->max_cpus = 8;
    mc->default_cpu_type = POWERPC_CPU_TYPE_NAME("e600");
    /* FIXME: what is this ram? */
    mc->default_ram_id = "mpc8610hpcd.ram";
    /* FIXME: eTSEC ? */
    /* machine_class_allow_dynamic_sysbus_dev(mc, TYPE_ETSEC_COMMON); */
}



static const TypeInfo e600plat_info = {
    .name = TYPE_E600PLAT_MACHINE,
    .parent = TYPE_PPCE600_MACHINE,
    .class_init = e600plat_machine_class_init,
    .interfaces = (InterfaceInfo[]) {
        { }
    }
};

static void e600plat_register_types(void)
{
    type_register_static(&e600plat_info);
}

static const TypeInfo e600_ccsr_info = {
    .name          = TYPE_CCSR,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(PPCE600CCSRState),
    .instance_init = e600_ccsr_initfn,
};

static const TypeInfo ppce600_info = {
    .name          = TYPE_PPCE600_MACHINE,
    .parent        = TYPE_MACHINE,
    .abstract      = true,
    .instance_size = sizeof(PPCE600MachineState),
    .class_size    = sizeof(PPCE600MachineClass),
};

static void e600_register_types(void)
{
    type_register_static(&e600_ccsr_info);
    type_register_static(&ppce600_info);
}

type_init(e600_register_types)
type_init(e600plat_register_types)
