/* Output dbx-format symbol table information from GNU compiler.
   Copyright (C) 1987 Free Software Foundation, Inc.

This file is part of GNU CC.

GNU CC is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY.  No author or distributor
accepts responsibility to anyone for the consequences of using it
or for whether it serves any particular purpose or works at all,
unless he says so in writing.  Refer to the GNU CC General Public
License for full details.

Everyone is granted permission to copy, modify and redistribute
GNU CC, but only under the conditions described in the
GNU CC General Public License.   A copy of this license is
supposed to have been given to you along with GNU CC so you
can know your rights and responsibilities.  It should be in a
file named COPYING.  Among other things, the copyright notice
and this notice must be preserved on all copies.  */


/* Output dbx-format symbol table data.
   This consists of many symbol table entries, each of them
   a .stabs assembler pseudo-op with four operands:
   a "name" which is really a description of one symbol and its type,
   a "code", which is a symbol defined in stab.h whose name starts with N_,
   an unused operand always 0,
   and a "value" which is an address or an offset.
   The name is enclosed in doublequote characters.

   Each function, variable, typedef, and structure tag
   has a symbol table entry to define it.
   The beginning and end of each level of name scoping within
   a function are also marked by special symbol table entries.

   The "name" consists of the symbol name, a colon, a kind-of-symbol letter,
   and a data type number.  The data type number may be followed by
   "=" and a type definition; normally this will happen the first time
   the type number is mentioned.  The type definition may refer to
   other types by number, and those type numbers may be followed
   by "=" and nested definitions.

   This can make the "name" quite long.
   When a name is more than 80 characters, we split the .stabs pseudo-op
   into two .stabs pseudo-ops, both sharing the same "code" and "value".
   The first one is marked as continued with a double-backslash at the
   end of its "name".

   The kind-of-symbol letter distinguished function names from global
   variables from file-scope variables from parameters from auto
   variables in memory from typedef names from register variables.
   See `dbxout_symbol'.

   The "code" is mostly redundant with the kind-of-symbol letter
   that goes in the "name", but not entirely: for symbols located
   in static storage, the "code" says which segment the address is in,
   which controls how it is relocated.

   The "value" for a symbol in static storage
   is the core address of the symbol (actually, the assembler
   label for the symbol).  For a symbol located in a stack slot
   it is the stack offset; for one in a register, the register number.
   For a typedef symbol, it is zero.

   For more on data type definitions, see `dbxout_type'.  */

#include "config.h"
#include "tree.h"
#include "rtl.h"
#include "c-tree.h"
#include <stdio.h>
#include <stab.h>

/* Stream for writing to assembler file.  */

static FILE *asmfile;

enum typestatus {TYPE_UNSEEN, TYPE_XREF, TYPE_DEFINED};

/* Vector recording the status of describing C data types.
   When we first notice a data type (a tree node),
   we assign it a number using next_type_number.
   That is its index in this vector.
   The vector element says whether we have yet output
   the definition of the type.  TYPE_XREF says we have
   output it as a cross-reference only.  */

enum typestatus *typevec;

/* Number of elements of space allocated in `typevec'.  */

static int typevec_len;

/* In dbx output, each type gets a unique number.
   This is the number for the next type output.
   The number, once assigned, is in the TYPE_SYMTAB_ADDRESS field.  */

static int next_type_number;

/* In dbx output, we must assign symbol-blocks id numbers
   in the order in which their beginnings are encountered.
   We output debugging info that refers to the beginning and
   end of the ranges of code in each block
   with assembler labels LBBn and LBEn, where n is the block number.
   The labels are generated in final, which assigns numbers to the
   blocks in the same way.  */

static int next_block_number;

/* These variables are for dbxout_symbol to communicate to
   dbxout_finish_symbol.
   current_sym_code is the symbol-type-code, a symbol N_... define in stab.h.
   current_sym_value and current_sym_addr are two ways to address the
   value to store in the symtab entry.
   current_sym_addr if nonzero represents the value as an rtx.
   If that is zero, current_sym_value is used.  This is used
   when the value is an offset (such as for auto variables,
   register variables and parms).  */

static int current_sym_code;
static int current_sym_value;
static rtx current_sym_addr;

/* Number of chars of symbol-description generated so far for the
   current symbol.  Used by CHARS and CONTIN.  */

static int current_sym_nchars;

/* Report having output N chars of the current symbol-description.  */

#define CHARS(N) (current_sym_nchars += (N))

/* Break the current symbol-description, generating a continuation,
   if it has become long.  */

#define CONTIN  \
  do {if (current_sym_nchars > 80) dbxout_continue ();} while (0)

void dbxout_types ();
void dbxout_tags ();
static void dbxout_type_name ();
static void dbxout_type ();
static void dbxout_type_def ();
static void dbxout_finish_symbol ();
static void dbxout_continue ();

/* At the beginning of compilation, start writing the symbol table.
   Initialize `typevec' and output the standard data types of C.  */

void
dbxout_init (asm_file, input_file_name)
     FILE *asm_file;
     char *input_file_name;
{
  asmfile = asm_file;

  typevec_len = 100;
  typevec = (enum typestatus *) xmalloc (typevec_len * sizeof typevec[0]);
  bzero (typevec, typevec_len * sizeof typevec[0]);
  
  fprintf (asmfile,
	   "Ltext:\t.stabs \"%s\",%d,0,0,Ltext\n",
	   input_filename, N_SO);

  next_type_number = 1;
  next_block_number = 2;

  /* Make sure that types `int' and `char' have numbers 1 and 2.
     Definitions of other integer types will refer to those numbers.  */

  dbxout_type_def (integer_type_node);
  dbxout_type_def (char_type_node);

  /* Get all permanent types not yet gotten
     and output them.  */

  dbxout_types (get_permanent_types ());
}

/* Continue a symbol-description that gets too big.
   End one symbol table entry with a double-backslash
   and start a new one, eventually producing something like
   .stabs "start......\\",code,0,value
   .stabs "...rest",code,0,value   */

static void
dbxout_continue ()
{
  fprintf (asmfile, "\\\\");
  dbxout_finish_symbol ();
  fprintf (asmfile, ".stabs \"");
  current_sym_nchars = 0;
}

/* Output a reference to a type.  If the type has not yet been
   described in the dbx output, output its definition now.
   For a type already defined, just refer to its definition
   using the type number.

   If FULL is nonzero, and the type has been described only with
   a forward-reference, output the definition now.
   If FULL is zero in this case, just refer to the forward-reference
   using the number previously allocated.  */

static void
dbxout_type (type, full)
     tree type;
     int full;
{
  register tree tem;

  if (TYPE_SYMTAB_ADDRESS (type) == 0)
    {
      /* Type has no dbx number assigned.  Assign next available number.  */
      TYPE_SYMTAB_ADDRESS (type) = next_type_number++;

      /* Make sure type vector is long enough to record about this type.  */

      if (next_type_number == typevec_len)
	{
	  typevec = (enum typestatus *) xrealloc (typevec, typevec_len * 2 * sizeof typevec[0]);
	  bzero (typevec + typevec_len, typevec_len * sizeof typevec[0]);
	  typevec_len *= 2;
	}
    }

  /* Output the number of this type, to refer to it.  */
  fprintf (asmfile, "%d", TYPE_SYMTAB_ADDRESS (type));
  CHARS (3);

  /* If this type's definition has been output or is now being output,
     that is all.  */

  switch (typevec[TYPE_SYMTAB_ADDRESS (type)])
    {
    case TYPE_UNSEEN:
      break;
    case TYPE_XREF:
      if (! full)
	return;
      break;
    case TYPE_DEFINED:
      return;
    }

  /* Output a definition now.  */

  fprintf (asmfile, "=");
  CHARS (1);

  /* Mark it as defined, so that if it is self-referent
     we will not get into an infinite recursion of definitions.  */

  typevec[TYPE_SYMTAB_ADDRESS (type)] = TYPE_DEFINED;

  switch (TREE_CODE (type))
    {
    case VOID_TYPE:
      /* For a void type, just define it as itself; ie, "5=5".
	 This makes us consider it defined
	 without saying what it is.  The debugger will make it
	 a void type when the reference is seen, and nothing will
	 ever override that default.  */
      fprintf (asmfile, "%d", TYPE_SYMTAB_ADDRESS (type));
      CHARS (3);
      break;

    case INTEGER_TYPE:
      if (type == char_type_node)
	/* Output the type `char' as a subrange of itself!
	   I don't understand this definition, just copied it
	   from the output of pcc.  */
	fprintf (asmfile, "r2;0;127;");
      else
	/* Output other integer types as subranges of `int'.  */
	fprintf (asmfile, "r1;%d;%d;",
		 TREE_INT_CST_LOW (TYPE_MIN_VALUE (type)),
		 TREE_INT_CST_LOW (TYPE_MAX_VALUE (type)));
      CHARS (25);
      break;

    case REAL_TYPE:
      /* This must be magic.  */
      fprintf (asmfile, "r1;%d;0;",
	       TREE_INT_CST_LOW (size_in_bytes (type)));
      CHARS (16);
      break;

    case ARRAY_TYPE:
      /* Output "a" followed by a range type definition
	 for the index type of the array
	 followed by a reference to the target-type.
	 ar1;0;N;M for an array of type M and size N.  */
      fprintf (asmfile, "ar1;0;%d;",
	       TREE_INT_CST_LOW (TYPE_MAX_VALUE (TYPE_DOMAIN (type))));
      CHARS (17);
      dbxout_type (TREE_TYPE (type), 0);
      break;

    case RECORD_TYPE:
    case UNION_TYPE:
      /* Output a structure type.  */
      if ((TYPE_NAME (type) != 0 && !full)
	  || TYPE_SIZE (type) == 0)
	{
	  /* If the type is just a cross reference, output one
	     and mark the type as partially described.
	     If it later becomes defined, we will output
	     its real definition.  */
	  fprintf (asmfile, (TREE_CODE (type) == RECORD_TYPE) ? "xs" : "xu");
	  CHARS (3);
	  dbxout_type_name (type);
	  fprintf (asmfile, ":");
	  typevec[TYPE_SYMTAB_ADDRESS (type)] = TYPE_XREF;
	  break;
	}
      tem = size_in_bytes (type);
      fprintf (asmfile, (TREE_CODE (type) == RECORD_TYPE) ? "s%d" : "u%d",
	       TREE_INT_CST_LOW (tem));
      CHARS (11);
      for (tem = TYPE_FIELDS (type); tem; tem = TREE_CHAIN (tem))
	/* Output the name, type, position (in bits), size (in bits)
	   of each field.  */
	/* Omit here the nameless fields that are used to skip bits.  */
	if (DECL_NAME (tem) != 0)
	  {
	    CONTIN;
	    fprintf (asmfile, "%s:", IDENTIFIER_POINTER (DECL_NAME (tem)));
	    CHARS (1 + strlen (IDENTIFIER_POINTER (DECL_NAME (tem))));
	    dbxout_type (TREE_TYPE (tem), 0);
	    fprintf (asmfile, ",%d,%d;", DECL_OFFSET (tem),
		     TREE_INT_CST_LOW (DECL_SIZE (tem)) * DECL_SIZE_UNIT (tem));
	    CHARS (23);
	  }
      putc (';', asmfile);
      CHARS (1);
      break;

    case ENUMERAL_TYPE:
      if ((TYPE_NAME (type) != 0 && !full)
	  || TYPE_SIZE (type) == 0)
	{
	  fprintf (asmfile, "xe");
	  CHARS (3);
	  dbxout_type_name (type);
	  typevec[TYPE_SYMTAB_ADDRESS (type)] = TYPE_XREF;
	  fprintf (asmfile, ":");
	  return;
	}
      putc ('e', asmfile);
      CHARS (1);
      for (tem = TYPE_VALUES (type); tem; tem = TREE_CHAIN (tem))
	{
	  fprintf (asmfile, "%s:%d,", IDENTIFIER_POINTER (TREE_PURPOSE (tem)),
		   TREE_INT_CST_LOW (TREE_VALUE (tem)));
	  CHARS (11 + strlen (IDENTIFIER_POINTER (TREE_PURPOSE (tem))));
	  if (TREE_CHAIN (tem) != 0)
	    CONTIN;
	}
      putc (';', asmfile);
      CHARS (1);
      break;

    case POINTER_TYPE:
      putc ('*', asmfile);
      CHARS (1);
      dbxout_type (TREE_TYPE (type), 0);
      break;

    case FUNCTION_TYPE:
      putc ('f', asmfile);
      CHARS (1);
      dbxout_type (TREE_TYPE (type), 0);
      break;
    }
}

/* Output the name of type TYPE, with no punctuation.
   Such names can be set up either by typedef declarations
   or by struct, enum and union tags.  */

static void
dbxout_type_name (type)
     register tree type;
{
  register char *name;
  if (TYPE_NAME (type) == 0)
    abort ();
  if (TREE_CODE (TYPE_NAME (type)) == IDENTIFIER_NODE)
    name = IDENTIFIER_POINTER (TYPE_NAME (type));
  else if (TREE_CODE (TYPE_NAME (type)) == TYPE_DECL)
    name = IDENTIFIER_POINTER (DECL_NAME (TYPE_NAME (type)));
  else
    abort ();

  fprintf (asmfile, "%s", name);
  CHARS (strlen (name));
}

/* Output a .stabs for the symbol defined by DECL,
   which must be a ..._DECL node in the normal namespace.
   It may be a CONST_DECL, a FUNCTION_DECL, a PARM_DECL or a VAR_DECL.
   LOCAL is nonzero if the scope is less than the entire file.  */

void
dbxout_symbol (decl, local)
     tree decl;
     int local;
{
  int symcode;
  int letter;

  /* If global, first output all types and all
     struct, enum and union tags that have been created
     and not yet output.  */

  if (local == 0)
    {
      dbxout_tags (gettags ());
      dbxout_types (get_permanent_types ());
    }

  current_sym_code = 0;
  current_sym_value = 0;
  current_sym_addr = 0;

  /* The output will always start with the symbol name,
     so count that always in the length-output-so-far.  */

  current_sym_nchars = 2 + strlen (IDENTIFIER_POINTER (DECL_NAME (decl)));

  switch (TREE_CODE (decl))
    {
    case CONST_DECL:
      /* Enum values are defined by defining the enum type.  */
      break;

    case FUNCTION_DECL:
      if (TREE_EXTERNAL (decl))
	break;
      if (GET_CODE (DECL_RTL (decl)) != MEM
	  || GET_CODE (XEXP (DECL_RTL (decl), 0)) != SYMBOL_REF)
	break;
      fprintf (asmfile, ".stabs \"%s:%c",
	       IDENTIFIER_POINTER (DECL_NAME (decl)),
	       TREE_PUBLIC (decl) ? 'F' : 'f');

      current_sym_code = N_FUN;
      current_sym_addr = XEXP (DECL_RTL (decl), 0);

      if (TREE_TYPE (TREE_TYPE (decl)))
	dbxout_type (TREE_TYPE (TREE_TYPE (decl)), 0);
      else
	dbxout_type (void_type_node, 0);
      dbxout_finish_symbol ();
      break;

    case TYPE_DECL:
      /* Output typedef name.  */
      fprintf (asmfile, ".stabs \"%s:t",
	       IDENTIFIER_POINTER (DECL_NAME (decl)));

      current_sym_code = N_LSYM;

      dbxout_type (TREE_TYPE (decl), 0);
      dbxout_finish_symbol ();
      break;
      
    case PARM_DECL:
      /* Parm decls go in their own separate chains
	 and are output by dbxout_reg_parms and dbxout_parms.  */
      abort ();

    case VAR_DECL:
      /* Don't mention a variable that is external.
	 Let the file that defines it describe it.  */
      if (TREE_EXTERNAL (decl))
	break;

      /* Don't mention a variable at all
	 if it was completely optimized into nothingness.  */
      if (GET_CODE (DECL_RTL (decl)) == REG
	  && REGNO (DECL_RTL (decl)) == -1)
	break;

      /* Ok, start a symtab entry and output the variable name.  */
      fprintf (asmfile, ".stabs \"%s:",
	       IDENTIFIER_POINTER (DECL_NAME (decl)));
      
      /* The kind-of-variable letter depends on where
	 the variable is and on the scope of its name:
	 G and N_GSYM for static storage and global scope,
	 S for static storage and file scope,
	 v for static storage and local scope,
	    for those two, use N_LCSYM if data is in bss segment,
	    N_STSYM if it is in data segment, or N_FUN if in text segment.
	 no letter at all, and N_LSYM, for auto variable,
	 r and N_RSYM for register variable.  */

      if (GET_CODE (DECL_RTL (decl)) == MEM
	  && GET_CODE (XEXP (DECL_RTL (decl), 0)) == SYMBOL_REF)
	{
	  if (TREE_PUBLIC (decl))
	    {
	      letter = 'G';
	      current_sym_code = N_GSYM;
	    }
	  else
	    {
	      current_sym_addr = XEXP (DECL_RTL (decl), 0);

	      letter = TREE_PERMANENT (decl) ? 'S' : 'v';

	      if (!DECL_INITIAL (decl))
		current_sym_code = N_LCSYM;
	      else if (TREE_READONLY (decl) && ! TREE_VOLATILE (decl))
		/* This is not quite right, but it's the closest
		   of all the codes that Unix defines.  */
		current_sym_code = N_FUN;
	      else
		current_sym_code = N_STSYM;
	    }
	  putc (letter, asmfile);
	  dbxout_type (TREE_TYPE (decl), 0);
	  dbxout_finish_symbol ();
	}
      else
	{
	  if (GET_CODE (DECL_RTL (decl)) == REG)
	    {
	      letter = 'r';
	      current_sym_code = N_RSYM;
	      current_sym_value = DBX_REGISTER_NUMBER (REGNO (DECL_RTL (decl)));
	    }
	  else
	    {
	      letter = 0;
	      current_sym_code = N_LSYM;
	      /* DECL_RTL looks like (MEM (PLUS (REG...) (CONST_INT...))).
		 We want the value of that CONST_INT.  */
	      current_sym_value = INTVAL (XEXP (XEXP (DECL_RTL (decl), 0), 1));
	    }
	  if (letter) putc (letter, asmfile);
	  dbxout_type (TREE_TYPE (decl), 0);
	  dbxout_finish_symbol ();
	}
      break;
    }
}

static void
dbxout_finish_symbol ()
{
  fprintf (asmfile, "\",%d,0,0,", current_sym_code);
  if (current_sym_addr)
    output_addr_const (asmfile, current_sym_addr);
  else
    fprintf (asmfile, "%d", current_sym_value);
  putc ('\n', asmfile);
}

/* Output definitions of all the decls in a chain.  */

static void
dbxout_syms (syms)
     tree syms;
{
  while (syms)
    {
      dbxout_symbol (syms, 1);
      syms = TREE_CHAIN (syms);
    }
}

/* The following two functions output definitions of function parameters.
   Each parameter gets a definition locating it in the parameter list.
   Each parameter that is a register variable gets a second definition
   locating it in the register.

   Printing or argument lists in gdb uses the definitions that
   locate in the parameter list.  But reference to the variable in
   expressions uses preferentially the definition as a register.  */

/* Output definitions, referring to storage in the parmlist,
   of all the parms in PARMS, which is a chain of PARM_DECL nodes.  */

static void
dbxout_parms (parms)
     tree parms;
{
  for (; parms; parms = TREE_CHAIN (parms))
    {
      current_sym_code = N_PSYM;
      current_sym_value = DECL_OFFSET (parms) / BITS_PER_UNIT;
      /* A parm declared char is really passed as an int,
	 so it occupies the least significant bytes.
	 On a big-endian machine those are not the low-numbered ones.  */
#ifdef BYTES_BIG_ENDIAN
      current_sym_value += (GET_MODE_SIZE (TYPE_MODE (DECL_ARG_TYPE (parms)))
			    - GET_MODE_SIZE (GET_MODE (DECL_RTL (parms))));
#endif
      current_sym_addr = 0;
      current_sym_nchars = 2 + strlen (IDENTIFIER_POINTER (DECL_NAME (parms)));

      fprintf (asmfile, ".stabs \"%s:p",
	       IDENTIFIER_POINTER (DECL_NAME (parms)));
      dbxout_type (TREE_TYPE (parms), 0);
      dbxout_finish_symbol ();
    }
}

/* Output definitions, referring to registers,
   of all the parms in PARMS which are stored in registers during the function.
   PARMS is a chain of PARM_DECL nodes.  */

static void
dbxout_reg_parms (parms)
     tree parms;
{
  while (parms)
    {
      if (GET_CODE (DECL_RTL (parms)) == REG
	  && REGNO (DECL_RTL (parms)) >= 0)
	{
	  current_sym_code = N_RSYM;
	  current_sym_value = DBX_REGISTER_NUMBER (REGNO (DECL_RTL (parms)));
	  current_sym_addr = 0;
	  current_sym_nchars = 2 + strlen (IDENTIFIER_POINTER (DECL_NAME (parms)));
	  fprintf (asmfile, ".stabs \"%s:r",
		   IDENTIFIER_POINTER (DECL_NAME (parms)));
	  dbxout_type (TREE_TYPE (parms), 0);
	  dbxout_finish_symbol ();
	}
      parms = TREE_CHAIN (parms);
    }
}

/* Given a chain of ..._TYPE nodes, all of which have names,
   output definitions of those names, as typedefs.  */

void
dbxout_types (types)
     register tree types;
{
  while (types)
    {
      if (TYPE_NAME (types)
	  && TREE_CODE (TYPE_NAME (types)) == TYPE_DECL)
	dbxout_type_def (types);
      types = TREE_CHAIN (types);
    }
}

/* Output a definition of a typedef name.
   It works much like any other kind of symbol definition.  */

static void
dbxout_type_def (type)
     tree type;
{
  current_sym_code = N_LSYM;
  current_sym_value = 0;
  current_sym_addr = 0;
  current_sym_nchars = 0;
  current_sym_nchars
    = 2 + strlen (IDENTIFIER_POINTER (DECL_NAME (TYPE_NAME (type))));

  fprintf (asmfile, ".stabs \"%s:t",
	   IDENTIFIER_POINTER (DECL_NAME (TYPE_NAME (type))));
  dbxout_type (type, 1);
  dbxout_finish_symbol ();
}

/* Output the tags (struct, union and enum definitions with names) for a block,
   given a list of them (a chain of TREE_LIST nodes) in TAGS.
   We must check to include those that have been mentioned already with
   only a cross-reference.  */

void
dbxout_tags (tags)
     tree tags;
{
  register tree link;
  for (link = tags; link; link = TREE_CHAIN (link))
    {
      register tree type = TREE_VALUE (link);
      if (TREE_PURPOSE (link) != 0
	  && (TYPE_SYMTAB_ADDRESS (type) == 0
	      || (typevec[TYPE_SYMTAB_ADDRESS (type)] != TYPE_DEFINED))
	  && TYPE_SIZE (type) != 0)
	{
	  current_sym_code = N_LSYM;
	  current_sym_value = 0;
	  current_sym_addr = 0;
	  current_sym_nchars = 2 + strlen (IDENTIFIER_POINTER (TREE_PURPOSE (link)));

	  fprintf (asmfile, ".stabs \"%s:T",
		   IDENTIFIER_POINTER (TREE_PURPOSE (link)));
	  dbxout_type (type, 1);
	  dbxout_finish_symbol ();
	  typevec[TYPE_SYMTAB_ADDRESS (type)] = TYPE_DEFINED;
	}
    }
}

/* Output everything about a symbol block (that is to say, a LET_STMT node
   that represents a scope level),
   including recursive output of contained blocks.

   STMT is the LET_STMT node.
   DEPTH is its depth within containing symbol blocks.
   ARGS is usually zero; but for the outermost block of the
   body of a function, it is a chain of PARM_DECLs for the function parameters.
   We output definitions of all the register parms
   as if they were local variables of that block.

   Actually, STMT may be several statements chained together.
   We handle them all in sequence.  */

static void
dbxout_block (stmt, depth, args)
     register tree stmt;
     int depth;
{
  int blocknum;

  while (stmt)
    {
      switch (TREE_CODE (stmt))
	{
	case COMPOUND_STMT:
	case LOOP_STMT:
	  dbxout_block (STMT_BODY (stmt), depth, 0);
	  break;

	case IF_STMT:
	  dbxout_block (STMT_THEN (stmt), depth, 0);
	  dbxout_block (STMT_ELSE (stmt), depth, 0);
	  break;

	case LET_STMT:
	  /* In dbx format, the syms of a block come before the N_LBRAC.  */
	  dbxout_tags (STMT_TYPE_TAGS (stmt));
	  dbxout_syms (STMT_VARS (stmt), 1);
	  if (args)
	    dbxout_reg_parms (args);

	  /* Now output an N_LBRAC symbol to represent the beginning of
	     the block.  Use the block's tree-walk order to generate
	     the assembler symbols LBBn and LBEn
	     that final will define around the code in this block.  */
	  if (depth > 0)
	    {
	      blocknum = next_block_number++;
	      fprintf (asmfile, ".stabn %d,0,0,LBB%d\n", N_LBRAC, blocknum);
	    }

	  /* Output the interior of the block.  */
	  dbxout_block (STMT_BODY (stmt), depth + 1, 0);

	  /* Refer to the marker for the end of the block.  */
	  if (depth > 0)
	    fprintf (asmfile, ".stabn %d,0,0,LBE%d\n", N_RBRAC, blocknum);
	}
      stmt = TREE_CHAIN (stmt);
    }
}

/* Output dbx data for a function definition.
   This includes a definition of the function name itself (a symbol),
   definitions of the parameters (locating them in the parameter list)
   and then output the block that makes up the function's body
   (including all the auto variables of the function).  */

void
dbxout_function (decl)
     tree decl;
{
  dbxout_symbol (decl, 0);
  dbxout_parms (DECL_ARGUMENTS (decl));
  dbxout_block (DECL_INITIAL (decl), 0, DECL_ARGUMENTS (decl));
}
