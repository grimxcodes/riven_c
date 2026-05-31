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
#define RIVEN_VERSION   "1.2.0"
#define MAX_TOKENS      65536
#define MAX_IDENT_LEN   256
#define MAX_STR_LEN     4096
#define MAX_VARS        4096
#define MAX_FRAMES      128
#define MAX_ARGS        64
#define MAX_COLL_SIZE   65536
#define MAX_SPARK_TASKS 256
#define MAX_IMPORTS     256

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
    TOK_EOF, TOK_UNKNOWN
} TokenType;

typedef struct {
    TokenType type;
    char      value[MAX_STR_LEN];
    int       line, col;
} Token;

/* ===================== AST ===================== */
typedef enum {
    NODE_PROGRAM, NODE_BLOCK,
    NODE_ASSIGN, NODE_FIRM,
    NODE_IF, NODE_FLOW, NODE_DURING,
    NODE_CRAFT, NODE_RETURN, NODE_CALL,
    NODE_STAMP, NODE_GRAB, NODE_FETCH,
    NODE_FRAME, NODE_SPAWN,
    NODE_MEMBER_ACCESS, NODE_MEMBER_ASSIGN,
    NODE_INDEX, NODE_INDEX_ASSIGN,
    NODE_COLL_LITERAL, NODE_REC_LITERAL,
    NODE_INC, NODE_DEC, NODE_RISE, NODE_DROP,
    NODE_BINOP, NODE_UNOP,
    NODE_IDENT,
    NODE_INT_LIT, NODE_DNUM_LIT, NODE_STR_LIT, NODE_BOOL_LIT, NODE_NULL_LIT,
    NODE_CONSISTOF, NODE_RESC, NODE_ATTACK,
    NODE_SPARK_DEF, NODE_SYNC, NODE_RAW,
    NODE_CAST,
    NODE_PTR, NODE_REF, NODE_BIND,
} NodeType;

typedef struct ASTNode ASTNode;
typedef struct { ASTNode **items; int count; int cap; } NodeList;

struct ASTNode {
    NodeType type;
    int      line;
    union {
        long long ival;
        double    dval;
        char      sval[MAX_STR_LEN];
        int       bval;
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
        /* member read: obj.field */
        struct { ASTNode *obj; char member[MAX_IDENT_LEN]; } member;
        /* member write: obj.field = val */
        struct { ASTNode *obj; char member[MAX_IDENT_LEN]; ASTNode *value; } member_assign;
        /* index read: obj[idx] */
        struct { ASTNode *obj; ASTNode *idx; } index;
        /* index write: obj[idx] = val */
        struct { ASTNode *obj; ASTNode *idx; ASTNode *value; } index_assign;
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
    };
};

/* ===================== RUNTIME VALUE ===================== */
typedef enum {
    VAL_INT, VAL_DNUM, VAL_STRING, VAL_BOOL, VAL_NULL,
    VAL_COLL, VAL_RECORD, VAL_FUNCTION, VAL_FRAME_OBJ,
    VAL_NATIVE_FN, VAL_ERROR,
    VAL_REF,   /* shared mutable box — true alias */
    VAL_PTR,   /* safe pointer to a RivenValue */
} ValueType;

typedef struct RivenValue RivenValue;

typedef struct {
    char       **keys;
    RivenValue **vals;
    int          count, cap;
} RecordMap;

typedef RivenValue *(*NativeFn)(RivenValue **args, int argc);

/* Shared mutable cell for ref aliasing */
typedef struct RivenBox {
    RivenValue *value;
    int         ref_count;
} RivenBox;

/* Safe pointer — points to a RivenValue* slot, never a raw C address */
typedef struct {
    RivenValue **slot;     /* pointer to the env slot holding the value */
    char         tag[MAX_IDENT_LEN];
    int          is_valid;
} RivenPtr;

struct RivenValue {
    ValueType type;
    int       ref_count;
    union {
        long long   ival;
        double      dval;
        char       *sval;
        int         bval;
        struct { RivenValue **items; int count; int cap; } coll;
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
        } frame_obj;
        NativeFn   native_fn;
        char      *error_msg;
        RivenBox  *box;   /* VAL_REF */
        RivenPtr   ptr;   /* VAL_PTR */
    };
};

/* ===================== FRAME DEFINITION ===================== */
typedef struct {
    char      name[MAX_IDENT_LEN];
    RecordMap fields;   /* default field values */
    RecordMap methods;  /* VAL_FUNCTION entries  */
    char      member_names[MAX_ARGS*2][MAX_IDENT_LEN];
    int       member_is_method[MAX_ARGS*2]; /* 1=method, 0=field */
    int       member_access[MAX_ARGS*2];    /* 1=open, 0=hidden  */
    int       member_count;
    char      boot_params[MAX_ARGS][MAX_IDENT_LEN];
    int       boot_param_count;
} FrameDef;

/* ===================== SPARK TASK ===================== */
typedef struct {
    RivenValue    *fn;
    RivenValue    *args[MAX_ARGS];
    int            arg_count;
    pthread_t      thread;
    RivenValue    *result;
    volatile int   done;
    pthread_mutex_t lock;
} SparkTask;

/* ===================== ENVIRONMENT ===================== */
typedef struct Env Env;
struct Env {
    char        names[MAX_VARS][MAX_IDENT_LEN];
    RivenValue *values[MAX_VARS];
    int         firm[MAX_VARS];
    int         count;
    Env        *parent;
};

/* ===================== LEXER / PARSER ===================== */
typedef struct {
    const char *src;
    int pos, line, col;
    Token      *tokens;
    int         token_count, token_cap;
} Lexer;

typedef struct { Token *tokens; int count; int pos; } Parser;

/* ===================== INTERPRETER ===================== */
typedef struct {
    Env       *global_env;
    FrameDef  *frame_defs[MAX_FRAMES];
    int        frame_def_count;
    SparkTask *spark_tasks[MAX_SPARK_TASKS];
    int        spark_task_count;
    pthread_mutex_t spark_lock;
    /* Import cache: resolved absolute paths */
    char       imported[MAX_IMPORTS][MAX_STR_LEN];
    int        import_count;
    /* AST registry: keep all parsed ASTs alive until interpreter is freed */
    ASTNode   *ast_registry[MAX_IMPORTS + 1];
    char      *src_registry[MAX_IMPORTS + 1]; /* heap-allocated source strings */
    Lexer     *lex_registry[MAX_IMPORTS + 1];
    Parser    *par_registry[MAX_IMPORTS + 1];
    int        ast_count;
    /* Current file path for relative imports */
    char       current_file[MAX_STR_LEN];
    /* Control flow */
    int        in_function;
    int        in_loop;
    int        raw_mode;
    RivenValue *return_val;
    RivenValue *thrown_error;
    int         has_error;
} Interpreter;

/* ===================== SIGNALS ===================== */
typedef enum {
    SIGNAL_NONE, SIGNAL_RETURN, SIGNAL_BREAK, SIGNAL_CONTINUE, SIGNAL_ERROR,
} Signal;

extern Signal g_signal;

/* ===================== PROTOTYPES ===================== */

/* NodeList */
void     nl_init(NodeList *l);
void     nl_push(NodeList *l, ASTNode *n);
void     nl_free(NodeList *l);

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
RivenValue *val_alloc_pub(ValueType type);
RivenValue *val_int(long long i);
RivenValue *val_dnum(double d);
RivenValue *val_string(const char *s);
RivenValue *val_bool(int b);
RivenValue *val_null(void);
RivenValue *val_error(const char *msg);
RivenValue *val_coll_empty(void);
void        val_coll_push(RivenValue *c, RivenValue *item);
RivenValue *val_record_empty(void);
void        val_record_set(RivenValue *r, const char *k, RivenValue *v);
RivenValue *val_record_get(RivenValue *r, const char *k);
/* Ref */
RivenValue *val_make_ref(RivenValue *initial);
RivenValue *val_alias_ref(RivenValue *src);
RivenValue *val_ref_get(RivenValue *ref);
void        val_ref_set(RivenValue *ref, RivenValue *v);
/* Ptr — safe, points to env slot */
RivenValue *val_make_ptr(RivenValue **slot, const char *tag);
/* Deref transparent read */
RivenValue *val_deref(RivenValue *v);
/* Lifecycle */
void        val_retain(RivenValue *v);
void        val_release(RivenValue *v);
/* Helpers */
char       *val_to_string(RivenValue *v);
int         val_truthy(RivenValue *v);
RivenValue *val_copy_deep(RivenValue *v);
void        val_print(RivenValue *v);
int         val_equal(RivenValue *a, RivenValue *b);

/* RecordMap */
void        record_init(RecordMap *m);
void        record_set(RecordMap *m, const char *key, RivenValue *val);
RivenValue *record_get(RecordMap *m, const char *key);
void        record_free(RecordMap *m);

/* Environment */
Env        *env_new(Env *parent);
void        env_free(Env *e);
RivenValue *env_get(Env *e, const char *name);
RivenValue **env_slot(Env *e, const char *name); /* get pointer to slot */
void        env_set(Env *e, const char *name, RivenValue *val, int firm);
void        env_update(Env *e, const char *name, RivenValue *val);
int         env_exists(Env *e, const char *name);

/* FrameDef */
FrameDef   *framedef_new(const char *name);
void        framedef_free(FrameDef *fd);
void        framedef_reg_member(FrameDef *fd, const char *bare, int is_method, int is_open);
int         framedef_access(FrameDef *fd, const char *bare, int is_method);
FrameDef   *interp_find_frame(Interpreter *interp, const char *name);
void        interp_add_frame(Interpreter *interp, FrameDef *fd);

/* Interpreter */
Interpreter *interp_new(void);
void         interp_free(Interpreter *interp);
RivenValue  *interp_exec(Interpreter *interp, ASTNode *node, Env *env);
void         interp_run_file(Interpreter *interp, const char *path);
void         interp_register_natives(Interpreter *interp, Env *env);

/* Stdlib */
void stdlib_register(Env *env);

/* Errors */
void riven_error(int line, const char *fmt, ...);
void riven_warn(int line, const char *fmt, ...);

#endif /* RIVEN_H */
