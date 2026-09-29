/*********************************Includes***********************************/
#include <dlfcn.h>
#include <pthread.h>
#include <sys/prctl.h>
#include <sys/time.h>
#include <signal.h>
#include <unistd.h>
#include <execinfo.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include "sysdbg.h"

/*********************************Defines************************************/
#define SYSDBG_LIBRARY_VERSION_MAJOR 1
#define SYSDBG_LIBRARY_VERSION_MINOR 0
#define SYSDBG_LIBRARY_VERSION_PATCH 0
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

/************************************Locals**********************************/
static FILE *g_backtrace_fp = NULL;
static char *g_backtrace_program_name = NULL;
static char *g_backtrace_version = NULL;

/************************************General Functions************************************/
static int sysdbg_safe_fread(FILE *fp, void *buff, int buff_size)
{
    int result = 0, read_size = 0;
    while (buff_size > 0)
    {
        read_size = fread(buff, 1, buff_size, fp);
        if (read_size == 0)
            break;

        buff_size -= read_size;
        result += read_size;
        buff = (char *)buff + read_size;
    }
    return result;
}
static long int sysdbg_get_time_msec()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}
static void sysdbg_get_program_name()
{
    char exe[50] = {0}, fname[512] = {0};
    int len = 0;
    snprintf(exe, sizeof(exe) - 1, "/proc/%u/exe", getpid());
    len = readlink(exe, fname, sizeof(fname) - 1);
    if (len < 0 || len >= (int)sizeof(fname))
    {
        printf("readlink: %s", strerror(errno));
        return;
    }
    fname[len] = '\0';
    g_backtrace_program_name = strdup(fname);
}

/************************************Backtrace Call Stack************************************/
static bool sysdbg_exec_shell(const char *cmd, char *buff, int buff_size)
{
    int bytes_read = 0;
    FILE *fp = NULL;

    fp = popen(cmd, "r");
    if (fp == NULL)
        return false;

    memset(buff, 0, buff_size);
    bytes_read = sysdbg_safe_fread(fp, buff, buff_size);
    if (bytes_read == buff_size)
    {
        pclose(fp);
        return false;
    }
    else
        buff[bytes_read] = '\0';

    pclose(fp);
    return true;
}
static int sysdbg_get_backtrace_string(void *trace_address, const char *exe_symbol_name)
{
    char cmd[128] = {0};
    char property[256] = {0};
    char not_care1[128] = {0}, not_care2[128] = {0}, not_care3[128] = {0};
    char library_path[256] = {0};
    char maps_line[1024] = {0};
    void *offset_start, *offset_end;
    int maps_column_num = 0;
    FILE *fd_maps = NULL;
    long long int exe_symbol_offset = 0;
    long long int stackAddr = 0, baseAddr = 0;
    char stackAddrBuf[32] = {0}, baseAddrBuf[32] = {0};
    char file_path[128] = {0};
    char buff[2048] = {0};
    int buff_size = 2048;

    memset(file_path, 0, sizeof(file_path));
    snprintf(file_path, sizeof(file_path), "/proc/%d/maps", getpid());
    fd_maps = fopen(file_path, "r");
    if (fd_maps == NULL)
        return -1;

    while (NULL != fgets(maps_line, sizeof(maps_line), fd_maps))
    {
        maps_column_num = sscanf(maps_line, "%p-%p\t%s\t%s\t%s\t%s\t%s", &offset_start, &offset_end, property, not_care1, not_care2, not_care3, library_path);
        if (maps_column_num == 7 && trace_address >= offset_start && trace_address <= offset_end)
            break;
    }
    fclose(fd_maps);

    memset(stackAddrBuf, 0, sizeof(stackAddrBuf));
    snprintf(stackAddrBuf, sizeof(stackAddrBuf), "%p", trace_address);
    stackAddr = strtoll(stackAddrBuf, NULL, 16);

    memset(baseAddrBuf, 0, sizeof(baseAddrBuf));
    snprintf(baseAddrBuf, sizeof(baseAddrBuf), "%p", offset_start);
    baseAddr = strtoll(baseAddrBuf, NULL, 16);

    memset(cmd, 0, sizeof(cmd));
    snprintf(cmd, sizeof(cmd), "addr2line -C -s -f -p -e %s 0x%llx", g_backtrace_program_name, stackAddr);
    memset(buff, 0, buff_size);
    if (false == sysdbg_exec_shell(cmd, buff, buff_size))
        return -1;

    if (NULL != strstr(buff, "??:0\n") || NULL != strstr(buff, "??:?\n"))
    {
        exe_symbol_offset = stackAddr - baseAddr;
        snprintf(cmd, sizeof(cmd), "addr2line -C -s -f -p -e %s 0x%llx", exe_symbol_name, exe_symbol_offset);
        memset(buff, 0, buff_size);
        if (false == sysdbg_exec_shell(cmd, buff, buff_size))
            return -1;
    }

    fprintf(g_backtrace_fp, "%s\r\n", buff);

    return 0;
}
static void sysdbg_addr2line_parse(void *trace_address)
{
    Dl_info info = {0};

    dladdr(trace_address, &info);
    sysdbg_get_backtrace_string(trace_address, info.dli_fname);
}
static void sysdbg_print_trace(int signal)
{
    void *trace[100] = {NULL};
    char **messages = NULL;
    int trace_size = 0;
    struct sigaction sa = {0};
    char file_name[256] = {0};

    snprintf(file_name, sizeof(file_name), "/data/log/%s-bt-%d-%ld", basename(g_backtrace_program_name), getpid(), sysdbg_get_time_msec());
    while (NULL == (g_backtrace_fp = fopen(file_name, "a+")))
        continue;

    fprintf(g_backtrace_fp, "backtrace_version %s", sysdbg_get_version());
    fprintf(g_backtrace_fp, "\n================>>> catch signal %d:%s <<<================\n", signal, strsignal(signal));
    trace_size = backtrace(trace, ARRAY_SIZE(trace));
    messages = backtrace_symbols(trace, trace_size);
    for (int i = 0; i < trace_size && messages != NULL; ++i)
    {
        fprintf(g_backtrace_fp, "[%02d] %s\r\n", i, messages[i]);
        sysdbg_addr2line_parse(trace[i]);
    }

    fflush(g_backtrace_fp);
    fsync(fileno(g_backtrace_fp));
    fclose(g_backtrace_fp);

    /* free(messages); Don't call free() with possibly corrupted memory. */
    if (messages != NULL)
        free(messages);

    /* Make sure we exit with the right signal at the end. So for instance the core will be dumped if enabled. */
    sigemptyset(&sa.sa_mask);

    /* When the SA_SIGINFO flag is set in sa_flags then sa_sigaction is used. Otherwise, sa_handler is used */
    sa.sa_flags = SA_NODEFER | SA_ONSTACK | SA_RESETHAND;
    sa.sa_handler = SIG_DFL;
    sigaction(signal, &sa, NULL);

    /* Resend signal */
    kill(getpid(), signal);

    return;
}
char *sysdbg_get_version()
{
    char version[512] = {0};
    snprintf(version, sizeof(version) - 1, "%u.%u.%u", SYSDBG_LIBRARY_VERSION_MAJOR, SYSDBG_LIBRARY_VERSION_MINOR, SYSDBG_LIBRARY_VERSION_PATCH);
    g_backtrace_version = strdup(version);
    return g_backtrace_version;
}
void sysdbg_init()
{
    if (!g_backtrace_program_name)
    {
        sysdbg_get_program_name();
        if (!g_backtrace_program_name)
            return;
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGBUS, sysdbg_print_trace);  // Bus error (bad memory access)
    signal(SIGFPE, sysdbg_print_trace);  // Floating point exception
    signal(SIGILL, sysdbg_print_trace);  // Illegal Instruction
    signal(SIGSEGV, sysdbg_print_trace); // Invalid memory reference
    signal(SIGABRT, sysdbg_print_trace); // Abort signal from abort(3)
    signal(SIGQUIT, sysdbg_print_trace); // Quit from keyboard
    signal(SIGIOT, sysdbg_print_trace);  // IOT trap. A synonym for SIGABRT
    signal(SIGSYS, sysdbg_print_trace);  // Bad argument to routine (SVr4)
#if defined(BACKWARD_SYSTEM_DARWIN)
    signal(SIGEMT, sysdbg_save_coredump); // emulation instruction executed
#endif
    signal(SIGTRAP, sysdbg_print_trace); // Trace/breakpoint trap
}
