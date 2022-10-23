#include "stdio.h"
#include "lips.h"

Lips_Interpreter* interp;
#define ARRAY_SIZE(arr) sizeof(arr) / sizeof(arr[0])

LIPS_DECLARE_FUNCTION(my_function) {
  char buff[128];
  int n = 0;
  n += Lips_PrintCell(interp, args[0], buff+n, sizeof(buff)-n);
  n += Lips_PrintCell(interp, args[1], buff+n, sizeof(buff)-n);
  n += Lips_PrintCell(interp, args[2], buff+n, sizeof(buff)-n);
  printf("my_function: interp=%p udata=%p args=%s\n", interp, udata, buff);
  return Lips_NewList(interp, 3, args);
}

LIPS_DECLARE_FUNCTION(printf) {
  static char buffer[1024];
  Lips_Cell list = Lips_NewList(interp, numargs, args);
  uint32_t n = Lips_PrintCell(interp, list, buffer, sizeof(buffer));
  printf("Printf: (numargs=%u) %s\n", numargs, buffer);
  return Lips_NewStringN(interp, buffer, n);
}

int main(int argc, char** argv) {
  interp = Lips_DefaultCreateInterpreter();

  Lips_Cell num = Lips_NewInteger(interp, 65);
  printf("is integer: %d\n", Lips_IsInteger(num));
  char buff[256];
#define PRINT_CELL(cell, str) Lips_PrintCell(interp, cell, buff, sizeof(buff)); printf("%s=%s\n", str, buff)

  printf("printed %d chars to buffer\n", Lips_PrintCell(interp, num, buff, sizeof(buff)));
  printf("cell: %s\n",  buff);

  Lips_Cell real = Lips_NewReal(interp, 1.2);

  Lips_Cell str = Lips_NewString(interp, "hello world");

  Lips_Cell cells[] = { str, /*Lips_NewList(interp, 1, &real)*/real, num };
  Lips_Cell list = Lips_NewList(interp, sizeof(cells) / sizeof(Lips_Cell), cells);
  Lips_PrintCell(interp, list, buff, sizeof(buff));
  printf("cell: %s\n",  buff);

  Lips_Define(interp, "booba", Lips_NewInteger(interp, 42));
  Lips_Define(interp, "q", Lips_NewString(interp, "bye bye"));
  printf("booba=%p\n", Lips_Intern(interp, "booba"));
  printf("q=%s\n", Lips_GetString(interp, Lips_Intern(interp, "q")));

  Lips_Define(interp, "some-complicated-function", Lips_NewCFunction(interp, F_my_function, 3, NULL));
  LIPS_DEFINE_FUNCTION(interp, printf, LIPS_NUM_ARGS_1|LIPS_NUM_ARGS_VAR, NULL);
  Lips_Define(interp, "aboba", Lips_EvalString(interp, "(lambda (a b) (printf 3 (some-complicated-function 45.0 b \"muffin\") a 9))", NULL));

  Lips_Invoke(interp, Lips_Intern(interp, "some-complicated-function"), list);

  PRINT_CELL(Lips_Intern(interp, "some-complicated-function"), "func");
  PRINT_CELL(Lips_Intern(interp, "macro"), "macro");
  PRINT_CELL(Lips_Intern(interp, "lambda"), "lambda");

  Lips_Cell carry = Lips_EvalString(interp, "(lambda () (some-complicated-function \"1\" 3 9.09))", NULL);
  Lips_Invoke(interp, carry, Lips_Nil(interp));

  const char* test_strings[] = {
    "(printf \"Hello world\" 10 3.14 booba)",
    "(some-complicated-function \"%s\" \"hi\" 34)",
    // "(some-complicated-function 1 2 3 4)"
    "(printf \"%s\" (some-complicated-function 1 2 3 ) 42)",
    "(aboba 123 3.141592)",
    "(progn (define hello \"world\") (define number 42))",
    "(list hello number)",
    "(define func1 (lambda () (define some-value 1707)))",
    "(func1)",
    "some-value",
    "(define macro1 (macro () (define some-value 2408)))",
    "(macro1)",
    "some-value",
    "(progn (define hans 100000000) (define moke \"bzzzzz\"))",
    "(list (typeof hans) (typeof moke))",
    "(typeof lambda)",
    "(typeof (typeof (typeof typeof)))",
    "(catch (define kokin (quote bad)) (throw kokin) (list 1 2 3))",
    "(define tail (lambda (a ...) (progn ...)))",
    "(tail 1 2 3 4 5 6)",
    "(define tail-macro (macro (a ...) (progn ...)))",
    "(tail-macro 8 7 6 5 4 3 2 1)"
  };
  for (int i = 0; i < ARRAY_SIZE(test_strings); i++) {
    const char* test_string = test_strings[i];
    Lips_Cell evaluated_string = Lips_EvalString(interp, test_string, NULL);
    Lips_PrintCell(interp, evaluated_string, buff, sizeof(buff));
    printf("Test %d: %s -> %s\n", i, test_string, buff);
  }

  Lips_DestroyInterpreter(interp);
  return 0;
}
