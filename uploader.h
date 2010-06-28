#ifndef UPLAODER_H
#define UPLAODER_H

/* Register a table that should be synchronized.  */
extern void uploader_table_register (const char *filename, const char *table,
				     bool delete_synchronized);

extern void *uploader_thread (void *arg);

#endif
