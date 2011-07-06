/* Utility functions for writing WARC files. */
#define _GNU_SOURCE

#include "wget.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <uuid/uuid.h>

#include "wget_warc.h"

extern char *version_string;

/* Set by main in main.c */
extern char *program_argstring;


/* The current WARC file (or NULL, if WARC is disabled).
   This is a pointer to a WFile object. */
static void *warc_current_wfile;

/* The record id of the warcinfo record of the current WARC file.  */
static char *warc_current_winfo_uuid_str;

/* The serial number of the current WARC file.  This number is
   incremented each time a new file is opened and is used in the
   WARC file's filename. */
static int  warc_current_file_number;


/* Helper functions to fill WRecord objects.
   The warctools library uses its own types for strings and integers.  */
#define WARC_WRAP_METHOD(name)                                          \
    bool warc_##name(void *record, char *u8_string)                     \
    {                                                                   \
      warc_u8_t *wu8_string = (warc_u8_t *)u8_string;                   \
      return WRecord_##name(record, wu8_string, w_strlen(wu8_string));  \
    }

WARC_WRAP_METHOD (setTargetUri)
WARC_WRAP_METHOD (setContentType)
WARC_WRAP_METHOD (setDate)
WARC_WRAP_METHOD (setRecordId)
WARC_WRAP_METHOD (setFilename)
WARC_WRAP_METHOD (setConcurrentTo)
WARC_WRAP_METHOD (setContentFromString)
WARC_WRAP_METHOD (setWarcInfoId)

bool
warc_setContentFromFileName (void *record, char *u8_filename)
{
  return WRecord_setContentFromFileName (record, u8_filename);
}

/* Use the contents of file as the body of the WARC record.
   Note: calling  destroy (record)  will also close the file. */
bool
warc_setContentFromFile (void *record, FILE *file)
{
  return WRecord_setContentFromFile (record, file);
}

bool
warc_setRecordType (void *record, const warc_rec_t t)
{
  return WRecord_setRecordType (record, t);
}


/* Fills timestamp with the current time and date.
   The UTC time is formatted following ISO 8601, as required
   for use in the WARC-Date header.
   The timestamp will be 21 characters long. */
void
warc_timestamp (char *timestamp)
{
  time_t rawtime;
  struct tm * timeinfo;
  time ( &rawtime );
  timeinfo = gmtime (&rawtime);
  strftime (timestamp, 21, "%Y-%m-%dT%H:%M:%SZ", timeinfo);
}

/* Fills urn_str with a UUID in the format required
   for the WARC-Record-Id header.
   The string will be 47 characters long. */
void
warc_uuid_str (char *urn_str)
{
  uuid_t record_id;
  uuid_generate (record_id);
  char uuid_str[37];
  uuid_unparse (record_id, uuid_str);
  sprintf(urn_str, "<urn:uuid:%s>", uuid_str);
}

/* Opens a new WARC file.
   
   This method will:
   1. close the current WARC file (if there is one);
   2. increment warc_current_file_number;
   3. open a new WARC file;
   4. write the initial warcinfo record.

   Returns true on success, false otherwise.
   */
bool
warc_start_new_file ()
{
  if (opt.warc_filename == NULL)
    return false;

  if (warc_current_wfile != NULL)
    destroy (warc_current_wfile);
  if (warc_current_winfo_uuid_str)
    free (warc_current_winfo_uuid_str);

  warc_current_file_number++;

  int base_filename_length = strlen (opt.warc_filename);
  /* filename format:  base + "-" + 5 digit serial number + ".warc.gz" */
  char *new_filename = alloca (base_filename_length + 1 + 5 + 8 + 1);

  char *extension = (opt.warc_compression_enabled ? "warc.gz" : "warc");

  /* If max size is enabled, we add a serial number to the file names. */
  if (opt.warc_maxsize > 0)
    sprintf (new_filename, "%s-%05d.%s", opt.warc_filename, warc_current_file_number, extension);
  else
    sprintf (new_filename, "%s.%s", opt.warc_filename, extension);

  wfile_comp_t compression = (opt.warc_compression_enabled ? WARC_FILE_COMPRESSED_GZIP_BEST_COMPRESSION : WARC_FILE_UNCOMPRESSED);

  warc_current_wfile = bless (WFile, new_filename, opt.warc_maxsize, WARC_FILE_WRITER, compression, ".");
  if (warc_current_wfile == NULL)
    {
      fprintf (stderr, "Error opening WARC file.\n");
      return false;
    }

  /* Write warc-info record as the first record of the file. */
  /* We add the record id of this info record to the other records in the file. */
  warc_current_winfo_uuid_str = (char *) malloc (48);
  warc_uuid_str (warc_current_winfo_uuid_str);

  char warc_timestamp_str [21];
  warc_timestamp (warc_timestamp_str);

  char *new_filename_copy, *new_filename_basename;
  new_filename_copy = strdup (new_filename);
  new_filename_basename = basename (new_filename_copy);

  void * infoWRecord = bless (WRecord);
  warc_setRecordType (infoWRecord, WARC_INFO_RECORD);
  warc_setContentType (infoWRecord, "application/warc-fields");
  warc_setDate (infoWRecord, warc_timestamp_str);
  warc_setRecordId (infoWRecord, warc_current_winfo_uuid_str);
  warc_setFilename (infoWRecord, new_filename_basename);

  char winfo_header_string [400]; /* lazy */
  sprintf (winfo_header_string, "software: Wget/%s (%s)\r\nformat: WARC File Format 1.0\r\nconformsTo: http://bibnum.bnf.fr/WARC/WARC_ISO_28500_version1_latestdraft.pdf\r\nrobots: %s\r\nwget-arguments: %s\r\n\r\n", version_string, OS_TYPE, (opt.use_robots ? "classic" : "off"), program_argstring);
  warc_setContentFromString (infoWRecord, winfo_header_string);

  /* Returns true on error. */
  if ( WFile_storeRecord (warc_current_wfile, infoWRecord) )
    {
      fprintf(stderr, "Error writing winfo record to WARC file.\n");
      destroy (infoWRecord);
      free (new_filename_copy);
      return false;
    }

  destroy (infoWRecord);
  free (new_filename_copy);

  return true;
}

/* Writes the record (a WRecord pointer) to the current WARC file.
   If the WARC file is full, the function will open a new file.
   Returns true if the writing was successful, false otherwise. */
bool
warc_store_record (void * record)
{
  if (warc_current_wfile != 0)
    {
      /* If the WARC file is full, start a new file. */
      if ( WFile_isFull (warc_current_wfile) )
        {
          if (! warc_start_new_file ())
            {
              fprintf(stderr, "Could not open new WARC file.\n");
              return false;
            }
        }

      /* Point to the current info record. */
      warc_setWarcInfoId (record, warc_current_winfo_uuid_str);

      /* This will return true if writing failed. */
      if ( WFile_storeRecord (warc_current_wfile, record) )
        {
          fprintf(stderr, "Error writing record to WARC file.\n");
          return false;
        }

      return true;
    }
  else
    {
      fprintf(stderr, "Called warc_store_record without open WFile.\n");
      return false;
    }
}

/* Initializes the WARC writer (if opt.warc_filename is set).
   This should be called before any WARC record is written. */
void
warc_init ()
{
  if (opt.warc_filename != NULL)
    {
      warc_current_file_number = -1;
      if (! warc_start_new_file ())
        {
          fprintf(stderr, "Could not open WARC file.\n");
          opt.warc_filename = 0;
        }
    }
}

/* Finishes the WARC writing.
   This should be called at the end of the program. */
void
warc_close ()
{
  if (warc_current_wfile != NULL)
    {
      free (warc_current_winfo_uuid_str);
      destroy (warc_current_wfile);
    }
}

/* Creates a temporary file for writing WARC output.
   The temporary file will be created in opt.warc_tempdir.
   Returns the pointer to the temporary file, or NULL. */
FILE *
warc_tempfile ()
{
  if (opt.warc_tempdir == NULL)
    return NULL;

  int dirlen = strlen (opt.warc_tempdir);
  char *filename = alloca (dirlen + 18);
  sprintf (filename, "%s/.wget_warc_XXXXXX", opt.warc_tempdir);

  int fd = mkstemp (filename);
  if (fd < 0)
    return NULL;

  if (unlink (filename) < 0)
    return NULL;

  return fdopen (fd, "wb+");
  /* TODO check for errors */
}

