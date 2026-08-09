#ifndef CLI_H
#define CLI_H

// Copyright 2026 MrSegfault
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
// associated documentation files (the “Software”), to deal in the Software without restriction,
// including without limitation the rights to use, copy, modify, merge, publish, distribute,
// sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all copies or
// substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
// NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

// cli.h: version 0.1.0 2026:07:31

// @description: cli.h is a simple C library that aims to give you the best
// command line parsing experience instead of chaining a bunch of if strcmp and
// also using getopt api which is not cross platform.
// The api works interms of tree words we will be using is command and flags
// commands are root nodes for flags to be childern structures with in the
// command as a command can have multiple flags.

#include <stdbool.h>
#define CLIDEF

// @note: Opaque structures as you as the user of the library do not need to
// know how it is implemented.
typedef struct command_t command_t;
typedef struct cli_t cli_t;
typedef enum cli_kind_t cli_kind_t;

// @note: this is the status that parsing the command line flags return us.
typedef enum {
    CLI_OK,
    CLI_HELP,
    CLI_ERROR,
} cli_status_t;

// @note: this is an array of strings.
typedef struct {
    char **items;
    unsigned int count, capacity;
} strings_t;

// @note: a call back function we can attach to a specific command. This call
// back function will pass the cmd so that we can extract all of the flag values
// we got from it with the get flags.
CLIDEF typedef void (*cli_call_back_t)(command_t *cmd);

// @note: cli_init will intalize the cli will the name and description we pass it
// note that both of them can be null as we set them by default if the user does
// not want to customize it.
CLIDEF cli_t *cli_init(const char *name, const char *description);

// @note: cli_destroy will destroy the cli_t structure we heap allocated.
CLIDEF void cli_destroy(cli_t *cli);

// @note: cli_root  is to get the root command aka the program and pass it to
// cli_command function that will do all the processing.
CLIDEF command_t *cli_root(cli_t *cli);

// @note: cli_command will be used to create a command note that commands can
// have childern meaning you can create sub commands for commands. Hence why we
// have a parent parameter. You then can pass the name of the command which is
// mandatory and desciption which can be null this is more for printing usage.
CLIDEF command_t *cli_command(command_t *parent, const char *name, const char *desc);

// @note: cli_command_alias will allow you to alias a command such that if you
// create a command build you can alias to b
CLIDEF void cli_command_alias(command_t *cmd, const char *alias);

// @note: cli_command_callback will set the callback for that specific command
// so that when that command is passed it will execute the callback function and
// run it.
CLIDEF void cli_command_callback(command_t *cmd, cli_call_back_t callback);

// @note: cli_parse will parse the command line expects argc and argv.
// It can return three things.
// CLI_ERROR: Parsing was not successful
// CLI_HELP:  Parsing was successful but the user either passes -h, --help or
// help on top of the command. which will then show that help usage.
// CLI_OK: Parsing was successful.
CLIDEF cli_status_t cli_parse(cli_t *cli, int argc, char **argv);

// @note: cli_create_flag_* will create a sub flag for a command if for example
// you have a command build and you want --output to be a flag that will output
// yes you set your root command build as the first argument the name to 'output'
// and description as 'output yes' and a value that will trigger this flag
// meaning you create it outside this function and pass it as a parameter. The
// rest of the functions including bool, int, string, and strings does the same job
// except accepting differnt types. So if you want an int value you would use 'cli_create_flag_int'.
CLIDEF void cli_create_flag_bool(command_t *cmd, const char *name, const char *description,
                                 bool *value);
CLIDEF void cli_create_flag_int(command_t *cmd, const char *name, const char *description,
                                int *value);
CLIDEF void cli_create_flag_string(command_t *cmd, const char *name, const char *description,
                                   char **value);
CLIDEF void cli_create_flag_strings(command_t *cmd, const char *name, const char *description,
                                    strings_t *value);

// @note: This defines a positional argument that is the same as
// create_flag_string except it does not have to be in a specific order the you
// can pass it build -o main main.c or build main.c -o main. Meaning it is not
// position independent and you can mark such flag as mandatory or not.
CLIDEF void cli_positional(command_t *cmd, const char *name, const char *description, char **value,
                           bool required);

// @note: cli_alias can mark a flag to be aliased if you have a flag output in cmd
// then you can obviously alias to just be o.
CLIDEF void cli_alias(command_t *cmd, const char *name, char alias);

// @note: cli_rest will allow you to basically do all flags after - to be their
// own strings so if you have build -o main main.c - -lc -lm -lX11 with this for
// example all the linker flags are the rest of the arguments that come after -.
CLIDEF void cli_rest(command_t *cmd, const char *description, strings_t *value);

// @note: cli_usage will dump the help of the cli.
CLIDEF void cli_usage(cli_t *cli);

// @note: cli_command_usage will dump the usage of a specific command
CLIDEF void cli_command_usage(command_t *cmd);

// @note: If no call back is set you can use the cli_active_command to have the
// condition to switch up on all the commands you created.
// if (active == build) {
//  logic
// }
CLIDEF command_t *cli_active_command(cli_t *cli);

// @note: All the functions below here are used to extract the value of a flag
// from a command so that you can use it in your callback as passing the
// values. each type can be extracted the exact same way.
CLIDEF bool cli_get_bool(command_t *cmd, const char *name);
CLIDEF int cli_get_int(command_t *cmd, const char *name);
CLIDEF char *cli_get_string(command_t *cmd, const char *name);
CLIDEF strings_t *cli_get_strings(command_t *cmd, const char *name);
CLIDEF char *cli_get_positional(command_t *cmd, const char *name);

#endif // CLI_H
