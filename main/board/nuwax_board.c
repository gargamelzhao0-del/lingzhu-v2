/* Board profile identity + startup banner (P1-1). */
#include "board/nuwax_board.h"
#include "board/esp_vocat_v1_0_pins.h"

#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "nuwax_board";

#if defined(CONFIG_NUWAX_DISPLAY_BL_ACTIVE_LOW)
#define NUWAX_BOARD_DISPLAY_BL_ACTIVE_LOW_VALUE 1
#else
#define NUWAX_BOARD_DISPLAY_BL_ACTIVE_LOW_VALUE 0
#endif

#if defined(CONFIG_NUWAX_DISPLAY_RST_ACTIVE_HIGH)
#define NUWAX_BOARD_DISPLAY_RST_ACTIVE_HIGH_VALUE 1
#else
#define NUWAX_BOARD_DISPLAY_RST_ACTIVE_HIGH_VALUE 0
#endif

const char *nuwax_board_name(void)
{
#if CONFIG_NUWAX_BOARD_ESP_VOCAT_V1_0
    return "Miaopan 2.0 (ESP32-S3-WROOM-1-N16R8)";
#else
    return "Legacy (pre-7.0 discrete mic/speaker)";
#endif
}

void nuwax_board_log_banner(void)
{
    ESP_LOGI(TAG, "board profile: %s", nuwax_board_name());
    /* Shared I2C bus (touch/IMU/gauge/codec) */
    ESP_LOGI(TAG, "  I2C: SCL=GPIO%d SDA=GPIO%d (touch shares bus)",
             CONFIG_NUWAX_TOUCH_PIN_SCL, CONFIG_NUWAX_TOUCH_PIN_SDA);
#if CONFIG_NUWAX_DISPLAY_QSPI_ENABLE
    ESP_LOGI(TAG, "  LCD QSPI: SCK=%d D0=%d D1=%d D2=%d D3=%d CS=%d RST=%d(reset_active_high=%d) BL=%d(active_low=%d)",
             CONFIG_NUWAX_DISPLAY_PIN_SCK, CONFIG_NUWAX_DISPLAY_PIN_MOSI,
             CONFIG_NUWAX_DISPLAY_PIN_DATA1, CONFIG_NUWAX_DISPLAY_PIN_DATA2,
             CONFIG_NUWAX_DISPLAY_PIN_DATA3, CONFIG_NUWAX_DISPLAY_PIN_CS,
             CONFIG_NUWAX_DISPLAY_PIN_RST,
             NUWAX_BOARD_DISPLAY_RST_ACTIVE_HIGH_VALUE,
             CONFIG_NUWAX_DISPLAY_PIN_BL,
             NUWAX_BOARD_DISPLAY_BL_ACTIVE_LOW_VALUE);
#else
    ESP_LOGI(TAG, "  LCD SPI: SCK=%d MOSI=%d DC=%d CS=%d RST=%d(reset_active_high=%d) BL=%d(active_low=%d)",
             CONFIG_NUWAX_DISPLAY_PIN_SCK, CONFIG_NUWAX_DISPLAY_PIN_MOSI,
             CONFIG_NUWAX_DISPLAY_PIN_DC, CONFIG_NUWAX_DISPLAY_PIN_CS,
             CONFIG_NUWAX_DISPLAY_PIN_RST,
             NUWAX_BOARD_DISPLAY_RST_ACTIVE_HIGH_VALUE,
             CONFIG_NUWAX_DISPLAY_PIN_BL,
             NUWAX_BOARD_DISPLAY_BL_ACTIVE_LOW_VALUE);
#endif
    /* Touch */
    ESP_LOGI(TAG, "  TOUCH: INT=GPIO%d RST=GPIO%d",
             CONFIG_NUWAX_TOUCH_PIN_INT, CONFIG_NUWAX_TOUCH_PIN_RST);
#if CONFIG_NUWAX_BOARD_ESP_VOCAT_V1_0
    /* Miaopan 2.0 pin map derived from the updated schematic (2026-05-08). */
    ESP_LOGI(TAG, "  TOUCH_WAKE: GPIO%d (independent input; not TP_INT/TP_RST)",
             NUWAX_VOCAT_TOUCH_WAKE_GPIO);
    ESP_LOGI(TAG, "  AUDIO: MCLK=%d BCK=%d WS=%d DOUT=%d DIN=%d PA_CTRL=%d CODEC_PWR=%d",
             NUWAX_VOCAT_I2S_MCLK_GPIO, NUWAX_VOCAT_I2S_BCK_GPIO,
             NUWAX_VOCAT_I2S_WS_GPIO, NUWAX_VOCAT_I2S_DOUT_GPIO,
             NUWAX_VOCAT_I2S_DIN_GPIO, NUWAX_VOCAT_PA_CTRL_GPIO,
             NUWAX_VOCAT_CODEC_PWR_GPIO);
    ESP_LOGI(TAG, "  STRAP pin assignments: GPIO0=%d GPIO3=%d GPIO45=%d GPIO46=%d",
              NUWAX_VOCAT_STRAP_BOOT_GPIO,
              NUWAX_VOCAT_STRAP_GPIO3,
              NUWAX_VOCAT_STRAP_GPIO45,
              NUWAX_VOCAT_STRAP_LCD_D3_GPIO);
    ESP_LOGI(TAG, "  POWER_HOLD=GPIO%d (PG2, active-high; SW3 startup latch)",
             NUWAX_VOCAT_POWER_CTRL_GPIO);
    /* P3-0: frozen slot semantics; hardware AEC remains health-gated. */
    ESP_LOGI(TAG, "  SLOT: mic_primary=slot0/MIC1 secondary=slot1/MIC2 "
             "aec_ref_candidate=slot2/MIC3 unused=slot3/MIC4");
    ESP_LOGI(TAG, "  AFE target input_format=MR (slot0=M, slot2=R) only after ref_health passes");
#else
    ESP_LOGI(TAG, "  MIC I2S: BCK=%d WS=%d DIN=%d | SPEAKER I2S: BCK=%d WS=%d DOUT=%d SD=%d",
             CONFIG_NUWAX_MIC_I2S_BCLK_GPIO, CONFIG_NUWAX_MIC_I2S_WS_GPIO,
             CONFIG_NUWAX_MIC_I2S_DIN_GPIO, CONFIG_NUWAX_SPEAKER_I2S_BCLK_GPIO,
             CONFIG_NUWAX_SPEAKER_I2S_WS_GPIO, CONFIG_NUWAX_SPEAKER_I2S_DOUT_GPIO,
             CONFIG_NUWAX_SPEAKER_SD_GPIO);
#endif
}
