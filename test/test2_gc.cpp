#include "stdio.h"
#include "lips.h"

Lips_Machine* machine;

void TEST(const char* str);

int main(int argc, char** argv) {
  machine = Lips_DefaultCreateMachine();

  TEST("(define kitty \"hello\")");
  Lips_GarbageCollect(machine);
  TEST("kitty");

  TEST("(list kitty 35 333)");
  TEST("(typeof kitty)");

  TEST("(define def-pseudo (lambda () (define some-value 1707)))");
  TEST("(def-pseudo)");
  TEST("some-value");

  TEST("(define def-true (macro () (define some-value 1707)))");
  TEST("(def-true)");
  TEST("some-value");

  TEST("(progn (define such (quote a)) (define waste \"!\"))");
  TEST("(list (typeof such) (typeof kitty) (typeof waste))");

  Lips_DestroyMachine(machine);
  return 0;
}

void TEST(const char* str)
{
  Lips_Cell cell = Lips_EvalString(machine, str, NULL);
  if (cell == NULL) {
    printf("test \"%s\" failed with error \"%s\"\n", str, Lips_GetError(machine));
  } else {
    char buff[256];
    Lips_PrintCell(machine, cell, buff, sizeof(buff));
    printf("%s -> %s\n", str, buff);
  }
}