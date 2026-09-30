#!/usr/bin/env python3
"""在主机上检查 SDK 中真实 CA 回调的内存所有权及异常清理。

先运行 tools/apply_idf_patches.py，然后设置 IDF_PATH 或传入 --idf-path。
也可以通过 --source 指向单独的 esp_crt_bundle.c 文件。
此测试不模拟密码学验证，不能代替 ESP-IDF 构建或设备 HTTPS 回归。
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest


SOURCE_PATH: Path | None = None


def extract_function(source: str, name: str) -> str:
    """提取 SDK 顶层函数，避免测试另一份手工复制的实现。"""
    match = re.search(
        rf"^static int {re.escape(name)}\([^{{]+\n\{{\n.*?^\}}$",
        source,
        flags=re.MULTILINE | re.DOTALL,
    )
    if match is None:
        raise AssertionError(f"SDK 中缺少预期函数 {name}；请先应用仓库补丁")
    return match.group(0)


# 只保留回调用到的字段；清理语义对应 v6.0.2 锁定的 Mbed TLS：
# espressif/mbedtls@6cc42afad309e861f4c07e6f106e2ab14a9cb8e5
# library/x509_crt.c:3203-3229。命名节点浅释放，raw 仅由 own_buffer 决定。
HARNESS_PREFIX = r"""
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MBEDTLS_PRIVATE(name) name
#define MBEDTLS_ASN1_CONSTRUCTED 0x20
#define MBEDTLS_ASN1_SEQUENCE 0x10
#define MBEDTLS_ERR_X509_FATAL_ERROR (-0x3000)
#define MBEDTLS_ERR_X509_ALLOC_FAILED (-0x3100)
#define KEY_PARSE_ERROR (-0x3200)
#define likely(value) (value)
#define unlikely(value) (value)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGD(...) ((void)0)

typedef struct {
    int tag;
    size_t len;
    unsigned char *p;
} mbedtls_asn1_buf;
typedef struct mbedtls_asn1_named_data {
    mbedtls_asn1_buf oid;
    mbedtls_asn1_buf val;
    struct mbedtls_asn1_named_data *next;
    unsigned char next_merged;
} mbedtls_asn1_named_data;
typedef struct { void *data; } mbedtls_pk_context;
typedef struct mbedtls_x509_crt {
    mbedtls_asn1_buf raw, issuer_raw, subject_raw;
    mbedtls_asn1_named_data issuer, subject;
    mbedtls_pk_context pk;
    int version;
    bool own_buffer, ca_istrue;
    struct mbedtls_x509_crt *next;
} mbedtls_x509_crt;
typedef const uint8_t *cert_t;

typedef struct {
    void *ptr;
    size_t size;
    bool mbedtls;
} allocation;
static allocation live[128];
static size_t live_count, live_bytes, allocation_attempts, fail_at;
static int parse_failure;

static void *tracked_calloc(size_t count, size_t size, bool mbedtls)
{
    allocation_attempts++;
    if (fail_at != 0 && allocation_attempts == fail_at) {
        return NULL;
    }
    void *ptr = calloc(count, size);
    assert(ptr != NULL);
    for (size_t i = 0; i < 128; i++) {
        if (live[i].ptr == NULL) {
            live[i] = (allocation){ptr, count * size, mbedtls};
            live_count++;
            live_bytes += count * size;
            return ptr;
        }
    }
    assert(!"too many live allocations");
    return NULL;
}

static void tracked_free(void *ptr, bool mbedtls)
{
    if (ptr == NULL) {
        return;
    }
    for (size_t i = 0; i < 128; i++) {
        if (live[i].ptr == ptr) {
            assert(live[i].mbedtls == mbedtls && "allocator mismatch");
            live_count--;
            live_bytes -= live[i].size;
            memset(ptr, 0xDD, live[i].size);
            live[i] = (allocation){0};
            free(ptr);
            return;
        }
    }
    assert(!"free of borrowed memory, or double free");
}

static void *mbedtls_calloc(size_t n, size_t s) { return tracked_calloc(n, s, true); }
static void mbedtls_free(void *p) { tracked_free(p, true); }
static void *c_calloc(size_t n, size_t s) { return tracked_calloc(n, s, false); }
static void c_free(void *p) { tracked_free(p, false); }

static const unsigned char bundle_name[] = "persistent bundle subject";
static const unsigned char bundle_key[] = "public key";
static const uint8_t *s_crt_bundle = bundle_name;
static bool lookup_matches = true;

static cert_t esp_crt_find_cert(const unsigned char *name, size_t size)
{
    assert(name == bundle_name && size == sizeof(bundle_name));
    return lookup_matches ? bundle_name : NULL;
}
static const uint8_t *esp_crt_get_name(cert_t cert) { assert(cert == bundle_name); return cert; }
static uint16_t esp_crt_get_name_len(cert_t cert) { assert(cert == bundle_name); return sizeof(bundle_name); }
static const uint8_t *esp_crt_get_key(cert_t cert) { assert(cert == bundle_name); return bundle_key; }
static uint16_t esp_crt_get_key_len(cert_t cert) { assert(cert == bundle_name); return sizeof(bundle_key); }

static void mbedtls_pk_init(mbedtls_pk_context *pk) { pk->data = NULL; }
static int mbedtls_pk_parse_public_key(mbedtls_pk_context *pk, const uint8_t *key, size_t size)
{
    assert(key == bundle_key && size == sizeof(bundle_key));
    if (parse_failure == 1) {
        return KEY_PARSE_ERROR;
    }
    pk->data = mbedtls_calloc(1, 17);
    if (pk->data == NULL || parse_failure == 2) {
        return KEY_PARSE_ERROR;
    }
    return 0;
}
static void mbedtls_pk_free(mbedtls_pk_context *pk)
{
    mbedtls_free(pk->data);
    pk->data = NULL;
}
static void mbedtls_x509_crt_init(mbedtls_x509_crt *crt)
{
    memset(crt, 0, sizeof(*crt));
}
static void free_named_data_shallow(mbedtls_asn1_named_data *node)
{
    while (node != NULL) {
        mbedtls_asn1_named_data *next = node->next;
        mbedtls_free(node);
        node = next;
    }
}
static void mbedtls_x509_crt_free(mbedtls_x509_crt *crt)
{
    mbedtls_x509_crt *current = crt;
    while (current != NULL) {
        mbedtls_pk_free(&current->pk);
        free_named_data_shallow(current->issuer.next);
        free_named_data_shallow(current->subject.next);
        if (current->raw.p != NULL && current->own_buffer) {
            mbedtls_free(current->raw.p);
        }
        mbedtls_x509_crt *previous = current;
        current = current->next;
        memset(previous, 0, sizeof(*previous));
        if (previous != crt) {
            mbedtls_free(previous);
        }
    }
}

/* 也跟踪标准分配器，保证回归测试能发现分配器混用。 */
#define calloc c_calloc
#define free c_free
"""

HARNESS_SUFFIX = r"""
#undef calloc
#undef free

#define MAX_NAMES 6
typedef struct {
    mbedtls_x509_crt crt;
    mbedtls_asn1_named_data nodes[MAX_NAMES - 1];
    unsigned char oids[MAX_NAMES][3];
    unsigned char values[MAX_NAMES][11];
} child_fixture;

static void prepare_child(child_fixture *fixture, size_t count)
{
    assert(count > 0 && count <= MAX_NAMES);
    memset(fixture, 0, sizeof(*fixture));
    fixture->crt.issuer_raw.p = (unsigned char *)bundle_name;
    fixture->crt.issuer_raw.len = sizeof(bundle_name);
    mbedtls_asn1_named_data *name = &fixture->crt.issuer;
    for (size_t i = 0; i < count; i++) {
        memset(fixture->oids[i], (int)i + 1, sizeof(fixture->oids[i]));
        memset(fixture->values[i], (int)i + 20, sizeof(fixture->values[i]));
        name->oid = (mbedtls_asn1_buf){6, sizeof(fixture->oids[i]), fixture->oids[i]};
        name->val = (mbedtls_asn1_buf){12, sizeof(fixture->values[i]), fixture->values[i]};
        name->next_merged = (unsigned char)(i % 2);
        if (i + 1 < count) {
            name->next = &fixture->nodes[i];
            name = name->next;
        }
    }
}

static void reset_tracking(void)
{
    assert(live_count == 0 && live_bytes == 0);
    allocation_attempts = fail_at = 0;
    parse_failure = 0;
    s_crt_bundle = bundle_name;
    lookup_matches = true;
}

static void check_candidate(const mbedtls_x509_crt *candidate, const child_fixture *fixture, size_t count)
{
    assert(candidate != NULL && candidate != &fixture->crt);
    assert(candidate->ca_istrue && candidate->version == 3);
    assert(candidate->subject_raw.p == bundle_name);
    assert(candidate->subject_raw.len == sizeof(bundle_name));
    assert(candidate->subject_raw.tag == (MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE));
    assert(!candidate->own_buffer && candidate->raw.p == NULL);
    assert(candidate->pk.data != NULL);
    const mbedtls_asn1_named_data *subject = &candidate->subject;
    const mbedtls_asn1_named_data *issuer = &fixture->crt.issuer;
    for (size_t i = 0; i < count; i++) {
        assert(subject != NULL && issuer != NULL && subject != issuer);
        assert(subject->oid.p == issuer->oid.p && subject->oid.len == issuer->oid.len);
        assert(subject->oid.tag == issuer->oid.tag);
        assert(subject->val.p == issuer->val.p && subject->val.len == issuer->val.len);
        assert(subject->val.tag == issuer->val.tag);
        assert(subject->next_merged == issuer->next_merged);
        subject = subject->next;
        issuer = issuer->next;
    }
    assert(subject == NULL && issuer == NULL);
}

static void release_candidate(mbedtls_x509_crt *candidate)
{
    /* Mbed TLS 在下一次 CA 回调前、以及验证退出时使用此顺序。 */
    mbedtls_x509_crt_free(candidate);
    mbedtls_free(candidate);
}

static void test_repeated_success(void)
{
    for (size_t names = 1; names <= MAX_NAMES; names++) {
        child_fixture fixture, snapshot;
        prepare_child(&fixture, names);
        memcpy(&snapshot, &fixture, sizeof(fixture));
        for (size_t iteration = 0; iteration < 1000; iteration++) {
            reset_tracking();
            mbedtls_x509_crt *candidate = NULL;
            assert(esp_crt_ca_cb_callback(NULL, &fixture.crt, &candidate) == 0);
            check_candidate(candidate, &fixture, names);
            /* 仅证书结构、公钥状态和后续名称节点需要独立分配。 */
            assert(allocation_attempts == names + 1);
            release_candidate(candidate);
            assert(live_count == 0 && live_bytes == 0);
            assert(memcmp(&fixture, &snapshot, sizeof(fixture)) == 0);
        }
    }
}

static void test_allocation_failures(void)
{
    child_fixture fixture, snapshot;
    prepare_child(&fixture, MAX_NAMES);
    memcpy(&snapshot, &fixture, sizeof(fixture));
    for (size_t failure = 1; failure <= MAX_NAMES + 1; failure++) {
        reset_tracking();
        fail_at = failure;
        mbedtls_x509_crt *candidate = NULL;
        int result = esp_crt_ca_cb_callback(NULL, &fixture.crt, &candidate);
        assert(result == (failure == 2 ? KEY_PARSE_ERROR : MBEDTLS_ERR_X509_ALLOC_FAILED));
        assert(candidate == NULL && allocation_attempts == failure);
        assert(live_count == 0 && live_bytes == 0);
        assert(memcmp(&fixture, &snapshot, sizeof(fixture)) == 0);
    }
    for (int failure = 1; failure <= 2; failure++) {
        reset_tracking();
        parse_failure = failure;
        mbedtls_x509_crt *candidate = NULL;
        assert(esp_crt_ca_cb_callback(NULL, &fixture.crt, &candidate) == KEY_PARSE_ERROR);
        assert(candidate == NULL && live_count == 0 && live_bytes == 0);
        assert(memcmp(&fixture, &snapshot, sizeof(fixture)) == 0);
    }
}

static void test_independent_candidates(void)
{
    child_fixture first_child, second_child;
    prepare_child(&first_child, 2);
    prepare_child(&second_child, 3);
    reset_tracking();
    mbedtls_x509_crt *first = NULL, *second = NULL;
    assert(esp_crt_ca_cb_callback(NULL, &first_child.crt, &first) == 0);
    assert(esp_crt_ca_cb_callback(NULL, &second_child.crt, &second) == 0);
    assert(first != second && first->subject.next != second->subject.next);
    release_candidate(first);
    check_candidate(second, &second_child, 3);
    release_candidate(second);
    assert(live_count == 0 && live_bytes == 0);
}

static void test_lookup_failures(void)
{
    child_fixture fixture;
    prepare_child(&fixture, 1);
    reset_tracking();
    lookup_matches = false;
    mbedtls_x509_crt *candidate = NULL;
    assert(esp_crt_ca_cb_callback(NULL, &fixture.crt, &candidate) == 0);
    assert(candidate == NULL && allocation_attempts == 0);
    s_crt_bundle = NULL;
    assert(esp_crt_ca_cb_callback(NULL, &fixture.crt, &candidate) == MBEDTLS_ERR_X509_FATAL_ERROR);
    assert(candidate == NULL && allocation_attempts == 0);
    assert(esp_crt_ref_asn1(NULL, &fixture.crt.subject) == -1);
    assert(esp_crt_ref_asn1(&fixture.crt.issuer, NULL) == -1);
}

int main(void)
{
    test_repeated_success();
    test_allocation_failures();
    test_independent_candidates();
    test_lookup_failures();
    puts("PASS: 6000 callback cycles, allocation and key-parse failures, borrowed buffers, allocator pairing");
    return 0;
}
"""


class CertificateBundleCallbackTests(unittest.TestCase):
    def test_callback_memory_ownership(self) -> None:
        source_path = SOURCE_PATH
        if source_path is None:
            idf_path = os.environ.get("IDF_PATH")
            if not idf_path:
                self.skipTest("需要已打补丁的 SDK：设置 IDF_PATH 或使用 --idf-path/--source")
            source_path = (
                Path(idf_path) / "components/mbedtls/esp_crt_bundle/esp_crt_bundle.c"
            )
        self.assertTrue(source_path.is_file(), f"SDK 源文件不存在: {source_path}")
        compiler = shlex.split(os.environ.get("CC", "cc"))
        self.assertTrue(compiler and shutil.which(compiler[0]), "需要主机 C 编译器（CC 或 cc）")
        source = source_path.read_text(encoding="utf-8")
        functions = "\n\n".join(
            extract_function(source, name)
            for name in ("esp_crt_ref_asn1", "esp_crt_ca_cb_callback")
        )
        with tempfile.TemporaryDirectory(prefix="crt-bundle-callback-") as directory:
            harness = Path(directory) / "callback.c"
            executable = Path(directory) / "callback"
            harness.write_text(HARNESS_PREFIX + functions + HARNESS_SUFFIX, encoding="utf-8")
            compile_result = subprocess.run(
                [
                    *compiler, "-std=c99", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-parameter", "-Wno-unused-function",
                    *shlex.split(os.environ.get("CRT_BUNDLE_TEST_CFLAGS", "")),
                    str(harness), "-o", str(executable),
                ],
                capture_output=True, text=True, check=False, timeout=60,
            )
            self.assertEqual(compile_result.returncode, 0, compile_result.stdout + compile_result.stderr)
            result = subprocess.run(
                [str(executable)], capture_output=True, text=True, check=False, timeout=30,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("PASS: 6000 callback cycles", result.stdout)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__, add_help=False)
    inputs = parser.add_mutually_exclusive_group()
    inputs.add_argument("--idf-path", type=Path)
    inputs.add_argument("--source", type=Path)
    args, unittest_args = parser.parse_known_args()
    if args.source:
        SOURCE_PATH = args.source
    elif args.idf_path:
        SOURCE_PATH = args.idf_path / "components/mbedtls/esp_crt_bundle/esp_crt_bundle.c"
    unittest.main(argv=[__file__, *unittest_args])
