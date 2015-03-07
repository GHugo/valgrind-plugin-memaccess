/*--------------------------------------------------------------------*/
/* MemoryAccess: capture all memory accesses and log it     ma_main.c */
/*--------------------------------------------------------------------*/

/*
   This file is part of MemoryAccess, a tool that capture all memory accesses
   and log it

   Copyright (C) 2015 Hugo Guiroux
	  gx.hugo@gmail.com

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
   02111-1307, USA.

   The GNU General Public License is contained in the file COPYING.
*/

#include "pub_tool_basics.h"
#include "pub_tool_tooliface.h"
#include "pub_tool_debuginfo.h"
#include "pub_tool_libcbase.h"
#include "pub_tool_mallocfree.h"
#include "pub_tool_libcprint.h"
#include "pub_tool_hashtable.h"
#include "pub_tool_options.h"
#include "pub_tool_libcassert.h"
#include "pub_tool_libcfile.h"
#include "pub_tool_libcproc.h"
#include "pub_tool_errormgr.h"

VgFile* log_fd;
void VG_(elapsed_wallclock_time) ( /*OUT*/HChar* buf, SizeT bufsize );

/* Command lines options are :
   --log=filename    if present, the file where to store timeline of
                     memory accesses (overwrite if needed)
*/
static Bool ma_process_cmd_line_option(const HChar* arg)
{
	const HChar* log_fsname_unexpanded = NULL;

	if (VG_STR_CLO(arg, "--log", log_fsname_unexpanded)) {
		HChar* logfilename;

		logfilename = VG_(expand_file_name)("--log",
											log_fsname_unexpanded);

		// Simply use buffered files
		log_fd = VG_(fopen)(logfilename,
						 VKI_O_CREAT|VKI_O_WRONLY|VKI_O_TRUNC,
						 VKI_S_IRUSR|VKI_S_IWUSR|VKI_S_IRGRP|VKI_S_IROTH);

		if (log_fd == NULL) {
			VG_(fmsg)("can't create log file '%s'\n",
					  logfilename);
			VG_(exit)(1);
		}
	}

	return True;
}

static void ma_print_usage(void)
{
   VG_(printf)(
"    --log=filename            where to store timeline of memory access \
                               (overwrite if needed)[no timeline]\n"
   );
}

static void ma_print_debug_usage(void)
{
   VG_(printf)(
"    (none)\n"
   );
}
static void ma_post_clo_init(void)
{
}

// Data of about a memory access
typedef struct {
	Addr dat;           // addr accessed
	Addr addr;          // instruction which has made the access
	unsigned int count; // how many times
} entry;

/* For now simple use an arrayn exponentially resized to store the memory
 * accesses. May scale badly, a hashmap would be better.
 */
#define START_LENGTH 256
unsigned int length;
unsigned int cur_size;
entry *count;


static
IRSB* ma_instrument ( VgCallbackClosure* closure,
					  IRSB* bb,
					  const VexGuestLayout* layout,
					  const VexGuestExtents* vge,
					  const VexArchInfo* archinfo_host,
					  IRType gWordTy, IRType hWordTy )
{
	int i = 0;
	IRExpr* addr = NULL;
	UInt size = 0;

	/* gcc warning */
	size = size;

	/* ma_instrument is called per block, each block containing multiple
	 * accesses.
	 */
	for (i = 0; i < bb->stmts_used; i++) {
		IRStmt* st = bb->stmts[i];
		if (!st)
			continue;

		/* Idea: for each kind of VEX instruction type, get the address and the
		  size of the memory access. Then get the adresse of the instruction
		  from the VgCallbackClosure argument and store it inside the global
		  variable count. The way to get adresses is mainly copied from the
		  Lackey example.
		*/
		switch(st->tag) {
		case Ist_IMark:
		{
			addr = mkIRExpr_HWord((HWord)st->Ist.IMark.addr);
			size = st->Ist.IMark.len;
			break;
		}
		case Ist_WrTmp:
		{
			IRExpr* data = st->Ist.WrTmp.data;
			if (data->tag == Iex_Load) {
				addr = data->Iex.Load.addr;
				size = sizeofIRType(data->Iex.Load.ty);
			}
			break;
		}
		case Ist_Store:
		{
			IRExpr* data = st->Ist.Store.data;
			IRType type = typeOfIRExpr(bb->tyenv, data);

			addr = st->Ist.Store.addr;
			size = sizeofIRType(type);
			break;
		}
		case Ist_StoreG:
		{
			IRStoreG* sg = st->Ist.StoreG.details;
			IRExpr* data = sg->data;
			IRType type = typeOfIRExpr(bb->tyenv, data);

			addr = sg->addr;
			size = sizeofIRType(type);
			break;
		}
		case Ist_LoadG:
		{
			IRLoadG* lg = st->Ist.LoadG.details;
			IRType type = Ity_INVALID;
			IRType typeWide = Ity_INVALID;
			typeOfIRLoadGOp(lg->cvt, &typeWide, &type);

			addr = lg->addr;
			size = sizeofIRType(type);
			break;
		}
		case Ist_Dirty:
		{
			IRDirty* d = st->Ist.Dirty.details;
			if (d->mFx != Ifx_None) {
				size = d->mSize;
				if (d->mFx == Ifx_Read || d->mFx == Ifx_Write || d->mFx == Ifx_Modify)
					addr = d->mAddr;
			}
			break;
		}
		case Ist_CAS:
		{
			IRCAS* cas = st->Ist.CAS.details;
			IRType dataTy = typeOfIRExpr(bb->tyenv, cas->dataLo);
			addr = cas->addr;
			size = sizeofIRType(dataTy);

			break;
		}
		case Ist_LLSC:
		{
			if (st->Ist.LLSC.storedata != NULL) {
				IRType dataTy = typeOfIRExpr(bb->tyenv, st->Ist.LLSC.storedata);
				addr = st->Ist.LLSC.addr;
				size = sizeofIRType(dataTy);
			}

			break;
		}
		// Other types of instructions, ignore it
		case Ist_NoOp:
		case Ist_AbiHint:
		case Ist_Put:
		case Ist_PutI:
		case Ist_MBE:
		case Ist_Exit:
			break;
		}

		if (addr != NULL) {

			// Exponentially expand the list if needed
			if (cur_size + 1 > length) {
				length *= 2;
				count = VG_(realloc)("Unable to reallocate count array", count, sizeof(entry) * length);
			}

			// Search if already seen
			int j = 0;
			while (j < cur_size) {
				if (count[j].addr == closure->nraddr)
					break;
				j++;
			}

			// Add new element
			if (j == cur_size) {
				count[j].addr = closure->nraddr;
				count[j].dat = (Addr)addr;
				count[j].count = 0;
				cur_size++;
			}

			count[j].count += 1;

			// Timeline of log accesses with microsends granularity (CSV file
			// format)
			if (log_fd != NULL) {
				HChar buf[50];
				VG_(elapsed_wallclock_time)(buf, sizeof buf);
				VG_(fprintf)(log_fd, "%s,0x%08lx,0x%08lx\n", buf, (Addr)addr, closure->nraddr);
			}
		}
	}

	return bb;
}

// QuickSort helper function to sort memory accesses by count
static Int sort_by_count(const void* el1, const void* el2) {
	const entry* e1 = el1;
	const entry* e2 = el2;

	return e2->count - e1->count;
}

static void ma_fini(Int exitcode)
{
	int i;

	if (log_fd != NULL)
		VG_(fclose)(log_fd);

	// Quicksort sorting by count (descending order)
	VG_(ssort)(count, cur_size, sizeof(entry), sort_by_count);

	// Print each accesses, and get debug & symbol info using describe_IP
	for (i = 0; i < cur_size; i++) {
		entry* en = &count[i];
		VG_(printf)("Access to 0x%08lx for 0x%08lx %d times: %s\n", en->addr, en->dat, en->count, VG_(describe_IP)(en->addr, NULL));
	}
}

// General information
static void ma_pre_clo_init(void)
{
   VG_(details_name)            ("MemoryAccess");
   VG_(details_version)         (NULL);
   VG_(details_description)     ("capture all memory accesses and log it");
   VG_(details_copyright_author)(
	  "Copyright (C) 2015, and GNU GPL'd, by Hugo Guiroux.");
   VG_(details_bug_reports_to)  ("gx.hugo@gmail.com");

   VG_(details_avg_translation_sizeB) ( 275 );

   VG_(basic_tool_funcs)        (ma_post_clo_init,
								 ma_instrument,
								 ma_fini);

   VG_(needs_command_line_options)(ma_process_cmd_line_option,
								   ma_print_usage,
								   ma_print_debug_usage);

   count = VG_(malloc)("Unable to allocate for count,", sizeof(entry) * START_LENGTH);
   length = START_LENGTH;
   cur_size = 0;
   log_fd = NULL;
}

VG_DETERMINE_INTERFACE_VERSION(ma_pre_clo_init)

/*--------------------------------------------------------------------*/
/*--- end                                                          ---*/
/*--------------------------------------------------------------------*/
