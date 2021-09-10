#ifndef E600_CCSR_H
#define E600_CCSR_H

#include "hw/sysbus.h"
#include "qom/object.h"

struct PPCE600CCSRState {
    /*< private >*/
    SysBusDevice parent;
    /*< public >*/

    MemoryRegion ccsr_space;
};

#define TYPE_CCSR "e600-ccsr"
OBJECT_DECLARE_SIMPLE_TYPE(PPCE600CCSRState, CCSR)

#endif /* E600_CCSR_H */
