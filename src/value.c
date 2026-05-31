#include "../include/riven.h"

/* ========================= RECORD MAP ========================= */
void record_init(RecordMap *m) { m->keys=NULL; m->vals=NULL; m->count=m->capacity=0; }

void record_set(RecordMap *m, const char *key, RivenValue *val) {
    for (int i = 0; i < m->count; i++) {
        if (strcmp(m->keys[i], key) == 0) {
            val_retain(val); val_release(m->vals[i]); m->vals[i] = val; return;
        }
    }
    if (m->count >= m->capacity) {
        m->capacity = m->capacity ? m->capacity*2 : 4;
        m->keys = realloc(m->keys, sizeof(char*)       * m->capacity);
        m->vals = realloc(m->vals, sizeof(RivenValue*) * m->capacity);
    }
    m->keys[m->count] = strdup(key);
    val_retain(val);
    m->vals[m->count] = val;
    m->count++;
}

RivenValue *record_get(RecordMap *m, const char *key) {
    for (int i = 0; i < m->count; i++)
        if (strcmp(m->keys[i], key) == 0) return m->vals[i];
    return NULL;
}

void record_free(RecordMap *m) {
    for (int i = 0; i < m->count; i++) { free(m->keys[i]); val_release(m->vals[i]); }
    free(m->keys); free(m->vals);
    m->keys=NULL; m->vals=NULL; m->count=m->capacity=0;
}

/* ========================= FRAME DEFINITIONS ========================= */
FrameDef *framedef_new(const char *name) {
    FrameDef *fd = calloc(1, sizeof(FrameDef));
    strncpy(fd->name, name, MAX_IDENT_LEN-1);
    record_init(&fd->fields);
    record_init(&fd->methods);
    return fd;
}

void framedef_free(FrameDef *fd) {
    if (!fd) return;
    record_free(&fd->fields);
    record_free(&fd->methods);
    free(fd);
}

FrameDef *interp_find_frame(Interpreter *interp, const char *name) {
    for (int i = 0; i < interp->frame_def_count; i++)
        if (strcmp(interp->frame_defs[i]->name, name) == 0)
            return interp->frame_defs[i];
    return NULL;
}

void interp_add_frame(Interpreter *interp, FrameDef *fd) {
    /* Replace if exists */
    for (int i = 0; i < interp->frame_def_count; i++) {
        if (strcmp(interp->frame_defs[i]->name, fd->name) == 0) {
            framedef_free(interp->frame_defs[i]);
            interp->frame_defs[i] = fd;
            return;
        }
    }
    if (interp->frame_def_count < MAX_FRAMES)
        interp->frame_defs[interp->frame_def_count++] = fd;
    else
        riven_error(0, "Frame limit reached");
}

/* ========================= VALUE ALLOC ========================= */
RivenValue *val_alloc_pub(ValueType type) {
    RivenValue *v = calloc(1, sizeof(RivenValue));
    v->type = type; v->ref_count = 1;
    return v;
}
#define val_alloc val_alloc_pub

RivenValue *val_int(long long i)   { RivenValue *v=val_alloc(VAL_INT);   v->ival=i; return v; }
RivenValue *val_dnum(double d)     { RivenValue *v=val_alloc(VAL_DNUM);  v->dval=d; return v; }
RivenValue *val_bool(int b)        { RivenValue *v=val_alloc(VAL_BOOL);  v->bval=b; return v; }
RivenValue *val_null(void)         { return val_alloc(VAL_NULL); }

RivenValue *val_string(const char *s) {
    RivenValue *v=val_alloc(VAL_STRING); v->sval=strdup(s?s:""); return v;
}
RivenValue *val_error(const char *msg) {
    RivenValue *v=val_alloc(VAL_ERROR); v->error_msg=strdup(msg?msg:"unknown error"); return v;
}
RivenValue *val_coll_empty(void) {
    RivenValue *v=val_alloc(VAL_COLL);
    v->coll.items=NULL; v->coll.count=v->coll.capacity=0; return v;
}
void val_coll_push(RivenValue *coll, RivenValue *item) {
    if (coll->type!=VAL_COLL) return;
    if (coll->coll.count>=coll->coll.capacity) {
        coll->coll.capacity = coll->coll.capacity ? coll->coll.capacity*2 : 4;
        coll->coll.items = realloc(coll->coll.items, sizeof(RivenValue*)*coll->coll.capacity);
    }
    val_retain(item); coll->coll.items[coll->coll.count++]=item;
}
RivenValue *val_record_empty(void) {
    RivenValue *v=val_alloc(VAL_RECORD); record_init(&v->record); return v;
}
void val_record_set(RivenValue *rec, const char *key, RivenValue *val) {
    if (rec->type!=VAL_RECORD) return; record_set(&rec->record, key, val);
}
RivenValue *val_record_get(RivenValue *rec, const char *key) {
    if (rec->type!=VAL_RECORD) return NULL; return record_get(&rec->record, key);
}

/* ========================= REF (aliased mutable cell) ========================= */
/* A RivenBox is a heap-allocated single cell. Multiple VAL_REF values can point
   at the same box.  Writing through ANY alias updates ALL aliases instantly.     */

RivenValue *val_make_ref(RivenValue *initial) {
    RivenBox *box = calloc(1, sizeof(RivenBox));
    box->ref_count = 1;
    val_retain(initial);
    box->value = initial;
    RivenValue *v = val_alloc(VAL_REF);
    v->box = box;
    return v;
}

/* Create a second alias pointing at the SAME box */
RivenValue *val_alias_ref(RivenValue *src) {
    if (src->type != VAL_REF) return val_make_ref(src);  /* wrap non-ref in new box */
    RivenBox *box = src->box;
    box->ref_count++;
    RivenValue *v = val_alloc(VAL_REF);
    v->box = box;
    return v;
}

RivenValue *val_ref_get(RivenValue *ref) {
    if (!ref || ref->type != VAL_REF) return ref;
    return ref->box ? ref->box->value : val_null();
}

void val_ref_set(RivenValue *ref, RivenValue *newval) {
    if (!ref || ref->type != VAL_REF || !ref->box) return;
    val_retain(newval);
    val_release(ref->box->value);
    ref->box->value = newval;
}

/* Auto-deref: if v is VAL_REF, return the contained value; else return v.
   Used by the interpreter before every read so refs are transparent to users. */
RivenValue *val_deref(RivenValue *v) {
    if (v && v->type == VAL_REF && v->box) return v->box->value;
    return v;
}

/* ========================= PTR (raw pointer descriptor) ========================= */
/* A VAL_PTR carries an address, a symbolic tag, validity flag, and byte-width.
   In safe mode the address is forbidden to be dereferenced.
   In raw mode, the interpreter permits read/write through ptr.                   */

RivenValue *val_make_ptr(uintptr_t addr, const char *tag, size_t sz) {
    RivenValue *v = val_alloc(VAL_PTR);
    v->ptr.address  = addr;
    strncpy(v->ptr.tag, tag ? tag : "ptr", MAX_IDENT_LEN-1);
    v->ptr.is_valid = 1;
    v->ptr.size     = sz;
    return v;
}

/* ========================= RETAIN / RELEASE ========================= */
void val_retain(RivenValue *v) { if (v) v->ref_count++; }

void val_release(RivenValue *v) {
    if (!v) return;
    v->ref_count--;
    if (v->ref_count > 0) return;
    switch (v->type) {
        case VAL_STRING:    free(v->sval);       break;
        case VAL_ERROR:     free(v->error_msg);  break;
        case VAL_COLL:
            for (int i = 0; i < v->coll.count; i++) val_release(v->coll.items[i]);
            free(v->coll.items);
            break;
        case VAL_RECORD:    record_free(&v->record); break;
        case VAL_FRAME_OBJ:
            record_free(&v->frame_obj.fields);
            record_free(&v->frame_obj.methods);
            break;
        case VAL_FUNCTION:  /* body owned by AST */ break;
        case VAL_REF: {
            RivenBox *box = v->box;
            if (box) {
                box->ref_count--;
                if (box->ref_count <= 0) {
                    val_release(box->value);
                    free(box);
                }
            }
            break;
        }
        case VAL_PTR:  /* plain struct, nothing to free */ break;
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
        case VAL_INT:    return v->ival != 0;
        case VAL_DNUM:   return v->dval != 0.0;
        case VAL_STRING: return v->sval && strlen(v->sval) > 0;
        case VAL_NULL:   return 0;
        case VAL_ERROR:  return 0;
        case VAL_PTR:    return v->ptr.is_valid;
        default:         return 1;
    }
}

int val_equal(RivenValue *a, RivenValue *b) {
    a = val_deref(a); b = val_deref(b);
    if (!a || !b) return a == b;
    if (a->type==VAL_NULL && b->type==VAL_NULL) return 1;
    if (a->type==VAL_BOOL && b->type==VAL_BOOL) return a->bval==b->bval;
    if (a->type==VAL_INT  && b->type==VAL_INT)  return a->ival==b->ival;
    if (a->type==VAL_DNUM && b->type==VAL_DNUM) return a->dval==b->dval;
    if (a->type==VAL_INT  && b->type==VAL_DNUM) return (double)a->ival==b->dval;
    if (a->type==VAL_DNUM && b->type==VAL_INT)  return a->dval==(double)b->ival;
    if (a->type==VAL_STRING && b->type==VAL_STRING) return strcmp(a->sval,b->sval)==0;
    if (a->type==VAL_PTR  && b->type==VAL_PTR)  return a->ptr.address==b->ptr.address;
    return 0;
}

/* ========================= PRINTING ========================= */
char *val_to_string(RivenValue *v) {
    if (!v) return strdup("emp");
    /* Transparent deref for display */
    if (v->type == VAL_REF) v = val_deref(v);
    char buf[512];
    switch (v->type) {
        case VAL_INT:    snprintf(buf,512,"%lld",v->ival);   return strdup(buf);
        case VAL_DNUM:   snprintf(buf,512,"%g",  v->dval);   return strdup(buf);
        case VAL_STRING: return strdup(v->sval);
        case VAL_BOOL:   return strdup(v->bval?"correct":"incorrect");
        case VAL_NULL:   return strdup("emp");
        case VAL_ERROR:  snprintf(buf,512,"[ERROR: %s]",v->error_msg); return strdup(buf);
        case VAL_PTR:
            if (v->ptr.address)
                snprintf(buf,512,"<ptr:%s@0x%lx>", v->ptr.tag, (unsigned long)v->ptr.address);
            else
                snprintf(buf,512,"<ptr:%s>", v->ptr.tag);
            return strdup(buf);
        case VAL_COLL: {
            char tmp[MAX_STR_LEN]="[";
            for (int i=0;i<v->coll.count;i++) {
                char *s=val_to_string(v->coll.items[i]);
                strncat(tmp,s,MAX_STR_LEN-strlen(tmp)-1); free(s);
                if (i<v->coll.count-1) strncat(tmp,", ",MAX_STR_LEN-strlen(tmp)-1);
            }
            strncat(tmp,"]",MAX_STR_LEN-strlen(tmp)-1);
            return strdup(tmp);
        }
        case VAL_RECORD: {
            char tmp[MAX_STR_LEN]="{";
            for (int i=0;i<v->record.count;i++) {
                strncat(tmp,v->record.keys[i],MAX_STR_LEN-strlen(tmp)-1);
                strncat(tmp,": ",MAX_STR_LEN-strlen(tmp)-1);
                char *s=val_to_string(v->record.vals[i]);
                strncat(tmp,s,MAX_STR_LEN-strlen(tmp)-1); free(s);
                if (i<v->record.count-1) strncat(tmp,", ",MAX_STR_LEN-strlen(tmp)-1);
            }
            strncat(tmp,"}",MAX_STR_LEN-strlen(tmp)-1);
            return strdup(tmp);
        }
        case VAL_FRAME_OBJ:
            snprintf(buf,512,"<frame:%s>",v->frame_obj.frame_name); return strdup(buf);
        case VAL_FUNCTION:
            snprintf(buf,512,"<craft:%s>",v->fn.name); return strdup(buf);
        case VAL_NATIVE_FN: return strdup("<native-craft>");
        default: return strdup("?");
    }
}

void val_print(RivenValue *v) { char *s=val_to_string(v); printf("%s",s); free(s); }

RivenValue *val_copy(RivenValue *v) {
    if (!v) return val_null();
    /* Copying a ref creates an ALIAS (same box), not a deep copy */
    if (v->type == VAL_REF) return val_alias_ref(v);
    switch (v->type) {
        case VAL_INT:    return val_int(v->ival);
        case VAL_DNUM:   return val_dnum(v->dval);
        case VAL_STRING: return val_string(v->sval);
        case VAL_BOOL:   return val_bool(v->bval);
        case VAL_NULL:   return val_null();
        default:         val_retain(v); return v;
    }
}

/* ========================= ENVIRONMENT ========================= */
Env *env_new(Env *parent) {
    Env *e = calloc(1, sizeof(Env)); e->parent=parent; e->count=0; return e;
}

void env_free(Env *e) {
    if (!e) return;
    for (int i=0;i<e->count;i++) val_release(e->values[i]);
    free(e);
}

RivenValue *env_get(Env *e, const char *name) {
    for (Env *cur=e; cur; cur=cur->parent)
        for (int i=0;i<cur->count;i++)
            if (strcmp(cur->names[i],name)==0) return cur->values[i];
    return NULL;
}

int env_exists(Env *e, const char *name) {
    for (Env *cur=e;cur;cur=cur->parent)
        for (int i=0;i<cur->count;i++)
            if (strcmp(cur->names[i],name)==0) return 1;
    return 0;
}

int env_is_firm(Env *e, const char *name) {
    for (Env *cur=e;cur;cur=cur->parent)
        for (int i=0;i<cur->count;i++)
            if (strcmp(cur->names[i],name)==0) return cur->firm[i];
    return 0;
}

void env_set(Env *e, const char *name, RivenValue *val, int firm) {
    for (int i=0;i<e->count;i++) {
        if (strcmp(e->names[i],name)==0) {
            if (e->firm[i]) { riven_error(0,"Cannot reassign firm constant '%s'",name); return; }
            val_retain(val); val_release(e->values[i]); e->values[i]=val; return;
        }
    }
    if (e->count>=MAX_VARS) { riven_error(0,"Variable limit reached"); return; }
    strncpy(e->names[e->count],name,MAX_IDENT_LEN-1);
    val_retain(val); e->values[e->count]=val; e->firm[e->count]=firm; e->count++;
}

void env_update(Env *e, const char *name, RivenValue *val) {
    for (Env *cur=e;cur;cur=cur->parent) {
        for (int i=0;i<cur->count;i++) {
            if (strcmp(cur->names[i],name)==0) {
                if (cur->firm[i]) { riven_error(0,"Cannot reassign firm '%s'",name); return; }
                /* If the stored value is a VAL_REF, write through the box */
                if (cur->values[i] && cur->values[i]->type == VAL_REF) {
                    val_ref_set(cur->values[i], val);
                } else {
                    val_retain(val); val_release(cur->values[i]); cur->values[i]=val;
                }
                return;
            }
        }
    }
    env_set(e, name, val, 0);
}

/* Write through the box of a ref-typed variable without replacing the binding */
void env_ref_write(Env *e, const char *name, RivenValue *val) {
    for (Env *cur=e;cur;cur=cur->parent)
        for (int i=0;i<cur->count;i++)
            if (strcmp(cur->names[i],name)==0) {
                if (cur->values[i] && cur->values[i]->type==VAL_REF)
                    val_ref_set(cur->values[i], val);
                else
                    { val_retain(val); val_release(cur->values[i]); cur->values[i]=val; }
                return;
            }
    env_set(e, name, val, 0);
}

/* ========================= RAW MEMORY OPS ========================= */
RivenValue *raw_read_mem(uintptr_t addr, size_t sz) {
    /* Read sz bytes from address, return as int */
    uint64_t result = 0;
    if (sz > 8) sz = 8;
    memcpy(&result, (void*)addr, sz);
    return val_int((long long)result);
}

void raw_write_mem(uintptr_t addr, size_t sz, uint64_t val) {
    if (sz > 8) sz = 8;
    memcpy((void*)addr, &val, sz);
}
