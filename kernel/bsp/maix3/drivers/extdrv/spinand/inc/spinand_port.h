#ifndef K230_SPINAND_PORT_H
#define K230_SPINAND_PORT_H

#include <rtthread.h>
#include "spinand.h"

struct rt_qspi_device;

/* Supply a zero-initialized flash and an attached, configured QSPI device.
 * The caller owns both objects and the bus configuration. Failed probes clean
 * up their allocations. Detach all MTD devices/users before deinitializing. */
rt_err_t spinand_probe(struct aic_spinand *flash, struct rt_qspi_device *device);
void spinand_deinit(struct aic_spinand *flash);

#endif
