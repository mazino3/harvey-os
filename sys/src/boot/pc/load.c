#include "u.h"
#include "lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "io.h"

#include "dosfs.h"

Type types[] = {
	{	Tfloppy,
		Fini|Fdos,
		floppyinit, floppyinitdev,
		floppygetdospart, 0, floppyboot,
	},
	{	Tether,
		Fbootp,
		etherinit, etherinitdev,
		0, 0, bootp,
	},
	{	Tsd,
		Fini|Fdos,
		sdinit, sdinitdev,
		sdgetdospart, sdaddconf, sdboot,
	},
	{	Tnil,
		0,
		0, 0, 0, 0,
		{ 0, },
	},
};

#include "etherif.h"

extern int ether2114xreset(Ether*);
extern int elnk3reset(Ether*);
extern int i82557reset(Ether*);
extern int elnk3reset(Ether*);
extern int ether589reset(Ether*);
extern int ne2000reset(Ether*);
extern int wd8003reset(Ether*);
extern int ec2treset(Ether*);

struct {
	char	*type;
	int	(*reset)(Ether*);
} ethercards[] = {
	{ "21140", ether2114xreset, },
	{ "2114x", ether2114xreset, },
	{ "i82557", i82557reset, },
	{ "elnk3", elnk3reset, },
	{ "3C509", elnk3reset, },
	{ "3C589", ether589reset, },
	{ "3C562", ether589reset, },
	{ "589E", ether589reset, },
	{ "NE2000", ne2000reset, },
	{ "WD8003", wd8003reset, },
	{ "EC2T", ec2treset, },

	{ 0, }
};

typedef struct Mode Mode;

enum {
	Maxdev		= 7,
	Dany		= -1,
	Nmedia		= 16,
	Nini		= 10,
};

enum {					/* mode */
	Mauto		= 0x00,
	Mlocal		= 0x01,
	Manual		= 0x02,
	NMode		= 0x03,
};

typedef struct Medium Medium;
struct Medium {
	Type*	type;
	int	flag;
	int	dev;
	char name[NAMELEN];

	Dos *inidos;
	char *part;
	char *ini;

	Medium*	next;
};

typedef struct Mode {
	char*	name;
	int	mode;
} Mode;

static Medium media[Nmedia];
static Medium *curmedium = media;

static Mode modes[NMode+1] = {
	[Mauto]		{ "auto",   Mauto,  },
	[Mlocal]	{ "local",  Mlocal, },
	[Manual]	{ "manual", Manual, },
};

static char *inis[] = {
	"plan9/plan9.ini",
	"plan9.ini",
	0
};

char **ini;

int scsi0port;

static Medium*
parse(char *line, char **file)
{
	char *p;
	Type *tp;
	Medium *mp;

	if(p = strchr(line, '!')) {
		*p++ = 0;
		*file = p;
	} else
		*file = "";

	for(tp = types; tp->type != Tnil; tp++)
		for(mp = tp->media; mp; mp = mp->next)
			if(strcmp(mp->name, line) == 0)
				return mp;
	if(p)
		*--p = '!';
	return nil;
}

static int
boot(Medium *mp, char *file)
{
	Type *tp;
	Medium *xmp;
	static int didaddconf;
	Boot b;

	memset(&b, 0, sizeof b);
	b.state = INITKERNEL;

	if(didaddconf == 0) {
		didaddconf = 1;
		for(tp = types; tp->type != Tnil; tp++)
			if(tp->addconf)
				for(xmp = tp->media; xmp; xmp = xmp->next)
					(*tp->addconf)(xmp->dev);
	}

	sprint(BOOTLINE, "%s!%s", mp->name, file);
	return (*mp->type->boot)(mp->dev, file, &b);
}

static Medium*
allocm(Type *tp)
{
	Medium **l;

	if(curmedium >= &media[Nmedia])
		return 0;

	for(l = &tp->media; *l; l = &(*l)->next)
		;
	*l = curmedium++;
	return *l;
}

char *parts[] = { "dos", "9fat", "data", 0 };

Medium*
probe(int type, int flag, int dev)
{
	Type *tp;
	int i;
	Medium *mp;
	Dosfile df;
	Dos *dos;
	char **partp;

	for(tp = types; tp->type != Tnil; tp++){
		if(type != Tany && type != tp->type)
			continue;

		if(flag != Fnone){
			for(mp = tp->media; mp; mp = mp->next){
				if((flag & mp->flag) && (dev == Dany || dev == mp->dev))
					return mp;
			}
		}

		if((tp->flag & Fprobe) == 0){
			tp->flag |= Fprobe;
			tp->mask = (*tp->init)();
		}

		for(i = 0; tp->mask; i++){
			if((tp->mask & (1<<i)) == 0)
				continue;
			tp->mask &= ~(1<<i);

			if((mp = allocm(tp)) == 0)
				continue;

			mp->dev = i;
			mp->flag = tp->flag;
			mp->type = tp;
			(*tp->initdev)(i, mp->name);

			if(mp->flag & Fini){
				mp->flag &= ~Fini;
				for(partp = parts; *partp; partp++){
					if((dos = (*tp->getdospart)(i, *partp)) == nil)
						continue;

					for(ini = inis; *ini; ini++){
						if(dosstat(dos, *ini, &df) > 0){
							mp->inidos = dos;
							mp->part = *partp;
							mp->ini = *ini;
							mp->flag |= Fini;
							goto Break2;
						}
					}
				}
			}
		Break2:
			if((flag & mp->flag) && (dev == Dany || dev == i))
				return mp;
		}
	}

	return 0;
}

void
main(void)
{
	Medium *mp;
	int flag, i, mode, tried;
	char def[2*NAMELEN], line[80], *p, *file;
	Type *tp;

	i8042a20();
	memset(m, 0, sizeof(Mach));
	trapinit();
	clockinit();
	alarminit();
	spllo();

	if((ulong)&end > (KZERO|(640*1024)))
		panic("i'm too big\n");

	for(tp = types; tp->type != Tnil; tp++){
		if(tp->type == Tether)
			continue;
		if((mp = probe(tp->type, Fini, Dany)) && (mp->flag & Fini)){
print("using %s!%s!%s\n", mp->name, mp->part, mp->ini);
			dotini(mp->inidos);
			break;
		}
	}

	consinit();

	/*
 	 * Even after we find the ini file, we keep probing disks,
	 * because we have to collect the partition tables and
	 * have boot devices for parse.
	 */
	probe(Tany, Fnone, Dany);

	tried = 0;
	mode = Mauto;
	
	p = getconf("bootfile");

	if(p != 0) {
		mode = Manual;
		for(i = 0; i < NMode; i++){
			if(strcmp(p, modes[i].name) == 0){
				mode = modes[i].mode;
				goto done;
			}
		}
		if((mp = parse(p, &file)) == nil) {
			print("Bad bootfile syntax: %s\n", p);
			goto done;
		}
		tried = boot(mp, file);
	}
done:
	if(tried == 0 && mode != Manual){
		flag = Fany;
		if(mode == Mlocal)
			flag &= ~Fbootp;
		if((mp = probe(Tany, flag, Dany)) && mp->type->type != Tfloppy)
			boot(mp, 0);
	}

	def[0] = 0;
	probe(Tany, Fnone, Dany);
	if(p = getconf("bootdef"))
		strcpy(def, p);

	flag = 0;
	for(tp = types; tp->type != Tnil; tp++){
		for(mp = tp->media; mp; mp = mp->next){
			if(flag == 0){
				flag = 1;
				print("Boot devices:");
			}
			print(" %s", mp->name);
		}
	}
	if(flag)
		print("\n");

	for(;;){
		if(getstr("boot from", line, sizeof(line), def, mode != Manual) >= 0)
			if(mp = parse(line, &file))
				boot(mp, file);
		def[0] = 0;
	}
}

int
getfields(char *lp, char **fields, int n, char sep)
{
	int i;

	for(i = 0; lp && *lp && i < n; i++){
		while(*lp == sep)
			*lp++ = 0;
		if(*lp == 0)
			break;
		fields[i] = lp;
		while(*lp && *lp != sep){
			if(*lp == '\\' && *(lp+1) == '\n')
				*lp++ = ' ';
			lp++;
		}
	}
	return i;
}

int
cistrcmp(char *a, char *b)
{
	int ac, bc;

	for(;;){
		ac = *a++;
		bc = *b++;
	
		if(ac >= 'A' && ac <= 'Z')
			ac = 'a' + (ac - 'A');
		if(bc >= 'A' && bc <= 'Z')
			bc = 'a' + (bc - 'A');
		ac -= bc;
		if(ac)
			return ac;
		if(bc == 0)
			break;
	}
	return 0;
}

int
cistrncmp(char *a, char *b, int n)
{
	unsigned ac, bc;

	while(n > 0){
		ac = *a++;
		bc = *b++;
		n--;

		if(ac >= 'A' && ac <= 'Z')
			ac = 'a' + (ac - 'A');
		if(bc >= 'A' && bc <= 'Z')
			bc = 'a' + (bc - 'A');

		ac -= bc;
		if(ac)
			return ac;
		if(bc == 0)
			break;
	}

	return 0;
}

void*
ialloc(ulong n, int align)
{

	static ulong palloc;
	ulong p;
	int a;

	if(palloc == 0)
		palloc = 3*1024*1024;

	p = palloc;
	if(align <= 0)
		align = 4;
	if(a = n % align)
		n += align - a;
	if(a = p % align)
		p += align - a;

	palloc = p+n;

	return memset((void*)(p|KZERO), 0, n);
}

static Block *allocbp;

Block*
allocb(int size)
{
	Block *bp, **lbp;
	ulong addr;

	lbp = &allocbp;
	for(bp = *lbp; bp; bp = bp->next){
		if((bp->lim - bp->base) >= size){
			*lbp = bp->next;
			break;
		}
		lbp = &bp->next;
	}
	if(bp == 0){
		bp = ialloc(sizeof(Block)+size+64, 0);
		addr = (ulong)bp;
		addr = ROUNDUP(addr + sizeof(Block), 8);
		bp->base = (uchar*)addr;
		bp->lim = ((uchar*)bp) + sizeof(Block)+size+64;
	}

	if(bp->flag)
		panic("allocb reuse\n");

	bp->rp = bp->base;
	bp->wp = bp->rp;
	bp->next = 0;
	bp->flag = 1;

	return bp;
}

void
freeb(Block* bp)
{
	bp->next = allocbp;
	allocbp = bp;

	bp->flag = 0;
}

enum {
	Paddr=		0x70,	/* address port */
	Pdata=		0x71,	/* data port */
};

uchar
nvramread(int offset)
{
	outb(Paddr, offset);
	return inb(Pdata);
}

void (*etherdetach)(void);

void
warp9(ulong entry)
{
	if(etherdetach)
		etherdetach();
	consdrain();
	(*(void(*)(void))(PADDR(entry)))();
}
