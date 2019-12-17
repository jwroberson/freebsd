#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/libkern.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/proc.h>
#include <sys/kthread.h>
#include <sys/conf.h>
#include <sys/mbuf.h>
#include <sys/smp.h>


struct umaperf_cpu {
	struct mtx 	upc_lock;
	struct mbuf	*upc_head;
	uint64_t	upc_allocs;
	uint64_t	upc_frees;
	int		upc_self;
} __aligned(CACHE_LINE_SIZE * 2);

static struct umaperf_cpu	up_cpu[MAXCPU];
static int umaperf_cpus = 32;
static int umaperf_started;
static int umaperf_pkts = 10;
static int umaperf_iterations = 1000000;
static volatile int umaperf_completed;

static void
umaperf_dequeue(struct umaperf_cpu *upc)
{
	struct mbuf *m, *n;

	/*
	 * Dequeue and free local packets.
	 */
	mtx_lock(&upc->upc_lock);
	m = upc->upc_head;
	upc->upc_head = NULL;
	mtx_unlock(&upc->upc_lock);
	for (; m != NULL; m = n) {
		n = m->m_nextpkt;
		upc->upc_frees++;
		m_free(m);
	}

}

static void
umaperf_enqueue(struct umaperf_cpu *upc, struct mbuf *head, struct mbuf *tail)
{

	/*
	 * Enqueue singly linked list.
	 */
	mtx_lock(&upc->upc_lock);
	if (upc->upc_head != NULL)
		tail->m_nextpkt = upc->upc_head;
	upc->upc_head = head;
	mtx_unlock(&upc->upc_lock);
}

static void
umaperf_thread(void *arg)
{
	struct umaperf_cpu *upc = arg;
	struct mbuf *head, *tail, *m;
	int i, j, c;

	for (i = 0; i < umaperf_iterations; i++) {
		/*
		 * Allocate and touch multiple 2k mbufs.
		 */
		head = tail = m = NULL;
		for (j = 0; j < umaperf_pkts; j++) {
			m = m_getcl(M_WAITOK, MT_DATA, M_PKTHDR);
			if (head == NULL)
				head = m;
			else
				tail->m_nextpkt = m;
			tail = m;
			bzero(m->m_data, m->m_len);
			upc->upc_allocs++;
		}

		/*
		 * Select the remote cpu.
		 */
		do {
			c = random() % umaperf_cpus;
		} while (c == upc->upc_self);
		umaperf_enqueue(&up_cpu[c], head, tail);

		/*
		 * Process local work.
		 */
		umaperf_dequeue(upc);
	}
	atomic_add_int(&umaperf_completed, 1);
	while (umaperf_completed != umaperf_started) {
		umaperf_dequeue(upc);
		pause("prf", 1);
	}
}

static void
umaperf_start(void)
{
	uint64_t allocs, frees;
	int i;

	umaperf_started = umaperf_cpus;
	umaperf_completed = 0;
	for (i = 0; i < umaperf_started; i++)
		kthread_add((void (*)(void *))umaperf_thread,
		    &up_cpu[i], curproc, NULL, 0, 0, "umaperf-%d", i);

	while (umaperf_completed != umaperf_started)
		pause("prf", hz/2);

	allocs = frees = 0;
	for (i = 0; i < umaperf_started; i++) {
		/* Don't leak any memory. */
		umaperf_dequeue(&up_cpu[i]);
		allocs += up_cpu[i].upc_allocs;
		frees += up_cpu[i].upc_frees;
	}
	printf("UMA Perf: %d threads did %jd allocs and %jd frees",
	    umaperf_started, (intmax_t)allocs, (intmax_t)frees);
}

static int
umaperf_modevent(module_t mod, int what, void *arg)
{
	int i;

	switch (what) {
	case MOD_LOAD:
		bzero(&up_cpu, sizeof(up_cpu));
		for (i = 0; i < MAXCPU; i++) {
			mtx_init(&up_cpu[i].upc_lock, "UMA Perf", NULL,
			    MTX_DEF);
			up_cpu[i].upc_self = i;
		}
		umaperf_start();
		break;
	case MOD_UNLOAD:
		for (i = 0; i < MAXCPU; i++)
			mtx_destroy(&up_cpu[i].upc_lock);
		break;
	default:
		break;
	}
	return (0);
}

moduledata_t umaperf_meta = {
	"umaperf",
	umaperf_modevent,
	NULL
};
DECLARE_MODULE(umaperf, umaperf_meta, SI_SUB_DRIVERS, SI_ORDER_MIDDLE);
MODULE_VERSION(umaperf, 1);
