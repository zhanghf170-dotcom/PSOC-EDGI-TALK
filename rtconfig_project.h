#ifndef RTCONFIG_PROJECT_H__
#define RTCONFIG_PROJECT_H__

/* Preserved across menuconfig / WriteRtconfigHeader regeneration:
 * Kconfig gates PKG_AHT10_I2C_BUS_NAME behind PKG_USING_AHT10_SAMPLE,
 * whereas applications/main.c uses PKG_AHT10_I2C_BUS_NAME directly
 * without building samples/aht10_sample.c.
 */
#ifndef PKG_AHT10_I2C_BUS_NAME
#define PKG_AHT10_I2C_BUS_NAME "i2c1"
#endif

#ifndef BSP_USING_SOFT_I2C2
#define BSP_USING_SOFT_I2C2
#endif
#ifndef BSP_SOFT_I2C2_SCL_PIN
#define BSP_SOFT_I2C2_SCL_PIN 105
#endif
#ifndef BSP_SOFT_I2C2_SDA_PIN
#define BSP_SOFT_I2C2_SDA_PIN 106
#endif

#ifndef WIFI_DEFAULT_SSID
#define WIFI_DEFAULT_SSID ""
#endif
#ifndef WIFI_DEFAULT_PWD
#define WIFI_DEFAULT_PWD ""
#endif

/* Keep full 16 KB TLS RX buffer for server cert chains, trim TX buffer to 8 KB
 * (still above MCP tools/list max 7.5 KB) to save 8 KB RAM per TLS session. */
#ifndef MBEDTLS_SSL_IN_CONTENT_LEN
#define MBEDTLS_SSL_IN_CONTENT_LEN 16384
#endif
#ifndef MBEDTLS_SSL_OUT_CONTENT_LEN
#define MBEDTLS_SSL_OUT_CONTENT_LEN 8192
#endif

#endif /* RTCONFIG_PROJECT_H__ */
