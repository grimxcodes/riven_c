#include "../include/riven.h"

/* ========================= RECORD MAP ========================= */
void record_init(RecordMap *m) { m->keys=NULL; m->vals=NULL; m->count=m->cap=0; }

void record_set(RecordMap *m, const char *key, RivenValue *val) {
    for (int i=0;i<m->count;i++) {
        if (strcmp(m->keys[i],key)==0) {
            val_retain(val); val_release(m->vals[i]); m->vals[i]=val; return;
        }
    }
    if (m->count>=m->cap) {
        m->cap = m->cap ? m->cap*2 : 4;
        m->keys = realloc(m->keys, sizeof(char*)*m->cap);
        m->vals = realloc(m->vals, sizeof(RivenValue*)*m->cap);
    }
    m->keys[m->count] = strdup(key);
    val_retain(val);
    m->vals[m->count++] = val;
}

RivenValue *record_get(RecordMap *m, const char *key) {
    for (int i=0;i<m->count;i++)
        if (strcmp(m->keys[i],key)==0) return m->vals[i];
    return NULL;
}

void record_free(RecordMap *m) {
    for (int i=0;i<m->count;i++) { free(m->keys[i]); val_release(m->vals[i]); }
    free(m->keys); free(m->vals);
    m->keys=NULL; m->vals=NULL; m->count=m->cap=0;
}

/* ========================= FRAME DEF ========================= */
FrameDef *framedef_new(const char *name) {
    FrameDef *fd = calloc(1, sizeof(FrameDef));
    strncpy(fd->name, name, MAX_IDENT_LEN-1);
    record_init(&fd->fields); record_init(&fd->methods);
    return fd;
}
void framedef_free(FrameDef *fd) {
    if (!fd) return;
    record_free(&fd->fields); record_free(&fd->methods);
    free(fd);
}
FrameDef *interp_find_frame(Interpreter *interp, const char *name) {
    for (int i=0;i<interp->frame_def_count;i++)
        if (strcmp(interp->frame_defs[i]->name,name)==0) return interp->frame_defs[i];
    return NULL;
}
void interp_add_frame(Interpreter *interp, FrameDef *fd) {
    for (int i=0;i<interp->frame_def_count;i++) {
        if (strcmp(interp->frame_defs[i]->name,fd->name)==0) {
            framedef_free(interp->frame_defs[i]);
            interp->frame_defs[i]=fd; return;
        }
    }
    if (interp->frame_def_count<MAX_FRAMES)
        interp->frame_defs[interp->frame_def_count++]=fd;
    else riven_error(0,"Frame definition limit reached");
}

/* Register a member's access level in FrameDef */
void framedef_reg_member(FrameDef *fd, const char *bare, int is_method, int is_open) {
    if (fd->member_count >= MAX_ARGS*2) return;
    strncpy(fd->member_names[fd->member_count], bare, MAX_IDENT_LEN-1);
    fd->member_names[fd->member_count][MAX_IDENT_LEN-1] = '\0';
    fd->member_is_method[fd->member_count] = is_method;
    fd->member_access[fd->member_count]    = is_open;
    fd->member_count++;
}

/* Returns 1=open/accessible, 0=hidden */
int framedef_access(FrameDef *fd, const char *bare, int is_method) {
    for (int i=0;i<fd->member_count;i++)
        if (fd->member_is_method[i]==is_method && strcmp(fd->member_names[i],bare)==0)
            return fd->member_access[i];
    return 1; /* unlabeled = open by default */
}

/* ========================= VALUE ALLOC ========================= */
RivenValue *val_alloc_pub(ValueType type) {
    RivenValue *v = calloc(1, sizeof(RivenValue));
    v->type = type; v->ref_count = 1;
    return v;
}

RivenValue *val_int(long long i)   { RivenValue *v=val_alloc_pub(VAL_INT);   v->ival=i; return v; }
RivenValue *val_dnum(double d)     { RivenValue *v=val_alloc_pub(VAL_DNUM);  v->dval=d; return v; }
RivenValue *val_bool(int b)        { RivenValue *v=val_alloc_pub(VAL_BOOL);  v->bval=b; return v; }
RivenValue *val_null(void)         { return val_alloc_pub(VAL_NULL); }
RivenValue *val_string(const char *s) {
    RivenValue *v=val_alloc_pub(VAL_STRING); v->sval=strdup(s?s:""); return v;
}
RivenValue *val_error(const char *msg) {
    RivenValue *v=val_alloc_pub(VAL_ERROR); v->error_msg=strdup(msg?msg:"error"); return v;
}
RivenValue *val_coll_empty(void) {
    RivenValue *v=val_alloc_pub(VAL_COLL);
    v->coll.items=NULL; v->coll.count=v->coll.cap=0; return v;
}
void val_coll_push(RivenValue *c, RivenValue *item) {
    if (!c||c->type!=VAL_COLL) return;
    if (c->coll.count>=c->coll.cap) {
        c->coll.cap = c->coll.cap ? c->coll.cap*2 : 4;
        c->coll.items = realloc(c->coll.items, sizeof(RivenValue*)*c->coll.cap);
    }
    val_retain(item); c->coll.items[c->coll.count++]=item;
}
RivenValue *val_record_empty(void) {
    RivenValue *v=val_alloc_pub(VAL_RECORD); record_init(&v->record); return v;
}
void val_record_set(RivenValue *r, const char *k, RivenValue *v) {
    if (r&&r->type==VAL_RECORD) record_set(&r->record,k,v);
}
RivenValue *val_record_get(RivenValue *r, const char *k) {
    if (!r||r->type!=VAL_RECORD) return NULL;
    return record_get(&r->record,k);
}

/* ========================= REF ========================= */
RivenValue *val_make_ref(RivenValue *initial) {
    RivenBox *box = calloc(1, sizeof(RivenBox));
    box->ref_count = 1;
    val_retain(initial); box->value = initial;
    RivenValue *v = val_alloc_pub(VAL_REF);
    v->box = box;
    return v;
}
RivenValue *val_alias_ref(RivenValue *src) {
    /* src must already be VAL_REF */
    src->box->ref_count++;
    RivenValue *v = val_alloc_pub(VAL_REF);
    v->box = src->box;
    return v;
}
RivenValue *val_ref_get(RivenValue *ref) {
    if (!ref || ref->type!=VAL_REF || !ref->box) return val_null();
    return ref->box->value;
}
void val_ref_set(RivenValue *ref, RivenValue *newval) {
    if (!ref || ref->type!=VAL_REF || !ref->box) return;
    val_retain(newval);
    val_release(ref->box->value);
    ref->box->value = newval;
}

/* ========================= PTR ========================= */
/* A safe pointer holds a pointer-to-slot (RivenValue**).
   Reading: *slot.  Writing: release old, retain new, store.
   This never touches arbitrary C memory — fully safe.        */
RivenValue *val_make_ptr(RivenValue **slot, const char *tag) {
    RivenValue *v = val_alloc_pub(VAL_PTR);
    v->ptr.slot     = slot;
    v->ptr.is_valid = (slot != NULL);
    strncpy(v->ptr.tag, tag?tag:"ptr", MAX_IDENT_LEN-1);
    return v;
}

/* ========================= DEREF ========================= */
/* Transparent read: if v is REF, return the contained value (not the box).
   The returned pointer is NOT a new reference — do not release it extra.  */
RivenValue *val_deref(RivenValue *v) {
    if (v && v->type==VAL_REF && v->box) return v->box->value;
    return v;
}

/* ========================= LIFECYCLE ========================= */
void val_retain(RivenValue *v) { if (v) v->ref_count++; }

void val_release(RivenValue *v) {
    if (!v) return;
    if (v->ref_count <= 0) return; /* guard against double-free */
    v->ref_count--;
    if (v->ref_count > 0) return;
    switch (v->type) {
        case VAL_STRING: free(v->sval); break;
        case VAL_ERROR:  free(v->error_msg); break;
        case VAL_COLL:
            for (int i=0;i<v->coll.count;i++) val_release(v->coll.items[i]);
            free(v->coll.items); break;
        case VAL_RECORD:    record_free(&v->record); break;
        case VAL_FRAME_OBJ:
            record_free(&v->frame_obj.fields);
            record_free(&v->frame_obj.methods); break;
        case VAL_FUNCTION:  /* body owned by AST, not freed here */ break;
        case VAL_REF: {
            RivenBox *box = v->box;
            if (box) {
                box->ref_count--;
                if (box->ref_count <= 0) {
                    val_release(box->value); free(box);
                }
            }
            break;
        }
        case VAL_PTR:  /* slot pointer not owned by the value */ break;
        default: break;
    }
    free(v);
}

/* ========================= PREDICATES ========================= */
int val_truthy(RivenValue *v) {
    v = val_deref(v);
    if (!v) return 0;
    switch (v->type) {
        case VAL_BOOL:   return v->bval;
        case VAL_INT:    return v->ival!=0;
        case VAL_DNUM:   return v->dval!=0.0;
        case VAL_STRING: return v->sval&&v->sval[0]!='\0';
        case VAL_NULL:   return 0;
        case VAL_ERROR:  return 0;
        case VAL_PTR:    return v->ptr.is_valid;
        default:         return 1;
    }
}

int val_equal(RivenValue *a, RivenValue *b) {
    a=val_deref(a); b=val_deref(b);
    if (!a||!b) return a==b;
    if (a->type==VAL_NULL && b->type==VAL_NULL) return 1;
    if (a->type==VAL_BOOL && b->type==VAL_BOOL) return a->bval==b->bval;
    if (a->type==VAL_INT  && b->type==VAL_INT)  return a->ival==b->ival;
    if (a->type==VAL_DNUM && b->type==VAL_DNUM) return a->dval==b->dval;
    if (a->type==VAL_INT  && b->type==VAL_DNUM) return (double)a->ival==b->dval;
    if (a->type==VAL_DNUM && b->type==VAL_INT)  return a->dval==(double)b->ival;
    if (a->type==VAL_STRING && b->type==VAL_STRING) return strcmp(a->sval,b->sval)==0;
    return 0;
}

/* ========================= TO STRING / PRINT ========================= */
char *val_to_string(RivenValue *v) {
    /* Always deref so refs print their content, not the box address */
    if (v && v->type==VAL_REF) v = val_deref(v);
    if (!v) return strdup("emp");
    char buf[512];
    switch (v->type) {
        case VAL_INT:    snprintf(buf,512,"%lld",v->ival);  return strdup(buf);
        case VAL_DNUM:   snprintf(buf,512,"%g",  v->dval);  return strdup(buf);
        case VAL_STRING: return strdup(v->sval);
        case VAL_BOOL:   return strdup(v->bval?"correct":"incorrect");
        case VAL_NULL:   return strdup("emp");
        case VAL_ERROR:  snprintf(buf,512,"[ERROR:%s]",v->error_msg); return strdup(buf);
        case VAL_PTR: {
            RivenValue *inner = v->ptr.slot ? *v->ptr.slot : NULL;
            if (inner) {
                char *s = val_to_string(inner);
                snprintf(buf,512,"<ptr:%s→%s>",v->ptr.tag,s);
                free(s);
            } else snprintf(buf,512,"<ptr:%s→null>",v->ptr.tag);
            return strdup(buf);
        }
        case VAL_COLL: {
            char tmp[MAX_STR_LEN]="[";
            for (int i=0;i<v->coll.count;i++) {
                char *s=val_to_string(v->coll.items[i]);
                if (strlen(tmp)+strlen(s)+4<MAX_STR_LEN) {
                    strcat(tmp,s);
                    if (i<v->coll.count-1) strcat(tmp,", ");
                }
                free(s);
            }
            strcat(tmp,"]"); return strdup(tmp);
        }
        case VAL_RECORD: {
            char tmp[MAX_STR_LEN]="{";
            for (int i=0;i<v->record.count;i++) {
                char *s=val_to_string(v->record.vals[i]);
                if (strlen(tmp)+strlen(v->record.keys[i])+strlen(s)+8<MAX_STR_LEN) {
                    strcat(tmp,v->record.keys[i]); strcat(tmp,": "); strcat(tmp,s);
                    if (i<v->record.count-1) strcat(tmp,", ");
                }
                free(s);
            }
            strcat(tmp,"}"); return strdup(tmp);
        }
        case VAL_FRAME_OBJ:
            snprintf(buf,512,"<%s>",v->frame_obj.frame_name); return strdup(buf);
        case VAL_FUNCTION:
            snprintf(buf,512,"<craft:%s>",v->fn.name); return strdup(buf);
        case VAL_NATIVE_FN: return strdup("<native-craft>");
        default: return strdup("?");
    }
}
void val_print(RivenValue *v) { char *s=val_to_string(v); printf("%s",s); free(s); }

/* Deep copy for frame field defaults */
RivenValue *val_copy_deep(RivenValue *v) {
    if (!v) return val_null();
    if (v->type==VAL_REF)  return val_alias_ref(v);
    switch (v->type) {
        case VAL_INT:    return val_int(v->ival);
        case VAL_DNUM:   return val_dnum(v->dval);
        case VAL_STRING: return val_string(v->sval);
        case VAL_BOOL:   return val_bool(v->bval);
        case VAL_NULL:   return val_null();
        case VAL_COLL: {
            RivenValue *nc = val_coll_empty();
            for (int i=0;i<v->coll.count;i++) {
                RivenValue *item = val_copy_deep(v->coll.items[i]);
                val_coll_push(nc, item); val_release(item);
            }
            return nc;
        }
        default: val_retain(v); return v;
    }
}

/* ========================= NODELIST ========================= */
void nl_init(NodeList *l) { l->items=NULL; l->count=l->cap=0; }
void nl_push(NodeList *l, ASTNode *n) {
    if (l->count>=l->cap) {
        l->cap = l->cap?l->cap*2:8;
        l->items = realloc(l->items, sizeof(ASTNode*)*l->cap);
    }
    l->items[l->count++]=n;
}
void nl_free(NodeList *l) { free(l->items); l->items=NULL; l->count=l->cap=0; }

/* ========================= ENVIRONMENT ========================= */
Env *env_new(Env *parent) {
    Env *e=calloc(1,sizeof(Env)); e->parent=parent; return e;
}
void env_free(Env *e) {
    if (!e) return;
    for (int i=0;i<e->count;i++) val_release(e->values[i]);
    free(e);
}
RivenValue *env_get(Env *e, const char *name) {
    for (Env *cur=e;cur;cur=cur->parent)
        for (int i=0;i<cur->count;i++)
            if (strcmp(cur->names[i],name)==0) return cur->values[i];
    return NULL;
}
/* Returns pointer to the RivenValue* slot — used to make safe ptrs */
RivenValue **env_slot(Env *e, const char *name) {
    for (Env *cur=e;cur;cur=cur->parent)
        for (int i=0;i<cur->count;i++)
            if (strcmp(cur->names[i],name)==0) return &cur->values[i];
    return NULL;
}
int env_exists(Env *e, const char *name) {
    for (Env *cur=e;cur;cur=cur->parent)
        for (int i=0;i<cur->count;i++)
            if (strcmp(cur->names[i],name)==0) return 1;
    return 0;
}
void env_set(Env *e, const char *name, RivenValue *val, int firm) {
    /* Update existing in current scope */
    for (int i=0;i<e->count;i++) {
        if (strcmp(e->names[i],name)==0) {
            if (e->firm[i]) { riven_error(0,"Cannot reassign firm '%s'",name); return; }
            val_retain(val); val_release(e->values[i]); e->values[i]=val; return;
        }
    }
    if (e->count>=MAX_VARS) { riven_error(0,"Variable limit"); return; }
    strncpy(e->names[e->count],name,MAX_IDENT_LEN-1);
    val_retain(val);
    e->values[e->count]=val;
    e->firm[e->count]=firm;
    e->count++;
}
void env_update(Env *e, const char *name, RivenValue *val) {
    for (Env *cur=e;cur;cur=cur->parent) {
        for (int i=0;i<cur->count;i++) {
            if (strcmp(cur->names[i],name)==0) {
                if (cur->firm[i]) { riven_error(0,"Cannot reassign firm '%s'",name); return; }
                /* If the slot holds a REF box, write through it */
                if (cur->values[i] && cur->values[i]->type==VAL_REF) {
                    val_ref_set(cur->values[i], val);
                } else {
                    val_retain(val); val_release(cur->values[i]); cur->values[i]=val;
                }
                return;
            }
        }
    }
    /* Not found — create in current scope */
    env_set(e, name, val, 0);
}

/* ========================= ERROR HELPERS ========================= */
