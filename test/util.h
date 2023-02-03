#pragma once
#include "stdio.h"

Lips_Machine* machine;

void TEST(const char* str)
{
  Lips_Cell cell = Lips_EvalString(machine, str, NULL);
  #define GREEN_COLOR "\x1b[32m"
  #define RED_COLOR "\x1b[31m"
  #define YELLOW_COLOR "\x1b[33m"
  #define RESET_COLOR "\x1b[0m"
  if (cell == NULL) {
    printf("test \"%s\" " RED_COLOR "failed" RESET_COLOR " with error " YELLOW_COLOR "\"%s\"" RESET_COLOR "\n", str, Lips_GetError(machine));
  } else {
    char buff[256];
    Lips_PrintCell(machine, cell, buff, sizeof(buff));
    printf("%s " GREEN_COLOR "->" RESET_COLOR " %s\n", str, buff);
  }
}

#define GC() printf("-----Garbage collection-----\n");  \
  Lips_GarbageCollect(machine)
