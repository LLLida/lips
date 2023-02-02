#pragma once
#include "stdio.h"

Lips_Machine* machine;

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

#define GC() printf("-----Garbage collection-----\n");  \
  Lips_GarbageCollect(machine)
