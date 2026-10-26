/* test.h - minimal unit test framework */

/**
 * 사용법:
 *   TEST(case_name) { CHECK(...); REQUIRE(...); }
 *
 * - TEST로 정의한 함수는 main 실행 전에 자동 등록된다 (constructor attribute).
 * - CHECK_*  : 실패를 기록하고 계속 진행한다.
 * - REQUIRE_*: 실패를 기록하고 현재 함수에서 return 한다. (void 함수 안에서만 사용)
 * - CMake가 소스에서 줄 맨 앞의 `TEST(name)`을 찾아 케이스별로 ctest에 등록한다.
 *   따라서 TEST(...)는 반드시 줄의 첫 글자부터 작성한다.
 */

#ifndef TEST_H
#define TEST_H

#include <stddef.h>
#include <stdint.h>

typedef void (*test_fn)(void);

void test_register(const char *name, const char *file, test_fn fn);
void test_fail(const char *file, int line, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
int  test_current_failed(void);

/* 함수 이름에 접두사를 붙여 테스트 이름과 대상 함수 이름(rb_init 등)이 충돌하지 않게 한다. */
#define TEST(name)                                                   \
  static void test__##name(void);                                    \
  __attribute__((constructor)) static void test__##name##__reg(void) \
  {                                                                  \
    test_register(#name, __FILE__, test__##name);                    \
  }                                                                  \
  static void test__##name(void)

/* ---------- boolean ---------- */

#define CHECK(cond)                                           \
  do {                                                        \
    if (!(cond))                                              \
      test_fail(__FILE__, __LINE__, "CHECK(%s)", #cond);      \
  } while (0)

#define REQUIRE(cond)                                         \
  do {                                                        \
    if (!(cond)) {                                            \
      test_fail(__FILE__, __LINE__, "REQUIRE(%s)", #cond);    \
      return;                                                 \
    }                                                         \
  } while (0)

/* 조건 없이 실패를 기록한다. 복잡한 검증 루프 안에서 사용. */
#define FAIL(msg) test_fail(__FILE__, __LINE__, "%s", (msg))

/* ---------- equality ---------- */

#define T_EQ_(T, fmt, a, b, on_fail)                                        \
  do {                                                                      \
    T t_a_ = (T)(a);                                                        \
    T t_b_ = (T)(b);                                                        \
    if (t_a_ != t_b_) {                                                     \
      test_fail(__FILE__, __LINE__, "%s == %s (" fmt " != " fmt ")",        \
                #a, #b, t_a_, t_b_);                                        \
      on_fail;                                                              \
    }                                                                       \
  } while (0)

#define CHECK_EQ_U(a, b)   T_EQ_(unsigned long long, "%llu", a, b, (void)0)
#define CHECK_EQ_I(a, b)   T_EQ_(long long, "%lld", a, b, (void)0)
#define CHECK_EQ_PTR(a, b) T_EQ_(const void *, "%p", a, b, (void)0)
#define REQUIRE_EQ_U(a, b) T_EQ_(unsigned long long, "%llu", a, b, return)
#define REQUIRE_EQ_I(a, b) T_EQ_(long long, "%lld", a, b, return)

/* 두 메모리 영역을 비교하고, 처음 다른 위치를 보고한다. */
#define CHECK_MEM(a, b, n)                                                    \
  do {                                                                        \
    const uint8_t *t_ma_ = (const uint8_t *)(a);                              \
    const uint8_t *t_mb_ = (const uint8_t *)(b);                              \
    size_t t_mn_ = (size_t)(n);                                               \
    for (size_t t_mi_ = 0; t_mi_ < t_mn_; t_mi_++) {                          \
      if (t_ma_[t_mi_] != t_mb_[t_mi_]) {                                     \
        test_fail(__FILE__, __LINE__, "%s[%zu] != %s[%zu] (0x%02x != 0x%02x)", \
                  #a, t_mi_, #b, t_mi_,                                       \
                  (unsigned)t_ma_[t_mi_], (unsigned)t_mb_[t_mi_]);            \
        break;                                                                \
      }                                                                       \
    }                                                                         \
  } while (0)

#endif  // TEST_H
