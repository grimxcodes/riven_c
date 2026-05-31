#include "../include/riven.h"
#include <time.h>
#include <pthread.h>

/* g_signal is defined in lexer.c — extern declared in riven.h */

/* ============================================================
   HELPERS
   ============================================================ */
static char *read_file_str(const char *path) {
    FILE *f = fopen(path,"r"); if (!f) return NULL;
    fseek(f,0,SEEK_END); long sz=ftell(f); rewind(f);
    char *buf=malloc(sz+1); fread(buf,1,sz,f); buf[sz]='\0'; fclose(f);
    return buf;
}

/* ============================================================
   INTERPRETER INIT / FREE
   ============================================================ */
Interpreter *interp_new(void) {
    Interpreter *interp = calloc(1, sizeof(Interpreter));
    interp->global_env = env_new(NULL);
    pthread_mutex_init(&interp->spark_lock, NULL);
    return interp;
}

void interp_free(Interpreter *interp) {
    /* Wait for all spark tasks */
    pthread_mutex_lock(&interp->spark_lock);
    for (int i = 0; i < interp->spark_task_count; i++) {
        SparkTask *t = interp->spark_tasks[i];
        if (t) {
            pthread_join(t->thread, NULL);
            val_release(t->fn);
            for (int j = 0; j < t->arg_count; j++) val_release(t->args[j]);
            if (t->result) val_release(t->result);
            pthread_mutex_destroy(&t->lock);
            free(t);
        }
    }
    pthread_mutex_unlock(&interp->spark_lock);
    pthread_mutex_destroy(&interp->spark_lock);

    for (int i = 0; i < interp->frame_def_count; i++)
        framedef_free(interp->frame_defs[i]);

    env_free(interp->global_env);
    if (interp->return_val)   val_release(interp->return_val);
    if (interp->thrown_error) val_release(interp->thrown_error);
    free(interp);
}

/* ============================================================
   STAMP FORMAT
   ============================================================ */
static char *stamp_format(const char *fmt, RivenValue **args, int argc) {
    char *buf = malloc(MAX_STR_LEN); int bi=0, ai=0;
    for (int i=0; fmt[i] && bi<MAX_STR_LEN-2; i++) {
        if (fmt[i]=='{' && fmt[i+1]=='}') {
            if (ai<argc) {
                RivenValue *av = val_deref(args[ai++]);
                char *s = val_to_string(av);
                for (int j=0; s[j] && bi<MAX_STR_LEN-2; j++) buf[bi++]=s[j];
                free(s);
            }
            i++;
        } else buf[bi++]=fmt[i];
    }
    buf[bi]='\0'; return buf;
}

/* ============================================================
   ACCESS CONTROL HELPERS
   ============================================================ */

/* Strip __open__ or __hidden__ prefix, return pointer into string after prefix.
   Sets *is_hidden to 1 if hidden, 0 if open, -1 if no prefix. */
static const char *strip_access(const char *name, int *is_hidden) {
    if (strncmp(name, "__hidden__", 10) == 0) { *is_hidden=1; return name+10; }
    if (strncmp(name, "__open__",   8)  == 0) { *is_hidden=0; return name+8;  }
    *is_hidden = -1; return name;
}

/* Check if a member name is accessible from outside the frame.
   Returns 1 = accessible, 0 = blocked. */
static int access_check(FrameDef *fd, const char *bare_name, int is_method) {
    int n = is_method ? fd->method_count : fd->field_count;
    const char (*names)[MAX_IDENT_LEN] = is_method ? fd->method_names : fd->field_names;
    const int  *access = is_method ? fd->method_access : fd->field_access;
    for (int i=0; i<n; i++)
        if (strcmp(names[i], bare_name)==0)
            return access[i]; /* 1=open, 0=hidden */
    return 1; /* unlabeled members default open */
}

/* ============================================================
   FORWARD DECLARATION
   ============================================================ */
static RivenValue *exec(Interpreter *interp, ASTNode *node, Env *env);

/* ============================================================
   CALL FUNCTION
   ============================================================ */
static RivenValue *call_function(Interpreter *interp, RivenValue *fn,
                                  RivenValue **args, int argc,
                                  RivenValue *self_obj)
{
    fn = val_deref(fn);
    if (fn->type == VAL_NATIVE_FN) return fn->native_fn(args, argc);
    if (fn->type != VAL_FUNCTION) {
        riven_error(0,"Value is not callable"); return val_error("not callable");
    }

    Env *fn_env = env_new(fn->fn.closure_env ? (Env*)fn->fn.closure_env
                                              : interp->global_env);
    if (self_obj) env_set(fn_env, "self", self_obj, 0);

    for (int i=0; i<fn->fn.param_count; i++) {
        RivenValue *arg = (i<argc) ? args[i] : val_null();
        env_set(fn_env, fn->fn.params[i], arg, 0);
        if (i>=argc) val_release(arg);
    }

    Signal prev_sig = g_signal;
    g_signal = SIGNAL_NONE;
    int prev_fn = interp->in_function;
    interp->in_function = 1;

    RivenValue *result = exec(interp, fn->fn.body, fn_env);

    interp->in_function = prev_fn;

    if (g_signal == SIGNAL_RETURN) {
        g_signal = SIGNAL_NONE;
        RivenValue *rv = interp->return_val; interp->return_val = NULL;
        env_free(fn_env);
        return rv ? rv : val_null();
    }

    env_free(fn_env);
    return result ? result : val_null();
}

/* ============================================================
   SPARK THREAD ENTRY
   ============================================================ */
static void *spark_thread_entry(void *arg) {
    SparkTask *task = (SparkTask*)arg;
    /* Each spark thread gets its own signal state — use a local interp copy */
    /* We re-use the same function value but need a fresh interpreter shell   */
    Interpreter *mini = interp_new();
    /* Copy global env snapshot from closure (read-only — no mutation back) */
    /* The function's closure_env is already captured; we just call it.      */
    RivenValue *result = call_function(mini, task->fn,
                                       task->args, task->arg_count, NULL);
    pthread_mutex_lock(&task->lock);
    task->result = result;
    task->done   = 1;
    pthread_mutex_unlock(&task->lock);
    interp_free(mini);
    return NULL;
}

/* Launch a spark task; returns a VAL_PTR acting as a task handle */
static RivenValue *launch_spark(Interpreter *interp, RivenValue *fn,
                                 RivenValue **args, int argc)
{
    if (interp->spark_task_count >= MAX_SPARK_TASKS) {
        riven_error(0,"Spark task limit reached"); return val_null();
    }
    SparkTask *task = calloc(1, sizeof(SparkTask));
    pthread_mutex_init(&task->lock, NULL);

    val_retain(fn); task->fn = fn;
    task->arg_count = argc;
    for (int i=0; i<argc; i++) { val_retain(args[i]); task->args[i]=args[i]; }
    task->done   = 0;
    task->result = NULL;

    pthread_mutex_lock(&interp->spark_lock);
    interp->spark_tasks[interp->spark_task_count++] = task;
    pthread_mutex_unlock(&interp->spark_lock);

    pthread_create(&task->thread, NULL, spark_thread_entry, task);

    /* Return handle: a PTR with address = task pointer */
    return val_make_ptr((uintptr_t)task, "spark-task", sizeof(SparkTask));
}

/* Sync: wait for all running spark tasks to complete */
static void sync_all_sparks(Interpreter *interp) {
    pthread_mutex_lock(&interp->spark_lock);
    int count = interp->spark_task_count;
    SparkTask **tasks = interp->spark_tasks;
    pthread_mutex_unlock(&interp->spark_lock);

    for (int i=0; i<count; i++) {
        SparkTask *t = tasks[i];
        if (t && !t->done)
            pthread_join(t->thread, NULL);
    }
}

/* ============================================================
   BINARY OPERATION
   ============================================================ */
static RivenValue *eval_binop(const char *op, RivenValue *l, RivenValue *r, int line) {
    l = val_deref(l); r = val_deref(r);
    int  both_int = (l->type==VAL_INT  && r->type==VAL_INT);
    int  any_dnum = (l->type==VAL_DNUM || r->type==VAL_DNUM);
    double lf = (l->type==VAL_DNUM) ? l->dval : (double)l->ival;
    double rf = (r->type==VAL_DNUM) ? r->dval : (double)r->ival;

    if (strcmp(op,"+")==0 && l->type==VAL_STRING) {
        char *rs=val_to_string(r); char buf[MAX_STR_LEN];
        snprintf(buf,MAX_STR_LEN,"%s%s",l->sval,rs); free(rs); return val_string(buf);
    }
    if (strcmp(op,"+")==0) { if(both_int) return val_int(l->ival+r->ival); if(any_dnum) return val_dnum(lf+rf); }
    if (strcmp(op,"-")==0) { if(both_int) return val_int(l->ival-r->ival); if(any_dnum) return val_dnum(lf-rf); }
    if (strcmp(op,"*")==0) {
        if (both_int) return val_int(l->ival*r->ival);
        if (any_dnum) return val_dnum(lf*rf);
        if (l->type==VAL_STRING && r->type==VAL_INT) {
            char buf[MAX_STR_LEN]=""; int sl=strlen(l->sval);
            for (int i=0;i<r->ival && (i+1)*sl<MAX_STR_LEN;i++) strncat(buf,l->sval,MAX_STR_LEN-strlen(buf)-1);
            return val_string(buf);
        }
    }
    if (strcmp(op,"/")==0) {
        if (rf==0.0) { riven_error(line,"Division by zero"); return val_error("div by zero"); }
        if (both_int) return val_int(l->ival/r->ival);
        if (any_dnum) return val_dnum(lf/rf);
    }
    if (strcmp(op,"%")==0) {
        if (both_int) { if(!r->ival) return val_error("mod by zero"); return val_int(l->ival%r->ival); }
        return val_dnum(fmod(lf,rf));
    }
    if (strcmp(op,"==")==0) return val_bool(val_equal(l,r));
    if (strcmp(op,"!=")==0) return val_bool(!val_equal(l,r));
    if (strcmp(op,"<") ==0) {
        if(both_int) return val_bool(l->ival<r->ival);
        if(any_dnum) return val_bool(lf<rf);
        if(l->type==VAL_STRING&&r->type==VAL_STRING) return val_bool(strcmp(l->sval,r->sval)<0);
    }
    if (strcmp(op,">")==0) {
        if(both_int) return val_bool(l->ival>r->ival);
        if(any_dnum) return val_bool(lf>rf);
        if(l->type==VAL_STRING&&r->type==VAL_STRING) return val_bool(strcmp(l->sval,r->sval)>0);
    }
    if (strcmp(op,"<=")==0) { if(both_int) return val_bool(l->ival<=r->ival); if(any_dnum) return val_bool(lf<=rf); }
    if (strcmp(op,">=")==0) { if(both_int) return val_bool(l->ival>=r->ival); if(any_dnum) return val_bool(lf>=rf); }
    if (strcmp(op,"and")==0) return val_bool(val_truthy(l)&&val_truthy(r));
    if (strcmp(op,"or") ==0) return val_bool(val_truthy(l)||val_truthy(r));

    riven_error(line,"Unknown operator '%s'",op);
    return val_null();
}

/* ============================================================
   BUILD FRAME INSTANCE  (shared by spawn + User() call)
   ============================================================ */
static RivenValue *build_frame_instance(Interpreter *interp, FrameDef *fd,
                                         RivenValue **args, int argc, Env *env)
{
    RivenValue *obj = calloc(1, sizeof(RivenValue));
    obj->type      = VAL_FRAME_OBJ;
    obj->ref_count = 1;
    strncpy(obj->frame_obj.frame_name, fd->name, MAX_IDENT_LEN-1);
    record_init(&obj->frame_obj.fields);
    record_init(&obj->frame_obj.methods);
    obj->frame_obj.open_method_count = 0;

    /* Copy default fields from FrameDef */
    for (int i=0; i<fd->fields.count; i++) {
        RivenValue *fv = val_copy(fd->fields.vals[i]);
        record_set(&obj->frame_obj.fields, fd->fields.keys[i], fv);
        val_release(fv);
    }
    /* Copy methods */
    for (int i=0; i<fd->methods.count; i++)
        record_set(&obj->frame_obj.methods, fd->methods.keys[i], fd->methods.vals[i]);

    /* Track open method names on the instance for external access checking */
    for (int i=0; i<fd->method_count; i++) {
        if (fd->method_access[i]==1) {
            strncpy(obj->frame_obj.open_methods[obj->frame_obj.open_method_count++],
                    fd->method_names[i], MAX_IDENT_LEN-1);
        }
    }

    /* Call __boot__ constructor with arguments */
    RivenValue *boot_fn = record_get(&obj->frame_obj.methods, "__boot__");
    if (boot_fn) {
        call_function(interp, boot_fn, args, argc, obj);
        g_signal = SIGNAL_NONE;
    }
    return obj;
}

/* ============================================================
   ACCESS ENFORCEMENT  (called before every external member access)
   ============================================================ */
/* Returns 1 if access allowed, 0 if blocked (and emits error) */
static int enforce_access(Interpreter *interp, RivenValue *obj,
                           const char *member_name, int line,
                           int is_self_call)
{
    /* self.anything always allowed */
    if (is_self_call) return 1;

    FrameDef *fd = interp_find_frame(interp, obj->frame_obj.frame_name);
    if (!fd) return 1; /* unknown frame → be permissive */

    /* Check methods */
    for (int i=0; i<fd->method_count; i++) {
        if (strcmp(fd->method_names[i], member_name)==0) {
            if (fd->method_access[i]==0) {
                riven_error(line,"Access denied: '%s' is hidden on frame '%s'",
                            member_name, obj->frame_obj.frame_name);
                return 0;
            }
            return 1;
        }
    }
    /* Check fields */
    for (int i=0; i<fd->field_count; i++) {
        if (strcmp(fd->field_names[i], member_name)==0) {
            if (fd->field_access[i]==0) {
                riven_error(line,"Access denied: field '%s' is hidden on frame '%s'",
                            member_name, obj->frame_obj.frame_name);
                return 0;
            }
            return 1;
        }
    }
    return 1; /* unlabeled → open */
}

/* ============================================================
   MAIN EXEC
   ============================================================ */
static RivenValue *exec(Interpreter *interp, ASTNode *node, Env *env) {
    if (!node) return val_null();
    if (g_signal!=SIGNAL_NONE && g_signal!=SIGNAL_ERROR) return val_null();

    switch (node->type) {

    /* ---- Literals ---- */
    case NODE_INT_LIT:  return val_int(node->ival);
    case NODE_DNUM_LIT: return val_dnum(node->dval);
    case NODE_STR_LIT:  return val_string(node->sval);
    case NODE_BOOL_LIT: return val_bool(node->bval);
    case NODE_NULL_LIT: return val_null();

    /* ---- Identifier ---- */
    case NODE_IDENT: {
        RivenValue *v = env_get(env, node->sval);
        if (!v) { riven_error(node->line,"Undefined variable '%s'",node->sval); return val_null(); }
        /* Return the raw binding — val_deref happens at use site for refs,
           so callers that need the box itself (assignment) can still get it. */
        val_retain(v); return v;
    }

    /* ---- BinOp ---- */
    case NODE_BINOP: {
        /* Compound assignment: obj.field = val  or  arr[i] = val */
        if (strcmp(node->binop.op,"=")==0) {
            RivenValue *rval = exec(interp, node->binop.right, env);
            ASTNode *lhs = node->binop.left;
            if (lhs->type==NODE_MEMBER_ACCESS) {
                RivenValue *obj = exec(interp, lhs->member.obj, env);
                RivenValue *dobj = val_deref(obj);
                if (dobj->type==VAL_FRAME_OBJ) {
                    /* Check write access */
                    FrameDef *fd = interp_find_frame(interp, dobj->frame_obj.frame_name);
                    if (fd) {
                        int is_self = 0;
                        RivenValue *self_v = env_get(env,"self");
                        if (self_v && val_deref(self_v)==dobj) is_self=1;
                        if (!is_self) {
                            int hidden=0;
                            for (int i=0;i<fd->field_count;i++)
                                if (strcmp(fd->field_names[i],lhs->member.member)==0)
                                { hidden=!fd->field_access[i]; break; }
                            if (hidden) {
                                riven_error(node->line,"Access denied: field '%s' is hidden",lhs->member.member);
                                val_release(obj); val_release(rval); return val_null();
                            }
                        }
                    }
                    record_set(&dobj->frame_obj.fields, lhs->member.member, rval);
                } else if (dobj->type==VAL_RECORD) {
                    record_set(&dobj->record, lhs->member.member, rval);
                }
                val_release(obj);
            } else if (lhs->type==NODE_INDEX) {
                RivenValue *obj = exec(interp, lhs->index.obj, env);
                RivenValue *idx = exec(interp, lhs->index.idx, env);
                RivenValue *dobj = val_deref(obj);
                if (dobj->type==VAL_COLL) {
                    int i=(int)idx->ival;
                    if (i<0) i=dobj->coll.count+i;
                    if (i>=0 && i<dobj->coll.count) {
                        val_retain(rval); val_release(dobj->coll.items[i]); dobj->coll.items[i]=rval;
                    } else riven_error(node->line,"Index out of bounds");
                }
                val_release(obj); val_release(idx);
            } else if (lhs->type==NODE_IDENT) {
                /* Plain name = val: if existing binding is a REF, write through */
                RivenValue *existing = env_get(env, lhs->sval);
                if (existing && existing->type==VAL_REF) {
                    val_ref_set(existing, rval);
                } else {
                    env_update(env, lhs->sval, rval);
                }
            }
            return rval;
        }

        RivenValue *l = exec(interp, node->binop.left, env);
        /* Short-circuit */
        if (strcmp(node->binop.op,"and")==0 && !val_truthy(l)) return l;
        if (strcmp(node->binop.op,"or") ==0 &&  val_truthy(l)) return l;
        RivenValue *r = exec(interp, node->binop.right, env);
        RivenValue *res = eval_binop(node->binop.op, l, r, node->line);
        val_release(l); val_release(r);
        return res;
    }

    /* ---- UnOp ---- */
    case NODE_UNOP: {
        RivenValue *v = val_deref(exec(interp, node->unop.operand, env));
        RivenValue *res;
        if (strcmp(node->unop.op,"-")==0) {
            if (v->type==VAL_INT) res=val_int(-v->ival);
            else if (v->type==VAL_DNUM) res=val_dnum(-v->dval);
            else res=val_error("Unary - on non-number");
        } else if (strcmp(node->unop.op,"!")==0||strcmp(node->unop.op,"not")==0) {
            res=val_bool(!val_truthy(v));
        } else res=val_null();
        val_release(v); return res;
    }

    /* ---- Assign / Firm ---- */
    case NODE_ASSIGN:
    case NODE_FIRM: {
        RivenValue *val = exec(interp, node->assign.value, env);
        /* If existing binding is a REF, write through the box */
        RivenValue *existing = env_get(env, node->assign.name);
        if (existing && existing->type==VAL_REF && !node->assign.is_firm) {
            val_ref_set(existing, val);
        } else if (env_exists(env, node->assign.name)) {
            env_update(env, node->assign.name, val);
        } else {
            env_set(env, node->assign.name, val, node->assign.is_firm);
        }
        return val;
    }

    /* ---- REF — creates a mutable shared alias ---- */
    case NODE_REF: {
        /* Evaluate RHS */
        RivenValue *rhs = exec(interp, node->refptr.value, env);
        RivenValue *box;
        if (rhs->type == VAL_REF) {
            /* alias: point at same box */
            box = val_alias_ref(rhs);
        } else {
            /* new box wrapping the value */
            box = val_make_ref(rhs);
        }
        val_release(rhs);
        /* Bind the VAL_REF box to the name */
        env_set(env, node->refptr.name, box, 0);
        val_release(box);
        return env_get(env, node->refptr.name);
    }

    /* ---- PTR — raw pointer descriptor ---- */
    case NODE_PTR: {
        RivenValue *rhs = exec(interp, node->refptr.value, env);
        RivenValue *rhs_d = val_deref(rhs);
        RivenValue *ptr_val;

        if (rhs_d->type == VAL_PTR) {
            /* Already a ptr: alias it */
            ptr_val = val_make_ptr(rhs_d->ptr.address, rhs_d->ptr.tag, rhs_d->ptr.size);
        } else if (rhs_d->type == VAL_INT) {
            /* Integer as address */
            if (!interp->raw_mode) {
                riven_error(node->line,
                    "Cannot create ptr from integer outside raw block "
                    "(use raw { ptr %s = %lld })",
                    node->refptr.name, rhs_d->ival);
                val_release(rhs);
                return val_null();
            }
            ptr_val = val_make_ptr((uintptr_t)rhs_d->ival, node->refptr.name, 0);
        } else if (rhs_d->type == VAL_FRAME_OBJ || rhs_d->type == VAL_RECORD ||
                   rhs_d->type == VAL_COLL) {
            /* Pointer to Riven object */
            ptr_val = val_make_ptr((uintptr_t)rhs_d, node->refptr.name, sizeof(RivenValue));
            ptr_val->ptr.is_valid = 1;
        } else {
            /* Symbolic tag (e.g. ptr memory = kernel) */
            const char *tag = (rhs_d->type==VAL_STRING) ? rhs_d->sval : node->refptr.name;
            ptr_val = val_make_ptr(0, tag, 0);
            /* Symbolic ptrs are only valid inside raw mode */
            ptr_val->ptr.is_valid = interp->raw_mode ? 1 : 0;
        }

        val_release(rhs);
        env_set(env, node->refptr.name, ptr_val, 0);
        val_release(ptr_val);
        return env_get(env, node->refptr.name);
    }

    /* ---- BIND — synonym for ref, kept for spec compat ---- */
    case NODE_BIND: {
        RivenValue *rhs = exec(interp, node->refptr.value, env);
        RivenValue *box = (rhs->type==VAL_REF) ? val_alias_ref(rhs) : val_make_ref(rhs);
        val_release(rhs);
        env_set(env, node->refptr.name, box, 0);
        val_release(box);
        return env_get(env, node->refptr.name);
    }

    /* ---- Program / Block ---- */
    case NODE_PROGRAM:
    case NODE_BLOCK: {
        Env *block_env = (node->type==NODE_BLOCK) ? env_new(env) : env;
        RivenValue *last = val_null();
        for (int i=0; i<node->block.stmts.count; i++) {
            if (g_signal!=SIGNAL_NONE) break;
            val_release(last);
            last = exec(interp, node->block.stmts.items[i], block_env);
        }
        if (node->type==NODE_BLOCK) env_free(block_env);
        return last;
    }

    /* ---- Stamp ---- */
    case NODE_STAMP: {
        RivenValue *fmt_v = exec(interp, node->stamp.fmt, env);
        RivenValue *args[MAX_ARGS];
        for (int i=0;i<node->stamp.arg_count;i++)
            args[i]=exec(interp, node->stamp.args[i], env);

        RivenValue *dfmt = val_deref(fmt_v);
        if (dfmt->type==VAL_STRING) {
            char *out=stamp_format(dfmt->sval, args, node->stamp.arg_count);
            printf("%s\n",out); free(out);
        } else { val_print(dfmt); printf("\n"); }

        val_release(fmt_v);
        for (int i=0;i<node->stamp.arg_count;i++) val_release(args[i]);
        return val_null();
    }

    /* ---- Grab ---- */
    case NODE_GRAB: {
        RivenValue *pr=exec(interp, node->grab.prompt, env);
        char *ps=val_to_string(val_deref(pr)); printf("%s",ps); free(ps); val_release(pr);
        fflush(stdout);
        char buf[MAX_STR_LEN]; if (!fgets(buf,MAX_STR_LEN,stdin)) return val_string("");
        int l=strlen(buf); if (l>0 && buf[l-1]=='\n') buf[l-1]='\0';
        return val_string(buf);
    }

    /* ---- Fetch ---- */
    case NODE_FETCH: {
        RivenValue *url=exec(interp, node->fetch.url, env);
        char *us=val_to_string(val_deref(url)); val_release(url);
        char cmd[MAX_STR_LEN];
        snprintf(cmd,MAX_STR_LEN,"curl -s --max-time 10 \"%s\" 2>/dev/null",us);
        free(us);
        FILE *fp=popen(cmd,"r");
        if (!fp) return val_string("{\"error\":\"fetch not available\"}");
        char buf[MAX_STR_LEN]; int bi=0,c;
        while ((c=fgetc(fp))!=EOF && bi<MAX_STR_LEN-1) buf[bi++]=(char)c;
        buf[bi]='\0'; pclose(fp);
        return val_string(bi>0?buf:"{\"error\":\"empty response\"}");
    }

    /* ---- Cast ---- */
    case NODE_CAST: {
        RivenValue *v=val_deref(exec(interp, node->cast.value, env));
        RivenValue *res;
        if (strcmp(node->cast.target_type,"int")==0) {
            if (v->type==VAL_INT)    res=val_int(v->ival);
            else if(v->type==VAL_DNUM)  res=val_int((long long)v->dval);
            else if(v->type==VAL_STRING) res=val_int(atoll(v->sval));
            else if(v->type==VAL_BOOL)   res=val_int(v->bval);
            else res=val_int(0);
        } else if (strcmp(node->cast.target_type,"dnum")==0) {
            if (v->type==VAL_DNUM)   res=val_dnum(v->dval);
            else if(v->type==VAL_INT)   res=val_dnum((double)v->ival);
            else if(v->type==VAL_STRING) res=val_dnum(atof(v->sval));
            else res=val_dnum(0.0);
        } else if (strcmp(node->cast.target_type,"txt")==0) {
            char *s=val_to_string(v); res=val_string(s); free(s);
        } else res=val_null();
        val_release(v); return res;
    }

    /* ---- If ---- */
    case NODE_IF: {
        RivenValue *cond=exec(interp, node->ifstmt.cond, env);
        int t=val_truthy(cond); val_release(cond);
        if (t) return exec(interp, node->ifstmt.body, env);
        if (node->ifstmt.altifs) return exec(interp, node->ifstmt.altifs, env);
        if (node->ifstmt.els)    return exec(interp, node->ifstmt.els, env);
        return val_null();
    }

    /* ---- Flow ---- */
    case NODE_FLOW: {
        RivenValue *cnt=val_deref(exec(interp, node->flow.count, env));
        long long n=(cnt->type==VAL_INT)?cnt->ival:(long long)cnt->dval;
        val_release(cnt);
        RivenValue *last=val_null();
        int prev=interp->in_loop; interp->in_loop=1;
        for (long long i=0;i<n;i++) {
            if (g_signal==SIGNAL_BREAK) { g_signal=SIGNAL_NONE; break; }
            if (g_signal==SIGNAL_CONTINUE) { g_signal=SIGNAL_NONE; continue; }
            if (g_signal==SIGNAL_RETURN) break;
            val_release(last);
            last=exec(interp, node->flow.body, env);
        }
        interp->in_loop=prev; return last;
    }

    /* ---- During ---- */
    case NODE_DURING: {
        RivenValue *last=val_null();
        int prev=interp->in_loop; interp->in_loop=1;
        while (1) {
            if (g_signal==SIGNAL_BREAK) { g_signal=SIGNAL_NONE; break; }
            if (g_signal==SIGNAL_RETURN) break;
            RivenValue *cond=exec(interp, node->during.cond, env);
            int t=val_truthy(cond); val_release(cond);
            if (!t) break;
            if (g_signal==SIGNAL_CONTINUE) { g_signal=SIGNAL_NONE; continue; }
            val_release(last);
            last=exec(interp, node->during.body, env);
        }
        interp->in_loop=prev; return last;
    }

    /* ---- Inc / Dec / Rise / Drop ---- */
    case NODE_INC: case NODE_RISE: {
        RivenValue *v=env_get(env, node->incdec.name);
        if (!v) { riven_error(node->line,"Undefined '%s'",node->incdec.name); return val_null(); }
        RivenValue *dv=val_deref(v);
        RivenValue *nv=(dv->type==VAL_INT)?val_int(dv->ival+1):val_dnum(dv->dval+1.0);
        if (v->type==VAL_REF) val_ref_set(v,nv);
        else env_update(env, node->incdec.name, nv);
        return nv;
    }
    case NODE_DEC: case NODE_DROP: {
        RivenValue *v=env_get(env, node->incdec.name);
        if (!v) { riven_error(node->line,"Undefined '%s'",node->incdec.name); return val_null(); }
        RivenValue *dv=val_deref(v);
        RivenValue *nv=(dv->type==VAL_INT)?val_int(dv->ival-1):val_dnum(dv->dval-1.0);
        if (v->type==VAL_REF) val_ref_set(v,nv);
        else env_update(env, node->incdec.name, nv);
        return nv;
    }

    /* ---- Craft ---- */
    case NODE_CRAFT: {
        RivenValue *fn=val_alloc_pub(VAL_FUNCTION);
        strncpy(fn->fn.name, node->craft.name, MAX_IDENT_LEN-1);
        fn->fn.param_count=node->craft.param_count;
        for (int i=0;i<node->craft.param_count;i++)
            strncpy(fn->fn.params[i], node->craft.params[i], MAX_IDENT_LEN-1);
        fn->fn.body=node->craft.body;
        fn->fn.closure_env=env;
        fn->fn.is_spark=node->craft.is_spark;
        env_set(env, node->craft.name, fn, 0);
        val_release(fn);
        return val_null();
    }

    /* ---- Return ---- */
    case NODE_RETURN: {
        RivenValue *rv=node->ret.value ? exec(interp,node->ret.value,env) : val_null();
        if (interp->return_val) val_release(interp->return_val);
        interp->return_val=rv; val_retain(rv);
        g_signal=SIGNAL_RETURN;
        return rv;
    }

    /* ---- Call ---- */
    case NODE_CALL: {
        RivenValue *self_obj=NULL;
        RivenValue *callee=NULL;

        if (node->call.callee->type==NODE_MEMBER_ACCESS) {
            self_obj = exec(interp, node->call.callee->member.obj, env);
            RivenValue *dself = val_deref(self_obj);
            const char *mname = node->call.callee->member.member;

            if (dself->type==VAL_FRAME_OBJ) {
                /* Access control check — determine if we're inside self */
                int is_self = 0;
                RivenValue *sv = env_get(env,"self");
                if (sv && val_deref(sv)==dself) is_self=1;

                if (!enforce_access(interp, dself, mname, node->line, is_self)) {
                    val_release(self_obj); return val_error("access denied");
                }
                callee=record_get(&dself->frame_obj.methods, mname);
                if (!callee) {
                    riven_error(node->line,"Method '%s' not found on '%s'",
                                mname, dself->frame_obj.frame_name);
                    val_release(self_obj); return val_error("method not found");
                }
                val_retain(callee);
                self_obj = dself; val_retain(self_obj);

            } else if (dself->type==VAL_RECORD) {
                callee=record_get(&dself->record, mname);
                if (!callee) { riven_error(node->line,"Key '%s' not found",mname); val_release(self_obj); return val_null(); }
                val_retain(callee);
            } else if (dself->type==VAL_COLL) {
                /* Built-in coll methods */
                RivenValue *cargs[MAX_ARGS]; int cargc=0;
                for (int i=0;i<node->call.arg_count;i++) cargs[cargc++]=exec(interp,node->call.args[i],env);
                RivenValue *res=val_null();
                if (strcmp(mname,"push")==0&&cargc>=1) { val_coll_push(dself,cargs[0]); }
                else if (strcmp(mname,"pop")==0) { if (dself->coll.count>0) { val_release(res); res=dself->coll.items[--dself->coll.count]; val_retain(res); } }
                else if (strcmp(mname,"len")==0) { val_release(res); res=val_int(dself->coll.count); }
                else riven_error(node->line,"Unknown coll method '%s'",mname);
                val_release(self_obj);
                for (int i=0;i<cargc;i++) val_release(cargs[i]);
                return res;
            } else if (dself->type==VAL_PTR) {
                /* ptr method calls in raw mode */
                if (!interp->raw_mode) {
                    riven_error(node->line,"Cannot call method on ptr outside raw block");
                    val_release(self_obj); return val_null();
                }
                RivenValue *cargs[MAX_ARGS]; int cargc=0;
                for (int i=0;i<node->call.arg_count;i++) cargs[cargc++]=exec(interp,node->call.args[i],env);
                RivenValue *res=val_null();
                if (strcmp(mname,"read")==0 && dself->ptr.is_valid && dself->ptr.address) {
                    size_t sz=(cargc>=1)?cargs[0]->ival:dself->ptr.size;
                    val_release(res); res=raw_read_mem(dself->ptr.address,sz?sz:8);
                } else if (strcmp(mname,"write")==0 && dself->ptr.is_valid && dself->ptr.address && cargc>=1) {
                    RivenValue *darg=val_deref(cargs[0]);
                    uint64_t wv=(darg->type==VAL_INT)?(uint64_t)darg->ival:0;
                    raw_write_mem(dself->ptr.address, dself->ptr.size?dself->ptr.size:8, wv);
                } else if (strcmp(mname,"addr")==0) {
                    val_release(res); res=val_int((long long)dself->ptr.address);
                } else if (strcmp(mname,"tag")==0) {
                    val_release(res); res=val_string(dself->ptr.tag);
                } else if (strcmp(mname,"valid")==0) {
                    val_release(res); res=val_bool(dself->ptr.is_valid);
                } else riven_error(node->line,"Unknown ptr method '%s'",mname);
                val_release(self_obj);
                for (int i=0;i<cargc;i++) val_release(cargs[i]);
                return res;
            } else {
                riven_error(node->line,"Cannot call method on this type");
                val_release(self_obj); return val_null();
            }

        } else {
            callee = exec(interp, node->call.callee, env);
        }

        /* Evaluate args */
        RivenValue *args[MAX_ARGS]; int argc=0;
        for (int i=0;i<node->call.arg_count;i++)
            args[argc++]=exec(interp, node->call.args[i], env);

        /* Check if callee is a FRAME TEMPLATE (User() without spawn) */
        RivenValue *dcallee = val_deref(callee);
        if (dcallee->type==VAL_RECORD && node->call.callee->type==NODE_IDENT) {
            /* Look for a matching FrameDef */
            FrameDef *fd = interp_find_frame(interp, node->call.callee->sval);
            if (fd) {
                RivenValue *inst = build_frame_instance(interp, fd, args, argc, env);
                val_release(callee);
                if (self_obj) val_release(self_obj);
                for (int i=0;i<argc;i++) val_release(args[i]);
                return inst;
            }
        }

        /* Spark functions: launch as background thread */
        if (dcallee->type==VAL_FUNCTION && dcallee->fn.is_spark) {
            RivenValue *handle = launch_spark(interp, dcallee, args, argc);
            val_release(callee);
            if (self_obj) val_release(self_obj);
            for (int i=0;i<argc;i++) val_release(args[i]);
            return handle;
        }

        RivenValue *result = call_function(interp, dcallee, args, argc, self_obj);

        val_release(callee);
        if (self_obj) val_release(self_obj);
        for (int i=0;i<argc;i++) val_release(args[i]);
        return result ? result : val_null();
    }

    /* ---- Member Access ---- */
    case NODE_MEMBER_ACCESS: {
        RivenValue *obj=exec(interp, node->member.obj, env);
        RivenValue *dobj=val_deref(obj);
        const char *mem=node->member.member;
        RivenValue *res=NULL;

        if (dobj->type==VAL_FRAME_OBJ) {
            /* Determine if self */
            int is_self=0;
            RivenValue *sv=env_get(env,"self");
            if (sv && val_deref(sv)==dobj) is_self=1;

            if (!enforce_access(interp, dobj, mem, node->line, is_self)) {
                val_release(obj); return val_null();
            }
            res=record_get(&dobj->frame_obj.fields, mem);
            if (!res) res=record_get(&dobj->frame_obj.methods, mem);
        } else if (dobj->type==VAL_RECORD) {
            res=record_get(&dobj->record, mem);
        } else if (dobj->type==VAL_COLL) {
            if (strcmp(mem,"len")==0) { val_release(obj); return val_int(dobj->coll.count); }
        } else if (dobj->type==VAL_PTR) {
            if (strcmp(mem,"addr")==0)  { val_release(obj); return val_int((long long)dobj->ptr.address); }
            if (strcmp(mem,"tag")==0)   { val_release(obj); return val_string(dobj->ptr.tag); }
            if (strcmp(mem,"valid")==0) { val_release(obj); return val_bool(dobj->ptr.is_valid); }
            if (strcmp(mem,"size")==0)  { val_release(obj); return val_int((long long)dobj->ptr.size); }
        }

        if (!res) { riven_error(node->line,"No field '%s'",mem); val_release(obj); return val_null(); }
        val_retain(res);
        val_release(obj);
        return res;
    }

    /* ---- Index ---- */
    case NODE_INDEX: {
        RivenValue *obj=exec(interp, node->index.obj, env);
        RivenValue *idx=exec(interp, node->index.idx, env);
        RivenValue *dobj=val_deref(obj);
        RivenValue *didx=val_deref(idx);
        RivenValue *res=NULL;

        if (dobj->type==VAL_COLL) {
            int i=(int)didx->ival;
            if (i<0) i=dobj->coll.count+i;
            if (i>=0&&i<dobj->coll.count) { res=dobj->coll.items[i]; val_retain(res); }
            else { riven_error(node->line,"Index %d out of bounds (len=%d)",i,dobj->coll.count); res=val_null(); }
        } else if (dobj->type==VAL_STRING) {
            int i=(int)didx->ival;
            if (i>=0&&i<(int)strlen(dobj->sval)) { char ch[2]={dobj->sval[i],0}; res=val_string(ch); }
            else res=val_null();
        } else if (dobj->type==VAL_RECORD) {
            if (didx->type==VAL_STRING) res=record_get(&dobj->record, didx->sval);
            if (res) val_retain(res); else res=val_null();
        } else { riven_error(node->line,"Cannot index this type"); res=val_null(); }

        val_release(obj); val_release(idx);
        return res;
    }

    /* ---- Collection literal ---- */
    case NODE_COLL_LITERAL: {
        RivenValue *coll=val_coll_empty();
        for (int i=0;i<node->coll.items.count;i++) {
            RivenValue *item=exec(interp, node->coll.items.items[i], env);
            val_coll_push(coll,item); val_release(item);
        }
        return coll;
    }

    /* ---- Record literal ---- */
    case NODE_REC_LITERAL: {
        RivenValue *rec=val_record_empty();
        for (int i=0;i<node->rec.count;i++) {
            RivenValue *v=exec(interp, node->rec.vals[i], env);
            val_record_set(rec, node->rec.keys[i], v);
            val_release(v);
        }
        return rec;
    }

    /* ---- Frame definition ---- */
    case NODE_FRAME: {
        FrameDef *fd = framedef_new(node->frame.name);

        for (int i=0; i<node->frame.members.count; i++) {
            ASTNode *m = node->frame.members.items[i];

            if (m->type == NODE_CRAFT) {
                int acc_hidden; /* -1=unlabeled,0=hidden,1=open */
                const char *bare = strip_access(m->craft.name, &acc_hidden);
                int is_open = (acc_hidden == -1) ? 1 : (acc_hidden == 0 ? 0 : 1);

                /* Build function value using the BARE name */
                RivenValue *fn = val_alloc_pub(VAL_FUNCTION);
                strncpy(fn->fn.name, bare, MAX_IDENT_LEN-1);
                fn->fn.param_count = m->craft.param_count;
                for (int j=0;j<m->craft.param_count;j++)
                    strncpy(fn->fn.params[j], m->craft.params[j], MAX_IDENT_LEN-1);
                fn->fn.body = m->craft.body;
                fn->fn.closure_env = env;

                /* Store under bare name */
                record_set(&fd->methods, bare, fn);
                val_release(fn);

                /* Register access metadata */
                if (fd->method_count < MAX_ARGS) {
                    strncpy(fd->method_names[fd->method_count], bare, MAX_IDENT_LEN-1);
                    fd->method_access[fd->method_count] = is_open;
                    fd->method_count++;
                }

                /* If this is boot, also store boot param names for spawn */
                if (strcmp(bare,"__boot__")==0) {
                    fd->boot_param_count = m->craft.param_count;
                    for (int j=0;j<m->craft.param_count;j++)
                        strncpy(fd->boot_params[j], m->craft.params[j], MAX_IDENT_LEN-1);
                }

            } else if (m->type==NODE_ASSIGN || m->type==NODE_FIRM) {
                int acc_hidden;
                const char *bare = strip_access(m->assign.name, &acc_hidden);
                int is_open = (acc_hidden == -1) ? 1 : (acc_hidden == 0 ? 0 : 1);

                RivenValue *fv = exec(interp, m->assign.value, env);
                record_set(&fd->fields, bare, fv);
                val_release(fv);

                if (fd->field_count < MAX_ARGS) {
                    strncpy(fd->field_names[fd->field_count], bare, MAX_IDENT_LEN-1);
                    fd->field_access[fd->field_count] = is_open;
                    fd->field_count++;
                }
            }
        }

        interp_add_frame(interp, fd);

        /* Also register a callable "constructor proxy" in env so User() works */
        /* We store a record acting as a marker; exec(NODE_CALL) checks framedef */
        RivenValue *proxy = val_record_empty();
        record_set(&proxy->record, "__is_frame__", val_bool(1));
        env_set(env, node->frame.name, proxy, 0);
        val_release(proxy);

        return val_null();
    }

    /* ---- Spawn ---- */
    case NODE_SPAWN: {
        FrameDef *fd = interp_find_frame(interp, node->spawn.frame_name);
        if (!fd) {
            riven_error(node->line,"Unknown frame '%s'",node->spawn.frame_name);
            return val_null();
        }
        RivenValue *args[MAX_ARGS]; int argc=0;
        for (int i=0;i<node->spawn.arg_count;i++)
            args[argc++]=exec(interp, node->spawn.args[i], env);
        RivenValue *obj = build_frame_instance(interp, fd, args, argc, env);
        for (int i=0;i<argc;i++) val_release(args[i]);
        return obj;
    }

    /* ---- Consistof ---- */
    case NODE_CONSISTOF: {
        char *src=read_file_str(node->consistof.path);
        if (!src) { riven_error(node->line,"Cannot import '%s'",node->consistof.path); return val_null(); }
        Lexer *lx=lexer_new(src); lexer_tokenize(lx);
        Parser *ps=parser_new(lx->tokens, lx->token_count);
        ASTNode *prog=parser_parse(ps);
        exec(interp, prog, env);
        ast_free(prog); parser_free(ps); lexer_free(lx); free(src);
        return val_null();
    }

    /* ---- Attack ---- */
    case NODE_ATTACK: {
        RivenValue *msg=exec(interp, node->attack.msg, env);
        char *ms=val_to_string(val_deref(msg)); val_release(msg);
        fprintf(stderr,"\033[1;31m[RIVEN ATTACK] %s\033[0m\n",ms);
        if (interp->thrown_error) val_release(interp->thrown_error);
        interp->thrown_error=val_error(ms);
        interp->has_error=1; g_signal=SIGNAL_ERROR;
        free(ms); return val_null();
    }

    /* ---- Resc ---- */
    case NODE_RESC: {
        g_signal=SIGNAL_NONE; interp->has_error=0;
        RivenValue *res=exec(interp, node->resc.body, env);
        if (g_signal==SIGNAL_ERROR) {
            g_signal=SIGNAL_NONE; interp->has_error=0;
            if (interp->thrown_error) {
                /* Expose the error message as 'err' inside the resc scope */
                fprintf(stderr,"\033[1;33m[RIVEN RESC] Caught: %s\033[0m\n",
                        interp->thrown_error->error_msg);
                val_release(interp->thrown_error); interp->thrown_error=NULL;
            }
        }
        return res;
    }

    /* ---- Spark (background function call) ---- */
    case NODE_SPARK:
        /* If we reach here as a bare node (not a call-site), it's a no-op.
           Real spark dispatch happens in NODE_CALL when fn->is_spark==1.     */
        return val_null();

    /* ---- Sync (barrier — wait for ALL spark tasks) ---- */
    case NODE_SYNC:
        sync_all_sparks(interp);
        return val_null();

    /* ---- Raw block ---- */
    case NODE_RAW: {
        /* Enter unsafe mode: ptr derefs, integer-to-ptr casts all allowed */
        int prev_raw = interp->raw_mode;
        interp->raw_mode = 1;
        fprintf(stderr,"\033[0;90m[raw] Entering unsafe mode\033[0m\n");
        RivenValue *res = exec(interp, node->raw.body, env);
        interp->raw_mode = prev_raw;
        fprintf(stderr,"\033[0;90m[raw] Exiting unsafe mode\033[0m\n");
        return res;
    }

    default:
        riven_error(node->line,"Unknown AST node type %d",node->type);
        return val_null();
    }
}

/* ============================================================
   PUBLIC API
   ============================================================ */
void interp_register_natives(Interpreter *interp, Env *env) {
    (void)interp;
    stdlib_register(env);
}

RivenValue *interp_exec(Interpreter *interp, ASTNode *node, Env *env) {
    return exec(interp, node, env);
}

void interp_run_file(Interpreter *interp, const char *path) {
    char *src=read_file_str(path);
    if (!src) { fprintf(stderr,"Cannot open file: %s\n",path); return; }
    Lexer *lx=lexer_new(src); lexer_tokenize(lx);
    Parser *ps=parser_new(lx->tokens, lx->token_count);
    ASTNode *prog=parser_parse(ps);
    interp_register_natives(interp, interp->global_env);
    exec(interp, prog, interp->global_env);
    ast_free(prog); parser_free(ps); lexer_free(lx); free(src);
}
