// drkPvr.cpp : Defines the entry point for the DLL application.
//

/*
	Plugin structure
	Interface
	SPG
	TA
	Renderer
*/

#include "drkPvr.h"

#include "ta.h"
#include "spg.h"
#include "pvr_regs.h"
#include "pvr_mem.h"
#include "Renderer_if.h"
#include <algorithm>


void libPvr_LockedBlockWrite (vram_block* block,u32 addr)
{
	rend_text_invl(block);
}


void libPvr_Reset(bool Manual)
{
	Regs_Reset(Manual);
	spg_Reset(Manual);
	//rend_reset(); //*TODO* wtf ?
}

s32 libPvr_Init(void)
{
   ta_ctx_init();
   //failed
	if (!spg_Init())
		return rv_error;
   //failed
	if (!rend_init())
		return rv_error;

	return rv_ok;
}

//called when exiting from sh4 thread , from the new thread context (for any thread specific de init) :P
void libPvr_Term(void)
{
	rend_term();
	spg_Term();
   ta_ctx_free();
}
