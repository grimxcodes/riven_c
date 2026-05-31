#include "../include/riven.h"
#include <time.h>
#include <libgen.h>   /* dirname */
#include <limits.h>   /* realpath */

/* ========================= FILE HELPERS ========================= */
static char *read_file(const char *path) {
    FILE *f=fopen(path,"r"); if(!f) return NULL;
    fseek(f,0,SEEK_END); long sz=ftell(f); rewind(f);
    char *buf=malloc(sz+1);
    if (fread(buf,1,sz,f) != (size_t)sz) { /* best-effort */ }
    buf[sz]='\0'; fclose(f); return buf;
}

/* Resolve path relative to a base file */
static void resolve_path(const char *base_file, const char *rel,
                         char *out, size_t outsz) {
    if (rel[0]=='/') {
        /* absolute */
        strncpy(out, rel, outsz-1); out[outsz-1]='\0'; return;
    }
    /* Copy base_file, then get its directory */
    char tmp[MAX_STR_LEN];
    strncpy(tmp, base_file, MAX_STR_LEN-1); tmp[MAX_STR_LEN-1]='\0';
    char *dir = dirname(tmp);
    snprintf(out, outsz, "%s/%s", dir, rel);
}

/* ========================= INIT / FREE ========================= */
Interpreter *interp_new(void) {
    Interpreter *interp=calloc(1,sizeof(Interpreter));
    interp->global_env=env_new(NULL);
    pthread_mutex_init(&interp->spark_lock,NULL);
    strncpy(interp->current_file,"<stdin>",MAX_STR_LEN-1);
    return interp;
}

void interp_free(Interpreter *interp) {
    /* Join all spark threads */
    pthread_mutex_lock(&interp->spark_lock);
    int cnt=interp->spark_task_count;
    pthread_mutex_unlock(&interp->spark_lock);
    for (int i=0;i<cnt;i++) {
        SparkTask *t=interp->spark_tasks[i];
        if (t) {
            pthread_join(t->thread,NULL);
            val_release(t->fn);
            for (int j=0;j<t->arg_count;j++) val_release(t->args[j]);
            if (t->result) val_release(t->result);
            pthread_mutex_destroy(&t->lock);
            free(t);
        }
    }
    pthread_mutex_destroy(&interp->spark_lock);
    for (int i=0;i<interp->frame_def_count;i++) framedef_free(interp->frame_defs[i]);
    env_free(interp->global_env);
    if (interp->return_val)   val_release(interp->return_val);
    if (interp->thrown_error) val_release(interp->thrown_error);
    free(interp);
}

/* ========================= FORWARD ========================= */
static RivenValue *exec(Interpreter *interp, ASTNode *node, Env *env);

/* ========================= STAMP FORMAT ========================= */
static char *do_format(const char *fmt, RivenValue **args, int argc) {
    char *buf=malloc(MAX_STR_LEN); int bi=0,ai=0;
    for (int i=0;fmt[i]&&bi<MAX_STR_LEN-2;i++) {
        if (fmt[i]=='{'&&fmt[i+1]=='}') {
            if (ai<argc) {
                char *s=val_to_string(args[ai++]);
                for (int j=0;s[j]&&bi<MAX_STR_LEN-2;j++) buf[bi++]=s[j];
                free(s);
            }
            i++;
        } else buf[bi++]=fmt[i];
    }
    buf[bi]='\0'; return buf;
}

/* ========================= ACCESS CONTROL ========================= */
/* Prefix encoding from parser:
   __O__ = open (public)
   __H__ = hidden (private)
   no prefix = open by default                                        */

static const char *strip_prefix(const char *name, int *is_open_out) {
    if (strncmp(name,"__O__",5)==0) { *is_open_out=1; return name+5; }
    if (strncmp(name,"__H__",5)==0) { *is_open_out=0; return name+5; }
    *is_open_out=1; /* default open */
    return name;
}

/* Returns 1=allowed, 0=blocked */
static int check_access(Interpreter *interp, RivenValue *obj,
                         const char *bare_name, int is_method, int line,
                         Env *env) {
    RivenValue *self_v = env_get(env,"self");
    if (self_v && val_deref(self_v)==obj) return 1; /* inside self: always ok */

    FrameDef *fd = interp_find_frame(interp, obj->frame_obj.frame_name);
    if (!fd) return 1;

    int acc = framedef_access(fd, bare_name, is_method);
    if (!acc) {
        riven_error(line,"Access denied: '%s' is %s on frame '%s'",
                    bare_name,
                    is_method?"a hidden method":"a hidden field",
                    obj->frame_obj.frame_name);
        return 0;
    }
    return 1;
}

/* ========================= CALL FUNCTION ========================= */
static RivenValue *call_fn(Interpreter *interp, RivenValue *fn,
                            RivenValue **args, int argc, RivenValue *self_obj)
{
    fn = val_deref(fn);
    if (!fn) { riven_error(0,"Null function call"); return val_null(); }
    if (fn->type==VAL_NATIVE_FN) return fn->native_fn(args,argc);
    if (fn->type!=VAL_FUNCTION) {
        riven_error(0,"Value of type %d is not callable",fn->type);
        return val_error("not callable");
    }

    Env *fn_env = env_new(fn->fn.closure_env ? (Env*)fn->fn.closure_env
                                              : interp->global_env);
    if (self_obj) env_set(fn_env,"self",self_obj,0);

    for (int i=0;i<fn->fn.param_count;i++) {
        RivenValue *arg=(i<argc)?args[i]:val_null();
        env_set(fn_env,fn->fn.params[i],arg,0);
        if (i>=argc) val_release(arg);
    }

    Signal prev=g_signal; g_signal=SIGNAL_NONE;
    int prev_fn=interp->in_function; interp->in_function=1;

    RivenValue *result=exec(interp,fn->fn.body,fn_env);

    interp->in_function=prev_fn;
    (void)prev;

    if (g_signal==SIGNAL_RETURN) {
        g_signal=SIGNAL_NONE;
        RivenValue *rv=interp->return_val; interp->return_val=NULL;
        env_free(fn_env);
        return rv?rv:val_null();
    }
    env_free(fn_env);
    return result?result:val_null();
}

/* ========================= FRAME INSTANTIATION ========================= */
static RivenValue *build_instance(Interpreter *interp, FrameDef *fd,
                                   RivenValue **args, int argc, Env *env)
{
    RivenValue *obj=calloc(1,sizeof(RivenValue));
    obj->type=VAL_FRAME_OBJ; obj->ref_count=1;
    strncpy(obj->frame_obj.frame_name,fd->name,MAX_IDENT_LEN-1);
    record_init(&obj->frame_obj.fields);
    record_init(&obj->frame_obj.methods);

    /* Copy default fields (deep copy so each instance has its own values) */
    for (int i=0;i<fd->fields.count;i++) {
        RivenValue *fv=val_copy_deep(fd->fields.vals[i]);
        record_set(&obj->frame_obj.fields,fd->fields.keys[i],fv);
        val_release(fv);
    }
    /* Copy method references */
    for (int i=0;i<fd->methods.count;i++)
        record_set(&obj->frame_obj.methods,fd->methods.keys[i],fd->methods.vals[i]);

    /* Call __boot__ constructor */
    RivenValue *boot=record_get(&obj->frame_obj.methods,"__boot__");
    if (boot) {
        call_fn(interp,boot,args,argc,obj);
        g_signal=SIGNAL_NONE;
    }
    return obj;
}

/* ========================= SPARK THREAD ========================= */
typedef struct { Interpreter *interp; SparkTask *task; } SparkArg;

static void *spark_entry(void *arg) {
    SparkArg *sa=(SparkArg*)arg;
    SparkTask *task=sa->task;
    Interpreter *interp=sa->interp;
    free(sa);

    RivenValue *result=call_fn(interp,task->fn,task->args,task->arg_count,NULL);
    pthread_mutex_lock(&task->lock);
    task->result=result;
    task->done=1;
    pthread_mutex_unlock(&task->lock);
    return NULL;
}

static RivenValue *launch_spark(Interpreter *interp, RivenValue *fn,
                                  RivenValue **args, int argc)
{
    pthread_mutex_lock(&interp->spark_lock);
    if (interp->spark_task_count>=MAX_SPARK_TASKS) {
        pthread_mutex_unlock(&interp->spark_lock);
        riven_error(0,"Spark task limit reached"); return val_null();
    }
    SparkTask *task=calloc(1,sizeof(SparkTask));
    pthread_mutex_init(&task->lock,NULL);
    val_retain(fn); task->fn=fn;
    task->arg_count=argc;
    for (int i=0;i<argc;i++) { val_retain(args[i]); task->args[i]=args[i]; }
    task->done=0; task->result=NULL;
    interp->spark_tasks[interp->spark_task_count++]=task;
    pthread_mutex_unlock(&interp->spark_lock);

    SparkArg *sa=malloc(sizeof(SparkArg));
    sa->interp=interp; sa->task=task;
    pthread_create(&task->thread,NULL,spark_entry,sa);

    /* Return a null handle — spark is fire-and-forget */
    return val_null();
}

static void sync_sparks(Interpreter *interp) {
    pthread_mutex_lock(&interp->spark_lock);
    int cnt=interp->spark_task_count;
    pthread_mutex_unlock(&interp->spark_lock);
    for (int i=0;i<cnt;i++) {
        SparkTask *t=interp->spark_tasks[i];
        if (t) pthread_join(t->thread,NULL);
    }
}

/* ========================= BINOP ========================= */
static RivenValue *eval_binop(const char *op,RivenValue *l,RivenValue *r,int line){
    l=val_deref(l); r=val_deref(r);
    if (!l) l=val_null(); if (!r) r=val_null();

    int bi=(l->type==VAL_INT&&r->type==VAL_INT);
    int af=(l->type==VAL_DNUM||r->type==VAL_DNUM);
    double lf=(l->type==VAL_DNUM)?l->dval:(double)l->ival;
    double rf=(r->type==VAL_DNUM)?r->dval:(double)r->ival;

    /* String concatenation */
    if (strcmp(op,"+")==0&&l->type==VAL_STRING) {
        char *rs=val_to_string(r); char buf[MAX_STR_LEN];
        snprintf(buf,MAX_STR_LEN,"%s%s",l->sval,rs); free(rs); return val_string(buf);
    }
    if (strcmp(op,"+")==0) {if(bi)return val_int(l->ival+r->ival);if(af)return val_dnum(lf+rf);}
    if (strcmp(op,"-")==0) {if(bi)return val_int(l->ival-r->ival);if(af)return val_dnum(lf-rf);}
    if (strcmp(op,"*")==0) {
        if(bi)return val_int(l->ival*r->ival);if(af)return val_dnum(lf*rf);
        if(l->type==VAL_STRING&&r->type==VAL_INT){
            char buf[MAX_STR_LEN]="";
            for(int i=0;i<r->ival;i++){
                if(strlen(buf)+strlen(l->sval)>=MAX_STR_LEN-1)break;
                strcat(buf,l->sval);
            }
            return val_string(buf);
        }
    }
    if (strcmp(op,"/")==0){
        if(rf==0.0){riven_error(line,"Division by zero");return val_error("div/0");}
        if(bi)return val_int(l->ival/r->ival);if(af)return val_dnum(lf/rf);
    }
    if (strcmp(op,"%")==0){
        if(bi){if(!r->ival){riven_error(line,"Modulo by zero");return val_error("mod/0");}return val_int(l->ival%r->ival);}
        return val_dnum(fmod(lf,rf));
    }
    if (strcmp(op,"==")==0) return val_bool(val_equal(l,r));
    if (strcmp(op,"!=")==0) return val_bool(!val_equal(l,r));
    if (strcmp(op,"<" )==0){if(bi)return val_bool(l->ival<r->ival);if(af)return val_bool(lf<rf);if(l->type==VAL_STRING&&r->type==VAL_STRING)return val_bool(strcmp(l->sval,r->sval)<0);}
    if (strcmp(op,">" )==0){if(bi)return val_bool(l->ival>r->ival);if(af)return val_bool(lf>rf);if(l->type==VAL_STRING&&r->type==VAL_STRING)return val_bool(strcmp(l->sval,r->sval)>0);}
    if (strcmp(op,"<=")==0){if(bi)return val_bool(l->ival<=r->ival);if(af)return val_bool(lf<=rf);}
    if (strcmp(op,">=")==0){if(bi)return val_bool(l->ival>=r->ival);if(af)return val_bool(lf>=rf);}
    if (strcmp(op,"and")==0) return val_bool(val_truthy(l)&&val_truthy(r));
    if (strcmp(op,"or" )==0) return val_bool(val_truthy(l)||val_truthy(r));
    riven_error(line,"Unknown operator '%s'",op); return val_null();
}

/* ========================= MAIN EXEC ========================= */
static RivenValue *exec(Interpreter *interp, ASTNode *node, Env *env) {
    if (!node) return val_null();
    if (g_signal!=SIGNAL_NONE) return val_null();

    switch (node->type) {

    case NODE_INT_LIT:  return val_int(node->ival);
    case NODE_DNUM_LIT: return val_dnum(node->dval);
    case NODE_STR_LIT:  return val_string(node->sval);
    case NODE_BOOL_LIT: return val_bool(node->bval);
    case NODE_NULL_LIT: return val_null();

    /* ---- Identifier ---- */
    case NODE_IDENT: {
        RivenValue *v=env_get(env,node->sval);
        if (!v) { riven_error(node->line,"Undefined variable '%s'",node->sval); return val_null(); }
        val_retain(v); return v;
        /* Note: we return the raw VAL_REF if present — callers that need the
           value transparent call val_deref. This lets assignment write through. */
    }

    /* ---- Program / Block ---- */
    case NODE_PROGRAM:
    case NODE_BLOCK: {
        Env *blk=(node->type==NODE_BLOCK)?env_new(env):env;
        RivenValue *last=val_null();
        for (int i=0;i<node->block.stmts.count;i++) {
            if (g_signal!=SIGNAL_NONE) break;
            val_release(last);
            last=exec(interp,node->block.stmts.items[i],blk);
            if (!last) last=val_null();
        }
        if (node->type==NODE_BLOCK) env_free(blk);
        return last;
    }

    /* ---- Assign ---- */
    case NODE_ASSIGN:
    case NODE_FIRM: {
        RivenValue *val=exec(interp,node->assign.value,env);
        if (!val) val=val_null();
        RivenValue *existing=env_get(env,node->assign.name);
        if (existing&&existing->type==VAL_REF&&!node->assign.is_firm) {
            val_ref_set(existing,val);
            val_release(val);
        } else if (env_exists(env,node->assign.name)) {
            env_update(env,node->assign.name,val);
            val_release(val);
        } else {
            env_set(env,node->assign.name,val,node->assign.is_firm);
            val_release(val);
        }
        return val_null();
    }

    /* ---- Member Assign: obj.field = val ---- */
    case NODE_MEMBER_ASSIGN: {
        RivenValue *obj=exec(interp,node->member_assign.obj,env);
        RivenValue *val=exec(interp,node->member_assign.value,env);
        if (!obj) { val_release(val); return val_null(); }
        RivenValue *dobj=val_deref(obj);
        const char *mem=node->member_assign.member;

        if (dobj->type==VAL_FRAME_OBJ) {
            if (!check_access(interp,dobj,mem,0,node->line,env)) {
                val_release(obj); val_release(val); return val_null();
            }
            record_set(&dobj->frame_obj.fields,mem,val);
        } else if (dobj->type==VAL_RECORD) {
            record_set(&dobj->record,mem,val);
        } else if (dobj->type==VAL_PTR) {
            /* ptr.value = x  — write through to the slot */
            if (strcmp(mem,"value")==0) {
                if (!dobj->ptr.is_valid||!dobj->ptr.slot) {
                    riven_error(node->line,"Invalid pointer write");
                    g_signal=SIGNAL_ERROR;
                    val_release(obj); val_release(val); return val_null();
                }
                RivenValue *slot_holder=*dobj->ptr.slot;
                if (slot_holder&&slot_holder->type==VAL_REF) {
                    val_ref_set(slot_holder,val);
                } else {
                    val_retain(val);
                    val_release(*dobj->ptr.slot);
                    *dobj->ptr.slot=val;
                }
            } else riven_error(node->line,"Cannot assign ptr.%s",mem);
        }
        val_release(obj); val_release(val);
        return val_null();
    }

    /* ---- Index Assign: obj[i] = val ---- */
    case NODE_INDEX_ASSIGN: {
        RivenValue *obj=exec(interp,node->index_assign.obj,env);
        RivenValue *idx=exec(interp,node->index_assign.idx,env);
        RivenValue *val=exec(interp,node->index_assign.value,env);
        RivenValue *dobj=val_deref(obj);
        RivenValue *didx=val_deref(idx);
        if (dobj&&dobj->type==VAL_COLL) {
            int i=(int)didx->ival;
            if (i<0) i=dobj->coll.count+i;
            if (i>=0&&i<dobj->coll.count) {
                val_retain(val); val_release(dobj->coll.items[i]); dobj->coll.items[i]=val;
            } else riven_error(node->line,"Index %d out of bounds",i);
        } else if (dobj&&dobj->type==VAL_RECORD&&didx&&didx->type==VAL_STRING) {
            record_set(&dobj->record,didx->sval,val);
        }
        val_release(obj); val_release(idx); val_release(val);
        return val_null();
    }

    /* ---- Ref ---- */
    case NODE_REF: {
        /* True aliasing: both the new name AND the source variable share the same box.
           Strategy:
             1. If RHS is an identifier whose slot already holds a VAL_REF → alias that box.
             2. If RHS is an identifier holding a plain value → wrap that value in a new box,
                UPDATE the source variable's slot to hold the box too, then alias.
             3. Otherwise → wrap the evaluated value in a new box (one-way box, no aliasing back).
        */
        RivenValue *box = NULL;

        if (node->refptr.value && node->refptr.value->type == NODE_IDENT) {
            const char *src_name = node->refptr.value->sval;
            RivenValue *src_v = env_get(env, src_name);

            if (src_v && src_v->type == VAL_REF) {
                /* Already a box — just alias it */
                box = val_alias_ref(src_v);
            } else if (src_v) {
                /* Plain value — create a box, then replace the source slot with the box */
                box = val_make_ref(src_v);
                /* Replace x's slot with the box so x also reads/writes through the box */
                /* We must do this via raw env_set which replaces the stored pointer */
                RivenValue **slot = env_slot(env, src_name);
                if (slot) {
                    val_release(*slot);
                    val_retain(box);
                    *slot = box;
                }
                /* Now alias the box for the new name */
                RivenValue *alias = val_alias_ref(box);
                val_release(box);
                box = alias;
            } else {
                box = val_make_ref(val_null());
            }
        } else {
            RivenValue *rhs = exec(interp, node->refptr.value, env);
            if (!rhs) rhs = val_null();
            if (rhs->type == VAL_REF) {
                box = val_alias_ref(rhs);
            } else {
                box = val_make_ref(rhs);
            }
            val_release(rhs);
        }

        env_set(env, node->refptr.name, box, 0);
        val_release(box);
        return val_null();
    }

    /* ---- Ptr — safe pointer to a named variable's env slot ---- */
    case NODE_PTR: {
        /* RHS must be an identifier whose slot we can take the address of */
        const char *varname = NULL;
        if (node->refptr.value && node->refptr.value->type==NODE_IDENT)
            varname = node->refptr.value->sval;

        RivenValue **slot = varname ? env_slot(env,varname) : NULL;

        if (!slot) {
            /* RHS is not a variable name — evaluate as value and wrap */
            RivenValue *rhs=exec(interp,node->refptr.value,env);
            if (!rhs) rhs=val_null();
            /* Create a temporary env binding so we have a slot */
            char tmp_name[MAX_IDENT_LEN];
            snprintf(tmp_name,MAX_IDENT_LEN,"__ptr_tmp_%s__",node->refptr.name);
            env_set(env,tmp_name,rhs,0);
            val_release(rhs);
            slot=env_slot(env,tmp_name);
        }

        const char *tag = varname ? varname : node->refptr.name;
        RivenValue *ptr=val_make_ptr(slot,tag);
        env_set(env,node->refptr.name,ptr,0);
        val_release(ptr);
        return val_null();
    }

    /* ---- Bind (alias for ref) ---- */
    case NODE_BIND: {
        RivenValue *rhs=exec(interp,node->refptr.value,env);
        if (!rhs) rhs=val_null();
        RivenValue *box=(rhs->type==VAL_REF)?val_alias_ref(rhs):val_make_ref(rhs);
        val_release(rhs);
        env_set(env,node->refptr.name,box,0);
        val_release(box);
        return val_null();
    }

    /* ---- BinOp ---- */
    case NODE_BINOP: {
        RivenValue *l=exec(interp,node->binop.left,env);
        if (!l) l=val_null();
        /* short circuit */
        if (strcmp(node->binop.op,"and")==0&&!val_truthy(l)) return l;
        if (strcmp(node->binop.op,"or" )==0&& val_truthy(l)) return l;
        RivenValue *r=exec(interp,node->binop.right,env);
        if (!r) r=val_null();
        RivenValue *res=eval_binop(node->binop.op,l,r,node->line);
        val_release(l); val_release(r);
        return res?res:val_null();
    }

    /* ---- UnOp ---- */
    case NODE_UNOP: {
        RivenValue *v=exec(interp,node->unop.operand,env);
        RivenValue *dv=val_deref(v);
        RivenValue *res=val_null();
        if (strcmp(node->unop.op,"-")==0) {
            if (dv->type==VAL_INT)  res=val_int(-dv->ival);
            else if(dv->type==VAL_DNUM) res=val_dnum(-dv->dval);
            else riven_error(node->line,"Unary - on non-number");
        } else { res=val_bool(!val_truthy(dv)); }
        val_release(v); return res;
    }

    /* ---- If ---- */
    case NODE_IF: {
        RivenValue *cond=exec(interp,node->ifstmt.cond,env);
        int t=val_truthy(cond); val_release(cond);
        if (t)                       return exec(interp,node->ifstmt.body,env);
        if (node->ifstmt.altifs)     return exec(interp,node->ifstmt.altifs,env);
        if (node->ifstmt.els)        return exec(interp,node->ifstmt.els,env);
        return val_null();
    }

    /* ---- Flow ---- */
    case NODE_FLOW: {
        RivenValue *cnt=exec(interp,node->flow.count,env);
        long long n=val_truthy(cnt)?(val_deref(cnt)->type==VAL_INT?val_deref(cnt)->ival:(long long)val_deref(cnt)->dval):0;
        val_release(cnt);
        RivenValue *last=val_null();
        int prev=interp->in_loop; interp->in_loop=1;
        for (long long i=0;i<n;i++) {
            if (g_signal==SIGNAL_BREAK){g_signal=SIGNAL_NONE;break;}
            if (g_signal==SIGNAL_CONTINUE){g_signal=SIGNAL_NONE;continue;}
            if (g_signal==SIGNAL_RETURN||g_signal==SIGNAL_ERROR) break;
            val_release(last);
            last=exec(interp,node->flow.body,env);
            if (!last) last=val_null();
        }
        interp->in_loop=prev; return last;
    }

    /* ---- During ---- */
    case NODE_DURING: {
        RivenValue *last=val_null();
        int prev=interp->in_loop; interp->in_loop=1;
        while (1) {
            if (g_signal==SIGNAL_BREAK){g_signal=SIGNAL_NONE;break;}
            if (g_signal==SIGNAL_RETURN||g_signal==SIGNAL_ERROR) break;
            RivenValue *cond=exec(interp,node->during.cond,env);
            int t=val_truthy(cond); val_release(cond);
            if (!t) break;
            if (g_signal==SIGNAL_CONTINUE){g_signal=SIGNAL_NONE;continue;}
            val_release(last);
            last=exec(interp,node->during.body,env);
            if (!last) last=val_null();
        }
        interp->in_loop=prev; return last;
    }

    /* ---- Rise / Inc ---- */
    case NODE_RISE: case NODE_INC: {
        RivenValue *v=env_get(env,node->incdec.name);
        if (!v){riven_error(node->line,"Undefined '%s'",node->incdec.name);return val_null();}
        RivenValue *dv=val_deref(v);
        RivenValue *nv=(dv->type==VAL_INT)?val_int(dv->ival+1):val_dnum(dv->dval+1.0);
        if (v->type==VAL_REF) val_ref_set(v,nv);
        else env_update(env,node->incdec.name,nv);
        val_release(nv);
        return val_null();
    }

    /* ---- Drop / Dec ---- */
    case NODE_DROP: case NODE_DEC: {
        RivenValue *v=env_get(env,node->incdec.name);
        if (!v){riven_error(node->line,"Undefined '%s'",node->incdec.name);return val_null();}
        RivenValue *dv=val_deref(v);
        RivenValue *nv=(dv->type==VAL_INT)?val_int(dv->ival-1):val_dnum(dv->dval-1.0);
        if (v->type==VAL_REF) val_ref_set(v,nv);
        else env_update(env,node->incdec.name,nv);
        val_release(nv);
        return val_null();
    }

    /* ---- Craft definition ---- */
    case NODE_CRAFT: {
        RivenValue *fn=val_alloc_pub(VAL_FUNCTION);
        strncpy(fn->fn.name,node->craft.name,MAX_IDENT_LEN-1);
        fn->fn.param_count=node->craft.param_count;
        for(int i=0;i<node->craft.param_count;i++)
            strncpy(fn->fn.params[i],node->craft.params[i],MAX_IDENT_LEN-1);
        fn->fn.body=node->craft.body;
        fn->fn.closure_env=env;
        fn->fn.is_spark=node->craft.is_spark;
        env_set(env,node->craft.name,fn,0);
        val_release(fn);
        return val_null();
    }

    /* ---- Return ---- */
    case NODE_RETURN: {
        RivenValue *rv=node->ret.value?exec(interp,node->ret.value,env):val_null();
        if (!rv) rv=val_null();
        if (interp->return_val) val_release(interp->return_val);
        interp->return_val=rv; val_retain(rv);
        g_signal=SIGNAL_RETURN;
        return rv;
    }

    /* ---- Call ---- */
    case NODE_CALL: {
        RivenValue *self_obj=NULL;
        RivenValue *callee=NULL;

        /* Method call: obj.method(args) */
        if (node->call.callee->type==NODE_MEMBER_ACCESS) {
            RivenValue *raw_obj=exec(interp,node->call.callee->member.obj,env);
            RivenValue *dobj=val_deref(raw_obj);
            const char *mname=node->call.callee->member.member;

            if (!dobj) { val_release(raw_obj); return val_null(); }

            if (dobj->type==VAL_FRAME_OBJ) {
                if (!check_access(interp,dobj,mname,1,node->line,env)) {
                    val_release(raw_obj); return val_error("access denied");
                }
                callee=record_get(&dobj->frame_obj.methods,mname);
                if (!callee) {
                    riven_error(node->line,"Method '%s' not found on '%s'",
                                mname,dobj->frame_obj.frame_name);
                    val_release(raw_obj); return val_null();
                }
                val_retain(callee);
                val_retain(dobj); self_obj=dobj;
                val_release(raw_obj);

            } else if (dobj->type==VAL_RECORD) {
                callee=record_get(&dobj->record,mname);
                if (!callee) {
                    riven_error(node->line,"Key '%s' not found in record",mname);
                    val_release(raw_obj); return val_null();
                }
                val_retain(callee);
                val_release(raw_obj);

            } else if (dobj->type==VAL_COLL) {
                /* Built-in collection methods */
                RivenValue *cargs[MAX_ARGS]; int cargc=0;
                for(int i=0;i<node->call.arg_count;i++)
                    cargs[cargc++]=exec(interp,node->call.args[i],env);
                RivenValue *res=val_null();
                if (strcmp(mname,"push")==0&&cargc>=1) {
                    val_coll_push(dobj,val_deref(cargs[0]));
                } else if (strcmp(mname,"pop")==0) {
                    if (dobj->coll.count>0) {
                        val_release(res);
                        res=dobj->coll.items[--dobj->coll.count];
                        val_retain(res);
                    }
                } else if (strcmp(mname,"len")==0) {
                    val_release(res); res=val_int(dobj->coll.count);
                } else {
                    riven_error(node->line,"Unknown coll method '%s'",mname);
                }
                val_release(raw_obj);
                for(int i=0;i<cargc;i++) val_release(cargs[i]);
                return res;

            } else if (dobj->type==VAL_PTR) {
                /* ptr.value read via "call" — treat ptr.value() as ptr.value */
                RivenValue *res=val_null();
                if (strcmp(mname,"value")==0) {
                    if (!dobj->ptr.is_valid||!dobj->ptr.slot) {
                        riven_error(node->line,"Invalid pointer read");
                        g_signal=SIGNAL_ERROR;
                    } else {
                        val_release(res);
                        res=*dobj->ptr.slot;
                        if (res) val_retain(res); else res=val_null();
                        res=val_deref(res); val_retain(res);
                    }
                } else riven_error(node->line,"Unknown ptr method '%s'",mname);
                val_release(raw_obj);
                return res;
            } else {
                riven_error(node->line,"Cannot call method on type %d",dobj->type);
                val_release(raw_obj); return val_null();
            }
        } else {
            callee=exec(interp,node->call.callee,env);
        }

        if (!callee) return val_null();

        /* Evaluate arguments */
        RivenValue *args[MAX_ARGS]; int argc=0;
        for(int i=0;i<node->call.arg_count;i++) {
            args[argc]=exec(interp,node->call.args[i],env);
            if (!args[argc]) args[argc]=val_null();
            argc++;
        }

        RivenValue *dcallee=val_deref(callee);

        /* Frame constructor call: FrameName(args) */
        if (dcallee&&dcallee->type==VAL_RECORD&&node->call.callee->type==NODE_IDENT) {
            FrameDef *fd=interp_find_frame(interp,node->call.callee->sval);
            if (fd) {
                RivenValue *inst=build_instance(interp,fd,args,argc,env);
                val_release(callee);
                if (self_obj) val_release(self_obj);
                for(int i=0;i<argc;i++) val_release(args[i]);
                return inst;
            }
        }

        /* Spark launch */
        if (dcallee&&dcallee->type==VAL_FUNCTION&&dcallee->fn.is_spark) {
            RivenValue *handle=launch_spark(interp,dcallee,args,argc);
            val_release(callee);
            if (self_obj) val_release(self_obj);
            for(int i=0;i<argc;i++) val_release(args[i]);
            return handle?handle:val_null();
        }

        RivenValue *result=call_fn(interp,dcallee,args,argc,self_obj);
        val_release(callee);
        if (self_obj) val_release(self_obj);
        for(int i=0;i<argc;i++) val_release(args[i]);
        return result?result:val_null();
    }

    /* ---- Member Access: obj.field ---- */
    case NODE_MEMBER_ACCESS: {
        RivenValue *obj=exec(interp,node->member.obj,env);
        if (!obj) return val_null();
        RivenValue *dobj=val_deref(obj);
        const char *mem=node->member.member;
        RivenValue *res=NULL;

        if (!dobj) { val_release(obj); return val_null(); }

        if (dobj->type==VAL_FRAME_OBJ) {
            if (!check_access(interp,dobj,mem,0,node->line,env)&&
                !check_access(interp,dobj,mem,1,node->line,env)) {
                val_release(obj); return val_null();
            }
            res=record_get(&dobj->frame_obj.fields,mem);
            if (!res) res=record_get(&dobj->frame_obj.methods,mem);
        } else if (dobj->type==VAL_RECORD) {
            res=record_get(&dobj->record,mem);
        } else if (dobj->type==VAL_COLL) {
            if (strcmp(mem,"len")==0){val_release(obj);return val_int(dobj->coll.count);}
        } else if (dobj->type==VAL_PTR) {
            if (strcmp(mem,"value")==0) {
                if (!dobj->ptr.is_valid||!dobj->ptr.slot) {
                    riven_error(node->line,"Invalid pointer read");
                    g_signal=SIGNAL_ERROR; val_release(obj); return val_null();
                }
                res=val_deref(*dobj->ptr.slot);
            } else if (strcmp(mem,"tag")==0)   { val_release(obj); return val_string(dobj->ptr.tag); }
              else if (strcmp(mem,"valid")==0)  { val_release(obj); return val_bool(dobj->ptr.is_valid); }
        }

        if (!res) {
            riven_error(node->line,"No member '%s'",mem);
            val_release(obj); return val_null();
        }
        val_retain(res);
        val_release(obj);
        return res;
    }

    /* ---- Index: obj[i] ---- */
    case NODE_INDEX: {
        RivenValue *obj=exec(interp,node->index.obj,env);
        RivenValue *idx=exec(interp,node->index.idx,env);
        RivenValue *dobj=val_deref(obj);
        RivenValue *didx=val_deref(idx);
        RivenValue *res=val_null();

        if (dobj&&dobj->type==VAL_COLL) {
            int i=(int)didx->ival;
            if (i<0) i=dobj->coll.count+i;
            if (i>=0&&i<dobj->coll.count){res=dobj->coll.items[i];val_retain(res);}
            else riven_error(node->line,"Index %d out of bounds (len=%d)",i,dobj->coll.count);
        } else if (dobj&&dobj->type==VAL_STRING) {
            int i=(int)didx->ival;
            if (i>=0&&i<(int)strlen(dobj->sval)){char ch[2]={dobj->sval[i],0};res=val_string(ch);}
        } else if (dobj&&dobj->type==VAL_RECORD&&didx&&didx->type==VAL_STRING) {
            res=record_get(&dobj->record,didx->sval);
            if (res) val_retain(res); else res=val_null();
        }
        val_release(obj); val_release(idx);
        return res;
    }

    /* ---- Collection literal ---- */
    case NODE_COLL_LITERAL: {
        RivenValue *c=val_coll_empty();
        for(int i=0;i<node->coll.items.count;i++) {
            RivenValue *item=exec(interp,node->coll.items.items[i],env);
            if (!item) item=val_null();
            val_coll_push(c,item); val_release(item);
        }
        return c;
    }

    /* ---- Record literal ---- */
    case NODE_REC_LITERAL: {
        RivenValue *r=val_record_empty();
        for(int i=0;i<node->rec.count;i++) {
            RivenValue *v=exec(interp,node->rec.vals[i],env);
            if (!v) v=val_null();
            val_record_set(r,node->rec.keys[i],v);
            val_release(v);
        }
        return r;
    }

    /* ---- Frame definition ---- */
    case NODE_FRAME: {
        FrameDef *fd=framedef_new(node->frame.name);

        for(int i=0;i<node->frame.members.count;i++) {
            ASTNode *m=node->frame.members.items[i];

            if (m->type==NODE_CRAFT) {
                int is_open;
                const char *bare=strip_prefix(m->craft.name,&is_open);

                RivenValue *fn=val_alloc_pub(VAL_FUNCTION);
                strncpy(fn->fn.name,bare,MAX_IDENT_LEN-1);
                fn->fn.param_count=m->craft.param_count;
                for(int j=0;j<m->craft.param_count;j++)
                    strncpy(fn->fn.params[j],m->craft.params[j],MAX_IDENT_LEN-1);
                fn->fn.body=m->craft.body;
                fn->fn.closure_env=env;
                fn->fn.is_spark=m->craft.is_spark;

                /* Store under bare name */
                record_set(&fd->methods,bare,fn);
                val_release(fn);

                /* Register access */
                framedef_reg_member(fd,bare,1,is_open);

                /* Boot param tracking */
                if (strcmp(bare,"__boot__")==0) {
                    fd->boot_param_count=m->craft.param_count;
                    for(int j=0;j<m->craft.param_count;j++)
                        strncpy(fd->boot_params[j],m->craft.params[j],MAX_IDENT_LEN-1);
                }

            } else if (m->type==NODE_ASSIGN||m->type==NODE_FIRM) {
                int is_open;
                const char *bare=strip_prefix(m->assign.name,&is_open);

                RivenValue *fv=exec(interp,m->assign.value,env);
                if (!fv) fv=val_null();
                record_set(&fd->fields,bare,fv);
                val_release(fv);
                framedef_reg_member(fd,bare,0,is_open);
            }
        }

        interp_add_frame(interp,fd);

        /* Register a sentinel in env so FrameName() call can find the FrameDef */
        RivenValue *sentinel=val_record_empty();
        env_set(env,node->frame.name,sentinel,0);
        val_release(sentinel);
        return val_null();
    }

    /* ---- Spawn ---- */
    case NODE_SPAWN: {
        FrameDef *fd=interp_find_frame(interp,node->spawn.frame_name);
        if (!fd) {
            riven_error(node->line,"Unknown frame '%s'",node->spawn.frame_name);
            return val_null();
        }
        RivenValue *args[MAX_ARGS]; int argc=0;
        for(int i=0;i<node->spawn.arg_count;i++) {
            args[argc]=exec(interp,node->spawn.args[i],env);
            if (!args[argc]) args[argc]=val_null();
            argc++;
        }
        RivenValue *obj=build_instance(interp,fd,args,argc,env);
        for(int i=0;i<argc;i++) val_release(args[i]);
        return obj;
    }

    /* ---- Stamp ---- */
    case NODE_STAMP: {
        RivenValue *fmt_v=exec(interp,node->stamp.fmt,env);
        RivenValue *args[MAX_ARGS];
        for(int i=0;i<node->stamp.arg_count;i++) {
            args[i]=exec(interp,node->stamp.args[i],env);
            if (!args[i]) args[i]=val_null();
        }
        RivenValue *dfmt=val_deref(fmt_v);
        if (dfmt&&dfmt->type==VAL_STRING) {
            char *out=do_format(dfmt->sval,args,node->stamp.arg_count);
            printf("%s\n",out); free(out);
        } else { if(fmt_v)val_print(fmt_v); printf("\n"); }
        val_release(fmt_v);
        for(int i=0;i<node->stamp.arg_count;i++) val_release(args[i]);
        return val_null();
    }

    /* ---- Grab ---- */
    case NODE_GRAB: {
        RivenValue *pr=exec(interp,node->grab.prompt,env);
        char *ps=val_to_string(pr); printf("%s",ps); free(ps); val_release(pr);
        fflush(stdout);
        char buf[MAX_STR_LEN];
        if (!fgets(buf,MAX_STR_LEN,stdin)) return val_string("");
        int l=strlen(buf); if(l>0&&buf[l-1]=='\n')buf[l-1]='\0';
        return val_string(buf);
    }

    /* ---- Fetch ---- */
    case NODE_FETCH: {
        RivenValue *url=exec(interp,node->fetch.url,env);
        char *us=val_to_string(val_deref(url)); val_release(url);
        char cmd[MAX_STR_LEN];
        snprintf(cmd,MAX_STR_LEN,"curl -s --max-time 10 \"%s\" 2>/dev/null",us);
        free(us);
        FILE *fp=popen(cmd,"r");
        if (!fp) return val_string("{\"error\":\"fetch unavailable\"}");
        char buf[MAX_STR_LEN]; int bi=0,c;
        while((c=fgetc(fp))!=EOF&&bi<MAX_STR_LEN-1)buf[bi++]=(char)c;
        buf[bi]='\0'; pclose(fp);
        return val_string(buf[0]?buf:"{\"error\":\"empty\"}");
    }

    /* ---- Cast ---- */
    case NODE_CAST: {
        RivenValue *v=exec(interp,node->cast.value,env);
        RivenValue *dv=val_deref(v);
        RivenValue *res=val_null();
        if (strcmp(node->cast.target_type,"int")==0) {
            if(dv->type==VAL_INT)    res=val_int(dv->ival);
            else if(dv->type==VAL_DNUM)   res=val_int((long long)dv->dval);
            else if(dv->type==VAL_STRING) res=val_int(atoll(dv->sval));
            else if(dv->type==VAL_BOOL)   res=val_int(dv->bval);
            else res=val_int(0);
        } else if (strcmp(node->cast.target_type,"dnum")==0) {
            if(dv->type==VAL_DNUM)   res=val_dnum(dv->dval);
            else if(dv->type==VAL_INT)    res=val_dnum((double)dv->ival);
            else if(dv->type==VAL_STRING) res=val_dnum(atof(dv->sval));
            else res=val_dnum(0.0);
        } else if (strcmp(node->cast.target_type,"txt")==0) {
            char *s=val_to_string(dv); res=val_string(s); free(s);
        }
        val_release(v); return res;
    }

    /* ---- Attack ---- */
    case NODE_ATTACK: {
        RivenValue *msg=exec(interp,node->attack.msg,env);
        char *ms=val_to_string(val_deref(msg)); val_release(msg);
        fprintf(stderr,"\033[1;31m[ATTACK] %s\033[0m\n",ms);
        if (interp->thrown_error) val_release(interp->thrown_error);
        interp->thrown_error=val_error(ms);
        interp->has_error=1; g_signal=SIGNAL_ERROR;
        free(ms); return val_null();
    }

    /* ---- Resc ---- */
    case NODE_RESC: {
        Signal saved=g_signal; g_signal=SIGNAL_NONE;
        int saved_err=interp->has_error; interp->has_error=0;
        /* Run the resc body in the CURRENT env so variables are visible after */
        ASTNode *body = node->resc.body;
        RivenValue *res = val_null();
        if (body && body->type == NODE_BLOCK) {
            /* Execute statements directly in current env without creating child scope */
            for (int i = 0; i < body->block.stmts.count; i++) {
                if (g_signal != SIGNAL_NONE) break;
                val_release(res);
                res = exec(interp, body->block.stmts.items[i], env);
                if (!res) res = val_null();
            }
        } else {
            res = exec(interp, body, env);
            if (!res) res = val_null();
        }
        if (g_signal==SIGNAL_ERROR) {
            g_signal=SIGNAL_NONE; interp->has_error=0;
            if (interp->thrown_error) {
                env_set(env,"err",val_string(interp->thrown_error->error_msg),0);
                val_release(interp->thrown_error);
                interp->thrown_error=NULL;
            }
        } else if (g_signal==SIGNAL_NONE) {
            (void)saved; (void)saved_err;
        }
        return res;
    }

    /* ---- Consistof ---- */
    case NODE_CONSISTOF: {
        char resolved[MAX_STR_LEN];
        resolve_path(interp->current_file, node->consistof.path,
                     resolved, sizeof(resolved));

        /* Import cache — do not reload */
        for(int i=0;i<interp->import_count;i++)
            if (strcmp(interp->imported[i],resolved)==0) return val_null();

        if (interp->import_count<MAX_IMPORTS)
            strncpy(interp->imported[interp->import_count++],resolved,MAX_STR_LEN-1);

        char *src=read_file(resolved);
        if (!src) {
            /* Try relative to cwd as fallback */
            src=read_file(node->consistof.path);
            if (!src) {
                riven_error(node->line,"Cannot import '%s' (resolved: %s)",
                            node->consistof.path, resolved);
                return val_null();
            }
            strncpy(resolved,node->consistof.path,MAX_STR_LEN-1);
        }

        /* Save and restore current_file for nested imports */
        char prev_file[MAX_STR_LEN];
        strncpy(prev_file,interp->current_file,MAX_STR_LEN-1);
        strncpy(interp->current_file,resolved,MAX_STR_LEN-1);

        Lexer *lx=lexer_new(src); lexer_tokenize(lx);
        Parser *ps=parser_new(lx->tokens,lx->token_count);
        ASTNode *prog=parser_parse(ps);

        /* Imports share the GLOBAL environment */
        exec(interp,prog,interp->global_env);

        ast_free(prog); parser_free(ps); lexer_free(lx); free(src);
        strncpy(interp->current_file,prev_file,MAX_STR_LEN-1);
        return val_null();
    }

    /* ---- Sync ---- */
    case NODE_SYNC:
        sync_sparks(interp);
        return val_null();

    /* ---- Raw block ---- */
    case NODE_RAW: {
        int prev=interp->raw_mode; interp->raw_mode=1;
        RivenValue *res=exec(interp,node->raw.body,env);
        interp->raw_mode=prev;
        if (!res) res=val_null();
        return res;
    }

    /* ---- Spark def (bare spark — handled at call site) ---- */
    case NODE_SPARK_DEF:
        return val_null();

    default:
        riven_error(node->line,"Unknown node type %d",node->type);
        return val_null();
    }
}

/* ========================= PUBLIC API ========================= */
void interp_register_natives(Interpreter *interp, Env *env) {
    (void)interp; stdlib_register(env);
}

RivenValue *interp_exec(Interpreter *interp, ASTNode *node, Env *env) {
    return exec(interp,node,env);
}

void interp_run_file(Interpreter *interp, const char *path) {
    char *src=read_file(path);
    if (!src){fprintf(stderr,"Cannot open: %s\n",path);return;}

    /* Resolve to absolute path for import cache */
    char abs_path[MAX_STR_LEN];
    if (realpath(path,abs_path)) strncpy(interp->current_file,abs_path,MAX_STR_LEN-1);
    else strncpy(interp->current_file,path,MAX_STR_LEN-1);

    Lexer *lx=lexer_new(src); lexer_tokenize(lx);
    Parser *ps=parser_new(lx->tokens,lx->token_count);
    ASTNode *prog=parser_parse(ps);
    interp_register_natives(interp,interp->global_env);
    exec(interp,prog,interp->global_env);
    ast_free(prog); parser_free(ps); lexer_free(lx); free(src);
}
