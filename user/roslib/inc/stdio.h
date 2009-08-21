#ifndef ROS_INC_STDIO_H
#define ROS_INC_STDIO_H

#include <lib.h>
#include <stdarg.h>

#ifndef NULL
#define NULL	((void *) 0)
#endif /* !NULL */

#ifdef DEBUG
#define printd(args...) cprintf(args)
#else
#define printd(fmt, ...) {}
#endif

// lib/stdio.c
void cputchar(int c);
int	 getchar(void);
int	 iscons(int fd);

// lib/printfmt.c

#ifdef __DEPUTY__
void	printfmt(void (*putch)(int, TV(t)), TV(t) putdat, const char *NTS fmt, ...);
void	vprintfmt(void (*putch)(int, TV(t)), TV(t) putdat, const char *NTS fmt, va_list);
#else
void	printfmt(void (*putch)(int, void**), void **putdat, const char *NTS fmt, ...);
void	vprintfmt(void (*putch)(int, void**), void **putdat, const char *NTS fmt, va_list);
#endif

// lib/printf.c
int	cprintf(const char * NTS fmt, ...);
int	vcprintf(const char * NTS fmt, va_list);

// lib/sprintf.c
int	snprintf(char *COUNT(size) str, int size, const char *NTS fmt, ...);
int	vsnprintf(char *COUNT(size) str, int size, const char *NTS fmt, va_list);

// lib/fprintf.c
int	printf(const char *NTS fmt, ...);
int	fprintf(int fd, const char *NTS fmt, ...);
int	vfprintf(int fd, const char *NTS fmt, va_list);

// lib/readline.c
char *NTS readline(const char *NTS prompt);

int	cprintf_async(async_desc_t** desc, const char * NTS fmt, ...);

#endif /* !ROS_INC_STDIO_H */
