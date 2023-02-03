#include "lips.h"

#include "util.h"

int main(int argc, char** argv)
{
  machine = Lips_DefaultCreateMachine();

  TEST("(plist)");
  TEST("(define cars (plist :toyota \"Corona\" :wolksvagen \"Polo\" :lada 1))");
  TEST("cars");

  GC();

  TEST("(get cars :tesla)");
  TEST("(get cars :toyota)");
  TEST("(remove cars :wolksvagen)");
  TEST("(get cars :wolksvagen)");
  TEST("cars");

  // FIXME: if we replace one of the strings with 'car' then app will crash!
  // assertion in lisp.c:2342 will be triggered
  TEST("(insert cars :class (plist :v 7.8 :bike 2 :red (list (quote sym) \"Hi!\") (quote booba) :plane))");
  TEST("cars");
  TEST("(get cars :class)");
  TEST("(get (get cars :class) :red)");

  GC();

  TEST("cars");
  TEST("(get cars :class)");

  Lips_DestroyMachine(machine);
  return 0;
}
