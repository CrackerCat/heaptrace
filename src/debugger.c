#define _GNU_SOURCE
#include <sys/syscall.h>
#include <inttypes.h>
#include <string.h>
#include <sys/personality.h>
#include <linux/auxvec.h>
#include <signal.h>
#include <string.h>

#include "debugger.h"
#include "handlers.h"
#include "heap.h"
#include "logging.h"
#include "breakpoint.h"
#include "options.h"
#include "funcid.h"
#include "proc.h"

static int in_breakpoint = 0;

int OPT_FOLLOW_FORK = 0;

void _check_breakpoints(HeaptraceContext *ctx) {
    struct user_regs_struct regs;
    ptrace(PTRACE_GETREGS, ctx->pid, NULL, &regs);
    uint64_t reg_rip = (uint64_t)regs.rip - 1;

    int _was_bp = 0;

    for (int i = 0; i < BREAKPOINTS_COUNT; i++) {
        Breakpoint *bp = ctx->breakpoints[i];
        if (bp) {
            if (bp->addr == reg_rip) { // hit the breakpoint
                _was_bp = 1;
                //printf("Hit breakpoint %s (0x%x)\n", bp->name, reg_rip);
                ptrace(PTRACE_POKEDATA, ctx->pid, reg_rip, (uint64_t)bp->orig_data);

                // move rip back by one
                regs.rip = reg_rip; // NOTE: this is actually $rip-1
                ptrace(PTRACE_SETREGS, ctx->pid, NULL, &regs);
                
                if (!in_breakpoint && !bp->_is_inside && bp->pre_handler) {
                    int nargs = bp->pre_handler_nargs;
                    if (nargs == 0) {
                        ((void(*)(HeaptraceContext *))bp->pre_handler)(ctx);
                    } else if (nargs == 1) {
                        ((void(*)(HeaptraceContext *, uint64_t))bp->pre_handler)(ctx, regs.rdi);
                    } else if (nargs == 2) {
                        ((void(*)(HeaptraceContext *, uint64_t, uint64_t))bp->pre_handler)(ctx, regs.rdi, regs.rsi);
                    } else if (nargs == 3) {
                        ((void(*)(HeaptraceContext *, uint64_t, uint64_t, uint64_t))bp->pre_handler)(ctx, regs.rdi, regs.rsi, regs.rdx);
                    } else {
                        ASSERT(0, "nargs is only supported up to 3 args; ignoring bp pre_handler. Please report this!");
                    }
                }
                
                // reset breakpoint and continue
                ptrace(PTRACE_SINGLESTEP, ctx->pid, NULL, NULL);
                wait(NULL);

                if (!bp->_is_inside) {
                    if (!bp->_bp) { // this is a regular breakpoint
                        if (!in_breakpoint) {
                            in_breakpoint = 1;
                            bp->_is_inside = 1;

                            if (bp->post_handler) {
                                uint64_t val_at_reg_rsp = (uint64_t)ptrace(PTRACE_PEEKDATA, ctx->pid, regs.rsp, NULL);
                                if (OPT_VERBOSE) {
                                    ProcMapsEntry *pme = pme_find_addr(ctx->pme_head, val_at_reg_rsp);
                                    if (pme) {
                                        ctx->ret_ptr_section_type = pme->pet;
                                    }
                                }

                                // install return value catcher breakpoint
                                Breakpoint *bp2 = (Breakpoint *)calloc(1, sizeof(struct Breakpoint));
                                bp2->name = "_tmp";
                                bp2->addr = val_at_reg_rsp;
                                bp2->pre_handler = 0;
                                bp2->post_handler = 0;
                                install_breakpoint(ctx, bp2);
                                bp2->_bp = bp;
                            } else {
                                // we don't need a return catcher, so no way to track being inside func
                                in_breakpoint = 0;
                            }
                        }

                        // reinstall original breakpoint
                        ptrace(PTRACE_POKEDATA, ctx->pid, reg_rip, ((uint64_t)bp->orig_data & ~((uint64_t)0xff)) | ((uint64_t)'\xcc' & (uint64_t)0xff));
                    } else { // this is a return value catcher breakpoint
                        Breakpoint *orig_bp = bp->_bp;
                        if (orig_bp) {
                            if (orig_bp->post_handler) {
                                ((void(*)(HeaptraceContext *, uint64_t))orig_bp->post_handler)(ctx, regs.rax);
                            }
                            _remove_breakpoint(ctx, bp, 1);
                            orig_bp->_is_inside = 0;
                        } else {
                            // we never installed a return value catcher breakpoint!
                            bp->_is_inside = 0;
                        }
                        in_breakpoint = 0;
                    }
                } else {
                    // reinstall original breakpoint
                    ptrace(PTRACE_POKEDATA, ctx->pid, reg_rip, ((uint64_t)bp->orig_data & ~((uint64_t)0xff)) | ((uint64_t)'\xcc' & (uint64_t)0xff));
                }

                //printf("BREAKPOINT peeked 0x%x at breakpoint 0x%x\n", ptrace(PTRACE_PEEKDATA, pid, reg_rip, 0L), reg_rip);

            }
        }
    }
}


static uint64_t _calc_offset(HeaptraceContext *ctx, SymbolEntry *se) {
    ProcMapsEntry *bin_pme = pme_walk(ctx->pme_head, PROCELF_TYPE_BINARY);
    ASSERT(bin_pme, "Target binary is missing from process mappings (!bin_pme in _calc_offset). Please report this!");

    if (se->type == SE_TYPE_STATIC) {
        return bin_pme->base + se->offset;
    } else if (se->type == SE_TYPE_DYNAMIC || se->type == SE_TYPE_DYNAMIC_PLT) {
        ProcMapsEntry *libc_pme = pme_walk(ctx->pme_head, PROCELF_TYPE_LIBC);
        if (!libc_pme) return 0;

        uint64_t got_ptr = bin_pme->base + se->offset;
        uint64_t got_val = ptrace(PTRACE_PEEKDATA, ctx->pid, got_ptr, NULL);
        debug(". peeked val=%p at GOT ptr=%p for %s (type=%d)\n", got_val, got_ptr, se->name, se->type);

        // check if this is in the PLT or if it's resolved to libc
        if (se->type == SE_TYPE_DYNAMIC_PLT && (got_val >= bin_pme->base && got_val < bin_pme->end)) {
            // I had issues where GOT contained the address + 0x6, see  https://github.com/Arinerron/heaptrace/issues/22#issuecomment-937420315
            // see https://www.intezer.com/blog/malware-analysis/executable-linkable-format-101-part-4-dynamic-linking/ for explanation why it's like that
            got_val -= (uint64_t)0x6;
        }

        return got_val;
    }

    return 0;
}


// attempts to identify functions in stripped ELFs (bin_pme->base only, not libc)
void evaluate_funcid(HeaptraceContext *ctx, Breakpoint **bps) {
    ProcMapsEntry *bin_pme = pme_walk(ctx->pme_head, PROCELF_TYPE_BINARY);
    ASSERT(bin_pme, "Target binary does not exist in process mappings (!bin_pme in evaluate_funcid). Please report this!");

    int _printed_debug = 0;
    FILE *f = fopen(ctx->target_path, "r");
    FunctionSignature *sigs = find_function_signatures(f);
    for (int i = 0; i < 5; i++) {
        FunctionSignature *sig = &sigs[i];
        //printf("(2) -> %s (%p) - %x (%p)\n", sig->name, sig, sig->offset, sig->offset);
        if (sig->offset) {
            if (!_printed_debug) {
                _printed_debug = 1;
                info("Attempting to identify function signatures in " COLOR_LOG_BOLD "%s" COLOR_LOG " (stripped)...\n", ctx->target_path);
            }
            uint64_t ptr = bin_pme->base + sig->offset;
            info(COLOR_LOG "* found " COLOR_LOG_BOLD "%s" COLOR_LOG " at " PTR ".\n" COLOR_RESET, sig->name, PTR_ARG(sig->offset));
            int j = 0;
            while (1) {
                Breakpoint *bp = bps[j++];
                if (!bp) break;
                if (!strcmp(sig->name, bp->name)) {
                    bp->addr = ptr;
                }
            }
        }
    }

    if (_printed_debug) info("\n");
    if (sigs) free(sigs);
    fclose(f);
}


void end_debugger(HeaptraceContext *ctx, int should_detach) {
    int status = ctx->status;
    int _was_sigsegv = 0;
    log(COLOR_LOG "\n================================= " COLOR_LOG_BOLD "END HEAPTRACE" COLOR_LOG " ================================\n" COLOR_RESET);
    int code = (status >> 8) & 0xffff;

    if (ctx->status16 == PTRACE_EVENT_EXEC) {
        log(COLOR_ERROR "Detaching heaptrace because process made a call to exec()");

        // we keep this logic in case someone makes one of the free/malloc hooks call /bin/sh :)
        if (ctx->between_pre_and_post) log(" while executing " COLOR_ERROR_BOLD "%s" COLOR_ERROR " (" SYM COLOR_ERROR ")", ctx->between_pre_and_post, get_oid(ctx));
        log("." COLOR_RESET " ", code);
    } else if ((status == STATUS_SIGSEGV) || status == 0x67f || (WIFSIGNALED(status) && !WIFEXITED(status))) { // some other abnormal code
        // XXX: this code checks if the whole `status` int == smth. We prob only want ctx->status16
        log(COLOR_ERROR "Process exited with signal " COLOR_ERROR_BOLD "SIG%s" COLOR_ERROR " (" COLOR_ERROR_BOLD "%d" COLOR_ERROR ")", sigabbrev_np(code), code);
        if (ctx->between_pre_and_post) log(" while executing " COLOR_ERROR_BOLD "%s" COLOR_ERROR " (" SYM COLOR_ERROR ")", ctx->between_pre_and_post, get_oid(ctx));
        log("." COLOR_RESET " ", code);
        _was_sigsegv = 1;
    }

    if (WCOREDUMP(status)) {
        log(COLOR_ERROR "Core dumped. " COLOR_LOG);
    }

    log("\n");
    show_stats(ctx);

    if (_was_sigsegv) check_should_break(ctx, 1, BREAK_SIGSEGV, 0);
    if (should_detach) ptrace(PTRACE_DETACH, ctx->pid, NULL, SIGCONT);
    free_ctx(ctx);
    exit(0);
}


char *get_libc_version(char *libc_path) {
    FILE *f = fopen(libc_path, "r");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *string = malloc(fsize + 1);
    fread(string, 1, fsize, f);
    fclose(f);
    string[fsize] = 0;

    char *_prefix = " version ";
    char *_version = memmem(string, fsize, _prefix, strlen(_prefix));
    if (!_version) return 0;
    _version += strlen(_prefix);
    char *_period = strstr(_version, ".\n");
    if (!_period) return 0;
    *_period = '\x00';

    char *version = strdup(_version);
    free(string);
    return version;
}


// this is triggered by a breakpoint. The address to _start (entry) is stored 
// in auxv and fetched on the first run.
void _pre_entry(HeaptraceContext *ctx) {
    ctx->should_map_syms = 1;
    check_should_break(ctx, 1, BREAK_MAIN, 0);
}


void start_debugger(HeaptraceContext *ctx) {
    SymbolEntry *se_malloc = (SymbolEntry *)calloc(1, sizeof(SymbolEntry));
    se_malloc->name = "malloc";
    Breakpoint *bp_malloc = (Breakpoint *)calloc(1, sizeof(struct Breakpoint));
    bp_malloc->name = "malloc";
    bp_malloc->pre_handler = pre_malloc;
    bp_malloc->pre_handler_nargs = 1;
    bp_malloc->post_handler = post_malloc;

    SymbolEntry *se_calloc = (SymbolEntry *)calloc(1, sizeof(SymbolEntry));
    se_calloc->name = "calloc";
    Breakpoint *bp_calloc = (Breakpoint *)calloc(1, sizeof(struct Breakpoint));
    bp_calloc->name = "calloc";
    bp_calloc->pre_handler = pre_calloc;
    bp_calloc->pre_handler_nargs = 2;
    bp_calloc->post_handler = post_calloc;

    SymbolEntry *se_free = (SymbolEntry *)calloc(1, sizeof(SymbolEntry));
    se_free->name = "free";
    Breakpoint *bp_free = (Breakpoint *)calloc(1, sizeof(struct Breakpoint));
    bp_free->name = "free";
    bp_free->pre_handler = pre_free;
    bp_free->pre_handler_nargs = 1;
    bp_free->post_handler = post_free;

    SymbolEntry *se_realloc = (SymbolEntry *)calloc(1, sizeof(SymbolEntry));
    se_realloc->name = "realloc";
    Breakpoint *bp_realloc = (Breakpoint *)calloc(1, sizeof(struct Breakpoint));
    bp_realloc->name = "realloc";
    bp_realloc->pre_handler = pre_realloc;
    bp_realloc->pre_handler_nargs = 2;
    bp_realloc->post_handler = post_realloc;

    SymbolEntry *se_reallocarray = (SymbolEntry *)calloc(1, sizeof(SymbolEntry));
    se_reallocarray->name = "reallocarray";
    Breakpoint *bp_reallocarray = (Breakpoint *)calloc(1, sizeof(struct Breakpoint));
    bp_reallocarray->name = "reallocarray";
    bp_reallocarray->pre_handler = pre_reallocarray;
    bp_reallocarray->pre_handler_nargs = 3;
    bp_reallocarray->post_handler = post_reallocarray;

    SymbolEntry *ses[] = {se_malloc, se_calloc, se_free, se_realloc, se_reallocarray, NULL};
    lookup_symbols(ctx, ses);

    if (ctx->target_interp_name) {
        //debug("Using interpreter \"%s\".\n", interp_name);
    }

    // ptrace section
    
    log(COLOR_LOG "================================ " COLOR_LOG_BOLD "BEGIN HEAPTRACE" COLOR_LOG " ===============================\n" COLOR_RESET);
    
    
    int show_banner = 0;
    ctx->target_is_dynamic = (se_malloc->type == SE_TYPE_DYNAMIC || se_calloc->type == SE_TYPE_DYNAMIC || se_free->type == SE_TYPE_DYNAMIC || se_realloc->type == SE_TYPE_DYNAMIC || se_reallocarray->type == SE_TYPE_DYNAMIC) || (se_malloc->type == SE_TYPE_DYNAMIC_PLT || se_calloc->type == SE_TYPE_DYNAMIC_PLT || se_free->type == SE_TYPE_DYNAMIC_PLT || se_realloc->type == SE_TYPE_DYNAMIC_PLT || se_reallocarray->type == SE_TYPE_DYNAMIC_PLT); // XXX: find a better way to do this LOL
    ctx->target_is_stripped = (se_malloc->type == SE_TYPE_UNRESOLVED && se_calloc->type == SE_TYPE_UNRESOLVED && se_free->type == SE_TYPE_UNRESOLVED && se_realloc->type == SE_TYPE_UNRESOLVED && se_reallocarray->type == SE_TYPE_UNRESOLVED);

    if (ctx->target_is_stripped && !strlen(symbol_defs_str)) {
        warn("Binary appears to be stripped or does not use the glibc heap; heaptrace was not able to resolve any symbols. Please specify symbols via the -s/--symbols argument. e.g.:\n\n      heaptrace --symbols 'malloc=libc+0x100,free=libc+0x200,realloc=bin+123' ./binary\n\nSee the help guide at https://github.com/Arinerron/heaptrace/wiki/Dealing-with-a-Stripped-Binary\n");
        show_banner = 1;
    }

    int look_for_brk = ctx->target_is_dynamic;

    assert(!ctx->target_is_dynamic || (ctx->target_is_dynamic && ctx->target_interp_name));
    if (ctx->target_interp_name) {
        //get_glibc_path(interp_name, chargv[0]);
    }

    if (show_banner) {
        log(COLOR_LOG "================================================================================\n" COLOR_RESET);
    }
    log("\n");

    int child = fork();
    if (!child) {
        // disable ASLR
        if (personality(ADDR_NO_RANDOMIZE) == -1) {
            warn("failed to disable aslr for child\n");
        }

        ptrace(PTRACE_TRACEME, 0, NULL, NULL);
        extern char **environ;
        if (execvpe(ctx->target_path, ctx->target_argv, environ) == -1) {
            fatal("failed to start target via execvp(\"%s\", ...): (%d) %s\n", ctx->target_path, errno, strerror(errno)); // XXX: not thread safe
            exit(1);
        }
    } else {
        ProcMapsEntry *pme_head;

        int status;
        //ctx->should_map_syms = !ctx->target_is_dynamic;
        ctx->should_map_syms = 0;
        int first_signal = 1; // XXX: this is confusing. refactor later.
        ctx->pid = child;
        debug("Started target process in PID %d\n", child);

        while(waitpid(child, &status, 0)) {
            // update ctx
            ctx->status = status;
            ctx->status16 = status >> 16;

            if (OPT_FOLLOW_FORK) {
                ptrace(PTRACE_SETOPTIONS, child, NULL, PTRACE_O_TRACEFORK | PTRACE_O_TRACEVFORK | PTRACE_O_TRACECLONE | PTRACE_O_TRACEEXEC);
            } else
                ptrace(PTRACE_SETOPTIONS, child, NULL, PTRACE_O_TRACEEXEC);

            struct user_regs_struct regs;
            ptrace(PTRACE_GETREGS, child, 0, &regs);

            if (first_signal) {
                first_signal = 0;
                ctx->target_at_entry = get_auxv_entry(child);

                ASSERT(ctx->target_at_entry, "unable to locate at_entry auxiliary vector. Please report this.");
                // temporary solution is to uncomment the should_map_syms = !ctx->target_is_dynamic
                // see blame for this commit, or see commit after commit 2394278.
                
                Breakpoint *bp_entry = (Breakpoint *)malloc(sizeof(struct Breakpoint));
                bp_entry->name = "_entry";
                bp_entry->addr = ctx->target_at_entry;
                bp_entry->pre_handler = _pre_entry;
                bp_entry->pre_handler_nargs = 0;
                bp_entry->post_handler = 0;
                install_breakpoint(ctx, bp_entry);
            }

            if (WIFEXITED(status) || WIFSIGNALED(status) || status == STATUS_SIGSEGV || status == 0x67f) {
                end_debugger(ctx, 0);
            } else if (status == 0x57f) { /* status SIGTRAP */ 
                _check_breakpoints(ctx);
            } else if (status >> 16 == PTRACE_EVENT_FORK || status >> 16 == PTRACE_EVENT_VFORK || status >> 16 == PTRACE_EVENT_CLONE) { /* fork, vfork, or clone */
                long newpid;
                ptrace(PTRACE_GETEVENTMSG, child, NULL, &newpid);
                //ptrace(PTRACE_DETACH, child, NULL, SIGSTOP);
                //_remove_breakpoints(child, 0);
                //ptrace(PTRACE_CONT, child, NULL, NULL);

                if (OPT_FOLLOW_FORK) {
                    log_heap(COLOR_RESET COLOR_RESET_BOLD "Detected fork in process (%d->%d). Following fork...\n\n", child, newpid);
                    ptrace(PTRACE_DETACH, child, NULL, SIGCONT);
                    child = newpid;
                    ctx->pid = newpid;
                    ptrace(PTRACE_SETOPTIONS, newpid, NULL, PTRACE_O_TRACEFORK | PTRACE_O_TRACEVFORK | PTRACE_O_TRACECLONE);
                    //ptrace(PTRACE_CONT, newpid, 0L, 0L);
                } else {
                    debug("detected process fork, use --follow-fork to folow it. Parent PID is %d, child PID is %d.\n", child, newpid);
                    ptrace(PTRACE_DETACH, newpid, NULL, SIGSTOP);
                    // XXX: There seems to be some kind of race condition here. The parent process randomly continues like a tenth of the time.
                    // As a stopgap solution I put the if(OPT_FOLLOW_FORK) around the ptrace(PTRACE_SETOPTIONS, ...) right after the while() loop start.
                    // But ideally we print out when there's a fork as shown above to that the user knows to use -F
                    //ptrace(PTRACE_CONT, child, 0L, 0L);
                }
            } else if (status >> 16 == PTRACE_EVENT_EXEC) {
                debug("Detected exec() call, detaching...\n");
                end_debugger(ctx, 1);
            } else {
                debug("warning: hit unknown status code %d\n", status);
            }

            if (ctx->should_map_syms) {
                ctx->should_map_syms = 0;

                // parse /proc/pid/maps
                pme_head = build_pme_list(child);
                ctx->pme_head = pme_head;
                ProcMapsEntry *bin_pme = pme_walk(pme_head, PROCELF_TYPE_BINARY);
                ProcMapsEntry *libc_pme = pme_walk(pme_head, PROCELF_TYPE_LIBC);
                
                // quick debug info about addresses/paths we found
                ASSERT(bin_pme, "Failed to find target binary in process mapping (!bin_pme). Please report this!");
                debug("found memory maps... binary (%s): %p-%p", bin_pme->name, bin_pme->base, bin_pme->end);
                if (libc_pme) {
                    char *name = libc_pme->name;
                    ctx->libc_path = name;
                    if (!name) name = "<UNKNOWN>";
                    debug2(", libc (%s): %p-%p", libc_pme->name, libc_pme->base, libc_pme->end);
                }
                debug2("\n");

                // print the type of binary etc
                if (ctx->target_is_dynamic) {
                    verbose(COLOR_RESET_BOLD "Dynamically-linked");
                    if (ctx->target_is_stripped) verbose(", stripped");
                    verbose(" binary")

                    if (libc_pme && libc_pme->name) {
                        char *ptr = get_libc_version(libc_pme->name);
                        char *libc_version = ptr;
                        if (!ptr) libc_version = "???";
                        verbose(" using glibc version %s (%s)\n" COLOR_RESET, libc_version, libc_pme->name);
                        ctx->libc_version = ptr;
                    } else { verbose("\n"); }
                } else {
                    verbose(COLOR_RESET_BOLD "Statically-linked");
                    if (ctx->target_is_stripped) verbose(", stripped");
                    verbose(" binary\n" COLOR_RESET);
                }

                // now that we know the base addresses, calculate offsets
                bp_malloc->addr = _calc_offset(ctx, se_malloc);
                bp_calloc->addr = _calc_offset(ctx, se_calloc);
                bp_free->addr = _calc_offset(ctx, se_free);
                bp_realloc->addr = _calc_offset(ctx, se_realloc);
                bp_reallocarray->addr = _calc_offset(ctx, se_reallocarray);
                
                // prep breakpoint arrays
                Breakpoint *bps[] = {bp_malloc, bp_calloc, bp_free, bp_realloc, bp_reallocarray, NULL};

                // final attempts to get symbol information (funcid + parse --symbol)
                if (ctx->target_is_stripped) evaluate_funcid(ctx, bps);
                evaluate_symbol_defs(ctx, bps);
                verbose("\n");

                // install breakpoints
                int k = 0;
                while (1) {
                    Breakpoint *bp = bps[k++];
                    if (!bp) break;
                    install_breakpoint(ctx, bp);
                }
            }

            ptrace(PTRACE_CONT, child, NULL, NULL);
        }

        warn("while loop exited. Please report this. Status: %d, exit status: %d\n", status, WEXITSTATUS(status));
    }
}
