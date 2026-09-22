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

/* A command's info byte is a pair of nibbles, which the format writes
 * as xy and most commands read as two numbers. */
static int
info_x(int info)
{
	return info >> 4;
}

static int
info_y(int info)
{
	return info & 0xF;
}

/* The row Cxx breaks to, which it writes in decimal; -1 where a nibble
 * past 9 leaves the command meaning nothing. */
static int
decimal_row(int info)
{
	int row = info_x(info) * 10 + info_y(info);

	if (info_x(info) > 9 || info_y(info) > 9 || row >= S3M_ROWS)
		return -1;
	return row;
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
	int x = info_x(info), y = info_y(info);

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
		if (info_x(info) == 0xF)
			by = info_y(info) * 4;
		else if (info_x(info) == 0xE)
			by = info_y(info);
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

/* A note so many semitones above another, carrying into the octave. */
static int
note_above(int note, int semitones)
{
	int semitone = (note & 0xF) + semitones;

	return ((note >> 4) + semitone / NOTES) << 4 | semitone % NOTES;
}

/* Gxx: four period units a tick toward the note beside it, stopping
 * there. */
static void
tone_porta(struct st3_chan *c)
{
	int by = c->porta_speed * 4;

	if (c->period == 0 || c->porta_target == 0)
		return;
	if (c->period < c->porta_target) {
		c->period += by;
		if (c->period > c->porta_target)
			c->period = c->porta_target;
	} else if (c->period > c->porta_target) {
		c->period -= by;
		if (c->period < c->porta_target)
			c->period = c->porta_target;
	}
	c->out_period = c->period;
}

/*
 * Jxy: the note, then x and y semitones above it, a tick each in turn.
 * The channel's own period is left where it was, so only this tick's
 * sounds arpeggiated.
 */
static void
arpeggio(struct replay *r, struct st3_chan *c, int info)
{
	const struct s3m *m = &r->song->s3m;
	int step = r->tick % 3, semitones;

	if (c->sample == 0 || c->period == 0)
		return;
	semitones = step == 0 ? 0 : step == 1 ? info_x(info) : info_y(info);
	c->out_period = note_period_of(note_above(c->note, semitones),
	    m->sample[c->sample].c2spd);
}

/* Ixy: sounding for x + 1 ticks, silent for y + 1, counting on from
 * where the row before left it. */
static void
tremor(struct st3_chan *c, int info)
{
	int ticks = c->tremor_off ? info_y(info) + 1 : info_x(info) + 1;

	if (c->tremor_count >= ticks) {
		c->tremor_off = !c->tremor_off;
		c->tremor_count = 0;
	}
	c->tremor_count++;
	if (c->tremor_off)
		c->out_volume = 0;
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
	int y = info_y(info);

	if (y == 0 || ++c->retrig_count < y)
		return;
	c->retrig_count = 0;
	c->volume = clamp(retrig_volume(c->volume, info_x(info)), 0, 63);
	c->out_volume = c->volume;
	c->trigger_offset = 0;
	v->trigger = true;
}

/*
 * SBx: the loop is the song's rather than a channel's, so one row and
 * one count serve them all.  SB0 marks the row to come back to; SBx
 * comes back x times, and once it has, the row after it begins the
 * next loop.
 */
static void
pattern_loop(struct replay *r, int times)
{
	struct st3_state *st = &r->st3;

	if (times == 0) {
		st->loop_row = r->row;
		return;
	}
	if (st->loop_count == 0)
		st->loop_count = times;
	else
		st->loop_count--;
	if (st->loop_count != 0)
		r->loop_to = st->loop_row;
	else
		st->loop_row = r->row + 1;
}

/*
 * What a row's note, instrument and volume do to a channel, which a
 * note held by Sxy with x of D does later rather than now.
 *
 * An instrument of its own starts nothing: it leaves its own volume and
 * plays from the sample's start.  A note beside a portamento says where
 * to slide to rather than starting there, unless there is nothing
 * playing to slide.
 */
static void
take_note(struct replay *r, struct st3_chan *c, struct replay_voice *v,
    int note, int instrument, int volume)
{
	const struct s3m *m = &r->song->s3m;
	bool porta = c->command == CMD('G') || c->command == CMD('L');
	int period;

	if (instrument != 0 && instrument <= m->nsamples) {
		c->sample = instrument;
		c->volume = clamp(m->sample[instrument].volume, 0, 63);
		c->offset_memory = 0;
	}
	/* Oxx keeps a memory of its own, and the start it names belongs
	 * to a note on its own row, whichever instrument that names. */
	if (c->command == CMD('O') && c->info != 0)
		c->offset_memory = c->info;
	if (volume != S3M_VOLUME_NONE && c->sample != 0)
		c->volume = clamp(volume, 0, 63);

	if (note == S3M_NOTE_CUT) {
		/* What was playing stops, and a note needs an instrument
		 * named with it to sound again. */
		c->cut = true;
		c->volume = 0;
		v->trigger = true;
	} else if (note != S3M_NOTE_NONE && c->sample != 0 &&
	    (!c->cut || instrument != 0)) {
		c->note = note;
		period = note_period_of(note, m->sample[c->sample].c2spd);
		if (porta && !c->cut && c->period != 0) {
			c->porta_target = period;
		} else {
			c->cut = false;
			c->period = period;
			c->porta_target = period;
			c->vib_pos = 0;
			c->trigger_offset =
			    (uint32_t)c->offset_memory * 256;
			v->trigger = true;
		}
	}

	/* A note taken on a later tick, as a held one is, has to be this
	 * tick's sound and not the one before it. */
	c->out_period = c->period;
	c->out_volume = c->volume;
}

/* The effects that act once, on a row's first tick. */
static void
row_effect(struct replay *r, struct st3_chan *c, struct replay_voice *v)
{
	const struct s3m *m = &r->song->s3m;
	/*
	 * The commands that share a memory read it where their own info
	 * is 00; the song's own — its speed, its tempo and where it goes
	 * next — have no memory to read, so 00 is theirs to mean.
	 */
	int info = c->info != 0 ? c->info : c->memory;
	int x = info_x(info), y = info_y(info);

	switch (c->command) {
	case CMD('A'):
		if (c->info != 0)
			r->speed = c->info;
		break;
	case CMD('B'):
		/* A jump names an order by its place in the file. */
		r->jump_order = m->order_of[c->info];
		break;
	case CMD('C'):
		if (decimal_row(c->info) >= 0)
			r->break_row = decimal_row(c->info);
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
	case CMD('G'):
		/* Gxx keeps a speed of its own, apart from the memory the
		 * rest of the commands share; Lxx slides at that speed
		 * while its own info slides the volume. */
		if (c->info != 0)
			c->porta_speed = c->info;
		break;
	case CMD('H'):
		if (x != 0)
			c->vib_speed = x;
		if (y != 0)
			c->vib_depth = y;
		break;
	case CMD('I'):
		tremor(c, info);
		break;
	case CMD('J'):
		arpeggio(r, c, info);
		break;
	case CMD('Q'):
		retrigger(c, v, info);
		break;
	case CMD('S'):
		if (x == 0x8)
			c->place = y;
		else if (x == 0xB)
			pattern_loop(r, y);
		break;
	case CMD('T'):
		/* Scream Tracker passes over a tempo it cannot run at. */
		if (c->info > 0x20)
			r->bpm = c->info;
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
	case CMD('G'):
		tone_porta(c);
		break;
	case CMD('H'):
		vibrato(c);
		break;
	case CMD('I'):
		tremor(c, info);
		break;
	case CMD('J'):
		arpeggio(r, c, info);
		break;
	case CMD('K'):
		/* A vibrato at the speed and depth Hxy left, with the
		 * info sliding the volume; neither moves on the row's
		 * first tick. */
		vibrato(c);
		volume_slide(c, info, false, s3m_fast_slides(m));
		break;
	case CMD('L'):
		tone_porta(c);
		volume_slide(c, info, false, s3m_fast_slides(m));
		break;
	case CMD('Q'):
		retrigger(c, v, info);
		break;
	case CMD('S'):
		/* A note held by Sxy with x of D starts on tick y. */
		if (info_x(info) == 0xD && c->delay != 0 &&
		    r->tick == c->delay) {
			take_note(r, c, v, c->hold_note, c->hold_instrument,
			    c->hold_volume);
			c->delay = 0;
		}
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

	/* A loop belongs to the order its rows are in. */
	if (r->order != r->st3.last_order) {
		r->st3.last_order = r->order;
		r->st3.loop_row = 0;
	}

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

		if (cell->command == CMD('S') && info_x(cell->info) == 0xD) {
			/* The row's note waits, and the instrument and
			 * volume beside it wait with it. */
			c->delay = info_y(cell->info);
			c->hold_note = cell->note;
			c->hold_instrument = cell->instrument;
			c->hold_volume = cell->volume;
		} else
			take_note(r, c, v, cell->note, cell->instrument,
			    cell->volume);

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
		/* The song's own volume and the file's master volume both
		 * scale every voice, 0x40 being the level a song is
		 * written for. */
		v->volume = c->out_volume * r->st3.global_volume *
		    m->master_volume / (64 * 64);
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
	NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
	NULL, NULL, "M channel volume", "N channel volume slide", NULL,
	"P panning slide", NULL, "R tremolo", NULL, NULL, "U fine vibrato",
	"V global volume", "W global volume slide", "X panning",
	"Y panbrello", "Z macro",
};

/* By the high nibble of an Sxy; S8 panning is played. */
static const char *const special_name[16] = {
	"S0 filter", "S1 glissando", "S2 finetune", "S3 vibrato waveform",
	"S4 tremolo waveform", "S5", "S6", "S7", NULL, "S9",
	"SA stereo control", NULL, "SC note cut", NULL,
	"SE pattern delay", "SF funk repeat",
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
					specials |= 1U << info_x(cell->info);
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
