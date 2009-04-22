
/*
 * libjio - A library for Journaled I/O
 * Alberto Bertogli (albertito@blitiri.com.ar)
 */

#ifndef _LIBJIO_H
#define _LIBJIO_H

#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <pthread.h>
#include <unistd.h>

/* Check if we're using Large File Support - otherwise refuse to build.
 * Otherwise, we would allow applications not using LFS to link with the
 * library (which uses LFS) and that's just begging for problems. There should
 * be a portable way for the C library to do some of this for us, but until I
 * find one, this is the best we can do */
#ifndef _LARGEFILE_SOURCE
#error "You must compile your application with Large File Support"
#endif

/*
 * Opaque types, the API does not expose these
 */

/** An open file, similar to a file descriptor */
typedef struct jfs jfs_t;

/** A single transaction */
typedef struct jtrans jtrans_t;


/*
 * Public types
 */

/** The result of a jfsck() run
 * @ingroup check
 */
struct jfsck_result {
	/** Total transactions files processed */
	int total;

	/** Number of invalid transactions */
	int invalid;

	/** Number of transactions in progress */
	int in_progress;

	/** Number of broken transactions */
	int broken;

	/** Number of corrupt transactions */
	int corrupt;

	/** Number of errors applying transactions */
	int apply_error;

	/** Number of transactions successfully reapplied */
	int reapplied;
};


/*
 * Core functions
 */

/** Open a file.
 *
 * Takes the same parameters as the UNIX open(2), with an additional one for
 * internal flags.
 *
 * The only supported internal flag is J_LINGER, which enables lingering
 * transactions.
 *
 * @param name path to the file to open
 * @param flags flags to pass to open(2)
 * @param mode mode to pass to open(2)
 * @param jflags journal flags
 * @returns a new jfs_t that identifies the open file on success, or NULL on
 *	error
 * @see jclose(), open()
 * @ingroup basic
 */
jfs_t *jopen(const char *name, int flags, int mode, int jflags);

/** Close a file opened with jopen().
 *
 * After a call to this function, the memory allocated for the open file will
 * be freed.
 *
 * If there was an autosync thread started for this file, it will be stopped.
 *
 * @param fs open file
 * @returns 0 on success, -1 on error
 * @see jopen(), jfs_autosync_start()
 * @ingroup basic
 */
int jclose(jfs_t *fs);

/** Sync a file. Makes sense only when using lingering transactions.
 *
 * @param fs open file
 * @returns 0 on success, -1 on error
 * @ingroup basic
 */
int jsync(jfs_t *fs);

/** Create a new transaction.
 *
 * @param fs open file the transaction will apply to
 * @returns a new transaction (must be freed using jtrans_free())
 * @see jtrans_free()
 * @ingroup basic
 */
jtrans_t *jtrans_init(jfs_t *fs);

/** Add an operation to a transaction.
 *
 * An operation consists of a buffer, its length, and the offset to write it
 * to.
 *
 * The file will not be touched (not even locked) until commit time, where the
 * first count bytes of buf will be written at offset.
 *
 * Transactions will be applied in order, and overlapping operations are
 * permitted, in which case the latest one will prevail.
 *
 * @param ts transaction
 * @param buf buffer to write
 * @param count how many bytes from the buffer to write
 * @param offset offset to write at
 * @returns 0 on success, -1 on error
 * @ingroup basic
 */
int jtrans_add(jtrans_t *ts, const void *buf, size_t count, off_t offset);

/** Commit a transaction.
 * 
 * All the operations added to it using jtrans_add() will be written to disk,
 * in the same order they were added.
 *
 * After this function returns successfully, all the data can be trusted to be
 * on the disk. The commit is atomic with regards to other processes using
 * libjio, but not accessing directly to the file.
 *
 * @param ts transaction
 * @returns the amount of bytes written to disk, or -1 if there was an error
 *	but atomic warranties were preserved, or -2 if there was an error and
 *	there is a possible break of atomic warranties (which is an indication
 *	of a severe underlying condition).
 * @ingroup basic
 */
ssize_t jtrans_commit(jtrans_t *ts);

/** Rollback a transaction.
 *
 * This function atomically undoes a previous committed transaction. After its
 * successful return, the data can be trusted to be on disk.
 *
 * Use with care.
 *
 * @param ts transaction
 * @returns the same as jtrans_commit()
 * @see jtrans_commit()
 * @ingroup basic
 */
ssize_t jtrans_rollback(jtrans_t *ts);

/** Free a transaction structure.
 *
 * @param ts transaction to free
 * @see jtrans_init()
 * @ingroup basic
 */
void jtrans_free(jtrans_t *ts);

/** Change the location of the journal directory.
 *
 * The file MUST NOT be in use by any other thread or process. The older
 * journal directory will be removed.
 *
 * @param fs open file
 * @param newpath path to the new journal directory, which will be created if
 * 	it doesn't exist
 * @returns 0 on success, -1 on error
 * @ingroup basic
 */
int jmove_journal(jfs_t *fs, const char *newpath);


/*
 * Autosync
 */

/** Start an autosync thread.
 *
 * The thread will call jsync(fs) every max_sec seconds, or every max_bytes
 * have been written. Only one autosync thread per open file is allowed.
 *
 * @param fs open file
 * @param max_sec maximum number of seconds that should pass between each
 * 	call to jsync()
 * @param max_bytes maximum number of bytes that should be written between
 *	each call to jsync()
 * @returns 0 on success, -1 on error
 * @ingroup basic
 */
int jfs_autosync_start(jfs_t *fs, time_t max_sec, size_t max_bytes);

/** Stop an autosync thread that was started using jfs_autosync_start(fs).
 * 
 * @param fs open file
 * @returns 0 on success, -1 on error
 * @ingroup basic
 */
int jfs_autosync_stop(jfs_t *fs);


/*
 * Journal checker
 */

/** Check and repair the given path.
 *
 * @param name path to the file to check
 * @param jdir journal directory of the given file, use NULL for the default
 * @param res structure where to store the result
 * @see struct jfsck_result, jfsck_cleanup()
 * @return 0 on success, < 0 on error, with the following possible negative
 * 	values: J_ENOENT if there was no such file with the given name,
 * 	J_ENOJOURNAL if there was no journal at the given jdir, J_ENOMEM if
 * 	memory could not be allocated.
 * @ingroup check
 */
int jfsck(const char *name, const char *jdir, struct jfsck_result *res);

/** Remove all the files in the journal directory, and clean it up.
 * TODO: this should be merged with jfsck() and removed */
int jfsck_cleanup(const char *name, const char *jdir);


/*
 * UNIX API wrappers
 */

/** Read from the file. Works just like UNIX read(2).
 *
 * @param fs file to read from
 * @param buf buffer used to store the data
 * @param count maximum number of bytes to read
 * @returns number of bytes read on success, or -1 on error
 * @see read(2)
 * @ingroup unix
 */
ssize_t jread(jfs_t *fs, void *buf, size_t count);

/** Read from the file at the given offset. Works just like UNIX pread(2).
 *
 * @param fs file to read from
 * @param buf buffer used to store the data
 * @param count maximum number of bytes to read
 * @param offset offset to read at
 * @returns number of bytes read on success, or -1 on error
 * @see pread(2)
 * @ingroup unix
 */
ssize_t jpread(jfs_t *fs, void *buf, size_t count, off_t offset);

/** Read from the file into multiple buffers. Works just like UNIX readv(2).
 *
 * @param fs file to read from
 * @param vector buffers used to store the data
 * @param count maximum number of bytes to read
 * @returns number of bytes read on success, or -1 on error
 * @see readv(2)
 * @ingroup unix
 */
ssize_t jreadv(jfs_t *fs, const struct iovec *vector, int count);

/** Write to the file. Works just like UNIX write(2).
 *
 * @param fs file to write to
 * @param buf buffer used to read the data from
 * @param count maximum number of bytes to write
 * @returns number of bytes written on success, or -1 on error
 * @see write(2)
 * @ingroup unix
 */
ssize_t jwrite(jfs_t *fs, const void *buf, size_t count);

/** Write to the file at the given offset. Works just like UNIX pwrite(2).
 *
 * @param fs file to write to
 * @param buf buffer used to read the data from
 * @param count maximum number of bytes to write
 * @param offset offset to write at
 * @returns number of bytes written on success, or -1 on error
 * @see pwrite(2)
 * @ingroup unix
 */
ssize_t jpwrite(jfs_t *fs, const void *buf, size_t count, off_t offset);

/** Write to the file from multiple buffers. Works just like UNIX writev(2).
 *
 * @param fs file to write to
 * @param vector buffers used to read the data from
 * @param count maximum number of bytes to write
 * @returns number of bytes written on success, or -1 on error
 * @see writev(2)
 * @ingroup unix
 */
ssize_t jwritev(jfs_t *fs, const struct iovec *vector, int count);

/** Truncates the file to the given length. Works just like UNIX ftruncate(2).
 *
 * @param fs file to truncate
 * @param length lenght to truncate to
 * @returns 0 on success, -1 on error
 * @see ftruncate(2)
 * @ingroup unix
 */
int jtruncate(jfs_t *fs, off_t length);

/** Reposition read/write file offset. Works just like UNIX lseek(2).
 *
 * @param fs file to change the offset to
 * @param offset offset to set
 * @param whence where to count offset from, can be SEEK_SET to count from the
 *	beginning of the file, SEEK_CUR to count from the current position, or
 *	SEEK_END to count from the end.
 * @returns the new offset counted from the beginning of the file, or -1 on
 *	error.
 * @ingroup unix
 */
off_t jlseek(jfs_t *fs, off_t offset, int whence);


/*
 * ANSI C stdio wrappers
 */

jfs_t *jfopen(const char *path, const char *mode);

int jfclose(jfs_t *stream);

jfs_t *jfreopen(const char *path, const char *mode, jfs_t *stream);

size_t jfread(void *ptr, size_t size, size_t nmemb, jfs_t *stream);

size_t jfwrite(const void *ptr, size_t size, size_t nmemb, jfs_t *stream);

int jfileno(jfs_t *stream);

int jfeof(jfs_t *stream);

void jclearerr(jfs_t *stream);

int jferror(jfs_t *stream);

int jfseek(jfs_t *stream, long offset, int whence);

long jftell(jfs_t *stream);

void jrewind(jfs_t *stream);

FILE *jfsopen(jfs_t *stream, const char *mode);



/*
 * jopen() and jtrans_create() flags
 */

/** Don't lock the file before operating on it */
#define J_NOLOCK	1

/** No need to read rollback information */
#define J_NOROLLBACK	2

/** Use lingering transactions */
#define J_LINGER	4

/** Marks a transaction as committed */
#define J_COMMITTED	8

/** Marks a transaction as rollbacked */
#define J_ROLLBACKED	16

/** Marks a transaction as rollbacking */
#define J_ROLLBACKING	32

/** Marks a file as read-only */
#define J_RDONLY	64


/*
 * jfsck() constants (return values)
 */

/** Success, shouldn't be used */
#define J_ESUCCESS	0

/** No such file or directory */
#define J_ENOENT	-1

/** No journal associated with the given file */
#define J_ENOJOURNAL	-2

/** Not enough free memory */
#define J_ENOMEM	-3


#endif

