/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "nuwax_ota_manifest.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

static const char *TAG = "nuwax_ota_manifest";

static bool numeric_core(const char *version);

static bool valid_semver_identifiers(const char *start, const char *end,
                                     bool prerelease)
{
    if (start == NULL || end == NULL || start >= end) {
        return false;
    }
    const char *part = start;
    while (part < end) {
        const char *next = part;
        bool numeric = true;
        while (next < end && *next != '.') {
            const unsigned char c = (unsigned char)*next;
            if (!(isalnum(c) || c == '-')) {
                return false;
            }
            numeric = numeric && isdigit(c);
            ++next;
        }
        const size_t len = (size_t)(next - part);
        if (len == 0U ||
            (prerelease && numeric && len > 1U && part[0] == '0')) {
            return false;
        }
        if (next == end) {
            return true;
        }
        part = next + 1;
    }
    return false;
}

static bool valid_semver(const char *version)
{
    if (version == NULL || version[0] == '\0') {
        return false;
    }
    const char *p = version;
    for (unsigned component = 0; component < 3U; ++component) {
        const char *start = p;
        while (isdigit((unsigned char)*p)) {
            ++p;
        }
        const size_t len = (size_t)(p - start);
        if (len == 0U || (len > 1U && start[0] == '0')) {
            return false;
        }
        if (component < 2U) {
            if (*p != '.') {
                return false;
            }
            ++p;
        }
    }

    if (*p == '-') {
        const char *start = ++p;
        while (*p != '\0' && *p != '+') {
            ++p;
        }
        if (!valid_semver_identifiers(start, p, true)) {
            return false;
        }
    }
    if (*p == '+') {
        const char *start = ++p;
        while (*p != '\0') {
            ++p;
        }
        if (!valid_semver_identifiers(start, p, false)) {
            return false;
        }
    }
    return *p == '\0';
}

static esp_err_t copy_required_string(const cJSON *node, char *dst,
                                      size_t dst_size)
{
    if (!cJSON_IsString(node) || node->valuestring == NULL ||
        node->valuestring[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    size_t len = strlen(node->valuestring);
    if (len >= dst_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(dst, node->valuestring, len + 1U);
    return ESP_OK;
}

static bool is_lower_hex_64(const char *text)
{
    if (text == NULL || strlen(text) != 64U) {
        return false;
    }
    for (size_t i = 0; i < 64U; ++i) {
        char c = text[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

static bool has_ascii_control_or_space(const char *text)
{
    for (const unsigned char *p = (const unsigned char *)text; *p != 0U; ++p) {
        if (*p <= 0x20U || *p == 0x7fU) {
            return true;
        }
    }
    return false;
}

static bool valid_update_id(const char *value)
{
    if (value == NULL || !isalnum((unsigned char)value[0])) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)value; *p != 0U; ++p) {
        if (!(isalnum(*p) || *p == '.' || *p == '-' || *p == '_' ||
              *p == '+' || *p == ':')) {
            return false;
        }
    }
    return true;
}

static unsigned json_key_count(const cJSON *object, const char *name)
{
    unsigned count = 0U;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, object) {
        if (item->string != NULL && strcmp(item->string, name) == 0) {
            ++count;
        }
    }
    return count;
}

static bool manifest_key_allowed(const char *name)
{
    static const char *const allowed[] = {
        "type", "command", "update_id", "url", "version",
        "size_bytes", "image_sha256",
    };
    if (name == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); ++i) {
        if (strcmp(name, allowed[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool manifest_envelope_valid(const cJSON *root)
{
    static const char *const required[] = {
        "type", "command", "update_id", "url", "version",
        "size_bytes", "image_sha256",
    };
    for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); ++i) {
        if (json_key_count(root, required[i]) != 1U) {
            return false;
        }
    }
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, root) {
        if (!manifest_key_allowed(item->string)) {
            return false;
        }
    }
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    const cJSON *command = cJSON_GetObjectItemCaseSensitive(root, "command");
    return cJSON_IsString(type) && type->valuestring != NULL &&
           strcmp(type->valuestring, "device_command") == 0 &&
           cJSON_IsString(command) && command->valuestring != NULL &&
           strcmp(command->valuestring, "ota_update") == 0;
}

esp_err_t nuwax_ota_manifest_parse(const char *json, size_t json_len,
                                   nuwax_ota_manifest_t *out)
{
    if (json == NULL || json_len == 0U || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    const char *parse_end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, json_len, &parse_end, false);
    if (root == NULL || !cJSON_IsObject(root)) {
        size_t offset = parse_end != NULL && parse_end >= json
                            ? (size_t)(parse_end - json)
                            : 0U;
        ESP_LOGE(TAG, "manifest JSON parse failed at offset %u",
                 (unsigned)offset);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    while (parse_end != NULL && parse_end < json + json_len &&
           isspace((unsigned char)*parse_end)) {
        ++parse_end;
    }
    if (parse_end == NULL || parse_end != json + json_len) {
        ESP_LOGE(TAG, "manifest contains trailing data");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    if (!manifest_envelope_valid(root)) {
        ESP_LOGE(TAG, "manifest envelope contains missing, duplicate, or unsupported fields");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = copy_required_string(
        cJSON_GetObjectItemCaseSensitive(root, "update_id"), out->update_id,
        sizeof(out->update_id));
    if (err == ESP_OK) {
        err = copy_required_string(
            cJSON_GetObjectItemCaseSensitive(root, "url"), out->url,
            sizeof(out->url));
    }
    if (err == ESP_OK) {
        err = copy_required_string(
            cJSON_GetObjectItemCaseSensitive(root, "version"), out->version,
            sizeof(out->version));
    }
    if (err == ESP_OK) {
        err = copy_required_string(
            cJSON_GetObjectItemCaseSensitive(root, "image_sha256"),
            out->image_sha256, sizeof(out->image_sha256));
    }

    const cJSON *size_node =
        cJSON_GetObjectItemCaseSensitive(root, "size_bytes");
    if (err == ESP_OK) {
        double size_value = cJSON_IsNumber(size_node)
                                ? size_node->valuedouble
                                : 0.0;
        if (!isfinite(size_value) || size_value < 1.0 ||
            size_value > (double)UINT32_MAX || floor(size_value) != size_value) {
            err = ESP_ERR_INVALID_SIZE;
        } else {
            out->size_bytes = (uint32_t)size_value;
        }
    }

    cJSON_Delete(root);

    if (err != ESP_OK) {
        memset(out, 0, sizeof(*out));
        ESP_LOGE(TAG, "manifest field parse failed: %s", esp_err_to_name(err));
        return err;
    }
    return nuwax_ota_manifest_validate(out);
}

esp_err_t nuwax_ota_manifest_validate(const nuwax_ota_manifest_t *manifest)
{
    if (manifest == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strncmp(manifest->url, "https://", 8U) != 0 ||
        manifest->url[8] == '\0') {
        ESP_LOGE(TAG, "manifest URL must be a non-empty HTTPS URL");
        return ESP_ERR_INVALID_ARG;
    }

    const char *authority = manifest->url + 8U;
    const char *authority_end = strpbrk(authority, "/?#");
    const size_t authority_len = authority_end != NULL
                                     ? (size_t)(authority_end - authority)
                                     : strlen(authority);
    if (authority_len == 0U || has_ascii_control_or_space(manifest->url) ||
        strchr(manifest->url, '#') != NULL ||
        strchr(manifest->url, '\\') != NULL ||
        memchr(authority, '@', authority_len) != NULL) {
        ESP_LOGE(TAG, "manifest URL must be a non-empty HTTPS URL");
        return ESP_ERR_INVALID_ARG;
    }
    if (!is_lower_hex_64(manifest->image_sha256)) {
        ESP_LOGE(TAG, "manifest SHA-256 must be 64 lowercase hex characters");
        return ESP_ERR_INVALID_CRC;
    }
    if (manifest->size_bytes == 0U ||
        manifest->size_bytes > NUWAX_OTA_MANIFEST_MAX_SIZE) {
        ESP_LOGE(TAG, "manifest size %lu is outside 1..%lu",
                 (unsigned long)manifest->size_bytes,
                 (unsigned long)NUWAX_OTA_MANIFEST_MAX_SIZE);
        return ESP_ERR_INVALID_SIZE;
    }
    if (!valid_semver(manifest->version)) {
        ESP_LOGE(TAG, "manifest version must be strict SemVer");
        return ESP_ERR_INVALID_ARG;
    }
    if (!valid_update_id(manifest->update_id)) {
        ESP_LOGE(TAG, "manifest update_id is invalid");
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static bool numeric_core(const char *version)
{
    if (version == NULL || !isdigit((unsigned char)version[0])) {
        return false;
    }
    bool need_digit = false;
    for (const unsigned char *p = (const unsigned char *)version;
         *p != 0U && *p != '-' && *p != '+'; ++p) {
        if (*p == '.') {
            if (need_digit) {
                return false;
            }
            need_digit = true;
        } else if (isdigit(*p)) {
            need_digit = false;
        } else {
            return false;
        }
    }
    return !need_digit;
}

static int compare_numeric_identifier(const char *a, size_t a_len,
                                      const char *b, size_t b_len)
{
    while (a_len > 1U && *a == '0') {
        ++a;
        --a_len;
    }
    while (b_len > 1U && *b == '0') {
        ++b;
        --b_len;
    }
    if (a_len != b_len) {
        return a_len < b_len ? -1 : 1;
    }
    const int rc = memcmp(a, b, a_len);
    return rc < 0 ? -1 : rc > 0 ? 1 : 0;
}

static bool identifier_is_numeric(const char *value, size_t len)
{
    if (len == 0U) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        if (!isdigit((unsigned char)value[i])) {
            return false;
        }
    }
    return true;
}

static int compare_text_identifier(const char *a, size_t a_len,
                                   const char *b, size_t b_len)
{
    const size_t shared = a_len < b_len ? a_len : b_len;
    const int rc = memcmp(a, b, shared);
    if (rc != 0) {
        return rc < 0 ? -1 : 1;
    }
    return a_len < b_len ? -1 : a_len > b_len ? 1 : 0;
}

static const char *core_end(const char *version)
{
    const char *end = version;
    while (*end != '\0' && *end != '-' && *end != '+') {
        ++end;
    }
    return end;
}

static int compare_numeric_core(const char *a, const char *b)
{
    const char *pa = a;
    const char *pb = b;
    const char *end_a = core_end(a);
    const char *end_b = core_end(b);
    static const char zero[] = "0";

    while (pa < end_a || pb < end_b) {
        const char *part_a = zero;
        const char *part_b = zero;
        size_t len_a = 1U;
        size_t len_b = 1U;
        if (pa < end_a) {
            part_a = pa;
            while (pa < end_a && *pa != '.') {
                ++pa;
            }
            len_a = (size_t)(pa - part_a);
            if (pa < end_a) {
                ++pa;
            }
        }
        if (pb < end_b) {
            part_b = pb;
            while (pb < end_b && *pb != '.') {
                ++pb;
            }
            len_b = (size_t)(pb - part_b);
            if (pb < end_b) {
                ++pb;
            }
        }
        const int rc = compare_numeric_identifier(part_a, len_a,
                                                  part_b, len_b);
        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}

static bool prerelease_span(const char *version, const char **start,
                            const char **end)
{
    const char *dash = strchr(version, '-');
    const char *plus = strchr(version, '+');
    if (dash == NULL || (plus != NULL && dash > plus)) {
        return false;
    }
    *start = dash + 1;
    *end = plus != NULL ? plus : version + strlen(version);
    return *start < *end;
}

static int compare_prerelease(const char *a, const char *b)
{
    const char *pa = NULL;
    const char *pb = NULL;
    const char *end_a = NULL;
    const char *end_b = NULL;
    const bool has_a = prerelease_span(a, &pa, &end_a);
    const bool has_b = prerelease_span(b, &pb, &end_b);
    if (has_a != has_b) {
        return has_a ? -1 : 1;
    }
    if (!has_a) {
        return 0;
    }

    while (pa < end_a && pb < end_b) {
        const char *part_a = pa;
        const char *part_b = pb;
        while (pa < end_a && *pa != '.') {
            ++pa;
        }
        while (pb < end_b && *pb != '.') {
            ++pb;
        }
        const size_t len_a = (size_t)(pa - part_a);
        const size_t len_b = (size_t)(pb - part_b);
        const bool numeric_a = identifier_is_numeric(part_a, len_a);
        const bool numeric_b = identifier_is_numeric(part_b, len_b);
        int rc;
        if (numeric_a && numeric_b) {
            rc = compare_numeric_identifier(part_a, len_a, part_b, len_b);
        } else if (numeric_a != numeric_b) {
            rc = numeric_a ? -1 : 1;
        } else {
            rc = compare_text_identifier(part_a, len_a, part_b, len_b);
        }
        if (rc != 0) {
            return rc;
        }
        if (pa < end_a) {
            ++pa;
        }
        if (pb < end_b) {
            ++pb;
        }
    }
    return pa < end_a ? 1 : pb < end_b ? -1 : 0;
}

int nuwax_ota_manifest_version_compare(const char *a, const char *b)
{
    if (a == NULL && b == NULL) {
        return 0;
    }
    if (a == NULL) {
        return -1;
    }
    if (b == NULL) {
        return 1;
    }
    if (!numeric_core(a) || !numeric_core(b)) {
        return strcmp(a, b);
    }
    const int core_rc = compare_numeric_core(a, b);
    return core_rc != 0 ? core_rc : compare_prerelease(a, b);
}

esp_err_t nuwax_ota_sha256_init(nuwax_ota_sha256_t *hash)
{
    if (hash == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(hash, 0, sizeof(*hash));
    hash->ctx = psa_hash_operation_init();
    if (psa_hash_setup(&hash->ctx, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        (void)psa_hash_abort(&hash->ctx);
        return ESP_FAIL;
    }
    hash->started = true;
    return ESP_OK;
}

esp_err_t nuwax_ota_sha256_update(nuwax_ota_sha256_t *hash,
                                  const uint8_t *data, size_t len)
{
    if (hash == NULL || !hash->started || (data == NULL && len != 0U)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len == 0U) {
        return ESP_OK;
    }
    return psa_hash_update(&hash->ctx, data, len) == PSA_SUCCESS
               ? ESP_OK
               : ESP_FAIL;
}

esp_err_t nuwax_ota_sha256_final(
    nuwax_ota_sha256_t *hash,
    char out_hex[NUWAX_OTA_MANIFEST_SHA256_LEN])
{
    if (hash == NULL || !hash->started || out_hex == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t digest[32];
    size_t digest_len = 0;
    psa_status_t status = psa_hash_finish(&hash->ctx, digest, sizeof(digest),
                                          &digest_len);
    hash->started = false;
    if (status != PSA_SUCCESS || digest_len != sizeof(digest)) {
        (void)psa_hash_abort(&hash->ctx);
        memset(digest, 0, sizeof(digest));
        return ESP_FAIL;
    }

    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(digest); ++i) {
        out_hex[i * 2U] = hex[digest[i] >> 4U];
        out_hex[i * 2U + 1U] = hex[digest[i] & 0x0fU];
    }
    out_hex[64] = '\0';
    memset(digest, 0, sizeof(digest));
    return ESP_OK;
}

void nuwax_ota_sha256_abort(nuwax_ota_sha256_t *hash)
{
    if (hash != NULL && hash->started) {
        (void)psa_hash_abort(&hash->ctx);
        hash->started = false;
    }
}
