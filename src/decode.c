/*
 * File: decode.c
 *
 * Copyright 2007-2008 Jorge Arellano Cid <jcid@dillo.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#include <zlib.h>
#include <iconv.h>
#include <errno.h>
#include <stdlib.h>     /* strtol */

#include "decode.h"
#include "utf8.hh"
#include "msg.h"

static const int bufsize = 8*1024;

/*
 * Decode chunked data
 */
static Dstr *Decode_chunked(Decode *dc, const char *instr, int inlen)
{
   char *inputPtr, *eol;
   int inputRemaining;
   int chunkRemaining = *((int *)dc->state);
   Dstr *output = dStr_sized_new(inlen);

   dStr_append_l(dc->leftover, instr, inlen);
   inputPtr = dc->leftover->str;
   inputRemaining = dc->leftover->len;

   while (inputRemaining > 0) {
      if (chunkRemaining > 2) {
         /* chunk body to copy */
         int copylen = MIN(chunkRemaining - 2, inputRemaining);
         dStr_append_l(output, inputPtr, copylen);
         chunkRemaining -= copylen;
         inputRemaining -= copylen;
         inputPtr += copylen;
      }

      if ((chunkRemaining == 2) && (inputRemaining > 0)) {
         /* CR to discard */
         chunkRemaining--;
         inputRemaining--;
         inputPtr++;
      }
      if ((chunkRemaining == 1) && (inputRemaining > 0)) {
         /* LF to discard */
         chunkRemaining--;
         inputRemaining--;
         inputPtr++;
      }

      /*
       * A chunk has a one-line header that begins with the chunk length
       * in hexadecimal.
       */
      if (!(eol = (char *)memchr(inputPtr, '\n', inputRemaining))) {
         break;   /* We don't have the whole line yet. */
      }

      if (!(chunkRemaining = strtol(inputPtr, NULL, 0x10))) {
         break;   /* A chunk length of 0 means we're done! */
      }
      inputRemaining -= (eol - inputPtr) + 1;
      inputPtr = eol + 1;
      chunkRemaining += 2; /* CRLF at the end of every chunk */
   }

   /* If we have a partial chunk header, save it for next time. */
   dStr_erase(dc->leftover, 0, inputPtr - dc->leftover->str);

   *(int *)dc->state = chunkRemaining;
   return output;
}

static void Decode_chunked_free(Decode *dc)
{
   dFree(dc->state);
   dStr_free(dc->leftover, 1);
}

/*
 * Decode gzipped data
 */
static Dstr *Decode_gzip(Decode *dc, const char *instr, int inlen)
{
   int rc = Z_OK;

   z_stream *zs = (z_stream *)dc->state;

   int inputConsumed = 0;
   Dstr *output = dStr_new("");

   while ((rc == Z_OK) && (inputConsumed < inlen)) {
      zs->next_in = (Bytef *)instr + inputConsumed;
      zs->avail_in = inlen - inputConsumed;

      zs->next_out = (Bytef *)dc->buffer;
      zs->avail_out = bufsize;

      rc = inflate(zs, Z_SYNC_FLUSH);

      dStr_append_l(output, dc->buffer, zs->total_out);

      if ((rc == Z_OK) || (rc == Z_STREAM_END)) {
         // Z_STREAM_END at end of file

         inputConsumed += zs->total_in;
         zs->total_out = 0;
         zs->total_in = 0;
      } else if (rc == Z_DATA_ERROR) {
         MSG_ERR("gzip decompression error\n");
      }
   }
   return output;
}

static void Decode_gzip_free(Decode *dc)
{
   (void)inflateEnd((z_stream *)dc->state);

   dFree(dc->state);
   dFree(dc->buffer);
}

/*
 * Translate to desired character set (UTF-8)
 */
static Dstr *Decode_charset(Decode *dc, const char *instr, int inlen)
{
   inbuf_t *inPtr;
   char *outPtr;
   size_t inLeft, outRoom;

   Dstr *output = dStr_new("");
   int rc = 0;

   dStr_append_l(dc->leftover, instr, inlen);
   inPtr = dc->leftover->str;
   inLeft = dc->leftover->len;

   while ((rc != EINVAL) && (inLeft > 0)) {

      outPtr = dc->buffer;
      outRoom = bufsize;

      rc = iconv((iconv_t)dc->state, &inPtr, &inLeft, &outPtr, &outRoom);

      // iconv() on success, number of bytes converted
      //         -1, errno == EILSEQ illegal byte sequence found
      //                      EINVAL partial character ends source buffer
      //                      E2BIG  destination buffer is full

      dStr_append_l(output, dc->buffer, bufsize - outRoom);

      if (rc == -1)
         rc = errno;
      if (rc == EILSEQ){
         inPtr++;
         inLeft--;
         dStr_append_l(output, utf8_replacement_char,
                       sizeof(utf8_replacement_char) - 1);
      }
   }
   dStr_erase(dc->leftover, 0, dc->leftover->len - inLeft);

   return output;
}

static void Decode_charset_free(Decode *dc)
{
   /* iconv_close() frees dc->state */
   (void)iconv_close((iconv_t)(dc->state));

   dFree(dc->buffer);
   dStr_free(dc->leftover, 1);
}

/*
 * Initialize transfer decoder. Currently handles "chunked".
 */
Decode *a_Decode_transfer_init(const char *format)
{
   Decode *dc = NULL;

   if (format && !dStrncasecmp(format, "chunked", 7)) {
      int *chunk_remaining = dNew(int, 1);
      *chunk_remaining = 0;
      dc = dNew(Decode, 1);
      dc->leftover = dStr_new("");
      dc->state = chunk_remaining;
      dc->decode = Decode_chunked;
      dc->free = Decode_chunked_free;
      dc->buffer = NULL; /* not used */
      _MSG("chunked!\n");
   }
   return dc;
}

/*
 * Initialize content decoder. Currently handles gzip.
 *
 * zlib is also capable of handling "deflate"/zlib-encoded data, but web
 * servers have not standardized on whether to send such data with a header.
 */
Decode *a_Decode_content_init(const char *format)
{
   Decode *dc = NULL;

   if (format && *format) {
      if (!dStrcasecmp(format, "gzip") || !dStrcasecmp(format, "x-gzip")) {
         z_stream *zs;
         _MSG("gzipped data!\n");

         dc = dNew(Decode, 1);
         dc->buffer = dNew(char, bufsize);
         dc->state = zs = dNew(z_stream, 1);
         zs->zalloc = NULL;
         zs->zfree = NULL;
         zs->next_in = NULL;
         zs->avail_in = 0;

         /* 16 is a magic number for gzip decoding */
         inflateInit2(zs, MAX_WBITS+16);

         dc->decode = Decode_gzip;
         dc->free = Decode_gzip_free;
         dc->leftover = NULL; /* not used */
      } else {
         MSG("Content-Encoding '%s' not recognized.\n", format);
      }
   }
   return dc;
}

/*
 * Legal names for the ASCII character set
 */
static int Decode_is_ascii(const char *str)
{
   return (!(dStrcasecmp(str, "ASCII") &&
             dStrcasecmp(str, "US-ASCII") &&
             dStrcasecmp(str, "us") &&
             dStrcasecmp(str, "IBM367") &&
             dStrcasecmp(str, "cp367") &&
             dStrcasecmp(str, "csASCII") &&
             dStrcasecmp(str, "ANSI_X3.4-1968") &&
             dStrcasecmp(str, "iso-ir-6") &&
             dStrcasecmp(str, "ANSI_X3.4-1986") &&
             dStrcasecmp(str, "ISO_646.irv:1991") &&
             dStrcasecmp(str, "ISO646-US")));
}

/*
 * Initialize decoder to translate from any character set known to iconv()
 * to UTF-8.
 *
 * GNU iconv(1) will provide a list of known character sets if invoked with
 * the "--list" flag.
 */
Decode *a_Decode_charset_init(const char *format)
{
   Decode *dc = NULL;

   if (format &&
       strlen(format) &&
       dStrcasecmp(format,"UTF-8") &&
       !Decode_is_ascii(format)) {

      iconv_t ic = iconv_open("UTF-8", format);
      if (ic != (iconv_t) -1) {
           dc = dNew(Decode, 1);
           dc->state = ic;
           dc->buffer = dNew(char, bufsize);
           dc->leftover = dStr_new("");

           dc->decode = Decode_charset;
           dc->free = Decode_charset_free;
      } else {
         MSG("Unable to convert from character encoding: '%s'\n", format);
      }
   }
   return dc;
}

/*
 * Decode data.
 */
Dstr *a_Decode_process(Decode *dc, const char *instr, int inlen)
{
   return dc->decode(dc, instr, inlen);
}

/*
 * Free the decoder.
 */
void a_Decode_free(Decode *dc)
{
   if (dc) {
      dc->free(dc);
      dFree(dc);
   }
}
