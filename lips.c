/* Lips - tiny Lisp interpreter designed for being embedded in games
 */
#include "assert.h"
#include "stdarg.h"
#include "stdio.h"
#include "string.h"

#include "lips.h"

/// CONSTANTS

// number of values in one bucket
#ifndef BUCKET_SIZE
#define BUCKET_SIZE 1024
#endif

// number buckets that Interpreter allocates by default
#ifndef NUM_DEFAULT_BUCKETS
#define NUM_DEFAULT_BUCKETS 16
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
typedef struct MemChunk MemChunk;
typedef struct FreeRegion FreeRegion;


/// MACROS

#define MAX_CHUNKS 16
#define LIPS_EOF (-1)
#define STACK_INVALID_POS ((uint32_t)-1)
#define DEAD_MASK (1<<31)
#define MARK_MASK (1<<30)
#define IS_DEAD(cell) ((cell).type & DEAD_MASK)
#define IS_WHITESPACE(c) ((c) == ' ' || (c) == '\n' || (c) == '\t' || (c) == '\r')
#define IS_SPECIAL_CHAR(c) ((c) == '(' || (c) == ')' || (c) == '\'' || (c) == '`')
#define IS_DIGIT(c) ((c) >= '0' && (c) <= '9')
#define LOG_ERROR(interpreter, ...) snprintf(interpreter->errbuff, sizeof(interpreter->errbuff), __VA_ARGS__)
#define TYPE_CHECK(interpreter, type, cell) if (!(GET_TYPE(cell) & (type))) LIPS_THROW_ERROR(interpreter, "Typecheck failed (%d & %d) at line %d", GET_TYPE(cell), type, __LINE__);
#define TYPE_CHECK_FORCED(type, cell) do {                              \
    assert((GET_TYPE(cell) & (type)) && "Typecheck failed"); \
  } while (0)
#define GET_STR(cell) ((cell)->data.str)
#define STR_DATA_CHUNK_INDEX(str) ((str)->flags & 15)
#define STR_DATA_BUCKET_INDEX(str) (((str)->flags >> 4) & ((1<<17)-1))
#define STR_DATA_REF_COUNT(str) ((str)->flags >> 21)
#define STR_DATA_MAKE_BUCKET_INDEX(id) ((id) << 4)
#define STR_DATA_MAKE_CHUNK_INDEX(id) ((id) & 15)
#define STR_DATA_REF_INC(str) ((str)->flags += (1<<21))
#define STR_DATA_REF_DEC(str) ((str)->flags -= (1<<21))
#define STR_PTR_CHUNK(interp, str) &(interp->chunks[STR_DATA_CHUNK_INDEX(str)])
#define STR_DATA_PTR(chunk, str) &(chunk)->data[str->ptr_offset]
#define STR_DATA_PTR2(interp, str) STR_DATA_PTR(STR_PTR_CHUNK(interp, str), str)
#define STR_DATA_LENGTH(str) ((str)->length & ((1<<28)-1))
#define STR_DATA_ALLOCATED(str) (((str)->length >> 28) + STR_DATA_LENGTH(str) + sizeof(StringData*))
#define STR_DATA_MAKE_LEN(n, alloc) ((n) | ((alloc) << 28))
#define GET_STR_PTR(interp, cell) STR_DATA_PTR(STR_PTR_CHUNK(interp, (cell)->data.str), (cell)->data.str)


/// LIST OF FUNCTIONS

static void* DefaultAlloc(size_t bytes);
static void DefaultDealloc(void* ptr, size_t bytes);

static Lips_Cell NewCell(Lips_Machine* interp) LIPS_HOT_FUNCTION;
static void DestroyCell(Lips_Machine* interpreter, Lips_Cell cell);

static StringData* StringCreate(Lips_Machine* interp, const char* str, uint32_t n);
static StringData* StringCreateWithHash(Lips_Machine* interp, const char* str, uint32_t n, uint32_t hash);
static void StringDestroy(Lips_Machine* interp, StringData* str);
static int StringEqual(Lips_Machine* interpreter, const StringData* lhs, const StringData* rhs);

const char* GDB_lips_to_c_string(Lips_Machine* interp, Lips_Cell cell);

static void ParserInit(Parser* parser, const char* str, uint32_t len);
static int ParserNextToken(Parser* parser);
static int Lips_IsTokenNumber(const Token* token);
static Lips_Cell ParseNumber(Lips_Machine* interp, const Token* token);
static Lips_Cell GenerateAST(Lips_Machine* interp, Parser* parser);

static void CreateBucket(Lips_AllocFunc alloc, Bucket* bucket, uint32_t elem_size);
static void DestroyBucket(Lips_Machine* interpreter, Bucket* bucket, uint32_t elem_size);
static Lips_Cell BucketNewCell(Bucket* bucket);
static void BucketDeleteCell(Bucket* bucket, Lips_Cell cell);
static StringData* BucketNewString(Bucket* bucket);
static void BucketDeleteString(Bucket* bucket, StringData* string);

static void CreateStack(Lips_AllocFunc alloc, Stack* stack, uint32_t size);
static void DestroyStack(Lips_DeallocFunc dealloc, Stack* stack);
static void StackGrow(Lips_AllocFunc alloc, Lips_DeallocFunc dealloc,
                      Stack* stack);
static void* StackRequire(Lips_AllocFunc alloc, Lips_DeallocFunc dealloc,
                          Stack* stack, uint32_t bytes);
// number of released bytes returned
static uint32_t StackRelease(Stack* stack, void* data);
static void* StackRequireFromBack(Lips_AllocFunc alloc, Lips_DeallocFunc dealloc,
                                 Stack* stack, uint32_t bytes);
static void* StackReleaseFromBack(Stack* stack, uint32_t bytes);

static EvalState* PushEvalState(Lips_Machine* interpreter);
static EvalState* PopEvalState(Lips_Machine* interpreter);

static void PushCatch(Lips_Machine* interpreter);
static void PopCatch(Lips_Machine* interpreter);
static EvalState* UnwindStack(Lips_Machine* interpreter);

static HashTable* InterpreterEnv(Lips_Machine* interpreter);
static HashTable* PushEnv(Lips_Machine* interpreter);
static void PopEnv(Lips_Machine* interpreter);
static HashTable* EnvParent(Lips_Machine* interpreter, HashTable* env);
// compute hash of null terminated string
static uint32_t ComputeHash(const char* string) LIPS_PURE_FUNCTION;
// compute hash of sized string(no null terminator at end)
static uint32_t ComputeHashN(const char* string, uint32_t n) LIPS_PURE_FUNCTION;
static HashTable* HashTableCreate(Lips_AllocFunc alloc, Lips_DeallocFunc dealloc,
                                  Stack* stack);
static void HashTableDestroy(Stack* stack, HashTable* ht);
static void HashTableReserve(Lips_Machine* interpreter, HashTable* ht, uint32_t capacity);
static Lips_Cell* HashTableInsert(Lips_Machine* itnerpreter,
                                  HashTable* ht, StringData* key, Lips_Cell value);
static Lips_Cell* HashTableSearch(Lips_Machine* interp, const HashTable* ht, const char* key);
static Lips_Cell* HashTableSearchWithHash(Lips_Machine* interp, const HashTable* ht, uint32_t hash, const char* key);
static StringData* HashTableSearchKey(Lips_Machine* interp, const HashTable* ht, uint32_t hash, const char* key, uint32_t n);
static void HashTableIterate(HashTable* ht, Iterator* it);
static int IteratorIsEmpty(const Iterator* it);
static void IteratorGet(const Iterator* it, StringData** key, Lips_Cell* value);
static void IteratorNext(Iterator* it);

static MemChunk* AddMemChunk(Lips_Machine* interpreter, uint32_t size);
static void RemoveMemChunk(Lips_Machine* interpreter, uint32_t i);
static char* ChunkFindSpace(MemChunk* chunk, uint32_t* bytes);
static void ChunkShrink(MemChunk* chunk);
static void ChunkSortRegions(MemChunk* chunk);
static FreeRegion* PushRegion(MemChunk* chunk, uint32_t offset, uint32_t size);
static Bucket* FindBucketForString(Lips_Machine* interpreter);
static MemChunk* FindChunkForString(Lips_Machine* interpreter, uint32_t n);

static void DefineWithCurrent(Lips_Machine* interpreter, Lips_Cell name, Lips_Cell value);
static uint32_t CheckArgumentCount(Lips_Machine* interpreter, Lips_Cell callable, Lips_Cell args);
static void DefineArgumentList(Lips_Machine* interpreter, Lips_Cell callable, Lips_Cell argvalues);
static void DefineArgumentArray(Lips_Machine* interpreter, Lips_Cell callable, uint32_t numargs, Lips_Cell* argvalues);
static void FreeArgumentArray(Lips_Machine* interpreter, uint32_t numargs, Lips_Cell* args);
static Lips_Cell EvalNonPair(Lips_Machine* interpreter, Lips_Cell cell);

static void Mark(Lips_Machine* interpreter);
static void Sweep(Lips_Machine* interpreter);

static LIPS_DECLARE_FUNCTION(list);
static LIPS_DECLARE_FUNCTION(car);
static LIPS_DECLARE_FUNCTION(cdr);
static LIPS_DECLARE_FUNCTION(equal);
static LIPS_DECLARE_FUNCTION(nilp);
static LIPS_DECLARE_FUNCTION(typeof);
static LIPS_DECLARE_FUNCTION(throw);
static LIPS_DECLARE_FUNCTION(call);

static LIPS_DECLARE_MACRO(lambda);
static LIPS_DECLARE_MACRO(macro);
static LIPS_DECLARE_MACRO(define);
static LIPS_DECLARE_MACRO(quote);
static LIPS_DECLARE_MACRO(progn);
static LIPS_DECLARE_MACRO(if);
static LIPS_DECLARE_MACRO(when);
static LIPS_DECLARE_MACRO(catch);
static LIPS_DECLARE_MACRO(intern);
// TODO: implement cond

/// STRUCTS

// Copy-On-Write string
struct StringData {
  uint32_t ptr_offset;
  // 0-3 bits - chunk index
  // 4-20 bits - bucket index
  // 21-31 bits - refcounter
  uint32_t flags;
  // 0-27 bits - string length without null terminator
  // 28-31 bits - allocated space - length
  uint32_t length;
  uint32_t hash;
};

struct HashTable {
  uint32_t allocated;
  // 0-30 bits - number of elements in hash table
  // 31 bit - constant flag
  uint32_t flags;
  uint32_t parent;
};
#define HASH_TABLE_DATA(ht) (Node*)((HashTable*)ht+1)
#define HASH_TABLE_CONSTANT_FLAG (1<<31)
#define HASH_TABLE_GET_SIZE(ht) ((ht)->flags & ~HASH_TABLE_CONSTANT_FLAG)
#define HASH_TABLE_SET_SIZE(ht, sz) (ht)->flags = ((ht)->flags & HASH_TABLE_CONSTANT_FLAG) | (sz)

struct Iterator {
  Node* node;
  uint32_t size;
};

struct Stack {
  uint8_t* data;
  uint32_t left;
  uint32_t right;
  uint32_t size;
};
// for debug
#define PRINT_STACK_DATA(stack) do {                                    \
    printf("Stack[offset=%u, size=%u] data={", (stack).offset, (stack).size); \
    for (uint32_t i = 0; i < (stack).offset; i++) {                     \
      putchar((stack).data[i]);                                         \
    }                                                                   \
    printf("}\n");                                                      \
  } while (0)

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
    struct {
      Lips_Macro ptr;
      void* udata;
    } cmacro;
  } data;
};
#define GET_TYPE(cell) ((cell)->type & 255)
#define GET_NUMARGS(cell) (((cell)->type >> 8) & 255)
#define GET_INTEGER(cell) (cell)->data.integer
#define GET_REAL(cell) (cell)->data.real
#define GET_STRING(cell) (cell)->data.str->ptr
#define GET_HEAD(cell) (cell)->data.list.head
#define GET_TAIL(cell) (cell)->data.list.tail
#define GET_CFUNC(cell) (cell)->data.cfunc
#define GET_CMACRO(cell) (cell)->data.cmacro
#define GET_LFUNC(cell) (cell)->data.lfunc
#define GET_HEAD_TYPE(cell) GET_TYPE(GET_HEAD(cell))
#define GET_TAIL_TYPE(cell) GET_TYPE(GET_TAIL(cell))

struct Node {
  StringData* key;
  Lips_Cell value;
};
#define NODE_VALID(node) ((node).value != NULL)

struct Bucket {
  void* data;
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
  union {
    Lips_Cell* array;
    Lips_Cell list;
  } args;
  Lips_Cell passed_args;
  union {
    struct {
      Lips_Cell* last;
      uint32_t count;
    } args;
    struct {
      Lips_Cell code;
      uint32_t parent;
    } exec;
  } data;
  uint32_t flags;
  // parent's position in stack
  uint32_t parent;
};
#define ES_STAGE_EVALUATING_ARGS 0
#define ES_STAGE_EXECUTING_CODE 1
#define ES_NUM_ENVS(es) ((es)->flags >> 1)
#define ES_STAGE(es) ((es)->flags & 1)
#define ES_INC_NUM_ENVS(es) ((es)->flags += 2)
#define ES_SET_STAGE(es, stage) (es)->flags = ((es)->flags & ~1) | stage
#define ES_ARG_COUNT(es) (es)->data.args.count
#define ES_LAST_ARG(es) (es)->data.args.last
#define ES_CODE(es) (es)->data.exec.code
#define ES_CATCH_PARENT(es) (es)->data.exec.parent

struct FreeRegion {
  uint32_t offset;
  uint32_t size;
  uint32_t lhs;
  uint32_t rhs;
};
#define REGION_LEFT(chunk, region) ((region)->lhs == (uint32_t)-1 ? NULL : (FreeRegion*)&(chunk)->data[(region)->lhs])
#define REGION_RIGHT(chunk, region) ((region)->rhs == (uint32_t)-1 ? NULL : (FreeRegion*)&(chunk)->data[(region)->rhs])

struct MemChunk {
  char* data;
  uint32_t numbytes;
  uint32_t used;
  uint32_t available;
  FreeRegion* first;
  FreeRegion* last;
  uint32_t numregions;
  uint32_t deletions;
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

struct Lips_Machine {
  Lips_AllocFunc alloc;
  Lips_DeallocFunc dealloc;
  // pools for cells
  Bucket* buckets;
  uint32_t numbuckets;
  uint32_t allocbuckets;
  // pools for string objects
  Bucket* str_buckets;
  uint32_t numstr_buckets;
  uint32_t allocstr_buckets;
  MemChunk chunks[MAX_CHUNKS];
  uint32_t numchunks;
  Stack stack;
  uint32_t envpos;
  uint32_t evalpos;
  uint32_t catchpos;
  Lips_Cell throwvalue;
  Lips_Cell default_file;
  Lips_Cell S_nil;
  Lips_Cell S_t;
  Lips_Cell S_filename;
  Lips_Cell T_integer;
  Lips_Cell T_real;
  Lips_Cell T_string;
  Lips_Cell T_symbol;
  Lips_Cell T_pair;
  Lips_Cell T_function;
  Lips_Cell T_macro;
  char errbuff[1024];
};
#define CURRENT_EVAL_STATE(interp) (EvalState*)((interp)->stack.data + (interp)->evalpos)

/// FUNCTIONS

LIPS_COLD_FUNCTION Lips_Machine*
Lips_CreateMachine(Lips_AllocFunc alloc, Lips_DeallocFunc dealloc)
{
  Lips_Machine* interp;
  interp = (Lips_Machine*)alloc(sizeof(Lips_Machine));
  if (interp == NULL) return NULL;
  interp->alloc = alloc;
  interp->dealloc = dealloc;
  interp->numbuckets = 0;
  interp->buckets = (Bucket*)alloc(sizeof(Bucket));
  interp->allocbuckets = 1;
  interp->numchunks = 0;
  AddMemChunk(interp, 1024);
  interp->str_buckets = (Bucket*)alloc(sizeof(Bucket));
  interp->allocstr_buckets = 1;
  interp->numstr_buckets = 0;
  CreateStack(alloc, &interp->stack, 16*1024);
  HashTable* env = HashTableCreate(interp->alloc, interp->dealloc, &interp->stack);
  env->parent = STACK_INVALID_POS;
  interp->envpos = ((uint8_t*)env - interp->stack.data);
  interp->evalpos = STACK_INVALID_POS;
  interp->catchpos = STACK_INVALID_POS;
  // define builtins
  interp->default_file = Lips_NewString(interp, "<eval>");
  interp->S_nil = Lips_Define(interp, "nil", Lips_NewPair(interp, NULL, NULL));
  // important optimization: this allows to reuse string "...",
  // so anytime we encounter "..." in code we don't allocate a new string
  Lips_Define(interp, "...", interp->S_nil);
  // t is just an integer
  // FIXME: do I need to add a new type for "t"?
  interp->S_t = Lips_Define(interp, "t", Lips_NewInteger(interp, 1));
  interp->S_filename = Lips_NewPair(interp, NULL, NULL);

  LIPS_DEFINE_FUNCTION(interp, list, LIPS_NUM_ARGS_VAR, NULL);
  LIPS_DEFINE_FUNCTION(interp, car, LIPS_NUM_ARGS_1, NULL);
  LIPS_DEFINE_FUNCTION(interp, cdr, LIPS_NUM_ARGS_1, NULL);
  Lips_Cell S_equal = LIPS_DEFINE_FUNCTION(interp, equal, LIPS_NUM_ARGS_2|LIPS_NUM_ARGS_VAR, NULL);
  Lips_Define(interp, "=", S_equal);
  Lips_Cell S_nilp = LIPS_DEFINE_FUNCTION(interp, nilp, LIPS_NUM_ARGS_1, NULL);
  Lips_Define(interp, "not", S_nilp);
  LIPS_DEFINE_FUNCTION(interp, typeof, LIPS_NUM_ARGS_1, NULL);
  LIPS_DEFINE_FUNCTION(interp, throw, LIPS_NUM_ARGS_1, NULL);
  LIPS_DEFINE_FUNCTION(interp, call, LIPS_NUM_ARGS_2, NULL);

  LIPS_DEFINE_MACRO(interp, lambda, LIPS_NUM_ARGS_2|LIPS_NUM_ARGS_VAR, NULL);
  LIPS_DEFINE_MACRO(interp, macro, LIPS_NUM_ARGS_2|LIPS_NUM_ARGS_VAR, NULL);
  LIPS_DEFINE_MACRO(interp, define, LIPS_NUM_ARGS_2, NULL);
  LIPS_DEFINE_MACRO(interp, quote, LIPS_NUM_ARGS_1, NULL);
  LIPS_DEFINE_MACRO(interp, progn, LIPS_NUM_ARGS_1|LIPS_NUM_ARGS_VAR, NULL);
  LIPS_DEFINE_MACRO(interp, if, LIPS_NUM_ARGS_3|LIPS_NUM_ARGS_VAR, NULL);
  LIPS_DEFINE_MACRO(interp, when, LIPS_NUM_ARGS_2|LIPS_NUM_ARGS_VAR, NULL);
  LIPS_DEFINE_MACRO(interp, catch, LIPS_NUM_ARGS_1|LIPS_NUM_ARGS_VAR, NULL);
  LIPS_DEFINE_MACRO(interp, intern, LIPS_NUM_ARGS_1, NULL);

  interp->T_integer  = Lips_NewSymbol(interp, "integer");
  interp->T_real     = Lips_NewSymbol(interp, "real");
  interp->T_string   = Lips_NewSymbol(interp, "string");
  interp->T_symbol   = Lips_NewSymbol(interp, "symbol");
  interp->T_pair     = Lips_NewSymbol(interp, "pair");
  interp->T_function = Lips_NewSymbol(interp, "function");
  interp->T_macro    = Lips_NewSymbol(interp, "macro");

  return interp;
}

LIPS_COLD_FUNCTION Lips_Machine*
Lips_DefaultCreateMachine()
{
  return Lips_CreateMachine(&DefaultAlloc, &DefaultDealloc);
}

LIPS_COLD_FUNCTION void
Lips_DestroyMachine(Lips_Machine* interpreter)
{
  PopEnv(interpreter);
  assert(interpreter->envpos == STACK_INVALID_POS &&
         "Tried to call Lips_DestroyInterpreter while running Lisp code");
  // clear all resources
  Lips_DeallocFunc dealloc = interpreter->dealloc;
  DestroyStack(dealloc, &interpreter->stack);
  for (uint32_t i = 0; i < interpreter->numbuckets; i++) {
    // destroy each cell in bucket
    Bucket* bucket = &interpreter->buckets[i];
    for (uint32_t i = 0; bucket->size > 0; i++) {
      Lips_Cell cell = (Lips_Value*)bucket->data + i;
      if ((cell->type & DEAD_MASK) == 0) {
        DestroyCell(interpreter, cell);
        bucket->size--;
      }
    }
    DestroyBucket(interpreter, bucket, sizeof(Lips_Value));
  }
  for (uint32_t i = 0; i < interpreter->numstr_buckets; i++)
    DestroyBucket(interpreter, &interpreter->str_buckets[i], sizeof(StringData));
  for (uint32_t i = interpreter->numchunks; i > 0; i--) {
    RemoveMemChunk(interpreter, i-1);
  }
  dealloc(interpreter->buckets, sizeof(Bucket) * interpreter->allocbuckets);
  dealloc(interpreter->str_buckets, sizeof(Bucket) * interpreter->allocstr_buckets);
  dealloc(interpreter, sizeof(Lips_Machine));
}

LIPS_HOT_FUNCTION Lips_Cell
Lips_Eval(Lips_Machine* interp, Lips_Cell cell)
{
#if 0
  // recursive version(easily readable)
  switch (GET_TYPE(cell)) {
  default: assert(0 && "Value has undefined type");
  case LIPS_TYPE_INTEGER:
  case LIPS_TYPE_REAL:
  case LIPS_TYPE_STRING:
    return cell;
  case LIPS_TYPE_SYMBOL:
    return Lips_InternCell(interpreter, cell);
  case LIPS_TYPE_PAIR: {
    Lips_Cell name = GET_HEAD(cell);
    Lips_Cell args = GET_TAIL(cell);
    TYPE_CHECK(interpreter, LIPS_TYPE_SYMBOL, name);
    Lips_Cell callable = Lips_InternCell(interpreter, name);
    if (callable == NULL || callable == interpreter->S_nil) {
      Lips_ThrowError(interpreter, "Eval: undefined symbol '%s'", GET_STR(name)->ptr);
    }
    return Lips_Invoke(interpreter, callable, args);
  }
  }
#else
  // don't even try to understand...
  // you just need to know that this is a non-recursive eval loop
  if (GET_TYPE(cell) == LIPS_TYPE_PAIR) {
    Lips_Cell ret;
    Lips_Cell name;
    const uint32_t startpos = interp->evalpos;
    EvalState* state = PushEvalState(interp);
    state->sexp = cell;
  eval:
    state->flags = 0;
    state->passed_args = GET_TAIL(state->sexp);
    name = GET_HEAD(state->sexp);
    if (LIPS_UNLIKELY(name == NULL)) {
      ret = interp->S_nil;
    } else {
      if (LIPS_UNLIKELY(!Lips_IsSymbol(name))) {
        Lips_SetError(interp, "First part of evaluated s-expression always must be a symbol");
        goto except;
      }
      state->callable = Lips_InternCell(interp, name);
      if (LIPS_UNLIKELY(state->callable == interp->S_nil)) {
        Lips_SetError(interp, "Eval: undefined symbol '%s'", GET_STR_PTR(interp, name));
        goto except;
      }
      ES_ARG_COUNT(state) = CheckArgumentCount(interp, state->callable, state->passed_args);
      if (LIPS_UNLIKELY(ES_ARG_COUNT(state) == (uint32_t)-1)) {
        goto except;
      }
      if (Lips_IsFunction(state->callable)) {
        // TODO: manage variable number of arguments
        state->args.array = StackRequireFromBack(interp->alloc, interp->dealloc, &interp->stack,
                                                 ES_ARG_COUNT(state) * sizeof(Lips_Cell));
        ES_LAST_ARG(state) = state->args.array;
      arg:
        while (state->passed_args) {
          // eval arguments
          Lips_Cell argument = GET_HEAD(state->passed_args);
          if (GET_TYPE(argument) == LIPS_TYPE_PAIR) {
            state = PushEvalState(interp);
            state->sexp = argument;
            goto eval;
          } else {
            *ES_LAST_ARG(state) = EvalNonPair(interp, argument);
            ES_LAST_ARG(state)++;
            state->passed_args = GET_TAIL(state->passed_args);
          }
        }
      } else {
        state->args.list = state->passed_args;
      }
      if (GET_TYPE(state->callable) & ((LIPS_TYPE_C_FUNCTION^LIPS_TYPE_FUNCTION)|
                                       (LIPS_TYPE_C_MACRO^LIPS_TYPE_MACRO))) {
        Lips_Cell c = state->callable;
        if (Lips_IsFunction(c)) {
          // every function must be executed inside it's own environment
          PushEnv(interp);
          ret = GET_CFUNC(c).ptr(interp, ES_ARG_COUNT(state), state->args.array, GET_CFUNC(c).udata);
          // array of arguments no more needed; we can free it
          FreeArgumentArray(interp, ES_ARG_COUNT(state), state->args.array);
          PopEnv(interp);
        } else {
          ret = GET_CMACRO(c).ptr(interp, state->args.list, GET_CMACRO(c).udata);
        }
        // fucntion returned null so need to go to latest catch
        if (LIPS_UNLIKELY(ret == NULL)) {
          if (ES_STAGE(state) == ES_STAGE_EXECUTING_CODE) {
            goto code;
          }
        except:
          if (interp->catchpos >= startpos) {
            // unhandled throw
            PopEvalState(interp);
            return NULL;
          }
          state = UnwindStack(interp);
          ret = interp->throwvalue;
        }
      } else {
        // push a new environment
        if (ES_ARG_COUNT(state) > 0 || Lips_IsFunction(state->callable)) {
          HashTable* env = PushEnv(interp);
          ES_INC_NUM_ENVS(state);
          if (ES_ARG_COUNT(state) > 0) {
            if (Lips_IsFunction(state->callable)) {
              DefineArgumentArray(interp, state->callable, ES_ARG_COUNT(state), state->args.array);
              // array of arguments no more needed; we can free it
              FreeArgumentArray(interp, ES_ARG_COUNT(state), state->args.array);
            } else {
              DefineArgumentList(interp, state->callable, state->args.list);
              env->flags |= HASH_TABLE_CONSTANT_FLAG;
            }
          }
        }
        // execute code
        ES_CODE(state) = GET_LFUNC(state->callable).body;
        ES_SET_STAGE(state, ES_STAGE_EXECUTING_CODE);
      code:
        while (ES_CODE(state)) {
          Lips_Cell expression = GET_HEAD(ES_CODE(state));
          if (GET_TYPE(expression) == LIPS_TYPE_PAIR) {
            // TODO: implement tail call optimization
            state = PushEvalState(interp);
            state->sexp = expression;
            goto eval;
          } else {
            ret = EvalNonPair(interp, expression);
          }
          ES_CODE(state) = GET_TAIL(ES_CODE(state));
        }
      }
    }
    state = PopEvalState(interp);
    if (interp->evalpos != startpos) {
      switch (ES_STAGE(state)) {
      case ES_STAGE_EVALUATING_ARGS:
        *ES_LAST_ARG(state) = ret;
        ES_LAST_ARG(state)++;
        state->passed_args = GET_TAIL(state->passed_args);
        goto arg;
      case ES_STAGE_EXECUTING_CODE:
        ES_CODE(state) = GET_TAIL(ES_CODE(state));
        goto code;
      }
    }
    return ret;
  } else {
    return EvalNonPair(interp, cell);
  }
#endif
  return cell;
}

Lips_Cell
Lips_EvalString(Lips_Machine* interpreter, const char* str, const char* filename)
{
  Parser parser;
  ParserInit(&parser, str, strlen(str));
  Lips_Cell ast = GenerateAST(interpreter, &parser);
  Lips_Cell str_filename;
  if (filename == NULL)  {
    str_filename = interpreter->default_file;
  } else {
    str_filename = Lips_NewString(interpreter, filename);
  }
  Lips_Cell temp = Lips_ListPushBack(interpreter, interpreter->S_filename, str_filename);
  Lips_Cell ret = Lips_Eval(interpreter, ast);
  GET_TAIL(temp) = NULL; // this equals to Lips_ListPop(interpreter, interpreter->S_filename);
  return ret;
}

const char*
Lips_GetError(const Lips_Machine* interpreter)
{
  return interpreter->errbuff;
}

LIPS_HOT_FUNCTION void
Lips_GarbageCollect(Lips_Machine* interpreter)
{
  Mark(interpreter);
  Sweep(interpreter);
}

Lips_Cell
Lips_Nil(Lips_Machine* interpreter)
{
  return interpreter->S_nil;
}

Lips_Cell
Lips_NewInteger(Lips_Machine* interpreter, int64_t num)
{
  Lips_Cell cell = NewCell(interpreter);
  cell->type = LIPS_TYPE_INTEGER;
  cell->data.integer = num;
  return cell;
}

Lips_Cell
Lips_NewReal(Lips_Machine* interpreter, double num)
{
  Lips_Cell cell = NewCell(interpreter);
  cell->type = LIPS_TYPE_REAL;
  cell->data.real = num;
  return cell;
}

Lips_Cell
Lips_NewString(Lips_Machine* interpreter, const char* str)
{
  return Lips_NewStringN(interpreter, str, strlen(str));
}

Lips_Cell
Lips_NewStringN(Lips_Machine* interpreter, const char* str, uint32_t n)
{
  Lips_Cell cell = NewCell(interpreter);
  cell->type = LIPS_TYPE_STRING;
  GET_STR(cell) = StringCreate(interpreter, str, n);
  STR_DATA_REF_INC(GET_STR(cell));
  return cell;
}

Lips_Cell
Lips_NewSymbol(Lips_Machine* interpreter, const char* str)
{
  return Lips_NewSymbolN(interpreter, str, strlen(str));
}

Lips_Cell
Lips_NewSymbolN(Lips_Machine* interpreter, const char* str, uint32_t n)
{
  Lips_Cell cell = NewCell(interpreter);
  cell->type = LIPS_TYPE_SYMBOL;
  StringData* data = NULL;
  // try to find string in environments so we don't waste space
  HashTable* env = InterpreterEnv(interpreter);
  uint32_t hash = ComputeHashN(str, n);
  do {
    data = HashTableSearchKey(interpreter, env, hash, str, n);
    if (data)
      goto skip;
    env = EnvParent(interpreter, env);
  } while (env);
  data = StringCreateWithHash(interpreter, str, n, hash);
 skip:
  GET_STR(cell) = data;
  STR_DATA_REF_INC(GET_STR(cell));
  return cell;
}

Lips_Cell
Lips_NewPair(Lips_Machine* interpreter, Lips_Cell head, Lips_Cell tail)
{
  Lips_Cell cell = NewCell(interpreter);
  cell->type = LIPS_TYPE_PAIR;
  cell->data.list.head = head;
  cell->data.list.tail = tail;
  return cell;
}

Lips_Cell
Lips_NewList(Lips_Machine* interpreter, uint32_t numCells, Lips_Cell* cells)
{
  Lips_Cell list = Lips_NewPair(interpreter, NULL, NULL);
  Lips_Cell curr = list;
  while (numCells--) {
    GET_HEAD(curr) = *cells;
    if (numCells > 0) {
      GET_TAIL(curr) = Lips_NewPair(interpreter, NULL, NULL);
      curr = GET_TAIL(curr);
    }
    cells++;
  }
  return list;
}

Lips_Cell
Lips_NewFunction(Lips_Machine* interpreter, Lips_Cell args, Lips_Cell body, uint8_t numargs)
{
  Lips_Cell cell = NewCell(interpreter);
  cell->type = LIPS_TYPE_FUNCTION | (numargs << 8);
  // TODO: check all arguments are symbols
  GET_LFUNC(cell).args = args;
  GET_LFUNC(cell).body = body;
  return cell;
}

Lips_Cell
Lips_NewMacro(Lips_Machine* interpreter, Lips_Cell args, Lips_Cell body, uint8_t numargs)
{
  Lips_Cell cell = NewCell(interpreter);
  cell->type = LIPS_TYPE_MACRO | (numargs << 8);
  GET_LFUNC(cell).args = args;
  GET_LFUNC(cell).body = body;
  return cell;
}

Lips_Cell
Lips_NewCFunction(Lips_Machine* interpreter, Lips_Func function, uint8_t numargs, void* udata)
{
  Lips_Cell cell = NewCell(interpreter);
  cell->type = LIPS_TYPE_C_FUNCTION | (numargs << 8);
  GET_CFUNC(cell).ptr = function;
  GET_CFUNC(cell).udata = udata;
  return cell;
}

Lips_Cell
Lips_NewCMacro(Lips_Machine* interpreter, Lips_Macro function, uint8_t numargs, void* udata)
{
  Lips_Cell cell = NewCell(interpreter);
  cell->type = LIPS_TYPE_C_MACRO | (numargs << 8);
  GET_CMACRO(cell).ptr = function;
  GET_CMACRO(cell).udata = udata;
  return cell;
}

uint32_t
Lips_GetType(const Lips_Cell cell)
{
  return GET_TYPE(cell);
}

uint32_t
Lips_PrintCell(Lips_Machine* interpreter, Lips_Cell cell, char* buff, uint32_t size)
{
  char* ptr = buff;
#define PRINT(...) ptr += snprintf(ptr, size - (ptr - buff), __VA_ARGS__)
#if 0
  // this is an implementation with recursion used
  switch (GET_TYPE(cell)) {
  default: return 0;
  case LIPS_TYPE_INTEGER:
    PRINT("%ld", GET_INTEGER(cell));
    break;
  case LIPS_TYPE_REAL:
    PRINT("%f", GET_REAL(cell));
    break;
  case LIPS_TYPE_STRING:
    PRINT("\"%s\"", GET_STRING(cell));
    break;
  case LIPS_TYPE_SYMBOL:
    PRINT("%s", GET_STRING(cell));
    break;
  case LIPS_TYPE_PAIR:
    PRINT("(");
    if (GET_HEAD(cell)) {
      ptr += Lips_PrintCell(interpreter, GET_HEAD(cell), ptr, size - (ptr - buff));
      cell = GET_TAIL(cell);
      while (cell && GET_HEAD(cell)) {
        PRINT(" ");
        ptr += Lips_PrintCell(interpreter, GET_HEAD(cell), ptr, size - (ptr - buff));
        if (!GET_TAIL(cell)) break;
        cell = GET_TAIL(cell);
      }
    }
    PRINT(")");
    break;
  }
#else
  if (GET_TYPE(cell) == LIPS_TYPE_PAIR) {
    uint32_t counter = 0;
    Lips_Cell* prev = NULL;
    PRINT("(");
    while (cell != NULL) {
      // TODO: do checks for buffer overflow
      if (GET_HEAD(cell) != NULL) {
        switch (GET_HEAD_TYPE(cell)) {
        default: assert(0);
        case LIPS_TYPE_INTEGER:
          PRINT("%ld", GET_INTEGER(GET_HEAD(cell)));
          break;
        case LIPS_TYPE_REAL:
          PRINT("%f", GET_REAL(GET_HEAD(cell)));
          break;
        case LIPS_TYPE_STRING:
          PRINT("\"%s\"", GET_STR_PTR(interpreter, GET_HEAD(cell)));
          break;
        case LIPS_TYPE_SYMBOL:
          PRINT("%s", GET_STR_PTR(interpreter, GET_HEAD(cell)));
          break;
        case LIPS_TYPE_PAIR:
          PRINT("(");
          prev = StackRequire(interpreter->alloc, interpreter->dealloc,
                              &interpreter->stack, sizeof(Lips_Cell));
          *prev = GET_TAIL(cell);
          cell = GET_HEAD(cell);
          counter++;
          goto skip;
        case LIPS_TYPE_FUNCTION:
        case LIPS_TYPE_C_FUNCTION: {
          uint32_t num = GET_NUMARGS(GET_HEAD(cell));
          if (num & LIPS_NUM_ARGS_VAR)
            PRINT("<func(%d+)>", num & (LIPS_NUM_ARGS_VAR-1));
          else
            PRINT("<func(%d)>", num);
        }
          break;
        case LIPS_TYPE_MACRO:
        case LIPS_TYPE_C_MACRO: {
          uint32_t num = GET_NUMARGS(GET_HEAD(cell));
          if (num & LIPS_NUM_ARGS_VAR)
            PRINT("<macro(%d+)>", num & (LIPS_NUM_ARGS_VAR-1));
          else
            PRINT("<macro(%d)>", num);
        }
          break;
        }
      }
      cell = GET_TAIL(cell);
      if (cell != NULL) {
        PRINT(" ");
      }
    skip:
      if (cell == NULL) {
        PRINT(")");
        if (counter == 0) {
          return ptr - buff;
        }
        cell = *prev;
        assert(StackRelease(&interpreter->stack, prev) == sizeof(Lips_Cell));
        prev--;
        counter--;
        if (cell == NULL) {
          goto skip;
        } else {
          PRINT(" ");
        }
      }
    }
  } else {
    switch (GET_TYPE(cell)) {
    default: return 0;
    case LIPS_TYPE_INTEGER:
      PRINT("%ld", GET_INTEGER(cell));
      break;
    case LIPS_TYPE_REAL:
      PRINT("%f", GET_REAL(cell));
      break;
    case LIPS_TYPE_STRING:
      PRINT("\"%s\"", GET_STR_PTR(interpreter, cell));
      break;
    case LIPS_TYPE_SYMBOL:
      PRINT("%s", GET_STR_PTR(interpreter, cell));
      break;
    case LIPS_TYPE_FUNCTION:
    case LIPS_TYPE_C_FUNCTION: {
      uint32_t num = GET_NUMARGS(cell);
      if (num & LIPS_NUM_ARGS_VAR)
        PRINT("<func(%d+)>", num & (LIPS_NUM_ARGS_VAR-1));
      else
        PRINT("<func(%d)>", num);
    }
      break;
    case LIPS_TYPE_MACRO:
    case LIPS_TYPE_C_MACRO: {
      uint32_t num = GET_NUMARGS(cell);
      if (num & LIPS_NUM_ARGS_VAR)
        PRINT("<macro(%d+)>", num & (LIPS_NUM_ARGS_VAR-1));
      else
        PRINT("<macro(%d)>", num);
    }
      break;
    }
  }
#endif
#undef PRINT
  return ptr - buff;
}

uint32_t
Lips_ListLength(Lips_Machine* interp, Lips_Cell list)
{
  (void)interp;
  TYPE_CHECK_FORCED(LIPS_TYPE_PAIR, list);
  uint32_t count = 0;
  if (GET_HEAD(list) == NULL) {
    assert(GET_TAIL(list) == NULL && "internal error: list semantic error");
  } else {
    while (list) {
      count++;
      assert(GET_HEAD(list) != NULL && "internal error: list semantic error");
      list = GET_TAIL(list);
    }
  }
  return count;
}

Lips_Cell
Lips_ListLastElement(Lips_Machine* interpreter, Lips_Cell list, uint32_t* length)
{
  TYPE_CHECK(interpreter, LIPS_TYPE_PAIR, list);
  Lips_Cell ret;
  uint32_t count = 0;
  if (GET_HEAD(list) == NULL) {
    assert(GET_TAIL(list) == NULL && "internal error: list semantic error");
    ret = NULL;
  } else {
    while (list) {
      count++;
      assert(GET_HEAD(list) != NULL && "internal error: list semantic error");
      ret = GET_HEAD(list);
      list = GET_TAIL(list);
    }
  }
  if (length) {
    *length = count;
  }
  return ret;
}

Lips_Cell
Lips_ListPushBack(Lips_Machine* interp, Lips_Cell list, Lips_Cell elem)
{
  TYPE_CHECK(interp, LIPS_TYPE_PAIR, list);
  if (GET_HEAD(list) == NULL) {
    GET_HEAD(list) = elem;
  } else {
    while (GET_TAIL(list) != NULL) {
      list = GET_TAIL(list);
    }
    GET_TAIL(list) = Lips_NewPair(interp, elem, NULL);
  }
  return list;
}

Lips_Cell
Lips_ListPopBack(Lips_Machine* interp, Lips_Cell list)
{
  TYPE_CHECK(interp, LIPS_TYPE_PAIR, list);
  Lips_Cell ret;
  if (GET_TAIL(list) == NULL) {
    assert(GET_HEAD(list) && "empty list");
    ret = GET_HEAD(list);
    GET_HEAD(list) = NULL;
  } else {
    Lips_Cell temp;
    do {
      temp = list;
      list = GET_TAIL(list);
    } while (GET_TAIL(list) != NULL);
    ret = GET_HEAD(list);
    GET_HEAD(list) = NULL;
    GET_TAIL(temp) = NULL;
  }
  return ret;
}

Lips_Cell
Lips_Define(Lips_Machine* interpreter, const char* name, Lips_Cell cell)
{
  assert(cell);
  HashTable* env = InterpreterEnv(interpreter);
  while (env->flags & HASH_TABLE_CONSTANT_FLAG) {
    env = EnvParent(interpreter, env);
  }
  StringData* key = StringCreate(interpreter, name, strlen(name));
  Lips_Cell* ptr = HashTableInsert(interpreter, env, key, cell);
  STR_DATA_REF_INC(key);
  if (ptr == NULL) {
    LIPS_THROW_ERROR(interpreter, "Value '%s' is already defined", name);
  }
  return cell;
}

Lips_Cell
Lips_DefineCell(Lips_Machine* interpreter, Lips_Cell cell, Lips_Cell value)
{
  TYPE_CHECK(interpreter, LIPS_TYPE_SYMBOL|LIPS_TYPE_STRING, cell);
  assert(value);
  HashTable* env = InterpreterEnv(interpreter);
  while (env->flags & HASH_TABLE_CONSTANT_FLAG) {
    env = EnvParent(interpreter, env);
  }
  Lips_Cell* ptr = HashTableInsert(interpreter, env, GET_STR(cell), value);
  STR_DATA_REF_INC(GET_STR(cell));
  if (ptr == NULL) {
    LIPS_THROW_ERROR(interpreter, "Value '%s' is already defined");
  }
  return value;
}

Lips_Cell
Lips_Intern(Lips_Machine* interpreter, const char* name)
{
  HashTable* env = InterpreterEnv(interpreter);
  do {
    Lips_Cell* ptr = HashTableSearch(interpreter, env, name);
    if (ptr) {
      return *ptr;
    }
    env = EnvParent(interpreter, env);
  } while (env);
  return NULL;
}

LIPS_HOT_FUNCTION Lips_Cell
Lips_InternCell(Lips_Machine* interpreter, Lips_Cell cell)
{
  TYPE_CHECK(interpreter, LIPS_TYPE_SYMBOL|LIPS_TYPE_STRING, cell);
  HashTable* env = InterpreterEnv(interpreter);
  do {
    Lips_Cell* ptr = HashTableSearchWithHash(interpreter, env,
                                             GET_STR(cell)->hash, GET_STR_PTR(interpreter, cell));
    if (ptr) {
      return *ptr;
    }
    env = EnvParent(interpreter, env);
  } while (env);
  return interpreter->S_nil;
}

LIPS_HOT_FUNCTION Lips_Cell
Lips_Invoke(Lips_Machine* interpreter, Lips_Cell callable, Lips_Cell args)
{
  Lips_Cell ast = Lips_NewPair(interpreter, callable, args);
  Lips_Cell ret = Lips_Eval(interpreter, ast);
  // FIXME: should we destroy 'ast'?
  return ret;
}

const char*
Lips_SetError(Lips_Machine* interpreter, const char* fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  int len = vsnprintf(interpreter->errbuff, sizeof(interpreter->errbuff), fmt, ap);
  va_end(ap);
  interpreter->throwvalue = Lips_NewStringN(interpreter, interpreter->errbuff, len);
  return interpreter->errbuff;
}

void
Lips_CalculateMemoryStats(Lips_Machine* interpreter, Lips_MemoryStats* stats)
{
  stats->allocated_bytes = 0;
  stats->cell_allocated_bytes = 0;
  stats->cell_used_bytes = 0;
  stats->str_allocated_bytes = 0;
  stats->str_used_bytes = 0;

  stats->allocated_bytes += sizeof(Lips_Machine);
  stats->allocated_bytes += sizeof(Bucket) * interpreter->allocbuckets;
  stats->allocated_bytes += sizeof(Bucket) * interpreter->allocstr_buckets;
  stats->allocated_bytes += interpreter->stack.size;

  for (uint32_t i = 0; i < interpreter->numbuckets; i++) {
    Bucket* bucket = &interpreter->buckets[i];
    stats->cell_allocated_bytes += BUCKET_SIZE * sizeof(Lips_Value);
    Lips_Value* const data = bucket->data;
    for (uint32_t i = 0, n = bucket->size; n > 0; i++) {
      uint32_t mask = *(uint32_t*)&data[i];
      if (mask ^ DEAD_MASK) {
        stats->cell_used_bytes += sizeof(Lips_Value);
        n--;
      }
    }
  }
  for (uint32_t i = 0; i < interpreter->numstr_buckets; i++) {
    Bucket* bucket = &interpreter->str_buckets[i];
    stats->str_allocated_bytes += BUCKET_SIZE * sizeof(StringData);
    StringData* const data = bucket->data;
    for (uint32_t i = 0, n = bucket->size; n > 0; i++) {
      uint32_t mask = *(uint32_t*)&data[i];
      if (mask ^ DEAD_MASK) {
        stats->str_used_bytes += sizeof(StringData);
        n--;
      }
    }
  }
  for (uint32_t i = 0; i < interpreter->numchunks; i++) {
    MemChunk* chunk = &interpreter->chunks[i];
    stats->str_allocated_bytes += chunk->numbytes;
    stats->str_used_bytes += chunk->used;
  }
  stats->allocated_bytes += stats->cell_allocated_bytes + stats->str_allocated_bytes;
}

int64_t
Lips_GetInteger(Lips_Machine* interpreter, Lips_Cell cell)
{
  (void)interpreter;
  TYPE_CHECK_FORCED(LIPS_TYPE_INTEGER, cell);
  return GET_INTEGER(cell);
}

double
Lips_GetReal(Lips_Machine* interpreter, Lips_Cell cell)
{
  (void)interpreter;
  TYPE_CHECK_FORCED(LIPS_TYPE_REAL, cell);
  return GET_REAL(cell);
}

const char*
Lips_GetString(Lips_Machine* interpreter, Lips_Cell cell)
{
  (void)interpreter;
  TYPE_CHECK(interpreter, LIPS_TYPE_STRING|LIPS_TYPE_SYMBOL, cell);
  return GET_STR_PTR(interpreter, cell);
}

Lips_Cell
Lips_CAR(Lips_Machine* interpreter, Lips_Cell cell)
{
  (void)interpreter;
  TYPE_CHECK(interpreter, LIPS_TYPE_PAIR, cell);
  return GET_HEAD(cell);
}

Lips_Cell
Lips_CDR(Lips_Machine* interpreter, Lips_Cell cell)
{
  (void)interpreter;
  TYPE_CHECK(interpreter, LIPS_TYPE_PAIR, cell);
  return GET_TAIL(cell);
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
DestroyCell(Lips_Machine* interpreter, Lips_Cell cell) {
  switch (GET_TYPE(cell)) {
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
    StringDestroy(interpreter, GET_STR(cell));
    break;
  case LIPS_TYPE_PAIR:

    break;
  }
}

StringData*
StringCreate(Lips_Machine* interp, const char* str, uint32_t n)
{
  Bucket* bucket = FindBucketForString(interp);
  StringData* data = BucketNewString(bucket);
  data->flags = STR_DATA_MAKE_BUCKET_INDEX(bucket - interp->str_buckets);
  uint32_t bytes = n+1+sizeof(StringData*);
  MemChunk* chunk = FindChunkForString(interp, bytes);
  data->flags |= STR_DATA_MAKE_CHUNK_INDEX(chunk - interp->chunks);
  char* ptr = ChunkFindSpace(chunk, &bytes);
  *(StringData**)(ptr) = data;
  chunk->used += n+1;
  data->ptr_offset = ptr + sizeof(StringData*) - chunk->data;
  ptr += sizeof(StringData*);
  data->length = STR_DATA_MAKE_LEN(n, bytes - n - sizeof(StringData*));
  strncpy(ptr, str, n);
  ptr[n] = '\0';
  data->hash = ComputeHash(ptr);
  return data;
}

StringData*
StringCreateWithHash(Lips_Machine* interp, const char* str, uint32_t n, uint32_t hash)
{
  Bucket* bucket = FindBucketForString(interp);
  StringData* data = BucketNewString(bucket);
  data->flags = STR_DATA_MAKE_BUCKET_INDEX(bucket - interp->str_buckets);
  uint32_t bytes = n+1+sizeof(StringData*);
  MemChunk* chunk = FindChunkForString(interp, bytes);
  data->flags |= STR_DATA_MAKE_CHUNK_INDEX(chunk - interp->chunks);
  char* ptr = ChunkFindSpace(chunk, &bytes);
  *(StringData**)(ptr) = data;
  chunk->used += n+1;
  data->ptr_offset = ptr + sizeof(StringData*) - chunk->data;
  ptr += sizeof(StringData*);
  data->length = STR_DATA_MAKE_LEN(n, bytes - n - sizeof(StringData*));
  strncpy(ptr, str, n);
  ptr[n] = '\0';
  data->hash = hash;
  return data;
}

void
StringDestroy(Lips_Machine* interp, StringData* str)
{
  STR_DATA_REF_DEC(str);
  if (STR_DATA_REF_COUNT(str) > 0)
    return;
  MemChunk* chunk = &interp->chunks[STR_DATA_CHUNK_INDEX(str)];
  char* ptr = STR_DATA_PTR(chunk, str) - sizeof(StringData*);
  assert(ptr >= chunk->data && ptr + STR_DATA_ALLOCATED(str) <= chunk->data + chunk->numbytes);
  if (chunk->deletions % 128 == 127) {
    ChunkShrink(chunk);
    printf("Chunk shrink!!!\n");
  }
  chunk->deletions++;
  PushRegion(chunk, ptr-chunk->data, STR_DATA_ALLOCATED(str));
  chunk->used -= STR_DATA_LENGTH(str)+1;
  Bucket* bucket = &interp->str_buckets[STR_DATA_BUCKET_INDEX(str)];
  BucketDeleteString(bucket, str);
}

int
StringEqual(Lips_Machine* interpreter, const StringData* lhs, const StringData* rhs)
{
  if (lhs->hash == rhs->hash) {
    if (STR_DATA_LENGTH(lhs) == STR_DATA_LENGTH(rhs)) {
      char* ptr1 = STR_DATA_PTR(STR_PTR_CHUNK(interpreter, lhs), lhs);
      char* ptr2 = STR_DATA_PTR(STR_PTR_CHUNK(interpreter, rhs), rhs);
      if (strcmp(ptr1, ptr2) == 0)
        return 1;
    }
  }
  return 0;
}

const char*
GDB_lips_to_c_string(Lips_Machine* interp, Lips_Cell cell)
{
  // FIXME: this is terrible
  char* buff = interp->errbuff + 512;
  Lips_PrintCell(interp, cell, buff, 512);
  return buff;
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
    assert(0);
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
    while (parser->pos < parser->length && IS_WHITESPACE(text[parser->pos]))
      parser->pos++;
  } while(text[parser->pos] == ';');
  if (parser->pos >= parser->length) return 0;
  if (IS_SPECIAL_CHAR(text[parser->pos])) {
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
      if (IS_WHITESPACE(text[end]) || IS_SPECIAL_CHAR(text[end]))
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
  return IS_DIGIT(token->str[0]) ||
    (token->str[0] == '-' && IS_DIGIT(token->str[1]));
}

Lips_Cell
ParseNumber(Lips_Machine* interpreter, const Token* token) {
  int is_float = 0;
  for (uint32_t i = 0; i < token->length; i++) {
    if (!IS_DIGIT(token->str[i])) {
      if (token->str[i] == '.') {
        is_float++;
      } else {
        LOG_ERROR(interpreter, "Found undefined character '%c' when parsing number in token '%.*s'",
                  token->str[i], token->length, token->str);
        return NULL;
      }
    }
  }
  if (is_float > 1) {
    LOG_ERROR(interpreter, "Encountered more than 1 '.' when parsing float in token '%.*s'",
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
NewCell(Lips_Machine* interp)
{
  for (uint32_t i = interp->numbuckets; i > 0; i--)
    if (interp->buckets[i-1].size < BUCKET_SIZE) {
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
  CreateBucket(interp->alloc, new_bucket, sizeof(Lips_Value));
  interp->numbuckets++;
  return BucketNewCell(new_bucket);
}

Lips_Cell
GenerateAST(Lips_Machine* interpreter, Parser* parser)
{
#if 0
  // this is an implementation with recursion, it is much more readable but a bit slower
  Lips_Cell tree = NULL;
  Lips_Cell cell = NULL;
  int code = ParserNextToken(parser);
  if (code == LIPS_EOF) {
    LOG_ERROR(interpreter, "EOF: expected \"");
  } else if (code == 1) {
    switch (parser->currtok.str[0]) {
    case '(':
      tree = Lips_NewPair(interpreter, NULL, NULL);
      cell = tree;
      while (1) {
        GET_HEAD(cell) = Lips_GenerateAST(interpreter, parser);
        if (GET_HEAD(cell) == NULL)
          break;
        GET_TAIL(cell) = Lips_NewPair(interpreter, NULL, NULL);
        cell = GET_TAIL(cell);
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
        GET_HEAD(cell) = Lips_NewPair(interpreter, NULL, NULL);
        cell = GET_HEAD(cell);
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
      GET_HEAD(cell) = Lips_NewStringN(interpreter,
                                       parser->currtok.str+1, parser->currtok.length-2);
      break;
    default:
      if (Lips_IsTokenNumber(&parser->currtok)) {
        GET_HEAD(cell) = ParseNumber(interpreter, &parser->currtok);
      } else {
        GET_HEAD(cell) = Lips_NewSymbolN(interpreter, parser->currtok.str, parser->currtok.length);
      }
      break;
    }
    code = ParserNextToken(parser);
    // don't waste memory by adding an empty list to the end
    if (parser->currtok.str[0] != ')') {
      // push new cell to the end
      GET_TAIL(cell) = Lips_NewPair(interpreter, NULL, NULL);
      cell = GET_TAIL(cell);
    }
    continue;
  skip_pushing:
    code = ParserNextToken(parser);
  }
  assert(counter == 0 && "parser internal error"); // I think this is useful, should I remove it?
  if (code == LIPS_EOF) {
    LOG_ERROR(interpreter, "EOF: expected \"");
  }
  Lips_Cell ret = stack[0];
  assert(StackRelease(&interpreter->stack, stack) == numbytes);
  return ret;
#endif
}

void
CreateBucket(Lips_AllocFunc alloc, Bucket* bucket, uint32_t elem_size)
{
  uint32_t i;
  bucket->data = (Lips_Value*)alloc(BUCKET_SIZE * elem_size);
  bucket->size = 0;
  bucket->next = 0;
  uint8_t* data = bucket->data;
  for (i = 0; i < BUCKET_SIZE; i++) {
    *(uint32_t*)data = (i + 1) | DEAD_MASK;
    data += elem_size;
  }
}

void
DestroyBucket(Lips_Machine* interpreter, Bucket* bucket, uint32_t elem_size)
{
  // free bucket's memory
  interpreter->dealloc(bucket->data, elem_size * BUCKET_SIZE);
}

Lips_Cell
BucketNewCell(Bucket* bucket)
{
  assert(bucket->size < BUCKET_SIZE && "Bucket out of space");
  Lips_Cell ret = (Lips_Cell)bucket->data + bucket->next;
  bucket->next = *(uint32_t*)ret ^ DEAD_MASK;
  bucket->size++;
  return ret;
}

void
BucketDeleteCell(Bucket* bucket, Lips_Cell cell)
{
  uint32_t index = cell - (Lips_Cell)bucket->data;
  assert(index < BUCKET_SIZE && "cell doesn't belong to this Bucket");
  assert(bucket->size > 0 && "Bucket is empty");
  *(uint32_t*)cell = bucket->next | DEAD_MASK;
  bucket->next = index;
  bucket->size--;
}

StringData*
BucketNewString(Bucket* bucket)
{
  assert(bucket->size < BUCKET_SIZE && "Bucket out of space");
  StringData* ret = (StringData*)bucket->data + bucket->next;
  bucket->next = *(uint32_t*)ret ^ DEAD_MASK;
  bucket->size++;
  return ret;
}

void
BucketDeleteString(Bucket* bucket, StringData* string)
{
  uint32_t index = string - (StringData*)bucket->data;
  assert(index < BUCKET_SIZE && "cell doesn't belong to this Bucket");
  assert(bucket->size > 0 && "Bucket is empty");
  *(uint32_t*)string = bucket->next | DEAD_MASK;
  bucket->next = index;
  bucket->size--;
}

void
CreateStack(Lips_AllocFunc alloc, Stack* stack, uint32_t size) {
  stack->data = alloc(size);
  stack->left = 0;
  stack->right = size;
  stack->size = size;
}

void
DestroyStack(Lips_DeallocFunc dealloc, Stack* stack)
{
  dealloc(stack->data, stack->size);
  stack->data = NULL;
  stack->size = 0;
}

void
StackGrow(Lips_AllocFunc alloc, Lips_DeallocFunc dealloc, Stack* stack)
{
  uint8_t* oldata = stack->data;
  uint32_t oldsize = stack->size;
  // TODO: pick a better grow policy
  stack->size = stack->size * 2;
  stack->data = (uint8_t*)alloc(stack->size);
  // FIXME: should we check for stack->data == NULL?
  memcpy(stack->data, oldata, oldsize);
  dealloc(oldata, oldsize);
  stack->right = stack->size - oldsize + stack->right;
}

void*
StackRequire(Lips_AllocFunc alloc, Lips_DeallocFunc dealloc,
             Stack* stack, uint32_t bytes)
{
  if (stack->left + stack->size - stack->right + bytes > stack->size) {
    StackGrow(alloc, dealloc, stack);
  }
  void* ret = (void*)(stack->data + stack->left);
  stack->left += bytes;
  return ret;
}

uint32_t
StackRelease(Stack* stack, void* data)
{
  assert((uint8_t*)data >= stack->data && (uint8_t*)data <= stack->data + stack->left);
  uint32_t bytes = (stack->data + stack->left) - (uint8_t*)data;
  stack->left -= bytes;
  return bytes;
}

void*
StackRequireFromBack(Lips_AllocFunc alloc, Lips_DeallocFunc dealloc, Stack* stack, uint32_t bytes)
{;
  if (stack->left + stack->size - stack->right + bytes > stack->size) {
    StackGrow(alloc, dealloc, stack);
  }
  stack->right -= bytes;
  return stack->data + stack->right;
}

void*
StackReleaseFromBack(Stack* stack, uint32_t bytes)
{
  assert(stack->right + bytes <= stack->size);
  void* ptr = stack->data + stack->right;
  stack->right += bytes;
  return ptr;
}

EvalState*
PushEvalState(Lips_Machine* interpreter)
{
  EvalState* newstate = StackRequireFromBack(interpreter->alloc, interpreter->dealloc,
                                             &interpreter->stack, sizeof(EvalState));
#ifndef NDEBUG
  memset(newstate, 0, sizeof(EvalState));
#endif
  newstate->parent = interpreter->evalpos;
  interpreter->evalpos = (uint8_t*)newstate - interpreter->stack.data;
  return newstate;
}

EvalState*
PopEvalState(Lips_Machine* interpreter)
{
  EvalState* child = CURRENT_EVAL_STATE(interpreter);
  // because of tail call optimization 1 state may have more than 1 environments
  for (uint32_t i = 0; i < ES_NUM_ENVS(child); i++) {
    PopEnv(interpreter);
  }
  interpreter->evalpos = child->parent;
  assert(StackReleaseFromBack(&interpreter->stack, sizeof(EvalState)) == child);
  if (interpreter->evalpos == STACK_INVALID_POS) {
    return NULL;
  }
  return CURRENT_EVAL_STATE(interpreter);
}

void
PushCatch(Lips_Machine* interpreter)
{
  EvalState* state = CURRENT_EVAL_STATE(interpreter);
  ES_CATCH_PARENT(state) = interpreter->catchpos;
  interpreter->catchpos = (uint8_t*)state - interpreter->stack.data;
}

void
PopCatch(Lips_Machine* interpreter)
{
  EvalState* catch = CURRENT_EVAL_STATE(interpreter);
  interpreter->catchpos = ES_CATCH_PARENT(catch);
}

EvalState*
UnwindStack(Lips_Machine* interpreter)
{
  EvalState* state = (EvalState*)(interpreter->stack.data + interpreter->evalpos);
  while (interpreter->catchpos != interpreter->evalpos) {
    state = PopEvalState(interpreter);
  }
  PopCatch(interpreter);
  return state;
}

HashTable*
InterpreterEnv(Lips_Machine* interpreter)
{
  HashTable* env = (HashTable*)(interpreter->stack.data + interpreter->envpos);
  return env;
}

HashTable*
PushEnv(Lips_Machine* interpreter)
{
  HashTable* env = HashTableCreate(interpreter->alloc, interpreter->dealloc,
                                   &interpreter->stack);
  env->parent = interpreter->envpos;
  interpreter->envpos = (uint8_t*)env - interpreter->stack.data;
  return env;
}

void
PopEnv(Lips_Machine* interpreter)
{
  HashTable* env = InterpreterEnv(interpreter);
  Iterator it;
  for (HashTableIterate(env, &it); !IteratorIsEmpty(&it); IteratorNext(&it)) {
    StringData* key;
    Lips_Cell cell;
    IteratorGet(&it, &key, &cell);
    StringDestroy(interpreter, key);
  }
  HashTableDestroy(&interpreter->stack, env);
  interpreter->envpos = env->parent;
}

HashTable*
EnvParent(Lips_Machine* interpreter, HashTable* env)
{
  if (env->parent == STACK_INVALID_POS) {
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

uint32_t
ComputeHashN(const char* string, uint32_t n)
{
  uint32_t p_pow = 1;
  uint32_t hash = 0;
  while (n--) {
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
  assert(StackRelease(stack, ht) == sizeof(HashTable) + ht->allocated * sizeof(Node));
}

void
HashTableReserve(Lips_Machine* interp, HashTable* ht, uint32_t capacity)
{
  assert(capacity > ht->allocated);
  if (HASH_TABLE_GET_SIZE(ht) == 0) {
    assert(StackRelease(&interp->stack, HASH_TABLE_DATA(ht)) == ht->allocated * sizeof(Node));
  }
  uint32_t preallocated = ht->allocated;
  ht->allocated = capacity;
  Node* nodes = StackRequire(interp->alloc, interp->dealloc, &interp->stack, capacity * sizeof(Node));
  nodes += capacity - preallocated;
  Node* data = HASH_TABLE_DATA(ht);
  memcpy(nodes, data, preallocated * sizeof(Node));
  for (uint32_t i = 0; i < capacity; i++) {
    data[i].value = NULL;
  }
  if (HASH_TABLE_GET_SIZE(ht) > 0) {
    uint32_t oldSize = HASH_TABLE_GET_SIZE(ht);
    HASH_TABLE_SET_SIZE(ht, 0);
    for (uint32_t i = 0; i < preallocated; i++) {
      if (NODE_VALID(nodes[i])) {
        HashTableInsert(interp, ht, nodes[i].key, nodes[i].value);
        if (HASH_TABLE_GET_SIZE(ht) == oldSize) break;
      }
    }
    assert(StackRelease(&interp->stack, data + capacity) == preallocated * sizeof(Node));
  }
}

Lips_Cell*
HashTableInsert(Lips_Machine* interp,
                HashTable* ht, StringData* key, Lips_Cell value) {
  assert(value && "Can not insert null");
  if (HASH_TABLE_GET_SIZE(ht) == ht->allocated) {
    uint32_t sz = (HASH_TABLE_GET_SIZE(ht) == 0) ? 1 : (HASH_TABLE_GET_SIZE(ht)<<1);
    HashTableReserve(interp, ht, sz);
  }
  Node* data = HASH_TABLE_DATA(ht);
  uint32_t id = key->hash % ht->allocated;
  while (NODE_VALID(data[id])) {
    // Hash table already has this element
    if (STR_DATA_LENGTH(key) == STR_DATA_LENGTH(data[id].key) &&
        key->hash == data[id].key->hash &&
        strcmp(STR_DATA_PTR2(interp, data[id].key), STR_DATA_PTR2(interp, key)) == 0) {
      return NULL;
    }
    id = (id+1) % ht->allocated;
  }
  data[id].key = key;
  data[id].value = value;
  // increment the size counter
  HASH_TABLE_SET_SIZE(ht, HASH_TABLE_GET_SIZE(ht)+1);
  return &data[id].value;
}

Lips_Cell*
HashTableSearch(Lips_Machine* interpreter, const HashTable* ht, const char* key)
{
  uint32_t hash = ComputeHash(key);
  return HashTableSearchWithHash(interpreter, ht, hash, key);
}

Lips_Cell*
HashTableSearchWithHash(Lips_Machine* interpreter, const HashTable* ht, uint32_t hash, const char* key)
{
  Node* data = HASH_TABLE_DATA(ht);
  uint32_t i = 0;
  uint32_t id = hash;
  while (i < HASH_TABLE_GET_SIZE(ht)) {
    id = id % ht->allocated;
    if (NODE_VALID(data[id])) {
      if (strcmp(STR_DATA_PTR2(interpreter, data[id].key), key) == 0)
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

StringData*
HashTableSearchKey(Lips_Machine* interp, const HashTable* ht, uint32_t hash, const char* key, uint32_t n)
{
  Node* data = HASH_TABLE_DATA(ht);
  uint32_t i = 0;
  uint32_t id = hash;
  while (i < HASH_TABLE_GET_SIZE(ht)) {
    id = id % ht->allocated;
    if (NODE_VALID(data[id])) {
      StringData* val = data[id].key;
      if (hash == val->hash &&
          strncmp(STR_DATA_PTR2(interp, val), key, n) == 0)
        return data[id].key;
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
  it->node = HASH_TABLE_DATA(ht);
  it->size = HASH_TABLE_GET_SIZE(ht);
  if (it->size > 0) {
    while (!NODE_VALID(*it->node)) {
      it->node++;
    }
  }
}

int
IteratorIsEmpty(const Iterator* it)
{
  return it->size == 0;
}

void
IteratorGet(const Iterator* it, StringData** key, Lips_Cell* value)
{
  *key = it->node->key;
  *value  = it->node->value;
}

void
IteratorNext(Iterator* it)
{
  assert(it->size > 0);
  it->node++;
  if (it->size > 1) {
    while (!NODE_VALID(*it->node)) {
      it->node++;
    }
  }
  it->size--;
}

uint32_t GetRealNumargs(Lips_Machine* interpreter, Lips_Cell callable)
{
  (void)interpreter;
  TYPE_CHECK_FORCED(LIPS_TYPE_FUNCTION|LIPS_TYPE_MACRO, callable);
  return (GET_NUMARGS(callable) & 127) + (GET_NUMARGS(callable) >> 7);
}

MemChunk*
AddMemChunk(Lips_Machine* interpreter, uint32_t size)
{
  assert(interpreter->numchunks < MAX_CHUNKS);
  MemChunk* chunk = &interpreter->chunks[interpreter->numchunks];
  chunk->data = interpreter->alloc(size);
  chunk->numbytes = size;
  chunk->used = 0;
  chunk->available = chunk->numbytes;
  chunk->numregions = 1;
  chunk->first = (FreeRegion*)chunk->data;
  chunk->first->offset = 0;
  chunk->first->size = chunk->numbytes;
  chunk->first->lhs = (uint32_t)-1;
  chunk->first->rhs = (uint32_t)-1;
  chunk->last = chunk->first;
  chunk->deletions = 0;
  interpreter->numchunks++;
  return chunk;
}

void
RemoveMemChunk(Lips_Machine* interpreter, uint32_t i)
{
  MemChunk* chunk = &interpreter->chunks[i];
  assert(chunk->used == 0 && chunk->available == chunk->numbytes);
  interpreter->dealloc(chunk->data, chunk->numbytes);
  if (i != interpreter->numchunks-1) {
    // swap with last place
    memcpy(&interpreter->chunks[i], &interpreter->chunks[interpreter->numchunks-1], sizeof(MemChunk));
  }
  interpreter->numchunks--;
}

char*
ChunkFindSpace(MemChunk* chunk, uint32_t* bytes)
{
  FreeRegion* region = NULL;
  uint32_t diff;
  // find a region which size differs from 'bytes' the least
  for (FreeRegion* it = chunk->last; it; it = REGION_LEFT(chunk, it)) {
    diff = it->size - *bytes;
    if (region == NULL ||
        diff < (region->size - *bytes)) {
      region = it;
      if (diff == 0)
        break;
    }
  }
  if (region == NULL) {
    // move all strings to left
    ChunkSortRegions(chunk);
    FreeRegion* region = chunk->first;
    uint32_t counter = region->offset;
    while (region) {
      uint32_t offset = region->offset;
      uint32_t size = region->size;
      region = REGION_RIGHT(chunk, region);
      StringData* str = (StringData*)(chunk->data + offset + size);
      if ((char*)str < chunk->data + chunk->numbytes) {
        memmove(chunk->data + counter, str, STR_DATA_ALLOCATED(str));
        counter += STR_DATA_ALLOCATED(str);
      }
    }
    // after we moved strings we have 1 big region at right side of chunk's data
    assert(counter + sizeof(FreeRegion*) <= chunk->numbytes);
    region = (FreeRegion*)&chunk->data[counter];
    region->offset = counter;
    region->size = chunk->numbytes - counter;
    region->lhs = (uint32_t)-1;
    region->rhs = (uint32_t)-1;
    chunk->first = region;
    chunk->last = region;
    chunk->numregions = 1;
  }
  FreeRegion* left = REGION_LEFT(chunk, region);
  FreeRegion* right = REGION_RIGHT(chunk, region);
  char* ret = chunk->data + region->offset;
  if (diff < sizeof(FreeRegion)) {
    *bytes = region->size;
    // remove 'region' from linked list
    if (left) {
      left->rhs = region->rhs;
    } else {
      chunk->first = right;
    }
    if (right) {
      right->lhs = region->lhs;
    } else {
      chunk->last = left;
    }
    chunk->numregions--;
  } else {
    // move region to right
    region->offset += *bytes;
    region->size -= *bytes;
    FreeRegion* region_off = (FreeRegion*)((char*)region + *bytes);
    memmove(region_off, region, sizeof(FreeRegion));
    // follow invariants
    if (left) {
      left->rhs += *bytes;
    } else {
      chunk->first = region_off;
    }
    if (right) {
      right->lhs -= *bytes;
    } else {
      chunk->last = region_off;
    }
  }
  chunk->available -= *bytes;
  return ret;
}

void
ChunkShrink(MemChunk* chunk)
{
  ChunkSortRegions(chunk);
  FreeRegion* region = NULL;
  // merge regions
  for (region = chunk->last; region; region = REGION_LEFT(chunk, region)) {
    FreeRegion* left = REGION_LEFT(chunk, region);
    while (left && (region->offset - region->size == left->offset)) {
      left->rhs = region->rhs;
      FreeRegion* right = REGION_RIGHT(chunk, region);
      if (right) {
        right->lhs = region->lhs;
      }
      left->size += region->size;
      if (region == chunk->last) {
        region = left;
      }
      region = left;
      chunk->numregions--;
      left = REGION_LEFT(chunk, region);
    }
  }
}

void
ChunkSortRegions(MemChunk* chunk)
{
  // sort regions using insertion sort(works very well on almost sorted arrays)
  FreeRegion* region = NULL;
  // sort regions using insertion sort(works very well on almost sorted arrays)
  for (region = chunk->first; region; region = REGION_RIGHT(chunk, region)) {
    FreeRegion* left = REGION_LEFT(chunk, region);
    while (left && region->offset <= left->offset) {
      memcpy(chunk->data + left->rhs, left, sizeof(FreeRegion));
      left =  REGION_LEFT(chunk, left);
    }
    if (REGION_RIGHT(chunk, left) != region) {
      memcpy(chunk->data + left->rhs, region, sizeof(FreeRegion));
    }
  }
  while (REGION_LEFT(chunk, chunk->first)) {
    chunk->first = REGION_LEFT(chunk, chunk->first);
  }
  while (REGION_RIGHT(chunk, chunk->last)) {
    chunk->last = REGION_RIGHT(chunk, chunk->last);
  }
}

FreeRegion*
PushRegion(MemChunk* chunk, uint32_t offset, uint32_t size)
{
  FreeRegion* ret = (FreeRegion*)&chunk->data[offset];
  if (LIPS_LIKELY(chunk->last != NULL)) {
    chunk->last->rhs = offset;
    ret->lhs = (char*)chunk->last - chunk->data;
  } else {
    ret->lhs = (uint32_t)-1;
    chunk->first = ret;
  }
  ret->rhs = (uint32_t)-1;
  ret->offset = offset;
  ret->size = size;
  chunk->last = ret;
  chunk->numregions++;
  chunk->available += size;
  return ret;
}

Bucket*
FindBucketForString(Lips_Machine* interp)
{
  for (uint32_t i = interp->numstr_buckets; i > 0; i--)
    if (interp->str_buckets[i-1].size < BUCKET_SIZE) {
      return &interp->str_buckets[i-1];
    }
  if (interp->numstr_buckets == interp->allocstr_buckets) {
    // we're out of storage for buckets, allocate more
    Bucket* new_buckets = interp->alloc(interp->allocstr_buckets * 2);
    assert(new_buckets);
    memcpy(new_buckets, interp->str_buckets, interp->numstr_buckets * sizeof(Bucket));
    interp->str_buckets = new_buckets;
    interp->dealloc(new_buckets, sizeof(Bucket) * interp->allocstr_buckets);
    interp->allocstr_buckets = interp->allocstr_buckets * 2;
  }
  Bucket* new_bucket = &interp->str_buckets[interp->numstr_buckets];
  CreateBucket(interp->alloc, new_bucket, sizeof(StringData));
  interp->numstr_buckets++;
  return new_bucket;
}

MemChunk*
FindChunkForString(Lips_Machine* interp, uint32_t n)
{
  // try to find a chunk with enough space
  for (uint32_t i = 0; i < interp->numchunks; i++) {
    if (interp->chunks[i].available >= n) {
      return &interp->chunks[i];
    }
  }
  // try to add a new chunk
  uint32_t sz = (n < 1024) ? 1024 : n;
  if (interp->numchunks < MAX_CHUNKS) {
    return AddMemChunk(interp, sz);
  }
  // find smallest chunk and reallocate it
  MemChunk* chunk = &interp->chunks[0];
  for (uint32_t i = 1; i < interp->numchunks; i++) {
    if (interp->chunks[i].numbytes < chunk->numbytes) {
      chunk = &interp->chunks[i];
    }
  }
  // reallocate chunk
  char* new_data = interp->alloc(chunk->numbytes + sz);
  assert(new_data);
  memcpy(new_data, chunk->data, chunk->numbytes);
  interp->dealloc(chunk->data, chunk->numbytes);
  chunk->data = new_data;
  // push a region to the end
  PushRegion(chunk, chunk->numbytes, sz);
  chunk->numbytes += sz;
  return chunk;
}

void
DefineWithCurrent(Lips_Machine* interpreter, Lips_Cell name, Lips_Cell value)
{
  TYPE_CHECK_FORCED(LIPS_TYPE_SYMBOL|LIPS_TYPE_STRING, name);
  assert(value);
  HashTable* env = InterpreterEnv(interpreter);
  Lips_Cell* ptr = HashTableInsert(interpreter, env,
                                   GET_STR(name), value);
  STR_DATA_REF_INC(GET_STR(name));
  assert(ptr && "Internal error(value is already defined)");
}

uint32_t
CheckArgumentCount(Lips_Machine* interpreter, Lips_Cell callable, Lips_Cell args)
{
  uint32_t numargs = GET_NUMARGS(callable) & (LIPS_NUM_ARGS_VAR-1);
  uint32_t variadic = GET_NUMARGS(callable) & LIPS_NUM_ARGS_VAR;
  uint32_t listlen = (args) ? Lips_ListLength(interpreter, args) : 0;
  if ((numargs > listlen) ||
      (numargs < listlen && variadic == 0)) {
    Lips_SetError(interpreter,
                  "Invalid number of arguments, passed %u arguments, but callable accepts %u",
                  listlen, numargs);
    return (uint32_t)-1;
  }
  return listlen;
}

void
DefineArgumentList(Lips_Machine* interpreter, Lips_Cell callable, Lips_Cell argvalues)
{
  TYPE_CHECK_FORCED(LIPS_TYPE_FUNCTION|LIPS_TYPE_MACRO, callable);
  TYPE_CHECK_FORCED(LIPS_TYPE_PAIR, argvalues);
  // reserve space for hash table
  HashTableReserve(interpreter, InterpreterEnv(interpreter),
                   GetRealNumargs(interpreter, callable));
  // define variables in a new environment
  Lips_Cell argnames = GET_LFUNC(callable).args;
  uint32_t count = (GET_NUMARGS(callable) & (LIPS_NUM_ARGS_VAR-1));
  for (uint32_t i = 0; i < count; i++) {
    DefineWithCurrent(interpreter, GET_HEAD(argnames), GET_HEAD(argvalues));
    argnames = GET_TAIL(argnames);
    argvalues = GET_TAIL(argvalues);
  }
  if (GET_NUMARGS(callable) & LIPS_NUM_ARGS_VAR) {
    DefineWithCurrent(interpreter, GET_HEAD(argnames), argvalues);
  }
}

void
DefineArgumentArray(Lips_Machine* interpreter, Lips_Cell callable,
                    uint32_t numargs, Lips_Cell* argvalues)
{
  TYPE_CHECK_FORCED(LIPS_TYPE_FUNCTION, callable);
  // reserve space for hash table
  HashTableReserve(interpreter, InterpreterEnv(interpreter),
                   GetRealNumargs(interpreter, callable));
  // define variables in a new environment
  Lips_Cell argnames = GET_LFUNC(callable).args;
  uint32_t count = (GET_NUMARGS(callable) & (LIPS_NUM_ARGS_VAR-1));
  for (uint32_t i = 0; i < count; i++) {
    if (GET_HEAD(argnames)) {
      Lips_DefineCell(interpreter, GET_HEAD(argnames), *argvalues);
    }
    argvalues++;
    argnames = GET_TAIL(argnames);
  }
  if (GET_NUMARGS(callable) & LIPS_NUM_ARGS_VAR) {
    Lips_Cell list = Lips_NewList(interpreter, numargs - count, argvalues+count);
    Lips_DefineCell(interpreter, GET_HEAD(argnames), list);
  }
}

void
FreeArgumentArray(Lips_Machine* interpreter, uint32_t numargs, Lips_Cell* args)
{
  assert(StackReleaseFromBack(&interpreter->stack, numargs * sizeof(Lips_Cell)) == args);
}

Lips_Cell
EvalNonPair(Lips_Machine* interpreter, Lips_Cell cell)
{
  assert(!Lips_IsList(cell));
  switch (GET_TYPE(cell)) {
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

void
Mark(Lips_Machine* interpreter)
{
  /*
    Sample implementation with recursion(simplified):
  */
#if 0
  void Mark(Cell cell) {
    switch (cell->type) {
    case int:
    case float:
    case string:
      /* types that do not contain references to other cells */
      cell->marked = true;
      return;
    case function:
      Mark(cell->func_args);
      Mark(cell->func_body);
      return;
    case list:
      Mark(car(cell));
      Mark(cdr(cell));
      return;
    }
  }
#endif
  HashTable* env = InterpreterEnv(interpreter);
  Iterator it;
  StringData* key;
  Lips_Cell value;
  uint32_t depth;
  Lips_Cell* prev;
  do {
    for (HashTableIterate(env, &it); !IteratorIsEmpty(&it); IteratorNext(&it)) {
      IteratorGet(&it, &key, &value);
      depth = 1;
    cycle:
      switch (GET_TYPE(value)) {
      default: assert(0 && "internal error");
      case LIPS_TYPE_INTEGER:
      case LIPS_TYPE_REAL:
      case LIPS_TYPE_STRING:
      case LIPS_TYPE_SYMBOL:
      case LIPS_TYPE_C_FUNCTION:
      case LIPS_TYPE_C_MACRO:
      case LIPS_TYPE_USER:
        value->type |= MARK_MASK;
        depth--;
        break;
      case LIPS_TYPE_MACRO:
      case LIPS_TYPE_FUNCTION:
        if (value->type & MARK_MASK) {
          depth--;
        } else {
          prev = StackRequire(interpreter->alloc, interpreter->dealloc,
                              &interpreter->stack, sizeof(Lips_Cell));
          *prev = GET_LFUNC(value).args;
          value = GET_LFUNC(value).body;
          depth++;
          goto cycle;
        }
        break;
      case LIPS_TYPE_PAIR:
        if (value->type & MARK_MASK) {
          depth--;
        } else {
          value->type |= MARK_MASK;
          if (GET_HEAD(value)) {
            prev = StackRequire(interpreter->alloc, interpreter->dealloc,
                                &interpreter->stack, sizeof(Lips_Cell));
            *prev = GET_TAIL(value);
            value = GET_HEAD(value);
            depth++;
          }
          goto cycle;
        }
        break;
      }
      if (depth > 0) {
        value = *prev;
        assert(StackRelease(&interpreter->stack, prev) == sizeof(Lips_Cell));
        while (!value && depth > 1) {
          prev--;
          depth--;
          value = *prev;
          assert(StackRelease(&interpreter->stack, prev) == sizeof(Lips_Cell));
        }
        if (value) {
          goto cycle;
        }
      }
    }
    env = EnvParent(interpreter, env);
  } while (env);
}

void
Sweep(Lips_Machine* interpreter)
{
  // delete all unmarked cells
  for (uint32_t i = 0; i < interpreter->numbuckets; i++) {
    Bucket* bucket = &interpreter->buckets[i];
    uint32_t count = bucket->size;
    for (uint32_t i = 0; count > 0; i++) {
      Lips_Cell cell = (Lips_Value*)bucket->data + i;
      if ((cell->type & DEAD_MASK) == 0) {
        if ((cell->type & MARK_MASK) == 0) {
          // destroy cell
          DestroyCell(interpreter, cell);
          BucketDeleteCell(bucket, cell);
        } else {
          // unmark cell
          cell->type &= ~MARK_MASK;
        }
        count--;
      }
    }
  }
}


/// Lisp functions

LIPS_DECLARE_FUNCTION(list)
{
  (void)udata;
  return Lips_NewList(interpreter, numargs, args);
}

LIPS_DECLARE_FUNCTION(car)
{
  (void)udata;
  assert(numargs == 1);
  Lips_Cell list = args[0];
  return Lips_CAR(interpreter, list);
}

LIPS_DECLARE_FUNCTION(cdr)
{
  (void)udata;
  assert(numargs == 1);
  Lips_Cell list = args[0];
  return Lips_CDR(interpreter, list);
}

LIPS_DECLARE_FUNCTION(equal)
{
  (void)udata;
  Lips_Cell lhs = args[0];
  Lips_Cell rhs;
  for (uint32_t i = 1; i < numargs; i++) {
    rhs = args[i];
    if (lhs == rhs)
      continue;
    // TODO: compare ints and floats
    if (GET_TYPE(lhs) == GET_TYPE(rhs)) {
      if (GET_TYPE(lhs) == LIPS_TYPE_INTEGER) {
        if (lhs->data.integer == rhs->data.integer)
          continue;
      } else if (GET_TYPE(lhs) == LIPS_TYPE_REAL) {
        if (lhs->data.real == rhs->data.real)
          continue;
      } else if (GET_TYPE(lhs) == LIPS_TYPE_STRING ||
                 GET_TYPE(lhs) == LIPS_TYPE_SYMBOL) {
        if (StringEqual(interpreter, GET_STR(lhs), GET_STR(rhs)))
          continue;
      } else if (GET_TYPE(lhs) == LIPS_TYPE_PAIR) {
        // FIXME: this should definitely look nicer but for now it works
        // or does it?
        if ((GET_HEAD(lhs) == NULL && GET_HEAD(rhs)) ||
            (GET_HEAD(lhs) && GET_HEAD(rhs) == NULL)) {
          return interpreter->S_nil;
        }
        int equals = 0;
        if (GET_HEAD(lhs) && GET_HEAD(rhs)) {
          Lips_Cell cells[2] = { GET_HEAD(lhs), GET_HEAD(rhs) };
          equals = F_equal(interpreter, 2, cells, NULL) != interpreter->S_nil;
        } else {
          equals = 1;
        }
        if (equals) {
          if ((GET_TAIL(lhs) == NULL && GET_TAIL(rhs)) ||
              (GET_TAIL(lhs) && GET_TAIL(rhs) == NULL)) {
            return interpreter->S_nil;
          }
          if (GET_TAIL(lhs) == NULL && GET_TAIL(rhs) == NULL)
            continue;
          Lips_Cell cells[2] = { GET_TAIL(lhs), GET_TAIL(rhs) };
          if (F_equal(interpreter, 2, cells, NULL) != interpreter->S_nil)
            continue;
        }
      } else if (GET_TYPE(lhs) == LIPS_TYPE_FUNCTION ||
                 GET_TYPE(rhs) == LIPS_TYPE_MACRO) {
        Lips_Cell cells1[2] = { GET_LFUNC(lhs).args, GET_LFUNC(rhs).args };
        Lips_Cell cells2[2] = { GET_LFUNC(lhs).body, GET_LFUNC(rhs).body };
        if (F_equal(interpreter, 2, cells1, NULL) != interpreter->S_nil &&
            F_equal(interpreter, 2, cells2, NULL) != interpreter->S_nil)
          continue;
      } else if (GET_TYPE(lhs) == LIPS_TYPE_C_FUNCTION) {
        if (GET_CFUNC(lhs).ptr == GET_CFUNC(rhs).ptr &&
            GET_CFUNC(lhs).udata == GET_CFUNC(rhs).udata)
          continue;
      } else if (GET_TYPE(lhs) == LIPS_TYPE_C_MACRO) {
        if (GET_CMACRO(lhs).ptr == GET_CMACRO(rhs).ptr &&
            GET_CMACRO(lhs).udata == GET_CMACRO(rhs).udata)
          continue;
      } else {
        assert(0);
      }
    }
    return interpreter->S_nil;
  }
  return interpreter->S_t;
}

LIPS_DECLARE_FUNCTION(nilp)
{
  (void)udata;
  assert(numargs == 1);
  Lips_Cell cells[2] = { args[0], interpreter->S_nil };
  return F_equal(interpreter, 2, cells, NULL);
}

LIPS_DECLARE_FUNCTION(typeof)
{
  (void)udata;
  assert(numargs == 1);
  Lips_Cell ret = NULL;
  switch (GET_TYPE(args[0])) {
  default: assert(0 && "faced undefined type");
  case LIPS_TYPE_INTEGER:
    ret = interpreter->T_integer;
    break;
  case LIPS_TYPE_REAL:
    ret = interpreter->T_real;
    break;
  case LIPS_TYPE_STRING:
    ret = interpreter->T_string;
    break;
  case LIPS_TYPE_SYMBOL:
    ret = interpreter->T_symbol;
    break;
  case LIPS_TYPE_PAIR:
    ret = interpreter->T_pair;
    break;
  case LIPS_TYPE_FUNCTION:
  case LIPS_TYPE_C_FUNCTION:
    ret = interpreter->T_function;
    break;
  case LIPS_TYPE_MACRO:
  case LIPS_TYPE_C_MACRO:
    ret = interpreter->T_macro;
    break;
  case LIPS_TYPE_USER:
    // TODO: user types
    LIPS_THROW_ERROR(interpreter, "User types are not supported currently");
    break;
  }
  return ret;
}

LIPS_DECLARE_FUNCTION(throw)
{
  (void)udata;
  assert(numargs == 1);
  interpreter->throwvalue = args[0];
  Lips_PrintCell(interpreter, interpreter->throwvalue, interpreter->errbuff, sizeof(interpreter->errbuff));
  return NULL;
}

LIPS_DECLARE_FUNCTION(call)
{
  (void)udata;
  assert(numargs == 2);
  TYPE_CHECK(interpreter, LIPS_TYPE_SYMBOL, args[0]);
  TYPE_CHECK(interpreter, LIPS_TYPE_PAIR, args[1]);
  return Lips_Invoke(interpreter, args[0], args[1]);
}

LIPS_DECLARE_MACRO(lambda)
{
  TYPE_CHECK(interpreter, LIPS_TYPE_PAIR, GET_HEAD(args));
  (void)udata;
  uint32_t len;
  Lips_Cell last = Lips_ListLastElement(interpreter, GET_HEAD(args), &len);
  if (len > 127) {
    LIPS_THROW_ERROR(interpreter,
                     "Too many arguments(%u), in Lips language callables have up to 127 named arguments", len);
  }
  if (last && Lips_IsSymbol(last) && strcmp(GET_STR_PTR(interpreter, last), "...") == 0) {
    len--;
    len |= LIPS_NUM_ARGS_VAR;
  }
  Lips_Cell lambda = Lips_NewFunction(interpreter, GET_HEAD(args), GET_TAIL(args), len);
  return lambda;
}

LIPS_DECLARE_MACRO(macro)
{
  TYPE_CHECK(interpreter, LIPS_TYPE_PAIR, GET_HEAD(args));
  (void)udata;
  uint32_t len;
  Lips_Cell last = Lips_ListLastElement(interpreter, GET_HEAD(args), &len);
  if (len > 127) {
    LIPS_THROW_ERROR(interpreter,
                    "Too many arguments(%u), in Lips language callables have up to 127 named arguments", len);
  }
  if (last && Lips_IsSymbol(last) && strcmp(GET_STR_PTR(interpreter, last), "...") == 0) {
    len--;
    len |= LIPS_NUM_ARGS_VAR;
  }
  Lips_Cell macro = Lips_NewMacro(interpreter, GET_HEAD(args), GET_TAIL(args), len);
  return macro;
}

LIPS_DECLARE_MACRO(define)
{
  (void)udata;
  Lips_Cell value = Lips_Eval(interpreter, GET_HEAD(GET_TAIL(args)));
  // TODO: handle error
  return Lips_DefineCell(interpreter, GET_HEAD(args), value);
}

LIPS_DECLARE_MACRO(quote)
{
  (void)udata;
  (void)interpreter;
  return GET_HEAD(args);
}

LIPS_DECLARE_MACRO(progn)
{
  (void)udata;
  EvalState* state = CURRENT_EVAL_STATE(interpreter);
  ES_SET_STAGE(state, ES_STAGE_EXECUTING_CODE);
  ES_CODE(state) = args;
  ES_CATCH_PARENT(state) = STACK_INVALID_POS;
  return NULL;
}

LIPS_DECLARE_MACRO(if)
{
  (void)udata;
  Lips_Cell condition = Lips_Eval(interpreter, GET_HEAD(args));
  if (F_nilp(interpreter, 1, &condition, NULL) == interpreter->S_nil) {
    return Lips_Eval(interpreter, GET_HEAD(GET_TAIL(args)));
  }
  return M_progn(interpreter, GET_TAIL(GET_TAIL(args)), NULL);
}

LIPS_DECLARE_MACRO(when)
{
  (void)udata;
  Lips_Cell condition = Lips_Eval(interpreter, GET_HEAD(args));
  if (F_nilp(interpreter, 1, &condition, NULL) == interpreter->S_nil) {
    return M_progn(interpreter, GET_TAIL(args), NULL);
  }
  return interpreter->S_nil;
}

LIPS_DECLARE_MACRO(catch)
{
  (void)udata;
  Lips_Cell ret = M_progn(interpreter, args, udata);
  PushCatch(interpreter);
  return ret;
}

LIPS_DECLARE_MACRO(intern)
{
  (void)udata;
  TYPE_CHECK(interpreter, LIPS_TYPE_STRING|LIPS_TYPE_SYMBOL, GET_HEAD(args));
  Lips_Cell cell = Lips_InternCell(interpreter, GET_HEAD(args));
  return cell;
}
