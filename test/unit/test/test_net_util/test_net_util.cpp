#include <unity.h>
#include <NetUtil.h>
#include <ctime>

using namespace Courier;

void setUp(void) {
    dnsFlushCountForTests = 0;
    systemClockForTests = 0;
}
void tearDown(void) {}

void test_flush_dns_increments_native_counter() {
    flushDnsCache();
    flushDnsCache();
    TEST_ASSERT_EQUAL(2, dnsFlushCountForTests);
}

void test_set_system_clock_records_epoch_natively() {
    setSystemClock((time_t)1771416000);
    TEST_ASSERT_EQUAL(1771416000, (long)systemClockForTests);
}

void test_parse_http_date_valid() {
    // 2026-02-18T12:00:00Z == 1771416000
    TEST_ASSERT_EQUAL(1771416000,
        (long)parseHttpDateToEpoch("Tue, 18 Feb 2026 12:00:00 GMT"));
}

void test_parse_http_date_epoch_boundary() {
    TEST_ASSERT_EQUAL(0, (long)parseHttpDateToEpoch("Thu, 01 Jan 1970 00:00:00 GMT"));
    // (epoch 0 parses to 0 which doubles as the failure value — acceptable:
    // a 1970 Date is never a clock we would set.)
    TEST_ASSERT_EQUAL(86400, (long)parseHttpDateToEpoch("Fri, 02 Jan 1970 00:00:00 GMT"));
}

void test_parse_http_date_rejects_garbage() {
    TEST_ASSERT_EQUAL(0, (long)parseHttpDateToEpoch(nullptr));
    TEST_ASSERT_EQUAL(0, (long)parseHttpDateToEpoch(""));
    TEST_ASSERT_EQUAL(0, (long)parseHttpDateToEpoch("not a date"));
    TEST_ASSERT_EQUAL(0, (long)parseHttpDateToEpoch("Tue, 18 Xxx 2026 12:00:00 GMT"));
}

void test_build_epoch_is_sane() {
    // The firmware was built after mid-2025 and before 2100.
    TEST_ASSERT_TRUE(buildEpoch() > (time_t)1750000000);
    TEST_ASSERT_TRUE(buildEpoch() < (time_t)4102444800);
}

int main(int argc, char** argv) {
    UNITY_BEGIN();
    RUN_TEST(test_flush_dns_increments_native_counter);
    RUN_TEST(test_set_system_clock_records_epoch_natively);
    RUN_TEST(test_parse_http_date_valid);
    RUN_TEST(test_parse_http_date_epoch_boundary);
    RUN_TEST(test_parse_http_date_rejects_garbage);
    RUN_TEST(test_build_epoch_is_sane);
    return UNITY_END();
}
