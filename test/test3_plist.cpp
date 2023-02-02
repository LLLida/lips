#include "lips.h"

#include "util.h"

int main(int argc, char** argv)
{
  machine = Lips_DefaultCreateMachine();

  TEST("(plist)");
  TEST("(define cars (plist :toyota \"Corona\" :wolksvagen \"Polo\" :lada 1))");
  TEST("cars");
  TEST("(get cars :tesla)");
  TEST("(get cars :toyota)");
  TEST("(remove cars :wolksvagen)");
  TEST("(get cars :wolksvagen)");
  TEST("cars");

  Lips_DestroyMachine(machine);
  return 0;
}
