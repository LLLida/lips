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
typedef struct StringPool StringPool;


/// MACROS

#define MAX_CHUNKS 16
#define LIPS_EOF (-1)
#define STACK_INVALID_POS ((uint32_t)-1)
#define DEAD_MASK (1u<<31)
#define MARK_MASK (1<<30)
#define IS_DEAD(cell) ((cell).type & DEAD_MASK)
#define IS_WHITESPACE(c) ((c) == ' ' || (c) == '\n' || (c) == '\t' || (c) == '\r')
#define IS_SPECIAL_CHAR(c) ((c) == '(' || (c) == ')' || (c) == '\'' || (c) == '`')
#define IS_DIGIT(c) ((c) >= '0' && (c) <= '9')
#define LOG_ERROR(machine, ...) snprintf(machine->errbuff, sizeof(machine->errbuff), __VA_ARGS__)
#define TYPE_CHECK(machine, type, cell) if (!(GET_TYPE(cell) & (type))) LIPS_THROW_ERROR(machine, "Typecheck failed (%d & %d) at line %d", GET_TYPE(cell), type, __LINE__);
#define TYPE_CHECK_FORCED(type, cell) do {                              \
    assert((GET_TYPE(cell) & (type)) && "Typecheck failed"); \
  } while (0)
#define GET_STR(cell) ((cell)->data.str)
#define GET_STR_PTR(cell) GET_STR(cell)->data


/// LIST OF FUNCTIONS

static void* DefaultAlloc(size_t bytes);
static void DefaultDealloc(void* ptr, size_t bytes);
static void* DefaultOpenFile(const char* file, size_t* datasize);
static void DefaultReadFile(void* filehandle, char* buffer, size_t bytes);
static void DefaultCloseFile(void* filehandle);

static Lips_Cell NewCell(Lips_Machine* interp) LIPS_HOT_FUNCTION;
static void DestroyCell(Lips_Machine* machine, Lips_Cell cell);

static StringData* StringCreateEmpty(Lips_Machine* machine, uint32_t n);
static StringData* StringCreate(Lips_Machine* machine, const char* str, uint32_t n);
static StringData* StringCreateWithHash(Lips_Machine* machine, const char* str, uint32_t n, uint32_t hash);
static void StringDestroy(Lips_Machine* machine, StringData* str);
static int StringEqual(const StringData* lhs, const StringData* rhs);

const char* GDB_lips_to_c_string(Lips_Machine* machine, Lips_Cell cell);

static void ParserInit(Parser* parser, const char* str, uint32_t len);
static int ParserNextToken(Parser* parser);
static int Lips_IsTokenNumber(const Token* token);
static Lips_Cell ParseNumber(Lips_Machine* machine, const Token* token);
static Lips_Cell GenerateAST(Lips_Machine* machine, Parser* parser);

static void CreateBucket(Lips_AllocFunc alloc, Bucket* bucket, uint32_t num_elmenents, uint32_t elem_size);
static void DestroyBucket(Lips_Machine* machine, Bucket* bucket, uint32_t elem_size);
static void* BucketNew(Bucket* bucket, uint32_t elem_size);
static void BucketDelete(Bucket* bucket, void* data, uint32_t elem_size);

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

static EvalState* PushEvalState(Lips_Machine* machine);
static EvalState* PopEvalState(Lips_Machine* machine);

static void PushCatch(Lips_Machine* machine);
static void PopCatch(Lips_Machine* machine);
static EvalState* UnwindStack(Lips_Machine* machine);

static HashTable* MachineEnv(Lips_Machine* machine);
static HashTable* PushEnv(Lips_Machine* machine);
static void PopEnv(Lips_Machine* machine);
static HashTable* EnvParent(Lips_Machine* machine, HashTable* env);
// compute hash of null terminated string
static uint32_t ComputeHash(const char* string) LIPS_PURE_FUNCTION;
// compute hash of sized string(no null terminator at end)
static uint32_t ComputeHashN(const char* string, uint32_t n) LIPS_PURE_FUNCTION;
static HashTable* HashTableCreate(Lips_AllocFunc alloc, Lips_DeallocFunc dealloc,
                                  Stack* stack);
static void HashTableDestroy(Stack* stack, HashTable* ht);
static void HashTableReserve(Lips_Machine* machine, HashTable* ht, uint32_t capacity);
static Lips_Cell* HashTableInsert(Lips_Machine* machine,
                                  HashTable* ht, StringData* key, Lips_Cell value);
static Lips_Cell* HashTableSearch(const HashTable* ht, const char* key);
static Lips_Cell* HashTableSearchWithHash(const HashTable* ht, uint32_t hash, const char* key);
static StringData* HashTableSearchKey(const HashTable* ht, uint32_t hash, const char* key, uint32_t n);
static void HashTableIterate(HashTable* ht, Iterator* it);
static int IteratorIsEmpty(const Iterator* it);
static void IteratorGet(const Iterator* it, StringData** key, Lips_Cell* value);
static void IteratorNext(Iterator* it);

static Bucket* FindBucketForString(Lips_Machine* machine);

static StringPool* GetStringPool(Lips_Machine* machine, uint32_t str_size_with_0);
static char* FindSpaceForString(StringPool* pool, Lips_AllocFunc alloc);
static void RemoveString(StringPool* pool, char* str);

static void DefineWithCurrent(Lips_Machine* machine, Lips_Cell name, Lips_Cell value);
static uint32_t CheckArgumentCount(Lips_Machine* machine, Lips_Cell callable, Lips_Cell args);
static void DefineArgumentList(Lips_Machine* machine, Lips_Cell callable, Lips_Cell argvalues);
static void DefineArgumentArray(Lips_Machine* machine, Lips_Cell callable, uint32_t numargs, Lips_Cell* argvalues);
static void FreeArgumentArray(Lips_Machine* machine, uint32_t numargs, Lips_Cell* args);
static Lips_Cell EvalNonPair(Lips_Machine* machine, Lips_Cell cell);

static void Mark(Lips_Machine* machine);
static void Sweep(Lips_Machine* machine);

static LIPS_DECLARE_FUNCTION(list);
static LIPS_DECLARE_FUNCTION(car);
static LIPS_DECLARE_FUNCTION(cdr);
static LIPS_DECLARE_FUNCTION(equal);
static LIPS_DECLARE_FUNCTION(nilp);
static LIPS_DECLARE_FUNCTION(typeof);
static LIPS_DECLARE_FUNCTION(throw);
static LIPS_DECLARE_FUNCTION(call);
static LIPS_DECLARE_FUNCTION(format);
static LIPS_DECLARE_FUNCTION(concat);
static LIPS_DECLARE_FUNCTION(slurp);
static LIPS_DECLARE_FUNCTION(load);

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
  char* data;
  // Do we really need to use bitfields? I think they slow memory access, not sure
  uint32_t length : 22;
  uint32_t bucket_index : 10;
  uint32_t refs;
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
#define HASH_TABLE_CONSTANT_FLAG (1u<<31)
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
  uint32_t num_elements;
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
#define ES_PASSED_ARGS(es) (es)->passed_args
#define ES_ARG_COUNT(es) (es)->data.args.count
#define ES_LAST_ARG(es) (es)->data.args.last
#define ES_CODE(es) (es)->data.exec.code
#define ES_CATCH_PARENT(es) (es)->data.exec.parent

#define NUM_STRING_POOL_SIZES 5
static const uint32_t string_pool_sizes[NUM_STRING_POOL_SIZES] = { 4, 8, 16, 32, 64 };

struct StringPool {
#define STRING_POOL_NUM_CHUNKS 10
  // i-th chunk has 4^i * chunk[0] bytes allocated
  Bucket chunks[STRING_POOL_NUM_CHUNKS];
  uint32_t str_max_size;
  uint32_t initial_chunk_size;
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

  // callbacks
  Lips_AllocFunc alloc;
  Lips_DeallocFunc dealloc;
  Lips_OpenFileFunc open_file;
  Lips_ReadFileFunc read_file;
  Lips_CloseFileFunc close_file;
  // pools for cells
  Bucket* buckets;
  uint32_t numbuckets;
  uint32_t allocbuckets;
  // pools for string objects
  Bucket* str_buckets;
  uint32_t numstr_buckets;
  uint32_t allocstr_buckets;
  // pools for string data
  // 4, 8, 16, 32, 64
  StringPool string_pools[NUM_STRING_POOL_SIZES];
  // our beloved stack
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
Lips_CreateMachine(const Lips_MachineCreateInfo* info)
{
  Lips_Machine* machine;
  machine = (Lips_Machine*)info->alloc(sizeof(Lips_Machine));
  if (machine == NULL) return NULL;
  machine->alloc = info->alloc;
  machine->dealloc = info->dealloc;
  machine->open_file = info->open_file;
  machine->read_file = info->read_file;
  machine->close_file = info->close_file;

  machine->numbuckets = 0;
  machine->buckets = (Bucket*)machine->alloc(sizeof(Bucket));
  machine->allocbuckets = 1;

  for (int i = 0; i < NUM_STRING_POOL_SIZES; i++) {
    machine->string_pools[i].str_max_size = string_pool_sizes[i];
    machine->string_pools[i].initial_chunk_size = 16 * 1024;
    memset(machine->string_pools[i].chunks, 0, sizeof(Bucket) * STRING_POOL_NUM_CHUNKS);
  }

  machine->str_buckets = (Bucket*)machine->alloc(sizeof(Bucket));
  machine->allocstr_buckets = 1;
  machine->numstr_buckets = 0;
  CreateStack(machine->alloc, &machine->stack, info->initial_stack_size);
  HashTable* env = HashTableCreate(machine->alloc, machine->dealloc, &machine->stack);
  env->parent = STACK_INVALID_POS;
  machine->envpos = ((uint8_t*)env - machine->stack.data);
  machine->evalpos = STACK_INVALID_POS;
  machine->catchpos = STACK_INVALID_POS;
  // define builtins
  machine->default_file = Lips_NewString(machine, "<eval>");
  machine->S_nil = Lips_Define(machine, "nil", Lips_NewPair(machine, NULL, NULL));
  // important optimization: this allows to reuse string "...",
  // so anytime we encounter "..." in code we don't allocate a new string
  Lips_Define(machine, "...", machine->S_nil);
  // t is just an integer
  // FIXME: do I need to add a new type for "t"?
  machine->S_t = Lips_Define(machine, "t", Lips_NewInteger(machine, 1));
  machine->S_filename = Lips_Define(machine, "<filename>", Lips_NewPair(machine, NULL, NULL));

  LIPS_DEFINE_FUNCTION(machine, list, LIPS_NUM_ARGS_VAR, NULL);
  LIPS_DEFINE_FUNCTION(machine, car, LIPS_NUM_ARGS_1, NULL);
  LIPS_DEFINE_FUNCTION(machine, cdr, LIPS_NUM_ARGS_1, NULL);
  Lips_Cell S_equal = LIPS_DEFINE_FUNCTION(machine, equal, LIPS_NUM_ARGS_2|LIPS_NUM_ARGS_VAR, NULL);
  Lips_Define(machine, "=", S_equal);
  Lips_Cell S_nilp = LIPS_DEFINE_FUNCTION(machine, nilp, LIPS_NUM_ARGS_1, NULL);
  Lips_Define(machine, "not", S_nilp);
  LIPS_DEFINE_FUNCTION(machine, typeof, LIPS_NUM_ARGS_1, NULL);
  LIPS_DEFINE_FUNCTION(machine, throw, LIPS_NUM_ARGS_1, NULL);
  LIPS_DEFINE_FUNCTION(machine, call, LIPS_NUM_ARGS_2, NULL);
  LIPS_DEFINE_FUNCTION(machine, format, LIPS_NUM_ARGS_1|LIPS_NUM_ARGS_VAR, NULL);
  LIPS_DEFINE_FUNCTION(machine, concat, LIPS_NUM_ARGS_2|LIPS_NUM_ARGS_VAR, NULL);
  LIPS_DEFINE_FUNCTION(machine, slurp, LIPS_NUM_ARGS_1, NULL);
  LIPS_DEFINE_FUNCTION(machine, load, LIPS_NUM_ARGS_1, NULL);

  LIPS_DEFINE_MACRO(machine, lambda, LIPS_NUM_ARGS_2|LIPS_NUM_ARGS_VAR, NULL);
  LIPS_DEFINE_MACRO(machine, macro, LIPS_NUM_ARGS_2|LIPS_NUM_ARGS_VAR, NULL);
  LIPS_DEFINE_MACRO(machine, define, LIPS_NUM_ARGS_2, NULL);
  LIPS_DEFINE_MACRO(machine, quote, LIPS_NUM_ARGS_1, NULL);
  LIPS_DEFINE_MACRO(machine, progn, LIPS_NUM_ARGS_1|LIPS_NUM_ARGS_VAR, NULL);
  LIPS_DEFINE_MACRO(machine, if, LIPS_NUM_ARGS_3|LIPS_NUM_ARGS_VAR, NULL);
  LIPS_DEFINE_MACRO(machine, when, LIPS_NUM_ARGS_2|LIPS_NUM_ARGS_VAR, NULL);
  LIPS_DEFINE_MACRO(machine, catch, LIPS_NUM_ARGS_1|LIPS_NUM_ARGS_VAR, NULL);
  LIPS_DEFINE_MACRO(machine, intern, LIPS_NUM_ARGS_1, NULL);

  machine->T_integer  = Lips_Define(machine, "<integer>", Lips_NewSymbol(machine, "integer"));
  machine->T_real     = Lips_Define(machine, "<real>", Lips_NewSymbol(machine, "real"));
  machine->T_string   = Lips_Define(machine, "<string>", Lips_NewSymbol(machine, "string"));
  machine->T_symbol   = Lips_Define(machine, "<symbol>", Lips_NewSymbol(machine, "symbol"));
  machine->T_pair     = Lips_Define(machine, "<pair>", Lips_NewSymbol(machine, "pair"));
  machine->T_function = Lips_Define(machine, "<function>", Lips_NewSymbol(machine, "function"));
  machine->T_macro    = Lips_Define(machine, "<macro>", Lips_NewSymbol(machine, "macro"));

  return machine;
}

LIPS_COLD_FUNCTION Lips_Machine*
Lips_DefaultCreateMachine()
{
  return Lips_CreateMachine(&(Lips_MachineCreateInfo) {
      .alloc = &DefaultAlloc,
      .dealloc = &DefaultDealloc,
      .open_file = &DefaultOpenFile,
      .read_file = &DefaultReadFile,
      .close_file = &DefaultCloseFile,
      .initial_stack_size = 16*1024
    });
}

LIPS_COLD_FUNCTION void
Lips_DestroyMachine(Lips_Machine* machine)
{
  PopEnv(machine);
  assert(machine->envpos == STACK_INVALID_POS &&
         "Tried to call Lips_DestroyMachine while running Lisp code");
  // clear all resources
  Lips_DeallocFunc dealloc = machine->dealloc;
  DestroyStack(dealloc, &machine->stack);
  for (uint32_t i = 0; i < machine->numbuckets; i++) {
    // destroy each cell in bucket
    Bucket* bucket = &machine->buckets[i];
    for (uint32_t i = 0; bucket->size > 0; i++) {
      Lips_Cell cell = (Lips_Value*)bucket->data + i;
      if ((cell->type & DEAD_MASK) == 0) {
        DestroyCell(machine, cell);
        bucket->size--;
      }
    }
    DestroyBucket(machine, bucket, sizeof(Lips_Value));
  }
  for (uint32_t i = 0; i < machine->numstr_buckets; i++)
    DestroyBucket(machine, &machine->str_buckets[i], sizeof(StringData));
  for (uint32_t i = 0; i < NUM_STRING_POOL_SIZES; i++) {
    for (uint32_t j = 0; j < STRING_POOL_NUM_CHUNKS; j++) {
      Bucket* chunk = &machine->string_pools[i].chunks[j];
      if (chunk->data)
        DestroyBucket(machine, chunk, string_pool_sizes[i]);
    }
  }
  dealloc(machine->buckets, sizeof(Bucket) * machine->allocbuckets);
  dealloc(machine->str_buckets, sizeof(Bucket) * machine->allocstr_buckets);
  dealloc(machine, sizeof(Lips_Machine));
}

LIPS_HOT_FUNCTION Lips_Cell
Lips_Eval(Lips_Machine* machine, Lips_Cell cell)
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
    return Lips_InternCell(machine, cell);
  case LIPS_TYPE_PAIR: {
    Lips_Cell name = GET_HEAD(cell);
    Lips_Cell args = GET_TAIL(cell);
    TYPE_CHECK(machine, LIPS_TYPE_SYMBOL, name);
    Lips_Cell callable = Lips_InternCell(machine, name);
    if (callable == NULL || callable == machine->S_nil) {
      Lips_ThrowError(machine, "Eval: undefined symbol '%s'", GET_STR(name)->ptr);
    }
    return Lips_Invoke(machine, callable, args);
  }
  }
#else
  // don't even try to understand...
  // you just need to know that this is a non-recursive eval loop
  if (GET_TYPE(cell) == LIPS_TYPE_PAIR) {
    Lips_Cell ret;
    Lips_Cell name;
    const uint32_t startpos = machine->evalpos;
    EvalState* state = PushEvalState(machine);
    state->sexp = cell;
  eval:
    state->flags = 0;
    ES_PASSED_ARGS(state) = GET_TAIL(state->sexp);
    name = GET_HEAD(state->sexp);
    if (LIPS_UNLIKELY(name == NULL)) {
      ret = machine->S_nil;
    } else {
      if (LIPS_UNLIKELY(!Lips_IsSymbol(name))) {
        Lips_SetError(machine, "First part of evaluated s-expression always must be a symbol");
        goto except;
      }
      state->callable = Lips_InternCell(machine, name);
      if (LIPS_UNLIKELY(state->callable == machine->S_nil)) {
        Lips_SetError(machine, "Eval: undefined symbol '%s'", GET_STR_PTR(name));
        goto except;
      }
      ES_ARG_COUNT(state) = CheckArgumentCount(machine, state->callable, ES_PASSED_ARGS(state));
      if (LIPS_UNLIKELY(ES_ARG_COUNT(state) == (uint32_t)-1)) {
        goto except;
      }
      if (Lips_IsFunction(state->callable)) {
        state->args.array = StackRequireFromBack(machine->alloc, machine->dealloc, &machine->stack,
                                                 ES_ARG_COUNT(state) * sizeof(Lips_Cell));
        ES_LAST_ARG(state) = state->args.array;
      arg:
        while (ES_PASSED_ARGS(state)) {
          // eval arguments
          Lips_Cell argument = GET_HEAD(ES_PASSED_ARGS(state));
          if (GET_TYPE(argument) == LIPS_TYPE_PAIR) {
            state = PushEvalState(machine);
            state->sexp = argument;
            goto eval;
          } else {
            *ES_LAST_ARG(state) = EvalNonPair(machine, argument);
            ES_LAST_ARG(state)++;
            ES_PASSED_ARGS(state) = GET_TAIL(ES_PASSED_ARGS(state));
          }
        }
      } else {
        state->args.list = ES_PASSED_ARGS(state);
      }
      if (GET_TYPE(state->callable) & ((LIPS_TYPE_C_FUNCTION^LIPS_TYPE_FUNCTION)|
                                       (LIPS_TYPE_C_MACRO^LIPS_TYPE_MACRO))) {
        Lips_Cell c = state->callable;
        if (Lips_IsFunction(c)) {
          // every function must be executed inside it's own environment
          PushEnv(machine);
          uint32_t count = ES_ARG_COUNT(state);
          ret = GET_CFUNC(c).ptr(machine, count, state->args.array, GET_CFUNC(c).udata);
          // array of arguments no more needed; we can free it
          FreeArgumentArray(machine, count, state->args.array);
          PopEnv(machine);
        } else {
          ret = GET_CMACRO(c).ptr(machine, state->args.list, GET_CMACRO(c).udata);
        }
        // fucntion returned null so need to go to latest catch
        if (LIPS_UNLIKELY(ret == NULL)) {
          if (ES_STAGE(state) == ES_STAGE_EXECUTING_CODE) {
            goto code;
          }
        except:
          if (machine->catchpos >= startpos) {
            // unhandled throw
            PopEvalState(machine);
            return NULL;
          }
          state = UnwindStack(machine);
          ret = machine->throwvalue;
        }
      } else {
        // push a new environment
        if (ES_ARG_COUNT(state) > 0 || Lips_IsFunction(state->callable)) {
          HashTable* env = PushEnv(machine);
          ES_INC_NUM_ENVS(state);
          if (ES_ARG_COUNT(state) > 0) {
            if (Lips_IsFunction(state->callable)) {
              DefineArgumentArray(machine, state->callable, ES_ARG_COUNT(state), state->args.array);
              // array of arguments no more needed; we can free it
              FreeArgumentArray(machine, ES_ARG_COUNT(state), state->args.array);
            } else {
              DefineArgumentList(machine, state->callable, state->args.list);
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
            state = PushEvalState(machine);
            state->sexp = expression;
            goto eval;
          } else {
            ret = EvalNonPair(machine, expression);
          }
          ES_CODE(state) = GET_TAIL(ES_CODE(state));
        }
      }
    }
    state = PopEvalState(machine);
    if (machine->evalpos != startpos) {
      switch (ES_STAGE(state)) {
      case ES_STAGE_EVALUATING_ARGS:
        *ES_LAST_ARG(state) = ret;
        ES_LAST_ARG(state)++;
        ES_PASSED_ARGS(state) = GET_TAIL(ES_PASSED_ARGS(state));
        goto arg;
      case ES_STAGE_EXECUTING_CODE:
        ES_CODE(state) = GET_TAIL(ES_CODE(state));
        goto code;
      }
    }
    return ret;
  } else {
    return EvalNonPair(machine, cell);
  }
#endif
  return cell;
}

Lips_Cell
Lips_EvalString(Lips_Machine* machine, const char* str, const char* filename)
{
  Parser parser;
  ParserInit(&parser, str, strlen(str));
  Lips_Cell ast = GenerateAST(machine, &parser);
  Lips_Cell str_filename;
  if (filename == NULL)  {
    str_filename = machine->default_file;
  } else {
    str_filename = Lips_NewString(machine, filename);
  }
  Lips_Cell temp = Lips_ListPushBack(machine, machine->S_filename, str_filename);
  Lips_Cell ret = Lips_Eval(machine, ast);
  GET_TAIL(temp) = NULL; // this equals to Lips_ListPop(machine, machine->S_filename);
  return ret;
}

const char*
Lips_GetError(const Lips_Machine* machine)
{
  return machine->errbuff;
}

LIPS_HOT_FUNCTION void
Lips_GarbageCollect(Lips_Machine* machine)
{
  Mark(machine);
  Sweep(machine);
}

Lips_Cell
Lips_Nil(Lips_Machine* machine)
{
  return machine->S_nil;
}

Lips_Cell
Lips_NewInteger(Lips_Machine* machine, int64_t num)
{
  Lips_Cell cell = NewCell(machine);
  cell->type = LIPS_TYPE_INTEGER;
  cell->data.integer = num;
  return cell;
}

Lips_Cell
Lips_NewReal(Lips_Machine* machine, double num)
{
  Lips_Cell cell = NewCell(machine);
  cell->type = LIPS_TYPE_REAL;
  cell->data.real = num;
  return cell;
}

Lips_Cell
Lips_NewString(Lips_Machine* machine, const char* str)
{
  return Lips_NewStringN(machine, str, strlen(str));
}

Lips_Cell
Lips_NewStringN(Lips_Machine* machine, const char* str, uint32_t n)
{
  Lips_Cell cell = NewCell(machine);
  cell->type = LIPS_TYPE_STRING;
  GET_STR(cell) = StringCreate(machine, str, n);
  GET_STR(cell)->refs++;
  return cell;
}

Lips_Cell
Lips_NewSymbol(Lips_Machine* machine, const char* str)
{
  return Lips_NewSymbolN(machine, str, strlen(str));
}

Lips_Cell
Lips_NewSymbolN(Lips_Machine* machine, const char* str, uint32_t n)
{
  Lips_Cell cell = NewCell(machine);
  cell->type = LIPS_TYPE_SYMBOL;
  StringData* data = NULL;
  // try to find string in environments so we don't waste space
  HashTable* env = MachineEnv(machine);
  uint32_t hash = ComputeHashN(str, n);
  do {
    data = HashTableSearchKey(env, hash, str, n);
    if (data)
      goto skip;
    env = EnvParent(machine, env);
  } while (env);
  data = StringCreateWithHash(machine, str, n, hash);
 skip:
  GET_STR(cell) = data;
  GET_STR(cell)->refs++;
  return cell;
}

Lips_Cell
Lips_NewKeyword(Lips_Machine* machine, const char* str)
{
  return Lips_NewKeywordN(machine, str, strlen(str));
}

Lips_Cell
Lips_NewKeywordN(Lips_Machine* machine, const char* str, uint32_t n)
{
  Lips_Cell cell = Lips_NewSymbolN(machine, str, n);
  cell->type = LIPS_TYPE_KEYWORD;
  return cell;
}

Lips_Cell
Lips_NewPair(Lips_Machine* machine, Lips_Cell head, Lips_Cell tail)
{
  Lips_Cell cell = NewCell(machine);
  cell->type = LIPS_TYPE_PAIR;
  cell->data.list.head = head;
  cell->data.list.tail = tail;
  return cell;
}

Lips_Cell
Lips_NewList(Lips_Machine* machine, uint32_t numCells, Lips_Cell* cells)
{
  Lips_Cell list = Lips_NewPair(machine, NULL, NULL);
  Lips_Cell curr = list;
  while (numCells--) {
    GET_HEAD(curr) = *cells;
    if (numCells > 0) {
      GET_TAIL(curr) = Lips_NewPair(machine, NULL, NULL);
      curr = GET_TAIL(curr);
    }
    cells++;
  }
  return list;
}

Lips_Cell
Lips_NewFunction(Lips_Machine* machine, Lips_Cell args, Lips_Cell body, uint8_t numargs)
{
  Lips_Cell cell = NewCell(machine);
  cell->type = LIPS_TYPE_FUNCTION | (numargs << 8);
  // TODO: check all arguments are symbols
  GET_LFUNC(cell).args = args;
  GET_LFUNC(cell).body = body;
  return cell;
}

Lips_Cell
Lips_NewMacro(Lips_Machine* machine, Lips_Cell args, Lips_Cell body, uint8_t numargs)
{
  Lips_Cell cell = NewCell(machine);
  cell->type = LIPS_TYPE_MACRO | (numargs << 8);
  GET_LFUNC(cell).args = args;
  GET_LFUNC(cell).body = body;
  return cell;
}

Lips_Cell
Lips_NewCFunction(Lips_Machine* machine, Lips_Func function, uint8_t numargs, void* udata)
{
  Lips_Cell cell = NewCell(machine);
  cell->type = LIPS_TYPE_C_FUNCTION | (numargs << 8);
  GET_CFUNC(cell).ptr = function;
  GET_CFUNC(cell).udata = udata;
  return cell;
}

Lips_Cell
Lips_NewCMacro(Lips_Machine* machine, Lips_Macro function, uint8_t numargs, void* udata)
{
  Lips_Cell cell = NewCell(machine);
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
Lips_PrintCell(Lips_Machine* machine, Lips_Cell cell, char* buff, uint32_t size)
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
      ptr += Lips_PrintCell(machine, GET_HEAD(cell), ptr, size - (ptr - buff));
      cell = GET_TAIL(cell);
      while (cell && GET_HEAD(cell)) {
        PRINT(" ");
        ptr += Lips_PrintCell(machine, GET_HEAD(cell), ptr, size - (ptr - buff));
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
          PRINT("\"%s\"", GET_STR_PTR(GET_HEAD(cell)));
          break;
        case LIPS_TYPE_SYMBOL:
          PRINT("%s", GET_STR_PTR(GET_HEAD(cell)));
          break;
        case LIPS_TYPE_KEYWORD:
          PRINT(":%s", GET_STR_PTR(GET_HEAD(cell)));
          break;
        case LIPS_TYPE_PAIR:
          PRINT("(");
          prev = StackRequire(machine->alloc, machine->dealloc,
                              &machine->stack, sizeof(Lips_Cell));
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
        assert(StackRelease(&machine->stack, prev) == sizeof(Lips_Cell));
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
      PRINT("\"%s\"", GET_STR_PTR(cell));
      break;
    case LIPS_TYPE_SYMBOL:
      PRINT("%s", GET_STR_PTR(cell));
      break;
    case LIPS_TYPE_KEYWORD:
      PRINT(":%s", GET_STR_PTR(cell));
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
Lips_ListLength(Lips_Machine* machine, Lips_Cell list)
{
  (void)machine;
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
Lips_ListLastElement(Lips_Machine* machine, Lips_Cell list, uint32_t* length)
{
  TYPE_CHECK(machine, LIPS_TYPE_PAIR, list);
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
Lips_ListPushBack(Lips_Machine* machine, Lips_Cell list, Lips_Cell elem)
{
  TYPE_CHECK(machine, LIPS_TYPE_PAIR, list);
  if (GET_HEAD(list) == NULL) {
    GET_HEAD(list) = elem;
  } else {
    while (GET_TAIL(list) != NULL) {
      list = GET_TAIL(list);
    }
    GET_TAIL(list) = Lips_NewPair(machine, elem, NULL);
  }
  return list;
}

Lips_Cell
Lips_ListPopBack(Lips_Machine* machine, Lips_Cell list)
{
  TYPE_CHECK(machine, LIPS_TYPE_PAIR, list);
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
Lips_Define(Lips_Machine* machine, const char* name, Lips_Cell cell)
{
  assert(cell);
  HashTable* env = MachineEnv(machine);
  while (env->flags & HASH_TABLE_CONSTANT_FLAG) {
    env = EnvParent(machine, env);
  }
  StringData* key = StringCreate(machine, name, strlen(name));
  Lips_Cell* ptr = HashTableInsert(machine, env, key, cell);
  key->refs++;
  if (ptr == NULL) {
    LIPS_THROW_ERROR(machine, "Value '%s' is already defined", name);
  }
  return cell;
}

Lips_Cell
Lips_DefineCell(Lips_Machine* machine, Lips_Cell cell, Lips_Cell value)
{
  TYPE_CHECK(machine, LIPS_TYPE_SYMBOL|LIPS_TYPE_STRING, cell);
  assert(value);
  HashTable* env = MachineEnv(machine);
  while (env->flags & HASH_TABLE_CONSTANT_FLAG) {
    env = EnvParent(machine, env);
  }
  Lips_Cell* ptr = HashTableInsert(machine, env, GET_STR(cell), value);
  GET_STR(cell)->refs++;
  if (ptr == NULL) {
    LIPS_THROW_ERROR(machine, "Value '%s' is already defined");
  }
  return value;
}

Lips_Cell
Lips_Intern(Lips_Machine* machine, const char* name)
{
  HashTable* env = MachineEnv(machine);
  do {
    Lips_Cell* ptr = HashTableSearch(env, name);
    if (ptr) {
      return *ptr;
    }
    env = EnvParent(machine, env);
  } while (env);
  return NULL;
}

LIPS_HOT_FUNCTION Lips_Cell
Lips_InternCell(Lips_Machine* machine, Lips_Cell cell)
{
  TYPE_CHECK(machine, LIPS_TYPE_SYMBOL|LIPS_TYPE_STRING, cell);
  HashTable* env = MachineEnv(machine);
  do {
    Lips_Cell* ptr = HashTableSearchWithHash(env, GET_STR(cell)->hash, GET_STR_PTR(cell));
    if (ptr) {
      return *ptr;
    }
    env = EnvParent(machine, env);
  } while (env);
  return machine->S_nil;
}

LIPS_HOT_FUNCTION Lips_Cell
Lips_Invoke(Lips_Machine* machine, Lips_Cell callable, Lips_Cell args)
{
  Lips_Cell ast = Lips_NewPair(machine, callable, args);
  Lips_Cell ret = Lips_Eval(machine, ast);
  // FIXME: should we destroy 'ast'?
  return ret;
}

const char*
Lips_SetError(Lips_Machine* machine, const char* fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  int len = vsnprintf(machine->errbuff, sizeof(machine->errbuff), fmt, ap);
  va_end(ap);
  machine->throwvalue = Lips_NewStringN(machine, machine->errbuff, len);
  return machine->errbuff;
}

void
Lips_CalculateMemoryStats(Lips_Machine* machine, Lips_MemoryStats* stats)
{
  stats->allocated_bytes = 0;
  stats->cell_allocated_bytes = 0;
  stats->cell_used_bytes = 0;
  stats->str_allocated_bytes = 0;
  stats->str_used_bytes = 0;

  stats->allocated_bytes += sizeof(Lips_Machine);
  stats->allocated_bytes += sizeof(Bucket) * machine->allocbuckets;
  stats->allocated_bytes += sizeof(Bucket) * machine->allocstr_buckets;
  stats->allocated_bytes += machine->stack.size;

  for (uint32_t i = 0; i < machine->numbuckets; i++) {
    Bucket* bucket = &machine->buckets[i];
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
  for (uint32_t i = 0; i < machine->numstr_buckets; i++) {
    Bucket* bucket = &machine->str_buckets[i];
    stats->str_allocated_bytes += BUCKET_SIZE * sizeof(StringData);
    StringData* const data = bucket->data;
    for (uint32_t i = 0, n = bucket->size; n > 0; i++) {
      uint32_t mask = *(uint32_t*)&data[i];
      if (mask ^ DEAD_MASK) {
        stats->str_used_bytes += sizeof(StringData);
        // +1 for null-terminator
        stats->str_used_bytes += data[i].length + 1;
        n--;
      }
    }
  }
  for (uint32_t i = 0; i < NUM_STRING_POOL_SIZES; i++) {
    for (uint32_t j = 0; j < STRING_POOL_NUM_CHUNKS; j++) {
      Bucket* chunk = &machine->string_pools[i].chunks[j];
      if (chunk->data == NULL)
        break;
      stats->str_allocated_bytes += chunk->num_elements * string_pool_sizes[i];;
    }
  }
  stats->allocated_bytes += stats->cell_allocated_bytes + stats->str_allocated_bytes;
}

int64_t
Lips_GetInteger(Lips_Machine* machine, Lips_Cell cell)
{
  (void)machine;
  TYPE_CHECK_FORCED(LIPS_TYPE_INTEGER, cell);
  return GET_INTEGER(cell);
}

double
Lips_GetReal(Lips_Machine* machine, Lips_Cell cell)
{
  (void)machine;
  TYPE_CHECK_FORCED(LIPS_TYPE_REAL, cell);
  return GET_REAL(cell);
}

const char*
Lips_GetString(Lips_Machine* machine, Lips_Cell cell)
{
  (void)machine;
  TYPE_CHECK(machine, LIPS_TYPE_STRING|LIPS_TYPE_SYMBOL, cell);
  return GET_STR_PTR(cell);
}

Lips_Cell
Lips_CAR(Lips_Machine* machine, Lips_Cell cell)
{
  (void)machine;
  TYPE_CHECK(machine, LIPS_TYPE_PAIR, cell);
  return GET_HEAD(cell);
}

Lips_Cell
Lips_CDR(Lips_Machine* machine, Lips_Cell cell)
{
  (void)machine;
  TYPE_CHECK(machine, LIPS_TYPE_PAIR, cell);
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

void*
DefaultOpenFile(const char* file, size_t* datasize)
{
  FILE* fp = fopen(file, "rb");
  if (fp) {
    fseek(fp, 0, SEEK_END);
    *datasize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
  }
  return fp;
}

void
DefaultReadFile(void* filehandle, char* buffer, size_t bytes)
{
  fread(buffer, 1, bytes, filehandle);
}

void
DefaultCloseFile(void* filehandle)
{
  fclose(filehandle);
}

void
DestroyCell(Lips_Machine* machine, Lips_Cell cell) {
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
  case LIPS_TYPE_KEYWORD:
    StringDestroy(machine, GET_STR(cell));
    break;
  case LIPS_TYPE_PAIR:

    break;
  }
}

StringData*
StringCreateEmpty(Lips_Machine* machine, uint32_t n)
{
  Bucket* bucket = FindBucketForString(machine);
  StringData* str = BucketNew(bucket, sizeof(StringData));
  str->length = n;
  str->bucket_index = bucket - machine->str_buckets;
  str->refs = 0;
  StringPool* pool = GetStringPool(machine, n+1);
  if (pool) {
    str->data = FindSpaceForString(pool, machine->alloc);
  } else {
    str->data = machine->alloc(n+1);
  }
  return str;
}

StringData*
StringCreate(Lips_Machine* machine, const char* str, uint32_t n)
{
  StringData* string = StringCreateEmpty(machine, n);
  strncpy(string->data, str, n);
  string->data[n] = '\0';
  string->hash = ComputeHash(string->data);
  return string;
}

StringData*
StringCreateWithHash(Lips_Machine* machine, const char* str, uint32_t n, uint32_t hash)
{
  StringData* string = StringCreateEmpty(machine, n);
  strncpy(string->data, str, n);
  string->data[n] = '\0';
  string->hash = hash;
  return string;
}

void
StringDestroy(Lips_Machine* machine, StringData* str)
{
  str->refs--;
  if (str->refs > 0) {
    return;
  }
  StringPool* pool = GetStringPool(machine, str->length+1);
  if (pool) {
    RemoveString(pool, str->data);
  } else {
    machine->dealloc(str->data, str->length+1);
  }
  Bucket* bucket = &machine->str_buckets[str->bucket_index];
  BucketDelete(bucket, str, sizeof(StringData));
}

int
StringEqual(const StringData* lhs, const StringData* rhs)
{
  if (lhs->hash == rhs->hash) {
    if (lhs->length == rhs->length) {
      if (strcmp(lhs->data, rhs->data) == 0)
        return 1;
    }
  }
  return 0;
}

const char*
GDB_lips_to_c_string(Lips_Machine* machine, Lips_Cell cell)
{
  // FIXME: this is terrible
  char* buff = machine->errbuff + 512;
  Lips_PrintCell(machine, cell, buff, 512);
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
ParseNumber(Lips_Machine* machine, const Token* token) {
  int is_float = 0;
  for (uint32_t i = 0; i < token->length; i++) {
    if (!IS_DIGIT(token->str[i])) {
      if (token->str[i] == '.') {
        is_float++;
      } else {
        LOG_ERROR(machine, "Found undefined character '%c' when parsing number in token '%.*s'",
                  token->str[i], token->length, token->str);
        return NULL;
      }
    }
  }
  if (is_float > 1) {
    LOG_ERROR(machine, "Encountered more than 1 '.' when parsing float in token '%.*s'",
              token->length, token->str);
    return NULL;
  }
  // TODO: use strtod and strtoll correctly
  if (is_float) {
    return Lips_NewReal(machine, strtod(token->str, NULL));
  } else {
    return Lips_NewInteger(machine, strtoll(token->str, NULL, 10));
  }
}

LIPS_HOT_FUNCTION Lips_Cell
NewCell(Lips_Machine* machine)
{
  for (uint32_t i = machine->numbuckets; i > 0; i--)
    if (machine->buckets[i-1].size < BUCKET_SIZE) {
      // we found a bucket with available storage, use it
      return BucketNew(&machine->buckets[i-1], sizeof(Lips_Value));
    }
  if (machine->numbuckets == machine->allocbuckets) {
    // we're out of storage for buckets, allocate more
    Bucket* new_buckets = machine->alloc(machine->allocbuckets * 2);
    if (!new_buckets) return NULL;
    memcpy(new_buckets, machine->buckets, machine->numbuckets * sizeof(Bucket));
    machine->buckets = new_buckets;
    machine->dealloc(new_buckets, sizeof(Bucket) * machine->allocbuckets);
    machine->allocbuckets = machine->allocbuckets * 2;
  }
  // push back a new bucket
  Bucket* new_bucket = &machine->buckets[machine->numbuckets];
  CreateBucket(machine->alloc, new_bucket, BUCKET_SIZE, sizeof(Lips_Value));
  machine->numbuckets++;
  return BucketNew(new_bucket, sizeof(Lips_Value));
}

Lips_Cell
GenerateAST(Lips_Machine* machine, Parser* parser)
{
#if 0
  // this is an implementation with recursion, it is much more readable but a bit slower
  Lips_Cell tree = NULL;
  Lips_Cell cell = NULL;
  int code = ParserNextToken(parser);
  if (code == LIPS_EOF) {
    LOG_ERROR(machine, "EOF: expected \"");
  } else if (code == 1) {
    switch (parser->currtok.str[0]) {
    case '(':
      tree = Lips_NewPair(machine, NULL, NULL);
      cell = tree;
      while (1) {
        GET_HEAD(cell) = Lips_GenerateAST(machine, parser);
        if (GET_HEAD(cell) == NULL)
          break;
        GET_TAIL(cell) = Lips_NewPair(machine, NULL, NULL);
        cell = GET_TAIL(cell);
      }
      break;
    case ')':
      break;
    case '"':
      tree = Lips_NewStringN(machine, parser->currtok.str+1, parser->currtok.length-2);
      break;
    default:
      if (Lips_IsTokenNumber(&parser->currtok)) {
        tree = Lips_ParseNumber(machine, &parser->currtok);
      } else {
        tree = Lips_NewSymbolN(machine, parser->currtok.str, parser->currtok.length);
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
        cell = Lips_NewStringN(machine,
                               parser->currtok.str+1, parser->currtok.length-2);
        break;
      case ':':
        cell = Lips_NewKeywordN(machine,
                                parser->currtok.str+1, parser->currtok.length-1);
        break;
      default:
        if (Lips_IsTokenNumber(&parser->currtok)) {
          cell = ParseNumber(machine, &parser->currtok);
        } else {
          cell = Lips_NewSymbolN(machine, parser->currtok.str, parser->currtok.length);
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
  Lips_Cell* stack = StackRequire(machine->alloc, machine->dealloc,
                                  &machine->stack, numbytes);
  int counter = 0;
  // this cycle looks messy but it works :)
  while (code == 1) {
    switch (parser->currtok.str[0]) {
    case '(':
      // add new cell to the queue
      if (cell == NULL) {
        cell = Lips_NewPair(machine, NULL, NULL);
        stack[counter] = cell;
      } else {
        stack[counter] = cell;
        GET_HEAD(cell) = Lips_NewPair(machine, NULL, NULL);
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
      GET_HEAD(cell) = Lips_NewStringN(machine,
                                       parser->currtok.str+1, parser->currtok.length-2);
      break;
    case ':':
      GET_HEAD(cell) = Lips_NewKeywordN(machine,
                                        parser->currtok.str+1, parser->currtok.length-1);
      break;
    default:
      if (Lips_IsTokenNumber(&parser->currtok)) {
        GET_HEAD(cell) = ParseNumber(machine, &parser->currtok);
      } else {
        GET_HEAD(cell) = Lips_NewSymbolN(machine, parser->currtok.str, parser->currtok.length);
      }
      break;
    }
    code = ParserNextToken(parser);
    // don't waste memory by adding an empty list to the end
    if (parser->currtok.str[0] != ')') {
      // push new cell to the end
      GET_TAIL(cell) = Lips_NewPair(machine, NULL, NULL);
      cell = GET_TAIL(cell);
    }
    continue;
  skip_pushing:
    code = ParserNextToken(parser);
  }
  assert(counter == 0 && "parser internal error"); // I think this is useful, should I remove it?
  if (code == LIPS_EOF) {
    LOG_ERROR(machine, "EOF: expected \"");
  }
  Lips_Cell ret = stack[0];
  assert(StackRelease(&machine->stack, stack) == numbytes);
  return ret;
#endif
}

void
CreateBucket(Lips_AllocFunc alloc, Bucket* bucket, uint32_t num_elements, uint32_t elem_size)
{
  uint32_t i;
  bucket->data = (Lips_Value*)alloc(num_elements * elem_size);
  bucket->num_elements = num_elements;
  bucket->size = 0;
  bucket->next = 0;
  uint8_t* data = bucket->data;
  for (i = 0; i < num_elements; i++) {
    *(uint32_t*)data = (i + 1) | DEAD_MASK;
    data += elem_size;
  }
}

void
DestroyBucket(Lips_Machine* machine, Bucket* bucket, uint32_t elem_size)
{
  // free bucket's memory
  machine->dealloc(bucket->data, elem_size * bucket->num_elements);
}

void*
BucketNew(Bucket* bucket, uint32_t elem_size)
{
  assert(bucket->size < bucket->num_elements && "Bucket out of space");
  void* ret = (char*)bucket->data + bucket->next * elem_size;
  bucket->next = *(uint32_t*)ret ^ DEAD_MASK;
  bucket->size++;
  return ret;
}

void
BucketDelete(Bucket* bucket, void* data, uint32_t elem_size)
{
  uint32_t index = ((char*)data - (char*)bucket->data) / elem_size;
  assert(index < bucket->num_elements && "cell doesn't belong to this Bucket");
  assert(bucket->size > 0 && "Bucket is empty");
  *(uint32_t*)data = bucket->next | DEAD_MASK;
  bucket->next = index;
  bucket->size--;
}

void
CreateStack(Lips_AllocFunc alloc, Stack* stack, uint32_t size)
{
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
PushEvalState(Lips_Machine* machine)
{
  EvalState* newstate = StackRequireFromBack(machine->alloc, machine->dealloc,
                                             &machine->stack, sizeof(EvalState));
#ifndef NDEBUG
  memset(newstate, 0, sizeof(EvalState));
#endif
  newstate->parent = machine->evalpos;
  machine->evalpos = (uint8_t*)newstate - machine->stack.data;
  return newstate;
}

EvalState*
PopEvalState(Lips_Machine* machine)
{
  EvalState* child = CURRENT_EVAL_STATE(machine);
  // because of tail call optimization 1 state may have more than 1 environments
  for (uint32_t i = 0; i < ES_NUM_ENVS(child); i++) {
    PopEnv(machine);
  }
  machine->evalpos = child->parent;
  assert(StackReleaseFromBack(&machine->stack, sizeof(EvalState)) == child);
  if (machine->evalpos == STACK_INVALID_POS) {
    return NULL;
  }
  return CURRENT_EVAL_STATE(machine);
}

void
PushCatch(Lips_Machine* machine)
{
  EvalState* state = CURRENT_EVAL_STATE(machine);
  ES_CATCH_PARENT(state) = machine->catchpos;
  machine->catchpos = (uint8_t*)state - machine->stack.data;
}

void
PopCatch(Lips_Machine* machine)
{
  EvalState* catch = CURRENT_EVAL_STATE(machine);
  machine->catchpos = ES_CATCH_PARENT(catch);
}

EvalState*
UnwindStack(Lips_Machine* machine)
{
  EvalState* state = (EvalState*)(machine->stack.data + machine->evalpos);
  while (machine->catchpos != machine->evalpos) {
    state = PopEvalState(machine);
  }
  PopCatch(machine);
  return state;
}

HashTable*
MachineEnv(Lips_Machine* machine)
{
  HashTable* env = (HashTable*)(machine->stack.data + machine->envpos);
  return env;
}

HashTable*
PushEnv(Lips_Machine* machine)
{
  HashTable* env = HashTableCreate(machine->alloc, machine->dealloc, &machine->stack);
  env->parent = machine->envpos;
  machine->envpos = (uint8_t*)env - machine->stack.data;
  return env;
}

void
PopEnv(Lips_Machine* machine)
{
  HashTable* env = MachineEnv(machine);
  Iterator it;
  for (HashTableIterate(env, &it); !IteratorIsEmpty(&it); IteratorNext(&it)) {
    StringData* key;
    Lips_Cell cell;
    IteratorGet(&it, &key, &cell);
    StringDestroy(machine, key);
  }
  HashTableDestroy(&machine->stack, env);
  machine->envpos = env->parent;
}

HashTable*
EnvParent(Lips_Machine* machine, HashTable* env)
{
  if (env->parent == STACK_INVALID_POS) {
    return NULL;
  }
  HashTable* parent = (HashTable*)(machine->stack.data + env->parent);
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
HashTableReserve(Lips_Machine* machine, HashTable* ht, uint32_t capacity)
{
  assert(capacity > ht->allocated);
  if (HASH_TABLE_GET_SIZE(ht) == 0) {
    assert(StackRelease(&machine->stack, HASH_TABLE_DATA(ht)) == ht->allocated * sizeof(Node));
  }
  uint32_t preallocated = ht->allocated;
  ht->allocated = capacity;
  Node* nodes = StackRequire(machine->alloc, machine->dealloc, &machine->stack, capacity * sizeof(Node));
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
        HashTableInsert(machine, ht, nodes[i].key, nodes[i].value);
        if (HASH_TABLE_GET_SIZE(ht) == oldSize) break;
      }
    }
    assert(StackRelease(&machine->stack, data + capacity) == preallocated * sizeof(Node));
  }
}

Lips_Cell*
HashTableInsert(Lips_Machine* machine,
                HashTable* ht, StringData* key, Lips_Cell value) {
  assert(value && "Can not insert null");
  if (HASH_TABLE_GET_SIZE(ht) == ht->allocated) {
    uint32_t sz = (HASH_TABLE_GET_SIZE(ht) == 0) ? 1 : (HASH_TABLE_GET_SIZE(ht)<<1);
    HashTableReserve(machine, ht, sz);
  }
  Node* data = HASH_TABLE_DATA(ht);
  uint32_t id = key->hash % ht->allocated;
  while (NODE_VALID(data[id])) {
    // Hash table already has this element
    if (StringEqual(data[id].key, key)) {
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
HashTableSearch(const HashTable* ht, const char* key)
{
  uint32_t hash = ComputeHash(key);
  return HashTableSearchWithHash(ht, hash, key);
}

Lips_Cell*
HashTableSearchWithHash(const HashTable* ht, uint32_t hash, const char* key)
{
  Node* data = HASH_TABLE_DATA(ht);
  uint32_t i = 0;
  uint32_t id = hash;
  while (i < HASH_TABLE_GET_SIZE(ht)) {
    id = id % ht->allocated;
    if (NODE_VALID(data[id])) {
      if (data[id].key->hash == hash && strcmp(data[id].key->data, key) == 0)
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
HashTableSearchKey(const HashTable* ht, uint32_t hash, const char* key, uint32_t n)
{
  Node* data = HASH_TABLE_DATA(ht);
  uint32_t i = 0;
  uint32_t id = hash;
  while (i < HASH_TABLE_GET_SIZE(ht)) {
    id = id % ht->allocated;
    if (NODE_VALID(data[id])) {
      StringData* val = data[id].key;
      if (hash == val->hash &&
          val->length == n &&
          strncmp(val->data, key, n) == 0)
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

uint32_t GetRealNumargs(Lips_Machine* machine, Lips_Cell callable)
{
  (void)machine;
  TYPE_CHECK_FORCED(LIPS_TYPE_FUNCTION|LIPS_TYPE_MACRO, callable);
  return (GET_NUMARGS(callable) & 127) + (GET_NUMARGS(callable) >> 7);
}

Bucket*
FindBucketForString(Lips_Machine* machine)
{
  for (uint32_t i = machine->numstr_buckets; i > 0; i--)
    if (machine->str_buckets[i-1].size < BUCKET_SIZE) {
      return &machine->str_buckets[i-1];
    }
  if (machine->numstr_buckets == machine->allocstr_buckets) {
    // we're out of storage for buckets, allocate more
    Bucket* new_buckets = machine->alloc(machine->allocstr_buckets * 2);
    assert(new_buckets);
    memcpy(new_buckets, machine->str_buckets, machine->numstr_buckets * sizeof(Bucket));
    machine->str_buckets = new_buckets;
    machine->dealloc(new_buckets, sizeof(Bucket) * machine->allocstr_buckets);
    machine->allocstr_buckets = machine->allocstr_buckets * 2;
  }
  Bucket* new_bucket = &machine->str_buckets[machine->numstr_buckets];
  CreateBucket(machine->alloc, new_bucket, BUCKET_SIZE, sizeof(StringData));
  machine->numstr_buckets++;
  return new_bucket;
}

StringPool*
GetStringPool(Lips_Machine* machine, uint32_t str_size_with_0)
{
  for (uint32_t i = 0; i < NUM_STRING_POOL_SIZES; i++) {
    if (str_size_with_0 <= string_pool_sizes[i])
      return &machine->string_pools[i];
  }
  return NULL;
}

char*
FindSpaceForString(StringPool* pool, Lips_AllocFunc alloc)
{
  Bucket* chunk = NULL;
  for (uint32_t i = 0; i < STRING_POOL_NUM_CHUNKS; i++) {
    if (pool->chunks[i].data == NULL) {
      // TODO: don't hardcode
      uint32_t size = pool->initial_chunk_size;
      if (i > 0) {
        size = 4 * pool->chunks[i-1].size;
      }
      chunk = &pool->chunks[i];
      CreateBucket(alloc, chunk, size / pool->str_max_size, pool->str_max_size);
      break;
    } else if (pool->chunks[i].size < pool->chunks[i].num_elements) {
      chunk = &pool->chunks[i];
      break;
    }
  }
  return BucketNew(chunk, pool->str_max_size);
}

void
RemoveString(StringPool* pool, char* str)
{
  for (uint32_t i = 0; i < STRING_POOL_NUM_CHUNKS; i++) {
    if (str >= (char*)pool->chunks[i].data &&
        str < (char*)pool->chunks[i].data + pool->chunks[i].num_elements * pool->str_max_size) {
      // TODO: delete empty chunks
      BucketDelete(&pool->chunks[i], str, pool->str_max_size);
      return;
    }
  }
  // unreachable
  assert(0);
}

void
DefineWithCurrent(Lips_Machine* machine, Lips_Cell name, Lips_Cell value)
{
  TYPE_CHECK_FORCED(LIPS_TYPE_SYMBOL|LIPS_TYPE_STRING, name);
  assert(value);
  HashTable* env = MachineEnv(machine);
  Lips_Cell* ptr = HashTableInsert(machine, env,
                                   GET_STR(name), value);
  GET_STR(name)->refs++;
  assert(ptr && "Internal error(value is already defined)");
}

uint32_t
CheckArgumentCount(Lips_Machine* machine, Lips_Cell callable, Lips_Cell args)
{
  uint32_t numargs = GET_NUMARGS(callable) & (LIPS_NUM_ARGS_VAR-1);
  uint32_t variadic = GET_NUMARGS(callable) & LIPS_NUM_ARGS_VAR;
  uint32_t listlen = (args) ? Lips_ListLength(machine, args) : 0;
  if ((numargs > listlen) ||
      (numargs < listlen && variadic == 0)) {
    Lips_SetError(machine,
                  "Invalid number of arguments, passed %u arguments, but callable accepts %u",
                  listlen, numargs);
    return (uint32_t)-1;
  }
  return listlen;
}

void
DefineArgumentList(Lips_Machine* machine, Lips_Cell callable, Lips_Cell argvalues)
{
  TYPE_CHECK_FORCED(LIPS_TYPE_FUNCTION|LIPS_TYPE_MACRO, callable);
  TYPE_CHECK_FORCED(LIPS_TYPE_PAIR, argvalues);
  // reserve space for hash table
  HashTableReserve(machine, MachineEnv(machine),
                   GetRealNumargs(machine, callable));
  // define variables in a new environment
  Lips_Cell argnames = GET_LFUNC(callable).args;
  uint32_t count = (GET_NUMARGS(callable) & (LIPS_NUM_ARGS_VAR-1));
  for (uint32_t i = 0; i < count; i++) {
    DefineWithCurrent(machine, GET_HEAD(argnames), GET_HEAD(argvalues));
    argnames = GET_TAIL(argnames);
    argvalues = GET_TAIL(argvalues);
  }
  if (GET_NUMARGS(callable) & LIPS_NUM_ARGS_VAR) {
    DefineWithCurrent(machine, GET_HEAD(argnames), argvalues);
  }
}

void
DefineArgumentArray(Lips_Machine* machine, Lips_Cell callable,
                    uint32_t numargs, Lips_Cell* argvalues)
{
  TYPE_CHECK_FORCED(LIPS_TYPE_FUNCTION, callable);
  // reserve space for hash table
  HashTableReserve(machine, MachineEnv(machine),
                   GetRealNumargs(machine, callable));
  // define variables in a new environment
  Lips_Cell argnames = GET_LFUNC(callable).args;
  uint32_t count = (GET_NUMARGS(callable) & (LIPS_NUM_ARGS_VAR-1));
  for (uint32_t i = 0; i < count; i++) {
    if (GET_HEAD(argnames)) {
      Lips_DefineCell(machine, GET_HEAD(argnames), argvalues[i]);
    }
    argnames = GET_TAIL(argnames);
  }
  if (GET_NUMARGS(callable) & LIPS_NUM_ARGS_VAR) {
    Lips_Cell list = Lips_NewList(machine, numargs - count, argvalues+count);
    Lips_DefineCell(machine, GET_HEAD(argnames), list);
  }
}

void
FreeArgumentArray(Lips_Machine* machine, uint32_t numargs, Lips_Cell* args)
{
  assert(StackReleaseFromBack(&machine->stack, numargs * sizeof(Lips_Cell)) == args);
}

Lips_Cell
EvalNonPair(Lips_Machine* machine, Lips_Cell cell)
{
  assert(!Lips_IsList(cell));
  switch (GET_TYPE(cell)) {
  case LIPS_TYPE_INTEGER:
  case LIPS_TYPE_REAL:
  case LIPS_TYPE_STRING:
  case LIPS_TYPE_KEYWORD:
  case LIPS_TYPE_FUNCTION:
  case LIPS_TYPE_C_FUNCTION:
  case LIPS_TYPE_MACRO:
  case LIPS_TYPE_C_MACRO:
    return cell;
  case LIPS_TYPE_SYMBOL:
    return Lips_InternCell(machine, cell);
  }
  assert(0 && "internal error: cell has undefined type");
}

void
Mark(Lips_Machine* machine)
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
  HashTable* env = MachineEnv(machine);
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
      case LIPS_TYPE_KEYWORD:
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
          prev = StackRequire(machine->alloc, machine->dealloc,
                              &machine->stack, sizeof(Lips_Cell));
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
            prev = StackRequire(machine->alloc, machine->dealloc,
                                &machine->stack, sizeof(Lips_Cell));
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
        assert(StackRelease(&machine->stack, prev) == sizeof(Lips_Cell));
        while (!value && depth > 1) {
          prev--;
          depth--;
          value = *prev;
          assert(StackRelease(&machine->stack, prev) == sizeof(Lips_Cell));
        }
        if (value) {
          goto cycle;
        }
      }
    }
    env = EnvParent(machine, env);
  } while (env);
}

void
Sweep(Lips_Machine* machine)
{
  // delete all unmarked cells
  for (uint32_t i = 0; i < machine->numbuckets; i++) {
    Bucket* bucket = &machine->buckets[i];
    uint32_t count = bucket->size;
    for (uint32_t i = 0; count > 0; i++) {
      Lips_Cell cell = (Lips_Value*)bucket->data + i;
      if ((cell->type & DEAD_MASK) == 0) {
        if ((cell->type & MARK_MASK) == 0) {
          // destroy cell
          DestroyCell(machine, cell);
          BucketDelete(bucket, cell, sizeof(Lips_Value));
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
  return Lips_NewList(machine, numargs, args);
}

LIPS_DECLARE_FUNCTION(car)
{
  (void)udata;
  assert(numargs == 1);
  Lips_Cell list = args[0];
  return Lips_CAR(machine, list);
}

LIPS_DECLARE_FUNCTION(cdr)
{
  (void)udata;
  assert(numargs == 1);
  Lips_Cell list = args[0];
  return Lips_CDR(machine, list);
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
        if (StringEqual(GET_STR(lhs), GET_STR(rhs)))
          continue;
      } else if (GET_TYPE(lhs) == LIPS_TYPE_PAIR) {
        // FIXME: this should definitely look nicer but for now it works
        // or does it?
        if ((GET_HEAD(lhs) == NULL && GET_HEAD(rhs)) ||
            (GET_HEAD(lhs) && GET_HEAD(rhs) == NULL)) {
          return machine->S_nil;
        }
        int equals = 0;
        if (GET_HEAD(lhs) && GET_HEAD(rhs)) {
          Lips_Cell cells[2] = { GET_HEAD(lhs), GET_HEAD(rhs) };
          equals = F_equal(machine, 2, cells, NULL) != machine->S_nil;
        } else {
          equals = 1;
        }
        if (equals) {
          if ((GET_TAIL(lhs) == NULL && GET_TAIL(rhs)) ||
              (GET_TAIL(lhs) && GET_TAIL(rhs) == NULL)) {
            return machine->S_nil;
          }
          if (GET_TAIL(lhs) == NULL && GET_TAIL(rhs) == NULL)
            continue;
          Lips_Cell cells[2] = { GET_TAIL(lhs), GET_TAIL(rhs) };
          if (F_equal(machine, 2, cells, NULL) != machine->S_nil)
            continue;
        }
      } else if (GET_TYPE(lhs) == LIPS_TYPE_FUNCTION ||
                 GET_TYPE(rhs) == LIPS_TYPE_MACRO) {
        Lips_Cell cells1[2] = { GET_LFUNC(lhs).args, GET_LFUNC(rhs).args };
        Lips_Cell cells2[2] = { GET_LFUNC(lhs).body, GET_LFUNC(rhs).body };
        if (F_equal(machine, 2, cells1, NULL) != machine->S_nil &&
            F_equal(machine, 2, cells2, NULL) != machine->S_nil)
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
    return machine->S_nil;
  }
  return machine->S_t;
}

LIPS_DECLARE_FUNCTION(nilp)
{
  (void)udata;
  assert(numargs == 1);
  Lips_Cell cells[2] = { args[0], machine->S_nil };
  return F_equal(machine, 2, cells, NULL);
}

LIPS_DECLARE_FUNCTION(typeof)
{
  (void)udata;
  assert(numargs == 1);
  Lips_Cell ret = NULL;
  switch (GET_TYPE(args[0])) {
  default: assert(0 && "faced undefined type");
  case LIPS_TYPE_INTEGER:
    ret = machine->T_integer;
    break;
  case LIPS_TYPE_REAL:
    ret = machine->T_real;
    break;
  case LIPS_TYPE_STRING:
    ret = machine->T_string;
    break;
  case LIPS_TYPE_SYMBOL:
    ret = machine->T_symbol;
    break;
  case LIPS_TYPE_PAIR:
    ret = machine->T_pair;
    break;
  case LIPS_TYPE_FUNCTION:
  case LIPS_TYPE_C_FUNCTION:
    ret = machine->T_function;
    break;
  case LIPS_TYPE_MACRO:
  case LIPS_TYPE_C_MACRO:
    ret = machine->T_macro;
    break;
  case LIPS_TYPE_USER:
    // TODO: user types
    LIPS_THROW_ERROR(machine, "User types are not supported currently");
    break;
  }
  return ret;
}

LIPS_DECLARE_FUNCTION(throw)
{
  (void)udata;
  assert(numargs == 1);
  machine->throwvalue = args[0];
  Lips_PrintCell(machine, machine->throwvalue, machine->errbuff, sizeof(machine->errbuff));
  return NULL;
}

LIPS_DECLARE_FUNCTION(call)
{
  (void)udata;
  assert(numargs == 2);
  TYPE_CHECK(machine, LIPS_TYPE_SYMBOL, args[0]);
  TYPE_CHECK(machine, LIPS_TYPE_PAIR, args[1]);
  return Lips_Invoke(machine, args[0], args[1]);
}

LIPS_DECLARE_FUNCTION(format)
{
  (void)udata;
  TYPE_CHECK(machine, LIPS_TYPE_STRING, args[0]);
  const char* fmt = GET_STR_PTR(args[0]);
  char initial_buff[1024];
  uint32_t len = strlen(fmt);
  // TODO: grow buffer if we're out of initial_buff
  char* buff = initial_buff;
  Lips_Cell* commands = args+1;
  for (uint32_t i = 0; i < len; i++) {
    if (fmt[i] == '%') {
      i++;
      if (fmt[i] == '\0') {
        LIPS_THROW_ERROR(machine, "format string ends in middle of format specifier");
      }
      if (numargs == 0) {
        LIPS_THROW_ERROR(machine, "not enough arguments for format string");
      }
      Lips_Cell command = *commands;
      switch (fmt[i]) {
      case 's':
        TYPE_CHECK(machine, LIPS_TYPE_STRING, command);
        buff += sprintf(buff, "%s", GET_STR_PTR(command));
        break;
      case 'd':
        if (Lips_IsInteger(command)) {
          buff += sprintf(buff, "%ld", GET_INTEGER(command));
        } else {
          TYPE_CHECK(machine, LIPS_TYPE_REAL, command);
          buff += sprintf(buff, "%f", GET_REAL(command));
        }
        break;
      case 'S':
        // We don't even check if we have overflow
        // hope we will fix it soon
        buff += Lips_PrintCell(machine, command, buff, sizeof(initial_buff) - (buff - initial_buff));
        break;
      default:
        LIPS_THROW_ERROR(machine, "invalid format operation %%%c", fmt[i]);
      }
      commands++;
      numargs--;
    } else {
      // write symbol to the buffer
      *buff = fmt[i];
      buff++;
    }
  }
  return Lips_NewStringN(machine, initial_buff, sizeof(initial_buff) - (buff - initial_buff));
}

LIPS_DECLARE_FUNCTION(concat)
{
  (void)udata;
  // compute size of resulting string
  uint32_t total_size = 0;
  for (uint32_t i = 0; i < numargs; i++) {
    TYPE_CHECK(machine, LIPS_TYPE_STRING, args[i]);
    total_size += GET_STR(args[i])->length;
  }
  // create a string
  Lips_Cell cell = NewCell(machine);
  cell->type = LIPS_TYPE_STRING;
  GET_STR(cell) = StringCreateEmpty(machine, total_size);
  GET_STR(cell)->refs++;
  char* curr = GET_STR_PTR(cell);
  // write argument contents to resulting string
  strcpy(curr, GET_STR_PTR(args[0]));
  curr += GET_STR(args[0])->length;
  for (uint32_t i = 1; i < numargs; i++) {
    strcat(curr, GET_STR_PTR(args[i]));
    curr += GET_STR(args[i])->length;
  }
  GET_STR(cell)->hash = ComputeHash(GET_STR_PTR(cell));
  return cell;
}

LIPS_DECLARE_FUNCTION(slurp)
{
  (void)numargs;
  (void)udata;
  TYPE_CHECK(machine, LIPS_TYPE_STRING, args[0]);
  const char* filename = GET_STR_PTR(args[0]);
  size_t bytes;
  void* file = machine->open_file(filename, &bytes);
  if (file == NULL) {
    LIPS_THROW_ERROR(machine, "failed to open file '%s'", filename);
  }
  // allocate a string
  Lips_Cell cell = NewCell(machine);
  cell->type = LIPS_TYPE_STRING;
  GET_STR(cell) = StringCreateEmpty(machine, bytes);
  GET_STR(cell)->refs++;
  // read file contents to the string
  char* buffer = GET_STR_PTR(cell);
  machine->read_file(file, buffer, bytes);
  buffer[bytes] = '\0';
  GET_STR(cell)->hash = ComputeHash(GET_STR_PTR(cell));
  machine->close_file(file);
  return cell;
}

LIPS_DECLARE_FUNCTION(load)
{
  (void)numargs;
  (void)udata;
  TYPE_CHECK(machine, LIPS_TYPE_STRING, args[0]);
  const char* filename = GET_STR_PTR(args[0]);
  size_t bytes;
  void* file = machine->open_file(filename, &bytes);
  if (file == NULL) {
    LIPS_THROW_ERROR(machine, "failed to open file '%s'", filename);
  }
  char* buff = machine->alloc(bytes + 10);
  int offset = sprintf(buff, "( ");
  machine->read_file(file, buff+offset, bytes);
  machine->close_file(file);
  buff[bytes + offset] = ')';
  buff[bytes + offset+1] = '\0';
  Parser parser;
  ParserInit(&parser, buff, bytes + offset + 1);
  Lips_Cell ast = GenerateAST(machine, &parser);
  machine->dealloc(buff, bytes);
  Lips_Cell temp = Lips_ListPushBack(machine, machine->S_filename, args[0]);
  Lips_Cell ret = M_progn(machine, ast, NULL);
  GET_TAIL(temp) = NULL; // this equals to Lips_ListPop(machine, machine->S_filename);
  return ret;
}

LIPS_DECLARE_MACRO(lambda)
{
  TYPE_CHECK(machine, LIPS_TYPE_PAIR, GET_HEAD(args));
  (void)udata;
  uint32_t len;
  Lips_Cell last = Lips_ListLastElement(machine, GET_HEAD(args), &len);
  if (len > 127) {
    LIPS_THROW_ERROR(machine,
                     "Too many arguments(%u), in Lips language callables have up to 127 named arguments", len);
  }
  if (last && Lips_IsSymbol(last) && strcmp(GET_STR_PTR(last), "...") == 0) {
    len--;
    len |= LIPS_NUM_ARGS_VAR;
  }
  Lips_Cell lambda = Lips_NewFunction(machine, GET_HEAD(args), GET_TAIL(args), len);
  return lambda;
}

LIPS_DECLARE_MACRO(macro)
{
  TYPE_CHECK(machine, LIPS_TYPE_PAIR, GET_HEAD(args));
  (void)udata;
  uint32_t len;
  Lips_Cell last = Lips_ListLastElement(machine, GET_HEAD(args), &len);
  if (len > 127) {
    LIPS_THROW_ERROR(machine,
                    "Too many arguments(%u), in Lips language callables have up to 127 named arguments", len);
  }
  if (last && Lips_IsSymbol(last) && strcmp(GET_STR_PTR(last), "...") == 0) {
    len--;
    len |= LIPS_NUM_ARGS_VAR;
  }
  Lips_Cell macro = Lips_NewMacro(machine, GET_HEAD(args), GET_TAIL(args), len);
  return macro;
}

LIPS_DECLARE_MACRO(define)
{
  (void)udata;
  // FIXME: avoid recursion
  Lips_Cell value = Lips_Eval(machine, GET_HEAD(GET_TAIL(args)));
  return Lips_DefineCell(machine, GET_HEAD(args), value);
}

LIPS_DECLARE_MACRO(quote)
{
  (void)udata;
  (void)machine;
  // just return argument
  return GET_HEAD(args);
}

LIPS_DECLARE_MACRO(progn)
{
  (void)udata;
  EvalState* state = CURRENT_EVAL_STATE(machine);
  ES_SET_STAGE(state, ES_STAGE_EXECUTING_CODE);
  ES_CODE(state) = args;
  ES_CATCH_PARENT(state) = STACK_INVALID_POS;
  return NULL;
}

LIPS_DECLARE_MACRO(if)
{
  (void)udata;
  Lips_Cell condition = Lips_Eval(machine, GET_HEAD(args));
  if (F_nilp(machine, 1, &condition, NULL) == machine->S_nil) {
    return Lips_Eval(machine, GET_HEAD(GET_TAIL(args)));
  }
  return M_progn(machine, GET_TAIL(GET_TAIL(args)), NULL);
}

LIPS_DECLARE_MACRO(when)
{
  (void)udata;
  Lips_Cell condition = Lips_Eval(machine, GET_HEAD(args));
  if (F_nilp(machine, 1, &condition, NULL) == machine->S_nil) {
    return M_progn(machine, GET_TAIL(args), NULL);
  }
  return machine->S_nil;
}

LIPS_DECLARE_MACRO(catch)
{
  (void)udata;
  Lips_Cell ret = M_progn(machine, args, udata);
  PushCatch(machine);
  return ret;
}

LIPS_DECLARE_MACRO(intern)
{
  (void)udata;
  TYPE_CHECK(machine, LIPS_TYPE_STRING|LIPS_TYPE_SYMBOL, GET_HEAD(args));
  Lips_Cell cell = Lips_InternCell(machine, GET_HEAD(args));
  return cell;
}
