/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Richard P. Curnow  1997-2003
 * Copyright (C) Miroslav Lichvar  2011-2014, 2018-2020
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 * 
 **********************************************************************

  =======================================================================

  Module to handle logging of diagnostic information
  */

#include "config.h"

#include "sysincl.h"

#include <syslog.h>

#include "conf.h"
#include "logging.h"
#include "memory.h"
#include "util.h"

/* This is used by DEBUG_LOG macro */
LOG_Severity log_min_severity = LOGS_INFO;

/* ================================================== */
/* Flag indicating we have initialised */
static int initialised = 0;

static FILE *file_log = NULL;
static int system_log = 0;

static int parent_fd = 0;

struct LogFile {
  const char *name;
  const char *banner;
  FILE *file;
  unsigned long writes;
};

static int n_filelogs = 0;

/* Increase this when adding a new logfile */
#define MAX_FILELOGS 6

static struct LogFile logfiles[MAX_FILELOGS];

/* Global prefix for debug messages */
static char *debug_prefix;

/* ================================================== */
/* Init function */

void
LOG_Initialise(void)
{
  debug_prefix = Strdup("");
  initialised = 1;
  LOG_OpenFileLog(NULL);
}

/* ================================================== */
/* Fini function */

void
LOG_Finalise(void)
{
  if (system_log)
    closelog();

  if (file_log)
    fclose(file_log);

  LOG_CycleLogFiles();

  Free(debug_prefix);

  initialised = 0;
}

/* ================================================== */

static void log_message(int fatal, LOG_Severity severity, const char *message)
{
  if (system_log) {
    int priority;
    switch (severity) {
      case LOGS_DEBUG:
        priority = LOG_DEBUG;
        break;
      case LOGS_INFO:
        priority = LOG_INFO;
        break;
      case LOGS_WARN:
        priority = LOG_WARNING;
        break;
      case LOGS_ERR:
        priority = LOG_ERR;
        break;
      case LOGS_FATAL:
        priority = LOG_CRIT;
        break;
      default:
        assert(0);
    }
    syslog(priority, fatal ? "Fatal error : %s" : "%s", message);
  } else if (file_log) {
    fprintf(file_log, fatal ? "Fatal error : %s\n" : "%s\n", message);
  }
}

/* ================================================== */

void LOG_Message(LOG_Severity severity,
#if DEBUG > 0
                 int line_number, const char *filename, const char *function_name,
#endif
                 const char *format, ...)
{
  char buf[2048];
  va_list other_args;
  time_t t;
  struct tm *tm;

  assert(initialised);

  if (!system_log && file_log && severity >= log_min_severity) {
    /* Don't clutter up syslog with timestamps and internal debugging info */
    time(&t);
    tm = gmtime(&t);
    if (tm) {
      strftime(buf, sizeof (buf), "%Y-%m-%dT%H:%M:%SZ", tm);
      fprintf(file_log, "%s ", buf);
    }
#if DEBUG > 0
    if (log_min_severity <= LOGS_DEBUG)
      fprintf(file_log, "%s%s:%d:(%s) ", debug_prefix, filename, line_number, function_name);
#endif
  }

  va_start(other_args, format);
  vsnprintf(buf, sizeof(buf), format, other_args);
  va_end(other_args);

  switch (severity) {
    case LOGS_DEBUG:
    case LOGS_INFO:
    case LOGS_WARN:
    case LOGS_ERR:
      if (severity >= log_min_severity)
        log_message(0, severity, buf);
      break;
    case LOGS_FATAL:
      if (severity >= log_min_severity)
        log_message(1, severity, buf);

      /* Send the message also to the foreground process if it is
         still running, or stderr if it is still open */
      if (parent_fd > 0) {
        if (write(parent_fd, buf, strlen(buf) + 1) < 0)
          ; /* Not much we can do here */
      } else if (system_log && parent_fd == 0) {
        system_log = 0;
        log_message(1, severity, buf);
      }
      exit(1);
      break;
    default:
      assert(0);
  }
}

/* ================================================== */

void
LOG_OpenFileLog(const char *log_file)
{
  FILE *f;

  if (log_file) {
    f = UTI_OpenFile(NULL, log_file, NULL, 'A', 0640);
  } else {
    f = stderr;
  }

  /* Enable line buffering */
  setvbuf(f, NULL, _IOLBF, BUFSIZ);

  if (file_log && file_log != stderr)
    fclose(file_log);

  file_log = f;
}


/* ================================================== */

void
LOG_OpenSystemLog(void)
{
  system_log = 1;
  openlog("chronyd", LOG_PID, LOG_DAEMON);
}

/* ================================================== */

void LOG_SetMinSeverity(LOG_Severity severity)
{
  /* Don't print any debug messages in a non-debug build */
  log_min_severity = CLAMP(DEBUG > 0 ? LOGS_DEBUG : LOGS_INFO, severity, LOGS_FATAL);
}

/* ================================================== */

LOG_Severity
LOG_GetMinSeverity(void)
{
  return log_min_severity;
}

/* ================================================== */

void
LOG_SetDebugPrefix(const char *prefix)
{
  Free(debug_prefix);
  debug_prefix = Strdup(prefix);
}

/* ================================================== */

void
LOG_SetParentFd(int fd)
{
  parent_fd = fd;
  if (file_log == stderr)
    file_log = NULL;
}

/* ================================================== */

void
LOG_CloseParentFd()
{
  if (parent_fd > 0)
    close(parent_fd);
  parent_fd = -1;
}

/* ================================================== */

LOG_FileID
LOG_FileOpen(const char *name, const char *banner)
{
  if (n_filelogs >= MAX_FILELOGS) {
    assert(0);
    return -1;
  }

  logfiles[n_filelogs].name = name;
  logfiles[n_filelogs].banner = banner;
  logfiles[n_filelogs].file = NULL;
  logfiles[n_filelogs].writes = 0;

  return n_filelogs++;
}

/* ================================================== */

void
LOG_FileWrite(LOG_FileID id, const char *format, ...)
{
  va_list other_args;
  int banner;

  if (id < 0 || id >= n_filelogs || !logfiles[id].name)
    return;

  if (!logfiles[id].file) {
    char *logdir = CNF_GetLogDir();

    if (!logdir) {
      LOG(LOGS_WARN, "logdir not specified");
      logfiles[id].name = NULL;
      return;
    }

    logfiles[id].file = UTI_OpenFile(logdir, logfiles[id].name, ".log", 'a', 0644);
    if (!logfiles[id].file) {
      /* Disable the log */
      logfiles[id].name = NULL;
      return;
    }
  }

  banner = CNF_GetLogBanner();
  if (banner && logfiles[id].writes++ % banner == 0) {
    char bannerline[256];
    int i, bannerlen;

    bannerlen = MIN(strlen(logfiles[id].banner), sizeof (bannerline) - 1);

    for (i = 0; i < bannerlen; i++)
      bannerline[i] = '=';
    bannerline[i] = '\0';

    fprintf(logfiles[id].file, "%s\n", bannerline);
    fprintf(logfiles[id].file, "%s\n", logfiles[id].banner);
    fprintf(logfiles[id].file, "%s\n", bannerline);
  }

  va_start(other_args, format);
  vfprintf(logfiles[id].file, format, other_args);
  va_end(other_args);
  fprintf(logfiles[id].file, "\n");

  fflush(logfiles[id].file);
}

/* ================================================== */

void
LOG_CycleLogFiles(void)
{
  LOG_FileID i;

  for (i = 0; i < n_filelogs; i++) {
    if (logfiles[i].file)
      fclose(logfiles[i].file);
    logfiles[i].file = NULL;
    logfiles[i].writes = 0;
  }
}

/* ================================================== */
