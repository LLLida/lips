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

  GC();

  TEST("(get (get cars :class) :red)");
  TEST("(get (get cars :class) :v)");
  TEST("(get (get cars :class) :bike)");
  TEST("(get (get cars :class) (quote booba))");

  GC();

  TEST("cars");
  TEST("(get cars :class)");

  TEST("(insert (get cars :class) :primes (cons 37 39))");
  TEST("(get (get cars :class) :primes)");
  TEST("(insert cars :lambda (lambda (name) (format \"Hello, %s!\" name)))");
  TEST("(get (intern \"cars\") :lambda)");

  Lips_DestroyMachine(machine);
  return 0;
}
