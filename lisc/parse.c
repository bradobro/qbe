/* really crude parser
 */
#include <ctype.h>
#include <string.h>

#include "lisc.h"

enum {
	NTemps = 256,
};

typedef enum {
	PXXX,
	PLbl,
	PPhi,
	PIns,
	PEnd,
} PState;

typedef enum {
	TXXX,
	TCopy,
	TAdd,
	TSub,
	TDiv,
	TMod,
	TPhi,
	TJmp,
	TJez,
	TRet,

	TNum,
	TVar,
	TLbl,
	TEq,
	TComma,
	TLParen,
	TRParen,
	TNL,
	TEOF,
} Token;


static FILE *inf;
static Token thead;

static Sym sym[NTemps];
static Ref ntemp;
static Phi phi[NPhis], *curp;
static Ins ins[NInss], *curi;
static struct {
	char name[NString];
	Blk *blk;
} bmap[NBlks+1];
static Blk *curb;
static Blk **blink;
static int nblk;

static struct {
	long long num;
	char *str;
} tokval;
static int lnum;


void *
alloc(size_t n)
{
	void *p;

	p = malloc(n);
	if (!p)
		abort();
	return p;
}

static void
err(char *s)
{
	/* todo, export the error handling in util.c */
	fprintf(stderr, "parse error: %s (line %d)\n", s, lnum);
	exit(1);
}

static Token
lex()
{
	static struct {
		char *str;
		Token tok;
	} tmap[] = {
		{ "copy", TCopy },
		{ "add", TAdd },
		{ "sub", TSub },
		{ "div", TDiv },
		{ "mod", TMod },
		{ "phi", TPhi },
		{ "jmp", TJmp },
		{ "jez", TJez },
		{ "ret", TRet },
		{ 0 },
	};
	static char tok[NString];
	int c, i, sgn;
	Token t;

	do
		c = fgetc(inf);
	while (isblank(c));
	switch (c) {
	case EOF:
		return TEOF;
	case ',':
		return TComma;
	case '(':
		return TLParen;
	case ')':
		return TRParen;
	case '=':
		return TEq;
	case '%':
		t = TVar;
		c = fgetc(inf);
		goto Alpha;
	case '@':
		t = TLbl;
		c = fgetc(inf);
		goto Alpha;
	case '#':
		while (fgetc(inf) != '\n')
			;
	case '\n':
		lnum++;
		return TNL;
	}
	if (isdigit(c) || c == '-') {
		if (c == '-') {
			tokval.num = 0;
			sgn = -1;
		} else {
			tokval.num = c - '0';
			sgn = 1;
		}
		for (;;) {
			c = fgetc(inf);
			if (!isdigit(c))
				break;
			tokval.num *= 10;
			tokval.num += c - '0';
		}
		ungetc(c, inf);
		tokval.num *= sgn;
		return TNum;
	}
	t = TXXX;
Alpha:
	if (!isalpha(c))
		err("lexing failure");
	i = 0;
	do {
		if (i >= NString-1)
			err("identifier too long");
		tok[i++] = c;
		c = fgetc(inf);
	} while (isalpha(c) || isdigit(c));
	tok[i] = 0;
	ungetc(c, inf);
	if (t != TXXX) {
		tokval.str = tok;
		return t;
	}
	for (i=0; tmap[i].str; i++)
		if (strcmp(tok, tmap[i].str) == 0)
			return tmap[i].tok;
	err("unknown keyword");
	return -1;
}

static Token
peek()
{
	if (thead == TXXX)
		thead = lex();
	return thead;
}

static Token
next()
{
	Token t;

	t = peek();
	thead = TXXX;
	return t;
}

static Blk *
blocka()
{
	static Blk zblock;
	Blk *b;

	b = alloc(sizeof *b);
	*b = zblock;
	*blink = b;
	blink = &b->link;
	nblk++;
	return b;
}

static Ref
varref(char *v)
{
	int t;

	for (t=Temp0; t<ntemp; t++)
		if (sym[t].type == STemp)
		if (strcmp(v, sym[t].name) == 0)
			return TEMP(t);
	if (t >= NTemps)
		err("too many temporaries");
	sym[t].type = STemp;
	strcpy(sym[t].name, v);
	sym[t].blk = 0;
	ntemp++;
	return TEMP(t);
}

static Ref
parseref()
{
	switch (next()) {
	case TVar:
		return varref(tokval.str);
	case TNum:
		if (tokval.num > NRefs || tokval.num < 0)
			/* todo, use constants table */
			abort();
		return CONST(tokval.num);
	default:
		return R;
	}
}

static Blk *
findblk(char *name)
{
	int i;

	assert(name[0]);
	for (i=0; i<NBlks; i++)
		if (!bmap[i].blk || strcmp(bmap[i].name, name) == 0)
			break;
	if (i == NBlks)
		err("too many blocks");
	if (!bmap[i].blk) {
		assert(bmap[i].name[0] == 0);
		strcpy(bmap[i].name, name);
		bmap[i].blk = blocka();
		strcpy(bmap[i].blk->name, name);
	}
	return bmap[i].blk;
}

static void
expect(Token t)
{
	static char *names[] = {
		[TLbl] = "label",
		[TComma] = "comma",
		[TEq] = "=",
		[TNL] = "newline",
		[TEOF] = 0,
	};
	char buf[128], *s1, *s2;
	Token t1;

	t1 = next();
	if (t == t1)
		return;
	s1 = names[t] ? names[t] : "??";
	s2 = names[t1] ? names[t1] : "??";
	snprintf(buf, sizeof buf,
		"%s expected (got %s instead)", s1, s2);
	err(buf);
}

static PState
parseline(PState ps)
{
	Ref args[NPreds] = {0};
	Ref r;
	Token t;
	Blk *b;
	int op, i, j;

	assert(args[0] == R);
	do
		t = next();
	while (t == TNL);
	if (ps == PLbl && t != TLbl && t != TEOF)
		err("label or end of file expected");
	switch (t) {
	default:
		err("label, instruction or jump expected");
	case TEOF:
		return PEnd;
	case TVar:
		break;
	case TLbl:
		b = findblk(tokval.str);
		if (curb) {
			curb->np = curp - phi;
			curb->ni = curi - ins;
			curb->ps = alloc(curb->np * sizeof(Phi));
			curb->is = alloc(curb->ni * sizeof(Ins));
			memcpy(curb->ps, curp, curb->np * sizeof(Phi));
			memcpy(curb->is, curi, curb->ni * sizeof(Ins));
			if (curb->jmp.type == JXXX) {
				curb->jmp.type = JJmp;
				curb->s1 = b;
			}
		}
		curb = b;
		if (curb->jmp.type != JXXX)
			err("multiple definitions of block");
		expect(TNL);
		return PPhi;
	case TRet:
		curb->jmp.type = JRet;
		expect(TNL);
		return PLbl;
	case TJmp:
		curb->jmp.type = JJmp;
		goto Jump;
	case TJez:
		curb->jmp.type = JJez;
		r = parseref();
		if (r == R)
			err("invalid argument for jez jump");
		curb->jmp.arg = r;
		expect(TComma);
	Jump:
		expect(TLbl);
		curb->s1 = findblk(tokval.str);
		if (curb->jmp.type == TJmp) {
			expect(TNL);
			return PLbl;
		}
		expect(TComma);
		expect(TLbl);
		curb->s2 = findblk(tokval.str);
		expect(TNL);
		return PLbl;
	}
	r = varref(tokval.str);
	expect(TEq);
	switch (next()) {
	case TCopy:
		op = OCopy;
		j = 1;
		break;
	case TAdd:
		op = OAdd;
		j = 2;
		break;
	case TSub:
		op = OSub;
		j = 2;
		break;
	case TDiv:
		op = ODiv;
		j = 2;
		break;
	case TMod:
		op = OMod;
		j = 2;
		break;
	case TPhi:
		if (ps != PPhi)
			err("unexpected phi instruction");
		op = -1;
		j = -1;
		break;
	default:
		err("invalid instruction");
	}
	i = 0;
	if (peek() != TNL)
		for (;;) {
			if (i == NPreds)
				err("too many arguments");
			args[i] = parseref();
			if (args[i] == R)
				err("invalid instruction argument");
			i++;
			t = peek();
			if (t == TNL)
				break;
			if (t != TComma)
				err("comma or end of line expected");
			next();
		}
	next();
	if (j >= 0 && i != j)
		err("invalid arity");
	if (op != -1) {
		if (curi - ins >= NInss)
			err("too many instructions in block");
		curi->to = r;
		curi->l = args[0];
		curi->r = args[1];
		curi++;
		return PIns;
	} else {
		if (curp - phi >= NPhis)
			err("too many phis in block");
		curp->to = r;
		memcpy(curp->args, args, i * sizeof(Ref));
		curp->na = i;
		curp++;
		return PPhi;
	}
}

Fn *
parsefn(FILE *f)
{
	int i;
	PState ps;
	Fn *fn;

	inf = f;
	for (i=0; i<NBlks; i++) {
		bmap[i].name[0] = 0;
		bmap[i].blk = 0;
	}
	for (i=Temp0; i<NTemps; i++) {
		sym[i].type = SUndef;
		sym[i].name[0] = 0;
		sym[i].blk = 0;
	}
	ntemp = Temp0;
	curp = phi;
	curi = ins;
	curb = 0;
	lnum = 1;
	nblk = 0;
	fn = alloc(sizeof *fn);
	blink = &fn->start;
	ps = PLbl;
	do
		ps = parseline(ps);
	while (ps != PEnd);
	fn->sym = alloc(ntemp * sizeof sym[0]);
	memcpy(fn->sym, sym, ntemp * sizeof sym[0]);
	fn->ntemp = ntemp;
	fn->nblk = nblk;
	fn->rpo = 0;
	return fn;
}



int
main()
{
	parsefn(stdin);
	return 0;
}