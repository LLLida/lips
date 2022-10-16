/* Lips - tiny Lisp interpreter designed for being embedded in games
 */
#include "assert.h"
#include "setjmp.h"
#include "stdarg.h"
#include "stdio.h"
#include "string.h"

#include "lips.h"

/// CONSTANTS

// number of values in one bucket
#ifndef LIPS_BUCKET_SIZE
#define LIPS_BUCKET_SIZE 1024
#endif

// number buckets that Interpreter allocates by default
#ifndef LIPS_NUM_DEFAULT_BUCKETS
#define LIPS_NUM_DEFAULT_BUCKETS 16
#endif

/// LIST OF STRUCTS

typedef struct Bucket Bucket;
typedef struct Iterator Iterator;
typedef struct HashTable HashTable;
typedef struct Stack Stack;
typedef struct Node Node;
typedef struct Parser Parser;
typedef struct StringData StringData;
typedef struct Token Token;
typedef struct EvalState EvalState;

/// MACROS

#define LIPS_EOF (-1)
#define LIPS_DEAD_MASK (1<<31)
#define LIPS_IS_DEAD(cell) ((cell).type & LIPS_DEAD_MASK)
#define LIPS_IS_WHITESPACE(c) ((c) == ' ' || (c) == '\n' || (c) == '\t' || (c) == '\r')
#define LIPS_IS_SPECIAL_CHAR(c) ((c) == '(' || (c) == ')' || (c) == '\'' || (c) == '`')
#define LIPS_IS_DIGIT(c) ((c) >= '0' && (c) <= '9')
#define LIPS_LOG_ERROR(interpreter, ...) snprintf(interpreter->errbuff, sizeof(interpreter->errbuff), __VA_ARGS__)
#define LIPS_TYPE_CHECK(interpreter, type, cell) if (!(LIPS_GET_TYPE(cell) & (type))) Lips_ThrowError(interpreter, "Typecheck failed (%d & %d)", LIPS_GET_TYPE(cell), type);
#define LIPS_STR(cell) (cell->data.str)
#define LIPS_STR_PTR(str) (str->ptr)
#define LIPS_STR_LEN(str) (str->length)

/// LIST OF FUNCTIONS

static void* DefaultAlloc(size_t bytes);
static void DefaultDealloc(void* ptr, size_t bytes);

static Lips_Cell NewCell(Lips_Interpreter* interp) LIPS_HOT_FUNCTION;
static void DestroyCell(Lips_Cell cell, Lips_DeallocFunc dealloc);

static StringData* StringCreate(Lips_AllocFunc alloc);
static void StringAllocate(StringData* str, Lips_AllocFunc alloc, const char* ptr, uint32_t n);
static void StringDestroy(StringData* str, Lips_DeallocFunc dealloc);
static void StringSet(Lips_AllocFunc alloc, StringData* str, uint32_t index, char c);
static StringData* StringCopy(StringData* src);

static void ParserInit(Parser* parser, const char* str, uint32_t len);
static int ParserNextToken(Parser* parser);
static int Lips_IsTokenNumber(const Token* token);
static Lips_Cell ParseNumber(Lips_Interpreter* interp, const Token* token);
static Lips_Cell GenerateAST(Lips_Interpreter* interp, Parser* parser);

static void CreateBucket(Lips_AllocFunc alloc, Bucket* bucket);
static void DestroyBucket(Lips_DeallocFunc dealloc, Bucket* bucket);
static Lips_Cell BucketNewCell(Bucket* bucket);
static void BucketDeleteCell(Bucket* bucket, Lips_Cell cell);

static void CreateStack(Lips_AllocFunc alloc, Stack* stack, uint32_t size);
static void DestroyStack(Lips_DeallocFunc dealloc, Stack* stack);
static void* StackRequire(Lips_AllocFunc alloc, Lips_DeallocFunc dealloc,
                               Stack* stack, uint32_t bytes);
static void StackRelease(Stack* stack, uint32_t bytes);

static HashTable* InterpreterEnv(Lips_Interpreter* interpreter);
static HashTable* PushEnv(Lips_Interpreter* interpreter);
static void PopEnv(Lips_Interpreter* interpreter);
static HashTable* EnvParent(Lips_Interpreter* interpreter, HashTable* env);
static uint32_t ComputeHash(const char* string) LIPS_PURE_FUNCTION;
static HashTable* HashTableCreate(Lips_AllocFunc alloc, Lips_DeallocFunc dealloc,
                                  Stack* stack);
static void HashTableDestroy(Stack* stack, HashTable* ht);
static void HashTableReserve(Lips_AllocFunc alloc, Lips_DeallocFunc dealloc,
                                  Stack* stack, HashTable* ht, uint32_t capacity);
static Lips_Cell* HashTableInsert(Lips_AllocFunc alloc, Lips_DeallocFunc dealloc, Stack* stack,
                                       HashTable* ht, const char* key, Lips_Cell value);
static Lips_Cell* HashTableInsertWithHash(Lips_AllocFunc alloc, Lips_DeallocFunc dealloc, Stack* stack,
                                               HashTable* ht, uint32_t hash,
                                               const char* key, Lips_Cell value);
static Lips_Cell* HashTableSearch(const HashTable* ht, const char* key);
static Lips_Cell* HashTableSearchWithHash(const HashTable* ht, uint32_t hash, const char* key);
static void HashTableIterate(HashTable* ht, Iterator* it);
static int IteratorIsEmpty(const Iterator* it);
static void IteratorGet(const Iterator* it, const char** key, Lips_Cell* value);
static void IteratorNext(Iterator* it);

static uint32_t CheckArgumentCount(Lips_Interpreter* interpreter, Lips_Cell callable, Lips_Cell args);
static Lips_Cell EvalNonPair(Lips_Interpreter* interpreter, Lips_Cell cell);

static Lips_Cell M_lambda(Lips_Interpreter* interpreter, Lips_Cell args, void* udata);
static Lips_Cell M_macro(Lips_Interpreter* interpreter, Lips_Cell args, void* udata);

/// STRUCTS

// Copy-On-Write string
struct StringData {
  uint32_t length;
  uint32_t counter;
  uint32_t hash;
  char* ptr;
};

struct HashTable {
  uint32_t allocated;
  uint32_t size;
  uint32_t parent;
};
#define LIPS_HASH_TABLE_DATA(ht) (Node*)((HashTable*)ht+1)

struct Iterator {
  Node* node;
  uint32_t size;
};

struct Stack {
  uint8_t* data;
  uint32_t offset;
  uint32_t size;
};

struct Lips_Value {
  // 0-7 bits - type
  // 8-14 bits - numargs if function
  // 15 bit - variable number of arguments if function
  // 31 bit - garbage collector mark
  uint32_t type;
  union {
    int64_t integer;
    double real;
    StringData* str;
    struct {
      Lips_Value* head;
      Lips_Value* tail;
    } list;
    struct {
      Lips_Value* args;
      Lips_Value* body;
    } lfunc;
    struct {
      Lips_Func ptr;
      void* udata;
    } cfunc;
  } data;
};
#define LIPS_GET_TYPE(cell) ((cell)->type & 255)
#define LIPS_GET_NUMARGS(cell) (((cell)->type >> 8) & 255)
#define LIPS_GET_INTEGER(cell) (cell)->data.integer
#define LIPS_GET_REAL(cell) (cell)->data.real
#define LIPS_GET_STRING(cell) (cell)->data.str->ptr
#define LIPS_GET_HEAD(cell) (cell)->data.list.head
#define LIPS_GET_TAIL(cell) (cell)->data.list.tail
#define LIPS_GET_HEAD_TYPE(cell) LIPS_GET_TYPE(LIPS_GET_HEAD(cell))
#define LIPS_GET_TAIL_TYPE(cell) LIPS_GET_TYPE(LIPS_GET_TAIL(cell))

struct Node {
  const char* key;
  Lips_Cell value;
  uint32_t hash;
};
#define LIPS_NODE_VALID(node) ((node).value != NULL)

struct Bucket {
  Lips_Value* data;
  uint32_t size;
  uint32_t next;
};

struct Token {
  const char* str;
  uint32_t length;
};

struct EvalState {
  Lips_Cell sexp;
  Lips_Cell callable;
  Lips_Cell args;
  Lips_Cell passed_args;
  union {
    Lips_Cell last;
    Lips_Cell code;
  } data;
  HashTable* env;
  int stage;
};

struct Parser {
  const char* text;
  uint32_t length;
  uint32_t pos;
  uint32_t currline;
  uint32_t currcol;
  uint32_t numlists;
  Token currtok;
};

struct Lips_Interpreter {
  Lips_AllocFunc alloc;
  Lips_DeallocFunc dealloc;
  Bucket* buckets;
  uint32_t numbuckets;
  uint32_t allocbuckets;
  Stack stack;
  uint32_t envpos;
  jmp_buf jmp; // this is used for exceptions
  Lips_Cell S_nil;
  Lips_Cell S_filename;
  char errbuff[1024];
};

/// FUNCTIONS

LIPS_COLD_FUNCTION Lips_Interpreter*
Lips_CreateInterpreter(Lips_AllocFunc alloc, Lips_DeallocFunc dealloc)
{
  Lips_Interpreter* interp;
  interp = (Lips_Interpreter*)alloc(sizeof(Lips_Interpreter));
  if (interp == NULL) return NULL;
  interp->alloc = alloc;
  interp->dealloc = dealloc;
  interp->numbuckets = 0;
  interp->buckets = (Bucket*)alloc(sizeof(Bucket));
  interp->allocbuckets = 1;
  CreateStack(alloc, &interp->stack, 16*1024);
  HashTable* env = HashTableCreate(interp->alloc, interp->dealloc, &interp->stack);
  env->parent = (uint32_t)-1;
  interp->envpos = ((uint8_t*)env - interp->stack.data);
  // define builtins
  interp->S_nil = Lips_NewPair(interp, NULL, NULL);
  interp->S_filename = Lips_NewPair(interp, NULL, NULL);
  Lips_Define(interp, "lambda", Lips_NewCMacro(interp, M_lambda, LIPS_NUM_ARGS_2|LIPS_NUM_ARGS_VAR, NULL));
  Lips_Define(interp, "macro", Lips_NewCMacro(interp, M_macro, LIPS_NUM_ARGS_2|LIPS_NUM_ARGS_VAR, NULL));
  return interp;
}

LIPS_COLD_FUNCTION Lips_Interpreter*
Lips_DefaultCreateInterpreter()
{
  return Lips_CreateInterpreter(&DefaultAlloc, &DefaultDealloc);
}

LIPS_COLD_FUNCTION void
Lips_DestroyInterpreter(Lips_Interpreter* interpreter)
{
  // clear all resources
  Lips_DeallocFunc dealloc = interpreter->dealloc;
  DestroyStack(dealloc, &interpreter->stack);
  for (uint32_t i = 0; i < interpreter->numbuckets; i++)
    DestroyBucket(dealloc, &interpreter->buckets[i]);
  dealloc(interpreter->buckets, sizeof(Bucket) * interpreter->allocbuckets);
  dealloc(interpreter, sizeof(Lips_Interpreter));
}

LIPS_HOT_FUNCTION Lips_Cell
Lips_Eval(Lips_Interpreter* interpreter, Lips_Cell cell)
{
#if 0
  // recursive version(easily readable)
  switch (LIPS_GET_TYPE(cell)) {
  default: assert(0 && "Value has undefined type");
  case LIPS_TYPE_INTEGER:
  case LIPS_TYPE_REAL:
  case LIPS_TYPE_STRING:
    return cell;
  case LIPS_TYPE_SYMBOL:
    return Lips_InternCell(interpreter, cell);
  case LIPS_TYPE_PAIR: {
    Lips_Cell name = LIPS_GET_HEAD(cell);
    Lips_Cell args = LIPS_GET_TAIL(cell);
    LIPS_TYPE_CHECK(interpreter, LIPS_TYPE_SYMBOL, name);
    Lips_Cell callable = Lips_InternCell(interpreter, name);
    if (callable == NULL || callable == interpreter->S_nil) {
      Lips_ThrowError(interpreter, "Eval: undefined symbol '%s'", LIPS_STR(name)->ptr);
    }
    return Lips_Invoke(interpreter, callable, args);
  }
  }
#else
  // don't even try to understand...
  // you just need to know that this is a non-recursive eval loop
  if (LIPS_GET_TYPE(cell) == LIPS_TYPE_PAIR) {
    uint32_t counter = 1;
    const uint32_t oldoffset = interpreter->stack.offset / sizeof(Lips_Cell);
    EvalState* state;
    Lips_Cell name;
    Lips_Cell ret;
    StackRequire(interpreter->alloc, interpreter->dealloc,
                      &interpreter->stack, sizeof(EvalState));
    EvalState* stack = (EvalState*)interpreter->stack.data + oldoffset;
    stack[0].sexp = cell;
  eval:
    state = &stack[counter-1];
    state->stage = 0;
    name = LIPS_GET_HEAD(state->sexp);
    state->passed_args = LIPS_GET_TAIL(state->sexp);
    LIPS_TYPE_CHECK(interpreter, LIPS_TYPE_SYMBOL, name);
    state->callable = Lips_InternCell(interpreter, name);
    if (state->callable == NULL) {
      Lips_ThrowError(interpreter, "Eval: undefined symbol '%s'", LIPS_STR(name)->ptr);
    }
    uint32_t argslen = CheckArgumentCount(interpreter, state->callable, state->passed_args);
    if (Lips_IsFunction(state->callable)) {n
      state->args = Lips_NewPair(interpreter, NULL, NULL);
      state->data.last = state->args;
    arg:
      while (state->passed_args) {
        // eval arguments
        Lips_Cell argument = LIPS_GET_HEAD(state->passed_args);
        if (LIPS_GET_TYPE(argument) == LIPS_TYPE_PAIR) {
          counter++;
          StackRequire(interpreter->alloc, interpreter->dealloc,
                            &interpreter->stack, sizeof(EvalState));
          stack = (EvalState*)interpreter->stack.data + oldoffset;
          stack[counter-1].sexp = argument;
          goto eval;
        } else {
          LIPS_GET_HEAD(state->data.last) = EvalNonPair(interpreter, argument);
          state->passed_args = LIPS_GET_TAIL(state->passed_args);
          if (state->passed_args) {
            LIPS_GET_TAIL(state->data.last) = Lips_NewPair(interpreter, NULL, NULL);
            state->data.last = LIPS_GET_TAIL(state->data.last);
          }
        }
      }
    } else {
      state->args = state->passed_args;
    }
    if (LIPS_GET_TYPE(state->callable) & ((LIPS_TYPE_C_FUNCTION^LIPS_TYPE_FUNCTION)|
                                          (LIPS_TYPE_C_MACRO^LIPS_TYPE_MACRO))) {
      // just call C function
      Lips_Cell c = state->callable;
      ret = c->data.cfunc.ptr(interpreter, state->args, c->data.cfunc.udata);
      state->env = NULL;
    } else {
      // push a new environment
      state->env = PushEnv(interpreter);
      if (argslen > 0) {
        // reserve space for hash table
        HashTableReserve(interpreter->alloc, interpreter->dealloc,
                              &interpreter->stack, state->env,
                              LIPS_GET_NUMARGS(state->callable));
        // define variables in a new environment
        Lips_Cell argnames = state->callable->data.lfunc.args;
        while (argnames) {
          if (LIPS_GET_HEAD(argnames)) {
            Lips_DefineCell(interpreter, LIPS_GET_HEAD(argnames), LIPS_GET_HEAD(state->args));
          }
          argnames = LIPS_GET_TAIL(argnames);
          state->args = LIPS_GET_TAIL(state->args);
        }
      }
      // execute code
      state->data.code = state->callable->data.lfunc.body;
      state->stage = 1;
    code:
      while (state->data.code) {
        if (LIPS_GET_TYPE(LIPS_GET_HEAD(state->data.code)) == LIPS_TYPE_PAIR) {
          // TODO: implement tail call optimization
          counter++;
          StackRequire(interpreter->alloc, interpreter->dealloc,
                            &interpreter->stack, sizeof(EvalState));
          stack = (EvalState*)interpreter->stack.data + oldoffset;
          stack[counter-1].sexp = LIPS_GET_HEAD(state->data.code);
          goto eval;
        } else {
          ret = EvalNonPair(interpreter, LIPS_GET_HEAD(state->data.code));
        }
        state->data.code = LIPS_GET_TAIL(state->data.code);
      }
    }
    counter--;
    if (state->env) {
      PopEnv(interpreter);
    }
    StackRelease(&interpreter->stack, sizeof(EvalState));
    if (counter > 0) {
      state = &stack[counter-1];
      if (state->stage == 0) {
        LIPS_GET_HEAD(state->data.last) = ret;
        state->passed_args = LIPS_GET_TAIL(state->passed_args);
        if (state->passed_args) {
          LIPS_GET_TAIL(state->data.last) = Lips_NewPair(interpreter, NULL, NULL);
          state->data.last = LIPS_GET_TAIL(state->data.last);
        }
        goto arg;
      } else {
        state->data.code = LIPS_GET_TAIL(state->data.code);
        goto code;
      }
    }
    return ret;
  } else {
    return EvalNonPair(interpreter, cell);
  }
#endif
  return cell;
}

Lips_Cell
Lips_EvalString(Lips_Interpreter* interpreter, const char* str, const char* filename)
{
  Parser parser;
  ParserInit(&parser, str, strlen(str));
  Lips_Cell ast = GenerateAST(interpreter, &parser);
  if (filename == NULL) filename = "<eval>";
  Lips_Cell str_filename = Lips_NewString(interpreter, filename);
  Lips_Cell temp = Lips_ListPushBack(interpreter, interpreter->S_filename, str_filename);
  Lips_Cell ret = Lips_Eval(interpreter, ast);
  LIPS_GET_TAIL(temp) = NULL; // this equals to Lips_ListPop(interpreter, interpreter->S_filename);
  return ret;
}

const char*
Lips_GetError(const Lips_Interpreter* interpreter)
{
  return interpreter->errbuff;
}

LIPS_HOT_FUNCTION void
Lips_GarbageCollect(Lips_Interpreter* interpreter)
{
  (void)interpreter;
  // TODO: garbage collection
}

Lips_Cell
Lips_Nil(Lips_Interpreter* interpreter)
{
  return interpreter->S_nil;
}

Lips_Cell
Lips_NewInteger(Lips_Interpreter* interpreter, int64_t num)
{
  Lips_Cell cell = NewCell(interpreter);
  cell->type = LIPS_TYPE_INTEGER;
  cell->data.integer = num;
  return cell;
}

Lips_Cell
Lips_NewReal(Lips_Interpreter* interpreter, double num)
{
  Lips_Cell cell = NewCell(interpreter);
  cell->type = LIPS_TYPE_REAL;
  cell->data.real = num;
  return cell;
}

Lips_Cell
Lips_NewString(Lips_Interpreter* interpreter, const char* str)
{
  return Lips_NewStringN(interpreter, str, strlen(str));
}

Lips_Cell
Lips_NewStringN(Lips_Interpreter* interpreter, const char* str, uint32_t n)
{
  Lips_Cell cell = NewCell(interpreter);
  cell->type = LIPS_TYPE_STRING;
  LIPS_STR(cell) = StringCreate(interpreter->alloc);
  StringAllocate(LIPS_STR(cell), interpreter->alloc, str, n);
  return cell;
}

Lips_Cell
Lips_NewSymbol(Lips_Interpreter* interpreter, const char* str)
{
  return Lips_NewSymbolN(interpreter, str, strlen(str));
}

Lips_Cell
Lips_NewSymbolN(Lips_Interpreter* interpreter, const char* str, uint32_t n)
{
  Lips_Cell cell = NewCell(interpreter);
  cell->type = LIPS_TYPE_SYMBOL;
  LIPS_STR(cell) = StringCreate(interpreter->alloc);
  StringAllocate(LIPS_STR(cell), interpreter->alloc, str, n);
  return cell;
}

Lips_Cell
Lips_NewPair(Lips_Interpreter* interpreter, Lips_Cell head, Lips_Cell tail)
{
  Lips_Cell cell = NewCell(interpreter);
  cell->type = LIPS_TYPE_PAIR;
  cell->data.list.head = head;
  cell->data.list.tail = tail;
  return cell;
}

Lips_Cell
Lips_NewList(Lips_Interpreter* interpreter, uint32_t numCells, Lips_Cell* cells)
{
  Lips_Cell list = Lips_NewPair(interpreter, NULL, NULL);
  Lips_Cell curr = list;
  while (numCells--) {
    LIPS_GET_HEAD(curr) = *cells;
    if (numCells > 0) {
      LIPS_GET_TAIL(curr) = Lips_NewPair(interpreter, NULL, NULL);
      curr = LIPS_GET_TAIL(curr);
    }
    cells++;
  }
  return list;
}

Lips_Cell
Lips_NewFunction(Lips_Interpreter* interpreter, Lips_Cell args, Lips_Cell body, uint8_t numargs)
{
  Lips_Cell cell = NewCell(interpreter);
  cell->type = LIPS_TYPE_FUNCTION | (numargs << 8);
  // TODO: check all arguments are symbols
  cell->data.lfunc.args = args;
  cell->data.lfunc.body = body;
  return cell;
}

Lips_Cell
Lips_NewMacro(Lips_Interpreter* interpreter, Lips_Cell args, Lips_Cell body, uint8_t numargs)
{
  Lips_Cell cell = NewCell(interpreter);
  cell->type = LIPS_TYPE_MACRO | (numargs << 8);
  cell->data.lfunc.args = args;
  cell->data.lfunc.body = body;
  return cell;
}

Lips_Cell
Lips_NewCFunction(Lips_Interpreter* interpreter, Lips_Func function, uint8_t numargs, void* udata)
{
  Lips_Cell cell = NewCell(interpreter);
  cell->type = LIPS_TYPE_C_FUNCTION | (numargs << 8);
  cell->data.cfunc.ptr = function;
  cell->data.cfunc.udata = udata;
  return cell;
}

Lips_Cell
Lips_NewCMacro(Lips_Interpreter* interpreter, Lips_Func function, uint8_t numargs, void* udata)
{
  Lips_Cell cell = NewCell(interpreter);
  cell->type = LIPS_TYPE_C_MACRO | (numargs << 8);
  cell->data.cfunc.ptr = function;
  cell->data.cfunc.udata = udata;
  return cell;
}

uint32_t
Lips_GetType(const Lips_Cell cell)
{
  return LIPS_GET_TYPE(cell);
}

uint32_t
Lips_PrintCell(Lips_Interpreter* interpreter, Lips_Cell cell, char* buff, uint32_t size)
{
  char* ptr = buff;
#define LIPS_PRINT(...) ptr += snprintf(ptr, size - (ptr - buff), __VA_ARGS__)
#if 0
  // this is an implementation with recursion used
  switch (LIPS_GET_TYPE(cell)) {
  default: return 0;
  case LIPS_TYPE_INTEGER:
    LIPS_PRINT("%ld", LIPS_GET_INTEGER(cell));
    break;
  case LIPS_TYPE_REAL:
    LIPS_PRINT("%f", LIPS_GET_REAL(cell));
    break;
  case LIPS_TYPE_STRING:
    LIPS_PRINT("\"%s\"", LIPS_GET_STRING(cell));
    break;
  case LIPS_TYPE_SYMBOL:
    LIPS_PRINT("%s", LIPS_GET_STRING(cell));
    break;
  case LIPS_TYPE_PAIR:
    LIPS_PRINT("(");
    if (LIPS_GET_HEAD(cell)) {
      ptr += Lips_PrintCell(interpreter, LIPS_GET_HEAD(cell), ptr, size - (ptr - buff));
      cell = LIPS_GET_TAIL(cell);
      while (cell && LIPS_GET_HEAD(cell)) {
        LIPS_PRINT(" ");
        ptr += Lips_PrintCell(interpreter, LIPS_GET_HEAD(cell), ptr, size - (ptr - buff));
        if (!LIPS_GET_TAIL(cell)) break;
        cell = LIPS_GET_TAIL(cell);
      }
    }
    LIPS_PRINT(")");
    break;
  }
#else
  if (LIPS_GET_TYPE(cell) == LIPS_TYPE_PAIR) {
    uint32_t counter = 0;
    const uint32_t oldoffset = interpreter->stack.offset / sizeof(Lips_Cell);
    Lips_Cell* stack = (Lips_Cell*)interpreter->stack.data + oldoffset;
    LIPS_PRINT("(");
    while (cell != NULL) {
      // TODO: do checks for buffer overflow
      if (LIPS_GET_HEAD(cell) != NULL) {
        switch (LIPS_GET_HEAD_TYPE(cell)) {
        default: assert(0);
        case LIPS_TYPE_INTEGER:
          LIPS_PRINT("%ld", LIPS_GET_INTEGER(LIPS_GET_HEAD(cell)));
          break;
        case LIPS_TYPE_REAL:
          LIPS_PRINT("%f", LIPS_GET_REAL(LIPS_GET_HEAD(cell)));
          break;
        case LIPS_TYPE_STRING:
          LIPS_PRINT("\"%s\"", LIPS_GET_STRING(LIPS_GET_HEAD(cell)));
          break;
        case LIPS_TYPE_SYMBOL:
          LIPS_PRINT("%s", LIPS_GET_STRING(LIPS_GET_HEAD(cell)));
          break;
        case LIPS_TYPE_PAIR:
          LIPS_PRINT("(");
          StackRequire(interpreter->alloc, interpreter->dealloc,
                            &interpreter->stack, sizeof(Lips_Cell));
          // memory location might change, need to reasign pointer
          stack = (Lips_Cell*)interpreter->stack.data + oldoffset;
          stack[counter] = LIPS_GET_TAIL(cell);
          counter++;
          cell = LIPS_GET_HEAD(cell);
          goto skip;
        case LIPS_TYPE_FUNCTION:
        case LIPS_TYPE_C_FUNCTION: {
          uint32_t num = LIPS_GET_NUMARGS(LIPS_GET_HEAD(cell));
          if (num & LIPS_NUM_ARGS_VAR)
            LIPS_PRINT("<func(%d+)>", num & (LIPS_NUM_ARGS_VAR-1));
          else
            LIPS_PRINT("<func(%d)>", num);
        }
          break;
        case LIPS_TYPE_MACRO:
        case LIPS_TYPE_C_MACRO: {
          uint32_t num = LIPS_GET_NUMARGS(LIPS_GET_HEAD(cell));
          if (num & LIPS_NUM_ARGS_VAR)
            LIPS_PRINT("<macro(%d+)>", num & (LIPS_NUM_ARGS_VAR-1));
          else
            LIPS_PRINT("<macro(%d)>", num);
        }
          break;
        }
      }
      cell = LIPS_GET_TAIL(cell);
      if (cell != NULL) {
        LIPS_PRINT(" ");
      }
    skip:
      if (cell == NULL) {
        LIPS_PRINT(")");
        if (counter == 0) {
          return ptr - buff;
        }
        StackRelease(&interpreter->stack, sizeof(Lips_Cell));
        counter--;
        cell = stack[counter];
        if (cell == NULL) {
          LIPS_PRINT(")");
        } else {
          LIPS_PRINT(" ");
        }
      }
    }
  } else {
    switch (LIPS_GET_TYPE(cell)) {
    default: return 0;
    case LIPS_TYPE_INTEGER:
      LIPS_PRINT("%ld", LIPS_GET_INTEGER(cell));
      break;
    case LIPS_TYPE_REAL:
      LIPS_PRINT("%f", LIPS_GET_REAL(cell));
      break;
    case LIPS_TYPE_STRING:
      LIPS_PRINT("\"%s\"", LIPS_GET_STRING(cell));
      break;
    case LIPS_TYPE_SYMBOL:
      LIPS_PRINT("%s", LIPS_GET_STRING(cell));
      break;
    case LIPS_TYPE_FUNCTION:
    case LIPS_TYPE_C_FUNCTION: {
      uint32_t num = LIPS_GET_NUMARGS(cell);
      if (num & LIPS_NUM_ARGS_VAR)
        LIPS_PRINT("<func(%d+)>", num & (LIPS_NUM_ARGS_VAR-1));
      else
        LIPS_PRINT("<func(%d)>", num);
    }
      break;
    case LIPS_TYPE_MACRO:
    case LIPS_TYPE_C_MACRO: {
      uint32_t num = LIPS_GET_NUMARGS(cell);
      if (num & LIPS_NUM_ARGS_VAR)
        LIPS_PRINT("<macro(%d+)>", num & (LIPS_NUM_ARGS_VAR-1));
      else
        LIPS_PRINT("<macro(%d)>", num);
    }
      break;
    }
  }
#endif
#undef LIPS_PRINT
  return ptr - buff;
}

uint32_t
Lips_ListLength(Lips_Interpreter* interp, Lips_Cell list)
{
  LIPS_TYPE_CHECK(interp, LIPS_TYPE_PAIR, list);
  uint32_t count = 0;
  if (LIPS_GET_HEAD(list) == NULL) {
    assert(LIPS_GET_TAIL(list) == NULL && "internal error: list semantic error");
  } else {
    while (list) {
      count++;
      assert(LIPS_GET_HEAD(list) != NULL && "internal error: list semantic error");
      list = LIPS_GET_TAIL(list);
    }
  }
  return count;
}

Lips_Cell
Lips_ListLastElement(Lips_Interpreter* interpreter, Lips_Cell list, uint32_t* length)
{
  LIPS_TYPE_CHECK(interpreter, LIPS_TYPE_PAIR, list);
  Lips_Cell ret;
  uint32_t count = 0;
  if (LIPS_GET_HEAD(list) == NULL) {
    assert(LIPS_GET_TAIL(list) == NULL && "internal error: list semantic error");
    ret = NULL;
  } else {
    while (list) {
      count++;
      assert(LIPS_GET_HEAD(list) != NULL && "internal error: list semantic error");
      ret = LIPS_GET_HEAD(list);
      list = LIPS_GET_TAIL(list);
    }
  }
  if (length) {
    *length = count;
  }
  return ret;
}

Lips_Cell
Lips_ListPushBack(Lips_Interpreter* interp, Lips_Cell list, Lips_Cell elem)
{
  LIPS_TYPE_CHECK(interp, LIPS_TYPE_PAIR, list);
  if (LIPS_GET_HEAD(list) == NULL) {
    LIPS_GET_HEAD(list) = elem;
  } else {
    while (LIPS_GET_TAIL(list) != NULL) {
      list = LIPS_GET_TAIL(list);
    }
    LIPS_GET_TAIL(list) = Lips_NewPair(interp, elem, NULL);
  }
  return list;
}

Lips_Cell
Lips_ListPopBack(Lips_Interpreter* interp, Lips_Cell list)
{
  LIPS_TYPE_CHECK(interp, LIPS_TYPE_PAIR, list);
  Lips_Cell ret;
  if (LIPS_GET_TAIL(list) == NULL) {
    assert(LIPS_GET_HEAD(list) && "empty list");
    ret = LIPS_GET_HEAD(list);
    LIPS_GET_HEAD(list) = NULL;
  } else {
    Lips_Cell temp;
    do {
      temp = list;
      list = LIPS_GET_TAIL(list);
    } while (LIPS_GET_TAIL(list) != NULL);
    ret = LIPS_GET_HEAD(list);
    LIPS_GET_HEAD(list) = NULL;
    LIPS_GET_TAIL(temp) = NULL;
  }
  return ret;
}

Lips_Cell
Lips_Define(Lips_Interpreter* interpreter, const char* name, Lips_Cell cell)
{
  assert(cell);
  HashTable* env = InterpreterEnv(interpreter);
  Lips_Cell* ptr = HashTableInsert(interpreter->alloc, interpreter->dealloc,
                                        &interpreter->stack, env,
                                        name, cell);
  if (ptr == NULL) {
    Lips_ThrowError(interpreter, "Value is already defined");
    return NULL;
  }
  return cell;
}

Lips_Cell
Lips_DefineCell(Lips_Interpreter* interpreter, Lips_Cell cell, Lips_Cell value)
{
  LIPS_TYPE_CHECK(interpreter, LIPS_TYPE_SYMBOL|LIPS_TYPE_STRING, cell);
  assert(value);
  HashTable* env = InterpreterEnv(interpreter);
  Lips_Cell* ptr = HashTableInsertWithHash(interpreter->alloc, interpreter->dealloc,
                                                &interpreter->stack, env,
                                                LIPS_STR(cell)->hash, LIPS_STR(cell)->ptr, value);
  if (ptr == NULL) {
    Lips_ThrowError(interpreter, "Value is already defined");
    return NULL;
  }
  return value;
}

Lips_Cell
Lips_Intern(Lips_Interpreter* interpreter, const char* name)
{
  HashTable* env = InterpreterEnv(interpreter);
  do {
    Lips_Cell* ptr = HashTableSearch(env, name);
    if (ptr) {
      return *ptr;
    }
    env = EnvParent(interpreter, env);
  } while (env);
  return NULL;
}

LIPS_HOT_FUNCTION Lips_Cell
Lips_InternCell(Lips_Interpreter* interpreter, Lips_Cell cell)
{
  LIPS_TYPE_CHECK(interpreter, LIPS_TYPE_SYMBOL|LIPS_TYPE_STRING, cell);
  HashTable* env = InterpreterEnv(interpreter);
  do {
    Lips_Cell* ptr = HashTableSearchWithHash(env, LIPS_STR(cell)->hash, LIPS_STR(cell)->ptr);
    if (ptr) {
      return *ptr;
    }
    env = EnvParent(interpreter, env);
  } while (env);
  return interpreter->S_nil;
}

LIPS_HOT_FUNCTION Lips_Cell
Lips_Invoke(Lips_Interpreter* interpreter, Lips_Cell callable, Lips_Cell args)
{
  Lips_Cell ret;
  LIPS_TYPE_CHECK(interpreter, LIPS_TYPE_FUNCTION|LIPS_TYPE_MACRO, callable);
  LIPS_TYPE_CHECK(interpreter, LIPS_TYPE_PAIR, args);
  uint32_t argslen = CheckArgumentCount(interpreter, callable, args);
  // evaluate arguments
  if (Lips_IsFunction(callable)) {
    Lips_Cell args_tail = Lips_NewPair(interpreter, NULL, NULL);
    Lips_Cell ev_args = args_tail;
    Lips_Cell temp = args;
    while (temp) {
      if (LIPS_GET_HEAD(temp)) {
        Lips_Cell value = Lips_Eval(interpreter, LIPS_GET_HEAD(temp));
        LIPS_GET_HEAD(args_tail) = value;
      }
      temp = LIPS_GET_TAIL(temp);
      if (temp) {
        LIPS_GET_TAIL(args_tail) = Lips_NewPair(interpreter, NULL, NULL);
        args_tail = LIPS_GET_TAIL(args_tail);
      }
    }
    args = ev_args;
  }
  if (LIPS_GET_TYPE(callable) & ((LIPS_TYPE_C_FUNCTION^LIPS_TYPE_FUNCTION)|
                                 (LIPS_TYPE_C_MACRO^LIPS_TYPE_MACRO))) {
    // just call C function
    ret = callable->data.cfunc.ptr(interpreter, args, callable->data.cfunc.udata);
  } else {
    // push a new environment
    HashTable* env = PushEnv(interpreter);
    if (argslen > 0) {
      // reserve space for hash table
      HashTableReserve(interpreter->alloc, interpreter->dealloc,
                            &interpreter->stack, env,
                            LIPS_GET_NUMARGS(callable));
      // define variables in a new environment
      Lips_Cell argnames = callable->data.lfunc.args;
      while (argnames) {
        if (LIPS_GET_HEAD(argnames)) {
          Lips_DefineCell(interpreter, LIPS_GET_HEAD(argnames), LIPS_GET_HEAD(args));
        }
        argnames = LIPS_GET_TAIL(argnames);
        args = LIPS_GET_TAIL(args);
      }
    }
    // execute code
    Lips_Cell code = callable->data.lfunc.body;
    while (code) {
      if (LIPS_GET_HEAD(code)) {
        ret = Lips_Eval(interpreter, LIPS_GET_HEAD(code));
      }
      code = LIPS_GET_TAIL(code);
    }
    // pop environment
    PopEnv(interpreter);
  }
  return ret;
}

const char*
Lips_SetError(Lips_Interpreter* interpreter, const char* fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(interpreter->errbuff, sizeof(interpreter->errbuff), fmt, ap);
  va_end(ap);
  printf("\n");
  fflush(stdout);
  return interpreter->errbuff;
}

int64_t
Lips_GetInteger(Lips_Interpreter* interpreter, Lips_Cell cell)
{
  (void)interpreter;
  LIPS_TYPE_CHECK(interpreter, LIPS_TYPE_INTEGER, cell);
  return LIPS_GET_INTEGER(cell);
}

double
Lips_GetReal(Lips_Interpreter* interpreter, Lips_Cell cell)
{
  (void)interpreter;
  LIPS_TYPE_CHECK(interpreter, LIPS_TYPE_REAL, cell);
  return LIPS_GET_REAL(cell);
}

const char*
Lips_GetString(Lips_Interpreter* interpreter, Lips_Cell cell)
{
  (void)interpreter;
  LIPS_TYPE_CHECK(interpreter, LIPS_TYPE_STRING|LIPS_TYPE_SYMBOL, cell);
  return LIPS_GET_STRING(cell);
}

Lips_Cell
Lips_CAR(Lips_Interpreter* interpreter, Lips_Cell cell)
{
  (void)interpreter;
  LIPS_TYPE_CHECK(interpreter, LIPS_TYPE_PAIR, cell);
  return LIPS_GET_HEAD(cell);
}

Lips_Cell
Lips_CDR(Lips_Interpreter* interpreter, Lips_Cell cell)
{
  (void)interpreter;
  LIPS_TYPE_CHECK(interpreter, LIPS_TYPE_PAIR, cell);
  return LIPS_GET_TAIL(cell);
}

void*
DefaultAlloc(size_t bytes)
{
  return malloc(bytes);
}

void
DefaultDealloc(void* ptr, size_t bytes)
{
  (void)bytes;
  free(ptr);
}

void
DestroyCell(Lips_Cell cell, Lips_DeallocFunc dealloc) {
  switch (LIPS_GET_TYPE(cell)) {
  default: assert(0 && "internal error: destroy_cell: faced undefined type of cell");
  case LIPS_TYPE_INTEGER:
  case LIPS_TYPE_REAL:
  case LIPS_TYPE_FUNCTION:
  case LIPS_TYPE_MACRO:
  case LIPS_TYPE_C_FUNCTION:
  case LIPS_TYPE_C_MACRO:
    // do nothing
    break;
  case LIPS_TYPE_STRING:
  case LIPS_TYPE_SYMBOL:
    StringDestroy(LIPS_STR(cell), dealloc);
    break;
  case LIPS_TYPE_PAIR:

    break;
  }
}

StringData*
StringCreate(Lips_AllocFunc alloc)
{
  StringData* str;
  str = (StringData*)alloc(sizeof(StringData));
  str->length = 0;
  str->counter = 0;
  str->ptr = NULL;
  return str;
}

void
StringAllocate(StringData* str, Lips_AllocFunc alloc, const char* ptr, uint32_t n)
{
  str->length = n;
  str->counter = 1;
  str->ptr = (char*)alloc(n+1);
  str->ptr[n] = '\0';
  strncpy(str->ptr, ptr, n);
  str->hash = ComputeHash(str->ptr);
}

void
StringDestroy(StringData* str, Lips_DeallocFunc dealloc)
{
  if (str->counter == 1) {
    dealloc(str->ptr, str->length + 1);
    // TODO: proper string allocations
    dealloc(str, sizeof(StringData));
  } else {
    str->counter--;
  }
}

void
StringSet(Lips_AllocFunc alloc, StringData* str, uint32_t index, char c)
{
  if (str->counter > 1) {
    // create a new string object
    StringData* newstr = StringCreate(alloc);
    StringAllocate(newstr, alloc, str->ptr, str->length);
    str = newstr;
  }
  assert(index < str->length && "string_set: index out of bounds");
  str->ptr[index] = c;
}

StringData*
StringCopy(StringData* src)
{
  // copies are very cheap, because we don't actually do copies
  src->counter += 1;
  return src;
}

void
ParserInit(Parser* parser, const char* str, uint32_t len)
{
  memset(parser, 0, sizeof(Parser));
  parser->text = str;
  if (len == 0) {
    parser->length = strlen(str);
  } else {
    parser->length = len;
  }
  int parenBalance = 0;
  for (uint32_t i = 0; i < parser->length; i++) {
    switch (parser->text[i]) {
    case '(':
      parenBalance++;
      if (parenBalance > (int)parser->numlists)
        parser->numlists = parenBalance;
      break;
    case ')':
      parenBalance--;
      break;
    }
  }
  if (parenBalance != 0) {
    // TODO: log an error
  }
}

int
ParserNextToken(Parser* parser)
{
  const char* text = parser->text;
  Token* token = &parser->currtok;
  // skip comments and whitespaces
  do {
    if (text[parser->pos] == ';')
      while (parser->pos < parser->length && text[parser->pos] != '\n' && text[parser->pos] != '\r')
        parser->pos++;
    while (parser->pos < parser->length && LIPS_IS_WHITESPACE(text[parser->pos]))
      parser->pos++;
  } while(text[parser->pos] == ';');
  if (parser->pos >= parser->length) return 0;
  if (LIPS_IS_SPECIAL_CHAR(text[parser->pos])) {
    token->str = text+parser->pos;
    token->length = 1;
  } else if (text[parser->pos] == '"') {
    // parse string, which can be multiline
    uint32_t end = parser->pos + 1;
    while (text[end] != '"') {
      if (end >= parser->length) {
        return LIPS_EOF;
      }
      end++;
    }
    end++;
    token->str = text + parser->pos;
    token->length = end - parser->pos;
  } else {
    // parse symbol or number
    uint32_t end = parser->pos + 1;
    while (end < parser->length) {
      if (LIPS_IS_WHITESPACE(text[end]) || LIPS_IS_SPECIAL_CHAR(text[end]))
        break;
      if (text[end] == ';') {
        // skip comment
        while (end < parser->length && text[end] != '\n' && text[end] != '\r')
          end++;
        break;
      }
      end++;
    }
    token->str = text + parser->pos;
    token->length = end - parser->pos;
  }
  parser->pos += token->length;
  return 1;
}

int
Lips_IsTokenNumber(const Token* token) {
  return LIPS_IS_DIGIT(token->str[0]) ||
    (token->str[0] == '-' && LIPS_IS_DIGIT(token->str[1]));
}

Lips_Cell
ParseNumber(Lips_Interpreter* interpreter, const Token* token) {
  int is_float = 0;
  for (uint32_t i = 0; i < token->length; i++) {
    if (!LIPS_IS_DIGIT(token->str[i])) {
      if (token->str[i] == '.') {
        is_float++;
      } else {
        LIPS_LOG_ERROR(interpreter, "Found undefined character '%c' when parsing number in token '%.*s'",
                       token->str[i], token->length, token->str);
        return NULL;
      }
    }
  }
  if (is_float > 1) {
    LIPS_LOG_ERROR(interpreter, "Encountered more than 1 '.' when parsing float in token '%.*s'",
                   token->length, token->str);
    return NULL;
  }
  // TODO: use strtod and strtoll correctly
  if (is_float) {
    return Lips_NewReal(interpreter, strtod(token->str, NULL));
  } else {
    return Lips_NewInteger(interpreter, strtoll(token->str, NULL, 10));
  }
}

LIPS_HOT_FUNCTION Lips_Cell
NewCell(Lips_Interpreter* interp)
{
  for (uint32_t i = interp->numbuckets; i > 0; i--)
    if (interp->buckets[i-1].size < LIPS_BUCKET_SIZE) {
      // we found a bucket with available storage, use it
      return BucketNewCell(&interp->buckets[i-1]);
    }
  if (interp->numbuckets == interp->allocbuckets) {
    // we're out of storage for buckets, allocate more
    Bucket* new_buckets = interp->alloc(interp->allocbuckets * 2);
    if (!new_buckets) return NULL;
    memcpy(new_buckets, interp->buckets, interp->numbuckets * sizeof(Bucket));
    interp->buckets = new_buckets;
    interp->dealloc(new_buckets, sizeof(Bucket) * interp->allocbuckets);
    interp->allocbuckets = interp->allocbuckets * 2;
  }
  // push back a new bucket
  Bucket* new_bucket = &interp->buckets[interp->numbuckets];
  CreateBucket(interp->alloc, new_bucket);
  interp->numbuckets++;
  return BucketNewCell(new_bucket);
}

Lips_Cell
GenerateAST(Lips_Interpreter* interpreter, Parser* parser)
{
#if 0
  // this is an implementation with recursion, it is much more readable but a bit slower
  Lips_Cell tree = NULL;
  Lips_Cell cell = NULL;
  int code = ParserNextToken(parser);
  if (code == LIPS_EOF) {
    LIPS_LOG_ERROR(interpreter, "EOF: expected \"");
  } else if (code == 1) {
    switch (parser->currtok.str[0]) {
    case '(':
      tree = Lips_NewPair(interpreter, NULL, NULL);
      cell = tree;
      while (1) {
        LIPS_GET_HEAD(cell) = Lips_GenerateAST(interpreter, parser);
        if (LIPS_GET_HEAD(cell) == NULL)
          break;
        LIPS_GET_TAIL(cell) = Lips_NewPair(interpreter, NULL, NULL);
        cell = LIPS_GET_TAIL(cell);
      }
      break;
    case ')':
      break;
    case '"':
      tree = Lips_NewStringN(interpreter, parser->currtok.str+1, parser->currtok.length-2);
      break;
    default:
      if (Lips_IsTokenNumber(&parser->currtok)) {
        tree = Lips_ParseNumber(interpreter, &parser->currtok);
      } else {
        tree = Lips_NewSymbolN(interpreter, parser->currtok.str, parser->currtok.length);
      }
      break;
    }
  }
  return tree;
#else
  // here we parse list of tokens and create our Abstract Syntax Tree.
  // this works without recursion but we still need a queue of nodes, which size
  // is equal to maximum paren depth(precomputed by parser).
  uint32_t numbytes = parser->numlists * sizeof(Lips_Cell);
  Lips_Cell cell = NULL;
  int code = ParserNextToken(parser);
  if (numbytes == 0) {
    while (code == 1) {
      switch (parser->currtok.str[0]) {
      case '"':
        cell = Lips_NewStringN(interpreter,
                               parser->currtok.str+1, parser->currtok.length-2);
        break;
      default:
        if (Lips_IsTokenNumber(&parser->currtok)) {
          cell = ParseNumber(interpreter, &parser->currtok);
        } else {
          cell = Lips_NewSymbolN(interpreter, parser->currtok.str, parser->currtok.length);
        }
        break;
      }
      code = ParserNextToken(parser);
    }
    return cell;
  }
  // here we would store queue of cells represented by parens
  // NOTE: we're not afraid of case when numbytes==0 because StackRequire simply just adds
  // a number to a pointer,
  Lips_Cell* stack = StackRequire(interpreter->alloc, interpreter->dealloc,
                                       &interpreter->stack, numbytes);
  int counter = 0;
  // this cycle looks messy but it works :)
  while (code == 1) {
    switch (parser->currtok.str[0]) {
    case '(':
      // add new cell to the queue
      if (cell == NULL) {
        cell = Lips_NewPair(interpreter, NULL, NULL);
        stack[counter] = cell;
      } else {
        stack[counter] = cell;
        LIPS_GET_HEAD(cell) = Lips_NewPair(interpreter, NULL, NULL);
        cell = LIPS_GET_HEAD(cell);
      }
      counter++;
      goto skip_pushing;
    case ')':
      // pop cell from queue
      counter--;
      if (counter == 0) {
        // don't waste memory by adding an empty list to the end
        goto skip_pushing;
      } else {
        cell = stack[counter];
      }
      break;
    case '"':
      LIPS_GET_HEAD(cell) = Lips_NewStringN(interpreter,
                                            parser->currtok.str+1, parser->currtok.length-2);
      break;
    default:
      if (Lips_IsTokenNumber(&parser->currtok)) {
        LIPS_GET_HEAD(cell) = ParseNumber(interpreter, &parser->currtok);
      } else {
        LIPS_GET_HEAD(cell) = Lips_NewSymbolN(interpreter, parser->currtok.str, parser->currtok.length);
      }
      break;
    }
    code = ParserNextToken(parser);
    // don't waste memory by adding an empty list to the end
    if (parser->currtok.str[0] != ')') {
      // push new cell to the end
      LIPS_GET_TAIL(cell) = Lips_NewPair(interpreter, NULL, NULL);
      cell = LIPS_GET_TAIL(cell);
    }
    continue;
  skip_pushing:
    code = ParserNextToken(parser);
  }
  assert(counter == 0 && "parser internal error"); // I think this is useful, should I remove it?
  if (code == LIPS_EOF) {
    LIPS_LOG_ERROR(interpreter, "EOF: expected \"");
  }
  Lips_Cell ret = stack[0];
  StackRelease(&interpreter->stack, numbytes);
  return ret;
#endif
}

void
CreateBucket(Lips_AllocFunc alloc, Bucket* bucket)
{
  uint32_t i;
  bucket->data = (Lips_Value*)alloc(LIPS_BUCKET_SIZE * sizeof(Lips_Value));
  bucket->size = 0;
  bucket->next = 0;
  for (i = 0; i < LIPS_BUCKET_SIZE; i++) {
    *(uint32_t*)&bucket->data[i] = (i + 1) | LIPS_DEAD_MASK;
  }
}

void
DestroyBucket(Lips_DeallocFunc dealloc, Bucket* bucket)
{
  // destroy each cell in bucket
  for (uint32_t i = 0; bucket->size > 0; i++) {
    Lips_Cell cell = bucket->data + i;
    if ((cell->type & LIPS_DEAD_MASK) == 0) {
      DestroyCell(cell, dealloc);
      bucket->size--;
    }
  }
  // free bucket's memory
  dealloc(bucket->data, sizeof(Lips_Value) * LIPS_BUCKET_SIZE);
}

Lips_Cell
BucketNewCell(Bucket* bucket)
{
  assert(bucket->size < LIPS_BUCKET_SIZE && "Bucket out of space");
  Lips_Cell ret = &bucket->data[bucket->next];
  bucket->next = *(uint32_t*)ret ^ LIPS_DEAD_MASK;
  bucket->size++;
  return ret;
}

void
BucketDeleteCell(Bucket* bucket, Lips_Cell cell)
{
  uint32_t index = cell - bucket->data;
  assert(index < LIPS_BUCKET_SIZE && "cell doesn't belong to this Bucket");
  assert(bucket->size > 0 && "Bucket is empty");
  *(uint32_t*)cell = bucket->next | LIPS_DEAD_MASK;
  bucket->next = index;
  bucket->size--;
}

void
CreateStack(Lips_AllocFunc alloc, Stack* stack, uint32_t size) {
  stack->data = alloc(size);
  stack->offset = 0;
  stack->size = size;
}

void
DestroyStack(Lips_DeallocFunc dealloc, Stack* stack)
{
  dealloc(stack->data, stack->size);
  stack->data = NULL;
  stack->size = 0;
}

void*
StackRequire(Lips_AllocFunc alloc, Lips_DeallocFunc dealloc,
                  Stack* stack, uint32_t bytes)
{
  if (stack->offset + bytes > stack->size) {
    uint8_t* oldata = stack->data;
    uint32_t oldsize = stack->size;
    // TODO: pick a better grow policy
    stack->size = stack->size * 2 + bytes;
    stack->data = (uint8_t*)alloc(stack->size);
    if (stack->data == NULL) {
      stack->data = oldata;
      stack->size = oldsize;
      return NULL;
    }
    memcpy(stack->data, oldata, oldsize);
    dealloc(oldata, oldsize);
  }
  void* ret = (void*)(stack->data + stack->offset);
  stack->offset += bytes;
  return ret;
}

void
StackRelease(Stack* stack, uint32_t bytes)
{
  assert(stack->offset >= bytes);
  stack->offset -= bytes;
}

HashTable*
InterpreterEnv(Lips_Interpreter* interpreter)
{
  HashTable* env = (HashTable*)(interpreter->stack.data + interpreter->envpos);
  return env;
}

HashTable*
PushEnv(Lips_Interpreter* interpreter)
{
  HashTable* env = HashTableCreate(interpreter->alloc, interpreter->dealloc,
                                             &interpreter->stack);
  env->parent = interpreter->envpos;
  interpreter->envpos = (uint8_t*)env - interpreter->stack.data;
  return env;
}

void
PopEnv(Lips_Interpreter* interpreter)
{
  HashTable* env = InterpreterEnv(interpreter);
  HashTableDestroy(&interpreter->stack, env);
  interpreter->envpos = env->parent;
}

HashTable*
EnvParent(Lips_Interpreter* interpreter, HashTable* env)
{
  if (env->parent == (uint32_t)-1) {
    return NULL;
  }
  HashTable* parent = (HashTable*)(interpreter->stack.data + env->parent);
  return parent;
}

uint32_t
ComputeHash(const char* string)
{
  // https://cp-algorithms.com/string/string-hashing.html
  uint32_t p_pow = 1;
  uint32_t hash = 0;
  while (*string) {
    hash = (hash + (*string - 'a' + 1) * p_pow) % 1000000009;
    p_pow = (p_pow * 31) % 1000000009;
    string++;
  }
  return hash;
}

HashTable*
HashTableCreate(Lips_AllocFunc alloc, Lips_DeallocFunc dealloc, Stack* stack)
{
  HashTable* ht = StackRequire(alloc, dealloc, stack, sizeof(HashTable));
  memset(ht, 0, sizeof(HashTable));
  return ht;
}

void
HashTableDestroy(Stack* stack, HashTable* ht)
{
  StackRelease(stack, ht->allocated * sizeof(Node));
  StackRelease(stack, sizeof(HashTable));
  assert((HashTable*)(stack->data + stack->offset) == ht && "internal error: incorrect hash table destroy");
}

void
HashTableReserve(Lips_AllocFunc alloc, Lips_DeallocFunc dealloc,
                      Stack* stack, HashTable* ht, uint32_t capacity)
{
  assert(capacity > ht->allocated);
  if (ht->size == 0) {
    StackRelease(stack, ht->allocated);
  }
  uint32_t preallocated = ht->allocated;
  ht->allocated = capacity;
  Node* nodes = StackRequire(alloc, dealloc, stack, capacity * sizeof(Node));
  nodes += capacity - preallocated;
  Node* data = LIPS_HASH_TABLE_DATA(ht);
  memcpy(nodes, data, preallocated * sizeof(Node));
  for (uint32_t i = 0; i < capacity; i++) {
    data[i].value = NULL;
  }
  if (ht->size > 0) {
    uint32_t oldSize = ht->size;
    ht->size = 0;
    for (uint32_t i = 0; i < preallocated; i++) {
      if (LIPS_NODE_VALID(nodes[i])) {
        HashTableInsertWithHash(alloc, dealloc, stack,
                                     ht, nodes[i].hash,
                                     nodes[i].key, nodes[i].value);
        if (ht->size == oldSize) break;
      }
    }
    StackRelease(stack, preallocated * sizeof(Node));
  }
}

Lips_Cell*
HashTableInsert(Lips_AllocFunc alloc, Lips_DeallocFunc dealloc, Stack* stack,
                     HashTable* ht, const char* key, Lips_Cell value) {
  uint32_t hash = ComputeHash(key);
  return HashTableInsertWithHash(alloc, dealloc, stack, ht, hash, key, value);
}

Lips_Cell*
HashTableInsertWithHash(Lips_AllocFunc alloc, Lips_DeallocFunc dealloc, Stack* stack,
                             HashTable* ht, uint32_t hash,
                             const char* key, Lips_Cell value) {
  assert(value && "Can not insert null");
  if (ht->size == ht->allocated) {
    uint32_t sz = (ht->size == 0) ? 1 : (ht->size<<1);
    HashTableReserve(alloc, dealloc, stack, ht, sz);
  }
  Node* data = LIPS_HASH_TABLE_DATA(ht);
  uint32_t id = hash % ht->allocated;
  while (LIPS_NODE_VALID(data[id])) {
    // Hash table already has this element
    if (hash == data[id].hash && strcmp(data[id].key, key) == 0) {
      return NULL;
    }
    id = (id+1) % ht->allocated;
  }
  data[id].hash = hash;
  data[id].key = key;
  data[id].value = value;
  // increment the size counter
  ht->size++;
  return &data[id].value;
}

Lips_Cell*
HashTableSearch(const HashTable* ht, const char* key)
{
  uint32_t hash = ComputeHash(key);
  return HashTableSearchWithHash(ht, hash, key);
}

Lips_Cell*
HashTableSearchWithHash(const HashTable* ht, uint32_t hash, const char* key)
{
  Node* data = LIPS_HASH_TABLE_DATA(ht);
  uint32_t i = 0;
  uint32_t id = hash;
  while (i < ht->size) {
    id = id % ht->allocated;
    if (LIPS_NODE_VALID(data[id])) {
      if (strcmp(data[id].key, key) == 0)
        return &data[id].value;
      i++;
      // linear probing
      id++;
    } else {
      return NULL;
    }
  }
  return NULL;
}

void
HashTableIterate(HashTable* ht, Iterator* it) {
  it->node = LIPS_HASH_TABLE_DATA(ht);
  it->size = ht->size;
  if (it->size > 0) {
    while (!LIPS_NODE_VALID(*it->node)) {
      it->node++;
    }
  }
}

int
IteratorIsEmpty(const Iterator* it) {
  return it->size == 0;
}

void
IteratorGet(const Iterator* it, const char** key, Lips_Cell* value) {
  *key = it->node->key;
  *value  = it->node->value;
}

void
IteratorNext(Iterator* it) {
  assert(it->size > 0);
  it->node++;
  if (it->size > 1) {
    while (!LIPS_NODE_VALID(*it->node)) {
      it->node++;
    }
  }
  it->size--;
}

uint32_t
CheckArgumentCount(Lips_Interpreter* interpreter, Lips_Cell callable, Lips_Cell args)
{
  uint32_t numargs = LIPS_GET_NUMARGS(callable) & (LIPS_NUM_ARGS_VAR-1);
  uint32_t variadic = LIPS_GET_NUMARGS(callable) & LIPS_NUM_ARGS_VAR;
  uint32_t listlen = Lips_ListLength(interpreter, args);
  if ((numargs > listlen) ||
      (numargs < listlen && variadic == 0)) {
        Lips_ThrowError(interpreter,
                        "Invalid number of arguments, passed %u arguments, but callable accepts %u",
                        numargs, listlen);
  }
  return listlen;
}

Lips_Cell
EvalNonPair(Lips_Interpreter* interpreter, Lips_Cell cell)
{
  assert(!Lips_IsList(cell));
  switch (LIPS_GET_TYPE(cell)) {
  case LIPS_TYPE_INTEGER:
  case LIPS_TYPE_REAL:
  case LIPS_TYPE_STRING:
  case LIPS_TYPE_FUNCTION:
  case LIPS_TYPE_C_FUNCTION:
  case LIPS_TYPE_MACRO:
  case LIPS_TYPE_C_MACRO:
    return cell;
  case LIPS_TYPE_SYMBOL:
    return Lips_InternCell(interpreter, cell);
  }
  assert(0 && "internal error: cell has undefined type");
}

Lips_Cell
M_lambda(Lips_Interpreter* interpreter, Lips_Cell args, void* udata)
{
  LIPS_TYPE_CHECK(interpreter, LIPS_TYPE_PAIR, LIPS_GET_HEAD(args));
  (void)udata;
  uint32_t len;
  Lips_Cell last = Lips_ListLastElement(interpreter, LIPS_GET_HEAD(args), &len);
  if (len > 127) {
    Lips_ThrowError(interpreter,
                    "Too many arguments(%u), in Lips language callables have up to 127 named arguments", len);
  }
  if (last && Lips_IsSymbol(last) && strcmp(LIPS_STR(last)->ptr, "...") == 0) {
    len |= LIPS_NUM_ARGS_VAR;
  }
  Lips_Cell lambda = Lips_NewFunction(interpreter, LIPS_GET_HEAD(args), LIPS_GET_TAIL(args), len);
  return lambda;
}

Lips_Cell M_macro(Lips_Interpreter* interpreter, Lips_Cell args, void* udata)
{
  LIPS_TYPE_CHECK(interpreter, LIPS_TYPE_PAIR, LIPS_GET_HEAD(args));
  (void)udata;
  uint32_t len;
  Lips_Cell last = Lips_ListLastElement(interpreter, args, &len);
  if (len > 127) {
    Lips_ThrowError(interpreter,
                    "Too many arguments(%u), in Lips language callables have up to 127 named arguments", len);
  }
  if (last && Lips_IsSymbol(last) && strcmp(LIPS_STR(last)->ptr, "...") == 0) {
    len |= LIPS_NUM_ARGS_VAR;
  }
  Lips_Cell macro = Lips_NewMacro(interpreter, LIPS_GET_HEAD(args), LIPS_GET_TAIL(args), len);
  return macro;
}
