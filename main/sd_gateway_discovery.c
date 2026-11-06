#include "sdkconfig.h"
#include "sd_gateway_discovery.h"
#if CONFIG_NUWAX_SD_GATEWAY_DISCOVERY_ENABLE
#include "sd_gateway_discovery_wire.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "psa/crypto.h"
#include <stdio.h>
#include <unistd.h>

esp_err_t nuwax_sd_gateway_discover(char *url, size_t capacity, bool (*cancelled)(void *), void *ctx)
{
    if (!url || !capacity) return ESP_ERR_INVALID_ARG;
    url[0] = '\0';
    if (cancelled && cancelled(ctx)) return ESP_ERR_NOT_FINISHED;
    esp_netif_ip_info_t info;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif || esp_netif_get_ip_info(netif, &info) != ESP_OK || !info.ip.addr) return ESP_ERR_INVALID_STATE;
    uint8_t identity[16], secret[32], nonce[16], query[72], reply[128];
    if (!sd_discovery_hex(CONFIG_NUWAX_SD_GATEWAY_ID, identity, sizeof(identity)) ||
        !sd_discovery_hex(CONFIG_NUWAX_SD_GATEWAY_DISCOVERY_KEY, secret, sizeof(secret))) return ESP_ERR_INVALID_STATE;
    if (psa_crypto_init() != PSA_SUCCESS) return ESP_FAIL;
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_HMAC);
    psa_set_key_algorithm(&attributes, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_MESSAGE | PSA_KEY_USAGE_VERIFY_MESSAGE);
    mbedtls_svc_key_id_t key = 0;
    psa_status_t status = psa_import_key(&attributes, secret, sizeof(secret), &key);
    psa_reset_key_attributes(&attributes);
    memset(secret, 0, sizeof(secret));
    if (status != PSA_SUCCESS) return ESP_FAIL;
    esp_fill_random(nonce, sizeof(nonce)); sd_discovery_query(query, nonce, identity);
    size_t mac_bytes = 0;
    status = psa_mac_compute(key, PSA_ALG_HMAC(PSA_ALG_SHA_256), query, 40, query + 40, 32, &mac_bytes);
    if (status != PSA_SUCCESS || mac_bytes != 32) { psa_destroy_key(key); return ESP_FAIL; }
    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { psa_destroy_key(key); return ESP_ERR_NO_MEM; }
    esp_err_t result = ESP_ERR_TIMEOUT;
    int enabled = 1;
    struct timeval slice = { .tv_sec = 0, .tv_usec = 100000 };
    if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &enabled, sizeof(enabled)) != 0 ||
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &slice, sizeof(slice)) != 0) { result = ESP_FAIL; goto done; }
    struct sockaddr_in local = { .sin_family = AF_INET, .sin_addr.s_addr = info.ip.addr, .sin_port = 0 };
    if (bind(sock, (struct sockaddr *)&local, sizeof(local)) != 0) { result = ESP_FAIL; goto done; }
    const uint32_t subnet_broadcast = info.ip.addr | ~info.netmask.addr;
    uint32_t fallback_ip = 0;
    char fallback_host[16] = {0};
    if (sscanf(CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_TEST_URL, "ws://%15[0-9.]:", fallback_host) == 1)
        fallback_ip = inet_addr(fallback_host);
    const int64_t deadline = esp_timer_get_time() + 2000000;
    int64_t next_send = 0;
    while (esp_timer_get_time() < deadline) {
        if (cancelled && cancelled(ctx)) { result = ESP_ERR_NOT_FINISHED; break; }
        esp_netif_ip_info_t current;
        if (esp_netif_get_ip_info(netif, &current) != ESP_OK || current.ip.addr != info.ip.addr) {
            result = ESP_ERR_NOT_FINISHED; break;
        }
        const int64_t now = esp_timer_get_time();
        if (now >= next_send) {
            uint32_t destinations[] = { subnet_broadcast, INADDR_BROADCAST, fallback_ip };
            for (size_t i = 0; i < 3; ++i) {
                if (!destinations[i] || (i == 2 && ((destinations[i] & info.netmask.addr) != (info.ip.addr & info.netmask.addr)))) continue;
                struct sockaddr_in target = { .sin_family = AF_INET, .sin_port = htons(CONFIG_NUWAX_SD_GATEWAY_DISCOVERY_PORT), .sin_addr.s_addr = destinations[i] };
                (void)sendto(sock, query, sizeof(query), 0, (struct sockaddr *)&target, sizeof(target));
            }
            next_send = now + 500000;
        }
        struct sockaddr_in peer; socklen_t peer_len = sizeof(peer);
        const int received = recvfrom(sock, reply, sizeof(reply), 0, (struct sockaddr *)&peer, &peer_len);
        if (received < 0) continue;
        if (peer.sin_family != AF_INET || ntohs(peer.sin_port) != CONFIG_NUWAX_SD_GATEWAY_DISCOVERY_PORT ||
            (peer.sin_addr.s_addr & info.netmask.addr) != (info.ip.addr & info.netmask.addr)) continue;
        const uint16_t port = sd_discovery_reply_port(reply, (size_t)received, query);
        if (!port || psa_mac_verify(key, PSA_ALG_HMAC(PSA_ALG_SHA_256), reply, 44, reply + 44, 32) != PSA_SUCCESS) continue;
        char host[16];
        if (!inet_ntop(AF_INET, &peer.sin_addr, host, sizeof(host))) continue;
        const int count = snprintf(url, capacity, "ws://%s:%u/api/device/ws", host, (unsigned)port);
        result = count > 0 && (size_t)count < capacity ? ESP_OK : ESP_ERR_INVALID_SIZE;
        if (result == ESP_OK) ESP_LOGI("sd_gateway", "verified test gateway discovered url=%s", url);
        break;
    }
done:
    close(sock); psa_destroy_key(key);
    if (result == ESP_ERR_TIMEOUT) ESP_LOGW("sd_gateway", "test gateway discovery timed out; keep SD files; check same LAN and UDP port %u", CONFIG_NUWAX_SD_GATEWAY_DISCOVERY_PORT);
    return result;
}
#endif
