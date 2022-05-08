/*
	lzz - lz4 solid file archive

	a simple to parse/write 32-bit chunk archive format, compressed solid with lz4.

	Jason A. Petrasko, muragami, muragami@wishray.com
	LICENSE: MIT -> https://opensource.org/licenses/MIT
*/


/*
	DEEP DIVE INTO THE FORMAT, you may tldr; -.-;

	----

	lzz is an lz4 compressed archive format, internally it operates in 4-byte/32-bit chunks:

			[0][1][2][3]

	----

	So, files and internal contents are always rounded to the nearest 32-bit word.
	There are only five types of chunks:

		Marker chunks where byte 0 = 	0
		Tag chunks where byte 0 = 		1
		Info chunks where byte 0 = 		2
		Data chunks where byte 0 = 		3
		Stop chunks where byte 0 = 		4

	Chunks where byte 0 is 5 or higher are custom chunks and only have one rule to follow, we will address that later.

	----

	Markers denote each individual entry. Markers must be in consecutive order, starting with 0.
	Entry 0 is tag and info data about the archive itself.
	Entry 1 is the first entry in the archive.
	Entry 2 the next and so on.
	It is an error to put Entry 4 after Entry 2, and the archive will be considered corrupt.
	Markers are in this format:
 		0   1   2   3
		[00][XX][XX][XX] = MARKER, ID = byte 1 + (byte 2 << 8) + (byte 3 << 16)


	A Stop chunk tells the reader it is at the end of the archive, so all archives end with a 0x04000000 chunk.
	This is the only stop chunk. It is illegal to not have a stop chunk, and invalidates the archive.

	Tag chunks are tags that apply to the marker entry. These are UTF-8 encoded text byte streams (after the chunk).
	Byte 1 of a tag chunk is the length of the tag name. Byte 2 is the length of the tag content. Byte 3 is open for
	end user use as they like. Marker 0 entry must contain at least the title of the archive like so:

		[01][05][0E][00][74][69][74][6C][65][73][61][6D][70][6C][65][20][61][72][63][68][69][76][65][00]

		is a 'title' tag for 'sample archive' in hex bytes. Note the trialing 00, not a require null but a spacer to
		make the data fit into 4-byte/32-bit chunks.

	There is pretty much no limit as to what tags you can add to marker entries, as long as you realize:
		tag name can't be more than 255 UTF-8 bytes.
		tag content can't be more than 255 UTF-8 bytes.

	Info chunks tells us about the marker entry. Info chunks are either standard format, if byte 1 <= 0x7F, or
	custom format when the byte 1 > 0x7F. Standard format info chunks have a following value chunk and then they
	end.

	Custom info chunks have chunks following equal to ((byte 2) + (byte 3) << 8) = 0 to 2047 chunks following.
	8kb size limit for these is hard enforced.

	Marker 0 (total archive entry) must contain these info chunks, the type of which is determined by byte 2:

		 0   1   2   3
		[02][00][00][00] = CONTENT COUNT => total contained entries in the archive (including marker entry)
		[02][01][XX][XX] = TOTAL SIZE => total size in bytes of this archive (including all chunks)
							this begins at byte 2 and continues through all of the next chunk, 48-bit length.

	Other standard info chunks:

		 0   1   2   3
		[02][02][00][00] = ELF CRC32 => the computed ELF CRC32 of all chunks processed so far (before this none)
		[02][03][XX][XX] = EXTENSION => 6 ascii characters of the file extension, including the '.'
							this begins at byte 2 and all through the next chunk.
							'nodata' denotes a chunk that isn't a file and has no data contents. 
		[02][04][00][00] = UID NUMBER => a unique ID number for this entry (one per marker entry)
		[02][05][XX][XX] = TOTAL DATA SIZE => total size in bytes of data for this entry
							this begins at byte 2 and continues through all of the next chunk, 48-bit length.
		[02][06][00][00] = INHERIT => inherit all tags from unique ID number that follows for this entry.
		[02][07][00][00] = TOTAL CODE LINES => total number of code line data blocks in this entry.
		[02][80][00][XX] = MIME => (byte 3) chunks of UTF-8 characters follow, the mime type of the entry.
							byte 3 must be 1 or more!

	All info chunks with byte 1 >= 8 and <= 7F are legal and considered custom info, and follow the standard
	info chunk rules (one chunk follows). All info chunks with byte 1 >= 80 are legal, and have the amount of
	chunks trailing as byte 2 + (byte 3 << 8). At least 1 chunk must trail.

	Data chunks are handled like so:

		 0   1   2   3
		[03][00][XX][XX] = BINARY DATA BLOCK => ((byte 2) + (byte 3) << 8) binary bytes follow.
		[03][01][XX][XX] = CODE LINE DATA BLOCK => ((byte 2) + (byte 3) << 8) UTF-8 bytes follow, including a line termination.
		[03][02][XX][00] = HASH OF DATA => chunks follow for the following hash types:
				[01]	 = (SHA2) SHA256 (8 chunks)
				[02]	 = (SHA2) SHA512 (16 chunks)

	Data chunks with byte 1 >= 3 are considered illegal, and throw an error. 

	Enforcing or including HASH OF DATA for data bearing entries is optional.

	Custom chunks are handled like so:

	 	0   1   2   3
		[>4][XX][XX][XX] = CUSTOM USER CHUNK => (byte 1) is user set. byte 2 + (byte 3 << 8) are bytes that follow to complete the chunk.
												following bytes must be aligned into chunk size.

	Callbacks for custom chunks can be set in the context, allowing you to handle them.

	----

	Entries 1 and higher must contain at least an EXTENSION info chunk.
	Additionally if that extension chunk isn't 'nodata' then a TOTAL DATA SIZE info chunk,
	which could be 0 in the case of an empty file. TOTAL DATA SIZE is required even if you are
	using TOTAL CODE LINES and CODE LINE DATA BLOCKs. In practice either a UID NUMBER info chunk or
	a title Tag chunk should be included, but the reader should not error if they are missing.

	Note the inherit Info chunk above, which allows all the tags to be inherited from another chunk
	that has at least a UID number. This means you need not repeat global data, but create a 'nodata'
	entry that contains tags that apply to multiple includes entries.

	----

	The error policy of *zz is:
		- ignore what you don't understand.
			As long as the archive contains a properly configured marker entry 0 and 1 or more
			following entries with at least an EXTENSION info chunk each, it is valid.
			This makes it very extendable as needed to your purposes.

		This means these 44 bytes are a valid *zz archive:

			[00][00][00][00] - marker entry zero
			[02][00][00][00]
			[01][00][00][00] - INFO: 1 contained entry
			[02][01][2C][00]
			[00][00][00][00] - INFO: 44 bytes total for this archive
			[02][03][6E][6F]
			[64][61][74][61] - INFO: extension = 'nodata'
			[01][05][00][00]
			[74][69][74][6C]
			[65][00][00][00] - TAG: title = empty string
			[04][00][00][00] - STOP

		It's an empty, dummy archive with no contents, but valid by all rules.

	There are no hard rules for when to add the ELF CRC integrity chunks. The code here inserts
	one at the end of every entry, and one before the STOP at the end. You could ignore them all 
	too, it is up the end software.

	----

	.uzz files are non lz4 compressed archives in this format, but normally you'll use
	.lzz which is LZ4 ran on the full archive (solid) data.
*/


#include <stddef.h>

typedef void (*LZZERROR)(const char *error);
typedef void* (*LZZMUTEX)();
typedef void (*LZZLOCK)(void *p);
typedef void (*LZZUNLOCK)(void *p);

typedef void* (*LZZCALLOC)(size_t num, size_t  sz);
typedef void* (*LZZMALLOC)(size_t num);
typedef void* (*LZZREALLOC)(void *p, size_t num);
typedef void (*LZZFREE)(void *p);

typedef int (*LZ4READ)(void *io, char *buffer, int bufferLength);
typedef int (*LZ4WRITE)(void *io, char *data, int dataLength);
typedef void (*LZ4DONE)(void *io);

typedef struct _lzzIO {
	char err[256];
	void *context;
	void *internal;
    LZ4READ read;
    LZ4WRITE write;
    LZ4DONE done;
} lzzIO;

typedef struct _lzzChunk {
	union {
		unsigned char byte[4];
		unsigned int i;
	};
} lzzChunk;

typedef struct _lzzString {
	const char *str;
	unsigned int len;
} lzzString;

typedef struct _lzzTag {
	unsigned int num;
	lzzString name;
	lzzString value;
} lzzTag;

typedef struct _lzzBlockArray {
	unsigned int count;
	unsigned int max;
	lzzChunk *chunk;
} lzzBlockArray;

typedef struct _lzzCodeLine {
	unsigned int count;
	unsigned int length;
	const char *start;
} lzzCodeLine;

typedef struct _lzzEntry {
	lzzBlockArray array;
	unsigned char *data;
	unsigned long long dataLength;
	lzzCodeLine *codeLine;
	char *title;
	char *extension;
	char *mime;
	unsigned int uid;
	unsigned int inheritTagId;
	void *user;
} lzzEntry;

typedef struct _lzzEntryArray {
	unsigned int count;
	unsigned int max;
	lzzEntry *entry;
} lzzEntryArray;

typedef struct _lzzErrors {
	char errBuffer[2048];
	unsigned short errIndex[15];
	unsigned short errCount;
} lzzErrors;

typedef lzzIO (*LZZFETCH)(int entry_num, const char *title, const char *extension, const char *mime, unsigned int uid);

typedef struct _lzzArchive {
	unsigned long long totalBytes;
	unsigned int count;
	lzzEntryArray table;
	lzzErrors e;
	LZZFETCH fetch;
	lzzBlockArray *fixedArray;
	void *safety;
	void *user;
} lzzArchive;

#define lzzMessage lzzArchive

// 8kb chunk buffer built into the parser state
typedef struct _lzzParserBuffer {
	lzzChunk data[2048];
} lzzParserBuffer;

typedef struct _lzzParserState {
	int markerId;
	unsigned int uid;
	unsigned int hash;
	unsigned long long pos;
	unsigned long long totalBytes;
	unsigned int inheritUid;
	lzzIO *io;
	lzzEntry *entry;
	lzzArchive *arc;
	lzzBlockArray block;
	lzzBlockArray blk;
	lzzParserBuffer buffer;
} lzzParserState;

// this is called first with block set to NULL, if you return an int, it'll read that many chunks into block and call it again.
typedef int (*LZZ_CUSTOM_CALLBACK)(lzzArchive *arc, lzzChunk *chunk, lzzChunk *block, unsigned int chunkLength);

typedef struct _lzzContext {
	LZZERROR errorHandler;
	LZZMUTEX mutex;
	LZZLOCK lock;
	LZZUNLOCK unlock;
	LZZCALLOC calloc;
	LZZMALLOC malloc;
	LZZREALLOC realloc;
	LZZFREE free;
	LZZ_CUSTOM_CALLBACK custom[256];
	unsigned int blocksFixed;
	unsigned int entriesFixed;
	unsigned int customLimit;
	unsigned long long bytesAllocated;
	void *user;
} lzzContext;

typedef int (*LZZFILTER)(void *p, lzzEntry *entry);

// read just the minimal needed chunks? or all the chunks even if we can't understand them
#define LZZ_READ_NORMAL 	0
#define LZZ_READ_MINIMAL 	1
#define LZZ_READ_FULL		2
#define LZZ_READ_DECODE		(1 << 8) 	// don't process code line data blocks, just make one byte block of them
#define LZZ_READ_HALT		(2 << 8) 	// halt on any error (usually ignore and press forward)
#define LZZ_READ_HALTHASH	(3 << 8) 	// halt on any hash error (implied by READ_HALT for all errors)

#define LZZ_MODE_FAST 	0		// fast compression
#define LZZ_MODE_HC		1 		// full slow compression
#define LZZ_MODE_FLAT	2		// uncompressed

// create and destroy a context
lzzContext *lzzCreateContext(LZZERROR err, LZZCALLOC c);	// c may be 0/NULL to use C's calloc internally
void lzzDestroyContext(lzzContext *context);

// add your own locking mechanism for thread safety here
void lzzMakeSafeContext(lzzContext *context, LZZMUTEX m, LZZLOCK l, LZZUNLOCK u);

// add your own calloc, malloc, realloc, and free here
void lzzMakeMemoryContext(lzzContext *context, LZZMALLOC m, LZZCALLOC c, LZZREALLOC r, LZZFREE f);

// use a fixed size (no-allocation) archive context, you can never read anything
// with more blocks than set here and archives always take this many blocks of memory even if smaller
// entries can be 0, and if so 800 will be allocated, about ~63kb on a 64-bit system
void lzzMakeMemoryContextFixed(lzzContext *context, unsigned int blocks, unsigned int entries);

// return to a dynamically allocated context if we were in ContextFastLimited (CFL) from above.
void lzzMakeMemoryContextDynamic(lzzContext *context);

// add a custom callback to the context
void lzzSetCustomCallback(lzzContext *context, int typeCode, LZZ_CUSTOM_CALLBACK cb);

// set the user pointer for this context (which will be added to all archives automatically but can be overridden)
void lzzSetUserPointer(lzzContext *context, void *u);

// read the info and tag chunks for an archive, ignore data chunks
lzzArchive* lzzScanFile(lzzContext *context, const char *fname, int method);
lzzArchive* lzzScanMemory(lzzContext *context, const char *block, unsigned long long len, int method);
lzzArchive* lzzScanIO(lzzContext *context, lzzIO *io, int method);

// just read everything, I suppose we might do this?
lzzArchive* lzzReadFile(lzzContext *context, const char *fname, int flags);
lzzArchive* lzzReadMemory(lzzContext *context, const char *block, unsigned long long len, int flags);
lzzArchive* lzzReadIO(lzzContext *context, lzzIO *io, int flags);

// these work with fixed memory context to re-use already allocated archive memory blocks
void lzzScanFileInto(lzzContext *context, lzzArchive *arc, const char *fname, int flags);
void lzzScanMemoryInto(lzzContext *context, lzzArchive *arc, const char *block, unsigned long long len, int flags);
void lzzScanIOInto(lzzContext *context, lzzArchive *arc, lzzIO *io, int flags);
void lzzReadFileInto(lzzContext *context, lzzArchive *arc, const char *fname, int flags);
void lzzReadMemoryInto(lzzContext *context, lzzArchive *arc, const char *block, unsigned long long len, int flags);
void lzzReadIOInto(lzzContext *context, lzzArchive *arc, lzzIO *io, int flags);

// writing out an archive from memory
unsigned long long lzzWriteFile(lzzContext *context, lzzArchive *rac, int mode, const char *fname);
unsigned long long lzzWriteMemory(lzzContext *context, lzzArchive *arc, int mode, char **data);
unsigned long long lzzWriteIO(lzzContext *context, lzzArchive *arc, int mode, lzzIO *io);

// just make an empty archive, so we can add to it in memory if we want to do that kind of thing
// if you are in CFL = ContextFastLimited a full allocation is made for it outright
lzzArchive* lzzEmptyArchive(lzzContext *context);

// add entries to the archive
int lzzAddFolder(lzzContext *context, lzzArchive *arc, const char *title);
// if you set data to NULL it'll be a stub entry
int lzzAddEntry(lzzContext *context, lzzArchive *arc, const char *title, const char *extension, const char *mime, unsigned int uid, const char *data, unsigned long long data_length);
// set the fetch routine for stub entries (added with null above)
int lzzSetFetcher(lzzContext *context, lzzArchive *arc, LZZFETCH f);

// errors on this archive?
int lzzNumErrors(lzzContext *context, lzzArchive *arc, unsigned int *count);
const char* lzzGetError(lzzContext *context, lzzArchive *arc);	// read until it returns 0

// if you just scanned, maybe you want to grab a list of entries to read their data in?
unsigned int lzzReadEntries(lzzContext *context, lzzArchive *arc, unsigned int *entries, unsigned int count);
// read all entries in with the given tag, return the entry numbers into entries and the count
void lzzReadEntriesWithTag(lzzContext *context, lzzArchive *arc, const char *tag_name, const char* tag_value, unsigned int **entries, unsigned int *count);
// read entries based on a custom filter function (return non-zero to read the data)
void lzzReadEntriesFilter(lzzContext *context, lzzArchive *arc, LZZFILTER filt, void *interal);
