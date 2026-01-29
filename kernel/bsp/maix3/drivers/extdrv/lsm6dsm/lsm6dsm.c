#include "lsm6dsm.h"
#include "rtconfig.h"
#include <string.h>
#include <stdlib.h>
#define DBG_ENABLE
#define DBG_LEVEL DBG_INFO
#define DBG_SECTION_NAME  "LSM6DSM"
#define DBG_COLOR
#include <rtdbg.h>
/* Private typedef ----------------------------------------------------------*/
static rt_size_t lsm6dsm_fetch_data(struct rt_sensor_device *sensor, void *buf, rt_size_t len);
static rt_err_t lsm6dsm_control(struct rt_sensor_device *sensor, int cmd, void *args);
/* Private define -----------------------------------------------------------*/

#ifndef PKG_LSM6DSM_I2C_ADDR_TYPE_LOW
#define PKG_LSM6DSM_I2C_ADDR_TYPE_LOW
#endif

#ifndef LSM6DSM_DEV_ADDR
#ifdef CONFIG_PKG_LSM6DSM_I2C_ADDR
    #define LSM6DSM_DEV_ADDR  CONFIG_PKG_LSM6DSM_I2C_ADDR
#else
    #ifdef PKG_LSM6DSM_I2C_ADDR_TYPE_LOW
        #define LSM6DSM_DEV_ADDR (LSM6DSM_I2C_ADD_L>>1)
    #else
        #define LSM6DSM_DEV_ADDR (LSM6DSM_I2C_ADD_H>>1)
    #endif
#endif
#endif

#define LSM_LOG(fmt,...)            LOG_D("%s %d: "fmt,__FUNCTION__,__LINE__,##__VA_ARGS__)
#define LSM_INFO                    LOG_I
#define LSM_WARN                    LOG_W
#define LSM_ERR                     LOG_E

#ifdef LOG_HEX
    #define LSM_DUMP(tag,mem,len)       LOG_HEX(tag,16,(rt_uint8_t *)(mem),len)
#else
    #define LSM_DUMP(tag,mem,len)
#endif

#define LSM6DSM_ODR_MIN                12
#define LSM6DSM_ODR_MAX                6666

#define LSM6DSM_ACCE_RANG_MIN          2000
#define LSM6DSM_ACCE_RANG_MAX          16000

#define LSM6DSM_GYRO_RANG_MIN          125
#define LSM6DSM_GYRO_RANG_MAX          2000

#define LSM6DSM_ODR_TO_HZ(odr) (((odr) == LSM6DSM_XL_ODR_12Hz5) ? 12.5f : \
                              ((odr) == LSM6DSM_XL_ODR_26Hz) ? 26.0f : \
                              ((odr) == LSM6DSM_XL_ODR_52Hz) ? 52.0f : \
                              ((odr) == LSM6DSM_XL_ODR_104Hz) ? 104.0f : \
                              ((odr) == LSM6DSM_XL_ODR_208Hz) ? 208.0f : \
                              ((odr) == LSM6DSM_XL_ODR_416Hz) ? 416.0f : \
                              ((odr) == LSM6DSM_XL_ODR_833Hz) ? 833.0f : \
                              ((odr) == LSM6DSM_XL_ODR_1k66Hz) ? 1660.0f : \
                              ((odr) == LSM6DSM_XL_ODR_3k33Hz) ? 3330.0f : \
                              ((odr) == LSM6DSM_XL_ODR_6k66Hz) ? 6660.0f : \
                              ((odr) == LSM6DSM_XL_ODR_1Hz6) ? 1.6f : 0 )

/* Private variables --------------------------------------------------------*/
static stmdev_ctx_t dev_ctx;
static struct rt_sensor_ops sensor_ops =
{
    lsm6dsm_fetch_data,
    lsm6dsm_control
};
/* Extern variables ---------------------------------------------------------*/
/* Private function prototypes ----------------------------------------------*/

static int write_reg(void *handle, uint8_t reg, uint8_t *bufp, uint16_t len)
{
    rt_uint8_t tmp = reg;
    struct rt_i2c_msg msgs[2];

    msgs[0].addr  = LSM6DSM_DEV_ADDR;             /* Slave address */
    msgs[0].flags = RT_I2C_WR;        /* Write flag */
    msgs[0].buf   = &tmp;             /* Slave register address */
    msgs[0].len   = 1;                /* Number of bytes sent */

    msgs[1].addr  = LSM6DSM_DEV_ADDR;             /* Slave address */
    msgs[1].flags = RT_I2C_WR | RT_I2C_NO_START;        /* Read flag */
    msgs[1].buf   = bufp;             /* Read data pointer */
    msgs[1].len   = len;              /* Number of bytes read */

    int ret = rt_i2c_transfer((struct rt_i2c_bus_device *)handle, msgs, 2);
    if (ret != 2)
    {
        LSM_ERR("I2C read error: %d\n", ret);
        return -RT_ERROR;
    }
    return RT_EOK;
}

static int read_reg(void *handle, uint8_t reg, uint8_t *bufp, uint16_t len)
{
    rt_uint8_t tmp = reg;
    
    struct rt_i2c_msg msgs[2];

    msgs[0].addr  = LSM6DSM_DEV_ADDR;             /* Slave address */
    msgs[0].flags = RT_I2C_WR;        /* Write flag */
    msgs[0].buf   = &tmp;             /* Slave register address */
    msgs[0].len   = 1;                /* Number of bytes sent */
    int ret = rt_i2c_transfer((struct rt_i2c_bus_device *)handle, msgs, 1);
    if (ret != 1)
    {
        LSM_ERR("[LSM_ERR] Write addr stage failed! reg: 0x%02x, ret: %d\n", reg, ret);
        return -RT_ERROR;
    }
    msgs[0].addr  = LSM6DSM_DEV_ADDR;
    msgs[0].flags = RT_I2C_RD;
    msgs[0].buf   = bufp;
    msgs[0].len   = len;
    ret = rt_i2c_transfer((struct rt_i2c_bus_device *)handle, msgs, 1);
    if (ret != 1)
    {
        LSM_ERR("[LSM_ERR] Read data stage failed! reg: 0x%02x, ret: %d\n", reg, ret);
        return -RT_ERROR;
    }

    return RT_EOK;
}

static lsm6dsm_odr_xl_t lsm6dsm_acce_convert_odr(float Odr)
{
    lsm6dsm_odr_xl_t new_odr;

    new_odr = (Odr <=   12.5f) ? LSM6DSM_XL_ODR_12Hz5
              : (Odr <=   26.0f) ? LSM6DSM_XL_ODR_26Hz
              : (Odr <=   52.0f) ? LSM6DSM_XL_ODR_52Hz
              : (Odr <=  104.0f) ? LSM6DSM_XL_ODR_104Hz
              : (Odr <=  208.0f) ? LSM6DSM_XL_ODR_208Hz
              : (Odr <=  416.0f) ? LSM6DSM_XL_ODR_416Hz
              : (Odr <=  833.0f) ? LSM6DSM_XL_ODR_833Hz
              : (Odr <= 1660.0f) ? LSM6DSM_XL_ODR_1k66Hz
              : (Odr <= 3330.0f) ? LSM6DSM_XL_ODR_3k33Hz
              :                    LSM6DSM_XL_ODR_6k66Hz;

    return new_odr;
}

static lsm6dsm_odr_g_t lsm6dsm_gyro_convert_odr(float Odr)
{
    lsm6dsm_odr_g_t new_odr;
    new_odr = (Odr <=   12.5f) ? LSM6DSM_GY_ODR_12Hz5
              : (Odr <=   26.0f) ? LSM6DSM_GY_ODR_26Hz
              : (Odr <=   52.0f) ? LSM6DSM_GY_ODR_52Hz
              : (Odr <=  104.0f) ? LSM6DSM_GY_ODR_104Hz
              : (Odr <=  208.0f) ? LSM6DSM_GY_ODR_208Hz
              : (Odr <=  416.0f) ? LSM6DSM_GY_ODR_416Hz
              : (Odr <=  833.0f) ? LSM6DSM_GY_ODR_833Hz
              : (Odr <= 1660.0f) ? LSM6DSM_GY_ODR_1k66Hz
              : (Odr <= 3330.0f) ? LSM6DSM_GY_ODR_3k33Hz
              :                    LSM6DSM_GY_ODR_6k66Hz;

    return new_odr;
}

static lsm6dsm_fs_xl_t lsm6dsm_acce_convert_fs(uint32_t rang)
{
    lsm6dsm_fs_xl_t new_fs;

    new_fs = (rang <=   2000) ? LSM6DSM_2g
             : (rang <=   4000) ? LSM6DSM_4g
             : (rang <=   8000) ? LSM6DSM_8g
             :                    LSM6DSM_16g;

    return new_fs;
}

static lsm6dsm_fs_g_t lsm6dsm_gyro_convert_fs(uint32_t rang)
{
    lsm6dsm_fs_g_t new_fs;
    new_fs = (rang <=   125) ? LSM6DSM_125dps
             : (rang <=   250) ? LSM6DSM_250dps
             : (rang <=   500) ? LSM6DSM_500dps
             : (rang <=  1000) ? LSM6DSM_1000dps
             :                    LSM6DSM_2000dps;

    return new_fs;
}

static int lsm6dsm_enable_step_counter(void)
{
    if (lsm6dsm_xl_data_rate_set(&dev_ctx, LSM6DSM_XL_ODR_26Hz))
    {
        return -RT_ERROR;
    }
    if (lsm6dsm_xl_full_scale_set(&dev_ctx, LSM6DSM_4g))
    {
        return -RT_ERROR;
    }
    if (lsm6dsm_pedo_threshold_set(&dev_ctx, 0x17))
    {
        return -RT_ERROR;
    }
    if (lsm6dsm_pedo_sens_set(&dev_ctx, PROPERTY_ENABLE))
    {
        return -RT_ERROR;
    }

    return RT_EOK;
}

static int lsm6dsm_disable_step_counter(float last_odr)
{
    lsm6dsm_pedo_sens_set(&dev_ctx, PROPERTY_DISABLE);
    lsm6dsm_pedo_threshold_set(&dev_ctx, 0x0);
    LSM_LOG("LSM6DSM step counter closed!");
    lsm6dsm_odr_xl_t new_odr = lsm6dsm_acce_convert_odr(last_odr);
    if (lsm6dsm_xl_data_rate_set(&dev_ctx, new_odr))
    {
        LSM_ERR("Set acce odr failed!");
        return -RT_ERROR;
    }
    return RT_EOK;
}

static rt_err_t lsm6dsm_init(struct rt_sensor_intf *intf)
{
    rt_uint8_t id;
    rt_uint32_t ts = 0;
    uint8_t rst = 1;
    struct rt_i2c_bus_device *i2c_bus_dev = (struct rt_i2c_bus_device *)rt_device_find(intf->dev_name);
    if (i2c_bus_dev == RT_NULL)
    {
        return -RT_ERROR;
    }

    dev_ctx.handle = i2c_bus_dev;
    dev_ctx.write_reg = write_reg;
    dev_ctx.read_reg = read_reg;

    if (lsm6dsm_device_id_get(&dev_ctx, &id) == RT_EOK)
    {
        if (id != LSM6DSM_ID)
        {
            LSM_WARN("This device(id=0x%X) is not LSM6DSM", id);
        }
    }
    //LSM_INFO("LSM6DSM ID: 0x%X", id);
    /* Restore default configuration */
    if (lsm6dsm_reset_set(&dev_ctx, PROPERTY_ENABLE) != RT_EOK)
    {
        LSM_ERR("Reset LSM6DSM failed");
        return -RT_ERROR;
    }
    ts = rt_tick_get_millisecond();
    do
    {
        lsm6dsm_reset_get(&dev_ctx, &rst);
        if ((rt_tick_get_millisecond() - ts) > 1000)
        {
            LSM_WARN("Reset LSM6DSM failed!");
            break;
        }
    }
    while (rst);
    rt_thread_mdelay(500);
    lsm6dsm_auto_increment_set(&dev_ctx, PROPERTY_ENABLE);
    /*  Enable Block Data Update */
    lsm6dsm_block_data_update_set(&dev_ctx, PROPERTY_ENABLE);
    lsm6dsm_fifo_mode_set(&dev_ctx, LSM6DSM_BYPASS_MODE);
    lsm6dsm_xl_data_rate_set(&dev_ctx, LSM6DSM_XL_ODR_OFF);
    lsm6dsm_gy_data_rate_set(&dev_ctx, LSM6DSM_GY_ODR_OFF);
    rt_thread_mdelay(50);

#ifdef PKG_LSM6DSM_USING_ACCE
    /* Set full scale */
    lsm6dsm_xl_full_scale_set(&dev_ctx, LSM6DSM_16g);

    /* Configure filtering chain(No aux interface)
     * Accelerometer - analog filter
     */
    lsm6dsm_xl_filter_analog_set(&dev_ctx, LSM6DSM_XL_ANA_BW_400Hz);
    /* Accelerometer - LPF1 path (LPF2 not used) */
    lsm6dsm_xl_lp1_bandwidth_set(&dev_ctx, LSM6DSM_XL_LP1_ODR_DIV_4);
    /* Accelerometer - LPF1 + LPF2 path */
    lsm6dsm_xl_lp2_bandwidth_set(&dev_ctx, LSM6DSM_XL_LOW_NOISE_LP_ODR_DIV_100);
#endif /*PKG_LSM6DSM_USING_ACCE*/

#ifdef PKG_LSM6DSM_USING_GYRO
    /* Set full scale */
    lsm6dsm_gy_full_scale_set(&dev_ctx, LSM6DSM_2000dps);
    lsm6dsm_gy_band_pass_set(&dev_ctx, LSM6DSM_HP_260mHz_LP1_STRONG);
#endif /*PKG_LSM6DSM_USING_GYRO*/

#ifdef PKG_LSM6DSM_USING_STEP

#endif /*PKG_LSM6DSM_USING_STEP*/
    return RT_EOK;
}

static rt_err_t lsm6dsm_set_range(rt_sensor_t sensor, rt_int32_t range)
{
    if (sensor->info.type == RT_SENSOR_CLASS_ACCE)
    {
        lsm6dsm_fs_xl_t fs = lsm6dsm_acce_convert_fs(range);
        if (lsm6dsm_xl_full_scale_set(&dev_ctx, fs))
        {
            LSM_ERR("Set acce full scale failed!");
            return -RT_ERROR;
        }
        LSM_LOG("set acce range %d; fs: %d", range, fs);
    }
    else if (sensor->info.type == RT_SENSOR_CLASS_GYRO)
    {
        lsm6dsm_fs_g_t fs = lsm6dsm_gyro_convert_fs(range);
        if (lsm6dsm_gy_full_scale_set(&dev_ctx, fs))
        {
            LSM_ERR("Set gyro full scale failed!");
            return -RT_ERROR;
        }
        LSM_LOG("set gyro range %d; fs: %d", range, fs);
    }

    return RT_EOK;
}

static rt_err_t lsm6dsm_set_odr(rt_sensor_t sensor, rt_uint16_t odr)
{
    if (sensor->info.type == RT_SENSOR_CLASS_ACCE)
    {
        lsm6dsm_odr_xl_t new_odr = lsm6dsm_acce_convert_odr((float)odr);
        if (lsm6dsm_xl_data_rate_set(&dev_ctx, new_odr))
        {
            LSM_ERR("Set acce odr failed!");
            return -RT_ERROR;
        }
        LSM_LOG("Set accelerometer odr %d|%4.2f", odr, LSM6DSM_ODR_TO_HZ(new_odr));
    }
    else if (sensor->info.type == RT_SENSOR_CLASS_GYRO)
    {
        lsm6dsm_odr_g_t new_odr = lsm6dsm_gyro_convert_odr((float)odr);
        if (lsm6dsm_gy_data_rate_set(&dev_ctx, new_odr))
        {
            LSM_ERR("Set gyro odr failed!");
            return -RT_ERROR;
        }
        LSM_LOG("Set gyroscop odr %d|%4.2f", odr, LSM6DSM_ODR_TO_HZ(new_odr));
    }

    return RT_EOK;
}

static rt_err_t lsm6dsm_set_mode(rt_sensor_t sensor, rt_uint8_t mode)
{
    rt_err_t ret = RT_EOK;
    switch (mode)
    {
    case RT_SENSOR_MODE_POLLING:
        lsm6dsm_fifo_mode_set(&dev_ctx, LSM6DSM_BYPASS_MODE);
        LSM_LOG("Set LSM6DSM to polling mode");
        break;
    case RT_SENSOR_MODE_INT:
    {
        LSM_LOG("Not support");
    }
    break;
    case RT_SENSOR_MODE_FIFO:
    {
        LSM_LOG("Not support");
    }
    break;
    case RT_SENSOR_MODE_NONE:
    default:
        break;
    }

    return ret;
}

static rt_err_t lsm6dsm_set_power(rt_sensor_t sensor, rt_uint8_t power)
{
    rt_err_t ret = RT_EOK;
    switch (power)
    {
    case RT_SENSOR_POWER_DOWN:
    {
        if (sensor->info.type == RT_SENSOR_CLASS_GYRO)
        {
            if (lsm6dsm_gy_data_rate_set(&dev_ctx, LSM6DSM_GY_ODR_OFF))
            {
                LSM_ERR("Close gyroscope failed!");
                ret = -RT_ERROR;
            }
            else
            {
                LSM_LOG("LSM6DSM gyroscope closed!");
            }
        }
        else
        {
            if (sensor->info.type == RT_SENSOR_CLASS_STEP)
            {
                if (lsm6dsm_disable_step_counter((float)sensor->config.odr))
                {
                    LSM_WARN("Close step counter failed!");
                }
                else
                {
                    LSM_LOG("LSM6DSM step counter closed!");
                }
            }
            else
            {
                LSM_LOG("LSM6DSM accelerometer closed!");
            }
            if (lsm6dsm_xl_data_rate_set(&dev_ctx, LSM6DSM_XL_ODR_OFF))
            {
                LSM_ERR("Close accelerometer failed!");
                ret = -RT_ERROR;
            }
        }
        break;
    }
    case RT_SENSOR_POWER_LOW:
    {
        if (sensor->info.type == RT_SENSOR_CLASS_GYRO)
        {
            if (lsm6dsm_gy_power_mode_set(&dev_ctx, LSM6DSM_GY_NORMAL))
            {
                LSM_ERR("Set gyro low power mode failed!");
                ret = -RT_ERROR;
                break;
            }

            if (lsm6dsm_gy_data_rate_set(&dev_ctx, LSM6DSM_GY_ODR_52Hz))
            {
                LSM_ERR("Set gyroscope low power odr failed!");
                ret = -RT_ERROR;
                break;
            }
            LSM_INFO("LSM6DSM gyro enter low power mode,set odr to 52Hz");
        }
        else
        {
            if (lsm6dsm_xl_power_mode_set(&dev_ctx, LSM6DSM_XL_NORMAL))
            {
                LSM_ERR("Set accelerometer low power mode failed!");
                ret = -RT_ERROR;
                break;
            }

            if (sensor->info.type == RT_SENSOR_CLASS_STEP)
            {
                if (lsm6dsm_enable_step_counter())
                {
                    LSM_ERR("Open step counter failed!");
                    ret = -RT_ERROR;
                    break;
                }
                LSM_INFO("LSM6DSM step enter low power mode,set ODR 26Hz!");
            }
            else
            {
                if (lsm6dsm_xl_data_rate_set(&dev_ctx, LSM6DSM_XL_ODR_52Hz))
                {
                    LSM_ERR("Set accelerometer odr failed!");
                    ret = -RT_ERROR;
                    break;
                }
                LSM_LOG("LSM6DSM accelerometer enter low power mode,Set acce odr 52Hz!");
            }
        }
        break;
    }
    case RT_SENSOR_POWER_NORMAL:
    {
        if (sensor->info.type == RT_SENSOR_CLASS_GYRO)
        {
            if (lsm6dsm_gy_power_mode_set(&dev_ctx, LSM6DSM_GY_NORMAL))
            {
                LSM_ERR("Set gyro normal power mode failed!");
                return -RT_ERROR;
            }
            lsm6dsm_odr_g_t new_odr = lsm6dsm_gyro_convert_odr((float)sensor->config.odr);
            if (lsm6dsm_gy_data_rate_set(&dev_ctx, new_odr))
            {
                LSM_ERR("Set gyro odr failed!");
                return -RT_ERROR;
            }
            LSM_INFO("LSM6DSM gyro enter normal power mode,ODR:%4.2f", LSM6DSM_ODR_TO_HZ(new_odr));
        }
        else
        {
            if (lsm6dsm_xl_power_mode_set(&dev_ctx, LSM6DSM_XL_NORMAL))
            {
                LSM_ERR("Set acce normal power mode failed!");
                ret = -RT_ERROR;
                break;
            }

            if (sensor->info.type == RT_SENSOR_CLASS_STEP)
            {
                if (lsm6dsm_enable_step_counter())
                {
                    LSM_ERR("Open step counter failed!");
                    ret = -RT_ERROR;
                    break;
                }
                LSM_INFO("LSM6DSM step enter normal power mode,acce ODR: 26Hz");
            }
            else
            {
                lsm6dsm_odr_xl_t new_odr = lsm6dsm_acce_convert_odr((float)sensor->config.odr);
                /* Set Output Data Rate for Acc */
                if (lsm6dsm_xl_data_rate_set(&dev_ctx, new_odr))
                {
                    LSM_ERR("Set acce odr(%d) failed!", new_odr);
                    ret = -RT_ERROR;
                    break;
                }
                LSM_INFO("LSM6DSM acce enter normal power mode,ODR:%4.2f", LSM6DSM_ODR_TO_HZ(new_odr));
            }
        }
        break;
    }
    case RT_SENSOR_POWER_HIGH:
    {
        if (sensor->info.type == RT_SENSOR_CLASS_GYRO)
        {
            if (lsm6dsm_gy_power_mode_set(&dev_ctx, LSM6DSM_GY_HIGH_PERFORMANCE))
            {
                LSM_ERR("Set gyro hig-performance power mode failed!");
                ret = -RT_ERROR;
                break;
            }
            lsm6dsm_odr_g_t new_odr = lsm6dsm_gyro_convert_odr((float)sensor->config.odr);
            if (lsm6dsm_gy_data_rate_set(&dev_ctx, new_odr))
            {
                LSM_ERR("Set gyro hig-performance power mode failed!");
                return -RT_ERROR;
                break;
            }
            LSM_LOG("LSM6DSM gyro enter hig-performance power mode,set ODR: %4.2f", LSM6DSM_ODR_TO_HZ(new_odr));
        }
        else
        {
            if (lsm6dsm_xl_power_mode_set(&dev_ctx, LSM6DSM_XL_HIGH_PERFORMANCE))
            {
                LSM_ERR("Set acce hig-performance mode failed!");
                return -RT_ERROR;
            }

            if (sensor->info.type == RT_SENSOR_CLASS_STEP)
            {
                if (lsm6dsm_enable_step_counter())
                {
                    LSM_ERR("Open step counter failed!");
                    ret = -RT_ERROR;
                    break;
                }
                LSM_LOG("LSM6DSM step enter hig-performance power mode,acce ODR: 26Hz");
            }
            else
            {
                lsm6dsm_odr_xl_t new_odr = lsm6dsm_acce_convert_odr((float)sensor->config.odr);
                /* Set Output Data Rate for Acc */
                if (lsm6dsm_xl_data_rate_set(&dev_ctx, new_odr))
                {
                    LSM_ERR("Set acce odr(%d) failed!", new_odr);
                    ret = -RT_ERROR;
                    break;
                }
                LSM_LOG("LSM6DSM accelerometer enter hig-performance power mode,set ODR: %4.2f", LSM6DSM_ODR_TO_HZ(new_odr));
            }
        }
        break;
    }
    default:
        break;
    }

    return ret;
}

static rt_size_t lsm6dsm_polling_fetch_data(rt_sensor_t sensor, struct rt_sensor_data *data)
{
    lsm6dsm_status_reg_t status;
    uint8_t ctrl4_reg = 0;
    int16_t raw_data[3] = {0};
    memset(data, 0, sizeof(struct rt_sensor_data));
    data->timestamp = rt_sensor_get_ts();
    /* Read output only if new value is available */
    if (lsm6dsm_status_reg_get(&dev_ctx, &status))
    {
        LSM_ERR("Get status failed!");
        return 0;
    }

    if (sensor->info.type == RT_SENSOR_CLASS_ACCE)
    {
        data->type = RT_SENSOR_CLASS_ACCE;
        lsm6dsm_fs_xl_t full_scale;
        if (!status.xlda)
        {
            LSM_WARN("Acce data is not ready!");
            return 0;
        }
        if (lsm6dsm_xl_full_scale_get(&dev_ctx, &full_scale))
        {
            LSM_ERR("Read full scale failed! ");
            return -RT_ERROR;
        }

        if (lsm6dsm_acceleration_raw_get(&dev_ctx, raw_data))
        {
            LSM_ERR("Read accelerometer data failed! ");
            return -RT_ERROR;
        }
        LSM_DUMP("raw_data", raw_data, 6);
        switch (full_scale)
        {
        case LSM6DSM_2g:
            data->data.acce.x = lsm6dsm_from_fs2g_to_mg(raw_data[0]);
            data->data.acce.y = lsm6dsm_from_fs2g_to_mg(raw_data[1]);
            data->data.acce.z = lsm6dsm_from_fs2g_to_mg(raw_data[2]);
            break;
        case LSM6DSM_4g:
            data->data.acce.x = lsm6dsm_from_fs4g_to_mg(raw_data[0]);
            data->data.acce.y = lsm6dsm_from_fs4g_to_mg(raw_data[1]);
            data->data.acce.z = lsm6dsm_from_fs4g_to_mg(raw_data[2]);
            break;
        case LSM6DSM_8g:
            data->data.acce.x = lsm6dsm_from_fs8g_to_mg(raw_data[0]);
            data->data.acce.y = lsm6dsm_from_fs8g_to_mg(raw_data[1]);
            data->data.acce.z = lsm6dsm_from_fs8g_to_mg(raw_data[2]);
            break;
        case LSM6DSM_16g:
            data->data.acce.x = lsm6dsm_from_fs16g_to_mg(raw_data[0]);
            data->data.acce.y = lsm6dsm_from_fs16g_to_mg(raw_data[1]);
            data->data.acce.z = lsm6dsm_from_fs16g_to_mg(raw_data[2]);
            break;
        default:
            break;
        }
        data->timestamp = rt_sensor_get_ts();
        return 1;
    }
    else if (sensor->info.type == RT_SENSOR_CLASS_GYRO)
    {
        lsm6dsm_fs_g_t full_scale;
        float real_gyro[3] = {0.0};
        data->type = RT_SENSOR_CLASS_GYRO;
        if (!status.gda)
        {
            LSM_WARN("Gyro data is not ready!");
            return 0;
        }
        if (lsm6dsm_gy_full_scale_get(&dev_ctx, &full_scale))
        {
            LSM_ERR("Read full scale failed! ");
            return -RT_ERROR;
        }

        if (lsm6dsm_angular_rate_raw_get(&dev_ctx, raw_data))
        {
            LSM_ERR("Read gyroscop data failed! ");
            return -RT_ERROR;
        }
        LSM_DUMP("raw_data", raw_data, 6);
        switch (full_scale)
        {
        case LSM6DSM_250dps:
            real_gyro[0] = lsm6dsm_from_fs250dps_to_mdps(raw_data[0]);
            real_gyro[1] = lsm6dsm_from_fs250dps_to_mdps(raw_data[1]);
            real_gyro[2] = lsm6dsm_from_fs250dps_to_mdps(raw_data[2]);
            break;
        case LSM6DSM_125dps:
            real_gyro[0] = lsm6dsm_from_fs125dps_to_mdps(raw_data[0]);
            real_gyro[1] = lsm6dsm_from_fs125dps_to_mdps(raw_data[1]);
            real_gyro[2] = lsm6dsm_from_fs125dps_to_mdps(raw_data[2]);
            break;
        case LSM6DSM_500dps:
            real_gyro[0] = lsm6dsm_from_fs500dps_to_mdps(raw_data[0]);
            real_gyro[1] = lsm6dsm_from_fs500dps_to_mdps(raw_data[1]);
            real_gyro[2] = lsm6dsm_from_fs500dps_to_mdps(raw_data[2]);
            break;
        case LSM6DSM_1000dps:
            real_gyro[0] = lsm6dsm_from_fs1000dps_to_mdps(raw_data[0]);
            real_gyro[1] = lsm6dsm_from_fs1000dps_to_mdps(raw_data[1]);
            real_gyro[2] = lsm6dsm_from_fs1000dps_to_mdps(raw_data[2]);
            break;
        case LSM6DSM_2000dps:
            real_gyro[0] = lsm6dsm_from_fs2000dps_to_mdps(raw_data[0]);
            real_gyro[1] = lsm6dsm_from_fs2000dps_to_mdps(raw_data[1]);
            real_gyro[2] = lsm6dsm_from_fs2000dps_to_mdps(raw_data[2]);
            break;
        default:
            break;
        }
        data->data.gyro.x = real_gyro[0] * 1000;
        data->data.gyro.y = real_gyro[1] * 1000;
        data->data.gyro.z = real_gyro[2] * 1000;
        data->timestamp = rt_sensor_get_ts();
        return 1;
    }
    else if (sensor->info.type == RT_SENSOR_CLASS_STEP)
    {
        uint16_t step = 0;
        data->type = RT_SENSOR_CLASS_STEP;
        if (lsm6dsm_read_reg(&dev_ctx, LSM6DSM_STEP_COUNTER_L, (uint8_t *)&step, 2) != RT_EOK)
        {
            LSM_ERR("Get step count failed!");
            return 0;
        }
        data->data.step = step;
        data->timestamp = rt_sensor_get_ts();
        return 1;
    }
    else if (sensor->info.type == RT_SENSOR_CLASS_TEMP)
    {
        data->type = RT_SENSOR_CLASS_TEMP;
        int16_t raw_temp = 0;
        if (status.tda)
        {
            if (lsm6dsm_temperature_raw_get(&dev_ctx, &raw_temp) != RT_EOK)
            {
                LSM_ERR("Read LSM6DSM temp failed!");
                return 0;
            }
            LSM_LOG("Raw temp: 0x%X", raw_temp);
            data->data.temp = lsm6dsm_from_lsb_to_celsius(raw_temp) * 10;
            data->timestamp = rt_sensor_get_ts();
            return 1;
        }
        else
        {
            LSM_WARN("Temp data is not ready!");
        }
    }
    else
    {
        LSM_ERR("Unsupported sensor type: %d", sensor->info.type);
    }
    return 0;
}

static rt_size_t lsm6dsm_fifo_fetch_data(rt_sensor_t sensor, struct rt_sensor_data *data, rt_size_t len)
{
    //TODO: add FIFO
    return 0;
}

static rt_size_t lsm6dsm_fetch_data(struct rt_sensor_device *sensor, void *buf, rt_size_t len)
{
    RT_ASSERT(buf);

    if (sensor->config.mode == RT_SENSOR_MODE_POLLING)
    {
        return lsm6dsm_polling_fetch_data(sensor, buf);
    }
    else if (sensor->config.mode == RT_SENSOR_MODE_INT)
    {
        return 0;
    }
    else if (sensor->config.mode == RT_SENSOR_MODE_FIFO)
    {
        return lsm6dsm_fifo_fetch_data(sensor, buf, len);
    }
    else
        return 0;
}

static rt_err_t lsm6dsm_control(struct rt_sensor_device *sensor, int cmd, void *args)
{
    rt_err_t result = RT_EOK;

    switch (cmd)
    {
    case RT_SENSOR_CTRL_GET_ID:
        result = lsm6dsm_device_id_get(&dev_ctx, args);
        break;
    case RT_SENSOR_CTRL_SET_RANGE:
        result = lsm6dsm_set_range(sensor, (rt_int32_t)args);
        break;
    case RT_SENSOR_CTRL_SET_ODR:
        result = lsm6dsm_set_odr(sensor, (rt_uint32_t)args & 0xffff);
        break;
    case RT_SENSOR_CTRL_SET_MODE:
        result = lsm6dsm_set_mode(sensor, (rt_uint32_t)args & 0xff);
        break;
    case RT_SENSOR_CTRL_SET_POWER:
        result = lsm6dsm_set_power(sensor, (rt_uint32_t)args & 0xff);
        break;
    case RT_SENSOR_CTRL_SELF_TEST:
        break;
    default:
        return -RT_ERROR;
    }
    return result;
}
/* Public function prototypes -----------------------------------------------*/
int rt_hw_lsm6dsm_init(const char *name, struct rt_sensor_config *cfg)
{
    int ret = RT_EOK;
    RT_ASSERT(cfg);

#ifdef PKG_LSM6DSM_USING_ACCE
    /* register LSM6DSM accelerometer sensor device*/
    static struct rt_sensor_device sensor_acce;
    rt_memset(&sensor_acce, 0, sizeof(struct rt_sensor_device));

    sensor_acce.info.type       = RT_SENSOR_CLASS_ACCE;
    sensor_acce.info.vendor     = RT_SENSOR_VENDOR_STM;
    sensor_acce.info.model      = "lsm6dsm_acc";
    sensor_acce.info.unit       = RT_SENSOR_UNIT_MGAUSS;
    sensor_acce.info.intf_type  = RT_SENSOR_INTF_I2C;
    sensor_acce.info.range_max  = LSM6DSM_ACCE_RANG_MAX;
    sensor_acce.info.range_min  = LSM6DSM_ACCE_RANG_MIN;
    sensor_acce.info.period_min = 10;
    sensor_acce.info.fifo_max = 0;
    if (cfg->range < LSM6DSM_ACCE_RANG_MIN)
    {
        cfg->range = LSM6DSM_ACCE_RANG_MIN;
    }
    cfg->odr = cfg->odr < LSM6DSM_ODR_MIN ? LSM6DSM_ODR_MIN : cfg->odr;
    rt_memcpy(&sensor_acce.config, cfg, sizeof(struct rt_sensor_config));
    sensor_acce.ops = &sensor_ops;
    ret = rt_hw_sensor_register(&sensor_acce, name, RT_DEVICE_FLAG_RDWR, RT_NULL);
    if (ret != RT_EOK)
    {
        LSM_ERR("Register LSM6DSM accelerometer sensor failed,err: %d", ret);
    }
#endif /*PKG_LSM6DSM_USING_ACCE*/
#ifdef PKG_LSM6DSM_USING_GYRO
    /*register LSM6DSM gyroscope sensor device*/
    static struct rt_sensor_device sensor_gyro;
    rt_memset(&sensor_gyro, 0, sizeof(struct rt_sensor_device));

    sensor_gyro.info.type       = RT_SENSOR_CLASS_GYRO;
    sensor_gyro.info.vendor     = RT_SENSOR_VENDOR_STM;
    sensor_gyro.info.model      = "lsm6dsm_gyro";
    sensor_gyro.info.unit       = RT_SENSOR_UNIT_MG;
    sensor_gyro.info.intf_type  = RT_SENSOR_INTF_I2C;
    sensor_gyro.info.range_max  = LSM6DSM_GYRO_RANG_MAX;
    sensor_gyro.info.range_min  = LSM6DSM_GYRO_RANG_MIN;
    sensor_gyro.info.period_min = 10;
    sensor_gyro.info.fifo_max = 0;
    if (cfg->range < LSM6DSM_GYRO_RANG_MIN)
    {
        cfg->range = LSM6DSM_GYRO_RANG_MIN;
    }
    cfg->odr = cfg->odr < LSM6DSM_ODR_MIN ? LSM6DSM_ODR_MIN : cfg->odr;
    rt_memcpy(&sensor_gyro.config, cfg, sizeof(struct rt_sensor_config));
    sensor_gyro.ops = &sensor_ops;

    ret = rt_hw_sensor_register(&sensor_gyro, name, RT_DEVICE_FLAG_RDWR, RT_NULL);
    if (ret != RT_EOK)
    {
        LSM_ERR("Register LSM6DSM gyroscope sensor failed,err: %d", ret);
    }
#endif /*PKG_LSM6DSM_USING_GYRO*/
#ifdef PKG_LSM6DSM_USING_STEP
    /* step sensor register */
    static struct rt_sensor_device sensor_step;
    rt_memset(&sensor_step, 0, sizeof(struct rt_sensor_device));

    sensor_step.info.type       = RT_SENSOR_CLASS_STEP;
    sensor_step.info.vendor     = RT_SENSOR_VENDOR_STM;
    sensor_step.info.model      = "lsm6dsm_step";
    sensor_step.info.unit       = RT_SENSOR_UNIT_ONE;
    sensor_step.info.intf_type  = RT_SENSOR_INTF_I2C;
    sensor_step.info.range_max  = 0;
    sensor_step.info.range_min  = 0;
    sensor_step.info.period_min = 10;
    sensor_step.info.fifo_max = 0;
    rt_memcpy(&sensor_step.config, cfg, sizeof(struct rt_sensor_config));
    sensor_step.ops = &sensor_ops;

    ret = rt_hw_sensor_register(&sensor_step, name, RT_DEVICE_FLAG_RDWR, RT_NULL);
    if (ret != RT_EOK)
    {
        LSM_ERR("Register LSM6DSM step count sensor failed,err: %d", ret);
    }
#endif /*PKG_LSM6DSM_USING_STEP*/

#ifdef PKG_LSM6DSM_USING_TEMP
    /* step sensor register */
    static struct rt_sensor_device sensor_temp;
    rt_memset(&sensor_temp, 0, sizeof(struct rt_sensor_device));

    sensor_temp.info.type       = RT_SENSOR_CLASS_TEMP;
    sensor_temp.info.vendor     = RT_SENSOR_VENDOR_STM;
    sensor_temp.info.model      = "lsm6dsm_temp";
    sensor_temp.info.unit       = RT_SENSOR_UNIT_DCELSIUS;
    sensor_temp.info.intf_type  = RT_SENSOR_INTF_I2C;
    sensor_temp.info.range_max  = 0;
    sensor_temp.info.range_min  = 0;
    sensor_temp.info.period_min = 10;
    sensor_temp.info.fifo_max = 0;
    rt_memcpy(&sensor_temp.config, cfg, sizeof(struct rt_sensor_config));
    sensor_temp.ops = &sensor_ops;

    ret = rt_hw_sensor_register(&sensor_temp, name, RT_DEVICE_FLAG_RDWR, RT_NULL);
    if (ret != RT_EOK)
    {
        LSM_ERR("Register LSM6DSM Temperature sensor failed,err: %d", ret);
    }
#endif /*PKG_LSM6DSM_USING_TEMP*/

    ret = lsm6dsm_init(&cfg->intf);
    if (ret != RT_EOK)
    {
        LSM_ERR("LSM6DSM init failed,err: %d", ret);
    }
    else
    {
        LSM_INFO("sensor init success");
    }

    return ret;
}

int lsm6dsm_device_init(void)
{
    struct rt_sensor_config lsm_cfg;
    rt_memset(&lsm_cfg, 0, sizeof(struct rt_sensor_config));
    int i2c_bus_index = LSM6DSM_DEV_I2C_BUS;
    char i2c_name[RT_NAME_MAX];
    snprintf(i2c_name, sizeof(i2c_name) - 1, "i2c%d", i2c_bus_index);
    lsm_cfg.intf.dev_name = i2c_name;
    
    lsm_cfg.intf.type = RT_SENSOR_INTF_I2C;
    lsm_cfg.mode = RT_SENSOR_MODE_POLLING;
    lsm_cfg.irq_pin.pin = RT_PIN_NONE; 

    rt_hw_lsm6dsm_init("lsm6dsm", &lsm_cfg);
    return 0;
}
#ifdef RT_USING_LSM6DSM
INIT_APP_EXPORT(lsm6dsm_device_init);
#endif