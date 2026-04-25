#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

// ────────────────────────────────────────────
// TEST 1: basic parent <-> child co_yield flow
// ────────────────────────────────────────────
void test_basic(void) {
  printf("=== test_basic ===\n");

  int pid = fork();

  if (pid == 0) {
    // CHILD
    int parent_pid = getpid() - 1; // הנחה פשוטה — parent pid = child pid - 1
    // הילד מחכה לpid של האב; ישן עד שהאב יעיר אותו
    int ret = co_yield(parent_pid, 200);
    printf("child: co_yield returned %d (expected 100)\n", ret);
    if (ret != 100)
      printf("FAIL: child expected 100, got %d\n", ret);
    else
      printf("PASS: child\n");
    exit(0);
  } else {
    // PARENT
    // נותן לילד זמן להיכנס לco_yield קודם
    sleep(1);
    int ret = co_yield(pid, 100);
    printf("parent: co_yield returned %d (expected 200)\n", ret);
    if (ret != 200)
      printf("FAIL: parent expected 200, got %d\n", ret);
    else
      printf("PASS: parent\n");
    wait(0);
  }
}

// ────────────────────────────────────────────
// TEST 2: error — yield to non-existent PID
// ────────────────────────────────────────────
void test_invalid_pid(void) {
  printf("=== test_invalid_pid ===\n");
  int ret = co_yield(99999, 42);
  if (ret == -1)
    printf("PASS: got -1 for non-existent PID\n");
  else
    printf("FAIL: expected -1, got %d\n", ret);
}

// ────────────────────────────────────────────
// TEST 3: error — self-yield
// ────────────────────────────────────────────
void test_self_yield(void) {
  printf("=== test_self_yield ===\n");
  int my_pid = getpid();
  int ret = co_yield(my_pid, 42);
  if (ret == -1)
    printf("PASS: got -1 for self-yield\n");
  else
    printf("FAIL: expected -1, got %d\n", ret);
}

// ────────────────────────────────────────────
// TEST 4: error — yield to killed process
// ────────────────────────────────────────────
void test_killed_process(void) {
  printf("=== test_killed_process ===\n");

  int pid = fork();

  if (pid == 0) {
    // הילד רק מחכה ונרדם — האב יהרוג אותו
    sleep(100);
    exit(0);
  } else {
    // האב הורג את הילד ואז מנסה co_yield אליו
    kill(pid);
    sleep(1); // נותן לilד להיהרג
    int ret = co_yield(pid, 42);
    if (ret == -1)
      printf("PASS: got -1 for killed process\n");
    else
      printf("FAIL: expected -1, got %d\n", ret);
    wait(0);
  }
}

int main(void) {
  test_basic();
  test_invalid_pid();
  test_self_yield();
  test_killed_process();
  printf("=== all tests done ===\n");
  exit(0);
}
