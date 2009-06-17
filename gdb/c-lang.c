/* C language support routines for GDB, the GNU debugger.

   Copyright (C) 1992, 1993, 1994, 1995, 1996, 1998, 1999, 2000, 2002, 2003,
   2004, 2005, 2007, 2008, 2009 Free Software Foundation, Inc.

   This file is part of GDB.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

#include "defs.h"
#include "symtab.h"
#include "gdbtypes.h"
#include "expression.h"
#include "parser-defs.h"
#include "language.h"
#include "c-lang.h"
#include "valprint.h"
#include "macroscope.h"
#include "gdb_assert.h"
#include "charset.h"
#include "gdb_string.h"
#include "demangle.h"
#include "cp-abi.h"
#include "cp-support.h"
#include "gdb_obstack.h"
#include <ctype.h>

extern void _initialize_c_language (void);

/* Given a C string type, STR_TYPE, return the corresponding target
   character set name.  */

static const char *
charset_for_string_type (enum c_string_type str_type)
{
  switch (str_type & ~C_CHAR)
    {
    case C_STRING:
      return target_charset ();
    case C_WIDE_STRING:
      return target_wide_charset ();
    case C_STRING_16:
      /* FIXME: UCS-2 is not always correct.  */
      if (gdbarch_byte_order (current_gdbarch) == BFD_ENDIAN_BIG)
	return "UCS-2BE";
      else
	return "UCS-2LE";
    case C_STRING_32:
      /* FIXME: UCS-4 is not always correct.  */
      if (gdbarch_byte_order (current_gdbarch) == BFD_ENDIAN_BIG)
	return "UCS-4BE";
      else
	return "UCS-4LE";
    }
  internal_error (__FILE__, __LINE__, "unhandled c_string_type");
}

/* Classify ELTTYPE according to what kind of character it is.  Return
   the enum constant representing the character type.  Also set
   *ENCODING to the name of the character set to use when converting
   characters of this type to the host character set.  */

static enum c_string_type
classify_type (struct type *elttype, const char **encoding)
{
  struct type *saved_type;
  enum c_string_type result;

  /* We do one or two passes -- one on ELTTYPE, and then maybe a
     second one on a typedef target.  */
  do
    {
      char *name = TYPE_NAME (elttype);

      if (TYPE_CODE (elttype) == TYPE_CODE_CHAR || !name)
	{
	  result = C_CHAR;
	  goto done;
	}

      if (!strcmp (name, "wchar_t"))
	{
	  result = C_WIDE_CHAR;
	  goto done;
	}

      if (!strcmp (name, "char16_t"))
	{
	  result = C_CHAR_16;
	  goto done;
	}

      if (!strcmp (name, "char32_t"))
	{
	  result = C_CHAR_32;
	  goto done;
	}

      saved_type = elttype;
      CHECK_TYPEDEF (elttype);
    }
  while (elttype != saved_type);

  /* Punt.  */
  result = C_CHAR;

 done:
  *encoding = charset_for_string_type (result);
  return result;
}

/* Return true if print_wchar can display W without resorting to a
   numeric escape, false otherwise.  */

static int
wchar_printable (gdb_wchar_t w)
{
  return (gdb_iswprint (w)
	  || w == LCST ('\a') || w == LCST ('\b')
	  || w == LCST ('\f') || w == LCST ('\n')
	  || w == LCST ('\r') || w == LCST ('\t')
	  || w == LCST ('\v'));
}

/* A helper function that converts the contents of STRING to wide
   characters and then appends them to OUTPUT.  */

static void
append_string_as_wide (const char *string, struct obstack *output)
{
  for (; *string; ++string)
    {
      gdb_wchar_t w = gdb_btowc (*string);
      obstack_grow (output, &w, sizeof (gdb_wchar_t));
    }
}

/* Print a wide character W to OUTPUT.  ORIG is a pointer to the
   original (target) bytes representing the character, ORIG_LEN is the
   number of valid bytes.  WIDTH is the number of bytes in a base
   characters of the type.  OUTPUT is an obstack to which wide
   characters are emitted.  QUOTER is a (narrow) character indicating
   the style of quotes surrounding the character to be printed.
   NEED_ESCAPE is an in/out flag which is used to track numeric
   escapes across calls.  */

static void
print_wchar (gdb_wint_t w, const gdb_byte *orig, int orig_len,
	     int width, struct obstack *output, int quoter,
	     int *need_escapep)
{
  int need_escape = *need_escapep;
  *need_escapep = 0;
  if (gdb_iswprint (w) && (!need_escape || (!gdb_iswdigit (w)
					    && w != LCST ('8')
					    && w != LCST ('9'))))
    {
      gdb_wchar_t wchar = w;

      if (w == gdb_btowc (quoter) || w == LCST ('\\'))
	obstack_grow_wstr (output, LCST ("\\"));
      obstack_grow (output, &wchar, sizeof (gdb_wchar_t));
    }
  else
    {
      switch (w)
	{
	case LCST ('\a'):
	  obstack_grow_wstr (output, LCST ("\\a"));
	  break;
	case LCST ('\b'):
	  obstack_grow_wstr (output, LCST ("\\b"));
	  break;
	case LCST ('\f'):
	  obstack_grow_wstr (output, LCST ("\\f"));
	  break;
	case LCST ('\n'):
	  obstack_grow_wstr (output, LCST ("\\n"));
	  break;
	case LCST ('\r'):
	  obstack_grow_wstr (output, LCST ("\\r"));
	  break;
	case LCST ('\t'):
	  obstack_grow_wstr (output, LCST ("\\t"));
	  break;
	case LCST ('\v'):
	  obstack_grow_wstr (output, LCST ("\\v"));
	  break;
	default:
	  {
	    int i;

	    for (i = 0; i + width <= orig_len; i += width)
	      {
		char octal[30];
		ULONGEST value = extract_unsigned_integer (&orig[i], width);
		sprintf (octal, "\\%lo", (long) value);
		append_string_as_wide (octal, output);
	      }
	    /* If we somehow have extra bytes, print them now.  */
	    while (i < orig_len)
	      {
		char octal[5];
		sprintf (octal, "\\%.3o", orig[i] & 0xff);
		append_string_as_wide (octal, output);
		++i;
	      }

	    *need_escapep = 1;
	  }
	  break;
	}
    }
}

/* Print the character C on STREAM as part of the contents of a literal
   string whose delimiter is QUOTER.  Note that that format for printing
   characters and strings is language specific. */

static void
c_emit_char (int c, struct type *type, struct ui_file *stream, int quoter)
{
  struct obstack wchar_buf, output;
  struct cleanup *cleanups;
  const char *encoding;
  gdb_byte *buf;
  struct wchar_iterator *iter;
  int need_escape = 0;

  classify_type (type, &encoding);

  buf = alloca (TYPE_LENGTH (type));
  pack_long (buf, type, c);

  iter = make_wchar_iterator (buf, TYPE_LENGTH (type), encoding,
			      TYPE_LENGTH (type));
  cleanups = make_cleanup_wchar_iterator (iter);

  /* This holds the printable form of the wchar_t data.  */
  obstack_init (&wchar_buf);
  make_cleanup_obstack_free (&wchar_buf);

  while (1)
    {
      int num_chars;
      gdb_wchar_t *chars;
      const gdb_byte *buf;
      size_t buflen;
      int print_escape = 1;
      enum wchar_iterate_result result;

      num_chars = wchar_iterate (iter, &result, &chars, &buf, &buflen);
      if (num_chars < 0)
	break;
      if (num_chars > 0)
	{
	  /* If all characters are printable, print them.  Otherwise,
	     we're going to have to print an escape sequence.  We
	     check all characters because we want to print the target
	     bytes in the escape sequence, and we don't know character
	     boundaries there.  */
	  int i;

	  print_escape = 0;
	  for (i = 0; i < num_chars; ++i)
	    if (!wchar_printable (chars[i]))
	      {
		print_escape = 1;
		break;
	      }

	  if (!print_escape)
	    {
	      for (i = 0; i < num_chars; ++i)
		print_wchar (chars[i], buf, buflen, TYPE_LENGTH (type),
			     &wchar_buf, quoter, &need_escape);
	    }
	}

      /* This handles the NUM_CHARS == 0 case as well.  */
      if (print_escape)
	print_wchar (gdb_WEOF, buf, buflen, TYPE_LENGTH (type), &wchar_buf,
		     quoter, &need_escape);
    }

  /* The output in the host encoding.  */
  obstack_init (&output);
  make_cleanup_obstack_free (&output);

  convert_between_encodings (INTERMEDIATE_ENCODING, host_charset (),
			     obstack_base (&wchar_buf),
			     obstack_object_size (&wchar_buf),
			     1, &output, translit_char);
  obstack_1grow (&output, '\0');

  fputs_filtered (obstack_base (&output), stream);

  do_cleanups (cleanups);
}

void
c_printchar (int c, struct type *type, struct ui_file *stream)
{
  enum c_string_type str_type;
  const char *encoding;

  str_type = classify_type (type, &encoding);
  switch (str_type)
    {
    case C_CHAR:
      break;
    case C_WIDE_CHAR:
      fputc_filtered ('L', stream);
      break;
    case C_CHAR_16:
      fputc_filtered ('u', stream);
      break;
    case C_CHAR_32:
      fputc_filtered ('U', stream);
      break;
    }

  fputc_filtered ('\'', stream);
  LA_EMIT_CHAR (c, type, stream, '\'');
  fputc_filtered ('\'', stream);
}

/* Print the character string STRING, printing at most LENGTH characters.
   LENGTH is -1 if the string is nul terminated.  Each character is WIDTH bytes
   long.  Printing stops early if the number hits print_max; repeat counts are
   printed as appropriate.  Print ellipses at the end if we had to stop before
   printing LENGTH characters, or if FORCE_ELLIPSES.  */

void
c_printstr (struct ui_file *stream, struct type *type, const gdb_byte *string,
	    unsigned int length, int force_ellipses,
	    const struct value_print_options *options)
{
  unsigned int i;
  unsigned int things_printed = 0;
  int in_quotes = 0;
  int need_comma = 0;
  int width = TYPE_LENGTH (type);
  struct obstack wchar_buf, output;
  struct cleanup *cleanup;
  enum c_string_type str_type;
  const char *encoding;
  struct wchar_iterator *iter;
  int finished = 0;
  int need_escape = 0;

  /* If the string was not truncated due to `set print elements', and
     the last byte of it is a null, we don't print that, in traditional C
     style.  */
  if (!force_ellipses
      && length > 0
      && (extract_unsigned_integer (string + (length - 1) * width, width) == 0))
    length--;

  str_type = classify_type (type, &encoding) & ~C_CHAR;
  switch (str_type)
    {
    case C_STRING:
      break;
    case C_WIDE_STRING:
      fputs_filtered ("L", stream);
      break;
    case C_STRING_16:
      fputs_filtered ("u", stream);
      break;
    case C_STRING_32:
      fputs_filtered ("U", stream);
      break;
    }

  if (length == 0)
    {
      fputs_filtered ("\"\"", stream);
      return;
    }

  if (length == -1)
    {
      unsigned long current_char = 1;
      for (i = 0; current_char; ++i)
	{
	  QUIT;
	  current_char = extract_unsigned_integer (string + i * width, width);
	}
      length = i;
    }

  /* Arrange to iterate over the characters, in wchar_t form.  */
  iter = make_wchar_iterator (string, length * width, encoding, width);
  cleanup = make_cleanup_wchar_iterator (iter);

  /* WCHAR_BUF is the obstack we use to represent the string in
     wchar_t form.  */
  obstack_init (&wchar_buf);
  make_cleanup_obstack_free (&wchar_buf);

  while (!finished && things_printed < options->print_max)
    {
      int num_chars;
      enum wchar_iterate_result result;
      gdb_wchar_t *chars;
      const gdb_byte *buf;
      size_t buflen;

      QUIT;

      if (need_comma)
	{
	  obstack_grow_wstr (&wchar_buf, LCST (", "));
	  need_comma = 0;
	}

      num_chars = wchar_iterate (iter, &result, &chars, &buf, &buflen);
      /* We only look at repetitions when we were able to convert a
	 single character in isolation.  This makes the code simpler
	 and probably does the sensible thing in the majority of
	 cases.  */
      while (num_chars == 1)
	{
	  /* Count the number of repetitions.  */
	  unsigned int reps = 0;
	  gdb_wchar_t current_char = chars[0];
	  const gdb_byte *orig_buf = buf;
	  int orig_len = buflen;

	  if (need_comma)
	    {
	      obstack_grow_wstr (&wchar_buf, LCST (", "));
	      need_comma = 0;
	    }

	  while (num_chars == 1 && current_char == chars[0])
	    {
	      num_chars = wchar_iterate (iter, &result, &chars, &buf, &buflen);
	      ++reps;
	    }

	  /* Emit CURRENT_CHAR according to the repetition count and
	     options.  */
	  if (reps > options->repeat_count_threshold)
	    {
	      if (in_quotes)
		{
		  if (options->inspect_it)
		    obstack_grow_wstr (&wchar_buf, LCST ("\\\", "));
		  else
		    obstack_grow_wstr (&wchar_buf, LCST ("\", "));
		  in_quotes = 0;
		}
	      obstack_grow_wstr (&wchar_buf, LCST ("'"));
	      need_escape = 0;
	      print_wchar (current_char, orig_buf, orig_len, width,
			   &wchar_buf, '\'', &need_escape);
	      obstack_grow_wstr (&wchar_buf, LCST ("'"));
	      {
		/* Painful gyrations.  */
		int j;
		char *s = xstrprintf (_(" <repeats %u times>"), reps);
		for (j = 0; s[j]; ++j)
		  {
		    gdb_wchar_t w = gdb_btowc (s[j]);
		    obstack_grow (&wchar_buf, &w, sizeof (gdb_wchar_t));
		  }
		xfree (s);
	      }
	      things_printed += options->repeat_count_threshold;
	      need_comma = 1;
	    }
	  else
	    {
	      /* Saw the character one or more times, but fewer than
		 the repetition threshold.  */
	      if (!in_quotes)
		{
		  if (options->inspect_it)
		    obstack_grow_wstr (&wchar_buf, LCST ("\\\""));
		  else
		    obstack_grow_wstr (&wchar_buf, LCST ("\""));
		  in_quotes = 1;
		  need_escape = 0;
		}

	      while (reps-- > 0)
		{
		  print_wchar (current_char, orig_buf, orig_len, width,
			       &wchar_buf, '"', &need_escape);
		  ++things_printed;
		}
	    }
	}

      /* NUM_CHARS and the other outputs from wchar_iterate are valid
	 here regardless of which branch was taken above.  */
      if (num_chars < 0)
	{
	  /* Hit EOF.  */
	  finished = 1;
	  break;
	}

      switch (result)
	{
	case wchar_iterate_invalid:
	  if (!in_quotes)
	    {
	      if (options->inspect_it)
		obstack_grow_wstr (&wchar_buf, LCST ("\\\""));
	      else
		obstack_grow_wstr (&wchar_buf, LCST ("\""));
	      in_quotes = 1;
	    }
	  need_escape = 0;
	  print_wchar (gdb_WEOF, buf, buflen, width, &wchar_buf,
		       '"', &need_escape);
	  break;

	case wchar_iterate_incomplete:
	  if (in_quotes)
	    {
	      if (options->inspect_it)
		obstack_grow_wstr (&wchar_buf, LCST ("\\\","));
	      else
		obstack_grow_wstr (&wchar_buf, LCST ("\","));
	      in_quotes = 0;
	    }
	  obstack_grow_wstr (&wchar_buf, LCST (" <incomplete sequence "));
	  print_wchar (gdb_WEOF, buf, buflen, width, &wchar_buf,
		       0, &need_escape);
	  obstack_grow_wstr (&wchar_buf, LCST (">"));
	  finished = 1;
	  break;
	}
    }

  /* Terminate the quotes if necessary.  */
  if (in_quotes)
    {
      if (options->inspect_it)
	obstack_grow_wstr (&wchar_buf, LCST ("\\\""));
      else
	obstack_grow_wstr (&wchar_buf, LCST ("\""));
    }

  if (force_ellipses || !finished)
    obstack_grow_wstr (&wchar_buf, LCST ("..."));

  /* OUTPUT is where we collect `char's for printing.  */
  obstack_init (&output);
  make_cleanup_obstack_free (&output);

  convert_between_encodings (INTERMEDIATE_ENCODING, host_charset (),
			     obstack_base (&wchar_buf),
			     obstack_object_size (&wchar_buf),
			     1, &output, translit_char);
  obstack_1grow (&output, '\0');

  fputs_filtered (obstack_base (&output), stream);

  do_cleanups (cleanup);
}

/* Obtain a C string from the inferior storing it in a newly allocated
   buffer in BUFFER, which should be freed by the caller.  The string is
   read until a null character is found. If VALUE is an array with known
   length, the function will not read past the end of the array.  LENGTH
   will contain the size of the string in bytes (not counting the null
   character).

   Assumes strings are terminated by a null character.  The size of a character
   is determined by the length of the target type of the pointer or array.
   This means that a null byte present in a multi-byte character will not
   terminate the string unless the whole character is null.

   CHARSET is always set to the target charset.  */

void
c_get_string (struct value *value, gdb_byte **buffer, int *length,
	      const char **charset)
{
  int err, width;
  unsigned int fetchlimit;
  struct type *type = check_typedef (value_type (value));
  struct type *element_type = TYPE_TARGET_TYPE (type);

  if (element_type == NULL)
    goto error;

  if (TYPE_CODE (type) == TYPE_CODE_ARRAY)
    {
      /* If we know the size of the array, we can use it as a limit on the
	 number of characters to be fetched.  */
      if (TYPE_NFIELDS (type) == 1
	  && TYPE_CODE (TYPE_FIELD_TYPE (type, 0)) == TYPE_CODE_RANGE)
	{
	  LONGEST low_bound, high_bound;

	  get_discrete_bounds (TYPE_FIELD_TYPE (type, 0),
			       &low_bound, &high_bound);
	  fetchlimit = high_bound - low_bound + 1;
	}
      else
	fetchlimit = UINT_MAX;
    }
  else if (TYPE_CODE (type) == TYPE_CODE_PTR)
    fetchlimit = UINT_MAX;
  else
    /* We work only with arrays and pointers.  */
    goto error;

  element_type = check_typedef (element_type);
  if (TYPE_CODE (element_type) != TYPE_CODE_INT
      && TYPE_CODE (element_type) != TYPE_CODE_CHAR)
    /* If the elements are not integers or characters, we don't consider it
       a string.  */
    goto error;

  width = TYPE_LENGTH (element_type);

  /* If the string lives in GDB's memory intead of the inferior's, then we
     just need to copy it to BUFFER.  Also, since such strings are arrays
     with known size, FETCHLIMIT will hold the size of the array.  */
  if ((VALUE_LVAL (value) == not_lval
       || VALUE_LVAL (value) == lval_internalvar)
      && fetchlimit != UINT_MAX)
    {
      int i;
      const gdb_byte *contents = value_contents (value);

      /* Look for a null character.  */
      for (i = 0; i < fetchlimit; i++)
	if (extract_unsigned_integer (contents + i * width, width) == 0)
	  break;

      /* I is now either the number of non-null characters, or FETCHLIMIT.  */
      *length = i * width;
      *buffer = xmalloc (*length);
      memcpy (*buffer, contents, *length);
      err = 0;
    }
  else
    {
      err = read_string (value_as_address (value), -1, width, fetchlimit,
			 buffer, length);
      if (err)
	{
	  xfree (*buffer);
	  error (_("Error reading string from inferior: %s"),
		 safe_strerror (err));
	}
    }

  /* If the last character is null, subtract it from LENGTH.  */
  if (*length > 0
      && extract_unsigned_integer (*buffer + *length - width, width) == 0)
    *length -= width;

  *charset = target_charset ();

  return;

 error:
  {
    char *type_str;

    type_str = type_to_string (type);
    if (type_str)
      {
	make_cleanup (xfree, type_str);
	error (_("Trying to read string with inappropriate type `%s'."),
	       type_str);
      }
    else
      error (_("Trying to read string with inappropriate type."));
  }
}


/* Evaluating C and C++ expressions.  */

/* Convert a UCN.  The digits of the UCN start at P and extend no
   farther than LIMIT.  DEST_CHARSET is the name of the character set
   into which the UCN should be converted.  The results are written to
   OUTPUT.  LENGTH is the maximum length of the UCN, either 4 or 8.
   Returns a pointer to just after the final digit of the UCN.  */

static char *
convert_ucn (char *p, char *limit, const char *dest_charset,
	     struct obstack *output, int length)
{
  unsigned long result = 0;
  gdb_byte data[4];
  int i;

  for (i = 0; i < length && p < limit && isxdigit (*p); ++i, ++p)
    result = (result << 4) + host_hex_value (*p);

  for (i = 3; i >= 0; --i)
    {
      data[i] = result & 0xff;
      result >>= 8;
    }

  convert_between_encodings ("UCS-4BE", dest_charset, data, 4, 4, output,
			     translit_none);

  return p;
}

/* Emit a character, VALUE, which was specified numerically, to
   OUTPUT.  TYPE is the target character type.  */

static void
emit_numeric_character (struct type *type, unsigned long value,
			struct obstack *output)
{
  gdb_byte *buffer;

  buffer = alloca (TYPE_LENGTH (type));
  pack_long (buffer, type, value);
  obstack_grow (output, buffer, TYPE_LENGTH (type));
}

/* Convert an octal escape sequence.  TYPE is the target character
   type.  The digits of the escape sequence begin at P and extend no
   farther than LIMIT.  The result is written to OUTPUT.  Returns a
   pointer to just after the final digit of the escape sequence.  */

static char *
convert_octal (struct type *type, char *p, char *limit, struct obstack *output)
{
  unsigned long value = 0;

  while (p < limit && isdigit (*p) && *p != '8' && *p != '9')
    {
      value = 8 * value + host_hex_value (*p);
      ++p;
    }

  emit_numeric_character (type, value, output);

  return p;
}

/* Convert a hex escape sequence.  TYPE is the target character type.
   The digits of the escape sequence begin at P and extend no farther
   than LIMIT.  The result is written to OUTPUT.  Returns a pointer to
   just after the final digit of the escape sequence.  */

static char *
convert_hex (struct type *type, char *p, char *limit, struct obstack *output)
{
  unsigned long value = 0;

  while (p < limit && isxdigit (*p))
    {
      value = 16 * value + host_hex_value (*p);
      ++p;
    }

  emit_numeric_character (type, value, output);

  return p;
}

#define ADVANCE					\
  do {						\
    ++p;					\
    if (p == limit)				\
      error (_("Malformed escape sequence"));	\
  } while (0)

/* Convert an escape sequence to a target format.  TYPE is the target
   character type to use, and DEST_CHARSET is the name of the target
   character set.  The backslash of the escape sequence is at *P, and
   the escape sequence will not extend past LIMIT.  The results are
   written to OUTPUT.  Returns a pointer to just past the final
   character of the escape sequence.  */

static char *
convert_escape (struct type *type, const char *dest_charset,
		char *p, char *limit, struct obstack *output)
{
  /* Skip the backslash.  */
  ADVANCE;

  switch (*p)
    {
    case '\\':
      obstack_1grow (output, '\\');
      ++p;
      break;

    case 'x':
      ADVANCE;
      if (!isxdigit (*p))
	error (_("\\x used with no following hex digits."));
      p = convert_hex (type, p, limit, output);
      break;

    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
      p = convert_octal (type, p, limit, output);
      break;

    case 'u':
    case 'U':
      {
	int length = *p == 'u' ? 4 : 8;
	ADVANCE;
	if (!isxdigit (*p))
	  error (_("\\u used with no following hex digits"));
	p = convert_ucn (p, limit, dest_charset, output, length);
      }
    }

  return p;
}

/* Given a single string from a (C-specific) OP_STRING list, convert
   it to a target string, handling escape sequences specially.  The
   output is written to OUTPUT.  DATA is the input string, which has
   length LEN.  DEST_CHARSET is the name of the target character set,
   and TYPE is the type of target character to use.  */

static void
parse_one_string (struct obstack *output, char *data, int len,
		  const char *dest_charset, struct type *type)
{
  char *limit;

  limit = data + len;

  while (data < limit)
    {
      char *p = data;
      /* Look for next escape, or the end of the input.  */
      while (p < limit && *p != '\\')
	++p;
      /* If we saw a run of characters, convert them all.  */
      if (p > data)
	convert_between_encodings (host_charset (), dest_charset,
				   data, p - data, 1, output, translit_none);
      /* If we saw an escape, convert it.  */
      if (p < limit)
	p = convert_escape (type, dest_charset, p, limit, output);
      data = p;
    }
}

/* Expression evaluator for the C language family.  Most operations
   are delegated to evaluate_subexp_standard; see that function for a
   description of the arguments.  */

static struct value *
evaluate_subexp_c (struct type *expect_type, struct expression *exp,
		   int *pos, enum noside noside)
{
  enum exp_opcode op = exp->elts[*pos].opcode;

  switch (op)
    {
    case OP_STRING:
      {
	int oplen, limit;
	struct type *type;
	struct obstack output;
	struct cleanup *cleanup;
	struct value *result;
	enum c_string_type dest_type;
	const char *dest_charset;

	obstack_init (&output);
	cleanup = make_cleanup_obstack_free (&output);

	++*pos;
	oplen = longest_to_int (exp->elts[*pos].longconst);

	++*pos;
	limit = *pos + BYTES_TO_EXP_ELEM (oplen + 1);
	dest_type
	  = (enum c_string_type) longest_to_int (exp->elts[*pos].longconst);
	switch (dest_type & ~C_CHAR)
	  {
	  case C_STRING:
	    type = language_string_char_type (exp->language_defn,
					      exp->gdbarch);
	    break;
	  case C_WIDE_STRING:
	    type = lookup_typename (exp->language_defn, exp->gdbarch,
				    "wchar_t", NULL, 0);
	    break;
	  case C_STRING_16:
	    type = lookup_typename (exp->language_defn, exp->gdbarch,
				    "char16_t", NULL, 0);
	    break;
	  case C_STRING_32:
	    type = lookup_typename (exp->language_defn, exp->gdbarch,
				    "char32_t", NULL, 0);
	    break;
	  default:
	    internal_error (__FILE__, __LINE__, "unhandled c_string_type");
	  }

	/* Ensure TYPE_LENGTH is valid for TYPE.  */
	check_typedef (type);

	dest_charset = charset_for_string_type (dest_type);

	++*pos;
	while (*pos < limit)
	  {
	    int len;

	    len = longest_to_int (exp->elts[*pos].longconst);

	    ++*pos;
	    if (noside != EVAL_SKIP)
	      parse_one_string (&output, &exp->elts[*pos].string, len,
				dest_charset, type);
	    *pos += BYTES_TO_EXP_ELEM (len);
	  }

	/* Skip the trailing length and opcode.  */
	*pos += 2;

	if (noside == EVAL_SKIP)
	  {
	    /* Return a dummy value of the appropriate type.  */
	    if ((dest_type & C_CHAR) != 0)
	      result = allocate_value (type);
	    else
	      result = value_cstring ("", 0, type);
	    do_cleanups (cleanup);
	    return result;
	  }

	if ((dest_type & C_CHAR) != 0)
	  {
	    LONGEST value;

	    if (obstack_object_size (&output) != TYPE_LENGTH (type))
	      error (_("Could not convert character constant to target character set"));
	    value = unpack_long (type, obstack_base (&output));
	    result = value_from_longest (type, value);
	  }
	else
	  {
	    int i;
	    /* Write the terminating character.  */
	    for (i = 0; i < TYPE_LENGTH (type); ++i)
	      obstack_1grow (&output, 0);
	    result = value_cstring (obstack_base (&output),
				    obstack_object_size (&output),
				    type);
	  }
	do_cleanups (cleanup);
	return result;
      }
      break;

    default:
      break;
    }
  return evaluate_subexp_standard (expect_type, exp, pos, noside);
}



/* Table mapping opcodes into strings for printing operators
   and precedences of the operators.  */

const struct op_print c_op_print_tab[] =
{
  {",", BINOP_COMMA, PREC_COMMA, 0},
  {"=", BINOP_ASSIGN, PREC_ASSIGN, 1},
  {"||", BINOP_LOGICAL_OR, PREC_LOGICAL_OR, 0},
  {"&&", BINOP_LOGICAL_AND, PREC_LOGICAL_AND, 0},
  {"|", BINOP_BITWISE_IOR, PREC_BITWISE_IOR, 0},
  {"^", BINOP_BITWISE_XOR, PREC_BITWISE_XOR, 0},
  {"&", BINOP_BITWISE_AND, PREC_BITWISE_AND, 0},
  {"==", BINOP_EQUAL, PREC_EQUAL, 0},
  {"!=", BINOP_NOTEQUAL, PREC_EQUAL, 0},
  {"<=", BINOP_LEQ, PREC_ORDER, 0},
  {">=", BINOP_GEQ, PREC_ORDER, 0},
  {">", BINOP_GTR, PREC_ORDER, 0},
  {"<", BINOP_LESS, PREC_ORDER, 0},
  {">>", BINOP_RSH, PREC_SHIFT, 0},
  {"<<", BINOP_LSH, PREC_SHIFT, 0},
  {"+", BINOP_ADD, PREC_ADD, 0},
  {"-", BINOP_SUB, PREC_ADD, 0},
  {"*", BINOP_MUL, PREC_MUL, 0},
  {"/", BINOP_DIV, PREC_MUL, 0},
  {"%", BINOP_REM, PREC_MUL, 0},
  {"@", BINOP_REPEAT, PREC_REPEAT, 0},
  {"-", UNOP_NEG, PREC_PREFIX, 0},
  {"!", UNOP_LOGICAL_NOT, PREC_PREFIX, 0},
  {"~", UNOP_COMPLEMENT, PREC_PREFIX, 0},
  {"*", UNOP_IND, PREC_PREFIX, 0},
  {"&", UNOP_ADDR, PREC_PREFIX, 0},
  {"sizeof ", UNOP_SIZEOF, PREC_PREFIX, 0},
  {"++", UNOP_PREINCREMENT, PREC_PREFIX, 0},
  {"--", UNOP_PREDECREMENT, PREC_PREFIX, 0},
  {NULL, 0, 0, 0}
};

enum c_primitive_types {
  c_primitive_type_int,
  c_primitive_type_long,
  c_primitive_type_short,
  c_primitive_type_char,
  c_primitive_type_float,
  c_primitive_type_double,
  c_primitive_type_void,
  c_primitive_type_long_long,
  c_primitive_type_signed_char,
  c_primitive_type_unsigned_char,
  c_primitive_type_unsigned_short,
  c_primitive_type_unsigned_int,
  c_primitive_type_unsigned_long,
  c_primitive_type_unsigned_long_long,
  c_primitive_type_long_double,
  c_primitive_type_complex,
  c_primitive_type_double_complex,
  c_primitive_type_decfloat,
  c_primitive_type_decdouble,
  c_primitive_type_declong,
  nr_c_primitive_types
};

void
c_language_arch_info (struct gdbarch *gdbarch,
		      struct language_arch_info *lai)
{
  const struct builtin_type *builtin = builtin_type (gdbarch);
  lai->string_char_type = builtin->builtin_char;
  lai->primitive_type_vector
    = GDBARCH_OBSTACK_CALLOC (gdbarch, nr_c_primitive_types + 1,
			      struct type *);
  lai->primitive_type_vector [c_primitive_type_int] = builtin->builtin_int;
  lai->primitive_type_vector [c_primitive_type_long] = builtin->builtin_long;
  lai->primitive_type_vector [c_primitive_type_short] = builtin->builtin_short;
  lai->primitive_type_vector [c_primitive_type_char] = builtin->builtin_char;
  lai->primitive_type_vector [c_primitive_type_float] = builtin->builtin_float;
  lai->primitive_type_vector [c_primitive_type_double] = builtin->builtin_double;
  lai->primitive_type_vector [c_primitive_type_void] = builtin->builtin_void;
  lai->primitive_type_vector [c_primitive_type_long_long] = builtin->builtin_long_long;
  lai->primitive_type_vector [c_primitive_type_signed_char] = builtin->builtin_signed_char;
  lai->primitive_type_vector [c_primitive_type_unsigned_char] = builtin->builtin_unsigned_char;
  lai->primitive_type_vector [c_primitive_type_unsigned_short] = builtin->builtin_unsigned_short;
  lai->primitive_type_vector [c_primitive_type_unsigned_int] = builtin->builtin_unsigned_int;
  lai->primitive_type_vector [c_primitive_type_unsigned_long] = builtin->builtin_unsigned_long;
  lai->primitive_type_vector [c_primitive_type_unsigned_long_long] = builtin->builtin_unsigned_long_long;
  lai->primitive_type_vector [c_primitive_type_long_double] = builtin->builtin_long_double;
  lai->primitive_type_vector [c_primitive_type_complex] = builtin->builtin_complex;
  lai->primitive_type_vector [c_primitive_type_double_complex] = builtin->builtin_double_complex;
  lai->primitive_type_vector [c_primitive_type_decfloat] = builtin->builtin_decfloat;
  lai->primitive_type_vector [c_primitive_type_decdouble] = builtin->builtin_decdouble;
  lai->primitive_type_vector [c_primitive_type_declong] = builtin->builtin_declong;

  lai->bool_type_default = builtin->builtin_int;
}

static const struct exp_descriptor exp_descriptor_c = 
{
  print_subexp_standard,
  operator_length_standard,
  op_name_standard,
  dump_subexp_body_standard,
  evaluate_subexp_c
};

const struct language_defn c_language_defn =
{
  "c",				/* Language name */
  language_c,
  range_check_off,
  type_check_off,
  case_sensitive_on,
  array_row_major,
  macro_expansion_c,
  &exp_descriptor_c,
  c_parse,
  c_error,
  null_post_parser,
  c_printchar,			/* Print a character constant */
  c_printstr,			/* Function to print string constant */
  c_emit_char,			/* Print a single char */
  c_print_type,			/* Print a type using appropriate syntax */
  c_print_typedef,		/* Print a typedef using appropriate syntax */
  c_val_print,			/* Print a value using appropriate syntax */
  c_value_print,		/* Print a top-level value */
  NULL,				/* Language specific skip_trampoline */
  NULL,				/* name_of_this */
  basic_lookup_symbol_nonlocal,	/* lookup_symbol_nonlocal */
  basic_lookup_transparent_type,/* lookup_transparent_type */
  NULL,				/* Language specific symbol demangler */
  NULL,				/* Language specific class_name_from_physname */
  c_op_print_tab,		/* expression operators for printing */
  1,				/* c-style arrays */
  0,				/* String lower bound */
  default_word_break_characters,
  default_make_symbol_completion_list,
  c_language_arch_info,
  default_print_array_index,
  default_pass_by_reference,
  c_get_string,
  LANG_MAGIC
};

enum cplus_primitive_types {
  cplus_primitive_type_int,
  cplus_primitive_type_long,
  cplus_primitive_type_short,
  cplus_primitive_type_char,
  cplus_primitive_type_float,
  cplus_primitive_type_double,
  cplus_primitive_type_void,
  cplus_primitive_type_long_long,
  cplus_primitive_type_signed_char,
  cplus_primitive_type_unsigned_char,
  cplus_primitive_type_unsigned_short,
  cplus_primitive_type_unsigned_int,
  cplus_primitive_type_unsigned_long,
  cplus_primitive_type_unsigned_long_long,
  cplus_primitive_type_long_double,
  cplus_primitive_type_complex,
  cplus_primitive_type_double_complex,
  cplus_primitive_type_bool,
  cplus_primitive_type_decfloat,
  cplus_primitive_type_decdouble,
  cplus_primitive_type_declong,
  nr_cplus_primitive_types
};

static void
cplus_language_arch_info (struct gdbarch *gdbarch,
			  struct language_arch_info *lai)
{
  const struct builtin_type *builtin = builtin_type (gdbarch);
  lai->string_char_type = builtin->builtin_char;
  lai->primitive_type_vector
    = GDBARCH_OBSTACK_CALLOC (gdbarch, nr_cplus_primitive_types + 1,
			      struct type *);
  lai->primitive_type_vector [cplus_primitive_type_int]
    = builtin->builtin_int;
  lai->primitive_type_vector [cplus_primitive_type_long]
    = builtin->builtin_long;
  lai->primitive_type_vector [cplus_primitive_type_short]
    = builtin->builtin_short;
  lai->primitive_type_vector [cplus_primitive_type_char]
    = builtin->builtin_char;
  lai->primitive_type_vector [cplus_primitive_type_float]
    = builtin->builtin_float;
  lai->primitive_type_vector [cplus_primitive_type_double]
    = builtin->builtin_double;
  lai->primitive_type_vector [cplus_primitive_type_void]
    = builtin->builtin_void;
  lai->primitive_type_vector [cplus_primitive_type_long_long]
    = builtin->builtin_long_long;
  lai->primitive_type_vector [cplus_primitive_type_signed_char]
    = builtin->builtin_signed_char;
  lai->primitive_type_vector [cplus_primitive_type_unsigned_char]
    = builtin->builtin_unsigned_char;
  lai->primitive_type_vector [cplus_primitive_type_unsigned_short]
    = builtin->builtin_unsigned_short;
  lai->primitive_type_vector [cplus_primitive_type_unsigned_int]
    = builtin->builtin_unsigned_int;
  lai->primitive_type_vector [cplus_primitive_type_unsigned_long]
    = builtin->builtin_unsigned_long;
  lai->primitive_type_vector [cplus_primitive_type_unsigned_long_long]
    = builtin->builtin_unsigned_long_long;
  lai->primitive_type_vector [cplus_primitive_type_long_double]
    = builtin->builtin_long_double;
  lai->primitive_type_vector [cplus_primitive_type_complex]
    = builtin->builtin_complex;
  lai->primitive_type_vector [cplus_primitive_type_double_complex]
    = builtin->builtin_double_complex;
  lai->primitive_type_vector [cplus_primitive_type_bool]
    = builtin->builtin_bool;
  lai->primitive_type_vector [cplus_primitive_type_decfloat]
    = builtin->builtin_decfloat;
  lai->primitive_type_vector [cplus_primitive_type_decdouble]
    = builtin->builtin_decdouble;
  lai->primitive_type_vector [cplus_primitive_type_declong]
    = builtin->builtin_declong;

  lai->bool_type_symbol = "bool";
  lai->bool_type_default = builtin->builtin_bool;
}

const struct language_defn cplus_language_defn =
{
  "c++",			/* Language name */
  language_cplus,
  range_check_off,
  type_check_off,
  case_sensitive_on,
  array_row_major,
  macro_expansion_c,
  &exp_descriptor_c,
  c_parse,
  c_error,
  null_post_parser,
  c_printchar,			/* Print a character constant */
  c_printstr,			/* Function to print string constant */
  c_emit_char,			/* Print a single char */
  c_print_type,			/* Print a type using appropriate syntax */
  c_print_typedef,		/* Print a typedef using appropriate syntax */
  c_val_print,			/* Print a value using appropriate syntax */
  c_value_print,		/* Print a top-level value */
  cplus_skip_trampoline,	/* Language specific skip_trampoline */
  "this",                       /* name_of_this */
  cp_lookup_symbol_nonlocal,	/* lookup_symbol_nonlocal */
  cp_lookup_transparent_type,   /* lookup_transparent_type */
  cplus_demangle,		/* Language specific symbol demangler */
  cp_class_name_from_physname,  /* Language specific class_name_from_physname */
  c_op_print_tab,		/* expression operators for printing */
  1,				/* c-style arrays */
  0,				/* String lower bound */
  default_word_break_characters,
  default_make_symbol_completion_list,
  cplus_language_arch_info,
  default_print_array_index,
  cp_pass_by_reference,
  c_get_string,
  LANG_MAGIC
};

const struct language_defn asm_language_defn =
{
  "asm",			/* Language name */
  language_asm,
  range_check_off,
  type_check_off,
  case_sensitive_on,
  array_row_major,
  macro_expansion_c,
  &exp_descriptor_c,
  c_parse,
  c_error,
  null_post_parser,
  c_printchar,			/* Print a character constant */
  c_printstr,			/* Function to print string constant */
  c_emit_char,			/* Print a single char */
  c_print_type,			/* Print a type using appropriate syntax */
  c_print_typedef,		/* Print a typedef using appropriate syntax */
  c_val_print,			/* Print a value using appropriate syntax */
  c_value_print,		/* Print a top-level value */
  NULL,				/* Language specific skip_trampoline */
  NULL,				/* name_of_this */
  basic_lookup_symbol_nonlocal,	/* lookup_symbol_nonlocal */
  basic_lookup_transparent_type,/* lookup_transparent_type */
  NULL,				/* Language specific symbol demangler */
  NULL,				/* Language specific class_name_from_physname */
  c_op_print_tab,		/* expression operators for printing */
  1,				/* c-style arrays */
  0,				/* String lower bound */
  default_word_break_characters,
  default_make_symbol_completion_list,
  c_language_arch_info, /* FIXME: la_language_arch_info.  */
  default_print_array_index,
  default_pass_by_reference,
  c_get_string,
  LANG_MAGIC
};

/* The following language_defn does not represent a real language.
   It just provides a minimal support a-la-C that should allow users
   to do some simple operations when debugging applications that use
   a language currently not supported by GDB.  */

const struct language_defn minimal_language_defn =
{
  "minimal",			/* Language name */
  language_minimal,
  range_check_off,
  type_check_off,
  case_sensitive_on,
  array_row_major,
  macro_expansion_c,
  &exp_descriptor_c,
  c_parse,
  c_error,
  null_post_parser,
  c_printchar,			/* Print a character constant */
  c_printstr,			/* Function to print string constant */
  c_emit_char,			/* Print a single char */
  c_print_type,			/* Print a type using appropriate syntax */
  c_print_typedef,		/* Print a typedef using appropriate syntax */
  c_val_print,			/* Print a value using appropriate syntax */
  c_value_print,		/* Print a top-level value */
  NULL,				/* Language specific skip_trampoline */
  NULL,				/* name_of_this */
  basic_lookup_symbol_nonlocal,	/* lookup_symbol_nonlocal */
  basic_lookup_transparent_type,/* lookup_transparent_type */
  NULL,				/* Language specific symbol demangler */
  NULL,				/* Language specific class_name_from_physname */
  c_op_print_tab,		/* expression operators for printing */
  1,				/* c-style arrays */
  0,				/* String lower bound */
  default_word_break_characters,
  default_make_symbol_completion_list,
  c_language_arch_info,
  default_print_array_index,
  default_pass_by_reference,
  c_get_string,
  LANG_MAGIC
};

void
_initialize_c_language (void)
{
  add_language (&c_language_defn);
  add_language (&cplus_language_defn);
  add_language (&asm_language_defn);
  add_language (&minimal_language_defn);
}
