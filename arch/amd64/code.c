/*	$Id$	*/
/*
 * Copyright (c) 2008 Michael Shalayeff
 * Copyright (c) 2003 Anders Magnusson (ragge@ludd.luth.se).
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


# include "pass1.h"

static int nsse, ngpr, nrsp, rsaoff;
static int thissse, thisgpr, thisrsp;
enum { INTEGER = 1, INTMEM, SSE, SSEMEM, X87, STRREG, STRMEM };
static const int argregsi[] = { RDI, RSI, RDX, RCX, R08, R09 };
/*
 * The Register Save Area looks something like this.
 * It is put first on stack with fixed offsets.
 * struct {
 *	long regs[6];
 *	double xmm[8][2]; // 16 byte in width
 * };
 */
#define	RSASZ		(6*SZLONG+8*2*SZDOUBLE)
#define	RSALONGOFF(x)	(RSASZ-(x)*SZLONG)
#define	RSADBLOFF(x)	((8*2*SZDOUBLE)-(x)*SZDOUBLE*2)
/* va_list */
#define	VAARGSZ		(SZINT*2+SZPOINT(CHAR)*2)
#define	VAGPOFF(x)	(x)
#define	VAFPOFF(x)	(x-SZINT)
#define	VAOFA(x)	(x-SZINT-SZINT)
#define	VARSA(x)	(x-SZINT-SZINT-SZPOINT(0))

int lastloc = -1;
static int stroffset;

static int argtyp(TWORD t, union dimfun *df, struct attr *ap);
static NODE *movtomem(NODE *p, int off, int reg);
static NODE *movtoreg(NODE *p, int rno);

/*
 * Define everything needed to print out some data (or text).
 * This means segment, alignment, visibility, etc.
 */
void
defloc(struct symtab *sp)
{
	extern char *nextsect;
	static char *loctbl[] = { "text", "data", "section .rodata" };
	int weak = 0;
	char *name;
	TWORD t;
	int s;

	if (sp == NULL) {
		lastloc = -1;
		return;
	}
	if (kflag) {
		loctbl[DATA] = "section .data.rel.rw,\"aw\",@progbits";
		loctbl[RDATA] = "section .data.rel.ro,\"aw\",@progbits";
	}
	t = sp->stype;
	s = ISFTN(t) ? PROG : ISCON(cqual(t, sp->squal)) ? RDATA : DATA;
	if ((name = sp->soname) == NULL)
		name = exname(sp->sname);
#ifdef TLS
	if (sp->sflags & STLS) {
		if (s != DATA)
			cerror("non-data symbol in tls section");
		nextsect = ".tdata";
	}
#endif
#ifdef GCC_COMPAT
	{
		struct attr *ga;

		if ((ga = attr_find(sp->sap, GCC_ATYP_SECTION)) != NULL)
			nextsect = ga->sarg(0);
		if ((ga = attr_find(sp->sap, GCC_ATYP_WEAK)) != NULL)
			weak = 1;
		if (attr_find(sp->sap, GCC_ATYP_DESTRUCTOR)) {
			printf("\t.section\t.dtors,\"aw\",@progbits\n");
			printf("\t.align 8\n\t.quad\t%s\n", name);
			lastloc = -1;
		}
		if (attr_find(sp->sap, GCC_ATYP_CONSTRUCTOR)) {
			printf("\t.section\t.ctors,\"aw\",@progbits\n");
			printf("\t.align 8\n\t.quad\t%s\n", name);
			lastloc = -1;
		}
		if ((ga = attr_find(sp->sap, GCC_ATYP_VISIBILITY)) &&
		    strcmp(ga->sarg(0), "default"))
			printf("\t.%s %s\n", ga->sarg(0), name);
	}
#endif

	if (nextsect) {
		printf("	.section %s\n", nextsect);
		nextsect = NULL;
		s = -1;
	} else if (s != lastloc)
		printf("	.%s\n", loctbl[s]);
	lastloc = s;
	while (ISARY(t))
		t = DECREF(t);
	s = ISFTN(t) ? ALINT : talign(t, sp->sap);
	if (s > ALCHAR)
		printf("	.align %d\n", s/ALCHAR);
	if (weak)
		printf("        .weak %s\n", name);
	else if (sp->sclass == EXTDEF) {
		printf("\t.globl %s\n", name);
		printf("\t.type %s,@%s\n", name,
		    ISFTN(t)? "function" : "object");
	}
	if (sp->slevel == 0)
		printf("%s:\n", name);
	else
		printf(LABFMT ":\n", sp->soffset);
}

/*
 * code for the end of a function
 * deals with struct return here
 */
void
efcode()
{
	struct symtab *sp;
	extern int gotnr;
	TWORD t;
	NODE *p, *r, *l;
	int o;

	gotnr = 0;	/* new number for next fun */
	sp = cftnsp;
	t = DECREF(sp->stype);
	if (t != STRTY && t != UNIONTY)
		return;
	if (argtyp(t, sp->sdf, sp->sap) != STRMEM)
		return;

	/* call memcpy() to put return struct at destination */
#define  cmop(x,y) block(CM, x, y, INT, 0, MKAP(INT))
	r = tempnode(stroffset, INCREF(t), sp->sdf, sp->sap);
	l = block(REG, NIL, NIL, INCREF(t), sp->sdf, sp->sap);
	regno(l) = RAX;
	o = tsize(t, sp->sdf, sp->sap)/SZCHAR;
	r = cmop(cmop(r, l), bcon(o));

	blevel++; /* Remove prototype at exit of function */
	sp = lookup(addname("memcpy"), 0);
	if (sp->stype == UNDEF) {
		sp->sap = MKAP(VOID);
		p = talloc();
		p->n_op = NAME;
		p->n_sp = sp;
		p->n_ap = sp->sap;
		p->n_type = VOID|FTN|(PTR<<TSHIFT);
		defid(p, EXTERN);
		nfree(p);
	}
	blevel--;

	p = doacall(sp, nametree(sp), r);
	ecomp(p);
	

	/* RAX contains destination after memcpy() */
}

/*
 * code for the beginning of a function; a is an array of
 * indices in symtab for the arguments; n is the number
 */
void
bfcode(struct symtab **s, int cnt)
{
	union arglist *al;
	struct symtab *sp;
	NODE *p, *r;
	int i, rno, typ;

	/* recalculate the arg offset and create TEMP moves */
	/* Always do this for reg, even if not optimizing, to free arg regs */
	nsse = ngpr = 0;
	nrsp = ARGINIT;
	if (cftnsp->stype == STRTY+FTN || cftnsp->stype == UNIONTY+FTN) {
		sp = cftnsp;
		if (argtyp(DECREF(sp->stype), sp->sdf, sp->sap) == STRMEM) {
			r = block(REG, NIL, NIL, LONG, 0, MKAP(LONG));
			regno(r) = argregsi[ngpr++];
			p = tempnode(0, r->n_type, r->n_df, r->n_ap);
			stroffset = regno(p);
			ecomp(buildtree(ASSIGN, p, r));
		}
	}

	for (i = 0; i < cnt; i++) {
		sp = s[i];

		if (sp == NULL)
			continue; /* XXX when happens this? */

		switch (typ = argtyp(sp->stype, sp->sdf, sp->sap)) {
		case INTEGER:
		case SSE:
			if (typ == SSE)
				rno = XMM0 + nsse++;
			else
				rno = argregsi[ngpr++];
			r = block(REG, NIL, NIL, sp->stype, sp->sdf, sp->sap);
			regno(r) = rno;
			p = tempnode(0, sp->stype, sp->sdf, sp->sap);
			sp->soffset = regno(p);
			sp->sflags |= STNODE;
			ecomp(buildtree(ASSIGN, p, r));
			break;

		case SSEMEM:
			sp->soffset = nrsp;
			nrsp += SZDOUBLE;
			if (xtemps) {
				p = tempnode(0, sp->stype, sp->sdf, sp->sap);
				p = buildtree(ASSIGN, p, nametree(sp));
				sp->soffset = regno(p->n_left);
				sp->sflags |= STNODE;
				ecomp(p);
			}
			break;

		case INTMEM:
			sp->soffset = nrsp;
			nrsp += SZLONG;
			if (xtemps) {
				p = tempnode(0, sp->stype, sp->sdf, sp->sap);
				p = buildtree(ASSIGN, p, nametree(sp));
				sp->soffset = regno(p->n_left);
				sp->sflags |= STNODE;
				ecomp(p);
			}
			break;

		case STRMEM: /* Struct in memory */
			sp->soffset = nrsp;
			nrsp += tsize(sp->stype, sp->sdf, sp->sap);
			break;

		case STRREG: /* Struct in register */
			/* Allocate space on stack for the struct */
			/* For simplicity always fetch two longwords */
			autooff += (2*SZLONG);

			r = block(REG, NIL, NIL, LONG, 0, MKAP(LONG));
			regno(r) = argregsi[ngpr++];
			ecomp(movtomem(r, -autooff, FPREG));

			if (tsize(sp->stype, sp->sdf, sp->sap) > SZLONG) {
				r = block(REG, NIL, NIL, LONG, 0, MKAP(LONG));
				regno(r) = argregsi[ngpr++];
				ecomp(movtomem(r, -autooff+SZLONG, FPREG));
			}

			sp->soffset = -autooff;
			break;

		default:
			cerror("bfcode: %d", typ);
		}
	}

	/* Check if there are varargs */
	if (cftnsp->sdf == NULL || cftnsp->sdf->dfun == NULL)
		return; /* no prototype */
	al = cftnsp->sdf->dfun;

	for (; al->type != TELLIPSIS; al++) {
		if (al->type == TNULL)
			return;
		if (BTYPE(al->type) == STRTY || BTYPE(al->type) == UNIONTY ||
		    ISARY(al->type))
			al++;
	}

	/* fix stack offset */
	SETOFF(autooff, ALMAX);

	/* Save reg arguments in the reg save area */
	p = NIL;
	for (i = ngpr; i < 6; i++) {
		r = block(REG, NIL, NIL, LONG, 0, MKAP(LONG));
		regno(r) = argregsi[i];
		r = movtomem(r, -RSALONGOFF(i)-autooff, FPREG);
		p = (p == NIL ? r : block(COMOP, p, r, INT, 0, MKAP(INT)));
	}
	for (i = nsse; i < 8; i++) {
		r = block(REG, NIL, NIL, DOUBLE, 0, MKAP(DOUBLE));
		regno(r) = i + XMM0;
		r = movtomem(r, -RSADBLOFF(i)-autooff, FPREG);
		p = (p == NIL ? r : block(COMOP, p, r, INT, 0, MKAP(INT)));
	}
	autooff += RSASZ;
	rsaoff = autooff;
	thissse = nsse;
	thisgpr = ngpr;
	thisrsp = nrsp;

	ecomp(p);
}


/*
 * by now, the automatics and register variables are allocated
 */
void
bccode()
{
	SETOFF(autooff, SZINT);
}

/* called just before final exit */
/* flag is 1 if errors, 0 if none */
void
ejobcode(int flag )
{
#define _MKSTR(x) #x
#define MKSTR(x) _MKSTR(x)
#define OS MKSTR(TARGOS)
        printf("\t.ident \"PCC: %s (%s)\"\n\t.end\n", PACKAGE_STRING, OS);
}

/*
 * Varargs stuff:
 * The ABI says that va_list should be declared as this typedef.
 * We handcraft it here and then just reference it.
 *
 * typedef struct {
 *	unsigned int gp_offset;
 *	unsigned int fp_offset;
 *	void *overflow_arg_area;
 *	void *reg_save_area;
 * } __builtin_va_list[1];
 */

static char *gp_offset, *fp_offset, *overflow_arg_area, *reg_save_area;

void
bjobcode()
{
	struct rstack *rp;
	NODE *p, *q;
	char *c;

	gp_offset = addname("gp_offset");
	fp_offset = addname("fp_offset");
	overflow_arg_area = addname("overflow_arg_area");
	reg_save_area = addname("reg_save_area");

	rp = bstruct(NULL, STNAME, NULL);
	p = block(NAME, NIL, NIL, UNSIGNED, 0, MKAP(UNSIGNED));
	soumemb(p, gp_offset, 0);
	soumemb(p, fp_offset, 0);
	p->n_type = VOID+PTR;
	p->n_ap = MKAP(VOID);
	soumemb(p, overflow_arg_area, 0);
	soumemb(p, reg_save_area, 0);
	nfree(p);
	q = dclstruct(rp);
	c = addname("__builtin_va_list");
	p = block(LB, bdty(NAME, c), bcon(1), INT, 0, MKAP(INT));
	p = tymerge(q, p);
	p->n_sp = lookup(c, 0);
	defid(p, TYPEDEF);
	nfree(q);
	nfree(p);
}

static NODE *
mkstkref(int off, TWORD typ)
{
	NODE *p;

	p = block(REG, NIL, NIL, PTR|typ, 0, MKAP(LONG));
	regno(p) = FPREG;
	return buildtree(PLUS, p, bcon(off/SZCHAR));
}

NODE *
amd64_builtin_stdarg_start(NODE *f, NODE *a, TWORD t)
{
	NODE *p, *r;

	/* use the values from the function header */
	p = a->n_left;
	r = buildtree(ASSIGN, structref(ccopy(p), STREF, reg_save_area),
	    mkstkref(-rsaoff, VOID));
	r = buildtree(COMOP, r,
	    buildtree(ASSIGN, structref(ccopy(p), STREF, overflow_arg_area),
	    mkstkref(thisrsp, VOID)));
	r = buildtree(COMOP, r,
	    buildtree(ASSIGN, structref(ccopy(p), STREF, gp_offset),
	    bcon(thisgpr*(SZLONG/SZCHAR))));
	r = buildtree(COMOP, r,
	    buildtree(ASSIGN, structref(ccopy(p), STREF, fp_offset),
	    bcon(thissse*(SZDOUBLE*2/SZCHAR)+48)));

	tfree(f);
	tfree(a);
	return r;
}

/*
 * Create a tree that should be like the expression
 *	((long *)(l->gp_offset >= 48 ?
 *	    l->overflow_arg_area += 8, l->overflow_arg_area :
 *	    l->gp_offset += 8, l->reg_save_area + l->gp_offset))[-1]
 * ...or similar for floats.
 */
static NODE *
bva(NODE *ap, NODE *dp, char *ot, int addto, int max)
{
	NODE *cm1, *cm2, *gpo, *ofa, *l1, *qc;
	TWORD nt;

	ofa = structref(ccopy(ap), STREF, overflow_arg_area);
	l1 = buildtree(PLUSEQ, ccopy(ofa), bcon(addto));
	cm1 = buildtree(COMOP, l1, ofa);

	gpo = structref(ccopy(ap), STREF, ot);
	l1 = buildtree(PLUSEQ, ccopy(gpo), bcon(addto));
	cm2 = buildtree(COMOP, l1, buildtree(PLUS, ccopy(gpo),
	    structref(ccopy(ap), STREF, reg_save_area)));
	qc = buildtree(QUEST,
	    buildtree(GE, gpo, bcon(max)),
	    buildtree(COLON, cm1, cm2));

	nt = (dp->n_type == DOUBLE ? DOUBLE : LONG);
	l1 = block(NAME, NIL, NIL, nt|PTR, 0, MKAP(nt));
	l1 = buildtree(CAST, l1, qc);
	qc = l1->n_right;
	nfree(l1->n_left);
	nfree(l1);

	/* qc has now a real type, for indexing */
	addto = dp->n_type == DOUBLE ? 2 : 1;
	qc = buildtree(UMUL, buildtree(PLUS, qc, bcon(-addto)), NIL);

	l1 = block(NAME, NIL, NIL, dp->n_type, dp->n_df, dp->n_ap);
	l1 = buildtree(CAST, l1, qc);
	qc = l1->n_right;
	nfree(l1->n_left);
	nfree(l1);

	return qc;
}

NODE *
amd64_builtin_va_arg(NODE *f, NODE *a, TWORD t)
{
	NODE *ap, *r, *dp;

	ap = a->n_left;
	dp = a->n_right;
	if (dp->n_type <= ULONGLONG || ISPTR(dp->n_type)) {
		/* type might be in general register */
		r = bva(ap, dp, gp_offset, 8, 48);
	} else if (dp->n_type == FLOAT || dp->n_type == DOUBLE) {
		/* Float are promoted to double here */
		if (dp->n_type == FLOAT)
			dp->n_type = DOUBLE;
		r = bva(ap, dp, fp_offset, 16, RSASZ/SZCHAR);
	} else {
		uerror("amd64_builtin_va_arg not supported type");
		goto bad;
	}
	tfree(a);
	tfree(f);
	return r;
bad:
	uerror("bad argument to __builtin_va_arg");
	return bcon(0);
}

NODE *
amd64_builtin_va_end(NODE *f, NODE *a, TWORD t)
{
	tfree(f);
	tfree(a);
	return bcon(0); /* nothing */
}

NODE *
amd64_builtin_va_copy(NODE *f, NODE *a, TWORD t) { cerror("amd64_builtin_va_copy"); return NULL; }

static NODE *
movtoreg(NODE *p, int rno)
{
	NODE *r;

	r = block(REG, NIL, NIL, p->n_type, p->n_df, p->n_ap);
	regno(r) = rno;
	return clocal(buildtree(ASSIGN, r, p));
}  

static NODE *
movtomem(NODE *p, int off, int reg)
{
	struct symtab s;
	NODE *r, *l;

	s.stype = p->n_type;
	s.squal = 0;
	s.sdf = p->n_df;
	s.sap = p->n_ap;
	s.soffset = off;
	s.sclass = AUTO;

	l = block(REG, NIL, NIL, PTR+STRTY, 0, 0);
	l->n_lval = 0;
	regno(l) = reg;

	r = block(NAME, NIL, NIL, p->n_type, p->n_df, p->n_ap);
	r->n_sp = &s;
	r = stref(block(STREF, l, r, 0, 0, 0));

	return clocal(buildtree(ASSIGN, r, p));
}  


/*
 * AMD64 parameter classification.
 */
static int
argtyp(TWORD t, union dimfun *df, struct attr *ap)
{
	int cl = 0;

	if (t <= ULONG || ISPTR(t)) {
		cl = ngpr < 6 ? INTEGER : INTMEM;
	} else if (t == FLOAT || t == DOUBLE) {
		cl = nsse < 8 ? SSE : SSEMEM;
	} else if (t == LDOUBLE) {
		cl = X87; /* XXX */
	} else if (t == STRTY || t == UNIONTY) {
		/* XXX no SSEOP handling */
		if ((tsize(t, df, ap) > 2*SZLONG) ||
		    (attr_find(ap, GCC_ATYP_PACKED) != NULL))
			cl = STRMEM;
		else
			cl = STRREG;
	} else
		cerror("FIXME: classify");
	return cl;
}

static NODE *
argput(NODE *p)
{
	NODE *q;
	int typ, r, ssz;

	if (p->n_op == CM) {
		p->n_left = argput(p->n_left);
		p->n_right = argput(p->n_right);
		return p;
	}

	/* first arg may be struct return pointer */
	/* XXX - check if varargs; setup al */
	switch (typ = argtyp(p->n_type, p->n_df, p->n_ap)) {
	case INTEGER:
	case SSE:
		if (typ == SSE)
			r = XMM0 + nsse++;
		else
			r = argregsi[ngpr++];
		p = movtoreg(p, r);
		break;

	case X87:
		r = nrsp;
		nrsp += SZLDOUBLE;
		p = movtomem(p, r, STKREG);
		break;

	case SSEMEM:
		r = nrsp;
		nrsp += SZDOUBLE;
		p = movtomem(p, r, STKREG);
		break;

	case INTMEM:
		r = nrsp;
		nrsp += SZLONG;
		p = movtomem(p, r, STKREG);
		break;

	case STRREG: /* Struct in registers */
		/* Cast to long pointer and move to the registers */
		/* XXX can overrun struct size */
		ssz = tsize(p->n_type, p->n_df, p->n_ap);

		if (ssz <= SZLONG) {
			q = cast(p->n_left, LONG+PTR, 0);
			nfree(p);
			q = buildtree(UMUL, q, NIL);
			p = movtoreg(q, argregsi[ngpr++]);
		} else if (ssz <= SZLONG*2) {
			NODE *qt, *q1, *q2, *ql, *qr;

			qt = tempnode(0, LONG+PTR, 0, MKAP(LONG));
			q1 = ccopy(qt);
			q2 = ccopy(qt);
			ql = buildtree(ASSIGN, qt, cast(p->n_left,LONG+PTR, 0));
			nfree(p);
			qr = movtoreg(buildtree(UMUL, q1, NIL),
			    argregsi[ngpr++]);
			ql = buildtree(COMOP, ql, qr);
			qr = buildtree(UMUL, buildtree(PLUS, q2, bcon(1)), NIL);
			qr = movtoreg(qr, argregsi[ngpr++]);
			p = buildtree(COMOP, ql, qr);
		} else
			cerror("STRREG");
		break;

	case STRMEM: {
		struct symtab s;
		NODE *l, *t;

		q = buildtree(UMUL, p->n_left, NIL);

		s.stype = p->n_type;
		s.squal = 0;
		s.sdf = p->n_df;
		s.sap = p->n_ap;
		s.soffset = nrsp;
		s.sclass = AUTO;

		nrsp += tsize(p->n_type, p->n_df, p->n_ap);

		l = block(REG, NIL, NIL, PTR+STRTY, 0, 0);
		l->n_lval = 0;
		regno(l) = STKREG;

		t = block(NAME, NIL, NIL, p->n_type, p->n_df, p->n_ap);
		t->n_sp = &s;
		t = stref(block(STREF, l, t, 0, 0, 0));

		t = (buildtree(ASSIGN, t, q));
		nfree(p);
		p = t->n_left;
		nfree(t);
		break;
		}

	default:
		cerror("argument %d", typ);
	}
	return p;
}

/*
 * Sort arglist so that register assignments ends up last.
 */
static int
argsort(NODE *p)
{
	NODE *q;
	int rv = 0;

	if (p->n_op != CM)
		return rv;
	if (p->n_right->n_op == ASSIGN && p->n_right->n_left->n_op == REG) {
		if (p->n_left->n_op == CM &&
		    p->n_left->n_right->n_op == STASG) {
			q = p->n_left->n_right;
			p->n_left->n_right = p->n_right;
			p->n_right = q;
			rv = 1;
		} else if (p->n_left->n_op == STASG) {
			q = p->n_left;
			p->n_left = p->n_right;
			p->n_right = q;
			rv = 1;
		}
	}
	return rv | argsort(p->n_left);
}

/*
 * Called with a function call with arguments as argument.
 * This is done early in buildtree() and only done once.
 * Returns p.
 */
NODE *
funcode(NODE *p)
{
	NODE *l, *r;

	nsse = ngpr = nrsp = 0;
	/* Check if hidden arg needed */
	/* If so, add it in pass2 */
	if ((l = p->n_left)->n_type == INCREF(FTN)+STRTY ||
	    l->n_type == INCREF(FTN)+UNIONTY) {
		int ssz = tsize(BTYPE(l->n_type), l->n_df, l->n_ap);
		if (ssz > 2*SZLONG)
			ngpr++;
	}

	/* Convert just regs to assign insn's */
	p->n_right = argput(p->n_right);

	/* Must sort arglist so that STASG ends up first */
	/* This avoids registers being clobbered */
	while (argsort(p->n_right))
		;

	/* Check if there are varargs */
	if (nsse || l->n_df == NULL || l->n_df->dfun == NULL) {
		; /* Need RAX */
	} else {
		union arglist *al = l->n_df->dfun;

		for (; al->type != TELLIPSIS; al++) {
			if (al->type == TNULL)
				return p; /* No need */
			if (BTYPE(al->type) == STRTY ||
			    BTYPE(al->type) == UNIONTY ||
			    ISARY(al->type))
				al++;
		}
	}

	/* Always emit number of SSE regs used */
	l = movtoreg(bcon(nsse), RAX);
	if (p->n_right->n_op != CM) {
		p->n_right = block(CM, l, p->n_right, INT, 0, MKAP(INT));
	} else {
		for (r = p->n_right; r->n_left->n_op == CM; r = r->n_left)
			;
		r->n_left = block(CM, l, r->n_left, INT, 0, MKAP(INT));
	}
	return p;
}

/*
 * return the alignment of field of type t
 */
int
fldal(unsigned int t)
{
	uerror("illegal field type");
	return(ALINT);
}

/* fix up type of field p */
void
fldty(struct symtab *p)
{
}

/*
 * XXX - fix genswitch.
 */
int
mygenswitch(int num, TWORD type, struct swents **p, int n)
{
	return 0;
}
