/* Lips - tiny Lisp interpreter designed for being embedded in games
   See lips.c for more details.
 */
#ifndef INCLUDED_LIPS_H
#define INCLUDED_LIPS_H

#include "stdint.h"
#include "stdlib.h"

#ifdef __cplusplus
extern "C" {
#endif

/// attributes for optimizer
#ifdef __GNUC__
// https://gcc.gnu.org/onlinedocs/gcc/Common-Function-Attributes.html#Common-Function-Attributes
// means that function is pure, example:
// int a1 = s(10);
// int a2 = s(10);
// int a3 = s(10);
// will be optimised to:
// int a1 = s(10);
// int a2 = a1;
// int a3 = a2;
  #define LIPS_PURE_FUNCTION __attribute__((pure))
  #define LIPS_HOT_FUNCTION __attribute__((hot))
  #define LIPS_COLD_FUNCTION __attribute__((cold))
  #define LIPS_DEPRECATED_FUNCTION __attribute__((deprecated))
#else
  #define LIPS_PURE_FUNCTION
  #define LIPS_HOT_FUNCTION
  #define LIPS_COLD_FUNCTION
  #define LIPS_DEPRECATED_FUNCTION
#endif

typedef void*(*Lips_AllocFunc)(size_t bytes);
typedef void(*Lips_DeallocFunc)(void* ptr, size_t bytes);

typedef struct Lips_Interpreter Lips_Interpreter;

typedef struct Lips_Value Lips_Value;

typedef Lips_Value* Lips_Cell;

typedef Lips_Cell(*Lips_Func)(Lips_Interpreter* interp, Lips_Cell args, void* udata);

enum {
  // 64-bit integer
  LIPS_TYPE_INTEGER = 1<<0,
  // 64-bit real
  LIPS_TYPE_REAL = 1<<1,
  // variable-length string
  LIPS_TYPE_STRING = 1<<2,
  // symbol
  LIPS_TYPE_SYMBOL = 1<<3,
  // listx
  LIPS_TYPE_PAIR = 1<<4,
  // function defined in Lips code
  LIPS_TYPE_FUNCTION = 1<<5,
  // function defined in C code
  LIPS_TYPE_C_FUNCTION = (1<<5)+1,
  // macro defined in Lips code
  LIPS_TYPE_MACRO = 1<<6,
  // macro defined in C code
  LIPS_TYPE_C_MACRO = (1<<6)+1,
  // user-defined type
  LIPS_TYPE_USER = 1<<7,
};

enum {
  LIPS_NUM_ARGS_0 = 0,
  LIPS_NUM_ARGS_1 = 1,
  LIPS_NUM_ARGS_2 = 2,
  LIPS_NUM_ARGS_3 = 3,
  LIPS_NUM_ARGS_4 = 4,
  LIPS_NUM_ARGS_5 = 5,
  LIPS_NUM_ARGS_6 = 6,
  LIPS_NUM_ARGS_7 = 7,
  LIPS_NUM_ARGS_8 = 8,
  // example of function that accepts at least 2 arguments:
  // LIPS_NUM_ARGS_2|LIPS_NUM_ARGS_VAR
  LIPS_NUM_ARGS_VAR = 128
};

/* Create a Lisp interpreter.
   @param alloc function that will be used by interpreter for memory allocation
   @param dealloc function that will be used by interpreter for memory deallocation
 */
Lips_Interpreter* Lips_CreateInterpreter(Lips_AllocFunc alloc, Lips_DeallocFunc dealloc) LIPS_COLD_FUNCTION;
/* Create a Lisp interpreter with malloc() as alloc function and free() as dealloc function.
 */
Lips_Interpreter* Lips_DefaultCreateInterpreter() LIPS_COLD_FUNCTION;
/* Destroy an Lisp interpreter.
   Note: you don't have to call Lips_GarbageCollect before call to this function,
   it frees all resources by itself.
 */
void Lips_DestroyInterpreter(Lips_Interpreter* interpreter) LIPS_COLD_FUNCTION;
/* Evaluate AST
 */
Lips_Cell Lips_Eval(Lips_Interpreter* interpreter, Lips_Cell cell) LIPS_HOT_FUNCTION;
/* Evaluate a null-terminated string.
   filename can be null
 */
Lips_Cell Lips_EvalString(Lips_Interpreter* interpreter, const char* str, const char* filename);
/* Get string representing current error message
 */
const char* Lips_GetError(const Lips_Interpreter* interpreter);
/* Delete unused cells.
 */
void Lips_GarbageCollect(Lips_Interpreter* interpreter) LIPS_HOT_FUNCTION;

Lips_Cell Lips_Nil(Lips_Interpreter* interpreter);
/* Create Lisp integer value.
 */
Lips_Cell Lips_NewInteger(Lips_Interpreter* interpreter, int64_t num);
/* Create Lisp real number value.
 */
Lips_Cell Lips_NewReal(Lips_Interpreter* interpreter, double num);
/* Create Lisp string value.
 */
Lips_Cell Lips_NewString(Lips_Interpreter* interpreter, const char* str);
/* Create Lisp string value.
 */
Lips_Cell Lips_NewStringN(Lips_Interpreter* interpreter, const char* str, uint32_t n);
/* Create Lisp symbol.
 */
Lips_Cell Lips_NewSymbol(Lips_Interpreter* interpreter, const char* str);
/* Create Lisp symbol.
 */
Lips_Cell Lips_NewSymbolN(Lips_Interpreter* interpreter, const char* str, uint32_t n);
Lips_Cell Lips_NewPair(Lips_Interpreter* interpreter, Lips_Cell head, Lips_Cell tail);
Lips_Cell Lips_NewList(Lips_Interpreter* interpreter, uint32_t numCells, Lips_Cell* cells);
Lips_Cell Lips_NewFunction(Lips_Interpreter* interpreter, Lips_Cell args, Lips_Cell body, uint8_t numargs);
Lips_Cell Lips_NewMacro(Lips_Interpreter* interpreter, Lips_Cell args, Lips_Cell body, uint8_t numargs);
Lips_Cell Lips_NewCFunction(Lips_Interpreter* interpreter, Lips_Func function, uint8_t numargs, void* udata);
Lips_Cell Lips_NewCMacro(Lips_Interpreter* interpreter, Lips_Func function, uint8_t numargs, void* udata);
uint32_t Lips_GetType(const Lips_Cell cell) LIPS_PURE_FUNCTION;
int64_t Lips_GetInteger(Lips_Interpreter* interpreter, Lips_Cell cell);
double Lips_GetReal(Lips_Interpreter* interpreter, Lips_Cell cell);
const char* Lips_GetString(Lips_Interpreter* interpreter, Lips_Cell cell);
Lips_Cell Lips_CAR(Lips_Interpreter* interpreter, Lips_Cell cell);
Lips_Cell Lips_CDR(Lips_Interpreter* interpreter, Lips_Cell cell);
/* Print cell to a buffer. Number of occupied characters is returned
 */
uint32_t Lips_PrintCell(Lips_Interpreter* interpreter, Lips_Cell cell, char* buff, uint32_t size);
uint32_t Lips_ListLength(Lips_Interpreter* interpreter, Lips_Cell list);
Lips_Cell Lips_ListLastElement(Lips_Interpreter* interpreter, Lips_Cell list, uint32_t* length);
Lips_Cell Lips_ListPushBack(Lips_Interpreter* interpreter, Lips_Cell list, Lips_Cell elem);
Lips_Cell Lips_ListPopBack(Lips_Interpreter* interp, Lips_Cell list);
Lips_Cell Lips_Define(Lips_Interpreter* interpreter, const char* name, Lips_Cell cell);
Lips_Cell Lips_DefineCell(Lips_Interpreter* interpreter, Lips_Cell cell, Lips_Cell value);
Lips_Cell Lips_Intern(Lips_Interpreter* interpreter, const char* name);
Lips_Cell Lips_InternCell(Lips_Interpreter* interpreter, Lips_Cell cell) LIPS_HOT_FUNCTION;
Lips_Cell Lips_Invoke(Lips_Interpreter* interpreter, Lips_Cell callable, Lips_Cell args) LIPS_HOT_FUNCTION;

const char* Lips_SetError(Lips_Interpreter* interpreter, const char* fmt, ...);

#define Lips_IsInteger(cell) (Lips_GetType(cell) & LIPS_TYPE_INTEGER)
#define Lips_IsReal(cell) (Lips_GetType(cell) & LIPS_TYPE_REAL)
#define Lips_IsString(cell) (Lips_GetType(cell) & LIPS_TYPE_STRING)
#define Lips_IsSymbol(cell) (Lips_GetType(cell) & LIPS_TYPE_SYMBOL)
#define Lips_IsList(cell) (Lips_GetType(cell) & LIPS_TYPE_PAIR)
#define Lips_IsEnv(cell) (Lips_GetType(cell) & LIPS_TYPE_ENV)
#define Lips_IsFunction(cell) (Lips_GetType(cell) & LIPS_TYPE_FUNCTION)
#define Lips_IsMacro(cell) (Lips_GetType(cell) & LIPS_TYPE_MACRO)
#define Lips_ThrowError(interpreter, ...) do { \
    Lips_SetError(interpreter, ##__VA_ARGS__);   \
    assert(0);                                      \
  } while(0)

#ifdef __cplusplus
}
#endif

#endif