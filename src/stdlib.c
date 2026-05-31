#include "../include/riven.h"
#include <time.h>
#include <math.h>

/* ================== NATIVE FUNCTION WRAPPERS ================== */

/* math.sqrt */
static RivenValue *native_math_sqrt(RivenValue **args, int argc) {
    if (argc < 1) return val_error("sqrt requires 1 argument");
    double x = (args[0]->type == VAL_DNUM) ? args[0]->dval : (double)args[0]->ival;
    return val_dnum(sqrt(x));
}
static RivenValue *native_math_pow(RivenValue **args, int argc) {
    if (argc < 2) return val_error("pow requires 2 args");
    double a = (args[0]->type==VAL_DNUM)?args[0]->dval:(double)args[0]->ival;
    double b = (args[1]->type==VAL_DNUM)?args[1]->dval:(double)args[1]->ival;
    return val_dnum(pow(a, b));
}
static RivenValue *native_math_abs(RivenValue **args, int argc) {
    if (argc < 1) return val_error("abs requires 1 arg");
    if (args[0]->type == VAL_INT) return val_int(llabs(args[0]->ival));
    return val_dnum(fabs(args[0]->dval));
}
static RivenValue *native_math_floor(RivenValue **args, int argc) {
    if (argc < 1) return val_error("floor requires 1 arg");
    double x = (args[0]->type==VAL_DNUM)?args[0]->dval:(double)args[0]->ival;
    return val_int((long long)floor(x));
}
static RivenValue *native_math_ceil(RivenValue **args, int argc) {
    if (argc < 1) return val_error("ceil requires 1 arg");
    double x = (args[0]->type==VAL_DNUM)?args[0]->dval:(double)args[0]->ival;
    return val_int((long long)ceil(x));
}
static RivenValue *native_math_round(RivenValue **args, int argc) {
    if (argc < 1) return val_error("round requires 1 arg");
    double x = (args[0]->type==VAL_DNUM)?args[0]->dval:(double)args[0]->ival;
    return val_int((long long)round(x));
}
static RivenValue *native_math_sin(RivenValue **args, int argc) {
    if (argc < 1) return val_error("sin requires 1 arg");
    double x = (args[0]->type==VAL_DNUM)?args[0]->dval:(double)args[0]->ival;
    return val_dnum(sin(x));
}
static RivenValue *native_math_cos(RivenValue **args, int argc) {
    if (argc < 1) return val_error("cos requires 1 arg");
    double x = (args[0]->type==VAL_DNUM)?args[0]->dval:(double)args[0]->ival;
    return val_dnum(cos(x));
}
static RivenValue *native_math_log(RivenValue **args, int argc) {
    if (argc < 1) return val_error("log requires 1 arg");
    double x = (args[0]->type==VAL_DNUM)?args[0]->dval:(double)args[0]->ival;
    return val_dnum(log(x));
}
static RivenValue *native_math_rand(RivenValue **args, int argc) {
    (void)args; (void)argc;
    return val_dnum((double)rand() / RAND_MAX);
}
static RivenValue *native_math_randint(RivenValue **args, int argc) {
    if (argc < 2) return val_error("randint requires 2 args");
    long long lo = args[0]->ival, hi = args[1]->ival;
    return val_int(lo + (rand() % (hi - lo + 1)));
}
static RivenValue *native_math_pi(RivenValue **args, int argc) {
    (void)args; (void)argc; return val_dnum(M_PI);
}

/* string functions */
static RivenValue *native_str_len(RivenValue **args, int argc) {
    if (argc < 1) return val_error("len requires 1 arg");
    if (args[0]->type == VAL_STRING) return val_int(strlen(args[0]->sval));
    if (args[0]->type == VAL_COLL)   return val_int(args[0]->coll.count);
    if (args[0]->type == VAL_RECORD) return val_int(args[0]->record.count);
    return val_int(0);
}
static RivenValue *native_str_upper(RivenValue **args, int argc) {
    if (argc < 1 || args[0]->type != VAL_STRING) return val_error("upper requires a txt");
    char *s = strdup(args[0]->sval);
    for (int i = 0; s[i]; i++) s[i] = toupper(s[i]);
    RivenValue *r = val_string(s); free(s); return r;
}
static RivenValue *native_str_lower(RivenValue **args, int argc) {
    if (argc < 1 || args[0]->type != VAL_STRING) return val_error("lower requires a txt");
    char *s = strdup(args[0]->sval);
    for (int i = 0; s[i]; i++) s[i] = tolower(s[i]);
    RivenValue *r = val_string(s); free(s); return r;
}
static RivenValue *native_str_split(RivenValue **args, int argc) {
    if (argc < 2 || args[0]->type != VAL_STRING || args[1]->type != VAL_STRING)
        return val_error("split(txt, sep)");
    RivenValue *coll = val_coll_empty();
    char *s = strdup(args[0]->sval);
    char *sep = args[1]->sval;
    char *tok = strtok(s, sep);
    while (tok) {
        RivenValue *sv = val_string(tok);
        val_coll_push(coll, sv); val_release(sv);
        tok = strtok(NULL, sep);
    }
    free(s); return coll;
}
static RivenValue *native_str_contains(RivenValue **args, int argc) {
    if (argc < 2) return val_error("contains(txt, pattern)");
    if (args[0]->type != VAL_STRING || args[1]->type != VAL_STRING) return val_bool(0);
    return val_bool(strstr(args[0]->sval, args[1]->sval) != NULL);
}
static RivenValue *native_str_starts(RivenValue **args, int argc) {
    if (argc < 2) return val_error("starts(txt, prefix)");
    if (args[0]->type != VAL_STRING || args[1]->type != VAL_STRING) return val_bool(0);
    return val_bool(strncmp(args[0]->sval, args[1]->sval, strlen(args[1]->sval)) == 0);
}
static RivenValue *native_str_ends(RivenValue **args, int argc) {
    if (argc < 2) return val_error("ends(txt, suffix)");
    if (args[0]->type != VAL_STRING || args[1]->type != VAL_STRING) return val_bool(0);
    size_t sl = strlen(args[0]->sval), el = strlen(args[1]->sval);
    if (el > sl) return val_bool(0);
    return val_bool(strcmp(args[0]->sval + sl - el, args[1]->sval) == 0);
}
static RivenValue *native_str_slice(RivenValue **args, int argc) {
    if (argc < 3) return val_error("slice(txt, start, end)");
    if (args[0]->type != VAL_STRING) return val_error("slice needs txt");
    const char *s = args[0]->sval;
    int len = strlen(s);
    int start = (int)args[1]->ival, end = (int)args[2]->ival;
    if (start < 0) start = 0; if (end > len) end = len;
    if (start > end) return val_string("");
    char buf[MAX_STR_LEN]; int bi = 0;
    for (int i = start; i < end && bi < MAX_STR_LEN-1; i++) buf[bi++] = s[i];
    buf[bi] = '\0';
    return val_string(buf);
}
static RivenValue *native_str_replace(RivenValue **args, int argc) {
    if (argc < 3) return val_error("replace(txt, from, to)");
    if (args[0]->type != VAL_STRING || args[1]->type != VAL_STRING || args[2]->type != VAL_STRING)
        return val_error("replace needs txt args");
    const char *src = args[0]->sval, *from = args[1]->sval, *to = args[2]->sval;
    char buf[MAX_STR_LEN]; int bi = 0; size_t fl = strlen(from);
    while (*src && bi < MAX_STR_LEN-1) {
        if (fl && strncmp(src, from, fl) == 0) {
            for (int i = 0; to[i] && bi < MAX_STR_LEN-1; i++) buf[bi++] = to[i];
            src += fl;
        } else buf[bi++] = *src++;
    }
    buf[bi] = '\0'; return val_string(buf);
}
static RivenValue *native_str_trim(RivenValue **args, int argc) {
    if (argc < 1 || args[0]->type != VAL_STRING) return val_error("trim needs txt");
    const char *s = args[0]->sval;
    while (*s && isspace(*s)) s++;
    char buf[MAX_STR_LEN]; strncpy(buf, s, MAX_STR_LEN-1); buf[MAX_STR_LEN-1] = '\0';
    int l = strlen(buf);
    while (l > 0 && isspace(buf[l-1])) buf[--l] = '\0';
    return val_string(buf);
}
static RivenValue *native_str_join(RivenValue **args, int argc) {
    if (argc < 2 || args[0]->type != VAL_COLL || args[1]->type != VAL_STRING)
        return val_error("join(coll, sep)");
    char buf[MAX_STR_LEN] = ""; const char *sep = args[1]->sval;
    for (int i = 0; i < args[0]->coll.count; i++) {
        char *s = val_to_string(args[0]->coll.items[i]);
        strncat(buf, s, MAX_STR_LEN - strlen(buf) - 1); free(s);
        if (i < args[0]->coll.count-1) strncat(buf, sep, MAX_STR_LEN - strlen(buf) - 1);
    }
    return val_string(buf);
}

/* coll functions */
static RivenValue *native_coll_push(RivenValue **args, int argc) {
    if (argc < 2 || args[0]->type != VAL_COLL) return val_error("push(coll, item)");
    val_coll_push(args[0], args[1]); return val_null();
}
static RivenValue *native_coll_pop(RivenValue **args, int argc) {
    if (argc < 1 || args[0]->type != VAL_COLL) return val_error("pop(coll)");
    if (args[0]->coll.count == 0) return val_null();
    RivenValue *v = args[0]->coll.items[--args[0]->coll.count];
    /* caller owns this now, so don't release here */
    return v;
}
static RivenValue *native_coll_remove(RivenValue **args, int argc) {
    if (argc < 2 || args[0]->type != VAL_COLL) return val_error("remove(coll, idx)");
    int idx = (int)args[1]->ival;
    if (idx < 0 || idx >= args[0]->coll.count) return val_error("Index out of bounds");
    val_release(args[0]->coll.items[idx]);
    for (int i = idx; i < args[0]->coll.count-1; i++)
        args[0]->coll.items[i] = args[0]->coll.items[i+1];
    args[0]->coll.count--;
    return val_null();
}
static RivenValue *native_coll_sort(RivenValue **args, int argc) {
    if (argc < 1 || args[0]->type != VAL_COLL) return val_error("sort(coll)");
    /* Bubble sort for simplicity */
    RivenValue **arr = args[0]->coll.items; int n = args[0]->coll.count;
    for (int i = 0; i < n-1; i++) for (int j = 0; j < n-i-1; j++) {
        RivenValue *a = arr[j], *b = arr[j+1];
        int swap = 0;
        if (a->type == VAL_INT && b->type == VAL_INT) swap = a->ival > b->ival;
        else if (a->type == VAL_DNUM && b->type == VAL_DNUM) swap = a->dval > b->dval;
        else if (a->type == VAL_STRING && b->type == VAL_STRING) swap = strcmp(a->sval, b->sval) > 0;
        if (swap) { arr[j] = b; arr[j+1] = a; }
    }
    return val_null();
}
static RivenValue *native_coll_reverse(RivenValue **args, int argc) {
    if (argc < 1 || args[0]->type != VAL_COLL) return val_error("reverse(coll)");
    RivenValue **arr = args[0]->coll.items; int n = args[0]->coll.count;
    for (int i = 0; i < n/2; i++) { RivenValue *t = arr[i]; arr[i] = arr[n-1-i]; arr[n-1-i] = t; }
    return val_null();
}
static RivenValue *native_coll_contains(RivenValue **args, int argc) {
    if (argc < 2 || args[0]->type != VAL_COLL) return val_error("contains(coll, item)");
    for (int i = 0; i < args[0]->coll.count; i++)
        if (val_equal(args[0]->coll.items[i], args[1])) return val_bool(1);
    return val_bool(0);
}
static RivenValue *native_coll_range(RivenValue **args, int argc) {
    if (argc < 2) return val_error("range(start, end)");
    long long start = args[0]->ival, end = args[1]->ival;
    long long step = (argc >= 3) ? args[2]->ival : 1;
    RivenValue *c = val_coll_empty();
    for (long long i = start; (step > 0 ? i < end : i > end); i += step) {
        RivenValue *iv = val_int(i); val_coll_push(c, iv); val_release(iv);
    }
    return c;
}

/* type checking */
static RivenValue *native_type_of(RivenValue **args, int argc) {
    if (argc < 1) return val_string("emp");
    switch (args[0]->type) {
        case VAL_INT:       return val_string("int");
        case VAL_DNUM:      return val_string("dnum");
        case VAL_STRING:    return val_string("txt");
        case VAL_BOOL:      return val_string("bool");
        case VAL_NULL:      return val_string("emp");
        case VAL_COLL:      return val_string("coll");
        case VAL_RECORD:    return val_string("rec");
        case VAL_FRAME_OBJ: return val_string("frame");
        case VAL_FUNCTION:  return val_string("craft");
        case VAL_NATIVE_FN: return val_string("native-craft");
        default:            return val_string("unknown");
    }
}
static RivenValue *native_is_int(RivenValue **args, int argc) {
    return val_bool(argc >= 1 && args[0]->type == VAL_INT);
}
static RivenValue *native_is_dnum(RivenValue **args, int argc) {
    return val_bool(argc >= 1 && args[0]->type == VAL_DNUM);
}
static RivenValue *native_is_txt(RivenValue **args, int argc) {
    return val_bool(argc >= 1 && args[0]->type == VAL_STRING);
}
static RivenValue *native_is_emp(RivenValue **args, int argc) {
    return val_bool(argc >= 1 && args[0]->type == VAL_NULL);
}
static RivenValue *native_is_coll(RivenValue **args, int argc) {
    return val_bool(argc >= 1 && args[0]->type == VAL_COLL);
}

/* I/O */
static RivenValue *native_stamp_ln(RivenValue **args, int argc) {
    for (int i = 0; i < argc; i++) { val_print(args[i]); if (i < argc-1) printf(" "); }
    printf("\n"); return val_null();
}
static RivenValue *native_read_file(RivenValue **args, int argc) {
    if (argc < 1 || args[0]->type != VAL_STRING) return val_error("read_file(path)");
    FILE *f = fopen(args[0]->sval, "r");
    if (!f) return val_error("Cannot open file");
    fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
    char *buf = malloc(sz+1); fread(buf, 1, sz, f); buf[sz] = '\0';
    fclose(f); RivenValue *r = val_string(buf); free(buf); return r;
}
static RivenValue *native_write_file(RivenValue **args, int argc) {
    if (argc < 2) return val_error("write_file(path, content)");
    FILE *f = fopen(args[0]->sval, "w");
    if (!f) return val_error("Cannot write file");
    char *s = val_to_string(args[1]); fprintf(f, "%s", s); free(s);
    fclose(f); return val_bool(1);
}
static RivenValue *native_append_file(RivenValue **args, int argc) {
    if (argc < 2) return val_error("append_file(path, content)");
    FILE *f = fopen(args[0]->sval, "a");
    if (!f) return val_error("Cannot append file");
    char *s = val_to_string(args[1]); fprintf(f, "%s", s); free(s);
    fclose(f); return val_bool(1);
}

/* system */
static RivenValue *native_exit_fn(RivenValue **args, int argc) {
    int code = (argc >= 1) ? (int)args[0]->ival : 0;
    exit(code); return val_null();
}
static RivenValue *native_time_now(RivenValue **args, int argc) {
    (void)args; (void)argc; return val_int((long long)time(NULL));
}
static RivenValue *native_sleep_fn(RivenValue **args, int argc) {
    if (argc < 1) return val_error("sleep(ms)");
    /* Use busy wait for portability */
    long long ms = args[0]->ival;
    clock_t start = clock();
    while ((clock() - start) * 1000 / CLOCKS_PER_SEC < ms) {}
    return val_null();
}

/* format/print helper */
static RivenValue *native_format(RivenValue **args, int argc) {
    if (argc < 1 || args[0]->type != VAL_STRING) return val_error("format(txt, ...)");
    const char *fmt = args[0]->sval;
    char buf[MAX_STR_LEN]; int bi = 0, ai = 1;
    for (int i = 0; fmt[i] && bi < MAX_STR_LEN-1; i++) {
        if (fmt[i] == '{' && fmt[i+1] == '}') {
            if (ai < argc) {
                char *s = val_to_string(args[ai++]);
                for (int j = 0; s[j] && bi < MAX_STR_LEN-1; j++) buf[bi++] = s[j];
                free(s);
            }
            i++; /* skip } */
        } else buf[bi++] = fmt[i];
    }
    buf[bi] = '\0'; return val_string(buf);
}

/* record helpers */
static RivenValue *native_rec_keys(RivenValue **args, int argc) {
    if (argc < 1 || args[0]->type != VAL_RECORD) return val_error("keys(rec)");
    RivenValue *c = val_coll_empty();
    for (int i = 0; i < args[0]->record.count; i++) {
        RivenValue *k = val_string(args[0]->record.keys[i]);
        val_coll_push(c, k); val_release(k);
    }
    return c;
}
static RivenValue *native_rec_vals(RivenValue **args, int argc) {
    if (argc < 1 || args[0]->type != VAL_RECORD) return val_error("vals(rec)");
    RivenValue *c = val_coll_empty();
    for (int i = 0; i < args[0]->record.count; i++) {
        val_coll_push(c, args[0]->record.vals[i]);
    }
    return c;
}
static RivenValue *native_rec_has(RivenValue **args, int argc) {
    if (argc < 2 || args[0]->type != VAL_RECORD || args[1]->type != VAL_STRING)
        return val_error("has(rec, key)");
    return val_bool(record_get(&args[0]->record, args[1]->sval) != NULL);
}

/* ========================= REGISTER ALL ========================= */
static void reg(Env *env, const char *name, NativeFn fn) {
    RivenValue *v = val_alloc_pub(VAL_NATIVE_FN);
    v->native_fn = fn;
    env_set(env, name, v, 0);
    val_release(v);
}

void stdlib_register(Env *env) {
    srand((unsigned)time(NULL));

    /* math */
    reg(env, "sqrt",    native_math_sqrt);
    reg(env, "pow",     native_math_pow);
    reg(env, "abs",     native_math_abs);
    reg(env, "floor",   native_math_floor);
    reg(env, "ceil",    native_math_ceil);
    reg(env, "round",   native_math_round);
    reg(env, "sin",     native_math_sin);
    reg(env, "cos",     native_math_cos);
    reg(env, "log",     native_math_log);
    reg(env, "rand",    native_math_rand);
    reg(env, "randint", native_math_randint);
    reg(env, "pi",      native_math_pi);

    /* string */
    reg(env, "len",      native_str_len);
    reg(env, "upper",    native_str_upper);
    reg(env, "lower",    native_str_lower);
    reg(env, "split",    native_str_split);
    reg(env, "contains", native_str_contains);
    reg(env, "starts",   native_str_starts);
    reg(env, "ends",     native_str_ends);
    reg(env, "slice",    native_str_slice);
    reg(env, "replace",  native_str_replace);
    reg(env, "trim",     native_str_trim);
    reg(env, "join",     native_str_join);
    reg(env, "format",   native_format);

    /* collection */
    reg(env, "push",      native_coll_push);
    reg(env, "pop",       native_coll_pop);
    reg(env, "remove",    native_coll_remove);
    reg(env, "sort",      native_coll_sort);
    reg(env, "reverse",   native_coll_reverse);
    reg(env, "range",     native_coll_range);

    /* type checking */
    reg(env, "typeof",  native_type_of);
    reg(env, "is_int",  native_is_int);
    reg(env, "is_dnum", native_is_dnum);
    reg(env, "is_txt",  native_is_txt);
    reg(env, "is_emp",  native_is_emp);
    reg(env, "is_coll", native_is_coll);

    /* coll membership check -- reuse str contains for coll too */
    reg(env, "has_item",  native_coll_contains);

    /* I/O */
    reg(env, "stamp_ln",    native_stamp_ln);
    reg(env, "read_file",   native_read_file);
    reg(env, "write_file",  native_write_file);
    reg(env, "append_file", native_append_file);

    /* record */
    reg(env, "keys", native_rec_keys);
    reg(env, "vals", native_rec_vals);
    reg(env, "has",  native_rec_has);

    /* system */
    reg(env, "exit",     native_exit_fn);
    reg(env, "time_now", native_time_now);
    reg(env, "sleep",    native_sleep_fn);

    /* constants */
    RivenValue *vtrue  = val_bool(1); env_set(env, "CORRECT",   vtrue,  1); val_release(vtrue);
    RivenValue *vfalse = val_bool(0); env_set(env, "INCORRECT", vfalse, 1); val_release(vfalse);
    RivenValue *vnull  = val_null();  env_set(env, "EMP",       vnull,  1); val_release(vnull);
    RivenValue *vpi    = val_dnum(M_PI); env_set(env, "PI",     vpi,    1); val_release(vpi);
    RivenValue *ve     = val_dnum(M_E);  env_set(env, "E",      ve,     1); val_release(ve);
}
