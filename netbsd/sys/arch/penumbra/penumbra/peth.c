/*	$NetBSD$	*/

/*
 * Penumbra Ethernet adapter (CLASS_ETHERNET) network interface driver.
 *
 * The device holds frames in memory of its own: fixed-size slots
 * mapped in its window, arranged as a receive ring and a transmit
 * ring (doc/system/devices/ethernet.md).  There is no DMA engine, so
 * the driver is the transport, copying each frame between a slot and
 * an mbuf.
 *
 * A ring is run by two counts of frames, one written by each side:
 * the device advances RX_PROD and TX_CONS, the driver RX_CONS and
 * TX_PROD.  The counts are 16 bits wide and wrap, so every difference
 * between two of them is taken as a uint16_t.
 *
 * Both interrupt sources are conditions rather than events.  RX holds
 * while the receive ring has a frame the driver has not released, so
 * draining the ring is what ends it.  TX holds while the transmit ring
 * has fewer frames queued than TX_WM, which the driver leaves at 0,
 * and so off, except while it waits for room in a full ring.
 *
 * Only a device that manages its own link is driven.  One whose PHY is
 * software's to manage (CAP.MDIO) is refused at attach.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/device.h>
#include <sys/callout.h>
#include <sys/cprng.h>
#include <sys/errno.h>
#include <sys/evcnt.h>
#include <sys/intr.h>
#include <sys/mbuf.h>
#include <sys/rndsource.h>
#include <sys/socket.h>
#include <sys/sockio.h>

#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_ether.h>
#include <net/if_media.h>
#include <net/bpf.h>

#include <machine/bootinfo.h>
#include <machine/bus_defs.h>
#include <machine/bus_funcs.h>
#include <machine/pbbus.h>

/* Registers, word-strided. */
#define	PE_CAP			0x000
#define	 PE_CAP_VERSION(x)	((u_int)__SHIFTOUT((x), __BITS(7, 0)))
#define	 PE_CAP_MDIO		__BIT(8)
#define	 PE_CAP_ADDR		__BIT(9)
#define	PE_IRQ_STATUS		0x004
#define	PE_IRQ_ENABLE		0x008
#define	 PE_IRQ_RX		__BIT(0)
#define	 PE_IRQ_TX		__BIT(1)
#define	PE_CTRL			0x00c
#define	 PE_CTRL_RX_EN		__BIT(0)
#define	 PE_CTRL_TX_EN		__BIT(1)
#define	 PE_CTRL_PROMISC	__BIT(2)
#define	 PE_CTRL_ALLMULTI	__BIT(3)
#define	PE_LINK			0x010
#define	 PE_LINK_UP		__BIT(0)
#define	 PE_LINK_SPEED(x)	((u_int)__SHIFTOUT((x), __BITS(9, 8)))
#define	PE_ADDR_LO		0x018
#define	PE_ADDR_HI		0x01c
#define	PE_FACTORY_LO		0x020
#define	PE_FACTORY_HI		0x024
#define	 PE_FACTORY_VALID	__BIT(31)
#define	PE_RX_GEOM		0x040
#define	PE_RX_PROD		0x044
#define	PE_RX_CONS		0x048
#define	PE_RX_DROP		0x04c
#define	PE_TX_GEOM		0x050
#define	PE_TX_PROD		0x054
#define	PE_TX_CONS		0x058
#define	PE_TX_WM		0x05c
#define	 PE_GEOM_SLOTS(x)	((u_int)__SHIFTOUT((x), __BITS(15, 0)))
#define	 PE_GEOM_SLOT_SIZE(x)	((u_int)__SHIFTOUT((x), __BITS(31, 16)))

/* The slot apertures, and the LEN that heads a slot. */
#define	PE_TX_APERTURE		0x08000
#define	PE_RX_APERTURE		0x10000
#define	PE_SLOT_PAD		2

/*
 * A slot starts its frame 2 bytes past a word boundary, which is where
 * an mbuf keeps one so that the header after the Ethernet header is
 * aligned.  The copies below rely on the two being the same.
 */
__CTASSERT(PE_SLOT_PAD == ETHER_ALIGN);

/* The device has this long after reset to produce a factory address. */
#define	PE_FACTORY_WAIT_MS	200
/*
 * Clearing TX_EN empties the transmit ring once the frame on the wire
 * has ended, which the longest frame at the slowest speed does in
 * 1.3 ms.
 */
#define	PE_STOP_WAIT_MS		10
/* A full transmit ring that stays full this long is a fault. */
#define	PE_WATCHDOG_S		5

struct peth_softc {
	device_t		sc_dev;
	bus_space_tag_t		sc_iot;
	bus_space_handle_t	sc_ioh;
	void			*sc_ih;

	struct ethercom		sc_ec;
	struct ifmedia		sc_media;
	callout_t		sc_tick;
	krndsource_t		sc_rnd;
	uint8_t			sc_enaddr[ETHER_ADDR_LEN];

	/* Ring geometry.  The slot counts are powers of two. */
	u_int			sc_rx_slots;
	u_int			sc_rx_slot_size;
	u_int			sc_tx_slots;
	u_int			sc_tx_slot_size;

	/* The counts the driver writes, as it last wrote them. */
	uint16_t		sc_rx_cons;
	uint16_t		sc_tx_prod;

	/* LINK and RX_DROP as the tick last read them. */
	uint32_t		sc_link;
	uint32_t		sc_rx_drop;

	/* Waits for room in a full transmit ring. */
	struct evcnt		sc_ev_txwait;
};

#define	PE_RD(sc, r)	bus_space_read_4((sc)->sc_iot, (sc)->sc_ioh, (r))
#define	PE_WR(sc, r, v)	bus_space_write_4((sc)->sc_iot, (sc)->sc_ioh, (r), (v))

static int	peth_match(device_t, cfdata_t, void *);
static void	peth_attach(device_t, device_t, void *);

static int	peth_init(struct ifnet *);
static void	peth_stop(struct ifnet *, int);
static void	peth_start(struct ifnet *);
static int	peth_ioctl(struct ifnet *, u_long, void *);
static void	peth_watchdog(struct ifnet *);
static int	peth_ifm_change(struct ifnet *);
static void	peth_ifm_status(struct ifnet *, struct ifmediareq *);
static int	peth_intr(void *);
static void	peth_tick(void *);

CFATTACH_DECL_NEW(peth, sizeof(struct peth_softc),
    peth_match, peth_attach, NULL, NULL);

static int
peth_match(device_t parent, cfdata_t cf, void *aux)
{
	struct pbbus_attach_args *pa = aux;

	return pa->pb_class == ACFG_CLASS_ETHERNET;
}

/*
 * The station address: the device's factory address when it has one,
 * and otherwise a locally administered address made up here.  Returns
 * whether it came from the device.
 */
static bool
peth_station_address(struct peth_softc *sc, uint32_t cap)
{
	uint32_t lo, hi = 0;
	bool factory;
	int ms;

	if ((cap & PE_CAP_ADDR) != 0) {
		for (ms = 0; ms < PE_FACTORY_WAIT_MS; ms++) {
			hi = PE_RD(sc, PE_FACTORY_HI);
			if ((hi & PE_FACTORY_VALID) != 0)
				break;
			delay(1000);
		}
	}
	factory = (hi & PE_FACTORY_VALID) != 0;
	if (factory) {
		lo = PE_RD(sc, PE_FACTORY_LO);
	} else {
		/* Byte 0 bit 0 clear: one station.  Bit 1 set: local. */
		lo = (cprng_fast32() & ~0x01U) | 0x02U;
		hi = cprng_fast32();
	}

	sc->sc_enaddr[0] = lo;
	sc->sc_enaddr[1] = lo >> 8;
	sc->sc_enaddr[2] = lo >> 16;
	sc->sc_enaddr[3] = lo >> 24;
	sc->sc_enaddr[4] = hi;
	sc->sc_enaddr[5] = hi >> 8;
	return factory;
}

static void
peth_attach(device_t parent, device_t self, void *aux)
{
	struct peth_softc *sc = device_private(self);
	struct pbbus_attach_args *pa = aux;
	struct ifnet *ifp = &sc->sc_ec.ec_if;
	uint32_t cap, rx, tx;
	bool factory;
	int error;

	sc->sc_dev = self;
	sc->sc_iot = pa->pb_iot;

	error = bus_space_map(sc->sc_iot, pa->pb_addr, pa->pb_size,
	    0, &sc->sc_ioh);
	if (error) {
		aprint_error(": can't map Ethernet window: %d\n", error);
		return;
	}

	cap = PE_RD(sc, PE_CAP);
	rx = PE_RD(sc, PE_RX_GEOM);
	tx = PE_RD(sc, PE_TX_GEOM);
	sc->sc_rx_slots = PE_GEOM_SLOTS(rx);
	sc->sc_rx_slot_size = PE_GEOM_SLOT_SIZE(rx);
	sc->sc_tx_slots = PE_GEOM_SLOTS(tx);
	sc->sc_tx_slot_size = PE_GEOM_SLOT_SIZE(tx);

	aprint_naive("\n");
	aprint_normal(": Penumbra Ethernet (v%u, rx %u, tx %u)\n",
	    PE_CAP_VERSION(cap), sc->sc_rx_slots, sc->sc_tx_slots);

	if (PE_CAP_VERSION(cap) != 1) {
		aprint_error_dev(self, "unsupported version\n");
		return;
	}
	if ((cap & PE_CAP_MDIO) != 0) {
		aprint_error_dev(self,
		    "the device's PHY needs managing, which is not supported\n");
		return;
	}
	if (!powerof2(sc->sc_rx_slots) || !powerof2(sc->sc_tx_slots) ||
	    sc->sc_rx_slots == 0 || sc->sc_tx_slots == 0 ||
	    sc->sc_rx_slot_size < MCLBYTES || sc->sc_tx_slot_size < MCLBYTES ||
	    PE_TX_APERTURE + sc->sc_tx_slots * sc->sc_tx_slot_size >
	    PE_RX_APERTURE ||
	    PE_RX_APERTURE + sc->sc_rx_slots * sc->sc_rx_slot_size >
	    pa->pb_size) {
		aprint_error_dev(self, "device reports rings it cannot hold\n");
		return;
	}

	/* Quiet until the interface is brought up. */
	PE_WR(sc, PE_IRQ_ENABLE, 0);
	PE_WR(sc, PE_TX_WM, 0);
	PE_WR(sc, PE_CTRL, 0);

	factory = peth_station_address(sc, cap);
	aprint_normal_dev(self, "Ethernet address %s%s\n",
	    ether_sprintf(sc->sc_enaddr), factory ? "" : " (made up)");

	strlcpy(ifp->if_xname, device_xname(self), IFNAMSIZ);
	ifp->if_softc = sc;
	ifp->if_flags = IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST;
	ifp->if_init = peth_init;
	ifp->if_stop = peth_stop;
	ifp->if_start = peth_start;
	ifp->if_ioctl = peth_ioctl;
	ifp->if_watchdog = peth_watchdog;
	IFQ_SET_READY(&ifp->if_snd);

	/* A slot holds 1518 bytes, an Ethernet frame with one VLAN tag. */
	sc->sc_ec.ec_capabilities |= ETHERCAP_VLAN_MTU;

	/* The device runs its own link, so there is one medium. */
	sc->sc_ec.ec_ifmedia = &sc->sc_media;
	ifmedia_init(&sc->sc_media, 0, peth_ifm_change, peth_ifm_status);
	ifmedia_add(&sc->sc_media, IFM_ETHER | IFM_AUTO, 0, NULL);
	ifmedia_set(&sc->sc_media, IFM_ETHER | IFM_AUTO);

	if_attach(ifp);
	if_deferred_start_init(ifp, NULL);
	ether_ifattach(ifp, sc->sc_enaddr);

	callout_init(&sc->sc_tick, 0);
	callout_setfunc(&sc->sc_tick, peth_tick, sc);
	rnd_attach_source(&sc->sc_rnd, device_xname(self), RND_TYPE_NET,
	    RND_FLAG_DEFAULT);
	evcnt_attach_dynamic(&sc->sc_ev_txwait, EVCNT_TYPE_MISC, NULL,
	    device_xname(self), "tx ring full");

	sc->sc_ih = intr_establish_xname(0, IPL_NET, peth_intr, sc,
	    device_xname(self));
	if (sc->sc_ih == NULL)
		aprint_error_dev(self, "can't establish interrupt\n");
}

/*
 * Write the filter modes to CTRL, with both directions enabled.  The
 * device takes group addresses all or none, so any membership at all
 * selects all.
 */
static void
peth_set_filter(struct peth_softc *sc)
{
	struct ethercom *ec = &sc->sc_ec;
	struct ifnet *ifp = &ec->ec_if;
	struct ether_multistep step;
	struct ether_multi *enm;
	uint32_t ctrl = PE_CTRL_RX_EN | PE_CTRL_TX_EN;

	if ((ifp->if_flags & IFF_PROMISC) != 0)
		ctrl |= PE_CTRL_PROMISC;

	ETHER_LOCK(ec);
	ETHER_FIRST_MULTI(step, ec, enm);
	if (enm != NULL) {
		ifp->if_flags |= IFF_ALLMULTI;
		ctrl |= PE_CTRL_ALLMULTI;
	} else {
		ifp->if_flags &= ~IFF_ALLMULTI;
	}
	ETHER_UNLOCK(ec);

	PE_WR(sc, PE_CTRL, ctrl);
}

static int
peth_init(struct ifnet *ifp)
{
	struct peth_softc *sc = ifp->if_softc;
	const uint8_t *ea = CLLADDR(ifp->if_sadl);
	int s;

	s = splnet();

	peth_stop(ifp, 0);

	PE_WR(sc, PE_ADDR_LO,
	    ea[0] | (ea[1] << 8) | (ea[2] << 16) | ((uint32_t)ea[3] << 24));
	PE_WR(sc, PE_ADDR_HI, ea[4] | (ea[5] << 8));

	/* Both rings start empty: release what arrived before. */
	sc->sc_rx_cons = PE_RD(sc, PE_RX_PROD);
	PE_WR(sc, PE_RX_CONS, sc->sc_rx_cons);
	sc->sc_tx_prod = PE_RD(sc, PE_TX_PROD);
	sc->sc_rx_drop = PE_RD(sc, PE_RX_DROP);

	peth_set_filter(sc);
	PE_WR(sc, PE_IRQ_ENABLE, PE_IRQ_RX | PE_IRQ_TX);

	ifp->if_flags |= IFF_RUNNING;
	ifp->if_flags &= ~IFF_OACTIVE;

	/* A value LINK never reads, so the first tick reports the link. */
	sc->sc_link = ~0U;
	callout_schedule(&sc->sc_tick, 1);

	splx(s);
	return 0;
}

static void
peth_stop(struct ifnet *ifp, int disable)
{
	struct peth_softc *sc = ifp->if_softc;
	int n;

	callout_stop(&sc->sc_tick);

	PE_WR(sc, PE_IRQ_ENABLE, 0);
	PE_WR(sc, PE_TX_WM, 0);
	PE_WR(sc, PE_CTRL, 0);

	/*
	 * A frame queued before the device has emptied the ring would be
	 * discarded with the rest, so wait the ring out here.
	 */
	for (n = 0; n < PE_STOP_WAIT_MS * 10; n++) {
		if ((uint16_t)PE_RD(sc, PE_TX_CONS) ==
		    (uint16_t)PE_RD(sc, PE_TX_PROD))
			break;
		delay(100);
	}
	if (n == PE_STOP_WAIT_MS * 10)
		aprint_error_dev(sc->sc_dev, "transmit ring did not empty\n");

	ifp->if_flags &= ~(IFF_RUNNING | IFF_OACTIVE);
	ifp->if_timer = 0;
}

/*
 * Store len bytes at an offset in a transmit slot.  Whole words go as
 * words when the source and the offset share an alignment, which the
 * first piece of a frame does: the slot and the mbuf both hold the
 * frame 2 bytes past a word boundary.
 */
static void
peth_put(struct peth_softc *sc, bus_size_t off, const uint8_t *p, size_t len)
{
	size_t words;

	if ((((uintptr_t)p ^ off) & 3) == 0) {
		for (; len > 0 && (off & 3) != 0; len--)
			bus_space_write_1(sc->sc_iot, sc->sc_ioh, off++, *p++);
		words = len >> 2;
		bus_space_write_region_4(sc->sc_iot, sc->sc_ioh, off,
		    (const uint32_t *)(const void *)p, words);
		off += words << 2;
		p += words << 2;
		len &= 3;
	}
	for (; len > 0; len--)
		bus_space_write_1(sc->sc_iot, sc->sc_ioh, off++, *p++);
}

/* Copy a frame into the transmit slot TX_PROD names, and queue it. */
static void
peth_tx_frame(struct peth_softc *sc, struct mbuf *m0)
{
	bus_size_t slot = PE_TX_APERTURE +
	    (sc->sc_tx_prod & (sc->sc_tx_slots - 1)) * sc->sc_tx_slot_size;
	bus_size_t off = slot + PE_SLOT_PAD;
	struct mbuf *m;

	for (m = m0; m != NULL; m = m->m_next) {
		peth_put(sc, off, mtod(m, const uint8_t *), m->m_len);
		off += m->m_len;
	}
	bus_space_write_2(sc->sc_iot, sc->sc_ioh, slot, m0->m_pkthdr.len);

	PE_WR(sc, PE_TX_PROD, ++sc->sc_tx_prod);
}

/*
 * The transmit ring is full and a frame is waiting for a slot.  Ask
 * for the TX interrupt, which peth_intr answers by restarting output.
 */
static void
peth_tx_wait(struct peth_softc *sc)
{
	PE_WR(sc, PE_TX_WM, (sc->sc_tx_slots + 1) / 2);
}

static void
peth_start(struct ifnet *ifp)
{
	struct peth_softc *sc = ifp->if_softc;
	struct mbuf *m;

	if ((ifp->if_flags & (IFF_RUNNING | IFF_OACTIVE)) != IFF_RUNNING)
		return;

	for (;;) {
		IFQ_POLL(&ifp->if_snd, m);
		if (m == NULL)
			break;

		if ((uint16_t)(sc->sc_tx_prod - PE_RD(sc, PE_TX_CONS)) >=
		    sc->sc_tx_slots) {
			ifp->if_flags |= IFF_OACTIVE;
			ifp->if_timer = PE_WATCHDOG_S;
			sc->sc_ev_txwait.ev_count++;
			peth_tx_wait(sc);
			break;
		}

		IFQ_DEQUEUE(&ifp->if_snd, m);
		if (m->m_pkthdr.len > ETHER_MAX_LEN - ETHER_CRC_LEN +
		    ETHER_VLAN_ENCAP_LEN) {
			if_statinc(ifp, if_oerrors);
			m_freem(m);
			continue;
		}

		peth_tx_frame(sc, m);
		bpf_mtap(ifp, m, BPF_D_OUT);
		if_statinc(ifp, if_opackets);
		m_freem(m);
	}
}

/*
 * An mbuf holding the frame in a receive slot, or NULL when there is
 * no mbuf for it.  The slot is copied as whole words from its start
 * into a word-aligned buffer: LEN lands in the two bytes an mbuf
 * leaves spare ahead of the frame, and the frame where it belongs.
 */
static struct mbuf *
peth_rx_frame(struct peth_softc *sc, bus_size_t slot, u_int len)
{
	u_int words = howmany(PE_SLOT_PAD + len, sizeof(uint32_t));
	struct mbuf *m;

	MGETHDR(m, M_DONTWAIT, MT_DATA);
	if (m == NULL)
		return NULL;
	if (words * sizeof(uint32_t) > MHLEN) {
		MCLGET(m, M_DONTWAIT);
		if ((m->m_flags & M_EXT) == 0) {
			m_freem(m);
			return NULL;
		}
	}

	bus_space_read_region_4(sc->sc_iot, sc->sc_ioh, slot,
	    mtod(m, uint32_t *), words);
	m->m_data += ETHER_ALIGN;
	m->m_pkthdr.len = m->m_len = len;
	return m;
}

/* Hand up every frame in the receive ring, releasing each slot. */
static void
peth_rxintr(struct peth_softc *sc)
{
	struct ifnet *ifp = &sc->sc_ec.ec_if;
	struct mbuf *m;
	bus_size_t slot;
	u_int len;

	while (sc->sc_rx_cons != (uint16_t)PE_RD(sc, PE_RX_PROD)) {
		slot = PE_RX_APERTURE +
		    (sc->sc_rx_cons & (sc->sc_rx_slots - 1)) *
		    sc->sc_rx_slot_size;
		len = bus_space_read_4(sc->sc_iot, sc->sc_ioh, slot) & 0xffff;

		/*
		 * The device stores only frames of 60 to 1518 bytes.  A
		 * length outside what a cluster holds would overrun the
		 * mbuf, so it is checked all the same.
		 */
		if (len <= MCLBYTES - ETHER_ALIGN)
			m = peth_rx_frame(sc, slot, len);
		else
			m = NULL;

		PE_WR(sc, PE_RX_CONS, ++sc->sc_rx_cons);

		if (m == NULL) {
			if_statinc(ifp, if_ierrors);
			continue;
		}
		m_set_rcvif(m, ifp);
		if_percpuq_enqueue(ifp->if_percpuq, m);
	}
}

static int
peth_intr(void *arg)
{
	struct peth_softc *sc = arg;
	struct ifnet *ifp = &sc->sc_ec.ec_if;
	uint32_t status;

	/* Both sources are masked while the interface is down. */
	if ((ifp->if_flags & IFF_RUNNING) == 0)
		return 0;
	status = PE_RD(sc, PE_IRQ_STATUS) & (PE_IRQ_RX | PE_IRQ_TX);
	if (status == 0)
		return 0;

	if ((status & PE_IRQ_RX) != 0)
		peth_rxintr(sc);

	if ((status & PE_IRQ_TX) != 0) {
		/* There is room again: the source goes back to off. */
		PE_WR(sc, PE_TX_WM, 0);
		ifp->if_flags &= ~IFF_OACTIVE;
		ifp->if_timer = 0;
		if_schedule_deferred_start(ifp);
	}

	rnd_add_uint32(&sc->sc_rnd, status);
	return 1;
}

/*
 * Once a second: the device raises no interrupt when its link changes,
 * and counts the frames it drops for want of a slot without saying so.
 */
static void
peth_tick(void *arg)
{
	struct peth_softc *sc = arg;
	struct ifnet *ifp = &sc->sc_ec.ec_if;
	uint32_t link, drop;
	int s;

	s = splnet();

	link = PE_RD(sc, PE_LINK);
	if (link != sc->sc_link) {
		sc->sc_link = link;
		if_link_state_change(ifp, (link & PE_LINK_UP) != 0 ?
		    LINK_STATE_UP : LINK_STATE_DOWN);
	}

	drop = PE_RD(sc, PE_RX_DROP);
	if_statadd(ifp, if_iqdrops, drop - sc->sc_rx_drop);
	sc->sc_rx_drop = drop;

	splx(s);

	callout_schedule(&sc->sc_tick, hz);
}

/*
 * The device sends whatever the state of its link, so the ring always
 * drains: one that stays full means a lost interrupt or a dead device.
 */
static void
peth_watchdog(struct ifnet *ifp)
{

	aprint_error_ifnet(ifp, "transmit ring stuck\n");
	if_statinc(ifp, if_oerrors);
	peth_init(ifp);
}

static int
peth_ioctl(struct ifnet *ifp, u_long command, void *data)
{
	struct peth_softc *sc = ifp->if_softc;
	int s, error;

	s = splnet();

	switch (command) {
	case SIOCSIFFLAGS:
		if ((error = ifioctl_common(ifp, command, data)) != 0)
			break;
		switch (ifp->if_flags & (IFF_UP | IFF_RUNNING)) {
		case IFF_RUNNING:
			peth_stop(ifp, 0);
			break;
		case IFF_UP:
			error = peth_init(ifp);
			break;
		case IFF_UP | IFF_RUNNING:
			peth_set_filter(sc);
			break;
		default:
			break;
		}
		break;

	default:
		error = ether_ioctl(ifp, command, data);
		if (error == ENETRESET) {
			if ((ifp->if_flags & IFF_RUNNING) != 0)
				peth_set_filter(sc);
			error = 0;
		}
		break;
	}

	splx(s);
	return error;
}

static int
peth_ifm_change(struct ifnet *ifp)
{

	/* The device runs its own link: there is nothing to select. */
	return 0;
}

static void
peth_ifm_status(struct ifnet *ifp, struct ifmediareq *ifmr)
{
	/* By LINK.SPEED; the fourth code is reserved. */
	static const int subtype[4] = {
		IFM_10_T, IFM_100_TX, IFM_1000_T, IFM_NONE
	};
	struct peth_softc *sc = ifp->if_softc;
	uint32_t link = PE_RD(sc, PE_LINK);

	ifmr->ifm_status = IFM_AVALID;
	ifmr->ifm_active = IFM_ETHER;
	if ((link & PE_LINK_UP) != 0) {
		ifmr->ifm_status |= IFM_ACTIVE;
		ifmr->ifm_active |= subtype[PE_LINK_SPEED(link)] | IFM_FDX;
	} else {
		ifmr->ifm_active |= IFM_NONE;
	}
}
