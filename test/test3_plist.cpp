#include "lips.h"

#include "util.h"

int main(int argc, char** argv)
{
  machine = Lips_DefaultCreateMachine();

  TEST("(plist)");

  Lips_DestroyMachine(machine);
  return 0;
}
