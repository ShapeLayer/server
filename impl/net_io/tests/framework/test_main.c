/* test_main.c - test runner */

/**
 * ./test_ringbuf                 모든 케이스 실행
 * ./test_ringbuf write           이름에 "write"가 들어간 케이스만 실행
 * ./test_ringbuf --exact NAME    이름이 정확히 NAME인 케이스만 실행 (ctest가 사용)
 * ./test_ringbuf --list          등록된 케이스 이름 출력
 */

#include "framework/test.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_MAX_CASES 1024

typedef struct {
  const char *name;
  const char *file;
  test_fn     fn;
} test_case_t;

static test_case_t g_cases[TEST_MAX_CASES];
static int         g_count;
static int         g_current_failed;

void test_register(const char *name, const char *file, test_fn fn)
{
  if (g_count >= TEST_MAX_CASES) {
    fprintf(stderr, "too many test cases (max %d)\n", TEST_MAX_CASES);
    abort();
  }
  g_cases[g_count].name = name;
  g_cases[g_count].file = file;
  g_cases[g_count].fn   = fn;
  g_count++;
}

void test_fail(const char *file, int line, const char *fmt, ...)
{
  va_list ap;
  printf("    %s:%d: ", file, line);
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
  putchar('\n');
  g_current_failed = 1;
}

int test_current_failed(void) { return g_current_failed; }

static void usage(const char *argv0)
{
  printf("usage: %s [--list] [--exact NAME | FILTER]\n", argv0);
}

int main(int argc, char **argv)
{
  const char *filter = NULL;
  int         exact  = 0;
  int         list   = 0;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--list") == 0) {
      list = 1;
    } else if (strcmp(argv[i], "--exact") == 0 && i + 1 < argc) {
      exact  = 1;
      filter = argv[++i];
    } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      usage(argv[0]);
      return 0;
    } else if (argv[i][0] == '-') {
      usage(argv[0]);
      return 2;
    } else {
      filter = argv[i];
    }
  }

  /* ASan/UBSan 보고(stderr)와 출력 순서가 섞이지 않게 버퍼링을 끈다. */
  setvbuf(stdout, NULL, _IONBF, 0);

  if (list) {
    for (int i = 0; i < g_count; i++)
      printf("%s\n", g_cases[i].name);
    return 0;
  }

  int run = 0, failed = 0;
  for (int i = 0; i < g_count; i++) {
    const test_case_t *tc = &g_cases[i];
    if (filter) {
      int match = exact ? strcmp(tc->name, filter) == 0 : strstr(tc->name, filter) != NULL;
      if (!match)
        continue;
    }

    g_current_failed = 0;
    printf("[ RUN  ] %s\n", tc->name);
    tc->fn();
    printf("%s %s\n", g_current_failed ? "[ FAIL ]" : "[  OK  ]", tc->name);

    run++;
    failed += g_current_failed;
  }

  if (run == 0) {
    printf("no test case matched '%s'\n", filter ? filter : "");
    return 1;
  }

  printf("\n%d run, %d passed, %d failed\n", run, run - failed, failed);
  return failed ? 1 : 0;
}
