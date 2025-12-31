#ifndef __SENSOR_LSM6DSM_H__
#define __SENSOR_LSM6DSM_H__
/* Includes -----------------------------------------------------------------*/
#include <sensor.h>
#include "lsm6dsm_reg.h"
/* Exported types -----------------------------------------------------------*/
/* Exported constants -------------------------------------------------------*/
/* Exported macro -----------------------------------------------------------*/
/* Exported functions -------------------------------------------------------*/
int rt_hw_lsm6dsm_init(const char *name, struct rt_sensor_config *cfg);

#endif /*__SENSOR_LSM6DSM_H__*/
/* End of file****************************************************************/
