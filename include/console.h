#ifndef CONSOLE_H
#define CONSOLE_H
#include <Windows.h>

#define BAR_WIDTH 50

#define COLOR_RESET "\x1b[0m"
#define COLOR_CYAN "\x1b[36m"
#define COLOR_YELLOW "\x1b[33m"
#define COLOR_MAGENTA "\x1b[35m"
#define COLOR_BLUE "\x1b[34m"
#define COLOR_ORANGE "\x1b[38;5;214m"
#define COLOR_RED "\x1b[31m"
#define COLOR_GREEN "\x1b[32m"

extern CRITICAL_SECTION console_lock;

void set_initialize_status(const char* func_name, const char* message);
void set_modified_status(const char* message);
void set_trim_status(const char* message);
void print_fatal_error(const char* msg);

void position_cursor(SHORT row, SHORT col);
void print_bar(int thread_id, double fraction, ULONG64 passthrough, ULONG64 total_passthroughs, ULONG64 faults);

#endif //CONSOLE_H
