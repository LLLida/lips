#include "stdio.h"
#include "string.h"
#include "lips.h"

Lips_Interpreter* interp;

int main(int argc, char** argv) {
  interp = Lips_DefaultCreateInterpreter();
  printf("Lips REPL. Type 'quit' to quit.\n");
  char buff[256];
  Lips_Cell eval;
  while (true) {
    printf(">>> ");
    fgets(buff, sizeof(buff), stdin);
    if (strncmp(buff, "quit", 4) == 0) {
      break;
    }
    eval = Lips_EvalString(interp, buff, NULL);
    Lips_PrintCell(interp, eval, buff, sizeof(buff));
    printf("%s\n", buff);
  }
  Lips_DestroyInterpreter(interp);
  return 0;
}
