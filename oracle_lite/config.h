#ifndef CONFIG_H
#define CONFIG_H

#define EPD_SPI_NUM        SPI2_HOST
#define ESP32_I2C_DEV_NUM  I2C_NUM_0

#define EPD_WIDTH  200
#define EPD_HEIGHT 200
#define LVGL_SPIRAM_BUFF_LEN (EPD_WIDTH * EPD_HEIGHT * 2)

/* EPD SPI pins */
#define EPD_DC_PIN    GPIO_NUM_10
#define EPD_CS_PIN    GPIO_NUM_11
#define EPD_SCK_PIN   GPIO_NUM_12
#define EPD_MOSI_PIN  GPIO_NUM_13
#define EPD_RST_PIN   GPIO_NUM_9
#define EPD_BUSY_PIN  GPIO_NUM_8


/* Power control pins */
#define EPD_PWR_PIN     GPIO_NUM_6
#define Audio_PWR_PIN   GPIO_NUM_42
#define VBAT_PWR_PIN    GPIO_NUM_17
#define BAT_ADC_PIN     4   // ADC1_CHANNEL_3 on ESP32-S3

#define BOOT_BUTTON_PIN GPIO_NUM_0
#define PWR_BUTTON_PIN  GPIO_NUM_18

/* Deep-sleep wake-up pin: the board's dedicated lower PWR button */
#define ext_wakeup_pin_1 GPIO_NUM_18

/* I2C bus */
#define ESP32_I2C_SDA_PIN GPIO_NUM_47
#define ESP32_I2C_SCL_PIN GPIO_NUM_48

/* LVGL tick timing */
#define EXAMPLE_LVGL_TICK_PERIOD_MS    5
#define EXAMPLE_LVGL_TASK_MAX_DELAY_MS 500
#define EXAMPLE_LVGL_TASK_MIN_DELAY_MS 100

/* I2C peripheral addresses */
#define I2C_RTC_DEV_Address        0x51
#define I2C_SHTC3_DEV_Address      0x70

/* Button GPIO aliases */
#define BTN_REC      0
#define BTN_PWR      18
#define PWR_HOLD_PIN 17

/* SD-MMC pins */
#define SD_CLK  39
#define SD_CMD  41
#define SD_D0   40

/* Audio */
#define SAMPLE_RATE  16000
#define REC_BUF      (8 * 1024)

/* Storage paths */
#define NOTES_DIR  "/notes"
#define INDEX_FILE "/notes/index.csv"

/* UI timing */
#define REC_HOLD_MS         320
#define ASK_DOUBLE_TAP_MS   360UL
#define COMPANION_HOLD_MS   3000UL
#define BTN_LONG_MS         600
#define BACKGROUND_IDLE_MS  8000UL
#define BACKGROUND_RETRY_MS 60000UL
#define AUTO_SLEEP_IDLE_MS  60000UL
#define AUTO_SLEEP_GRACE_MS 180000UL
#define COMPANION_IDLE_SLEEP_MS 300000UL // 5 minutes without HTTP/button activity
#define TICKER_INTERVAL_MS  950

/* Battery warning */
#define BAT_CHECK_INTERVAL_MS  30000
#define BAT_LOW_THRESHOLD      15
#define BAT_RECOVER_THRESHOLD  20

/* Time & firmware */
#ifndef ORACLE_PRIVATE_DIAGNOSTICS
#define ORACLE_PRIVATE_DIAGNOSTICS 0
#endif
// Device time zone (POSIX rule). Singapore Time, UTC+8, no daylight saving.
#define LOCAL_TZ_RULE          "<+08>-8"
#ifndef ORACLE_LITE_VERSION
#define ORACLE_LITE_VERSION "1.0.0-rc002"   // fallback only; PlatformIO uses version.txt
#endif
#define FIRMWARE_VERSION       "v" ORACLE_LITE_VERSION
#define FW_VERSION             "v" ORACLE_LITE_VERSION

#endif // CONFIG_H
