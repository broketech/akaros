// INFERNO
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
#include <ip.h>

extern uint32_t	kerndate;
extern char *eve;

void
mkqid(struct qid *q, int64_t path, uint32_t vers, int type)
{
	q->type = type;
	q->vers = vers;
	q->path = path;
}

int
devno(int c, int user)
{
	int i;

	for(i = 0; devtab[i] != NULL; i++) {
		if(devtab[i]->dc == c)
			return i;
	}
	if(user == 0)
		panic("devno %C 0x%ux", c, c);

	return -1;
}

void
devdir(struct chan *c, struct qid qid, char *n,
       int64_t length, char *user, long perm, struct dir *db)
{
	db->name = n;
	if(c->flag&CMSG)
		qid.type |= QTMOUNT;
	db->qid = qid;
	db->type = devtab[c->type]->dc;
	db->dev = c->dev;
	db->mode = perm;
	db->mode |= qid.type << 24;
	db->atime = seconds();
	db->mtime = kerndate;
	db->length = length;
	db->uid = user;
	db->gid = eve;
	db->muid = user;
}

/*
 * the zeroth element of the table MUST be the directory itself for ..
*/
int
devgen(struct chan *c, char *unused_char_p_t, struct dirtab *tab, int ntab, int i,
       struct dir *dp)
{
	if(tab == 0)
		return -1;
	if(i != DEVDOTDOT){
		/* skip over the first element, that for . itself */
		i++;
		if(i >= ntab)
			return -1;
		tab += i;
	}
	devdir(c, tab->qid, tab->name, tab->length, eve, tab->perm, dp);
	return 1;
}

void
devreset(void)
{
}

void
devinit(void)
{
}

void
devshutdown(void)
{
}

struct chan*
devattach(int tc, char *spec)
{
	struct chan *c;
	char *buf;

	c = newchan();
	mkqid(&c->qid, 0, 0, QTDIR);
	c->type = devno(tc, 0);
	if(spec == NULL)
		spec = "";
	buf = kzmalloc(4 + strlen(spec) + 1, 0);
	snprintf(buf, sizeof(buf), "#%C%s", tc, spec);
	c->name = newcname(buf);
	kfree(buf);
	return c;
}


struct chan*
devclone(struct chan *c)
{
	struct chan *nc;

	if(c->flag & COPEN)
		panic("clone of open file type %C\n", devtab[c->type]->dc);

	nc = newchan();

	nc->type = c->type;
	nc->dev = c->dev;
	nc->mode = c->mode;
	nc->qid = c->qid;
	nc->offset = c->offset;
	nc->umh = NULL;
	nc->mountid = c->mountid;
	nc->aux = c->aux;
	nc->mqid = c->mqid;
	nc->mcp = c->mcp;
	return nc;
}

struct walkqid*
devwalk(struct chan *c,
	struct chan *nc, char **name, int nname,
	struct dirtab *tab, int ntab, Devgen *gen)
{
	ERRSTACK(2);
	int i, j, alloc;
	struct walkqid *wq;
	char *n;
	struct dir dir;

	if(nname > 0)
		isdir(c);

	alloc = 0;
	wq = kzmalloc(sizeof(struct walkqid) + (nname - 1) * sizeof(struct qid), 0);
	if(waserror()){
		if(alloc && wq->clone!=NULL)
			cclose(wq->clone);
		kfree(wq);
		return NULL;
	}
	if(nc == NULL){
		nc = devclone(c);
		nc->type = 0;	/* device doesn't know about this channel yet */
		alloc = 1;
	}
	wq->clone = nc;

	for(j=0; j<nname; j++){
		if(!(nc->qid.type&QTDIR)){
			if(j==0)
				error(Enotdir);
			goto Done;
		}
		n = name[j];
		if(strcmp(n, ".") == 0){
    Accept:
			wq->qid[wq->nqid++] = nc->qid;
			continue;
		}
		if(strcmp(n, "..") == 0){
			(*gen)(nc, NULL, tab, ntab, DEVDOTDOT, &dir);
			nc->qid = dir.qid;
			goto Accept;
		}
		/*
		 * Ugly problem: If we're using devgen, make sure we're
		 * walking the directory itself, represented by the first
		 * entry in the table, and not trying to step into a sub-
		 * directory of the table, e.g. /net/net. Devgen itself
		 * should take care of the problem, but it doesn't have
		 * the necessary information (that we're doing a walk).
		 */
		if(gen==devgen && nc->qid.path!=tab[0].qid.path)
			goto Notfound;
		for(i=0;; i++) {
			switch((*gen)(nc, n, tab, ntab, i, &dir)){
			case -1:
			Notfound:
				if(j == 0)
					error(Enonexist);
				set_errstr(Enonexist);
				goto Done;
			case 0:
				continue;
			case 1:
				if(strcmp(n, dir.name) == 0){
					nc->qid = dir.qid;
					goto Accept;
				}
				continue;
			}
		}
	}
	/*
	 * We processed at least one name, so will return some data.
	 * If we didn't process all nname entries succesfully, we drop
	 * the cloned channel and return just the Qids of the walks.
	 */
Done:
	poperror();
	if(wq->nqid < nname){
		if(alloc)
			cclose(wq->clone);
		wq->clone = NULL;
	}else if(wq->clone){
		/* attach cloned channel to same device */
		wq->clone->type = c->type;
	}
	return wq;
}

int
devstat(struct chan *c, uint8_t *db, int n,
	struct dirtab *tab, int ntab, Devgen *gen)
{
	int i;
	struct dir dir;
	char *p, *elem;

	for(i=0;; i++)
		switch((*gen)(c, NULL, tab, ntab, i, &dir)){
		case -1:
			if(c->qid.type & QTDIR){
				if(c->name == NULL)
					elem = "???";
				else if(strcmp(c->name->s, "/") == 0)
					elem = "/";
				else
					for(elem=p=c->name->s; *p; p++)
						if(*p == '/')
							elem = p+1;
				devdir(c, c->qid, elem, 0, eve, DMDIR|0555, &dir);
				n = convD2M(&dir, db, n);
				if(n == 0)
					error(Ebadarg);
				return n;
			}
			printd("%s %s: devstat %C %llux\n",
				up->text, up->env->user,
				devtab[c->type]->dc, c->qid.path);

			error(Enonexist);
		case 0:
			break;
		case 1:
			if(c->qid.path == dir.qid.path) {
				if(c->flag&CMSG)
					dir.mode |= DMMOUNT;
				n = convD2M(&dir, db, n);
				if(n == 0)
					error(Ebadarg);
				return n;
			}
			break;
		}
}

long
devdirread(struct chan *c, char *d, long n,
	   struct dirtab *tab, int ntab, Devgen *gen)
{
	long m, dsz;
	/* this is gross. Make it 2 so we have room at the end for
	 * bad things.
	 */
	struct dir dir[4];

	for(m=0; m<n; c->dri++) {
		switch((*gen)(c, NULL, tab, ntab, c->dri, &dir[0])){
		case -1:
			return m;

		case 0:
			break;

		case 1:
			dsz = convD2M(&dir[0], ( uint8_t *)d, n-m);
			if(dsz <= BIT16SZ){	/* <= not < because this isn't stat; read is stuck */
				if(m == 0)
					error(Eshort);
				return m;
			}
			m += dsz;
			d += dsz;
			break;
		}
	}

	return m;
}

/*
 * error(Eperm) if open permission not granted for up->env->user.
 */
void
devpermcheck(char *fileuid, uint32_t perm, int omode)
{
	uint32_t t;
	static int access[] = { 0400, 0200, 0600, 0100 };

	if(strcmp(current->user, fileuid) == 0)
		perm <<= 0;
	else
	if(strcmp(current->user, eve) == 0)
		perm <<= 3;
	else
		perm <<= 6;

	t = access[omode&3];
	if((t&perm) != t)
		error(Eperm);
}

struct chan*
devopen(struct chan *c, int omode, struct dirtab *tab, int ntab, Devgen *gen)
{
	int i;
	struct dir dir;

	for(i=0;; i++) {
		switch((*gen)(c, NULL, tab, ntab, i, &dir)){
		case -1:
			goto Return;
		case 0:
			break;
		case 1:
			if(c->qid.path == dir.qid.path) {
				devpermcheck(dir.uid, dir.mode, omode);
				goto Return;
			}
			break;
		}
	}
Return:
	c->offset = 0;
	if((c->qid.type&QTDIR) && omode!=OREAD)
		error(Eperm);
	c->mode = openmode(omode);
	c->flag |= COPEN;
	return c;
}

void
devcreate(struct chan*c, char *unused_char_p_t, int unused_int, uint32_t u)
{
	error(Eperm);
}

struct block*
devbread(struct chan *c, long n, uint32_t offset)
{
	ERRSTACK(2);
	struct block *bp;

	bp = allocb(n);
	if(bp == 0)
		error(Enomem);
	if(waserror()) {
		freeb(bp);
		nexterror();
	}
	bp->wp += devtab[c->type]->read(c, bp->wp, n, offset);
	poperror();
	return bp;
}

long
devbwrite(struct chan *c, struct block *bp, uint32_t offset)
{
	ERRSTACK(2);
	long n;

	if(waserror()) {
		freeb(bp);
		nexterror();
	}
	n = devtab[c->type]->write(c, bp->rp, BLEN(bp), offset);
	poperror();
	freeb(bp);

	return n;
}

void
devremove(struct chan*c)
{
	error(Eperm);
}

int
devwstat(struct chan*c, uint8_t *unused_uint8_p_t, int i)
{
	error(Eperm);
	return 0;
}

void
devpower(int i)
{
	error(Eperm);
}

#if 0
int
devconfig( int unused_int, char *c, DevConf *)
{
	error(Eperm);
	return 0;
}
#endif
/*
 * check that the name in a wstat is plausible
 */
void
validwstatname(char *name)
{
	validname(name, 0);
	if(strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
		error(Efilename);
}

struct dev*
devbyname(char *name)
{
	int i;

	for(i = 0; devtab[i] != NULL; i++)
		if(strcmp(devtab[i]->name, name) == 0)
			return devtab[i];
	return NULL;
}

