#include "options.h"
#include "heap.h"
#include "logging.h"
#include "debugger.h"
#include "handlers.h"

uint64_t BREAK_AT = 0;
uint64_t BREAK_AFTER = 0;
int BREAK_MAIN = 0;
int BREAK_SIGSEGV = 0;

char *OPT_GDB_PATH = "/usr/bin/gdb";


// see if it's time to pause
// XXX: this function should be in debugger.c
void check_should_break(HeaptraceContext *ctx, uint64_t oid, uint64_t break_at, int prepend_newline) {
    // try reading from params second
    int should_break = (break_at == oid);

    // now actually break if necessary
    if (should_break) {
        debug2("\n");
        debug("decided to break @ check_should_break(oid=%d, break_at=%d, prepend_newline=%d)\n", oid, break_at, prepend_newline);
        debug("\tBREAK_AT=%d, BREAK_AFTER=%d, BREAK_MAIN=%d, BREAK_SIGSEGV=%d\n", BREAK_AT, BREAK_AFTER, BREAK_MAIN, BREAK_SIGSEGV);
        debug("\tBETWEEN_PRE_AND_POST=%d\n", ctx->between_pre_and_post);

        if (prepend_newline) log("\n"); // XXX: this hack is because malloc/realloc need a newline before paused msg
        log(COLOR_ERROR "    [   PROCESS PAUSED   ]\n");
        log(COLOR_ERROR "    |   * attaching GDB via: " COLOR_ERROR_BOLD "%s -p %d\n" COLOR_RESET, OPT_GDB_PATH, ctx->pid);
        if (prepend_newline) log("    "); // XXX/HACK: see above

        // launch gdb
        _remove_breakpoints(ctx, BREAKPOINT_OPTS_ALL); // TODO/XXX: use end_debugger
        ptrace(PTRACE_DETACH, ctx->pid, NULL, SIGSTOP);

        char buf[10+1];
        snprintf(buf, 10, "%d", ctx->pid);
        char *args[] = {OPT_GDB_PATH, "-p", buf, NULL};
        if (execv(args[0], args) == -1) {
            ASSERT(0, "failed to execute debugger %s: %s (errno %d)", args[0], strerror(errno), errno);
        }
        //raise(SIGSTOP);
    }
}


// returns the current operation ID
uint64_t get_oid(HeaptraceContext *ctx) {
    uint64_t oid = ctx->malloc_count + ctx->calloc_count + ctx->free_count + ctx->realloc_count + ctx->reallocarray_count;
    ASSERT(oid < (uint64_t)0xFFFFFFFFFFFFFFF0LLU, "ran out of oids"); // avoid overflows
    return oid;
}


void show_stats(HeaptraceContext *ctx) {
    uint64_t unfreed_sum = 0;

    /* // TODO: convert to BST code
    Chunk cur_chunk;
    int _prefix = 0; // hack for getting newlines right
    for (int i = 0; i < MAX_CHUNKS; i++) {
        cur_chunk = chunk_meta[i];
        if (cur_chunk.state == STATE_MALLOC) {
            if (OPT_VERBOSE) {
                if (!_prefix) {
                    _prefix = 1;
                    log("\n");
                }
                log(COLOR_ERROR "* chunk malloc'd in operation " SYM COLOR_ERROR " was never freed\n", cur_chunk.ops[STATE_MALLOC]);
            }
            unfreed_sum += CHUNK_SIZE(cur_chunk.size);
        }
    }
    */

    if (unfreed_sum && OPT_VERBOSE) log(COLOR_LOG "------\n");
    log(COLOR_LOG "Statistics:\n");
    log("... total mallocs: " CNT "\n", ctx->malloc_count);
    log("... total callocs: " CNT "\n", ctx->calloc_count);
    log("... total frees: " CNT "\n", ctx->free_count);
    log("... total reallocs: " CNT "\n", ctx->realloc_count);
    log("... total reallocarrays: " CNT "\n" COLOR_RESET, ctx->reallocarray_count);

    if (unfreed_sum) {
        log(COLOR_ERROR "... total bytes lost: " SZ_ERR "\n", SZ_ARG(unfreed_sum));
    }

    log("%s", COLOR_RESET);
}
