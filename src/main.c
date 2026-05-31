#include "../include/riven.h"
#include <signal.h>

/* ===================== REPL ===================== */
static void print_banner(void) {
    printf("\033[1;36m");
    printf("  ____  _                  _                \n");
    printf(" |  _ \\(_)_   _____ _ __  | |    __ _ _ __  __ _ \n");
    printf(" | |_) | \\ \\ / / _ \\ '_ \\ | |   / _` | '_ \\/ _` |\n");
    printf(" |  _ <| |\\ V /  __/ | | || |__| (_| | | | | (_| |\n");
    printf(" |_| \\_\\_| \\_/ \\___|_| |_||_____\\__,_|_| |_|\\__, |\n");
    printf("                                              |___/ \n");
    printf("\033[0m");
    printf("\033[1;33m  Riven Language Interpreter v%s\033[0m\n", RIVEN_VERSION);
    printf("\033[0;90m  Type 'exit' or Ctrl+D to quit. Type 'help' for help.\033[0m\n\n");
}

static void print_help(void) {
    printf("\033[1;32mRiven Language Quick Reference:\033[0m\n");
    printf("  stamp(\"msg\")           - Print output\n");
    printf("  name = grab(\"prompt\")  - Read input\n");
    printf("  firm PI = 3.14         - Constant\n");
    printf("  craft fn(a,b){...}     - Function\n");
    printf("  frame User { boot(){} open login(){} } - Class\n");
    printf("  user = spawn User()    - Create object\n");
    printf("  coll nums = [1,2,3]    - Collection\n");
    printf("  during x < 10 { }      - While loop\n");
    printf("  flow 5 { }             - Fixed loop\n");
    printf("  if x > 0 { } altif ... { } else { }\n");
    printf("  rise x / drop x        - Increment/decrement\n");
    printf("  x+> / x-<              - Inc/dec operators\n");
    printf("  resc { attack(\"err\") } - Error handling\n");
    printf("  ref r = data           - Reference\n");
    printf("  int(x) dnum(x) txt(x)  - Type cast\n");
    printf("  consistof \"file.rvh\"   - Import file\n");
    printf("  ~~ comment             - Single-line comment\n");
    printf("  << block comment >>    - Multi-line comment\n");
    printf("\033[1;32mBuilt-in functions:\033[0m\n");
    printf("  len, upper, lower, split, contains, trim, replace, slice\n");
    printf("  push, pop, sort, reverse, range, join\n");
    printf("  sqrt, pow, abs, floor, ceil, round, sin, cos, rand, randint\n");
    printf("  typeof, is_int, is_dnum, is_txt, is_emp, is_coll\n");
    printf("  read_file, write_file, append_file, time_now, exit\n");
    printf("  keys, vals, has, format\n\n");
}

static void run_repl(void) {
    print_banner();
    Interpreter *interp = interp_new();
    interp_register_natives(interp, interp->global_env);

    char line[MAX_STR_LEN * 4];
    char multi[MAX_STR_LEN * 16]; multi[0] = '\0';
    int in_multi = 0;

    while (1) {
        if (in_multi)
            printf("\033[0;36m...  \033[0m");
        else
            printf("\033[1;35mriven>\033[0m ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) {
            printf("\n\033[0;90mGoodbye!\033[0m\n");
            break;
        }

        /* Trim newline */
        int ll = strlen(line);
        if (ll > 0 && line[ll-1] == '\n') line[--ll] = '\0';

        /* Commands */
        if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0) {
            printf("\033[0;90mGoodbye!\033[0m\n"); break;
        }
        if (strcmp(line, "help") == 0) { print_help(); continue; }
        if (strcmp(line, "clear") == 0) { printf("\033[2J\033[H"); continue; }
        if (strcmp(line, "tokens") == 0) { printf("(debug: lex mode)\n"); continue; }
        if (strlen(line) == 0) continue;

        /* Multi-line detection: if ends with { but no matching } */
        strncat(multi, line, sizeof(multi) - strlen(multi) - 2);
        strncat(multi, "\n", sizeof(multi) - strlen(multi) - 1);

        /* Count braces */
        int opens = 0;
        for (int i = 0; multi[i]; i++) {
            if (multi[i] == '{') opens++;
            else if (multi[i] == '}') opens--;
        }
        if (opens > 0) { in_multi = 1; continue; }
        in_multi = 0;

        /* Execute */
        Lexer *lx = lexer_new(multi);
        lexer_tokenize(lx);
        Parser *ps = parser_new(lx->tokens, lx->token_count);
        ASTNode *prog = parser_parse(ps);

        g_signal = SIGNAL_NONE;
        RivenValue *result = interp_exec(interp, prog, interp->global_env);

        /* Print result if not null/void */
        if (result && result->type != VAL_NULL && g_signal == SIGNAL_NONE) {
            printf("\033[0;32m= \033[0m");
            val_print(result);
            printf("\n");
        }

        if (result) val_release(result);
        ast_free(prog);
        parser_free(ps);
        lexer_free(lx);
        multi[0] = '\0';
        g_signal = SIGNAL_NONE;
    }

    interp_free(interp);
}

/* ===================== FILE RUN ===================== */
static void run_file(const char *path) {
    Interpreter *interp = interp_new();
    interp_run_file(interp, path);
    interp_free(interp);
}

/* ===================== TOKEN DUMP ===================== */
static void dump_tokens(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "Cannot open: %s\n", path); return; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
    char *src = malloc(sz+1); fread(src, 1, sz, f); src[sz] = '\0'; fclose(f);
    Lexer *lx = lexer_new(src); lexer_tokenize(lx);
    lexer_print_tokens(lx);
    lexer_free(lx); free(src);
}

/* ===================== AST DUMP ===================== */
static void dump_ast(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "Cannot open: %s\n", path); return; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
    char *src = malloc(sz+1); fread(src, 1, sz, f); src[sz] = '\0'; fclose(f);
    Lexer *lx = lexer_new(src); lexer_tokenize(lx);
    Parser *ps = parser_new(lx->tokens, lx->token_count);
    ASTNode *prog = parser_parse(ps);
    ast_print(prog, 0);
    ast_free(prog); parser_free(ps); lexer_free(lx); free(src);
}

/* ===================== USAGE ===================== */
static void print_usage(const char *prog) {
    fprintf(stderr, "\033[1;36mRiven Language Interpreter v%s\033[0m\n", RIVEN_VERSION);
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s                    Start REPL\n", prog);
    fprintf(stderr, "  %s run <file.rv>       Run a Riven source file\n", prog);
    fprintf(stderr, "  %s tokens <file.rv>    Dump tokens (debug)\n", prog);
    fprintf(stderr, "  %s ast <file.rv>       Dump AST (debug)\n", prog);
    fprintf(stderr, "  %s help                Show this help\n", prog);
}

/* ===================== MAIN ===================== */
int main(int argc, char **argv) {
    if (argc == 1) {
        run_repl();
        return 0;
    }

    if (argc >= 2) {
        if (strcmp(argv[1], "run") == 0 || strcmp(argv[1], "forge") == 0) {
            if (argc < 3) { fprintf(stderr, "Specify a .rv file\n"); return 1; }
            run_file(argv[2]);
        } else if (strcmp(argv[1], "tokens") == 0) {
            if (argc < 3) { fprintf(stderr, "Specify a .rv file\n"); return 1; }
            dump_tokens(argv[2]);
        } else if (strcmp(argv[1], "ast") == 0) {
            if (argc < 3) { fprintf(stderr, "Specify a .rv file\n"); return 1; }
            dump_ast(argv[2]);
        } else if (strcmp(argv[1], "help") == 0 || strcmp(argv[1], "--help") == 0) {
            print_usage(argv[0]);
        } else if (strcmp(argv[1], "install") == 0) {
            if (argc < 3) { fprintf(stderr, "Specify package name\n"); return 1; }
            printf("\033[1;33m[rvn] Installing package '%s'...\033[0m\n", argv[2]);
            printf("\033[1;32m[rvn] Package manager not yet fully wired. Coming in v2.0\033[0m\n");
        } else {
            /* Treat as direct file run if it ends in .rv */
            if (strstr(argv[1], ".rv")) run_file(argv[1]);
            else { print_usage(argv[0]); return 1; }
        }
    }
    return 0;
}
