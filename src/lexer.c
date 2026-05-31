#include "../include/riven.h"

Signal g_signal = SIGNAL_NONE;

/* ===== ERROR HELPERS ===== */
void riven_error(int line, const char *fmt, ...) {
    fprintf(stderr, "\033[1;31m[RIVEN ERROR] Line %d: \033[0m", line);
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fprintf(stderr, "\n");
}
void riven_warn(int line, const char *fmt, ...) {
    fprintf(stderr, "\033[1;33m[RIVEN WARN]  Line %d: \033[0m", line);
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fprintf(stderr, "\n");
}

/* ===== NODELIST ===== */
void nodelist_init(NodeList *l) { l->items = NULL; l->count = 0; l->cap = 0; }
void nodelist_push(NodeList *l, ASTNode *n) {
    if (l->count >= l->cap) {
        l->cap = l->cap ? l->cap * 2 : 8;
        l->items = realloc(l->items, sizeof(ASTNode *) * l->cap);
    }
    l->items[l->count++] = n;
}
ASTNode *nodelist_get(NodeList *l, int i) { return (i >= 0 && i < l->count) ? l->items[i] : NULL; }
void nodelist_free(NodeList *l) { free(l->items); l->items = NULL; l->count = l->cap = 0; }

/* ===== LEXER ===== */
Lexer *lexer_new(const char *src) {
    Lexer *l = calloc(1, sizeof(Lexer));
    l->src = src; l->pos = 0; l->line = 1; l->col = 1;
    return l;
}
void lexer_free(Lexer *l) { free(l); }

static char lexer_peek(Lexer *l) { return l->src[l->pos]; }
static char lexer_peek2(Lexer *l) { return l->src[l->pos] ? l->src[l->pos+1] : 0; }
static char lexer_advance(Lexer *l) {
    char c = l->src[l->pos++];
    if (c == '\n') { l->line++; l->col = 1; } else { l->col++; }
    return c;
}

static void lexer_skip_whitespace(Lexer *l) {
    while (l->src[l->pos] && isspace(l->src[l->pos])) lexer_advance(l);
}

static void lexer_skip_line_comment(Lexer *l) {
    while (l->src[l->pos] && l->src[l->pos] != '\n') lexer_advance(l);
}

static void lexer_skip_block_comment(Lexer *l) {
    lexer_advance(l); lexer_advance(l); /* consume << */
    while (l->src[l->pos]) {
        if (l->src[l->pos] == '>' && l->src[l->pos+1] == '>') {
            lexer_advance(l); lexer_advance(l); return;
        }
        lexer_advance(l);
    }
    riven_error(l->line, "Unterminated block comment");
}

static Token make_tok(TokenType t, const char *val, int line, int col) {
    Token tok; tok.type = t; tok.line = line; tok.col = col;
    strncpy(tok.value, val, MAX_STR_LEN-1); tok.value[MAX_STR_LEN-1] = '\0';
    return tok;
}

static struct { const char *word; TokenType type; } keywords[] = {
    {"riven",    TOK_RIVEN},
    {"core",     TOK_CORE},
    {"firm",     TOK_FIRM},
    {"craft",    TOK_CRAFT},
    {"returns",  TOK_RETURNS},
    {"if",       TOK_IF},
    {"altif",    TOK_ALTIF},
    {"else",     TOK_ELSE},
    {"flow",     TOK_FLOW},
    {"during",   TOK_DURING},
    {"frame",    TOK_FRAME},
    {"boot",     TOK_BOOT},
    {"spawn",    TOK_SPAWN},
    {"open",     TOK_OPEN},
    {"hidden",   TOK_HIDDEN},
    {"consistof",TOK_CONSISTOF},
    {"resc",     TOK_RESC},
    {"attack",   TOK_ATTACK},
    {"spark",    TOK_SPARK},
    {"sync",     TOK_SYNC},
    {"bind",     TOK_BIND},
    {"ref",      TOK_REF},
    {"ptr",      TOK_PTR},
    {"raw",      TOK_RAW},
    {"rise",     TOK_RISE},
    {"drop",     TOK_DROP},
    {"and",      TOK_AND},
    {"or",       TOK_OR},
    {"not",      TOK_NOT},
    {"rec",      TOK_REC},
    {"coll",     TOK_COLL},
    {"int",      TOK_INT_TYPE},
    {"dnum",     TOK_DNUM_TYPE},
    {"txt",      TOK_TXT_TYPE},
    {"fetch",    TOK_FETCH},
    {"stamp",    TOK_STAMP},
    {"grab",     TOK_GRAB},
    {"correct",  TOK_CORRECT},
    {"incorrect",TOK_INCORRECT},
    {"emp",      TOK_EMP},
    {NULL, TOK_UNKNOWN}
};

static TokenType keyword_type(const char *word) {
    for (int i = 0; keywords[i].word; i++)
        if (strcmp(keywords[i].word, word) == 0) return keywords[i].type;
    return TOK_IDENT;
}

static void lexer_read_string(Lexer *l, Token *tok) {
    lexer_advance(l); /* skip opening " */
    char buf[MAX_STR_LEN]; int bi = 0; int line = l->line;
    while (l->src[l->pos] && l->src[l->pos] != '"') {
        char c = l->src[l->pos];
        if (c == '\\') {
            lexer_advance(l);
            char esc = l->src[l->pos];
            switch (esc) {
                case 'n':  buf[bi++] = '\n'; break;
                case 't':  buf[bi++] = '\t'; break;
                case '"':  buf[bi++] = '"';  break;
                case '\\': buf[bi++] = '\\'; break;
                default:   buf[bi++] = esc;  break;
            }
        } else { buf[bi++] = c; }
        lexer_advance(l);
        if (bi >= MAX_STR_LEN-1) break;
    }
    if (l->src[l->pos] != '"') riven_error(line, "Unterminated string");
    else lexer_advance(l);
    buf[bi] = '\0';
    *tok = make_tok(TOK_STRING, buf, line, l->col);
}

void lexer_tokenize(Lexer *l) {
    while (1) {
        lexer_skip_whitespace(l);
        if (!l->src[l->pos]) break;

        int line = l->line, col = l->col;
        char c = l->src[l->pos];

        /* Single-line comment ~~ */
        if (c == '~' && l->src[l->pos+1] == '~') {
            lexer_advance(l); lexer_advance(l);
            lexer_skip_line_comment(l);
            continue;
        }
        /* Block comment << >> */
        if (c == '<' && l->src[l->pos+1] == '<') {
            lexer_skip_block_comment(l);
            continue;
        }

        /* String */
        if (c == '"') {
            Token tok;
            lexer_read_string(l, &tok);
            l->tokens[l->token_count++] = tok;
            continue;
        }

        /* Number */
        if (isdigit(c) || (c == '.' && isdigit(l->src[l->pos+1]))) {
            char buf[64]; int bi = 0; int is_dnum = 0;
            while (isdigit(l->src[l->pos]) || l->src[l->pos] == '.') {
                if (l->src[l->pos] == '.') is_dnum = 1;
                buf[bi++] = l->src[l->pos++]; l->col++;
            }
            buf[bi] = '\0';
            l->tokens[l->token_count++] = make_tok(is_dnum ? TOK_DNUM : TOK_INT, buf, line, col);
            continue;
        }

        /* Identifier / keyword */
        if (isalpha(c) || c == '_') {
            char buf[MAX_IDENT_LEN]; int bi = 0;
            while (isalnum(l->src[l->pos]) || l->src[l->pos] == '_') {
                buf[bi++] = l->src[l->pos++]; l->col++;
            }
            buf[bi] = '\0';
            TokenType kw = keyword_type(buf);
            l->tokens[l->token_count++] = make_tok(kw, buf, line, col);
            continue;
        }

        /* +> increment, + plus */
        if (c == '+') {
            if (l->src[l->pos+1] == '>') {
                lexer_advance(l); lexer_advance(l);
                l->tokens[l->token_count++] = make_tok(TOK_INC, "+>", line, col);
            } else {
                lexer_advance(l);
                l->tokens[l->token_count++] = make_tok(TOK_PLUS, "+", line, col);
            }
            continue;
        }
        /* -< decrement, -> (unused), - minus */
        if (c == '-') {
            if (l->src[l->pos+1] == '<') {
                lexer_advance(l); lexer_advance(l);
                l->tokens[l->token_count++] = make_tok(TOK_DEC, "-<", line, col);
            } else {
                lexer_advance(l);
                l->tokens[l->token_count++] = make_tok(TOK_MINUS, "-", line, col);
            }
            continue;
        }
        /* == or = */
        if (c == '=') {
            lexer_advance(l);
            if (l->src[l->pos] == '=') { lexer_advance(l); l->tokens[l->token_count++] = make_tok(TOK_EQ, "==", line, col); }
            else l->tokens[l->token_count++] = make_tok(TOK_ASSIGN, "=", line, col);
            continue;
        }
        /* != */
        if (c == '!') {
            lexer_advance(l);
            if (l->src[l->pos] == '=') { lexer_advance(l); l->tokens[l->token_count++] = make_tok(TOK_NEQ, "!=", line, col); }
            else l->tokens[l->token_count++] = make_tok(TOK_NOT_OP, "!", line, col);
            continue;
        }
        /* <= or < */
        if (c == '<') {
            lexer_advance(l);
            if (l->src[l->pos] == '=') { lexer_advance(l); l->tokens[l->token_count++] = make_tok(TOK_LTE, "<=", line, col); }
            else l->tokens[l->token_count++] = make_tok(TOK_LT, "<", line, col);
            continue;
        }
        /* >= or > */
        if (c == '>') {
            lexer_advance(l);
            if (l->src[l->pos] == '=') { lexer_advance(l); l->tokens[l->token_count++] = make_tok(TOK_GTE, ">=", line, col); }
            else l->tokens[l->token_count++] = make_tok(TOK_GT, ">", line, col);
            continue;
        }
        /* && */
        if (c == '&' && l->src[l->pos+1] == '&') {
            lexer_advance(l); lexer_advance(l);
            l->tokens[l->token_count++] = make_tok(TOK_AND_OP, "&&", line, col);
            continue;
        }
        /* || */
        if (c == '|' && l->src[l->pos+1] == '|') {
            lexer_advance(l); lexer_advance(l);
            l->tokens[l->token_count++] = make_tok(TOK_OR_OP, "||", line, col);
            continue;
        }

        /* Single chars */
        struct { char ch; TokenType t; const char *s; } singles[] = {
            {'*', TOK_STAR,    "*"}, {'/', TOK_SLASH,   "/"},
            {'%', TOK_PERCENT, "%"}, {'.', TOK_DOT,     "."},
            {',', TOK_COMMA,   ","}, {':', TOK_COLON,   ":"},
            {';', TOK_SEMICOLON,";"},{'{', TOK_LBRACE,  "{"},
            {'}', TOK_RBRACE,  "}"}, {'(', TOK_LPAREN,  "("},
            {')', TOK_RPAREN,  ")"}, {'[', TOK_LBRACKET,"["},
            {']', TOK_RBRACKET,"]"}, {0,   TOK_UNKNOWN, ""}
        };
        int matched = 0;
        for (int i = 0; singles[i].ch; i++) {
            if (c == singles[i].ch) {
                lexer_advance(l);
                l->tokens[l->token_count++] = make_tok(singles[i].t, singles[i].s, line, col);
                matched = 1; break;
            }
        }
        if (!matched) {
            char unk[2] = {c, 0};
            riven_warn(line, "Unknown character '%s'", unk);
            lexer_advance(l);
        }
    }
    l->tokens[l->token_count++] = make_tok(TOK_EOF, "", l->line, l->col);
}

void lexer_print_tokens(Lexer *l) {
    const char *names[] = {
        "INT","DNUM","STRING","CORRECT","INCORRECT","EMP",
        "RIVEN","CORE","FIRM","CRAFT","RETURNS","IF","ALTIF","ELSE",
        "FLOW","DURING","FRAME","BOOT","SPAWN","OPEN","HIDDEN",
        "CONSISTOF","RESC","ATTACK","SPARK","SYNC","BIND","REF","PTR","RAW",
        "RISE","DROP","AND","OR","NOT","REC","COLL",
        "INT_TYPE","DNUM_TYPE","TXT_TYPE","FETCH","STAMP","GRAB",
        "IDENT","ASSIGN","EQ","NEQ","LT","GT","LTE","GTE",
        "PLUS","MINUS","STAR","SLASH","PERCENT","INC","DEC",
        "AND_OP","OR_OP","NOT_OP","DOT","COMMA","COLON","SEMICOLON",
        "LBRACE","RBRACE","LPAREN","RPAREN","LBRACKET","RBRACKET",
        "TILDE_TILDE","EOF","UNKNOWN"
    };
    for (int i = 0; i < l->token_count; i++) {
        Token *t = &l->tokens[i];
        int idx = t->type; if (idx < 0) idx = 0;
        printf("[%3d:%2d] %-14s '%s'\n", t->line, t->col, names[idx], t->value);
    }
}
