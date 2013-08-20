#include <vfs.h>
#include <kfs.h>
#include <slab.h>
#include <kmalloc.h>
#include <kref.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <error.h>
#include <cpio.h>
#include <pmap.h>
#include <smp.h>
#include <etherif.h>


static struct ether *etherxx[Maxether];

struct chan*
etherattach(char* spec, struct errbuf *perrbuf)
{
	ERRSTACK(2);
	uint32_t ctlrno;
	char *p;
	struct chan *chan;

	ctlrno = 0;
	/* 'spec' is the special, i.e. l0, l1, etc. */
	if(spec && *spec){
		ctlrno = strtoul(spec, &p, 0);
		if((ctlrno == 0 && p == spec) || *p || (ctlrno >= Maxether))
			error(Ebadarg);
	}
	if(etherxx[ctlrno] == 0)
		error(Enodev);

	chan = devattach('l', spec, perrbuf);
	if(waserror()){
		chanfree(chan);
		nexterror();
	}
	chan->devno = ctlrno;
	if(etherxx[ctlrno]->attach)
		etherxx[ctlrno]->attach(etherxx[ctlrno]);
	if (0)poperror();
	return chan;
}

static struct walkqid*
etherwalk(struct chan* chan, struct chan* nchan, char** name, int nname, struct errbuf *perrbuf)
{
	return netifwalk(etherxx[chan->devno], chan, nchan, name, nname, perrbuf);
}

static long
etherstat(struct chan* chan, uint8_t* dp, long n, struct errbuf *perrbuf)
{
	return netifstat(etherxx[chan->devno], chan, dp, n, perrbuf);
}

static struct chan*
etheropen(struct chan* chan, int omode, struct errbuf *perrbuf)
{
	return netifopen(etherxx[chan->devno], chan, omode, perrbuf);
}

static void
ethercreate(struct chan*, char*, int, int, struct errbuf *perrbuf)
{
}

static void
etherclose(struct chan* chan, struct errbuf *perrbuf)
{
	netifclose(etherxx[chan->devno], chan, perrbuf);
}

static long
etherread(struct chan* chan, void* buf, long n, int64_t off, struct errbuf *perrbuf)
{
	struct ether *ether;
	uint32_t offset = off;

	ether = etherxx[chan->devno];
	if((chan->qid.type & QTDIR) == 0 && ether->ifstat){
		/*
		 * With some controllers it is necessary to reach
		 * into the chip to extract statistics.
		 */
		if(NETTYPE(chan->qid.path) == Nifstatqid)
			return ether->ifstat(ether, buf, n, offset, perrbuf);
		else if(NETTYPE(chan->qid.path) == Nstatqid)
			ether->ifstat(ether, buf, 0, offset, perrbuf);
	}

	return netifread(ether, chan, buf, n, offset, perrbuf);
}

static struct block*
etherbread(struct chan* chan, long n, int64_t offset, struct errbuf *perrbuf)
{
	return netifbread(etherxx[chan->devno], chan, n, offset, perrbuf);
}

static long
etherwstat(struct chan* chan, uint8_t* dp, long n, struct errbuf *perrbuf)
{
	return netifwstat(etherxx[chan->devno], chan, dp, n, perrbuf);
}

#if 0 
part of plan 9 packet dump infrastructure.
wrap output packets back to input
static void
etherrtrace(Netfile* f, struct etherpkt* pkt, int len)
{
	int i, n;
	struct block *bp;

	if(qwindow(f->iq) <= 0)
		return;
	if(len > 58)
		n = 58;
	else
		n = len;
	bp = iallocb(64);
	if(bp == NULL)
		return;
	memmove(bp->wp, pkt->d, n);
	i = TK2MS(sys->ticks);
	bp->wp[58] = len>>8;
	bp->wp[59] = len;
	bp->wp[60] = i>>24;
	bp->wp[61] = i>>16;
	bp->wp[62] = i>>8;
	bp->wp[63] = i;
	bp->wp += 64;
	qpass(f->iq, bp);
}
#endif

struct block*
etheriq(struct ether* ether, struct block* bp, int fromwire)
{
	struct etherpkt *pkt;
	uint16_t type;
	int len, multi, tome, fromme;
	Netfile **ep, *f, **fp, *fx;
	struct block *xbp;

	ether->inpackets++;

	pkt = (struct etherpkt*)bp->rp;
	len = BLEN(bp);
	type = (pkt->type[0]<<8)|pkt->type[1];
	fx = 0;
	ep = &ether->f[Ntypes];

	multi = pkt->d[0] & 1;
	/* check for valid multicast addresses */
	if(multi && memcmp(pkt->d, ether->bcast, sizeof(pkt->d)) != 0 && ether->prom == 0){
		if(!activemulti(ether, pkt->d, sizeof(pkt->d))){
			if(fromwire){
				freeb(bp);
				bp = 0;
			}
			return bp;
		}
	}

	/* is it for me? */
	tome = memcmp(pkt->d, ether->ea, sizeof(pkt->d)) == 0;
	fromme = memcmp(pkt->s, ether->ea, sizeof(pkt->s)) == 0;

	/*
	 * Multiplex the packet to all the connections which want it.
	 * If the packet is not to be used subsequently (fromwire != 0),
	 * attempt to simply pass it into one of the connections, thereby
	 * saving a copy of the data (usual case hopefully).
	 */
	for(fp = ether->f; fp < ep; fp++){
		if(f = *fp)
		if(f->type == type || f->type < 0)
		if(tome || multi || f->prom || f->bridge & 2){
			/* Don't want to hear bridged packets */
			if(f->bridge && !fromwire && !fromme)
				continue;
			if(!f->headersonly){
				if(fromwire && fx == 0)
					fx = f;
				else if(xbp = iallocb(len)){
					memmove(xbp->wp, pkt, len);
					xbp->wp += len;
					if(qpass(f->iq, xbp) < 0)
						ether->soverflows++;
				}
				else
					ether->soverflows++;
			}
			else
				etherrtrace(f, pkt, len);
		}
	}

	if(fx){
		if(qpass(fx->iq, bp) < 0)
			ether->soverflows++;
		return 0;
	}
	if(fromwire){
		freeb(bp);
		return 0;
	}

	return bp;
}

static int
etheroq(struct ether* ether, struct block* bp)
{
	int len, loopback, s;
	struct etherpkt *pkt;

	ether->outpackets++;

	/*
	 * Check if the packet has to be placed back onto the input queue,
	 * i.e. if it's a loopback or broadcast packet or the interface is
	 * in promiscuous mode.
	 * If it's a loopback packet indicate to etheriq that the data isn't
	 * needed and return, etheriq will pass-on or free the block.
	 * To enable bridging to work, only packets that were originated
	 * by this interface are fed back.
	 */
	pkt = (struct etherpkt*)bp->rp;
	len = BLEN(bp);
	loopback = memcmp(pkt->d, ether->ea, sizeof(pkt->d)) == 0;
	if(loopback || memcmp(pkt->d, ether->bcast, sizeof(pkt->d)) == 0 || ether->prom){
		s = splhi();
		etheriq(ether, bp, 0);
		splx(s);
	}

	if(!loopback){
		qbwrite(ether->oq, bp);
		if(ether->transmit != NULL)
			ether->transmit(ether);
	} else
		freeb(bp);

	return len;
}

static long
etherwrite(struct chan* chan, void* buf, long n, int64_t unused, struct errbuf *perrbuf)
{
	ERRSTACK(2);
	struct ether *ether;
	struct block *bp;
	int nn, onoff;
	Cmdbuf *cb;

	ether = etherxx[chan->devno];
	if(NETTYPE(chan->qid.path) != Ndataqid) {
		nn = netifwrite(ether, chan, buf, n);
		if(nn >= 0)
			return nn;
		cb = parsecmd(buf, n);
		if(cb->f[0] && strcmp(cb->f[0], "nonblocking") == 0){
			if(cb->nf <= 1)
				onoff = 1;
			else
				onoff = atoi(cb->f[1]);
			qnoblock(ether->oq, onoff);
			kfree(cb);
			return n;
		}
		kfree(cb);
		if(ether->ctl != NULL)
			return ether->ctl(ether, buf, n);

		error(Ebadctl);
	}

	if(n > ether->mtu)
		error(Etoobig);
	if(n < ether->minmtu)
		error(Etoosmall);

	bp = allocb(n);
	if(waserror()){
		freeb(bp);
		nexterror();
	}
	memmove(bp->rp, buf, n);
	if((ether->f[NETID(chan->qid.path)]->bridge & 2) == 0)
		memmove(bp->rp+Eaddrlen, ether->ea, Eaddrlen);
	if (0)poperror();
	bp->wp += n;

	return etheroq(ether, bp);
}

static long
etherbwrite(struct chan* chan, struct block* bp, int64_t unused, struct errbuf *perrbuf)
{
	struct ether *ether;
	long n;

	n = BLEN(bp);
	if(NETTYPE(chan->qid.path) != Ndataqid){
		if(waserror()) {
			freeb(bp);
			nexterror();
		}
		n = etherwrite(chan, bp->rp, n, 0, perrbuf);
		if (0)poperror();
		freeb(bp);
		return n;
	}
	ether = etherxx[chan->devno];

	if(n > ether->mtu){
		freeb(bp);
		error(Etoobig);
	}
	if(n < ether->minmtu){
		freeb(bp);
		error(Etoosmall);
	}

	return etheroq(ether, bp);
}

static struct {
	char*	type;
	int	(*reset)(struct ether*);
} cards[Maxether+1];

void
addethercard(char* t, int (*r)(struct ether*))
{
	static int ncard;

	if(ncard == Maxether)
		panic("too many ether cards");
	cards[ncard].type = t;
	cards[ncard].reset = r;
	ncard++;
}

int
parseether(uint8_t *to, char *from)
{
	char nip[4];
	char *p;
	int i;

	p = from;
	for(i = 0; i < Eaddrlen; i++){
		if(*p == 0)
			return -1;
		nip[0] = *p++;
		if(*p == 0)
			return -1;
		nip[1] = *p++;
		nip[2] = 0;
		to[i] = strtoul(nip, 0, 16);
		if(*p == ':')
			p++;
	}
	return 0;
}

static struct ether*
etherprobe(int cardno, int ctlrno)
{
	int i, j;
	struct ether *ether;
	char buf[128], name[32];

	ether = kmalloc(sizeof(struct ether), 0);
	memset(ether, 0, sizeof(struct ether));
	ether->ctlrno = ctlrno;
	ether->tbdf = BUSUNKNOWN;
	ether->mbps = 10;
	ether->minmtu = ETHERMINTU;
	ether->mtu = ETHERMAXTU;
	ether->maxmtu = ETHERMAXTU;

	if(cardno < 0){
		for(cardno = 0; cards[cardno].type; cardno++){
			if(cistrcmp(cards[cardno].type, ether->type))
				continue;
			for(i = 0; i < ether->nopt; i++){
				if(strncmp(ether->opt[i], "ea=", 3))
					continue;
				if(parseether(ether->ea, &ether->opt[i][3]))
					memset(ether->ea, 0, Eaddrlen);
			}
			break;
		}
	}

	if(cardno >= Maxether || cards[cardno].type == NULL){
		kfree(ether);
		return NULL;
	}
	if(cards[cardno].reset(ether) < 0){
		kfree(ether);
		return NULL;
	}

	/*
	 * IRQ2 doesn't really exist, it's used to gang the interrupt
	 * controllers together. A device set to IRQ2 will appear on
	 * the second interrupt controller as IRQ9.
	 */
	if(ether->irq == 2)
		ether->irq = 9;
	snprint(name, sizeof(name), "ether%d", ctlrno);

	/*
	 * If ether->irq is <0, it is a hack to indicate no interrupt
	 * used by ethersink.
	 */
	if(ether->irq >= 0)
		intrenable(ether->irq, ether->interrupt, ether, ether->tbdf, name);

	i = sprint(buf, "#l%d: %s: %dMbps port %#p irq %d tu %d",
		ctlrno, cards[cardno].type, ether->mbps, ether->port, ether->irq, ether->mtu);
	if(ether->mem)
		i += sprint(buf+i, " addr %#p", ether->mem);
	if(ether->size)
		i += sprint(buf+i, " size 0x%luX", ether->size);
	i += sprint(buf+i, ": %2.2ux%2.2ux%2.2ux%2.2ux%2.2ux%2.2ux",
		ether->ea[0], ether->ea[1], ether->ea[2],
		ether->ea[3], ether->ea[4], ether->ea[5]);
	sprint(buf+i, "\n");
	printd(buf);

	j = ether->mbps;
	if(j > 1000)
		j *= 10;
	for(i = 0; j >= 100; i++)
		j /= 10;
	i = (128<<i)*1024;
	netifinit(ether, name, Ntypes, i);
	if(ether->oq == 0)
		ether->oq = qopen(i, Qmsg, 0, 0);
	if(ether->oq == 0)
		panic("etherreset %s", name);
	ether->alen = Eaddrlen;
	memmove(ether->addr, ether->ea, Eaddrlen);
	memset(ether->bcast, 0xFF, Eaddrlen);

	return ether;
}

static void
etherreset(void)
{
	struct ether *ether;
	int cardno, ctlrno;

	for(ctlrno = 0; ctlrno < Maxether; ctlrno++){
		if((ether = etherprobe(-1, ctlrno)) == NULL)
			continue;
		etherxx[ctlrno] = ether;
	}

	if(getconf("*noetherprobe"))
		return;

	cardno = ctlrno = 0;
	while(cards[cardno].type != NULL && ctlrno < Maxether){
		if(etherxx[ctlrno] != NULL){
			ctlrno++;
			continue;
		}
		if((ether = etherprobe(cardno, ctlrno)) == NULL){
			cardno++;
			continue;
		}
		etherxx[ctlrno] = ether;
		ctlrno++;
	}
}

static void
ethershutdown(void)
{
	char name[32];
	int i;
	struct ether *ether;

	for(i = 0; i < Maxether; i++){
		ether = etherxx[i];
		if(ether == NULL)
			continue;
		if(ether->shutdown == NULL) {
			printd("#l%d: no shutdown function\n", i);
			continue;
		}
		snprint(name, sizeof(name), "ether%d", i);
		if(ether->irq >= 0){
		//	intrdisable(ether->irq, ether->interrupt, ether, ether->tbdf, name);
		}
		(*ether->shutdown)(ether);
	}
}


#define POLY 0xedb88320

/* really slow 32 bit crc for ethers */
uint32_t
ethercrc(uint8_t *p, int len)
{
	int i, j;
	uint32_t crc, b;

	crc = 0xffffffff;
	for(i = 0; i < len; i++){
		b = *p++;
		for(j = 0; j < 8; j++){
			crc = (crc>>1) ^ (((crc^b) & 1) ? POLY : 0);
			b >>= 1;
		}
	}
	return crc;
}

struct dev etherdevtab = {
	'l',
	"ether",

	etherreset,
	devinit,
	ethershutdown,
	etherattach,
	etherwalk,
	etherstat,
	etheropen,
	ethercreate,
	etherclose,
	etherread,
	etherbread,
	etherwrite,
	etherbwrite,
	devremove,
	etherwstat,
};