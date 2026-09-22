/*
 * ScreamTracker 3 replay semantics, one tick at a time.
 *
 * A row is read on its first tick, where notes start and the
 * once-a-row effects apply; slides and vibrato act on each later tick.
 * Where trackers disagree, Scream Tracker 3.21 is the reference, as
 * st3play carries it.
 *
 * Its channels share one remembered info byte, which any row's
 * command writes and most commands read when their own is 00, so the
 * memory is kept once per channel rather than once per effect.
 */

#include <math.h>
#include <stdio.h>

#include "replay.h"

/* A command is its letter's place in the alphabet. */
#define	CMD(letter)	((letter) - 'A' + 1)

#define	NOTES		12
#define	PERIOD_MIN	64	/* where a sounding period is held */
#define	PERIOD_MAX	32767

/* A note's period is the octave's, shifted down by its own octave and
 * scaled by the rate its sample is tuned to. */
#define	PERIOD_C2SPD	8363

/* The periods of one octave, as the format defines them. */
static const int note_period[NOTES] = {
	1712, 1616, 1524, 1440, 1356, 1280, 1208, 1140, 1076, 1016, 960, 907,
};

/* Half a sine, 0..255: the vibrato waveform. */
static int sine_table[32];

static void
tables_init(void)
{
	static bool done;
	int i;

	if (done)
		return;
	for (i = 0; i < 32; i++)
		sine_table[i] = (int)floor(255 * sin(M_PI * i / 32));
	done = true;
}

static int
clamp(int v, int lo, int hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

/* The period a note plays at on a sample tuned to c2spd. */
static int
note_period_of(int note, uint32_t c2spd)
{
	uint32_t period;
	int octave = note >> 4, semitone = note & 0xF;

	if (semitone >= NOTES || c2spd == 0)
		return 0;
	period = PERIOD_C2SPD *
	    (uint32_t)((16 * note_period[semitone]) >> octave) / c2spd;
	return period > PERIOD_MAX ? PERIOD_MAX : (int)period;
}

/*
 * Where a sample plays from: Scream Tracker plays through to the end
 * of the loop, or of the sample where there is none, and then repeats
 * the loop.  A start past all of that is a sound that cannot begin.
 */
static void
sample_regions(const struct s3m_sample *smp, uint32_t offset,
    struct replay_voice *v)
{
	uint32_t first = smp->loop_length != 0 ?
	    smp->loop_start + smp->loop_length : smp->length;

	if (offset >= first)
		return;
	v->play.offset = smp->offset + offset;
	v->play.length = first - offset;
	v->loop.offset = smp->offset + smp->loop_start;
	v->loop.length = smp->loop_length;
}

/* ── Effects ─────────────────────────────────────────────────── */

/*
 * Dxy: a nibble of F makes the slide a fine one, which moves once on
 * the row's first tick.  The rest move on the later ticks — and on the
 * first as well, for the files ST3.00 wrote — sliding up by the high
 * nibble, or down by the low one where that is set.
 */
static void
volume_slide(struct st3_chan *c, int info, bool first, bool fast)
{
	int x = info >> 4, y = info & 0xF;

	if (y == 0xF && x != 0) {
		if (first)
			c->volume += x;
	} else if (x == 0xF && y != 0) {
		if (first)
			c->volume -= y;
	} else if (!first || fast) {
		c->volume += y == 0 ? x : -y;
	}
	c->volume = clamp(c->volume, 0, 63);
	c->out_volume = c->volume;
}

/*
 * Exx and Fxx: on the row's first tick, a high nibble of F or E makes
 * the slide fine or extra fine; the rest slide on each later tick.
 * Down lengthens the period and up shortens it.
 */
static void
porta(struct st3_chan *c, int info, bool first, bool down)
{
	int by;

	if (c->period == 0)
		return;
	if (first) {
		if ((info >> 4) == 0xF)
			by = (info & 0xF) * 4;
		else if ((info >> 4) == 0xE)
			by = info & 0xF;
		else
			return;
	} else {
		if (info >= 0xE0)
			return;
		by = info * 4;
	}
	c->period = clamp(c->period + (down ? by : -by), 0, PERIOD_MAX);
	c->out_period = c->period;
}

/* Hxy: the waveform moves the period by depth 32nds of its own scale,
 * and the position advances by the speed on every tick but the row's
 * first. */
static void
vibrato(struct st3_chan *c)
{
	int delta = sine_table[c->vib_pos & 31] * c->vib_depth >> 5;

	c->out_period = c->period + ((c->vib_pos & 32) ? -delta : delta);
	c->vib_pos = (c->vib_pos + c->vib_speed) & 63;
}

/* What Qxy's high nibble does to the volume at each retrigger. */
static int
retrig_volume(int volume, int x)
{
	switch (x) {
	case 0x1: case 0x2: case 0x3: case 0x4: case 0x5:
		return volume - (1 << (x - 1));
	case 0x6:
		return volume * 10 / 16;
	case 0x7:
		return volume / 2;
	case 0x9: case 0xA: case 0xB: case 0xC: case 0xD:
		return volume + (1 << (x - 9));
	case 0xE:
		return volume * 24 / 16;
	case 0xF:
		return volume * 2;
	default:
		return volume;
	}
}

/* Qxy: the sample restarts from its own start every y ticks, at the
 * volume the high nibble leaves. */
static void
retrigger(struct st3_chan *c, struct replay_voice *v, int info)
{
	int y = info & 0xF;

	if (y == 0 || ++c->retrig_count < y)
		return;
	c->retrig_count = 0;
	c->volume = clamp(retrig_volume(c->volume, info >> 4), 0, 63);
	c->out_volume = c->volume;
	c->trigger_offset = 0;
	v->trigger = true;
}

/* The effects that act once, on a row's first tick. */
static void
row_effect(struct replay *r, struct st3_chan *c, struct replay_voice *v)
{
	const struct s3m *m = &r->song->s3m;
	int info = c->info != 0 ? c->info : c->memory;
	int x = info >> 4, y = info & 0xF;

	switch (c->command) {
	case CMD('A'):
		if (info != 0)
			r->speed = info;
		break;
	case CMD('B'):
		/* A jump names an order by its place in the file. */
		r->jump_order = m->order_of[info];
		break;
	case CMD('C'):
		/* The row is written in decimal, and a nibble past 9
		 * leaves the command meaning nothing. */
		if (x <= 9 && y <= 9 && x * 10 + y < S3M_ROWS)
			r->break_row = x * 10 + y;
		break;
	case CMD('D'):
		volume_slide(c, info, true, s3m_fast_slides(m));
		break;
	case CMD('E'):
		porta(c, info, true, true);
		break;
	case CMD('F'):
		porta(c, info, true, false);
		break;
	case CMD('H'):
		if (x != 0)
			c->vib_speed = x;
		if (y != 0)
			c->vib_depth = y;
		break;
	case CMD('Q'):
		retrigger(c, v, info);
		break;
	case CMD('S'):
		if (x == 0x8)
			c->place = y;
		break;
	case CMD('T'):
		/* Scream Tracker passes over a tempo it cannot run at. */
		if (info > 0x20)
			r->bpm = info;
		break;
	}
}

/* The effects that act on each tick of a row after the first. */
static void
tick_effect(struct replay *r, struct st3_chan *c, struct replay_voice *v)
{
	const struct s3m *m = &r->song->s3m;
	int info = c->info != 0 ? c->info : c->memory;

	c->out_period = c->period;
	c->out_volume = c->volume;

	switch (c->command) {
	case CMD('D'):
		volume_slide(c, info, false, s3m_fast_slides(m));
		break;
	case CMD('E'):
		porta(c, info, false, true);
		break;
	case CMD('F'):
		porta(c, info, false, false);
		break;
	case CMD('H'):
		vibrato(c);
		break;
	case CMD('Q'):
		retrigger(c, v, info);
		break;
	}
}

/* ── Rows ────────────────────────────────────────────────────── */

static void
row_start(struct replay *r, struct replay_tick *t)
{
	const struct s3m *m = &r->song->s3m;
	int pattern = r->song->order[r->order];
	int ch;

	for (ch = 0; ch < m->channels; ch++) {
		const struct s3m_cell *cell = s3m_cell(m, pattern, r->row, ch);
		struct st3_chan *c = &r->st3.chan[ch];
		struct replay_voice *v = &t->voice[ch];

		c->command = cell->command;
		c->info = cell->info;
		/* Any row's info byte becomes the one the commands that
		 * share a memory recall, whatever command carried it. */
		if (cell->info != 0)
			c->memory = cell->info;
		/* Qxy counts on across rows, except where a row says
		 * nothing or slides the volume. */
		if (cell->command == 0 || cell->command == CMD('D'))
			c->retrig_count = 0;
		if (cell->instrument != 0 && cell->instrument <= m->nsamples) {
			const struct s3m_sample *smp =
			    &m->sample[cell->instrument];

			/* An instrument of its own starts nothing: it
			 * leaves its volume, and plays from the start. */
			c->sample = cell->instrument;
			c->volume = clamp(smp->volume, 0, 63);
			c->offset_memory = 0;
		}
		/* Oxx keeps a memory of its own, and the start it names
		 * belongs to a note on its own row, whichever instrument
		 * that note names. */
		if (cell->command == CMD('O') && cell->info != 0)
			c->offset_memory = cell->info;
		if (cell->volume != S3M_VOLUME_NONE && c->sample != 0)
			c->volume = clamp(cell->volume, 0, 63);

		if (cell->note == S3M_NOTE_CUT) {
			/* What was playing stops, and a note needs an
			 * instrument named with it to sound again. */
			c->cut = true;
			c->volume = 0;
			v->trigger = true;
		} else if (cell->note != S3M_NOTE_NONE && c->sample != 0 &&
		    (!c->cut || cell->instrument != 0)) {
			c->cut = false;
			c->note = cell->note;
			c->period = note_period_of(cell->note,
			    m->sample[c->sample].c2spd);
			c->vib_pos = 0;
			c->trigger_offset = (uint32_t)c->offset_memory * 256;
			v->trigger = true;
		}

		c->out_period = c->period;
		c->out_volume = c->volume;
		row_effect(r, c, v);
	}
}

void
st3_start(struct replay *r)
{
	const struct s3m *m = &r->song->s3m;
	int ch;

	tables_init();
	r->speed = m->speed;
	r->bpm = m->tempo;
	r->st3.global_volume = m->global_volume;
	for (ch = 0; ch < m->channels; ch++)
		r->st3.chan[ch].place = m->pan[ch];
}

void
st3_play(struct replay *r, struct replay_tick *t, bool row)
{
	const struct s3m *m = &r->song->s3m;
	int ch;

	/* Only a row's own Oxx starts anywhere but the sample's start. */
	for (ch = 0; ch < m->channels; ch++)
		r->st3.chan[ch].trigger_offset = 0;
	if (row)
		row_start(r, t);
	else
		for (ch = 0; ch < m->channels; ch++)
			tick_effect(r, &r->st3.chan[ch], &t->voice[ch]);

	for (ch = 0; ch < m->channels; ch++) {
		struct st3_chan *c = &r->st3.chan[ch];
		struct replay_voice *v = &t->voice[ch];

		v->instrument = c->sample;
		v->period = c->out_period == 0 ? 0 :
		    clamp(c->out_period, PERIOD_MIN, PERIOD_MAX);
		/* The song's own volume scales every voice. */
		v->volume = c->out_volume * r->st3.global_volume / 64;
		v->pan = s3m_pan(c->place);
		/* A start while the channel is cut plays nothing, which
		 * is how a voice falls silent. */
		if (v->trigger && c->sample != 0 && !c->cut)
			sample_regions(&m->sample[c->sample],
			    c->trigger_offset, v);
	}
}

/* ── What it does not play ───────────────────────────────────── */

/* By command letter; the ones modplay plays are left out. */
static const char *const command_name[27] = {
	NULL, NULL, NULL, NULL, NULL, NULL, NULL,
	"G tone portamento", NULL, "I tremor", "J arpeggio",
	"K vibrato with volume slide", "L portamento with volume slide",
	NULL, NULL, NULL, "P panning slide", NULL, "R tremolo", NULL, NULL,
	"U fine vibrato", "V global volume", "W channel volume",
	"X panning", "Y panbrello", "Z macro",
};

/* By the high nibble of an Sxy; S8 panning is played. */
static const char *const special_name[16] = {
	"S0 filter", "S1 glissando", "S2 finetune", "S3 vibrato waveform",
	"S4 tremolo waveform", "S5", "S6", "S7", NULL, "S9",
	"SA stereo control", "SB pattern loop", "SC note cut",
	"SD note delay", "SE pattern delay", "SF funk repeat",
};

const char *
st3_unsupported(const struct song *song)
{
	static const struct {
		int	flag;
		const char *name;
	} kinds[] = {
		{ S3M_HAS_ADLIB, "AdLib instruments" },
		{ S3M_HAS_PACKED, "packed samples" },
		{ S3M_HAS_16BIT, "16-bit samples" },
		{ S3M_HAS_STEREO, "stereo samples" },
	};
	static char list[512];
	const struct s3m *m = &song->s3m;
	uint32_t commands = 0, specials = 0;
	size_t len = 0, i;
	int o, row, ch;

	for (o = 0; o < song->length; o++)
		for (row = 0; row < S3M_ROWS; row++)
			for (ch = 0; ch < m->channels; ch++) {
				const struct s3m_cell *cell =
				    s3m_cell(m, song->order[o], row, ch);

				if (cell->command > 26)
					continue;
				if (cell->command == CMD('S'))
					specials |= 1U << (cell->info >> 4);
				else if (command_name[cell->command] != NULL)
					commands |= 1U << cell->command;
			}

	list[0] = '\0';
	for (i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++)
		if ((m->unplayable & kinds[i].flag) != 0)
			len += snprintf(list + len, sizeof(list) - len, "%s%s",
			    len > 0 ? ", " : "", kinds[i].name);
	for (i = 1; i < 27; i++)
		if ((commands & 1U << i) != 0)
			len += snprintf(list + len, sizeof(list) - len, "%s%s",
			    len > 0 ? ", " : "", command_name[i]);
	for (i = 0; i < 16; i++)
		if ((specials & 1U << i) != 0 && special_name[i] != NULL)
			len += snprintf(list + len, sizeof(list) - len, "%s%s",
			    len > 0 ? ", " : "", special_name[i]);
	return list[0] != '\0' ? list : NULL;
}
