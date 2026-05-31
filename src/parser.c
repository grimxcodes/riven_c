#include "../include/riven.h"

/* ========== HELPERS ========== */
Parser *parser_new(Token *tokens, int count) {
    Parser *p = calloc(1, sizeof(Parser));
    p->tokens = tokens; p->count = count; p->pos = 0;
    return p;
}
void parser_free(Parser *p) { free(p); }

static Token *p_peek(Parser *p) { return &p->tokens[p->pos]; }
static Token *p_peek2(Parser *p) {
    return (p->pos+1 < p->count) ? &p->tokens[p->pos+1] : &p->tokens[p->count-1];
}
static Token *p_advance(Parser *p) {
    Token *t = &p->tokens[p->pos];
    if (p->pos < p->count-1) p->pos++;
    return t;
}
static int p_check(Parser *p, TokenType t) { return p_peek(p)->type == t; }
static int p_match(Parser *p, TokenType t) {
    if (p_check(p, t)) { p_advance(p); return 1; } return 0;
}
static Token *p_expect(Parser *p, TokenType t, const char *msg) {
    if (!p_check(p, t)) {
        riven_error(p_peek(p)->line, "Expected %s but got '%s'", msg, p_peek(p)->value);
        exit(1);
    }
    return p_advance(p);
}

static ASTNode *new_node(NodeType type, int line) {
    ASTNode *n = calloc(1, sizeof(ASTNode));
    n->type = type; n->line = line;
    return n;
}

/* Forward declarations */
static ASTNode *parse_stmt(Parser *p);
static ASTNode *parse_expr(Parser *p);
static ASTNode *parse_assign_expr(Parser *p);
static ASTNode *parse_block(Parser *p);

/* ========== EXPRESSION PARSING ========== */

static ASTNode *parse_primary(Parser *p) {
    Token *t = p_peek(p);
    int line = t->line;

    /* Literals */
    if (p_check(p, TOK_INT)) {
        p_advance(p);
        ASTNode *n = new_node(NODE_INT_LIT, line);
        n->ival = atoll(t->value);
        return n;
    }
    if (p_check(p, TOK_DNUM)) {
        p_advance(p);
        ASTNode *n = new_node(NODE_DNUM_LIT, line);
        n->dval = atof(t->value);
        return n;
    }
    if (p_check(p, TOK_STRING)) {
        p_advance(p);
        ASTNode *n = new_node(NODE_STR_LIT, line);
        strncpy(n->sval, t->value, MAX_STR_LEN-1);
        return n;
    }
    if (p_check(p, TOK_CORRECT)) {
        p_advance(p);
        ASTNode *n = new_node(NODE_BOOL_LIT, line);
        n->bval = 1; return n;
    }
    if (p_check(p, TOK_INCORRECT)) {
        p_advance(p);
        ASTNode *n = new_node(NODE_BOOL_LIT, line);
        n->bval = 0; return n;
    }
    if (p_check(p, TOK_EMP)) {
        p_advance(p);
        return new_node(NODE_NULL_LIT, line);
    }

    /* Type casts: int(...), dnum(...), txt(...) */
    if (p_check(p, TOK_INT_TYPE) || p_check(p, TOK_DNUM_TYPE) || p_check(p, TOK_TXT_TYPE)) {
        char typ[32]; strncpy(typ, t->value, 31); p_advance(p);
        p_expect(p, TOK_LPAREN, "(");
        ASTNode *val = parse_expr(p);
        p_expect(p, TOK_RPAREN, ")");
        ASTNode *n = new_node(NODE_CAST, line);
        strncpy(n->cast.target_type, typ, 31);
        n->cast.value = val;
        return n;
    }

    /* fetch(...) */
    if (p_check(p, TOK_FETCH)) {
        p_advance(p);
        p_expect(p, TOK_LPAREN, "(");
        ASTNode *url = parse_expr(p);
        p_expect(p, TOK_RPAREN, ")");
        ASTNode *n = new_node(NODE_FETCH, line);
        n->fetch.url = url;
        return n;
    }

    /* grab(...) */
    if (p_check(p, TOK_GRAB)) {
        p_advance(p);
        p_expect(p, TOK_LPAREN, "(");
        ASTNode *prompt = parse_expr(p);
        p_expect(p, TOK_RPAREN, ")");
        ASTNode *n = new_node(NODE_GRAB, line);
        n->grab.prompt = prompt;
        return n;
    }

    /* spawn User() */
    if (p_check(p, TOK_SPAWN)) {
        p_advance(p);
        Token *name_tok = p_expect(p, TOK_IDENT, "frame name");
        ASTNode *n = new_node(NODE_SPAWN, line);
        strncpy(n->spawn.frame_name, name_tok->value, MAX_IDENT_LEN-1);
        n->spawn.arg_count = 0;
        if (p_match(p, TOK_LPAREN)) {
            while (!p_check(p, TOK_RPAREN) && !p_check(p, TOK_EOF)) {
                n->spawn.args[n->spawn.arg_count++] = parse_expr(p);
                if (!p_match(p, TOK_COMMA)) break;
            }
            p_expect(p, TOK_RPAREN, ")");
        }
        return n;
    }

    /* Parenthesized expression */
    if (p_check(p, TOK_LPAREN)) {
        p_advance(p);
        ASTNode *inner = parse_expr(p);
        p_expect(p, TOK_RPAREN, ")");
        return inner;
    }

    /* Collection literal [1,2,3] */
    if (p_check(p, TOK_LBRACKET)) {
        p_advance(p);
        ASTNode *n = new_node(NODE_COLL_LITERAL, line);
        nodelist_init(&n->coll.items);
        while (!p_check(p, TOK_RBRACKET) && !p_check(p, TOK_EOF)) {
            nodelist_push(&n->coll.items, parse_expr(p));
            if (!p_match(p, TOK_COMMA)) break;
        }
        p_expect(p, TOK_RBRACKET, "]");
        return n;
    }

    /* Record literal rec { key = val } */
    if (p_check(p, TOK_REC) && p_peek2(p)->type == TOK_LBRACE) {
        p_advance(p); p_advance(p);
        ASTNode *n = new_node(NODE_REC_LITERAL, line);
        n->rec.count = 0;
        while (!p_check(p, TOK_RBRACE) && !p_check(p, TOK_EOF)) {
            Token *kn = p_expect(p, TOK_IDENT, "key");
            strncpy(n->rec.keys[n->rec.count], kn->value, MAX_IDENT_LEN-1);
            p_expect(p, TOK_ASSIGN, "=");
            n->rec.vals[n->rec.count++] = parse_expr(p);
            p_match(p, TOK_COMMA);
        }
        p_expect(p, TOK_RBRACE, "}");
        return n;
    }

    /* Identifier */
    if (p_check(p, TOK_IDENT)) {
        p_advance(p);
        ASTNode *n = new_node(NODE_IDENT, line);
        strncpy(n->sval, t->value, MAX_IDENT_LEN-1);
        return n;
    }

    riven_error(line, "Unexpected token '%s' in expression", t->value);
    p_advance(p);
    return new_node(NODE_NULL_LIT, line);
}

static ASTNode *parse_postfix(Parser *p) {
    ASTNode *left = parse_primary(p);
    while (1) {
        int line = p_peek(p)->line;
        /* Member access: left.member */
        if (p_check(p, TOK_DOT)) {
            p_advance(p);
            Token *mem = p_expect(p, TOK_IDENT, "member name");
            ASTNode *n = new_node(NODE_MEMBER_ACCESS, line);
            n->member.obj = left;
            strncpy(n->member.member, mem->value, MAX_IDENT_LEN-1);
            left = n;
        }
        /* Index: left[expr] */
        else if (p_check(p, TOK_LBRACKET)) {
            p_advance(p);
            ASTNode *idx = parse_expr(p);
            p_expect(p, TOK_RBRACKET, "]");
            ASTNode *n = new_node(NODE_INDEX, line);
            n->index.obj = left; n->index.idx = idx;
            left = n;
        }
        /* Call: left(args) */
        else if (p_check(p, TOK_LPAREN)) {
            p_advance(p);
            ASTNode *n = new_node(NODE_CALL, line);
            n->call.callee = left; n->call.arg_count = 0;
            while (!p_check(p, TOK_RPAREN) && !p_check(p, TOK_EOF)) {
                n->call.args[n->call.arg_count++] = parse_expr(p);
                if (!p_match(p, TOK_COMMA)) break;
            }
            p_expect(p, TOK_RPAREN, ")");
            left = n;
        }
        else break;
    }
    return left;
}

static ASTNode *parse_unary(Parser *p) {
    int line = p_peek(p)->line;
    if (p_check(p, TOK_MINUS) || p_check(p, TOK_NOT_OP) || p_check(p, TOK_NOT)) {
        Token *op = p_advance(p);
        ASTNode *operand = parse_unary(p);
        ASTNode *n = new_node(NODE_UNOP, line);
        strncpy(n->unop.op, op->value, 7);
        n->unop.operand = operand;
        return n;
    }
    return parse_postfix(p);
}

static ASTNode *parse_mul(Parser *p) {
    ASTNode *left = parse_unary(p);
    while (p_check(p, TOK_STAR) || p_check(p, TOK_SLASH) || p_check(p, TOK_PERCENT)) {
        Token *op = p_advance(p); int line = op->line;
        ASTNode *right = parse_unary(p);
        ASTNode *n = new_node(NODE_BINOP, line);
        strncpy(n->binop.op, op->value, 7);
        n->binop.left = left; n->binop.right = right;
        left = n;
    }
    return left;
}

static ASTNode *parse_add(Parser *p) {
    ASTNode *left = parse_mul(p);
    while (p_check(p, TOK_PLUS) || p_check(p, TOK_MINUS)) {
        Token *op = p_advance(p); int line = op->line;
        ASTNode *right = parse_mul(p);
        ASTNode *n = new_node(NODE_BINOP, line);
        strncpy(n->binop.op, op->value, 7);
        n->binop.left = left; n->binop.right = right;
        left = n;
    }
    return left;
}

static ASTNode *parse_cmp(Parser *p) {
    ASTNode *left = parse_add(p);
    while (p_check(p, TOK_LT) || p_check(p, TOK_GT) ||
           p_check(p, TOK_LTE) || p_check(p, TOK_GTE) ||
           p_check(p, TOK_EQ) || p_check(p, TOK_NEQ)) {
        Token *op = p_advance(p); int line = op->line;
        ASTNode *right = parse_add(p);
        ASTNode *n = new_node(NODE_BINOP, line);
        strncpy(n->binop.op, op->value, 7);
        n->binop.left = left; n->binop.right = right;
        left = n;
    }
    return left;
}

static ASTNode *parse_logical(Parser *p) {
    ASTNode *left = parse_cmp(p);
    while (p_check(p, TOK_AND) || p_check(p, TOK_OR) ||
           p_check(p, TOK_AND_OP) || p_check(p, TOK_OR_OP)) {
        Token *op = p_advance(p); int line = op->line;
        const char *op_str = (op->type == TOK_AND || op->type == TOK_AND_OP) ? "and" : "or";
        ASTNode *right = parse_cmp(p);
        ASTNode *n = new_node(NODE_BINOP, line);
        strncpy(n->binop.op, op_str, 7);
        n->binop.left = left; n->binop.right = right;
        left = n;
    }
    return left;
}

static ASTNode *parse_expr(Parser *p) { return parse_logical(p); }

/* ========== STATEMENT PARSING ========== */

static ASTNode *parse_block(Parser *p) {
    int line = p_peek(p)->line;
    p_expect(p, TOK_LBRACE, "{");
    ASTNode *block = new_node(NODE_BLOCK, line);
    nodelist_init(&block->block.stmts);
    while (!p_check(p, TOK_RBRACE) && !p_check(p, TOK_EOF)) {
        ASTNode *s = parse_stmt(p);
        if (s) nodelist_push(&block->block.stmts, s);
    }
    p_expect(p, TOK_RBRACE, "}");
    return block;
}

static ASTNode *parse_stamp(Parser *p) {
    int line = p_peek(p)->line;
    p_advance(p); /* consume stamp */
    p_expect(p, TOK_LPAREN, "(");
    ASTNode *n = new_node(NODE_STAMP, line);
    n->stamp.fmt = parse_expr(p);
    n->stamp.arg_count = 0;
    while (p_match(p, TOK_COMMA)) {
        n->stamp.args[n->stamp.arg_count++] = parse_expr(p);
    }
    p_expect(p, TOK_RPAREN, ")");
    p_match(p, TOK_SEMICOLON);
    return n;
}

static ASTNode *parse_craft(Parser *p, int is_spark) {
    int line = p_peek(p)->line;
    p_advance(p); /* consume craft */
    Token *name = p_expect(p, TOK_IDENT, "function name");
    ASTNode *n = new_node(NODE_CRAFT, line);
    strncpy(n->craft.name, name->value, MAX_IDENT_LEN-1);
    n->craft.param_count = 0;
    n->craft.is_spark = is_spark;
    p_expect(p, TOK_LPAREN, "(");
    while (!p_check(p, TOK_RPAREN) && !p_check(p, TOK_EOF)) {
        Token *param = p_expect(p, TOK_IDENT, "parameter");
        strncpy(n->craft.params[n->craft.param_count++], param->value, MAX_IDENT_LEN-1);
        if (!p_match(p, TOK_COMMA)) break;
    }
    p_expect(p, TOK_RPAREN, ")");
    n->craft.body = parse_block(p);
    return n;
}

static ASTNode *parse_frame(Parser *p) {
    int line = p_peek(p)->line;
    p_advance(p); /* consume frame */
    Token *name = p_expect(p, TOK_IDENT, "frame name");
    ASTNode *n = new_node(NODE_FRAME, line);
    strncpy(n->frame.name, name->value, MAX_IDENT_LEN-1);
    nodelist_init(&n->frame.members);
    p_expect(p, TOK_LBRACE, "{");
    while (!p_check(p, TOK_RBRACE) && !p_check(p, TOK_EOF)) {
        ASTNode *m = parse_stmt(p);
        if (m) nodelist_push(&n->frame.members, m);
    }
    p_expect(p, TOK_RBRACE, "}");
    return n;
}

static ASTNode *parse_if(Parser *p) {
    int line = p_peek(p)->line;
    p_advance(p); /* consume if */
    ASTNode *cond = parse_expr(p);
    ASTNode *body = parse_block(p);
    ASTNode *n = new_node(NODE_IF, line);
    n->ifstmt.cond = cond; n->ifstmt.body = body;
    n->ifstmt.altifs = NULL; n->ifstmt.els = NULL;

    /* altif chains */
    ASTNode *last_if = n;
    while (p_check(p, TOK_ALTIF)) {
        int aline = p_peek(p)->line; p_advance(p);
        ASTNode *ac = parse_expr(p);
        ASTNode *ab = parse_block(p);
        ASTNode *alt = new_node(NODE_IF, aline);
        alt->ifstmt.cond = ac; alt->ifstmt.body = ab;
        alt->ifstmt.altifs = NULL; alt->ifstmt.els = NULL;
        last_if->ifstmt.altifs = alt;
        last_if = alt;
    }
    if (p_check(p, TOK_ELSE)) {
        p_advance(p);
        last_if->ifstmt.els = parse_block(p);
    }
    return n;
}

static ASTNode *parse_flow(Parser *p) {
    int line = p_peek(p)->line;
    p_advance(p); /* consume flow */
    ASTNode *count = parse_expr(p);
    ASTNode *body  = parse_block(p);
    ASTNode *n = new_node(NODE_FLOW, line);
    n->flow.count = count; n->flow.body = body;
    return n;
}

static ASTNode *parse_during(Parser *p) {
    int line = p_peek(p)->line;
    p_advance(p); /* consume during */
    ASTNode *cond = parse_expr(p);
    ASTNode *body = parse_block(p);
    ASTNode *n = new_node(NODE_DURING, line);
    n->during.cond = cond; n->during.body = body;
    return n;
}

static ASTNode *parse_stmt(Parser *p) {
    Token *t = p_peek(p);
    int line = t->line;

    p_match(p, TOK_SEMICOLON); /* skip stray semicolons */
    t = p_peek(p);

    if (p_check(p, TOK_EOF)) return NULL;

    /* consistof "file.rvh" */
    if (p_check(p, TOK_CONSISTOF)) {
        p_advance(p);
        Token *path = p_expect(p, TOK_STRING, "path string");
        ASTNode *n = new_node(NODE_CONSISTOF, line);
        strncpy(n->consistof.path, path->value, MAX_STR_LEN-1);
        return n;
    }

    /* riven core { } */
    if (p_check(p, TOK_RIVEN)) {
        p_advance(p);
        if (p_check(p, TOK_CORE)) {
            p_advance(p);
            return parse_block(p);
        }
        riven_error(line, "Expected 'core' after 'riven'");
        return NULL;
    }

    /* firm name = val */
    if (p_check(p, TOK_FIRM)) {
        p_advance(p);
        Token *name = p_expect(p, TOK_IDENT, "constant name");
        p_expect(p, TOK_ASSIGN, "=");
        ASTNode *val = parse_expr(p);
        ASTNode *n = new_node(NODE_FIRM, line);
        strncpy(n->assign.name, name->value, MAX_IDENT_LEN-1);
        n->assign.value = val; n->assign.is_firm = 1;
        p_match(p, TOK_SEMICOLON);
        return n;
    }

    /* craft */
    if (p_check(p, TOK_CRAFT)) return parse_craft(p, 0);

    /* spark craft */
    if (p_check(p, TOK_SPARK)) {
        p_advance(p);
        if (p_check(p, TOK_CRAFT)) return parse_craft(p, 1);
        /* spark expr; -- treat as expression statement */
    }

    /* frame */
    if (p_check(p, TOK_FRAME)) return parse_frame(p);

    /* if */
    if (p_check(p, TOK_IF)) return parse_if(p);

    /* flow */
    if (p_check(p, TOK_FLOW)) return parse_flow(p);

    /* during */
    if (p_check(p, TOK_DURING)) return parse_during(p);

    /* returns */
    if (p_check(p, TOK_RETURNS)) {
        p_advance(p);
        ASTNode *n = new_node(NODE_RETURN, line);
        n->ret.value = (!p_check(p, TOK_SEMICOLON) && !p_check(p, TOK_RBRACE))
                       ? parse_expr(p) : NULL;
        p_match(p, TOK_SEMICOLON);
        return n;
    }

    /* stamp */
    if (p_check(p, TOK_STAMP)) return parse_stamp(p);

    /* rise name */
    if (p_check(p, TOK_RISE)) {
        p_advance(p);
        Token *name = p_expect(p, TOK_IDENT, "variable name");
        ASTNode *n = new_node(NODE_RISE, line);
        strncpy(n->incdec.name, name->value, MAX_IDENT_LEN-1);
        return n;
    }

    /* drop name */
    if (p_check(p, TOK_DROP)) {
        p_advance(p);
        Token *name = p_expect(p, TOK_IDENT, "variable name");
        ASTNode *n = new_node(NODE_DROP, line);
        strncpy(n->incdec.name, name->value, MAX_IDENT_LEN-1);
        return n;
    }

    /* resc { } */
    if (p_check(p, TOK_RESC)) {
        p_advance(p);
        ASTNode *n = new_node(NODE_RESC, line);
        n->resc.body = parse_block(p);
        return n;
    }

    /* attack("msg") */
    if (p_check(p, TOK_ATTACK)) {
        p_advance(p);
        p_expect(p, TOK_LPAREN, "(");
        ASTNode *msg = parse_expr(p);
        p_expect(p, TOK_RPAREN, ")");
        ASTNode *n = new_node(NODE_ATTACK, line);
        n->attack.msg = msg;
        p_match(p, TOK_SEMICOLON);
        return n;
    }

    /* sync; */
    if (p_check(p, TOK_SYNC)) {
        p_advance(p); p_match(p, TOK_SEMICOLON);
        return new_node(NODE_SYNC, line);
    }

    /* raw { } */
    if (p_check(p, TOK_RAW)) {
        p_advance(p);
        ASTNode *n = new_node(NODE_RAW, line);
        n->raw.body = parse_block(p);
        return n;
    }

    /* ref name = expr */
    if (p_check(p, TOK_REF)) {
        p_advance(p);
        Token *name = p_expect(p, TOK_IDENT, "ref name");
        p_expect(p, TOK_ASSIGN, "=");
        ASTNode *val = parse_expr(p);
        ASTNode *n = new_node(NODE_REF, line);
        strncpy(n->refptr.name, name->value, MAX_IDENT_LEN-1);
        n->refptr.value = val;
        p_match(p, TOK_SEMICOLON);
        return n;
    }

    /* ptr name = expr */
    if (p_check(p, TOK_PTR)) {
        p_advance(p);
        Token *name = p_expect(p, TOK_IDENT, "ptr name");
        p_expect(p, TOK_ASSIGN, "=");
        ASTNode *val = parse_expr(p);
        ASTNode *n = new_node(NODE_PTR, line);
        strncpy(n->refptr.name, name->value, MAX_IDENT_LEN-1);
        n->refptr.value = val;
        p_match(p, TOK_SEMICOLON);
        return n;
    }

    /* bind name = expr */
    if (p_check(p, TOK_BIND)) {
        p_advance(p);
        Token *name = p_expect(p, TOK_IDENT, "bind name");
        p_expect(p, TOK_ASSIGN, "=");
        ASTNode *val = parse_expr(p);
        ASTNode *n = new_node(NODE_BIND, line);
        strncpy(n->refptr.name, name->value, MAX_IDENT_LEN-1);
        n->refptr.value = val;
        p_match(p, TOK_SEMICOLON);
        return n;
    }

    /* open / hidden — access specifiers inside frame bodies */
    if (p_check(p, TOK_OPEN) || p_check(p, TOK_HIDDEN)) {
        int hidden = p_check(p, TOK_HIDDEN); p_advance(p);
        const char *acc_prefix = hidden ? "__hidden__" : "__open__";

        if (p_check(p, TOK_CRAFT)) {
            ASTNode *fn = parse_craft(p, 0);
            /* Prefix encodes access level; interpreter strips it when registering */
            char tagged[MAX_IDENT_LEN];
            snprintf(tagged, MAX_IDENT_LEN, "%s%s", acc_prefix, fn->craft.name);
            strncpy(fn->craft.name, tagged, MAX_IDENT_LEN-1);
            return fn;
        }
        /* open/hidden field declaration: open name = expr */
        Token *name = p_expect(p, TOK_IDENT, "field name");
        p_expect(p, TOK_ASSIGN, "=");
        ASTNode *val = parse_expr(p);
        ASTNode *n = new_node(NODE_ASSIGN, line);
        char tagged[MAX_IDENT_LEN];
        snprintf(tagged, MAX_IDENT_LEN, "%s%s", acc_prefix, name->value);
        strncpy(n->assign.name, tagged, MAX_IDENT_LEN-1);
        n->assign.value = val; n->assign.is_firm = 0;
        p_match(p, TOK_SEMICOLON);
        return n;
    }

    /* boot(params...) { } — constructor, may have parameters */
    if (p_check(p, TOK_BOOT)) {
        p_advance(p);
        ASTNode *n = new_node(NODE_CRAFT, line);
        strncpy(n->craft.name, "__boot__", MAX_IDENT_LEN-1);
        n->craft.param_count = 0; n->craft.is_spark = 0;
        p_expect(p, TOK_LPAREN, "(");
        while (!p_check(p, TOK_RPAREN) && !p_check(p, TOK_EOF)) {
            Token *param = p_expect(p, TOK_IDENT, "parameter");
            strncpy(n->craft.params[n->craft.param_count++], param->value, MAX_IDENT_LEN-1);
            if (!p_match(p, TOK_COMMA)) break;
        }
        p_expect(p, TOK_RPAREN, ")");
        n->craft.body = parse_block(p);
        return n;
    }

    /* Expression statement: assignment or call */
    ASTNode *expr = parse_expr(p);

    /* Check for assignment: expr = value or name += etc */
    if (p_check(p, TOK_ASSIGN)) {
        p_advance(p);
        ASTNode *val = parse_expr(p);
        p_match(p, TOK_SEMICOLON);
        /* If left side is ident, make simple assign */
        if (expr->type == NODE_IDENT) {
            ASTNode *n = new_node(NODE_ASSIGN, line);
            strncpy(n->assign.name, expr->sval, MAX_IDENT_LEN-1);
            n->assign.value = val; n->assign.is_firm = 0;
            ast_free(expr);
            return n;
        }
        /* member assign or index assign: wrap as binop-style assign */
        /* We handle obj.field = val as a special assign */
        ASTNode *n = new_node(NODE_ASSIGN, line);
        strncpy(n->assign.name, "__lhs__", MAX_IDENT_LEN-1);
        n->assign.value = val; n->assign.is_firm = 0;
        /* Pack left side as refptr.value for interpreter to handle */
        ASTNode *wrap = new_node(NODE_BINOP, line);
        strncpy(wrap->binop.op, "=", 7);
        wrap->binop.left = expr;
        wrap->binop.right = val;
        ast_free(n);
        p_match(p, TOK_SEMICOLON);
        return wrap;
    }

    /* name+> or name-< as standalone statements */
    if (p_check(p, TOK_INC)) {
        p_advance(p);
        if (expr->type == NODE_IDENT) {
            ASTNode *n = new_node(NODE_INC, line);
            strncpy(n->incdec.name, expr->sval, MAX_IDENT_LEN-1);
            ast_free(expr);
            return n;
        }
    }
    if (p_check(p, TOK_DEC)) {
        p_advance(p);
        if (expr->type == NODE_IDENT) {
            ASTNode *n = new_node(NODE_DEC, line);
            strncpy(n->incdec.name, expr->sval, MAX_IDENT_LEN-1);
            ast_free(expr);
            return n;
        }
    }

    p_match(p, TOK_SEMICOLON);
    return expr; /* expression statement */
}

ASTNode *parser_parse(Parser *p) {
    ASTNode *prog = new_node(NODE_PROGRAM, 1);
    nodelist_init(&prog->block.stmts);
    while (!p_check(p, TOK_EOF)) {
        ASTNode *s = parse_stmt(p);
        if (s) nodelist_push(&prog->block.stmts, s);
    }
    return prog;
}

/* ===== AST CLEANUP ===== */
void ast_free(ASTNode *n) {
    if (!n) return;
    switch (n->type) {
        case NODE_BINOP:  ast_free(n->binop.left); ast_free(n->binop.right); break;
        case NODE_UNOP:   ast_free(n->unop.operand); break;
        case NODE_ASSIGN: case NODE_FIRM: ast_free(n->assign.value); break;
        case NODE_IF:     ast_free(n->ifstmt.cond); ast_free(n->ifstmt.body);
                          ast_free(n->ifstmt.altifs); ast_free(n->ifstmt.els); break;
        case NODE_FLOW:   ast_free(n->flow.count); ast_free(n->flow.body); break;
        case NODE_DURING: ast_free(n->during.cond); ast_free(n->during.body); break;
        case NODE_CRAFT:  ast_free(n->craft.body); break;
        case NODE_RETURN: ast_free(n->ret.value); break;
        case NODE_CALL:
            ast_free(n->call.callee);
            for (int i = 0; i < n->call.arg_count; i++) ast_free(n->call.args[i]);
            break;
        case NODE_STAMP:
            ast_free(n->stamp.fmt);
            for (int i = 0; i < n->stamp.arg_count; i++) ast_free(n->stamp.args[i]);
            break;
        case NODE_PROGRAM: case NODE_BLOCK:
            for (int i = 0; i < n->block.stmts.count; i++) ast_free(n->block.stmts.items[i]);
            nodelist_free(&n->block.stmts);
            break;
        case NODE_FRAME:
            for (int i = 0; i < n->frame.members.count; i++) ast_free(n->frame.members.items[i]);
            nodelist_free(&n->frame.members);
            break;
        case NODE_SPAWN:
            for (int i = 0; i < n->spawn.arg_count; i++) ast_free(n->spawn.args[i]);
            break;
        case NODE_MEMBER_ACCESS: ast_free(n->member.obj); break;
        case NODE_INDEX: ast_free(n->index.obj); ast_free(n->index.idx); break;
        case NODE_COLL_LITERAL:
            for (int i = 0; i < n->coll.items.count; i++) ast_free(n->coll.items.items[i]);
            nodelist_free(&n->coll.items);
            break;
        case NODE_REC_LITERAL:
            for (int i = 0; i < n->rec.count; i++) ast_free(n->rec.vals[i]);
            break;
        case NODE_GRAB: ast_free(n->grab.prompt); break;
        case NODE_FETCH: ast_free(n->fetch.url); break;
        case NODE_RESC: ast_free(n->resc.body); break;
        case NODE_ATTACK: ast_free(n->attack.msg); break;
        case NODE_RAW: ast_free(n->raw.body); break;
        case NODE_CAST: ast_free(n->cast.value); break;
        case NODE_REF: case NODE_PTR: case NODE_BIND: ast_free(n->refptr.value); break;
        default: break;
    }
    free(n);
}

/* ===== AST PRINT (debug) ===== */
void ast_print(ASTNode *n, int indent) {
    if (!n) return;
    for (int i = 0; i < indent; i++) printf("  ");
    switch (n->type) {
        case NODE_INT_LIT:  printf("INT(%lld)\n", n->ival); break;
        case NODE_DNUM_LIT: printf("DNUM(%g)\n", n->dval); break;
        case NODE_STR_LIT:  printf("STR(\"%s\")\n", n->sval); break;
        case NODE_BOOL_LIT: printf("BOOL(%s)\n", n->bval ? "correct":"incorrect"); break;
        case NODE_NULL_LIT: printf("EMP\n"); break;
        case NODE_IDENT:    printf("IDENT(%s)\n", n->sval); break;
        case NODE_BINOP:    printf("BINOP(%s)\n", n->binop.op);
                            ast_print(n->binop.left, indent+1);
                            ast_print(n->binop.right, indent+1); break;
        case NODE_ASSIGN:   printf("ASSIGN(%s)\n", n->assign.name);
                            ast_print(n->assign.value, indent+1); break;
        case NODE_CRAFT:    printf("CRAFT(%s)\n", n->craft.name); break;
        case NODE_CALL:     printf("CALL\n"); ast_print(n->call.callee, indent+1); break;
        case NODE_STAMP:    printf("STAMP\n"); ast_print(n->stamp.fmt, indent+1); break;
        default:            printf("NODE(%d)\n", n->type); break;
    }
}
