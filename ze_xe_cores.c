#include <level_zero/ze_api.h>
#include <stdio.h>

int main() {
    zeInit(0);

    uint32_t driverCount = 0;
    zeDriverGet(&driverCount, NULL);
    if (driverCount == 0) {
        puts("No Level Zero drivers found");
        return 1;
    }

    ze_driver_handle_t driver;
    zeDriverGet(&driverCount, &driver);

    uint32_t deviceCount = 0;
    zeDeviceGet(driver, &deviceCount, NULL);
    if (deviceCount == 0) {
        puts("No Level Zero devices found");
        return 1;
    }

    ze_device_handle_t device;
    zeDeviceGet(driver, &deviceCount, &device);

    ze_device_properties_t p = {
        .stype = ZE_STRUCTURE_TYPE_DEVICE_PROPERTIES
    };
    zeDeviceGetProperties(device, &p);

    uint32_t total_eus =
        p.numSlices *
        p.numSubslicesPerSlice *
        p.numEUsPerSubslice;

    uint32_t xe_cores = total_eus / 16;  // Xe-LP mapping

    printf("Slices              : %u\n", p.numSlices);
    printf("Subslices / Slice   : %u\n", p.numSubslicesPerSlice);
    printf("EUs / Subslice      : %u\n", p.numEUsPerSubslice);
    printf("Total EUs           : %u\n", total_eus);
    printf("Xe cores (Xe-LP)    : %u\n", xe_cores);

    return 0;
}
