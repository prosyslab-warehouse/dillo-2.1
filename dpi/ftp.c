/*
 * Dpi for FTP.
 *
 * This server checks the ftp-URL to be a directory (requires wget).
 * If true, it sends back an html representation of it, and if not
 * a dpip message (which is caught by dillo who redirects the ftp URL
 * to the downloads server).
 *
 * Feel free to polish!
 *
 * Copyright 2003-2007 Jorge Arellano Cid <jcid@dillo.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 */

/*
 * TODO:
 * - Send feedback about the FTP login process from wget's stderr.
 *   i.e. capture our child's stderr, process it, and report back.
 * - Handle simultaneous connections.
 *   If ftp.dpi is implemented with a low level ftp library, it becomes
 *   possible to keep the connection alive, and thus make browsing of ftp
 *   directories faster (this avoids one login per page, and forks). Perhaps
 *   it's not worth, but can be done.
 */

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>
#include <sys/time.h>
#include <ctype.h>

#include "../dpip/dpip.h"
#include "dpiutil.h"
#include "d_size.h"

/*
 * Debugging macros
 * (Set debugging messages to stderr, to see them)
 */
#define _MSG(...)
//#define MSG(...)  fprintf(stderr, "[ftp dpi]: " __VA_ARGS__)
#define MSG(...)  printf("[ftp dpi]: " __VA_ARGS__)

/*
 * Global variables
 */
static SockHandler *sh = NULL;
static char **dl_argv = NULL;

/*---------------------------------------------------------------------------*/

/* TODO: could use dStr ADT! */
typedef struct ContentType_ {
   const char *str;
   int len;
} ContentType_t;

static const ContentType_t MimeTypes[] = {
   { "application/octet-stream", 24 },
   { "text/html", 9 },
   { "text/plain", 10 },
   { "image/gif", 9 },
   { "image/png", 9 },
   { "image/jpeg", 10 },
   { NULL, 0 }
};

/*
 * Detects 'Content-Type' from a data stream sample.
 *
 * It uses the magic(5) logic from file(1). Currently, it
 * only checks the few mime types that Dillo supports.
 *
 * 'Data' is a pointer to the first bytes of the raw data.
 *
 * Return value: (0 on success, 1 on doubt, 2 on lack of data).
 */
static int a_Misc_get_content_type_from_data2(void *Data, size_t Size,
                                              const char **PT)
{
   int st = 1;      /* default to "doubt' */
   int Type = 0;    /* default to "application/octet-stream" */
   char *p = Data;
   uchar_t ch;
   size_t i, non_ascci;

   /* HTML try */
   for (i = 0; i < Size && isspace(p[i]); ++i);
   if ((Size - i >= 5  && !dStrncasecmp(p+i, "<html", 5)) ||
       (Size - i >= 5  && !dStrncasecmp(p+i, "<head", 5)) ||
       (Size - i >= 6  && !dStrncasecmp(p+i, "<title", 6)) ||
       (Size - i >= 14 && !dStrncasecmp(p+i, "<!doctype html", 14)) ||
       /* this line is workaround for FTP through the Squid proxy */
       (Size - i >= 17 && !dStrncasecmp(p+i, "<!-- HTML listing", 17))) {

      Type = 1;
      st = 0;
   /* Images */
   } else if (Size >= 4 && !dStrncasecmp(p, "GIF8", 4)) {
      Type = 3;
      st = 0;
   } else if (Size >= 4 && !dStrncasecmp(p, "\x89PNG", 4)) {
      Type = 4;
      st = 0;
   } else if (Size >= 2 && !dStrncasecmp(p, "\xff\xd8", 2)) {
      /* JPEG has the first 2 bytes set to 0xffd8 in BigEndian - looking
       * at the character representation should be machine independent. */
      Type = 5;
      st = 0;

   /* Text */
   } else {
      /* We'll assume "text/plain" if the set of chars above 127 is <= 10%
       * of the sample. This helps to catch ASCII, LATIN1 and UTF-8 as text.
       * Better heuristics are welcomed! :-) */
      non_ascci = 0;
      Size = MIN (Size, 256);
      for (i = 0; i < Size; i++) {
         ch = (uchar_t) p[i];
         if ((ch < 32 || ch > 126) && !isspace(ch))
            ++non_ascci;
      }
      if (Size == 256) {
         Type = (non_ascci > Size/10) ? 0 : 2;
         st = 0;
      } else {
         Type = (non_ascci > Size/10) ? 0 : 2;
      }
   }

   *PT = MimeTypes[Type].str;
   return st;
}

/*---------------------------------------------------------------------------*/

/*
 * Build a shell command using wget for this URL.
 */
static void make_wget_argv(char *url)
{
   char *esc_url;

   if (dl_argv) {
      dFree(dl_argv[2]);
      dFree(dl_argv);
   }
   dl_argv = dNew(char*, 10);

   esc_url = Escape_uri_str(url, "'");
   /* avoid malicious SMTP relaying with FTP urls */
   Filter_smtp_hack(esc_url);

   dl_argv[0] = "wget";
   dl_argv[1] = "-O-";
   dl_argv[2] = esc_url;
   dl_argv[3] = NULL;
}

/*
 * Fork, exec command, get its output and send via stdout.
 * Return: Number of bytes transfered.
 */
static int try_ftp_transfer(char *url)
{
#define MinSZ 256

   ssize_t n;
   int nb, has_mime_type, has_html_header;
   const char *mime_type = "application/octet-stream";
   char buf[4096], *d_cmd;
   Dstr *dbuf = dStr_sized_new(4096);
   pid_t ch_pid;
   int aborted = 0;
   int DataPipe[2];

   if (pipe(DataPipe) < 0) {
      MSG("pipe, %s\n", dStrerror(errno));
      return 0;
   }

   /* Prepare args for execvp() */
   make_wget_argv(url);

   /* Start the child process */
   if ((ch_pid = fork()) == 0) {
      /* child */
      /* start wget */
      close(DataPipe[0]);
      dup2(DataPipe[1], 1); /* stdout */
      execvp(dl_argv[0], dl_argv);
      _exit(1);
   } else if (ch_pid < 0) {
      perror("fork, ");
      exit(1);
   } else {
      /* father continues below */
      close(DataPipe[1]);
   }

   /* Read/Write the real data */
   nb = 0;
   has_mime_type = 0;
   has_html_header = 0;
   do {
      while ((n = read(DataPipe[0], buf, 4096)) < 0 && errno == EINTR);
      if (n > 0) {
         dStr_append_l(dbuf, buf, n);
         if (!has_mime_type && dbuf->len < MinSZ)
            continue;
      } else if (n < 0)
         break;

      if (!has_mime_type) {
         if (dbuf->len > 0)
            a_Misc_get_content_type_from_data2(dbuf->str,dbuf->len,&mime_type);
         has_mime_type = 1;

         if (strcmp(mime_type, "application/octet-stream") == 0) {
            /* abort transfer */
            kill(ch_pid, SIGTERM);
            /* The "application/octet-stream" MIME type will be sent and
             * Dillo will offer a download dialog */
            aborted = 1;
         }
      }

      if (!has_html_header && dbuf->len) {
         /* Send dpip tag */
         d_cmd = a_Dpip_build_cmd("cmd=%s url=%s", "start_send_page", url);
         sock_handler_write_str(sh, 1, d_cmd);
         dFree(d_cmd);

         /* Send HTTP header. */
         sock_handler_write_str(sh, 0, "Content-type: ");
         sock_handler_write_str(sh, 0, mime_type);
         sock_handler_write_str(sh, 1, "\n\n");
         has_html_header = 1;
      }

      if (!aborted && dbuf->len) {
         sock_handler_write(sh, 0, dbuf->str, dbuf->len);
         nb += dbuf->len;
         dStr_truncate(dbuf, 0);
      }
   } while (n > 0 && !aborted);

   return nb;
}

/*
 *
 */
int main(int argc, char **argv)
{
   char *dpip_tag = NULL, *cmd = NULL, *url = NULL, *url2 = NULL;
   int nb;
   char *p, *d_cmd;

   /* Debugging with a command line argument */
   if (argc == 2)
      dpip_tag = dStrdup(argv[1]);

   /* Initialize the SockHandler */
   sh = sock_handler_new(STDIN_FILENO, STDOUT_FILENO, 8*1024);

   /* wget may need to write a temporary file... */
   chdir("/tmp");

   /* Read the dpi command from STDIN */
   if (!dpip_tag)
      dpip_tag = sock_handler_read(sh);
   MSG("tag=[%s]\n", dpip_tag);

   cmd = a_Dpip_get_attr(dpip_tag, strlen(dpip_tag), "cmd");
   url = a_Dpip_get_attr(dpip_tag, strlen(dpip_tag), "url");
   if (!cmd || !url) {
      MSG("ERROR, cmd=%s, url=%s\n", cmd, url);
      exit (EXIT_FAILURE);
   }

   if ((nb = try_ftp_transfer(url)) == 0) {
      /* Transfer failed, the requested file may not exist or be a symlink
       * to a directory. Try again... */
      if ((p = strrchr(url, '/')) && p[1]) {
         url2 = dStrconcat(url, "/", NULL);
         nb = try_ftp_transfer(url2);
      }
   }

   if (nb == 0) {
      /* The transfer failed, let dillo know... */
      d_cmd = a_Dpip_build_cmd("cmd=%s to_cmd=%s msg=%s",
                               "answer", "open_url", "not a directory");
      sock_handler_write_str(sh, 1, d_cmd);
      dFree(d_cmd);
   }

   dFree(cmd);
   dFree(url);
   dFree(url2);
   dFree(dpip_tag);

   /* Finish the SockHandler */
   sock_handler_close(sh);
   sock_handler_free(sh);

   return 0;
}

