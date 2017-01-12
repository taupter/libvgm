/**********************************************************************************************
 *
 *   OKI MSM6258 ADPCM
 *
 *   TODO:
 *   3-bit ADPCM support
 *   Recording?
 *
 **********************************************************************************************/


#ifdef _DEBUG
#include <stdio.h>
#endif
#include <stdlib.h>
#include <stddef.h>	// for NULL
#include <math.h>

#include <stdtype.h>
#include "../snddef.h"
#include "../EmuHelper.h"
#include "../EmuCores.h"
#include "okim6258.h"


static void okim6258_update(void *param, UINT32 samples, DEV_SMPL **outputs);
static UINT8 device_start_okim6258(const OKIM6258_CFG* cfg, DEV_INFO* retDevInf);
static void device_stop_okim6258(void *chip);
static void device_reset_okim6258(void *chip);

static void okim6258_set_divider(void *chip, UINT8 divmode);
static void okim6258_set_clock(void *chip, UINT32 clock);
static UINT32 okim6258_get_vclk(void *chip);

static UINT8 okim6258_status_r(void *chip, UINT8 offset);
static void okim6258_data_w(void *chip, UINT8 data);
static void okim6258_ctrl_w(void *chip, UINT8 data);
static void okim6258_write(void *chip, UINT8 offset, UINT8 data);

static void okim6258_set_options(void *chip, UINT32 Options);
static void okim6258_set_srchg_cb(void *chip, DEVCB_SRATE_CHG CallbackFunc, void* DataPtr);
static void okim6258_set_mute_mask(void *chip, UINT32 MuteMask);


static DEVDEF_RWFUNC devFunc[] =
{
	{RWF_REGISTER | RWF_WRITE, DEVRW_A8D8, 0, okim6258_write},
	{RWF_REGISTER | RWF_READ, DEVRW_A8D8, 0, okim6258_status_r},
	{0x00, 0x00, 0, NULL}
};
static DEV_DEF devDef =
{
	"MSM6258", "MAME", FCC_MAME,
	
	(DEVFUNC_START)device_start_okim6258,
	device_stop_okim6258,
	device_reset_okim6258,
	okim6258_update,
	
	okim6258_set_options,	// SetOptionBits
	okim6258_set_mute_mask,
	NULL,	// SetPanning
	okim6258_set_srchg_cb,	// SetSampleRateChangeCallback
	NULL,	// LinkDevice
	
	devFunc,	// rwFuncs
};
const DEV_DEF* devDefList_OKIM6258[] =
{
	&devDef,
	NULL
};


#define COMMAND_STOP		(1 << 0)
#define COMMAND_PLAY		(1 << 1)
#define	COMMAND_RECORD		(1 << 2)

#define STATUS_PLAYING		(1 << 1)
#define STATUS_RECORDING	(1 << 2)

static const UINT32 dividers[4] = { 1024, 768, 512, 512 };

typedef struct _okim6258_state okim6258_state;
struct _okim6258_state
{
	void* chipInf;

	UINT8  status;

	UINT32 master_clock;	/* master clock frequency */
	UINT32 divider;			/* master clock divider */
	UINT8 adpcm_type;		/* 3/4 bit ADPCM select */
	UINT8 data_in;			/* ADPCM data-in register */
	UINT8 nibble_shift;		/* nibble select */

	UINT8 output_10allow;	// allow 10 bit output
	UINT8 output_bits;
	INT32 output_mask;	// TODO: handle this properly

	// Valley Bell: Added a small queue to prevent race conditions.
	UINT8 data_buf[8];
	UINT8 data_in_last;
	UINT8 data_buf_pos;
	// Data Empty Values:
	//	00 - data written, but not read yet
	//	01 - read data, waiting for next write
	//	02 - tried to read, but had no data
	UINT8 data_empty;
	// Valley Bell: Added panning (for Sharp X68000)
	UINT8 pan;
	INT32 last_smpl;

	INT32 signal;
	INT32 step;
	
	UINT8 clock_buffer[0x04];
	UINT32 initial_clock;
	UINT8 initial_div;
	
	UINT8 Muted;
	
	DEVCB_SRATE_CHG SmpRateFunc;
	void* SmpRateData;
};

/* step size index shift table */
static const int index_shift[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

/* lookup table for the precomputed difference */
static int diff_lookup[49*16];

/* tables computed? */
static int tables_computed = 0;

/**********************************************************************************************

     compute_tables -- compute the difference tables

***********************************************************************************************/

static void compute_tables(void)
{
	/* nibble to bit map */
	static const int nbl2bit[16][4] =
	{
		{ 1, 0, 0, 0}, { 1, 0, 0, 1}, { 1, 0, 1, 0}, { 1, 0, 1, 1},
		{ 1, 1, 0, 0}, { 1, 1, 0, 1}, { 1, 1, 1, 0}, { 1, 1, 1, 1},
		{-1, 0, 0, 0}, {-1, 0, 0, 1}, {-1, 0, 1, 0}, {-1, 0, 1, 1},
		{-1, 1, 0, 0}, {-1, 1, 0, 1}, {-1, 1, 1, 0}, {-1, 1, 1, 1}
	};

	int step, nib;

	if (tables_computed)
		return;
	
	/* loop over all possible steps */
	for (step = 0; step <= 48; step++)
	{
		/* compute the step value */
		int stepval = floor(16.0 * pow(11.0 / 10.0, (double)step));

		/* loop over all nibbles and compute the difference */
		for (nib = 0; nib < 16; nib++)
		{
			diff_lookup[step*16 + nib] = nbl2bit[nib][0] *
				(stepval   * nbl2bit[nib][1] +
				 stepval/2 * nbl2bit[nib][2] +
				 stepval/4 * nbl2bit[nib][3] +
				 stepval/8);
		}
	}

	tables_computed = 1;
}


static INT16 clock_adpcm(okim6258_state *chip, UINT8 nibble)
{
	INT32 max = chip->output_mask - 1;
	INT32 min = -chip->output_mask;

	// original MAME algorithm (causes a DC offset over time)
	//chip->signal += diff_lookup[chip->step * 16 + (nibble & 15)];

	// awesome algorithm ported from XM6 - it works PERFECTLY
	int sample = diff_lookup[chip->step * 16 + (nibble & 15)];
	chip->signal = ((sample << 8) + (chip->signal * 245)) >> 8;

	/* clamp to the maximum */
	if (chip->signal > max)
		chip->signal = max;
	else if (chip->signal < min)
		chip->signal = min;

	/* adjust the step size and clamp */
	chip->step += index_shift[nibble & 7];
	if (chip->step > 48)
		chip->step = 48;
	else if (chip->step < 0)
		chip->step = 0;

	/* return the signal scaled up to 32767 */
	return chip->signal << 4;
}

/**********************************************************************************************

     okim6258_update -- update the sound chip so that it is in sync with CPU execution

***********************************************************************************************/

static void okim6258_update(void *param, UINT32 samples, DEV_SMPL **outputs)
{
	okim6258_state *chip = (okim6258_state *)param;
	DEV_SMPL *bufL = outputs[0];
	DEV_SMPL *bufR = outputs[1];

	if ((chip->status & STATUS_PLAYING) && ! chip->Muted)
	{
		UINT8 nibble_shift = chip->nibble_shift;

		while (samples)
		{
			/* Compute the new amplitude and update the current step */
			//int nibble = (chip->data_in >> nibble_shift) & 0xf;
			int nibble;
			INT16 sample;
			
			if (! nibble_shift)
			{
				// 1st nibble - get data
				if (! chip->data_empty)
				{
					chip->data_in = chip->data_buf[chip->data_buf_pos >> 4];
					chip->data_buf_pos += 0x10;
					chip->data_buf_pos &= 0x7F;
					if ((chip->data_buf_pos >> 4) == (chip->data_buf_pos & 0x0F))
						chip->data_empty ++;
				}
				else
				{
					//chip->data_in = chip->data_in_last;
					if (chip->data_empty < 0x80)
						chip->data_empty ++;
				}
			}
			nibble = (chip->data_in >> nibble_shift) & 0xf;

			/* Output to the buffer */
			//sample = clock_adpcm(chip, nibble);
			if (chip->data_empty < 0x02)
			{
				sample = clock_adpcm(chip, nibble);
				chip->last_smpl = sample;
			}
			else
			{
				// Valley Bell: data_empty behaviour (loosely) ported from XM6
				if (chip->data_empty >= 0x02 + 0x01)
				{
					chip->data_empty -= 0x01;
					chip->signal = chip->signal * 15 / 16;
					chip->last_smpl = chip->signal << 4;
				}
				sample = chip->last_smpl;
			}

			nibble_shift ^= 4;

			*bufL++ = (chip->pan & 0x02) ? 0x00 : sample;
			*bufR++ = (chip->pan & 0x01) ? 0x00 : sample;
			samples--;
		}

		/* Update the parameters */
		chip->nibble_shift = nibble_shift;
	}
	else
	{
		/* Fill with 0 */
		while (samples--)
		{
			*bufL++ = 0;
			*bufR++ = 0;
		}
	}
}



/**********************************************************************************************

     OKIM6258_start -- start emulation of an OKIM6258-compatible chip

***********************************************************************************************/

static UINT32 get_vclk(okim6258_state* info)
{
	UINT32 clk_rnd;
	
	clk_rnd = info->master_clock;
	clk_rnd += info->divider / 2;	 // for better rounding - should help some of the streams
	return clk_rnd / info->divider;
}

static UINT8 device_start_okim6258(const OKIM6258_CFG* cfg, DEV_INFO* retDevInf)
{
	okim6258_state *info;

	info = (okim6258_state *)calloc(1, sizeof(okim6258_state));
	if (info == NULL)
		return 0xFF;

	compute_tables();

	info->initial_clock = cfg->_genCfg.clock;
	info->initial_div = cfg->divider;
	info->adpcm_type = cfg->adpcmBits;
	
	info->master_clock = info->initial_clock;
	info->clock_buffer[0x00] = (info->master_clock & 0x000000FF) >>  0;
	info->clock_buffer[0x01] = (info->master_clock & 0x0000FF00) >>  8;
	info->clock_buffer[0x02] = (info->master_clock & 0x00FF0000) >> 16;
	info->clock_buffer[0x03] = (info->master_clock & 0xFF000000) >> 24;
	info->SmpRateFunc = NULL;

	/* D/A precision is 10-bits but 12-bit data can be output serially to an external DAC */
	info->output_bits = cfg->outputBits ? 12 : 10;
	if (info->output_10allow)
		info->output_mask = (1 << (info->output_bits - 1));
	else
		info->output_mask = (1 << (12 - 1));
	info->divider = dividers[info->initial_div & 0x03];

	info->signal = -2;
	info->step = 0;

	okim6258_set_mute_mask(info, 0x00);

	info->chipInf = info;
	INIT_DEVINF(retDevInf, (DEV_DATA*)info, get_vclk(info), &devDef);
	return 0x00;
}


/**********************************************************************************************

     OKIM6258_stop -- stop emulation of an OKIM6258-compatible chip

***********************************************************************************************/

static void device_stop_okim6258(void *chip)
{
	okim6258_state *info = (okim6258_state *)chip;
	
	free(info);
	return;
}

static void device_reset_okim6258(void *chip)
{
	okim6258_state *info = (okim6258_state *)chip;

	info->master_clock = info->initial_clock;
	info->clock_buffer[0x00] = (info->initial_clock & 0x000000FF) >>  0;
	info->clock_buffer[0x01] = (info->initial_clock & 0x0000FF00) >>  8;
	info->clock_buffer[0x02] = (info->initial_clock & 0x00FF0000) >> 16;
	info->clock_buffer[0x03] = (info->initial_clock & 0xFF000000) >> 24;
	info->divider = dividers[info->initial_div & 0x03];
	if (info->SmpRateFunc != NULL)
		info->SmpRateFunc(info->SmpRateData, get_vclk(info));
	
	info->signal = -2;
	info->step = 0;
	info->status = 0;

	// Valley Bell: Added reset of the Data In register.
	info->data_in = 0x00;
	info->data_buf[0] = info->data_buf[1] = 0x00;
	info->data_buf_pos = 0x00;
	info->data_empty = 0xFF;
	info->pan = 0x00;
}


/**********************************************************************************************

     okim6258_set_divider -- set the master clock divider

***********************************************************************************************/

static void okim6258_set_divider(void *chip, UINT8 divmode)
{
	okim6258_state *info = (okim6258_state *)chip;

	info->divider = dividers[divmode & 0x03];
	if (info->SmpRateFunc != NULL)
		info->SmpRateFunc(info->SmpRateData, get_vclk(info));
}


/**********************************************************************************************

     okim6258_set_clock -- set the master clock

***********************************************************************************************/

static void okim6258_set_clock(void *chip, UINT32 clock)
{
	okim6258_state *info = (okim6258_state *)chip;

	if (clock)
	{
		// set to specific value
		info->master_clock = clock;
	}
	else
	{
		// set to value from buffer
		info->master_clock =	(info->clock_buffer[0x00] <<  0) |
								(info->clock_buffer[0x01] <<  8) |
								(info->clock_buffer[0x02] << 16) |
								(info->clock_buffer[0x03] << 24);
	}
	if (info->SmpRateFunc != NULL)
		info->SmpRateFunc(info->SmpRateData, get_vclk(info));
}


/**********************************************************************************************

     okim6258_get_vclk -- get the VCLK/sampling frequency

***********************************************************************************************/

UINT32 okim6258_get_vclk(void *chip)
{
	okim6258_state *info = (okim6258_state *)chip;

	return get_vclk(info);
}


/**********************************************************************************************

     okim6258_status_r -- read the status port of an OKIM6258-compatible chip

***********************************************************************************************/

static UINT8 okim6258_status_r(void *chip, UINT8 offset)
{
	okim6258_state *info = (okim6258_state *)chip;

	return (info->status & STATUS_PLAYING) ? 0x00 : 0x80;
}


/**********************************************************************************************

     okim6258_data_w -- write to the control port of an OKIM6258-compatible chip

***********************************************************************************************/
static void okim6258_data_w(void *chip, UINT8 data)
{
	okim6258_state *info = (okim6258_state *)chip;

	//info->data_in = data;
	//info->nibble_shift = 0;
	
	if (info->data_empty >= 0x02)
		info->data_buf_pos = 0x00;
	info->data_in_last = data;
	info->data_buf[info->data_buf_pos & 0x0F] = data;
	info->data_buf_pos += 0x01;
	info->data_buf_pos &= 0xF7;
	if ((info->data_buf_pos >> 4) == (info->data_buf_pos & 0x0F))
	{
		logerror("Warning: FIFO full!\n");
		info->data_buf_pos = (info->data_buf_pos & 0xF0) | ((info->data_buf_pos-1) & 0x07);
	}
	info->data_empty = 0x00;
}


/**********************************************************************************************

     okim6258_ctrl_w -- write to the control port of an OKIM6258-compatible chip

***********************************************************************************************/

static void okim6258_ctrl_w(void *chip, UINT8 data)
{
	okim6258_state *info = (okim6258_state *)chip;

	if (data & COMMAND_STOP)
	{
		info->status &= ~(STATUS_PLAYING | STATUS_RECORDING);
		return;
	}

	if (data & COMMAND_PLAY)
	{
		if (!(info->status & STATUS_PLAYING))
		{
			info->status |= STATUS_PLAYING;

			/* Also reset the ADPCM parameters */
			info->signal = -2;	// Note: XM6 lets this fade to 0 when nothing is going on
			info->step = 0;
			info->nibble_shift = 0;
			
			info->data_buf[0x00] = data;
			info->data_buf_pos = 0x01;	// write pos 01, read pos 00
			info->data_empty = 0x00;
		}
		info->step = 0;	// this line was verified with the source of XM6
		info->nibble_shift = 0;
	}
	else
	{
		info->status &= ~STATUS_PLAYING;
	}

	if (data & COMMAND_RECORD)
	{
		logerror("M6258: Record enabled\n");
		info->status |= STATUS_RECORDING;
	}
	else
	{
		info->status &= ~STATUS_RECORDING;
	}
}

static void okim6258_set_clock_byte(void *chip, UINT8 offset, UINT8 val)
{
	okim6258_state *info = (okim6258_state *)chip;
	
	info->clock_buffer[offset] = val;
	
	return;
}

static void okim6258_pan_w(void *chip, UINT8 data)
{
	okim6258_state *info = (okim6258_state *)chip;

	info->pan = data;
	
	return;
}


static void okim6258_write(void *chip, UINT8 offset, UINT8 data)
{
	switch(offset)
	{
	case 0x00:
		okim6258_ctrl_w(chip, data);
		break;
	case 0x01:
		okim6258_data_w(chip, data);
		break;
	case 0x02:
		okim6258_pan_w(chip, data);
		break;
	case 0x08:
	case 0x09:
	case 0x0A:
		okim6258_set_clock_byte(chip, offset & 0x03, data);
		break;
	case 0x0B:
		okim6258_set_clock_byte(chip, offset & 0x03, data);
		okim6258_set_clock(chip, 0);	// refresh clock
		break;
	case 0x0C:
		okim6258_set_divider(chip, data);
		break;
	}
	
	return;
}


static void okim6258_set_options(void *chip, UINT32 Options)
{
	okim6258_state *info = (okim6258_state *)chip;
	
	info->output_10allow = (Options >> 0) & 0x01;
	if (info->output_10allow)
		info->output_mask = (1 << (info->output_bits - 1));
	else
		info->output_mask = (1 << (12 - 1));
	
	return;
}

static void okim6258_set_srchg_cb(void *chip, DEVCB_SRATE_CHG CallbackFunc, void* DataPtr)
{
	okim6258_state *info = (okim6258_state *)chip;
	
	// set Sample Rate Change Callback routine
	info->SmpRateFunc = CallbackFunc;
	info->SmpRateData = DataPtr;
	
	return;
}

static void okim6258_set_mute_mask(void *chip, UINT32 MuteMask)
{
	okim6258_state *info = (okim6258_state *)chip;
	
	info->Muted = MuteMask & 0x01;
	
	return;
}
