#ifndef RIVEN_H
#define RIVEN_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <ctype.h>
#include <stdarg.h>
#include <pthread.h>

/* ===================== VERSION ===================== */
#define RIVEN_VERSION  "1.1.0"
#define MAX_TOKENS     65536
#define MAX_NODES      65536
#define MAX_IDENT_LEN  256
#define MAX_STR_LEN    4096
#define MAX_SCOPE      256
#define MAX_VARS       4096
#define MAX_FRAMES     128
#define MAX_CALL_STACK 512
#define MAX_ARGS       64
#define MAX_COLL_SIZE  65536
#define MAX_SPARK_TASKS 256

/* ===================== TOKEN TYPES ===================== */
typedef enum {
    TOK_INT, TOK_DNUM, TOK_STRING, TOK_CORRECT, TOK_INCORRECT, TOK_EMP,
    TOK_RIVEN, TOK_CORE, TOK_FIRM, TOK_CRAFT, TOK_RETURNS,
    TOK_IF, TOK_ALTIF, TOK_ELSE,
    TOK_FLOW, TOK_DURING,
    TOK_FRAME, TOK_BOOT, TOK_SPAWN, TOK_OPEN, TOK_HIDDEN,
    TOK_CONSISTOF, TOK_RESC, TOK_ATTACK,
    TOK_SPARK, TOK_SYNC,
    TOK_BIND, TOK_REF, TOK_PTR, TOK_RAW,
    TOK_RISE, TOK_DROP,
    TOK_AND, TOK_OR, TOK_NOT,
    TOK_REC, TOK_COLL,
    TOK_INT_TYPE, TOK_DNUM_TYPE, TOK_TXT_TYPE,
    TOK_FETCH, TOK_STAMP, TOK_GRAB,
    TOK_IDENT,
    TOK_ASSIGN, TOK_EQ, TOK_NEQ,
    TOK_LT, TOK_GT, TOK_LTE, TOK_GTE,
    TOK_PLUS, TOK_MINUS, TOK_STAR, TOK_SLASH, TOK_PERCENT,
    TOK_INC, TOK_DEC,
    TOK_AND_OP, TOK_OR_OP, TOK_NOT_OP,
    TOK_DOT, TOK_COMMA, TOK_COLON, TOK_SEMICOLON,
    TOK_LBRACE, TOK_RBRACE, TOK_LPAREN, TOK_RPAREN,
    TOK_LBRACKET, TOK_RBRACKET,
    TOK_TILDE_TILDE,
    TOK_EOF,
    TOK_UNKNOWN
} TokenType;

/* ===================== TOKEN ===================== */
typedef struct {
    TokenType   type;
    char        value[MAX_STR_LEN];
    int         line;
    int         col;
} Token;

/* ===================== AST NODE TYPES ===================== */
typedef enum {
    NODE_PROGRAM, NODE_BLOCK,
    NODE_ASSIGN, NODE_FIRM,
    NODE_IF, NODE_FLOW, NODE_DURING,
    NODE_CRAFT, NODE_RETURN, NODE_CALL,
    NODE_STAMP, NODE_GRAB, NODE_FETCH,
    NODE_FRAME, NODE_SPAWN,
    NODE_MEMBER_ACCESS, NODE_INDEX,
    NODE_COLL_LITERAL, NODE_REC_LITERAL,
    NODE_INC, NODE_DEC, NODE_RISE, NODE_DROP,
    NODE_BINOP, NODE_UNOP,
    NODE_IDENT,
    NODE_INT_LIT, NODE_DNUM_LIT, NODE_STR_LIT, NODE_BOOL_LIT, NODE_NULL_LIT,
    NODE_CONSISTOF, NODE_RESC, NODE_ATTACK,
    NODE_SPARK, NODE_SYNC, NODE_RAW,
    NODE_CAST,
    NODE_PTR, NODE_REF, NODE_BIND,
} NodeType;

/* ===================== AST NODE ===================== */
typedef struct ASTNode ASTNode;
typedef struct { ASTNode **items; int count; int capacity; } NodeList;

struct ASTNode {
    NodeType type;
    int      line;
    union {
        long long  ival;
        double     dval;
        char       sval[MAX_STR_LEN];
        int        bval;
        struct { ASTNode *left; ASTNode *right; char op[8]; }  binop;
        struct { ASTNode *operand; char op[8]; }               unop;
        struct { char name[MAX_IDENT_LEN]; ASTNode *value; int is_firm; } assign;
        struct { ASTNode *cond; ASTNode *body; ASTNode *altifs; ASTNode *els; } ifstmt;
        struct { ASTNode *count; ASTNode *body; } flow;
        struct { ASTNode *cond;  ASTNode *body; } during;
        struct {
            char     name[MAX_IDENT_LEN];
            char     params[MAX_ARGS][MAX_IDENT_LEN];
            int      param_count;
            ASTNode *body;
            int      is_spark;
        } craft;
        struct { ASTNode *value; } ret;
        struct { ASTNode *callee; ASTNode *args[MAX_ARGS]; int arg_count; } call;
        struct { NodeList stmts; } block;
        struct { char name[MAX_IDENT_LEN]; NodeList members; } frame;
        struct { char frame_name[MAX_IDENT_LEN]; ASTNode *args[MAX_ARGS]; int arg_count; } spawn;
        struct { ASTNode *obj; char member[MAX_IDENT_LEN]; } member;
        struct { ASTNode *obj; ASTNode *idx; } index;
        struct { NodeList items; } coll;
        struct { char keys[MAX_ARGS][MAX_IDENT_LEN]; ASTNode *vals[MAX_ARGS]; int count; } rec;
        struct { char name[MAX_IDENT_LEN]; } incdec;
        struct { ASTNode *fmt; ASTNode *args[MAX_ARGS]; int arg_count; } stamp;
        struct { ASTNode *prompt; } grab;
        struct { ASTNode *url; }    fetch;
        struct { ASTNode *body; }   resc;
        struct { ASTNode *msg; }    attack;
        struct { ASTNode *body; }   raw;
        struct { char path[MAX_STR_LEN]; } consistof;
        struct { char target_type[32]; ASTNode *value; } cast;
        struct { char name[MAX_IDENT_LEN]; ASTNode *value; } refptr;
        struct { int dummy; } sync_stmt;
    };
};

/* ===================== RUNTIME VALUE ===================== */
typedef enum {
    VAL_INT, VAL_DNUM, VAL_STRING, VAL_BOOL, VAL_NULL,
    VAL_COLL, VAL_RECORD, VAL_FUNCTION, VAL_FRAME_OBJ,
    VAL_NATIVE_FN, VAL_ERROR,
    VAL_REF,   /* mutable heap cell — true alias */
    VAL_PTR,   /* raw pointer with address + tag    */
} ValueType;

typedef struct RivenValue RivenValue;
typedef struct {
    char       **keys;
    RivenValue **vals;
    int          count;
    int          capacity;
} RecordMap;

typedef RivenValue *(*NativeFn)(RivenValue **args, int argc);

/* ----- Ref box: a single heap cell shared by aliases ----- */
typedef struct RivenBox {
    RivenValue *value;     /* the actual value */
    int         ref_count; /* how many VAL_REF values point here */
} RivenBox;

/* ----- Raw pointer descriptor ----- */
typedef struct {
    uintptr_t   address;          /* actual address (0 for named symbols) */
    char        tag[MAX_IDENT_LEN]; /* symbolic name e.g. "kernel", "heap" */
    int         is_valid;         /* bounds/lifetime check */
    size_t      size;             /* byte width (0 = unknown) */
} RivenPtr;

struct RivenValue {
    ValueType type;
    int       ref_count;
    union {
        long long   ival;
        double      dval;
        char       *sval;
        int         bval;
        struct { RivenValue **items; int count; int capacity; } coll;
        RecordMap   record;
        struct {
            char     name[MAX_IDENT_LEN];
            char     params[MAX_ARGS][MAX_IDENT_LEN];
            int      param_count;
            ASTNode *body;
            void    *closure_env;
            int      is_spark;
        } fn;
        struct {
            char      frame_name[MAX_IDENT_LEN];
            RecordMap fields;
            RecordMap methods;
            /* access control bitmask: keys listed as open/hidden */
            char      open_methods[MAX_ARGS][MAX_IDENT_LEN];
            int       open_method_count;
        } frame_obj;
        NativeFn    native_fn;
        char       *error_msg;
        RivenBox   *box;   /* VAL_REF  — shared mutable cell */
        RivenPtr    ptr;   /* VAL_PTR  — raw pointer struct  */
    };
};

/* ===================== FRAME DEFINITION ===================== */
/* Stored in the interpreter's frame table, carries access metadata */
typedef struct {
    char     name[MAX_IDENT_LEN];
    RecordMap fields;           /* default field values  */
    RecordMap methods;          /* VAL_FUNCTION entries  */
    /* access: 1 = open (public), 0 = hidden (private) */
    char     method_names[MAX_ARGS][MAX_IDENT_LEN];
    int      method_access[MAX_ARGS];  /* 1=open, 0=hidden */
    int      method_count;
    char     field_names[MAX_ARGS][MAX_IDENT_LEN];
    int      field_access[MAX_ARGS];
    int      field_count;
    /* boot param names (supports parameterised constructor) */
    char     boot_params[MAX_ARGS][MAX_IDENT_LEN];
    int      boot_param_count;
} FrameDef;

/* ===================== SPARK TASK ===================== */
typedef struct SparkTask {
    RivenValue    *fn;       /* borrowed ref – must retain */
    RivenValue    *args[MAX_ARGS];
    int            arg_count;
    pthread_t      thread;
    RivenValue    *result;   /* written when done */
    int            done;     /* atomic-ish flag   */
    pthread_mutex_t lock;
} SparkTask;

/* ===================== ENVIRONMENT ===================== */
typedef struct Env Env;
struct Env {
    char        names[MAX_VARS][MAX_IDENT_LEN];
    RivenValue *values[MAX_VARS];
    int         count;
    int         firm[MAX_VARS];
    Env        *parent;
};

/* ===================== LEXER ===================== */
typedef struct {
    const char *src;
    int         pos, line, col;
    Token       tokens[MAX_TOKENS];
    int         token_count;
} Lexer;

/* ===================== PARSER ===================== */
typedef struct { Token *tokens; int count; int pos; } Parser;

/* ===================== INTERPRETER ===================== */
typedef struct {
    Env        *global_env;
    /* Frame definitions keyed by name */
    FrameDef   *frame_defs[MAX_FRAMES];
    int         frame_def_count;
    /* Spark task registry */
    SparkTask  *spark_tasks[MAX_SPARK_TASKS];
    int         spark_task_count;
    pthread_mutex_t spark_lock;
    /* Control flow */
    int         in_function;
    int         in_loop;
    int         raw_mode;         /* 0=safe, 1=raw/unsafe */
    RivenValue *return_val;
    RivenValue *thrown_error;
    int         has_error;
} Interpreter;

/* ===================== CONTROL FLOW SIGNALS ===================== */
typedef enum {
    SIGNAL_NONE, SIGNAL_RETURN, SIGNAL_BREAK, SIGNAL_CONTINUE, SIGNAL_ERROR,
} Signal;

extern Signal g_signal;

/* ===================== FUNCTION PROTOTYPES ===================== */

/* Lexer */
Lexer   *lexer_new(const char *src);
void     lexer_tokenize(Lexer *l);
void     lexer_free(Lexer *l);
void     lexer_print_tokens(Lexer *l);

/* Parser */
Parser  *parser_new(Token *tokens, int count);
ASTNode *parser_parse(Parser *p);
void     parser_free(Parser *p);
void     ast_free(ASTNode *node);
void     ast_print(ASTNode *node, int indent);

/* Values */
RivenValue *val_alloc_pub(ValueType type);   /* allocate raw value cell   */
RivenValue *val_alias_ref(RivenValue *src);  /* alias an existing ref box */
RivenValue *val_int(long long i);
RivenValue *val_dnum(double d);
RivenValue *val_string(const char *s);
RivenValue *val_bool(int b);
RivenValue *val_null(void);
RivenValue *val_error(const char *msg);
RivenValue *val_coll_empty(void);
void        val_coll_push(RivenValue *coll, RivenValue *item);
RivenValue *val_record_empty(void);
void        val_record_set(RivenValue *rec, const char *key, RivenValue *val);
RivenValue *val_record_get(RivenValue *rec, const char *key);
RivenValue *val_make_ref(RivenValue *initial);   /* NEW: create a ref box */
RivenValue *val_ref_get(RivenValue *ref);         /* NEW: dereference       */
void        val_ref_set(RivenValue *ref, RivenValue *v); /* NEW: write through */
RivenValue *val_make_ptr(uintptr_t addr, const char *tag, size_t sz); /* NEW */
void        val_retain(RivenValue *v);
void        val_release(RivenValue *v);
char       *val_to_string(RivenValue *v);
int         val_truthy(RivenValue *v);
RivenValue *val_copy(RivenValue *v);
void        val_print(RivenValue *v);
int         val_equal(RivenValue *a, RivenValue *b);
RivenValue *val_deref(RivenValue *v); /* auto-deref VAL_REF when reading */

/* Environment */
Env        *env_new(Env *parent);
void        env_free(Env *e);
RivenValue *env_get(Env *e, const char *name);
void        env_set(Env *e, const char *name, RivenValue *val, int firm);
void        env_update(Env *e, const char *name, RivenValue *val);
int         env_exists(Env *e, const char *name);
int         env_is_firm(Env *e, const char *name);
/* NEW: write through a ref-typed variable */
void        env_ref_write(Env *e, const char *name, RivenValue *val);

/* Interpreter */
Interpreter *interp_new(void);
void         interp_free(Interpreter *interp);
RivenValue  *interp_exec(Interpreter *interp, ASTNode *node, Env *env);
void         interp_run_file(Interpreter *interp, const char *path);
void         interp_register_natives(Interpreter *interp, Env *env);

/* Frame definitions */
FrameDef   *framedef_new(const char *name);
void        framedef_free(FrameDef *fd);
FrameDef   *interp_find_frame(Interpreter *interp, const char *name);
void        interp_add_frame(Interpreter *interp, FrameDef *fd);

/* RecordMap helpers */
void        record_init(RecordMap *m);
void        record_set(RecordMap *m, const char *key, RivenValue *val);
RivenValue *record_get(RecordMap *m, const char *key);
void        record_free(RecordMap *m);

/* NodeList helpers */
void     nodelist_init(NodeList *l);
void     nodelist_push(NodeList *l, ASTNode *n);
ASTNode *nodelist_get(NodeList *l, int i);
void     nodelist_free(NodeList *l);

/* Error helpers */
void riven_error(int line, const char *fmt, ...);
void riven_warn(int line, const char *fmt, ...);

/* Native stdlib */
void stdlib_register(Env *env);

/* Raw mode built-ins */
RivenValue *raw_read_mem(uintptr_t addr, size_t sz);
void        raw_write_mem(uintptr_t addr, size_t sz, uint64_t val);

#endif /* RIVEN_H */
