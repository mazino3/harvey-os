#include <u.h>
#include <libc.h>
#include <draw.h>
#include <thread.h>
#include <cursor.h>
#include <mouse.h>
#include <keyboard.h>
#include <frame.h>
#include <auth.h>
#include <fcall.h>
#include <plumb.h>
#include "dat.h"
#include "fns.h"

/*
 *  WASHINGTON (AP) - The Food and Drug Administration warned
 * consumers Wednesday not to use ``Rio'' hair relaxer products
 * because they may cause severe hair loss or turn hair green....
 *    The FDA urged consumers who have experienced problems with Rio
 * to notify their local FDA office, local health department or the
 * company at 1‑800‑543‑3002.
 */

void		resize(void);
void		move(void);
void		delete(void);
void		hide(void);
void		unhide(int);
void		newtile(int);
Image	*sweep(void);
Image	*bandsize(Window*);
Image*	drag(Window*, Rectangle*);
void		refresh(Rectangle);
void		resized(void);
Channel	*exitchan;	/* chan(int) */
Channel	*winclosechan; /* chan(Window*); */
Rectangle	viewr;
int		threadrforkflag = 0;	/* should be RFENVG but that hides rio from plumber */

void	mousethread(void*);
void	keyboardthread(void*);
void winclosethread(void*);
void deletethread(void*);
void	initcmd(void*);

char		*fontname;
int		mainpid;

enum
{
	New,
	Reshape,
	Move,
	Delete,
	Hide,
	Exit,
};

enum
{
	Cut,
	Paste,
	Snarf,
	Plumb,
	Send,
	Scroll,
};

char		*menu2str[] = {
 [Cut]		"cut",
 [Paste]		"paste",
 [Snarf]		"snarf",
 [Plumb]		"plumb",
 [Send]		"send",
 [Scroll]		"scroll",
			nil
};

Menu menu2 =
{
	menu2str
};

int	Hidden = Exit+1;

char		*menu3str[100] = {
 [New]		"New",
 [Reshape]	"Resize",
 [Move]		"Move",
 [Delete]		"Delete",
 [Hide]		"Hide",
 [Exit]		"Exit",
			nil
};

Menu menu3 =
{
	menu3str
};

char *rcargv[] = { "rc", "-i", nil };

int errorshouldabort = 0;

void
derror(Display*, char *errorstr)
{
	error(errorstr);
}

void
threadmain(int argc, char *argv[])
{
	char *initstr, *s;
	static void *arg[1];
	char buf[256];

	if(strstr(argv[0], ".out") == nil){
		menu3str[Exit] = nil;
		Hidden--;
	}
	initstr = nil;
	maxtab = 0;
	ARGBEGIN{
	case 'f':
		fontname = ARGF();
		break;
	case 'i':
		initstr = ARGF();
		break;
	case 's':
		scrolling = TRUE;
		break;
	}ARGEND

	mainpid = getpid();
	if(getwd(buf, sizeof buf) == nil)
		startdir = estrdup(".");
	else
		startdir = estrdup(buf);
	if(fontname == nil)
		fontname = getenv("font");
	if(fontname == nil)
		fontname = "/lib/font/bit/lucm/unicode.9.font";
	s = getenv("tabstop");
	if(s != nil)
		maxtab = strtol(s, nil, 0);
	if(maxtab == 0)
		maxtab = 4;
	/* check font before barging ahead */
	if(access(fontname, 0) < 0){
		threadprint(2, "rio: can't access %s: %r\n", fontname);
		exits("font open");
	}
	putenv("font", fontname);

	snarffd = open("/dev/snarf", OREAD|OCEXEC);

	if(geninitdraw(nil, derror, nil, "rio", nil, Refnone) < 0){
		threadprint(2, "rio: can't open display: %r\n");
		exits("display open");
	}
	iconinit();
	view = screen;
	viewr = view->r;
	mousectl = initmouse(nil, screen);
	if(mousectl == nil)
		error("can't find mouse");
	mouse = mousectl;
	keyboardctl = initkeyboard(nil);
	if(keyboardctl == nil)
		error("can't find keyboard");
	wscreen = allocscreen(screen, background, 0);
	if(wscreen == nil)
		error("can't allocate screen");
	draw(view, viewr, background, nil, ZP);
	flushimage(display, 1);

	exitchan = chancreate(sizeof(int), 0);
	winclosechan = chancreate(sizeof(Window*), 0);
	deletechan = chancreate(sizeof(char*), 0);

	timerinit();
	threadcreate(keyboardthread, nil, STACK);
	threadcreate(mousethread, nil, STACK);
	threadcreate(winclosethread, nil, STACK);
	threadcreate(deletethread, nil, STACK);
	filsys = filsysinit(xfidinit());

	if(filsys == nil)
		fprint(2, "rio: can't create file system server: %r\n");
	else{
		errorshouldabort = 1;	/* suicide if there's trouble after this */
		if(initstr)
			proccreate(initcmd, initstr, STACK);
		notify(shutdown);/**/
		recv(exitchan, nil);
	}
	killprocs();
	threadexitsall(nil);
}

/*
 * /dev/snarf updates when the file is closed, so we must open our own
 * fd here rather than use snarffd
 */
void
putsnarf(void)
{
	int fd, i, n;

	if(snarffd<0 || nsnarf==0)
		return;
	fd = open("/dev/snarf", OWRITE);
	if(fd < 0)
		return;
	/* snarf buffer could be huge, so fprint will truncate; do it in blocks */
	for(i=0; i<nsnarf; i+=n){
		n = nsnarf-i;
		if(n >= 256)
			n = 256;
		if(threadprint(fd, "%.*S", n, snarf+i) < 0)
			break;
	}
	close(fd);
}

void
getsnarf(void)
{
	int i, n, nb, nulls;
	char *sn, buf[1024];

	if(snarffd < 0)
		return;
	sn = nil;
	i = 0;
	seek(snarffd, 0, 0);
	while((n = read(snarffd, buf, sizeof buf)) > 0){
		sn = erealloc(sn, i+n+1);
		memmove(sn+i, buf, n);
		i += n;
		sn[i] = 0;
	}
	if(i > 0){
		snarf = runerealloc(snarf, i+1);
		cvttorunes(sn, i, snarf, &nb, &nsnarf, &nulls);
		free(sn);
	}
}

void
initcmd(void *arg)
{
	char *cmd;

	cmd = arg;
	rfork(RFENVG|RFFDG|RFNOTEG|RFNAMEG);
	procexecl(nil, "/bin/rc", "rc", "-c", cmd, 0);
	threadprint(2, "rio: exec failed: %r\n");
	exits("exec");
}

char *oknotes[] =
{
	"delete",
	"hangup",
	"kill",
	"exit",
	"kilall",	/* used internally by thread library during shutdown */
	nil
};

void
shutdown1(void *, char *msg)
{
	int i;

	killprocs();
	for(i=0; oknotes[i]; i++)
		if(strncmp(oknotes[i], msg, strlen(oknotes[i])) == 0)
			threadexitsall(msg);
	threadprint(2, "rio %d: abort: %s\n", getpid(), msg);
	abort();
	exits(msg);
}

void
shutdown(void *a, char *msg)	/* extra call to get stack trace on 386 */
{
	shutdown1(a, msg);
}

void
killprocs(void)
{
	int i;

	filsysclose(filsys);
	for(i=0; i<nwindow; i++)
		postnote(PNGROUP, window[i]->pid, "hangup");
	remove(srvpipe);
	remove(srvwctl);
}

void
keyboardthread(void*)
{
	Rune buf[2][20], *rp;
	int n, i;

	n = 0;
	for(;;){
		if(nelem(buf[0])!=20)threadprint(2, "NO");
		rp = buf[n];
		n = 1-n;
		recv(keyboardctl->c, rp);
		for(i=1; i<nelem(buf[0])-1; i++)
			if(nbrecv(keyboardctl->c, rp+i) <= 0)
				break;
		rp[i] = L'\0';
		if(input != nil)
			sendp(input->ck, rp);
	}
}

int
portion(int x, int lo, int hi)
{
	x -= lo;
	hi -= lo;
	if(x < 20)
		return 0;
	if(x > hi-20)
		return 2;
	return 1;
}

int
whichcorner(Window *w, Point p)
{
	int i, j;
	
	i = portion(p.x, w->screenr.min.x, w->screenr.max.x);
	j = portion(p.y, w->screenr.min.y, w->screenr.max.y);
	return 3*j+i;
}

void
cornercursor(Window *w, Point p, int force)
{
	if(w!=nil && winborder(w, p))
		riosetcursor(corners[whichcorner(w, p)], force);
	else
		wsetcursor(w, force);
}

/* thread to allow fsysproc to synchronize window closing with main proc */
void
winclosethread(void*)
{
	Window *w;

	for(;;){
		w = recvp(winclosechan);
		wclose(w);
	}
}

/* thread to make Deleted windows that the client still holds disappear offscreen after an interval */
void
deletethread(void*)
{
	char *s;
	Image *i;

	for(;;){
		s = recvp(deletechan);
		i = namedimage(display, s);
		if(i != nil){
			/* move it off-screen to hide it, since client is slow in letting it go */
			originwindow(i, i->r.min, view->r.max);
		}
		freeimage(i);
		free(s);
	}
}

void
deletetimeoutproc(void *v)
{
	char *s;

	s = estrdup(v);
	sleep(750);	/* remove window from screen after 3/4 of a second */
	sendp(deletechan, s);
}

void
mousethread(void*)
{
	int sending, inside, scrolling, moving, band;
	Window *oin, *w;
	Image *i;
	Rectangle r;
	Point xy;
	Mouse tmp;
	enum {
		MReshape,
		MMouse,
		NALT
	};
	static Alt alts[NALT+1];

	sending = FALSE;
	scrolling = FALSE;
	moving = FALSE;

	alts[MReshape].c = mousectl->resizec;
	alts[MReshape].v = nil;
	alts[MReshape].op = CHANRCV;
	alts[MMouse].c = mousectl->c;
	alts[MMouse].v = &mousectl->Mouse;
	alts[MMouse].op = CHANRCV;
	alts[NALT].op = CHANEND;

	for(;;)
	    switch(alt(alts)){
		case MReshape:
			resized();
			break;
		case MMouse:
		Again:
			if(input!=nil && input->i!=nil){
				/* convert to logical coordinates */
				xy.x = mouse->xy.x + (input->i->r.min.x-input->screenr.min.x);
				xy.y = mouse->xy.y + (input->i->r.min.y-input->screenr.min.y);
				inside = ptinrect(mouse->xy, insetrect(input->screenr, Selborder));
				if(input->mouseopen)
					scrolling = FALSE;
				else if(scrolling)
					scrolling = mouse->buttons;
				else
					scrolling = mouse->buttons && ptinrect(xy, input->scrollr);
				/* topped will be zero if window has been bottomed */
				if(sending == FALSE && !scrolling && winborder(input, mouse->xy) && input->topped>0){
					moving = TRUE;
				}else if(inside && (scrolling || input->mouseopen || (mouse->buttons&1)))
					sending = TRUE;
			}else
				sending = FALSE;
			if(sending){
				if(mouse->buttons == 0){
					cornercursor(input, mouse->xy, 0);
					sending = FALSE;
				}else
					wsetcursor(input, 0);
				tmp = mousectl->Mouse;
				tmp.xy = xy;
				send(input->mc.c, &tmp);
				continue;
			}
			w = wpointto(mouse->xy);
			/* change cursor if over anyone's border */
			if(w != nil)
				cornercursor(w, mouse->xy, 0);
			else
				riosetcursor(nil, 0);
			if(moving && (mouse->buttons&7)){
				oin = input;
				band = mouse->buttons & 3;
				sweeping = 1;
				if(band)
					i = bandsize(input);
				else
					i = drag(input, &r);
				sweeping = 0;
				if(i != nil){
					if(input == oin){
						if(band)
							wsendctlmesg(input, Reshaped, i->r, i);
						else
							wsendctlmesg(input, Moved, r, i);
						cornercursor(input, mouse->xy, 1);
					}else
						freeimage(i);
				}
			}
			if(w != nil)
				cornercursor(w, mouse->xy, 0);
			/* we're not sending the event, but if button is down maybe we should */
			if(mouse->buttons){
				/* w->topped will be zero if window has been bottomed */
				if(w==nil || (w==input && w->topped>0)){
					if(mouse->buttons & 1)
						;
					else if(mouse->buttons & 2){
						if(input && !input->mouseopen)
							button2menu(input);
					}else if(mouse->buttons & 4)
						button3menu();
				}else{
					/* if button 1 event in the window, top the window and wait for button up. */
					/* otherwise, top the window and pass the event on */
					if(wtop(mouse->xy) && (mouse->buttons!=1 || winborder(w, mouse->xy)))
						goto Again;
					goto Drain;
				}
			}
			moving = FALSE;
			break;

		Drain:
			do
				readmouse(mousectl);
			while(mousectl->buttons);
			moving = FALSE;
			goto Again;	/* recalculate mouse position, cursor */
		}
}

void
resized(void)
{
	Image *im;
	int i, j, ishidden;
	Rectangle r;
	Point o, n;
	Window *w;

	if(getwindow(display, Refnone) < 0)
		error("failed to re-attach window");
	view = screen;
	freescreen(wscreen);
	wscreen = allocscreen(screen, background, 0);
	if(wscreen == nil)
		error("can't re-allocate screen");
	draw(view, view->r, background, nil, ZP);
	o = subpt(viewr.max, viewr.min);
	n = subpt(view->clipr.max, view->clipr.min);
	for(i=0; i<nwindow; i++){
		w = window[i];
		if(w->deleted)
			continue;
		r = rectsubpt(w->i->r, viewr.min);
		r.min.x = (r.min.x*n.x)/o.x;
		r.min.y = (r.min.y*n.y)/o.y;
		r.max.x = (r.max.x*n.x)/o.x;
		r.max.y = (r.max.y*n.y)/o.y;
		r = rectaddpt(r, screen->clipr.min);
		ishidden = 0;
		for(j=0; j<nhidden; j++)
			if(w == hidden[j]){
				ishidden = 1;
				break;
			}
		if(ishidden)
			im = allocimage(display, r, display->chan, 0, DWhite);
		else
			im = allocwindow(wscreen, r, Refbackup, DWhite);
		if(im)
			wsendctlmesg(w, Reshaped, r, im);
	}
	viewr = screen->r;
	flushimage(display, 1);}

void
button3menu(void)
{
	int i;

	for(i=0; i<nhidden; i++)
		menu3str[i+Hidden] = hidden[i]->label;
	menu3str[i+Hidden] = nil;

	sweeping = 1;
	/*BUG: MENUHIT SHOULD TAKE FONT, COLOR, ETC. */
	switch(i = menuhit(3, mousectl, &menu3, wscreen)){
	case -1:
		break;
	case New:
		new(sweep(), 0, nil, "/bin/rc", nil);
		break;
	case Reshape:
		resize();
		break;
	case Move:
		move();
		break;
	case Delete:
		delete();
		break;
	case Hide:
		hide();
		break;
	case Exit:
		if(Hidden > Exit){
			send(exitchan, nil);
			break;
		}
		/* else fall through */
	default:
		unhide(i);
		break;
	}
	sweeping = 0;
}

void
button2menu(Window *w)
{
	if(w->deleted)
		return;
	incref(w);
	if(w->scrolling)
		menu2str[Scroll] = "noscroll";
	else
		menu2str[Scroll] = "scroll";
	switch(menuhit(2, mousectl, &menu2, wscreen)){
	case Cut:
		wsnarf(w);
		wcut(w);
		wscrdraw(w);
		break;

	case Snarf:
		wsnarf(w);
		break;

	case Paste:
		getsnarf();
		wpaste(w);
		wscrdraw(w);
		break;

	case Plumb:
		wplumb(w);
		break;

	case Send:
		getsnarf();
		wsnarf(w);
		if(nsnarf == 0)
			break;
		if(w->rawing){
			waddraw(w, snarf, nsnarf);
			if(snarf[nsnarf-1] != '\n')
				waddraw(w, L"\n", 1);
		}else{
			winsert(w, snarf, nsnarf, w->nr);
			if(snarf[nsnarf-1] != '\n')
				winsert(w, L"\n", 1, w->nr);
		}
		wsetselect(w, w->nr, w->nr);
		wshow(w, w->nr);
		break;

	case Scroll:
		if(w->scrolling ^= 1)
			wshow(w, w->nr);
		break;
	}
	wclose(w);
	wsendctlmesg(w, Wakeup, ZR, nil);
	flushimage(display, 1);
}

Point
onscreen(Point p)
{
	p.x = max(screen->clipr.min.x, p.x);
	p.x = min(screen->clipr.max.x, p.x);
	p.y = max(screen->clipr.min.y, p.y);
	p.y = min(screen->clipr.max.y, p.y);
	return p;
}

Image*
sweep(void)
{
	Image *i, *oi;
	Rectangle r;
	Point p0, p;

	i = nil;
	riosetcursor(&crosscursor, 1);
	while(mouse->buttons == 0)
		readmouse(mousectl);
	p0 = onscreen(mouse->xy);
	p = p0;
	r.min = p;
	r.max = p;
	oi = nil;
	while(mouse->buttons == 4){
		readmouse(mousectl);
		if(mouse->buttons != 4 && mouse->buttons != 0)
			break;
		if(!eqpt(mouse->xy, p)){
			p = onscreen(mouse->xy);
			r = canonrect(Rpt(p0, p));
			if(Dx(r)>5 && Dy(r)>5){
				i = allocwindow(wscreen, r, Refnone, 0xEEEEEEFF); /* grey */
				freeimage(oi);
				if(i == nil)
					goto Rescue;
				oi = i;
				border(i, r, Selborder, red, ZP);
				flushimage(display, 1);
			}
		}
	}
	if(mouse->buttons != 0)
		goto Rescue;
	if(i==nil || Dx(i->r)<100 || Dy(i->r)<3*font->height)
		goto Rescue;
	oi = i;
	i = allocwindow(wscreen, oi->r, Refbackup, DWhite);
	freeimage(oi);
	if(i == nil)
		goto Rescue;
	border(i, r, Selborder, red, ZP);
	cornercursor(input, mouse->xy, 1);
	return i;

 Rescue:
	freeimage(i);
	cornercursor(input, mouse->xy, 1);
	while(mouse->buttons)
		readmouse(mousectl);
	return nil;
}

/*
 * BUG: should interlock so applications don't change screen
 * while tmp[] holds backing store
 */
Image*
drag(Window *w, Rectangle *rp)
{
	Image *i, *ni;
	Point p, op, d, dm, om;
	Rectangle r;

	i = w->i;
	om = mouse->xy;
	riosetcursor(&boxcursor, 1);
	dm = subpt(mouse->xy, w->screenr.min);
	d = subpt(i->r.max, i->r.min);
	op = subpt(mouse->xy, dm);
	drawgetrect(Rect(op.x, op.y, op.x+d.x, op.y+d.y), 1);
	flushimage(display, 1);
	while(mouse->buttons == 4){
		p = subpt(mouse->xy, dm);
		if(!eqpt(p, op)){
			drawgetrect(Rect(op.x, op.y, op.x+d.x, op.y+d.y), 0);
			drawgetrect(Rect(p.x, p.y, p.x+d.x, p.y+d.y), 1);
			flushimage(display, 1);
			op = p;
		}
		readmouse(mousectl);
	}
	r = Rect(op.x, op.y, op.x+d.x, op.y+d.y);
	drawgetrect(r, 0);
	cornercursor(input, mouse->xy, 1);
	flushimage(display, 1);
	if(mouse->buttons!=0 || (ni=allocwindow(wscreen, r, Refbackup, DWhite))==nil){
		moveto(mousectl, om);
		while(mouse->buttons)
			readmouse(mousectl);
		*rp = Rect(0, 0, 0, 0);
		return nil;
	}
	draw(ni, ni->r, i, nil, i->r.min);
	*rp = r;
	return ni;
}

Rectangle
whichrect(Rectangle r, Point p, int which)
{
	switch(which){
	case 0:	/* top left */
		r = Rect(p.x, p.y, r.max.x, r.max.y);
		break;
	case 2:	/* top right */
		r = Rect(r.min.x, p.y, p.x, r.max.y);
		break;
	case 6:	/* bottom left */
		r = Rect(p.x, r.min.y, r.max.x, p.y);
		break;
	case 8:	/* bottom right */
		r = Rect(r.min.x, r.min.y, p.x, p.y);
		break;
	case 1:	/* top edge */
		r = Rect(r.min.x, p.y, r.max.x, r.max.y);
		break;
	case 5:	/* right edge */
		r = Rect(r.min.x, r.min.y, p.x, r.max.y);
		break;
	case 7:	/* bottom edge */
		r = Rect(r.min.x, r.min.y, r.max.x, p.y);
		break;
	case 3:		/* left edge */
		r = Rect(p.x, r.min.y, r.max.x, r.max.y);
		break;
	}
	return canonrect(r);
}

Image*
bandsize(Window *w)
{
	Image *i;
	Rectangle r, or;
	Point p, startp;
	int which, but;

	p = mouse->xy;
	startp = p;
	which = whichcorner(w, p);
	r = whichrect(w->screenr, p, which);
	drawgetrect(r, 1);
	or = r;
	but = mouse->buttons;
	while(mouse->buttons == but){
		p = onscreen(mouse->xy);
		r = whichrect(w->screenr, p, which);
		if(!eqrect(r, or)){
			/* not too big, not too small, just right */
			if(Dx(r)<=Dx(viewr) && Dy(r)<=Dy(viewr)
			  && Dx(r)>=100 && Dy(r)>=3*font->height){
				drawgetrect(or, 0);
				drawgetrect(r, 1);
				flushimage(display, 1);
				or = r;
			}
		}
		readmouse(mousectl);
	}
	p = mouse->xy;
	drawgetrect(or, 0);
	flushimage(display, 1);
	wsetcursor(input, 1);
	if(mouse->buttons != 0 || Dx(or)<100 || Dy(or)<3*font->height){
		while(mouse->buttons)
			readmouse(mousectl);
		return nil;
	}
	if(abs(p.x-startp.x)+abs(p.y-startp.y) <= 1)
		return nil;
	i = allocwindow(wscreen, or, Refbackup, DWhite);
	if(i == nil)
		return nil;
	border(i, r, Selborder, red, ZP);
	return i;
}

Window*
pointto(int wait)
{
	Window *w;

	riosetcursor(&sightcursor, 1);
	while(mouse->buttons == 0)
		readmouse(mousectl);
	if(mouse->buttons == 4)
		w = wpointto(mouse->xy);
	else
		w = nil;
	if(wait)
		while(mouse->buttons){
			if(mouse->buttons!=4 && w !=nil){	/* cancel */
				cornercursor(input, mouse->xy, 0);
				w = nil;
			}
			readmouse(mousectl);
		}
	cornercursor(input, mouse->xy, 0);
	return w;
}

void
delete(void)
{
	Window *w;

	w = pointto(TRUE);
	if(w)
		wsendctlmesg(w, Deleted, ZR, nil);
}

void
resize(void)
{
	Window *w;
	Image *i;

	w = pointto(TRUE);
	if(w == nil)
		return;
	i = sweep();
	if(i)
		wsendctlmesg(w, Reshaped, i->r, i);
}

void
move(void)
{
	Window *w;
	Image *i;
	Rectangle r;

	w = pointto(FALSE);
	if(w == nil)
		return;
	i = drag(w, &r);
	if(i)
		wsendctlmesg(w, Moved, r, i);
	cornercursor(input, mouse->xy, 1);
}

void
hide(void)
{
	Window *w;
	Image *i;

	w = pointto(TRUE);
	if(w == nil)
		return;
	i = allocimage(display, w->screenr, w->i->chan, 0, DWhite);
	if(i){
		hidden[nhidden++] = w;
		wsendctlmesg(w, Reshaped, ZR, i);
	}
}

void
unhide(int h)
{
	Window *w;
	Image *i;

	h -= Hidden;
	w = hidden[h];
	if(w == nil)
		return;
	i = allocwindow(wscreen, w->i->r, Refbackup, DWhite);
	if(i == nil)
		return;
	if(i){
		--nhidden;
		memmove(hidden+h, hidden+h+1, (nhidden-h)*sizeof(Window*));
		wsendctlmesg(w, Reshaped, w->i->r, i);
	}
}

Window*
new(Image *i, int pid, char *dir, char *cmd, char **argv)
{
	Window *w;
	Mousectl *mc;
	Channel *cm, *ck, *cctl, *cpid;
	void **arg;

	if(i == nil)
		return nil;
	cm = chancreate(sizeof(Mouse), 0);
	ck = chancreate(sizeof(Rune*), 0);
	cctl = chancreate(sizeof(Wctlmesg), 4);
	cpid = chancreate(sizeof(int), 0);
	if(cm==nil || ck==nil || cctl==nil)
		error("new: channel alloc failed");
	mc = emalloc(sizeof(Mousectl));
	*mc = *mousectl;
	mc->image = i;
	mc->c = cm;
	w = wmk(i, mc, ck, cctl);
	window = erealloc(window, ++nwindow*sizeof(Window*));
	window[nwindow-1] = w;
	threadcreate(winctl, w, 8192);
	wcurrent(w);
	flushimage(display, 1);
	if(pid == 0){
		arg = emalloc(5*sizeof(void*));
		arg[0] = w;
		arg[1] = cpid;
		arg[2] = cmd;
		if(argv == nil)
			arg[3] = rcargv;
		else
			arg[3] = argv;
		arg[4] = dir;
		proccreate(winshell, arg, 8192);
		pid = recvul(cpid);
		free(arg);
	}
	wsetpid(w, pid);
	wsetname(w);
	free(cpid);
	return w;
}
