// command.h

#ifndef COMMAND_H
#define COMMAND_H

typedef void (*CommandFunction)(int argc, char **argv);

typedef struct Command {
    char *name;
    CommandFunction function;
    struct Command *next;
} Command;

// Functions to manage commands
void command_init();
void register_command(const char *name, CommandFunction function);
void unregister_command(const char *name);
CommandFunction find_command(const char *name);

void register_wifi_commands();

#endif // COMMAND_H