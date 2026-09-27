/*
 * CLI subcommands. Each gets argv starting at the command name.
 */
#ifndef VFW_COMMANDS_H
#define VFW_COMMANDS_H

int cmd_info(int argc, char **argv);
int cmd_avi(int argc, char **argv);
int cmd_convert(int argc, char **argv);
int cmd_list(int argc, char **argv);
int cmd_probe(int argc, char **argv);

#endif /* VFW_COMMANDS_H */
