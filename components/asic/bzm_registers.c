#include "bzm_registers.h"

const char * bzm_local_register_name(uint8_t offset)
{
    switch (offset) {
    case BZM_LOCAL_REG_RESULT_STATUS_CONTROL:
        return "result_status_control";
    case BZM_LOCAL_REG_UART_TDM_CONTROL:
        return "uart_tdm_control";
    case BZM_LOCAL_REG_SLOW_CLOCK_DIVIDER:
        return "slow_clock_divider";
    case BZM_LOCAL_REG_TDM_DELAY:
        return "tdm_delay";
    case BZM_LOCAL_REG_UART_TX:
        return "uart_tx";
    case BZM_LOCAL_REG_ASIC_ID:
        return "asic_id";
    case BZM_LOCAL_REG_PLL0_CONTROL:
        return "pll0_control";
    case BZM_LOCAL_REG_PLL0_ENABLE:
        return "pll0_enable";
    case BZM_LOCAL_REG_PLL0_MISC:
        return "pll0_misc";
    case BZM_LOCAL_REG_PLL1_CONTROL:
        return "pll1_control";
    case BZM_LOCAL_REG_PLL1_ENABLE:
        return "pll1_enable";
    case BZM_LOCAL_REG_PLL1_MISC:
        return "pll1_misc";
    case BZM_LOCAL_REG_DTS_RESET_POWERDOWN:
        return "dts_reset_powerdown";
    case BZM_LOCAL_REG_TEMPERATURE_TUNE_CODE:
        return "temperature_tune_code";
    case BZM_LOCAL_REG_THERMAL_TRIP_STATUS:
        return "thermal_trip_status";
    case BZM_LOCAL_REG_TEMPERATURE_CODE_STATUS:
        return "temperature_code_status";
    case BZM_LOCAL_REG_SENSOR_CLOCK_DIVIDER:
        return "sensor_clock_divider";
    case BZM_LOCAL_REG_VSENSOR_RESET_POWERDOWN:
        return "vsensor_reset_powerdown";
    case BZM_LOCAL_REG_VSENSOR_CONFIG:
        return "vsensor_config";
    case BZM_LOCAL_REG_VSENSOR_CONTROL:
        return "vsensor_control";
    case BZM_LOCAL_REG_VSENSOR_CH0_STATUS:
        return "vsensor_ch0_status";
    case BZM_LOCAL_REG_VSENSOR_CH1_CH2_STATUS:
        return "vsensor_ch1_ch2_status";
    case BZM_LOCAL_REG_BANDGAP:
        return "bandgap";
    case BZM_LOCAL_REG_IO_PEPS_DRIVE_STRENGTH:
        return "io_peps_drive_strength";
    case BZM_LOCAL_REG_DLL0_LOCK_0:
        return "dll0_lock_0";
    case BZM_LOCAL_REG_DLL0_LOCK_1:
        return "dll0_lock_1";
    case BZM_LOCAL_REG_DLL1_LOCK_0:
        return "dll1_lock_0";
    case BZM_LOCAL_REG_DLL1_LOCK_1:
        return "dll1_lock_1";
    default:
        return "unknown";
    }
}

size_t bzm_local_register_width(uint8_t offset)
{
    (void) offset;
    return BZM_CONTROL_REGISTER_WIDTH;
}
