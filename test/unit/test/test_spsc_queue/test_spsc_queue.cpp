#include <unity.h>
#include "CourierSpscQueue.h"

void setUp(void) {}
void tearDown(void) {}

void test_empty_on_construction() {
    CourierSpscQueue<int, 4> q;
    TEST_ASSERT_TRUE(q.empty());
    int out = 99;
    TEST_ASSERT_FALSE(q.pop(out));
    TEST_ASSERT_EQUAL(99, out);
}

void test_push_pop_preserves_fifo_order() {
    CourierSpscQueue<int, 4> q;
    TEST_ASSERT_TRUE(q.push(1));
    TEST_ASSERT_TRUE(q.push(2));
    TEST_ASSERT_TRUE(q.push(3));
    int out;
    TEST_ASSERT_TRUE(q.pop(out)); TEST_ASSERT_EQUAL(1, out);
    TEST_ASSERT_TRUE(q.pop(out)); TEST_ASSERT_EQUAL(2, out);
    TEST_ASSERT_TRUE(q.pop(out)); TEST_ASSERT_EQUAL(3, out);
    TEST_ASSERT_FALSE(q.pop(out));
    TEST_ASSERT_TRUE(q.empty());
}

void test_push_returns_false_when_full() {
    CourierSpscQueue<int, 3> q;
    TEST_ASSERT_TRUE(q.push(10));
    TEST_ASSERT_TRUE(q.push(20));
    TEST_ASSERT_TRUE(q.push(30));
    TEST_ASSERT_FALSE(q.push(40));
    int out;
    TEST_ASSERT_TRUE(q.pop(out)); TEST_ASSERT_EQUAL(10, out);
    TEST_ASSERT_TRUE(q.push(40));
    TEST_ASSERT_TRUE(q.pop(out)); TEST_ASSERT_EQUAL(20, out);
    TEST_ASSERT_TRUE(q.pop(out)); TEST_ASSERT_EQUAL(30, out);
    TEST_ASSERT_TRUE(q.pop(out)); TEST_ASSERT_EQUAL(40, out);
}

void test_wraparound() {
    CourierSpscQueue<int, 3> q;
    int out;
    for (int i = 0; i < 50; ++i) {
        TEST_ASSERT_TRUE(q.push(i));
        TEST_ASSERT_TRUE(q.pop(out));
        TEST_ASSERT_EQUAL(i, out);
    }
    TEST_ASSERT_TRUE(q.empty());
}

void test_capacity_constant() {
    TEST_ASSERT_EQUAL(8u, (CourierSpscQueue<int, 8>::capacity()));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_empty_on_construction);
    RUN_TEST(test_push_pop_preserves_fifo_order);
    RUN_TEST(test_push_returns_false_when_full);
    RUN_TEST(test_wraparound);
    RUN_TEST(test_capacity_constant);
    return UNITY_END();
}
