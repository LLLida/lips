#include "argp.h"
#include "stdio.h"
#include "string.h"
#include "lips.h"

struct Arguments {
  const char* eval_file;
};

static char doc[] = "Lips programming language REPL";
static char args_doc[] = "";

argp_option options[] = {
  {"load-file", 'l', "FILE", 0, "Evaluate file at the startup"},
  {0}
};

Lips_Machine* machine;

static error_t parse_opt (int key, char *arg, struct argp_state *state);

struct argp argp = { options, parse_opt, args_doc, doc };

int main(int argc, char** argv) {

  Arguments arguments = {};
  argp_parse(&argp, argc, argv, 0, 0, &arguments);

  machine = Lips_DefaultCreateMachine();

  if (arguments.eval_file) {
    char buff[64];
    snprintf(buff, sizeof(buff), "(load \"%s\")", arguments.eval_file);
    Lips_Cell ret = Lips_EvalString(machine, buff, NULL);
    if (ret) {
      printf("loaded file '%s'\n", arguments.eval_file);
    } else {
      printf("failed to file '%s' with error %s\n", arguments.eval_file, Lips_GetError(machine));
    }
  }

  printf("Lips REPL. Type 'quit' to quit.\n");
  char buff[256];
  Lips_Cell eval;
  while (true) {
    printf(">>> ");
    fgets(buff, sizeof(buff), stdin);
    if (strncmp(buff, "quit", strlen(buff)-1) == 0) {
      break;
    }
    eval = Lips_EvalString(machine, buff, NULL);
    if (eval) {
      Lips_PrintCell(machine, eval, buff, sizeof(buff));
    } else {
      sprintf(buff, "Unhandled exception: %s", Lips_GetError(machine));
    }
    printf("%s\n", buff);
  }
  Lips_DestroyMachine(machine);
  return 0;
}

error_t
parse_opt (int key, char *arg, struct argp_state *state)
{
  auto arguments = (Arguments*)state->input;
  switch (key)
    {
    case 'l':
      arguments->eval_file = arg;
      break;

    case ARGP_KEY_ARG:
      if (state->arg_num >= 2)
        /* Too many arguments. */
        argp_usage (state);
      break;

    default:
      return ARGP_ERR_UNKNOWN;
    }
  return 0;
}
