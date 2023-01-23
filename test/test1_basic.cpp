#include "stdio.h"
#include "lips.h"

Lips_Machine* machine;
#define ARRAY_SIZE(arr) sizeof(arr) / sizeof(arr[0])

LIPS_DECLARE_FUNCTION(my_function) {
  char buff[128];
  int n = 0;
  n += Lips_PrintCell(machine, args[0], buff+n, sizeof(buff)-n);
  n += Lips_PrintCell(machine, args[1], buff+n, sizeof(buff)-n);
  n += Lips_PrintCell(machine, args[2], buff+n, sizeof(buff)-n);
  printf("my_function: machine=%p udata=%p args=%s\n", machine, udata, buff);
  return Lips_NewList(machine, 3, args);
}

LIPS_DECLARE_FUNCTION(printf) {
  static char buffer[1024];
  Lips_Cell list = Lips_NewList(machine, numargs, args);
  uint32_t n = Lips_PrintCell(machine, list, buffer, sizeof(buffer));
  printf("Printf: (numargs=%u) %s\n", numargs, buffer);
  return Lips_NewStringN(machine, buffer, n);
}

int main(int argc, char** argv) {
  machine = Lips_DefaultCreateMachine();

  Lips_Cell num = Lips_NewInteger(machine, 65);
  printf("is integer: %d\n", Lips_IsInteger(num));
  char buff[256];
#define PRINT_CELL(cell, str) Lips_PrintCell(machine, cell, buff, sizeof(buff)); printf("%s=%s\n", str, buff)

  printf("printed %d chars to buffer\n", Lips_PrintCell(machine, num, buff, sizeof(buff)));
  printf("cell: %s\n",  buff);

  Lips_Cell real = Lips_NewReal(machine, 1.2);

  Lips_Cell str = Lips_NewString(machine, "hello world");

  Lips_Cell cells[] = { str, /*Lips_NewList(machine, 1, &real)*/real, num };
  Lips_Cell list = Lips_NewList(machine, sizeof(cells) / sizeof(Lips_Cell), cells);
  Lips_PrintCell(machine, list, buff, sizeof(buff));
  printf("cell: %s\n",  buff);

  Lips_Define(machine, "booba", Lips_NewInteger(machine, 42));
  Lips_Define(machine, "q", Lips_NewString(machine, "bye bye"));
  printf("booba=%p\n", Lips_Intern(machine, "booba"));
  printf("q=%s\n", Lips_GetString(machine, Lips_Intern(machine, "q")));

  Lips_Define(machine, "some-complicated-function", Lips_NewCFunction(machine, F_my_function, 3, NULL));
  LIPS_DEFINE_FUNCTION(machine, printf, LIPS_NUM_ARGS_1|LIPS_NUM_ARGS_VAR, NULL);
  Lips_Define(machine, "aboba", Lips_EvalString(machine, "(lambda (a b) (printf 3 (some-complicated-function 45.0 b \"muffin\") a 9))", NULL));

  Lips_Invoke(machine, Lips_NewSymbol(machine, "some-complicated-function"), list);

  PRINT_CELL(Lips_Intern(machine, "some-complicated-function"), "func");
  PRINT_CELL(Lips_Intern(machine, "macro"), "macro");
  PRINT_CELL(Lips_Intern(machine, "lambda"), "lambda");

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
    "(tail-macro 8 7 6 5 4 3 2 1)",
    "(define poker-face (lambda (arg) (throw arg)))",
    "(catch (poker-face 333) (quote error))",
    "()",
    "(catch (3 \"kitty\" 3.141592))",
    "(catch (lady gaga))",
    "(define catch2 (macro (...) (call (quote catch) ...)))",
    "(catch2 (car (list 2 3)) (throw \"food\") (list 73 73 73))",
    "(format \"The answer is %d\" 887)",
    "(format \"exception No. %d: %s\" 10.05 (catch (poker-face \"Welcome to Lisp\")))",
    "(format \"printing an s-expression: %S\" (list 65 (quote hooker) (tail-macro 12 34 56) ()))",
    "(format \"printing many integers: %d %d %d %d %d %d %d %d %d\" 1 2 3 4 5 6 7 8 9 )",
    "(format \"some-validation: %d\")",
    ":some-keyword",
    "(tail)",
    "(tail :a :m :o :n \"g us\")",
    "(define Hello \"Hello\")",
    "(concat Hello \" \" \"world\")",
    "(slurp \"CTestTestfile.cmake\")"
  };
  for (int i = 0; i < ARRAY_SIZE(test_strings); i++) {
    const char* test_string = test_strings[i];
    Lips_Cell evaluated_string = Lips_EvalString(machine, test_string, NULL);
    if (evaluated_string == NULL) {
      printf("Test %d: %s FAILED unhandled exception '%s'\n", i, test_string, Lips_GetError(machine));
    } else {
      Lips_PrintCell(machine, evaluated_string, buff, sizeof(buff));
      printf("Test %d: %s -> %s\n", i, test_string, buff);
    }
    if (i % 10 == 9) {
      Lips_GarbageCollect(machine);
    }
  }

  Lips_MemoryStats memoryStats;
  Lips_CalculateMemoryStats(machine, &memoryStats);
  printf("MemoryStats: \n\tallocated=%u\n\tcells=%u\n\tcells_used=%u\n\tstrings=%u\n\tstrings_used=%u\n",
         memoryStats.allocated_bytes,
         memoryStats.cell_allocated_bytes,
         memoryStats.cell_used_bytes,
         memoryStats.str_allocated_bytes,
         memoryStats.str_used_bytes);

  Lips_GarbageCollect(machine);
  printf("=======Garbage collection done=======\n");

  Lips_CalculateMemoryStats(machine, &memoryStats);
  printf("MemoryStats: \n\tallocated=%u\n\tcells=%u\n\tcells_used=%u\n\tstrings=%u\n\tstrings_used=%u\n",
         memoryStats.allocated_bytes,
         memoryStats.cell_allocated_bytes,
         memoryStats.cell_used_bytes,
         memoryStats.str_allocated_bytes,
         memoryStats.str_used_bytes);

  Lips_DestroyMachine(machine);
  return 0;
}
