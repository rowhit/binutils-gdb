/* Target-dependent code for Mitsubishi D10V, for GDB.

   Copyright 1996, 1997, 1998, 1999, 2000, 2001, 2002, 2003 Free Software
   Foundation, Inc.

   This file is part of GDB.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place - Suite 330,
   Boston, MA 02111-1307, USA.  */

/*  Contributed by Martin Hunt, hunt@cygnus.com */

#include "defs.h"
#include "frame.h"
#include "frame-unwind.h"
#include "frame-base.h"
#include "symtab.h"
#include "gdbtypes.h"
#include "gdbcmd.h"
#include "gdbcore.h"
#include "gdb_string.h"
#include "value.h"
#include "inferior.h"
#include "dis-asm.h"
#include "symfile.h"
#include "objfiles.h"
#include "language.h"
#include "arch-utils.h"
#include "regcache.h"
#include "remote.h"
#include "floatformat.h"
#include "gdb/sim-d10v.h"
#include "sim-regno.h"

#include "gdb_assert.h"

struct gdbarch_tdep
  {
    int a0_regnum;
    int nr_dmap_regs;
    unsigned long (*dmap_register) (int nr);
    unsigned long (*imap_register) (int nr);
  };

/* These are the addresses the D10V-EVA board maps data and
   instruction memory to. */

enum memspace {
  DMEM_START  = 0x2000000,
  IMEM_START  = 0x1000000,
  STACK_START = 0x200bffe
};

/* d10v register names. */

enum
  {
    R0_REGNUM = 0,
    R3_REGNUM = 3,
    D10V_FP_REGNUM = 11,
    LR_REGNUM = 13,
    _SP_REGNUM = 15,
    PSW_REGNUM = 16,
    _PC_REGNUM = 18,
    NR_IMAP_REGS = 2,
    NR_A_REGS = 2,
    TS2_NUM_REGS = 37,
    TS3_NUM_REGS = 42,
    /* d10v calling convention. */
    ARG1_REGNUM = R0_REGNUM,
    ARGN_REGNUM = R3_REGNUM,
    RET1_REGNUM = R0_REGNUM,
  };

#define NR_DMAP_REGS (gdbarch_tdep (current_gdbarch)->nr_dmap_regs)
#define A0_REGNUM (gdbarch_tdep (current_gdbarch)->a0_regnum)

/* Local functions */

extern void _initialize_d10v_tdep (void);

static CORE_ADDR d10v_read_sp (void);

static CORE_ADDR d10v_read_fp (void);

static void d10v_eva_prepare_to_trace (void);

static void d10v_eva_get_trace_data (void);

static CORE_ADDR
d10v_stack_align (CORE_ADDR len)
{
  return (len + 1) & ~1;
}

/* Should we use EXTRACT_STRUCT_VALUE_ADDRESS instead of
   EXTRACT_RETURN_VALUE?  GCC_P is true if compiled with gcc
   and TYPE is the type (which is known to be struct, union or array).

   The d10v returns anything less than 8 bytes in size in
   registers. */

static int
d10v_use_struct_convention (int gcc_p, struct type *type)
{
  long alignment;
  int i;
  /* The d10v only passes a struct in a register when that structure
     has an alignment that matches the size of a register. */
  /* If the structure doesn't fit in 4 registers, put it on the
     stack. */
  if (TYPE_LENGTH (type) > 8)
    return 1;
  /* If the struct contains only one field, don't put it on the stack
     - gcc can fit it in one or more registers. */
  if (TYPE_NFIELDS (type) == 1)
    return 0;
  alignment = TYPE_LENGTH (TYPE_FIELD_TYPE (type, 0));
  for (i = 1; i < TYPE_NFIELDS (type); i++)
    {
      /* If the alignment changes, just assume it goes on the
         stack. */
      if (TYPE_LENGTH (TYPE_FIELD_TYPE (type, i)) != alignment)
	return 1;
    }
  /* If the alignment is suitable for the d10v's 16 bit registers,
     don't put it on the stack. */
  if (alignment == 2 || alignment == 4)
    return 0;
  return 1;
}


static const unsigned char *
d10v_breakpoint_from_pc (CORE_ADDR *pcptr, int *lenptr)
{
  static unsigned char breakpoint[] =
  {0x2f, 0x90, 0x5e, 0x00};
  *lenptr = sizeof (breakpoint);
  return breakpoint;
}

/* Map the REG_NR onto an ascii name.  Return NULL or an empty string
   when the reg_nr isn't valid. */

enum ts2_regnums
  {
    TS2_IMAP0_REGNUM = 32,
    TS2_DMAP_REGNUM = 34,
    TS2_NR_DMAP_REGS = 1,
    TS2_A0_REGNUM = 35
  };

static const char *
d10v_ts2_register_name (int reg_nr)
{
  static char *register_names[] =
  {
    "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7",
    "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
    "psw", "bpsw", "pc", "bpc", "cr4", "cr5", "cr6", "rpt_c",
    "rpt_s", "rpt_e", "mod_s", "mod_e", "cr12", "cr13", "iba", "cr15",
    "imap0", "imap1", "dmap", "a0", "a1"
  };
  if (reg_nr < 0)
    return NULL;
  if (reg_nr >= (sizeof (register_names) / sizeof (*register_names)))
    return NULL;
  return register_names[reg_nr];
}

enum ts3_regnums
  {
    TS3_IMAP0_REGNUM = 36,
    TS3_DMAP0_REGNUM = 38,
    TS3_NR_DMAP_REGS = 4,
    TS3_A0_REGNUM = 32
  };

static const char *
d10v_ts3_register_name (int reg_nr)
{
  static char *register_names[] =
  {
    "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7",
    "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
    "psw", "bpsw", "pc", "bpc", "cr4", "cr5", "cr6", "rpt_c",
    "rpt_s", "rpt_e", "mod_s", "mod_e", "cr12", "cr13", "iba", "cr15",
    "a0", "a1",
    "spi", "spu",
    "imap0", "imap1",
    "dmap0", "dmap1", "dmap2", "dmap3"
  };
  if (reg_nr < 0)
    return NULL;
  if (reg_nr >= (sizeof (register_names) / sizeof (*register_names)))
    return NULL;
  return register_names[reg_nr];
}

/* Access the DMAP/IMAP registers in a target independent way.

   Divide the D10V's 64k data space into four 16k segments:
   0x0000 -- 0x3fff, 0x4000 -- 0x7fff, 0x8000 -- 0xbfff, and 
   0xc000 -- 0xffff.

   On the TS2, the first two segments (0x0000 -- 0x3fff, 0x4000 --
   0x7fff) always map to the on-chip data RAM, and the fourth always
   maps to I/O space.  The third (0x8000 - 0xbfff) can be mapped into
   unified memory or instruction memory, under the control of the
   single DMAP register.

   On the TS3, there are four DMAP registers, each of which controls
   one of the segments.  */

static unsigned long
d10v_ts2_dmap_register (int reg_nr)
{
  switch (reg_nr)
    {
    case 0:
    case 1:
      return 0x2000;
    case 2:
      return read_register (TS2_DMAP_REGNUM);
    default:
      return 0;
    }
}

static unsigned long
d10v_ts3_dmap_register (int reg_nr)
{
  return read_register (TS3_DMAP0_REGNUM + reg_nr);
}

static unsigned long
d10v_dmap_register (int reg_nr)
{
  return gdbarch_tdep (current_gdbarch)->dmap_register (reg_nr);
}

static unsigned long
d10v_ts2_imap_register (int reg_nr)
{
  return read_register (TS2_IMAP0_REGNUM + reg_nr);
}

static unsigned long
d10v_ts3_imap_register (int reg_nr)
{
  return read_register (TS3_IMAP0_REGNUM + reg_nr);
}

static unsigned long
d10v_imap_register (int reg_nr)
{
  return gdbarch_tdep (current_gdbarch)->imap_register (reg_nr);
}

/* MAP GDB's internal register numbering (determined by the layout fo
   the REGISTER_BYTE array) onto the simulator's register
   numbering. */

static int
d10v_ts2_register_sim_regno (int nr)
{
  /* Only makes sense to supply raw registers.  */
  gdb_assert (nr >= 0 && nr < NUM_REGS);
  if (nr >= TS2_IMAP0_REGNUM
      && nr < TS2_IMAP0_REGNUM + NR_IMAP_REGS)
    return nr - TS2_IMAP0_REGNUM + SIM_D10V_IMAP0_REGNUM;
  if (nr == TS2_DMAP_REGNUM)
    return nr - TS2_DMAP_REGNUM + SIM_D10V_TS2_DMAP_REGNUM;
  if (nr >= TS2_A0_REGNUM
      && nr < TS2_A0_REGNUM + NR_A_REGS)
    return nr - TS2_A0_REGNUM + SIM_D10V_A0_REGNUM;
  return nr;
}

static int
d10v_ts3_register_sim_regno (int nr)
{
  /* Only makes sense to supply raw registers.  */
  gdb_assert (nr >= 0 && nr < NUM_REGS);
  if (nr >= TS3_IMAP0_REGNUM
      && nr < TS3_IMAP0_REGNUM + NR_IMAP_REGS)
    return nr - TS3_IMAP0_REGNUM + SIM_D10V_IMAP0_REGNUM;
  if (nr >= TS3_DMAP0_REGNUM
      && nr < TS3_DMAP0_REGNUM + TS3_NR_DMAP_REGS)
    return nr - TS3_DMAP0_REGNUM + SIM_D10V_DMAP0_REGNUM;
  if (nr >= TS3_A0_REGNUM
      && nr < TS3_A0_REGNUM + NR_A_REGS)
    return nr - TS3_A0_REGNUM + SIM_D10V_A0_REGNUM;
  return nr;
}

/* Index within `registers' of the first byte of the space for
   register REG_NR.  */

static int
d10v_register_byte (int reg_nr)
{
  if (reg_nr < A0_REGNUM)
    return (reg_nr * 2);
  else if (reg_nr < (A0_REGNUM + NR_A_REGS))
    return (A0_REGNUM * 2
	    + (reg_nr - A0_REGNUM) * 8);
  else
    return (A0_REGNUM * 2
	    + NR_A_REGS * 8
	    + (reg_nr - A0_REGNUM - NR_A_REGS) * 2);
}

/* Number of bytes of storage in the actual machine representation for
   register REG_NR.  */

static int
d10v_register_raw_size (int reg_nr)
{
  if (reg_nr < A0_REGNUM)
    return 2;
  else if (reg_nr < (A0_REGNUM + NR_A_REGS))
    return 8;
  else
    return 2;
}

/* Return the GDB type object for the "standard" data type
   of data in register N.  */

static struct type *
d10v_register_type (struct gdbarch *gdbarch, int reg_nr)
{
  if (reg_nr == PC_REGNUM)
    return builtin_type_void_func_ptr;
  if (reg_nr == _SP_REGNUM || reg_nr == D10V_FP_REGNUM)
    return builtin_type_void_data_ptr;
  else if (reg_nr >= A0_REGNUM
      && reg_nr < (A0_REGNUM + NR_A_REGS))
    return builtin_type_int64;
  else
    return builtin_type_int16;
}

static int
d10v_daddr_p (CORE_ADDR x)
{
  return (((x) & 0x3000000) == DMEM_START);
}

static int
d10v_iaddr_p (CORE_ADDR x)
{
  return (((x) & 0x3000000) == IMEM_START);
}

static CORE_ADDR
d10v_make_daddr (CORE_ADDR x)
{
  return ((x) | DMEM_START);
}

static CORE_ADDR
d10v_make_iaddr (CORE_ADDR x)
{
  if (d10v_iaddr_p (x))
    return x;	/* Idempotency -- x is already in the IMEM space. */
  else
    return (((x) << 2) | IMEM_START);
}

static CORE_ADDR
d10v_convert_iaddr_to_raw (CORE_ADDR x)
{
  return (((x) >> 2) & 0xffff);
}

static CORE_ADDR
d10v_convert_daddr_to_raw (CORE_ADDR x)
{
  return ((x) & 0xffff);
}

static void
d10v_address_to_pointer (struct type *type, void *buf, CORE_ADDR addr)
{
  /* Is it a code address?  */
  if (TYPE_CODE (TYPE_TARGET_TYPE (type)) == TYPE_CODE_FUNC
      || TYPE_CODE (TYPE_TARGET_TYPE (type)) == TYPE_CODE_METHOD)
    {
      store_unsigned_integer (buf, TYPE_LENGTH (type), 
                              d10v_convert_iaddr_to_raw (addr));
    }
  else
    {
      /* Strip off any upper segment bits.  */
      store_unsigned_integer (buf, TYPE_LENGTH (type), 
                              d10v_convert_daddr_to_raw (addr));
    }
}

static CORE_ADDR
d10v_pointer_to_address (struct type *type, const void *buf)
{
  CORE_ADDR addr = extract_address (buf, TYPE_LENGTH (type));

  /* Is it a code address?  */
  if (TYPE_CODE (TYPE_TARGET_TYPE (type)) == TYPE_CODE_FUNC
      || TYPE_CODE (TYPE_TARGET_TYPE (type)) == TYPE_CODE_METHOD
      || TYPE_CODE_SPACE (TYPE_TARGET_TYPE (type)))
    return d10v_make_iaddr (addr);
  else
    return d10v_make_daddr (addr);
}

/* Don't do anything if we have an integer, this way users can type 'x
   <addr>' w/o having gdb outsmart them.  The internal gdb conversions
   to the correct space are taken care of in the pointer_to_address
   function.  If we don't do this, 'x $fp' wouldn't work.  */
static CORE_ADDR
d10v_integer_to_address (struct type *type, void *buf)
{
  LONGEST val;
  val = unpack_long (type, buf);
  return val;
}

/* Write into appropriate registers a function return value
   of type TYPE, given in virtual format.  

   Things always get returned in RET1_REGNUM, RET2_REGNUM, ... */

static void
d10v_store_return_value (struct type *type, struct regcache *regcache,
			 const void *valbuf)
{
  /* Only char return values need to be shifted right within the first
     regnum.  */
  if (TYPE_LENGTH (type) == 1
      && TYPE_CODE (type) == TYPE_CODE_INT)
    {
      bfd_byte tmp[2];
      tmp[1] = *(bfd_byte *)valbuf;
      regcache_cooked_write (regcache, RET1_REGNUM, tmp);
    }
  else
    {
      int reg;
      /* A structure is never more than 8 bytes long.  See
         use_struct_convention().  */
      gdb_assert (TYPE_LENGTH (type) <= 8);
      /* Write out most registers, stop loop before trying to write
         out any dangling byte at the end of the buffer.  */
      for (reg = 0; (reg * 2) + 1 < TYPE_LENGTH (type); reg++)
	{
	  regcache_cooked_write (regcache, RET1_REGNUM + reg,
				 (bfd_byte *) valbuf + reg * 2);
	}
      /* Write out any dangling byte at the end of the buffer.  */
      if ((reg * 2) + 1 == TYPE_LENGTH (type))
	regcache_cooked_write_part (regcache, reg, 0, 1,
				    (bfd_byte *) valbuf + reg * 2);
    }
}

/* Extract from an array REGBUF containing the (raw) register state
   the address in which a function should return its structure value,
   as a CORE_ADDR (or an expression that can be used as one).  */

static CORE_ADDR
d10v_extract_struct_value_address (struct regcache *regcache)
{
  ULONGEST addr;
  regcache_cooked_read_unsigned (regcache, ARG1_REGNUM, &addr);
  return (addr | DMEM_START);
}

static int
check_prologue (unsigned short op)
{
  /* st  rn, @-sp */
  if ((op & 0x7E1F) == 0x6C1F)
    return 1;

  /* st2w  rn, @-sp */
  if ((op & 0x7E3F) == 0x6E1F)
    return 1;

  /* subi  sp, n */
  if ((op & 0x7FE1) == 0x01E1)
    return 1;

  /* mv  r11, sp */
  if (op == 0x417E)
    return 1;

  /* nop */
  if (op == 0x5E00)
    return 1;

  /* st  rn, @sp */
  if ((op & 0x7E1F) == 0x681E)
    return 1;

  /* st2w  rn, @sp */
  if ((op & 0x7E3F) == 0x3A1E)
    return 1;

  return 0;
}

static CORE_ADDR
d10v_skip_prologue (CORE_ADDR pc)
{
  unsigned long op;
  unsigned short op1, op2;
  CORE_ADDR func_addr, func_end;
  struct symtab_and_line sal;

  /* If we have line debugging information, then the end of the */
  /* prologue should the first assembly instruction of  the first source line */
  if (find_pc_partial_function (pc, NULL, &func_addr, &func_end))
    {
      sal = find_pc_line (func_addr, 0);
      if (sal.end && sal.end < func_end)
	return sal.end;
    }

  if (target_read_memory (pc, (char *) &op, 4))
    return pc;			/* Can't access it -- assume no prologue. */

  while (1)
    {
      op = (unsigned long) read_memory_integer (pc, 4);
      if ((op & 0xC0000000) == 0xC0000000)
	{
	  /* long instruction */
	  if (((op & 0x3FFF0000) != 0x01FF0000) &&	/* add3 sp,sp,n */
	      ((op & 0x3F0F0000) != 0x340F0000) &&	/* st  rn, @(offset,sp) */
	      ((op & 0x3F1F0000) != 0x350F0000))	/* st2w  rn, @(offset,sp) */
	    break;
	}
      else
	{
	  /* short instructions */
	  if ((op & 0xC0000000) == 0x80000000)
	    {
	      op2 = (op & 0x3FFF8000) >> 15;
	      op1 = op & 0x7FFF;
	    }
	  else
	    {
	      op1 = (op & 0x3FFF8000) >> 15;
	      op2 = op & 0x7FFF;
	    }
	  if (check_prologue (op1))
	    {
	      if (!check_prologue (op2))
		{
		  /* if the previous opcode was really part of the prologue */
		  /* and not just a NOP, then we want to break after both instructions */
		  if (op1 != 0x5E00)
		    pc += 4;
		  break;
		}
	    }
	  else
	    break;
	}
      pc += 4;
    }
  return pc;
}

struct d10v_unwind_cache
{
  CORE_ADDR return_pc;
  /* The previous frame's inner most stack address.  Used as this
     frame ID's stack_addr.  */
  CORE_ADDR prev_sp;
  /* The frame's base, optionally used by the high-level debug info.  */
  CORE_ADDR base;
  int size;
  CORE_ADDR *saved_regs;
  /* How far the SP and r11 (FP) have been offset from the start of
     the stack frame (as defined by the previous frame's stack
     pointer).  */
  LONGEST sp_offset;
  LONGEST r11_offset;
  int uses_frame;
  void **regs;
};

static int
prologue_find_regs (struct d10v_unwind_cache *info, unsigned short op,
		    CORE_ADDR addr)
{
  int n;

  /* st  rn, @-sp */
  if ((op & 0x7E1F) == 0x6C1F)
    {
      n = (op & 0x1E0) >> 5;
      info->sp_offset -= 2;
      info->saved_regs[n] = info->sp_offset;
      return 1;
    }

  /* st2w  rn, @-sp */
  else if ((op & 0x7E3F) == 0x6E1F)
    {
      n = (op & 0x1E0) >> 5;
      info->sp_offset -= 4;
      info->saved_regs[n] = info->sp_offset;
      info->saved_regs[n + 1] = info->sp_offset + 2;
      return 1;
    }

  /* subi  sp, n */
  if ((op & 0x7FE1) == 0x01E1)
    {
      n = (op & 0x1E) >> 1;
      if (n == 0)
	n = 16;
      info->sp_offset -= n;
      return 1;
    }

  /* mv  r11, sp */
  if (op == 0x417E)
    {
      info->uses_frame = 1;
      info->r11_offset = info->sp_offset;
      return 1;
    }

  /* st  rn, @r11 */
  if ((op & 0x7E1F) == 0x6816)
    {
      n = (op & 0x1E0) >> 5;
      info->saved_regs[n] = info->r11_offset;
      return 1;
    }

  /* nop */
  if (op == 0x5E00)
    return 1;

  /* st  rn, @sp */
  if ((op & 0x7E1F) == 0x681E)
    {
      n = (op & 0x1E0) >> 5;
      info->saved_regs[n] = info->sp_offset;
      return 1;
    }

  /* st2w  rn, @sp */
  if ((op & 0x7E3F) == 0x3A1E)
    {
      n = (op & 0x1E0) >> 5;
      info->saved_regs[n] = info->sp_offset;
      info->saved_regs[n + 1] = info->sp_offset + 2;
      return 1;
    }

  return 0;
}

/* Put here the code to store, into fi->saved_regs, the addresses of
   the saved registers of frame described by FRAME_INFO.  This
   includes special registers such as pc and fp saved in special ways
   in the stack frame.  sp is even more special: the address we return
   for it IS the sp for the next frame. */

struct d10v_unwind_cache *
d10v_frame_unwind_cache (struct frame_info *next_frame,
			 void **this_prologue_cache)
{
  CORE_ADDR pc;
  ULONGEST prev_sp;
  ULONGEST this_base;
  unsigned long op;
  unsigned short op1, op2;
  int i;
  struct d10v_unwind_cache *info;

  if ((*this_prologue_cache))
    return (*this_prologue_cache);

  info = FRAME_OBSTACK_ZALLOC (struct d10v_unwind_cache);
  (*this_prologue_cache) = info;
  info->saved_regs = frame_obstack_zalloc (SIZEOF_FRAME_SAVED_REGS);

  info->size = 0;
  info->return_pc = 0;
  info->sp_offset = 0;

  info->uses_frame = 0;
  for (pc = frame_func_unwind (next_frame);
       pc > 0 && pc < frame_pc_unwind (next_frame);
       pc += 4)
    {
      op = (unsigned long) read_memory_integer (pc, 4);
      if ((op & 0xC0000000) == 0xC0000000)
	{
	  /* long instruction */
	  if ((op & 0x3FFF0000) == 0x01FF0000)
	    {
	      /* add3 sp,sp,n */
	      short n = op & 0xFFFF;
	      info->sp_offset += n;
	    }
	  else if ((op & 0x3F0F0000) == 0x340F0000)
	    {
	      /* st  rn, @(offset,sp) */
	      short offset = op & 0xFFFF;
	      short n = (op >> 20) & 0xF;
	      info->saved_regs[n] = info->sp_offset + offset;
	    }
	  else if ((op & 0x3F1F0000) == 0x350F0000)
	    {
	      /* st2w  rn, @(offset,sp) */
	      short offset = op & 0xFFFF;
	      short n = (op >> 20) & 0xF;
	      info->saved_regs[n] = info->sp_offset + offset;
	      info->saved_regs[n + 1] = info->sp_offset + offset + 2;
	    }
	  else
	    break;
	}
      else
	{
	  /* short instructions */
	  if ((op & 0xC0000000) == 0x80000000)
	    {
	      op2 = (op & 0x3FFF8000) >> 15;
	      op1 = op & 0x7FFF;
	    }
	  else
	    {
	      op1 = (op & 0x3FFF8000) >> 15;
	      op2 = op & 0x7FFF;
	    }
	  if (!prologue_find_regs (info, op1, pc) 
	      || !prologue_find_regs (info, op2, pc))
	    break;
	}
    }

  info->size = -info->sp_offset;

  /* Compute the frame's base, and the previous frame's SP.  */
  if (info->uses_frame)
    {
      /* The SP was moved to the FP.  This indicates that a new frame
         was created.  Get THIS frame's FP value by unwinding it from
         the next frame.  */
      frame_unwind_unsigned_register (next_frame, D10V_FP_REGNUM, &this_base);
      /* The FP points at the last saved register.  Adjust the FP back
         to before the first saved register giving the SP.  */
      prev_sp = this_base + info->size;
    }
  else if (info->saved_regs[SP_REGNUM])
    {
      /* The SP was saved (which is very unusual), the frame base is
	 just the PREV's frame's TOP-OF-STACK.  */
      this_base = read_memory_unsigned_integer (info->saved_regs[SP_REGNUM], 
						register_size (current_gdbarch,
							       SP_REGNUM));
      prev_sp = this_base;
    }
  else
    {
      /* Assume that the FP is this frame's SP but with that pushed
         stack space added back.  */
      frame_unwind_unsigned_register (next_frame, SP_REGNUM, &this_base);
      prev_sp = this_base + info->size;
    }

  info->base = d10v_make_daddr (this_base);
  info->prev_sp = d10v_make_daddr (prev_sp);

  /* Adjust all the saved registers so that they contain addresses and
     not offsets.  */
  for (i = 0; i < NUM_REGS - 1; i++)
    if (info->saved_regs[i])
      {
	info->saved_regs[i] = (info->prev_sp + info->saved_regs[i]);
      }

  if (info->saved_regs[LR_REGNUM])
    {
      CORE_ADDR return_pc 
	= read_memory_unsigned_integer (info->saved_regs[LR_REGNUM], 
					register_size (current_gdbarch, LR_REGNUM));
      info->return_pc = d10v_make_iaddr (return_pc);
    }
  else
    {
      ULONGEST return_pc;
      frame_unwind_unsigned_register (next_frame, LR_REGNUM, &return_pc);
      info->return_pc = d10v_make_iaddr (return_pc);
    }

  /* The SP_REGNUM is special.  Instead of the address of the SP, the
     previous frame's SP value is saved.  */
  info->saved_regs[SP_REGNUM] = info->prev_sp;

  return info;
}

static void
d10v_print_registers_info (struct gdbarch *gdbarch, struct ui_file *file,
			   struct frame_info *frame, int regnum, int all)
{
  if (regnum >= 0)
    {
      default_print_registers_info (gdbarch, file, frame, regnum, all);
      return;
    }

  {
    ULONGEST pc, psw, rpt_s, rpt_e, rpt_c;
    frame_read_unsigned_register (frame, PC_REGNUM, &pc);
    frame_read_unsigned_register (frame, PSW_REGNUM, &psw);
    frame_read_unsigned_register (frame, frame_map_name_to_regnum ("rpt_s", -1), &rpt_s);
    frame_read_unsigned_register (frame, frame_map_name_to_regnum ("rpt_e", -1), &rpt_e);
    frame_read_unsigned_register (frame, frame_map_name_to_regnum ("rpt_c", -1), &rpt_c);
    fprintf_filtered (file, "PC=%04lx (0x%lx) PSW=%04lx RPT_S=%04lx RPT_E=%04lx RPT_C=%04lx\n",
		     (long) pc, (long) d10v_make_iaddr (pc), (long) psw,
		     (long) rpt_s, (long) rpt_e, (long) rpt_c);
  }

  {
    int group;
    for (group = 0; group < 16; group += 8)
      {
	int r;
	fprintf_filtered (file, "R%d-R%-2d", group, group + 7);
	for (r = group; r < group + 8; r++)
	  {
	    ULONGEST tmp;
	    frame_read_unsigned_register (frame, r, &tmp);
	    fprintf_filtered (file, " %04lx", (long) tmp);
	  }
	fprintf_filtered (file, "\n");
      }
  }

  /* Note: The IMAP/DMAP registers don't participate in function
     calls.  Don't bother trying to unwind them.  */

  {
    int a;
    for (a = 0; a < NR_IMAP_REGS; a++)
      {
	if (a > 0)
	  fprintf_filtered (file, "    ");
	fprintf_filtered (file, "IMAP%d %04lx", a, d10v_imap_register (a));
      }
    if (NR_DMAP_REGS == 1)
      /* Registers DMAP0 and DMAP1 are constant.  Just return dmap2.  */
      fprintf_filtered (file, "    DMAP %04lx\n", d10v_dmap_register (2));
    else
      {
	for (a = 0; a < NR_DMAP_REGS; a++)
	  {
	    fprintf_filtered (file, "    DMAP%d %04lx", a, d10v_dmap_register (a));
	  }
	fprintf_filtered (file, "\n");
      }
  }

  {
    char *num = alloca (max_register_size (gdbarch));
    int a;
    fprintf_filtered (file, "A0-A%d", NR_A_REGS - 1);
    for (a = A0_REGNUM; a < A0_REGNUM + NR_A_REGS; a++)
      {
	int i;
	fprintf_filtered (file, "  ");
	frame_register_read (frame, a, num);
	for (i = 0; i < max_register_size (current_gdbarch); i++)
	  {
	    fprintf_filtered (file, "%02x", (num[i] & 0xff));
	  }
      }
  }
  fprintf_filtered (file, "\n");
}

static void
show_regs (char *args, int from_tty)
{
  d10v_print_registers_info (current_gdbarch, gdb_stdout,
			     get_current_frame (), -1, 1);
}

static CORE_ADDR
d10v_read_pc (ptid_t ptid)
{
  ptid_t save_ptid;
  CORE_ADDR pc;
  CORE_ADDR retval;

  save_ptid = inferior_ptid;
  inferior_ptid = ptid;
  pc = (int) read_register (PC_REGNUM);
  inferior_ptid = save_ptid;
  retval = d10v_make_iaddr (pc);
  return retval;
}

static void
d10v_write_pc (CORE_ADDR val, ptid_t ptid)
{
  ptid_t save_ptid;

  save_ptid = inferior_ptid;
  inferior_ptid = ptid;
  write_register (PC_REGNUM, d10v_convert_iaddr_to_raw (val));
  inferior_ptid = save_ptid;
}

static CORE_ADDR
d10v_read_sp (void)
{
  return (d10v_make_daddr (read_register (SP_REGNUM)));
}

static CORE_ADDR
d10v_read_fp (void)
{
  return (d10v_make_daddr (read_register (D10V_FP_REGNUM)));
}

/* When arguments must be pushed onto the stack, they go on in reverse
   order.  The below implements a FILO (stack) to do this. */

struct stack_item
{
  int len;
  struct stack_item *prev;
  void *data;
};

static struct stack_item *push_stack_item (struct stack_item *prev,
					   void *contents, int len);
static struct stack_item *
push_stack_item (struct stack_item *prev, void *contents, int len)
{
  struct stack_item *si;
  si = xmalloc (sizeof (struct stack_item));
  si->data = xmalloc (len);
  si->len = len;
  si->prev = prev;
  memcpy (si->data, contents, len);
  return si;
}

static struct stack_item *pop_stack_item (struct stack_item *si);
static struct stack_item *
pop_stack_item (struct stack_item *si)
{
  struct stack_item *dead = si;
  si = si->prev;
  xfree (dead->data);
  xfree (dead);
  return si;
}


static CORE_ADDR
d10v_push_dummy_call (struct gdbarch *gdbarch, struct regcache *regcache,
		      CORE_ADDR dummy_addr, int nargs, struct value **args,
		      CORE_ADDR sp, int struct_return, CORE_ADDR struct_addr)
{
  int i;
  int regnum = ARG1_REGNUM;
  struct stack_item *si = NULL;
  long val;

  /* Set the return address.  For the d10v, the return breakpoint is
     always at DUMMY_ADDR.  */
  regcache_cooked_write_unsigned (regcache, LR_REGNUM,
				  d10v_convert_iaddr_to_raw (dummy_addr));

  /* If STRUCT_RETURN is true, then the struct return address (in
     STRUCT_ADDR) will consume the first argument-passing register.
     Both adjust the register count and store that value.  */
  if (struct_return)
    {
      regcache_cooked_write_unsigned (regcache, regnum, struct_addr);
      regnum++;
    }

  /* Fill in registers and arg lists */
  for (i = 0; i < nargs; i++)
    {
      struct value *arg = args[i];
      struct type *type = check_typedef (VALUE_TYPE (arg));
      char *contents = VALUE_CONTENTS (arg);
      int len = TYPE_LENGTH (type);
      int aligned_regnum = (regnum + 1) & ~1;

      /* printf ("push: type=%d len=%d\n", TYPE_CODE (type), len); */
      if (len <= 2 && regnum <= ARGN_REGNUM)
	/* fits in a single register, do not align */
	{
	  val = extract_unsigned_integer (contents, len);
	  regcache_cooked_write_unsigned (regcache, regnum++, val);
	}
      else if (len <= (ARGN_REGNUM - aligned_regnum + 1) * 2)
	/* value fits in remaining registers, store keeping left
	   aligned */
	{
	  int b;
	  regnum = aligned_regnum;
	  for (b = 0; b < (len & ~1); b += 2)
	    {
	      val = extract_unsigned_integer (&contents[b], 2);
	      regcache_cooked_write_unsigned (regcache, regnum++, val);
	    }
	  if (b < len)
	    {
	      val = extract_unsigned_integer (&contents[b], 1);
	      regcache_cooked_write_unsigned (regcache, regnum++, (val << 8));
	    }
	}
      else
	{
	  /* arg will go onto stack */
	  regnum = ARGN_REGNUM + 1;
	  si = push_stack_item (si, contents, len);
	}
    }

  while (si)
    {
      sp = (sp - si->len) & ~1;
      write_memory (sp, si->data, si->len);
      si = pop_stack_item (si);
    }

  /* Finally, update the SP register.  */
  regcache_cooked_write_unsigned (regcache, SP_REGNUM,
				  d10v_convert_daddr_to_raw (sp));

  return sp;
}


/* Given a return value in `regbuf' with a type `valtype', 
   extract and copy its value into `valbuf'.  */

static void
d10v_extract_return_value (struct type *type, struct regcache *regcache,
			   void *valbuf)
{
  int len;
#if 0
  printf("RET: TYPE=%d len=%d r%d=0x%x\n", TYPE_CODE (type), 
	 TYPE_LENGTH (type), RET1_REGNUM - R0_REGNUM, 
	 (int) extract_unsigned_integer (regbuf + REGISTER_BYTE(RET1_REGNUM), 
					 register_size (current_gdbarch, RET1_REGNUM)));
#endif
  if (TYPE_LENGTH (type) == 1)
    {
      ULONGEST c;
      regcache_cooked_read_unsigned (regcache, RET1_REGNUM, &c);
      store_unsigned_integer (valbuf, 1, c);
    }
  else
    {
      /* For return values of odd size, the first byte is in the
	 least significant part of the first register.  The
	 remaining bytes in remaining registers. Interestingly, when
	 such values are passed in, the last byte is in the most
	 significant byte of that same register - wierd. */
      int reg = RET1_REGNUM;
      int off = 0;
      if (TYPE_LENGTH (type) & 1)
	{
	  regcache_cooked_read_part (regcache, RET1_REGNUM, 1, 1,
				     (bfd_byte *)valbuf + off);
	  off++;
	  reg++;
	}
      /* Transfer the remaining registers.  */
      for (; off < TYPE_LENGTH (type); reg++, off += 2)
	{
	  regcache_cooked_read (regcache, RET1_REGNUM + reg,
				(bfd_byte *) valbuf + off);
	}
    }
}

/* Translate a GDB virtual ADDR/LEN into a format the remote target
   understands.  Returns number of bytes that can be transfered
   starting at TARG_ADDR.  Return ZERO if no bytes can be transfered
   (segmentation fault).  Since the simulator knows all about how the
   VM system works, we just call that to do the translation. */

static void
remote_d10v_translate_xfer_address (CORE_ADDR memaddr, int nr_bytes,
				    CORE_ADDR *targ_addr, int *targ_len)
{
  long out_addr;
  long out_len;
  out_len = sim_d10v_translate_addr (memaddr, nr_bytes,
				     &out_addr,
				     d10v_dmap_register,
				     d10v_imap_register);
  *targ_addr = out_addr;
  *targ_len = out_len;
}


/* The following code implements access to, and display of, the D10V's
   instruction trace buffer.  The buffer consists of 64K or more
   4-byte words of data, of which each words includes an 8-bit count,
   an 8-bit segment number, and a 16-bit instruction address.

   In theory, the trace buffer is continuously capturing instruction
   data that the CPU presents on its "debug bus", but in practice, the
   ROMified GDB stub only enables tracing when it continues or steps
   the program, and stops tracing when the program stops; so it
   actually works for GDB to read the buffer counter out of memory and
   then read each trace word.  The counter records where the tracing
   stops, but there is no record of where it started, so we remember
   the PC when we resumed and then search backwards in the trace
   buffer for a word that includes that address.  This is not perfect,
   because you will miss trace data if the resumption PC is the target
   of a branch.  (The value of the buffer counter is semi-random, any
   trace data from a previous program stop is gone.)  */

/* The address of the last word recorded in the trace buffer.  */

#define DBBC_ADDR (0xd80000)

/* The base of the trace buffer, at least for the "Board_0".  */

#define TRACE_BUFFER_BASE (0xf40000)

static void trace_command (char *, int);

static void untrace_command (char *, int);

static void trace_info (char *, int);

static void tdisassemble_command (char *, int);

static void display_trace (int, int);

/* True when instruction traces are being collected.  */

static int tracing;

/* Remembered PC.  */

static CORE_ADDR last_pc;

/* True when trace output should be displayed whenever program stops.  */

static int trace_display;

/* True when trace listing should include source lines.  */

static int default_trace_show_source = 1;

struct trace_buffer
  {
    int size;
    short *counts;
    CORE_ADDR *addrs;
  }
trace_data;

static void
trace_command (char *args, int from_tty)
{
  /* Clear the host-side trace buffer, allocating space if needed.  */
  trace_data.size = 0;
  if (trace_data.counts == NULL)
    trace_data.counts = (short *) xmalloc (65536 * sizeof (short));
  if (trace_data.addrs == NULL)
    trace_data.addrs = (CORE_ADDR *) xmalloc (65536 * sizeof (CORE_ADDR));

  tracing = 1;

  printf_filtered ("Tracing is now on.\n");
}

static void
untrace_command (char *args, int from_tty)
{
  tracing = 0;

  printf_filtered ("Tracing is now off.\n");
}

static void
trace_info (char *args, int from_tty)
{
  int i;

  if (trace_data.size)
    {
      printf_filtered ("%d entries in trace buffer:\n", trace_data.size);

      for (i = 0; i < trace_data.size; ++i)
	{
	  printf_filtered ("%d: %d instruction%s at 0x%s\n",
			   i,
			   trace_data.counts[i],
			   (trace_data.counts[i] == 1 ? "" : "s"),
			   paddr_nz (trace_data.addrs[i]));
	}
    }
  else
    printf_filtered ("No entries in trace buffer.\n");

  printf_filtered ("Tracing is currently %s.\n", (tracing ? "on" : "off"));
}

/* Print the instruction at address MEMADDR in debugged memory,
   on STREAM.  Returns length of the instruction, in bytes.  */

static int
print_insn (CORE_ADDR memaddr, struct ui_file *stream)
{
  /* If there's no disassembler, something is very wrong.  */
  if (tm_print_insn == NULL)
    internal_error (__FILE__, __LINE__,
		    "print_insn: no disassembler");

  if (TARGET_BYTE_ORDER == BFD_ENDIAN_BIG)
    tm_print_insn_info.endian = BFD_ENDIAN_BIG;
  else
    tm_print_insn_info.endian = BFD_ENDIAN_LITTLE;
  return TARGET_PRINT_INSN (memaddr, &tm_print_insn_info);
}

static void
d10v_eva_prepare_to_trace (void)
{
  if (!tracing)
    return;

  last_pc = read_register (PC_REGNUM);
}

/* Collect trace data from the target board and format it into a form
   more useful for display.  */

static void
d10v_eva_get_trace_data (void)
{
  int count, i, j, oldsize;
  int trace_addr, trace_seg, trace_cnt, next_cnt;
  unsigned int last_trace, trace_word, next_word;
  unsigned int *tmpspace;

  if (!tracing)
    return;

  tmpspace = xmalloc (65536 * sizeof (unsigned int));

  last_trace = read_memory_unsigned_integer (DBBC_ADDR, 2) << 2;

  /* Collect buffer contents from the target, stopping when we reach
     the word recorded when execution resumed.  */

  count = 0;
  while (last_trace > 0)
    {
      QUIT;
      trace_word =
	read_memory_unsigned_integer (TRACE_BUFFER_BASE + last_trace, 4);
      trace_addr = trace_word & 0xffff;
      last_trace -= 4;
      /* Ignore an apparently nonsensical entry.  */
      if (trace_addr == 0xffd5)
	continue;
      tmpspace[count++] = trace_word;
      if (trace_addr == last_pc)
	break;
      if (count > 65535)
	break;
    }

  /* Move the data to the host-side trace buffer, adjusting counts to
     include the last instruction executed and transforming the address
     into something that GDB likes.  */

  for (i = 0; i < count; ++i)
    {
      trace_word = tmpspace[i];
      next_word = ((i == 0) ? 0 : tmpspace[i - 1]);
      trace_addr = trace_word & 0xffff;
      next_cnt = (next_word >> 24) & 0xff;
      j = trace_data.size + count - i - 1;
      trace_data.addrs[j] = (trace_addr << 2) + 0x1000000;
      trace_data.counts[j] = next_cnt + 1;
    }

  oldsize = trace_data.size;
  trace_data.size += count;

  xfree (tmpspace);

  if (trace_display)
    display_trace (oldsize, trace_data.size);
}

static void
tdisassemble_command (char *arg, int from_tty)
{
  int i, count;
  CORE_ADDR low, high;

  if (!arg)
    {
      low = 0;
      high = trace_data.size;
    }
  else
    { 
      char *space_index = strchr (arg, ' ');
      if (space_index == NULL)
	{
	  low = parse_and_eval_address (arg);
	  high = low + 5;
	}
      else
	{
	  /* Two arguments.  */
	  *space_index = '\0';
	  low = parse_and_eval_address (arg);
	  high = parse_and_eval_address (space_index + 1);
	  if (high < low)
	    high = low;
	}
    }

  printf_filtered ("Dump of trace from %s to %s:\n", paddr_u (low), paddr_u (high));

  display_trace (low, high);

  printf_filtered ("End of trace dump.\n");
  gdb_flush (gdb_stdout);
}

static void
display_trace (int low, int high)
{
  int i, count, trace_show_source, first, suppress;
  CORE_ADDR next_address;

  trace_show_source = default_trace_show_source;
  if (!have_full_symbols () && !have_partial_symbols ())
    {
      trace_show_source = 0;
      printf_filtered ("No symbol table is loaded.  Use the \"file\" command.\n");
      printf_filtered ("Trace will not display any source.\n");
    }

  first = 1;
  suppress = 0;
  for (i = low; i < high; ++i)
    {
      next_address = trace_data.addrs[i];
      count = trace_data.counts[i];
      while (count-- > 0)
	{
	  QUIT;
	  if (trace_show_source)
	    {
	      struct symtab_and_line sal, sal_prev;

	      sal_prev = find_pc_line (next_address - 4, 0);
	      sal = find_pc_line (next_address, 0);

	      if (sal.symtab)
		{
		  if (first || sal.line != sal_prev.line)
		    print_source_lines (sal.symtab, sal.line, sal.line + 1, 0);
		  suppress = 0;
		}
	      else
		{
		  if (!suppress)
		    /* FIXME-32x64--assumes sal.pc fits in long.  */
		    printf_filtered ("No source file for address %s.\n",
				 local_hex_string ((unsigned long) sal.pc));
		  suppress = 1;
		}
	    }
	  first = 0;
	  print_address (next_address, gdb_stdout);
	  printf_filtered (":");
	  printf_filtered ("\t");
	  wrap_here ("    ");
	  next_address = next_address + print_insn (next_address, gdb_stdout);
	  printf_filtered ("\n");
	  gdb_flush (gdb_stdout);
	}
    }
}

static CORE_ADDR
d10v_unwind_pc (struct gdbarch *gdbarch, struct frame_info *next_frame)
{
  ULONGEST pc;
  frame_unwind_unsigned_register (next_frame, PC_REGNUM, &pc);
  return d10v_make_iaddr (pc);
}

/* Given a GDB frame, determine the address of the calling function's
   frame.  This will be used to create a new GDB frame struct.  */

static void
d10v_frame_this_id (struct frame_info *next_frame,
		    void **this_prologue_cache,
		    struct frame_id *this_id)
{
  struct d10v_unwind_cache *info
    = d10v_frame_unwind_cache (next_frame, this_prologue_cache);
  CORE_ADDR base;
  CORE_ADDR pc;

  /* The PC/FUNC is easy.  */
  pc = frame_func_unwind (next_frame);

  /* This is meant to halt the backtrace at "_start".  Make sure we
     don't halt it at a generic dummy frame. */
  if (pc == IMEM_START || pc <= IMEM_START || inside_entry_file (pc))
    return;

  /* Hopefully the prologue analysis either correctly determined the
     frame's base (which is the SP from the previous frame), or set
     that base to "NULL".  */
  base = info->prev_sp;
  if (base == STACK_START || base == 0)
    return;

  /* Check that we're not going round in circles with the same frame
     ID (but avoid applying the test to sentinel frames which do go
     round in circles).  Can't use frame_id_eq() as that doesn't yet
     compare the frame's PC value.  */
  if (frame_relative_level (next_frame) >= 0
      && get_frame_type (next_frame) != DUMMY_FRAME
      && frame_id_eq (get_frame_id (next_frame),
		      frame_id_build (base, pc)))
    return;

  (*this_id) = frame_id_build (base, pc);
}

static void
saved_regs_unwinder (struct frame_info *next_frame,
		     CORE_ADDR *this_saved_regs,
		     int regnum, int *optimizedp,
		     enum lval_type *lvalp, CORE_ADDR *addrp,
		     int *realnump, void *bufferp)
{
  if (this_saved_regs[regnum] != 0)
    {
      if (regnum == SP_REGNUM)
	{
	  /* SP register treated specially.  */
	  *optimizedp = 0;
	  *lvalp = not_lval;
	  *addrp = 0;
	  *realnump = -1;
	  if (bufferp != NULL)
	    store_address (bufferp, register_size (current_gdbarch, regnum),
			   this_saved_regs[regnum]);
	}
      else
	{
	  /* Any other register is saved in memory, fetch it but cache
	     a local copy of its value.  */
	  *optimizedp = 0;
	  *lvalp = lval_memory;
	  *addrp = this_saved_regs[regnum];
	  *realnump = -1;
	  if (bufferp != NULL)
	    {
	      /* Read the value in from memory.  */
	      read_memory (this_saved_regs[regnum], bufferp,
			   register_size (current_gdbarch, regnum));
	    }
	}
      return;
    }

  /* No luck, assume this and the next frame have the same register
     value.  If a value is needed, pass the request on down the chain;
     otherwise just return an indication that the value is in the same
     register as the next frame.  */
  frame_register_unwind (next_frame, regnum, optimizedp, lvalp, addrp,
			 realnump, bufferp);
}


static void
d10v_frame_prev_register (struct frame_info *next_frame,
			  void **this_prologue_cache,
			  int regnum, int *optimizedp,
			  enum lval_type *lvalp, CORE_ADDR *addrp,
			  int *realnump, void *bufferp)
{
  struct d10v_unwind_cache *info
    = d10v_frame_unwind_cache (next_frame, this_prologue_cache);
  if (regnum == PC_REGNUM)
    {
      /* The call instruction saves the caller's PC in LR.  The
	 function prologue of the callee may then save the LR on the
	 stack.  Find that possibly saved LR value and return it.  */
      saved_regs_unwinder (next_frame, info->saved_regs, LR_REGNUM, optimizedp,
			   lvalp, addrp, realnump, bufferp);
    }
  else
    {
      saved_regs_unwinder (next_frame, info->saved_regs, regnum, optimizedp,
			   lvalp, addrp, realnump, bufferp);
    }
}

static const struct frame_unwind d10v_frame_unwind = {
  NORMAL_FRAME,
  d10v_frame_this_id,
  d10v_frame_prev_register
};

const struct frame_unwind *
d10v_frame_p (CORE_ADDR pc)
{
  return &d10v_frame_unwind;
}

static CORE_ADDR
d10v_frame_base_address (struct frame_info *next_frame, void **this_cache)
{
  struct d10v_unwind_cache *info
    = d10v_frame_unwind_cache (next_frame, this_cache);
  return info->base;
}

static const struct frame_base d10v_frame_base = {
  &d10v_frame_unwind,
  d10v_frame_base_address,
  d10v_frame_base_address,
  d10v_frame_base_address
};

/* Assuming NEXT_FRAME->prev is a dummy, return the frame ID of that
   dummy frame.  The frame ID's base needs to match the TOS value
   saved by save_dummy_frame_tos(), and the PC match the dummy frame's
   breakpoint.  */

static struct frame_id
d10v_unwind_dummy_id (struct gdbarch *gdbarch, struct frame_info *next_frame)
{
  ULONGEST base;
  frame_unwind_unsigned_register (next_frame, SP_REGNUM, &base);
  return frame_id_build (d10v_make_daddr (base), frame_pc_unwind (next_frame));
}

static gdbarch_init_ftype d10v_gdbarch_init;

static struct gdbarch *
d10v_gdbarch_init (struct gdbarch_info info, struct gdbarch_list *arches)
{
  struct gdbarch *gdbarch;
  int d10v_num_regs;
  struct gdbarch_tdep *tdep;
  gdbarch_register_name_ftype *d10v_register_name;
  gdbarch_register_sim_regno_ftype *d10v_register_sim_regno;

  /* Find a candidate among the list of pre-declared architectures. */
  arches = gdbarch_list_lookup_by_info (arches, &info);
  if (arches != NULL)
    return arches->gdbarch;

  /* None found, create a new architecture from the information
     provided. */
  tdep = XMALLOC (struct gdbarch_tdep);
  gdbarch = gdbarch_alloc (&info, tdep);

  switch (info.bfd_arch_info->mach)
    {
    case bfd_mach_d10v_ts2:
      d10v_num_regs = 37;
      d10v_register_name = d10v_ts2_register_name;
      d10v_register_sim_regno = d10v_ts2_register_sim_regno;
      tdep->a0_regnum = TS2_A0_REGNUM;
      tdep->nr_dmap_regs = TS2_NR_DMAP_REGS;
      tdep->dmap_register = d10v_ts2_dmap_register;
      tdep->imap_register = d10v_ts2_imap_register;
      break;
    default:
    case bfd_mach_d10v_ts3:
      d10v_num_regs = 42;
      d10v_register_name = d10v_ts3_register_name;
      d10v_register_sim_regno = d10v_ts3_register_sim_regno;
      tdep->a0_regnum = TS3_A0_REGNUM;
      tdep->nr_dmap_regs = TS3_NR_DMAP_REGS;
      tdep->dmap_register = d10v_ts3_dmap_register;
      tdep->imap_register = d10v_ts3_imap_register;
      break;
    }

  set_gdbarch_read_pc (gdbarch, d10v_read_pc);
  set_gdbarch_write_pc (gdbarch, d10v_write_pc);
  set_gdbarch_read_fp (gdbarch, d10v_read_fp);
  set_gdbarch_read_sp (gdbarch, d10v_read_sp);

  set_gdbarch_num_regs (gdbarch, d10v_num_regs);
  set_gdbarch_sp_regnum (gdbarch, 15);
  set_gdbarch_pc_regnum (gdbarch, 18);
  set_gdbarch_register_name (gdbarch, d10v_register_name);
  set_gdbarch_register_size (gdbarch, 2);
  set_gdbarch_register_bytes (gdbarch, (d10v_num_regs - 2) * 2 + 16);
  set_gdbarch_register_byte (gdbarch, d10v_register_byte);
  set_gdbarch_register_raw_size (gdbarch, d10v_register_raw_size);
  set_gdbarch_register_virtual_size (gdbarch, generic_register_size);
  set_gdbarch_register_type (gdbarch, d10v_register_type);

  set_gdbarch_ptr_bit (gdbarch, 2 * TARGET_CHAR_BIT);
  set_gdbarch_addr_bit (gdbarch, 32);
  set_gdbarch_address_to_pointer (gdbarch, d10v_address_to_pointer);
  set_gdbarch_pointer_to_address (gdbarch, d10v_pointer_to_address);
  set_gdbarch_integer_to_address (gdbarch, d10v_integer_to_address);
  set_gdbarch_short_bit (gdbarch, 2 * TARGET_CHAR_BIT);
  set_gdbarch_int_bit (gdbarch, 2 * TARGET_CHAR_BIT);
  set_gdbarch_long_bit (gdbarch, 4 * TARGET_CHAR_BIT);
  set_gdbarch_long_long_bit (gdbarch, 8 * TARGET_CHAR_BIT);
  /* NOTE: The d10v as a 32 bit ``float'' and ``double''. ``long
     double'' is 64 bits. */
  set_gdbarch_float_bit (gdbarch, 4 * TARGET_CHAR_BIT);
  set_gdbarch_double_bit (gdbarch, 4 * TARGET_CHAR_BIT);
  set_gdbarch_long_double_bit (gdbarch, 8 * TARGET_CHAR_BIT);
  switch (info.byte_order)
    {
    case BFD_ENDIAN_BIG:
      set_gdbarch_float_format (gdbarch, &floatformat_ieee_single_big);
      set_gdbarch_double_format (gdbarch, &floatformat_ieee_single_big);
      set_gdbarch_long_double_format (gdbarch, &floatformat_ieee_double_big);
      break;
    case BFD_ENDIAN_LITTLE:
      set_gdbarch_float_format (gdbarch, &floatformat_ieee_single_little);
      set_gdbarch_double_format (gdbarch, &floatformat_ieee_single_little);
      set_gdbarch_long_double_format (gdbarch, &floatformat_ieee_double_little);
      break;
    default:
      internal_error (__FILE__, __LINE__,
		      "d10v_gdbarch_init: bad byte order for float format");
    }

  set_gdbarch_extract_return_value (gdbarch, d10v_extract_return_value);
  set_gdbarch_push_dummy_call (gdbarch, d10v_push_dummy_call);
  set_gdbarch_store_return_value (gdbarch, d10v_store_return_value);
  set_gdbarch_extract_struct_value_address (gdbarch, d10v_extract_struct_value_address);
  set_gdbarch_use_struct_convention (gdbarch, d10v_use_struct_convention);

  set_gdbarch_skip_prologue (gdbarch, d10v_skip_prologue);
  set_gdbarch_inner_than (gdbarch, core_addr_lessthan);
  set_gdbarch_decr_pc_after_break (gdbarch, 4);
  set_gdbarch_function_start_offset (gdbarch, 0);
  set_gdbarch_breakpoint_from_pc (gdbarch, d10v_breakpoint_from_pc);

  set_gdbarch_remote_translate_xfer_address (gdbarch, remote_d10v_translate_xfer_address);

  set_gdbarch_frame_args_skip (gdbarch, 0);
  set_gdbarch_frameless_function_invocation (gdbarch, frameless_look_for_prologue);

  set_gdbarch_frame_num_args (gdbarch, frame_num_args_unknown);
  set_gdbarch_stack_align (gdbarch, d10v_stack_align);

  set_gdbarch_register_sim_regno (gdbarch, d10v_register_sim_regno);

  set_gdbarch_print_registers_info (gdbarch, d10v_print_registers_info);

  frame_unwind_append_predicate (gdbarch, d10v_frame_p);
  frame_base_set_default (gdbarch, &d10v_frame_base);

  /* Methods for saving / extracting a dummy frame's ID.  */
  set_gdbarch_unwind_dummy_id (gdbarch, d10v_unwind_dummy_id);
  set_gdbarch_save_dummy_frame_tos (gdbarch, generic_save_dummy_frame_tos);

  /* Return the unwound PC value.  */
  set_gdbarch_unwind_pc (gdbarch, d10v_unwind_pc);

  return gdbarch;
}

void
_initialize_d10v_tdep (void)
{
  register_gdbarch_init (bfd_arch_d10v, d10v_gdbarch_init);

  tm_print_insn = print_insn_d10v;

  target_resume_hook = d10v_eva_prepare_to_trace;
  target_wait_loop_hook = d10v_eva_get_trace_data;

  deprecate_cmd (add_com ("regs", class_vars, show_regs, "Print all registers"),
		 "info registers");

  add_com ("itrace", class_support, trace_command,
	   "Enable tracing of instruction execution.");

  add_com ("iuntrace", class_support, untrace_command,
	   "Disable tracing of instruction execution.");

  add_com ("itdisassemble", class_vars, tdisassemble_command,
	   "Disassemble the trace buffer.\n\
Two optional arguments specify a range of trace buffer entries\n\
as reported by info trace (NOT addresses!).");

  add_info ("itrace", trace_info,
	    "Display info about the trace data buffer.");

  add_show_from_set (add_set_cmd ("itracedisplay", no_class,
				  var_integer, (char *) &trace_display,
			     "Set automatic display of trace.\n", &setlist),
		     &showlist);
  add_show_from_set (add_set_cmd ("itracesource", no_class,
			   var_integer, (char *) &default_trace_show_source,
		      "Set display of source code with trace.\n", &setlist),
		     &showlist);

}
